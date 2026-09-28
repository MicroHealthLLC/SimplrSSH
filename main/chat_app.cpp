/*
 * ChatGPT App
 * One chat session with an OpenAI model: your messages right-aligned, replies
 * left-aligned, the input line at the bottom. Hold the trackball to talk
 * (Whisper), optionally hear the reply (TTS). Network and audio work runs in a
 * short-lived worker task, so the screen and keyboard stay responsive.
 *
 * Saved: API key in the vault ("chat:openai"), model and spoken-replies setting
 * in NVS namespace "chat". The session lives in RAM until 'clear' or restart.
 */

#include "ssh_terminal.hpp"
#include "secret_vault.hpp"
#include "sd_card.hpp"
#include "audio.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "settings_nvs.hpp"
#include <algorithm>
#include <cstdio>

static const char *TAG = "CHAT";
static const char *KEY_ID = "chat:openai";
static const char *MODELS[] = {"gpt-4o-mini", "gpt-4.1-mini", "gpt-4.1-nano", "gpt-4o"};
static const int MODEL_COUNT = 4;
static const size_t MAX_HISTORY = 12;          // Messages kept for context
static const size_t MAX_CONTEXT_CHARS = 6000;  // Context sent per request
static const uint32_t MAX_BUBBLES = 30;        // Labels kept on screen
static const size_t MAX_RECORD_SAMPLES = audio::MIC_RATE * 30;  // 30 s, ~960 KB in PSRAM
static const uint32_t WORKER_STACK = 10240;

enum Bubble { BUBBLE_USER, BUBBLE_AI, BUBBLE_INFO };

static std::string s_key_file;  // SD card key file just imported (offered for deletion)

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \r\n\t");
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \r\n\t") - a + 1);
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void SSHTerminal::chat_load_settings()
{
    nvs_handle_t h;
    if (settings_nvs::open("chat", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char model[64];
    size_t len = sizeof(model);
    if (nvs_get_str(h, "model", model, &len) == ESP_OK && model[0]) {
        chat_model = model;
    }
    uint32_t speak = 0;
    if (nvs_get_u32(h, "speak", &speak) == ESP_OK) {
        chat_speak = speak != 0;
    }
    nvs_close(h);
}

void SSHTerminal::chat_save_settings()
{
    nvs_handle_t h;
    esp_err_t err = settings_nvs::open("chat", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "model", chat_model.c_str());
        if (err == ESP_OK) {
            err = nvs_set_u32(h, "speak", chat_speak ? 1 : 0);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        append_text("ERROR: Could not save ChatGPT settings.\n");
    }
}

void SSHTerminal::chat_store_key(const std::string& key)
{
    wizard.secret = key;
    with_vault([this]() {
        if (vault::put(KEY_ID, wizard.secret) == ESP_OK) {
            append_text("API key saved on this device (encrypted).\n");
        } else {
            append_text("ERROR: Could not save the API key.\n");
        }
        vault::wipe(wizard.secret);
        bool open_now = chat_open_after_key;
        chat_open_after_key = false;
        if (!s_key_file.empty()) {
            wizard_goto(WizardStep::ChatKeyDelete);  // Offer to remove the key file from the card
            chat_open_after_key = open_now;
        } else if (open_now) {
            open_chat();
        } else {
            wizard_goto(WizardStep::ChatMenu);
        }
    });
}

// ---------------------------------------------------------------------------
// Chat view
// ---------------------------------------------------------------------------

lv_obj_t* SSHTerminal::chat_add_bubble(int kind, const std::string& text)
{
    lv_obj_t* label = lv_label_create(chat_view);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(label, kind == BUBBLE_USER ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(kind == BUBBLE_USER ? 0xFFFF00 : kind == BUBBLE_AI ? 0x00FF00 : 0x8080A0), 0);
    lv_label_set_text(label, text.c_str());
    while (lv_obj_get_child_count(chat_view) > MAX_BUBBLES) {
        lv_obj_delete(lv_obj_get_child(chat_view, 0));
    }
    lv_obj_update_layout(chat_view);
    lv_obj_scroll_to_y(chat_view, LV_COORD_MAX, LV_ANIM_OFF);
    return label;
}

// Worker-side UI helpers: take the display lock and skip if the view has closed
lv_obj_t* SSHTerminal::chat_ui_bubble(uint32_t generation, int kind, const std::string& text)
{
    lv_obj_t* label = NULL;
    if (bsp_display_lock(1000)) {
        if (chat_view && generation == chat_generation) {
            label = chat_add_bubble(kind, text);
        }
        bsp_display_unlock();
    }
    return label;
}

void SSHTerminal::chat_ui_update(uint32_t generation, lv_obj_t* label, const std::string& text)
{
    if (label && bsp_display_lock(200)) {
        if (chat_view && generation == chat_generation) {
            lv_label_set_text(label, text.c_str());
            lv_obj_update_layout(chat_view);
            lv_obj_scroll_to_y(chat_view, LV_COORD_MAX, LV_ANIM_OFF);
        }
        bsp_display_unlock();
    }
}

void SSHTerminal::chat_ui_banner(const std::string& text)
{
    if (bsp_display_lock(200)) {
        input_banner = text;
        update_input_display();
        bsp_display_unlock();
    }
}

void SSHTerminal::handle_chat_command(const std::string& command)
{
    wizard_reset();
    if (command.find("settings") != std::string::npos) {
        wizard.menu = WizardStep::ChatMenu;
        wizard_goto(WizardStep::ChatMenu);
    } else {
        open_chat();
    }
}

void SSHTerminal::open_chat()
{
    if (!vault::has(KEY_ID)) {
        append_text("First, add your OpenAI API key.\n");
        chat_open_after_key = true;
        wizard.menu = WizardStep::ChatMenu;
        wizard_goto(WizardStep::ChatKeySource);
        return;
    }
    with_vault([this]() {
        wizard_reset();
        chat_active = true;
        chat_view = lv_obj_create(terminal_screen);
        lv_obj_set_size(chat_view, lv_obj_get_width(terminal_output), lv_obj_get_height(terminal_output));
        lv_obj_align_to(chat_view, terminal_output, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(chat_view, lv_color_black(), 0);
        lv_obj_set_style_border_color(chat_view, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_border_width(chat_view, 2, 0);
        lv_obj_set_style_radius(chat_view, 0, 0);
        lv_obj_set_style_pad_all(chat_view, 4, 0);
        lv_obj_set_style_pad_row(chat_view, 6, 0);
        lv_obj_set_flex_flow(chat_view, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(chat_view, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(terminal_output, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(side_panel);

        chat_add_bubble(BUBBLE_INFO, "ChatGPT (" + chat_model + ") - type, or hold the trackball to talk.\n"
                                     "'clear' new chat | 'settings' | 'exit' menu");
        for (const auto& m : chat_history) {
            chat_add_bubble(m.from_user ? BUBBLE_USER : BUBBLE_AI, m.text);
        }
    });
}

void SSHTerminal::close_chat()
{
    chat_recording = false;
    chat_generation = chat_generation + 1;  // A running worker stops touching the view
    chat_active = false;
    input_banner.clear();
    if (chat_view) {
        lv_obj_delete(chat_view);
        chat_view = NULL;
    }
    lv_obj_clear_flag(terminal_output, LV_OBJ_FLAG_HIDDEN);
    update_input_display();
}

void SSHTerminal::chat_clear()
{
    if (chat_busy) {
        append_text("Wait for the current reply to finish.\n");
        return;
    }
    chat_history.clear();
    if (chat_view) {
        lv_obj_clean(chat_view);
        chat_add_bubble(BUBBLE_INFO, "New chat.");
    }
}

// Enter pressed in the chat (display lock held)
void SSHTerminal::chat_submit(const std::string& raw)
{
    std::string text = trim(raw);
    std::string cmd = lower(text);
    if (text.empty()) {
        return;
    }
    if (cmd == "exit" || cmd == "menu") {
        go_home();
    } else if (cmd == "clear") {
        chat_clear();
    } else if (cmd == "settings") {
        close_chat();
        wizard.menu = WizardStep::ChatMenu;
        wizard_goto(WizardStep::ChatMenu);
    } else if (chat_busy) {
        chat_add_bubble(BUBBLE_INFO, "Still answering - one moment.");
    } else if (!wifi_connected) {
        chat_add_bubble(BUBBLE_INFO, "WiFi is off. Type 'exit', then pick WiFi.");
    } else {
        chat_add_bubble(BUBBLE_USER, text);
        chat_history.push_back({true, text});
        chat_start_job(false);
    }
}

void SSHTerminal::chat_start_job(bool voice)
{
    chat_busy = true;
    chat_job_voice = voice;
    if (xTaskCreate(chat_worker, "chat", WORKER_STACK, this, 4, NULL) != pdPASS) {
        chat_busy = false;
        chat_recording = false;
        if (!voice) {
            chat_history.pop_back();
        }
        chat_add_bubble(BUBBLE_INFO, "Not enough memory right now - try again.");
    }
}

void SSHTerminal::chat_worker(void* param)
{
    SSHTerminal* t = (SSHTerminal*)param;
    t->chat_job();
    t->chat_ui_banner("");
    t->chat_busy = false;
    vTaskDelete(NULL);
}

// Runs in the worker task: record + transcribe (voice), ask the model, speak the reply
void SSHTerminal::chat_job()
{
    const uint32_t gen = chat_generation;
    std::string key, error;
    if (vault::get(KEY_ID, key) != ESP_OK) {
        chat_recording = false;
        chat_ui_bubble(gen, BUBBLE_INFO, "Can't read the API key - set it in 'settings'.");
        return;
    }

    if (chat_job_voice) {
        int16_t* pcm = (int16_t*)heap_caps_malloc(MAX_RECORD_SAMPLES * 2, MALLOC_CAP_SPIRAM);
        if (!pcm || audio::mic_start() != ESP_OK) {
            chat_recording = false;
            heap_caps_free(pcm);
            chat_ui_bubble(gen, BUBBLE_INFO, "Microphone unavailable.");
            vault::wipe(key);
            return;
        }
        size_t n = 0;
        int shown = -1;
        while (chat_recording && n < MAX_RECORD_SAMPLES) {
            n += audio::mic_read(pcm + n, std::min<size_t>(1600, MAX_RECORD_SAMPLES - n));
            int secs = n / audio::MIC_RATE;
            if (secs != shown) {
                shown = secs;
                chat_ui_banner("* Recording " + std::to_string(secs) + "s - release to send");
            }
        }
        chat_recording = false;
        audio::mic_stop();

        std::string heard;
        if (n < audio::MIC_RATE / 2) {
            chat_ui_bubble(gen, BUBBLE_INFO, "Too short - hold the trackball while you speak.");
        } else {
            chat_ui_banner("Transcribing...");
            if (openai::transcribe(key, pcm, n, audio::MIC_RATE, heard, error) != ESP_OK) {
                chat_ui_bubble(gen, BUBBLE_INFO, error);
            } else if (heard.empty()) {
                chat_ui_bubble(gen, BUBBLE_INFO, "Didn't catch that - try again.");
            }
        }
        heap_caps_free(pcm);
        if (heard.empty()) {
            vault::wipe(key);
            return;
        }
        if (bsp_display_lock(1000)) {
            chat_history.push_back({true, heard});
            if (chat_view && gen == chat_generation) {
                chat_add_bubble(BUBBLE_USER, heard);
            }
            bsp_display_unlock();
        }
    }

    // Context: the most recent messages within the size budget
    std::vector<openai::Message> context;
    if (bsp_display_lock(1000)) {
        size_t chars = 0;
        for (auto it = chat_history.rbegin(); it != chat_history.rend() && context.size() < MAX_HISTORY; ++it) {
            chars += it->text.size();
            if (chars > MAX_CONTEXT_CHARS && !context.empty()) {
                break;
            }
            context.insert(context.begin(), *it);
        }
        bsp_display_unlock();
    }

    chat_ui_banner("Thinking...");
    lv_obj_t* label = chat_ui_bubble(gen, BUBBLE_AI, "...");
    std::string reply;
    int64_t last_draw = 0;
    esp_err_t err = openai::chat(key, chat_model, context, [&](const std::string& piece) {
        if (reply.size() < 4000) {
            reply += piece;
        }
        int64_t now = esp_timer_get_time();
        if (now - last_draw > 150000) {  // Redraw at most ~7 times a second
            last_draw = now;
            chat_ui_update(gen, label, reply);
        }
    }, error);

    if (err != ESP_OK || reply.empty()) {
        chat_ui_update(gen, label, "! " + (error.empty() ? std::string("No reply") : error));
        if (bsp_display_lock(1000)) {
            if (!chat_history.empty() && chat_history.back().from_user) {
                chat_history.pop_back();  // Unanswered: keep the context consistent
            }
            bsp_display_unlock();
        }
        vault::wipe(key);
        return;
    }
    chat_ui_update(gen, label, reply);
    if (bsp_display_lock(1000)) {
        chat_history.push_back({false, reply});
        while (chat_history.size() > MAX_HISTORY) {
            chat_history.erase(chat_history.begin());
        }
        bsp_display_unlock();
    }

    if (chat_speak && audio::speaker_start() == ESP_OK) {
        chat_ui_banner("Speaking...");
        uint8_t carry = 0;
        bool has_carry = false;
        openai::speak(key, reply, [&](const uint8_t* data, size_t len) {
            // Keep whole 16-bit samples across chunk boundaries
            if (has_carry && len > 0) {
                uint8_t pair[2] = {carry, data[0]};
                audio::speaker_write(pair, 2);
                data++;
                len--;
                has_carry = false;
            }
            if (len & 1) {
                carry = data[len - 1];
                has_carry = true;
                len--;
            }
            audio::speaker_write(data, len);
        }, error);
        audio::speaker_stop();
    }
    vault::wipe(key);
    ESP_LOGI(TAG, "Reply: %u chars", (unsigned)reply.size());
}

// ---------------------------------------------------------------------------
// Trackball push-to-talk
// ---------------------------------------------------------------------------

void SSHTerminal::on_trackball_hold(bool start, bool long_press)
{
    if (start) {
        if (!chat_active || wizard_active()) {
            return;
        }
        if (chat_busy) {
            chat_add_bubble(BUBBLE_INFO, "Still answering - one moment.");
        } else if (!wifi_connected) {
            chat_add_bubble(BUBBLE_INFO, "WiFi is off. Type 'exit', then pick WiFi.");
        } else {
            chat_recording = true;
            input_banner = "* Recording - release to send";
            update_input_display();
            chat_start_job(true);
        }
    } else if (chat_recording) {
        chat_recording = false;  // The worker stops recording and sends
    } else if (long_press && !chat_active) {
        delete_current_history_entry();
    }
}

// ---------------------------------------------------------------------------
// ChatGPT settings menu
// ---------------------------------------------------------------------------

bool SSHTerminal::chat_step_prompt()
{
    switch (wizard.step) {
        case WizardStep::ChatMenu:
            append_text("\n== ChatGPT ==\n");
            append_text(("Model: " + chat_model + " | Key: " + (vault::has(KEY_ID) ? "saved" : "not set") +
                         " | Spoken replies: " + (chat_speak ? "on" : "off") + "\n").c_str());
            append_text(" 1) Chat\n"
                        " 2) API key\n"
                        " 3) Model\n");
            append_text(chat_speak ? " 4) Spoken replies: turn off\n" : " 4) Spoken replies: turn on\n");
            append_text(" 5) New chat (clear)\n"
                        " 0) Back\n"
                        "Select [1]: ");
            wizard.choices = {"1", "2", "3", "4", "5", "0"};
            return true;
        case WizardStep::ChatKeySource:
            append_text("OpenAI API key (platform.openai.com > API keys):\n"
                        " 1) Type it\n"
                        " 2) Load from SD card file openai.key\n"
                        " 0) Back\n"
                        "Select [1]: ");
            wizard.choices = {"1", "2", "0"};
            return true;
        case WizardStep::ChatKey:
            append_text("API key (sk-...): ");
            return true;
        case WizardStep::ChatKeyDelete:
            append_text(("Delete " + s_key_file + " from the SD card now? (y/n) [y]: ").c_str());
            wizard.choices = {"y", "n"};
            return true;
        case WizardStep::ChatModel:
            append_text("Model:\n");
            for (int i = 0; i < MODEL_COUNT; i++) {
                append_text((" " + std::to_string(i + 1) + ") " + MODELS[i] + (chat_model == MODELS[i] ? " *" : "") +
                             (i == 0 ? "  (fast, low cost)" : "") + "\n").c_str());
            }
            append_text(" 5) Other (type a model name)\n 0) Back\nSelect: ");
            wizard.choices = {"1", "2", "3", "4", "5", "0"};
            return true;
        case WizardStep::ChatModelOther:
            append_text("Model name (e.g. gpt-4o): ");
            return true;
        default:
            return false;
    }
}

bool SSHTerminal::chat_step_input(const std::string& raw_input, const std::string& input)
{
    switch (wizard.step) {
        case WizardStep::ChatMenu:
            if (input.empty() || input == "1") {
                open_chat();
            } else if (input == "2") {
                chat_open_after_key = false;
                wizard_goto(WizardStep::ChatKeySource);
            } else if (input == "3") {
                wizard_goto(WizardStep::ChatModel);
            } else if (input == "4") {
                chat_speak = !chat_speak;
                chat_save_settings();
                wizard_prompt();
            } else if (input == "5") {
                chat_clear();
                append_text("New chat started.\n");
                wizard_prompt();
            } else if (input == "0") {
                go_home();
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            return true;

        case WizardStep::ChatKeySource:
            if (input.empty() || input == "1") {
                wizard_goto(WizardStep::ChatKey);
            } else if (input == "2") {
                std::string key;
                if (sdcard::mount() != ESP_OK) {
                    append_text("No SD card found.\n");
                    wizard_prompt();
                    return true;
                }
                for (const char* name : {"openai.key", "openai.txt"}) {
                    std::string path = std::string(sdcard::MOUNT_POINT) + "/" + name;
                    FILE* f = fopen(path.c_str(), "r");
                    if (f) {
                        char buf[400];
                        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                        fclose(f);
                        buf[n] = '\0';
                        key = trim(buf);
                        s_key_file = name;
                        break;
                    }
                }
                sdcard::unmount();
                if (key.rfind("sk-", 0) != 0 || key.size() < 20 || key.find(' ') != std::string::npos) {
                    append_text("No valid key found. Put the key (sk-...) alone in openai.key\n"
                                "at the top of the SD card.\n");
                    s_key_file.clear();
                    wizard_prompt();
                } else {
                    chat_store_key(key);
                }
                vault::wipe(key);
            } else if (input == "0") {
                wizard_done();
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            return true;

        case WizardStep::ChatKey: {
            std::string key = trim(raw_input);
            if (key.empty()) {
                wizard_done();
            } else if (key.rfind("sk-", 0) != 0 || key.size() < 20) {
                append_text("That doesn't look like an OpenAI key (starts with sk-).\n");
                wizard_prompt();
            } else {
                chat_store_key(key);
            }
            vault::wipe(key);
            return true;
        }

        case WizardStep::ChatKeyDelete:
            if (lower(input) != "n") {
                bool removed = false;
                if (sdcard::mount() == ESP_OK) {
                    removed = remove((std::string(sdcard::MOUNT_POINT) + "/" + s_key_file).c_str()) == 0;
                    sdcard::unmount();
                }
                append_text(removed ? "Removed from the SD card.\n" : "Couldn't remove it - delete it on a computer.\n");
            }
            s_key_file.clear();
            if (chat_open_after_key) {
                chat_open_after_key = false;
                open_chat();
            } else {
                wizard_goto(WizardStep::ChatMenu);
            }
            return true;

        case WizardStep::ChatModel: {
            int n = input.size() == 1 ? input[0] - '0' : -1;
            if (n >= 1 && n <= MODEL_COUNT) {
                chat_model = MODELS[n - 1];
                chat_save_settings();
                wizard_goto(WizardStep::ChatMenu);
            } else if (n == 5) {
                wizard_goto(WizardStep::ChatModelOther);
            } else if (n == 0) {
                wizard_goto(WizardStep::ChatMenu);
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            return true;
        }

        case WizardStep::ChatModelOther:
            if (!input.empty() && input.size() < 60 && input.find(' ') == std::string::npos) {
                chat_model = input;
                chat_save_settings();
            }
            wizard_goto(WizardStep::ChatMenu);
            return true;

        default:
            return false;
    }
}
