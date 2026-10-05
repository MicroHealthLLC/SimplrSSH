/*
 * ChatGPT App
 * One chat session with an OpenAI model: your messages right-aligned, replies
 * left-aligned, the input line at the bottom. Hold the trackball (Tab5: Ctrl+Space) to talk
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
#include "board.hpp"
#include "settings_nvs.hpp"
#include "mbedtls/platform_util.h"
#include <algorithm>
#include <cstdio>

static const char *TAG = "CHAT";
static const char *KEY_ID = "chat:openai";
static const size_t MODELS_SHOWN = 20;        // Models listed in the Model menu
static const size_t MAX_KEY_LEN = 256;         // Vault limit
static const size_t MAX_HISTORY = 12;          // Messages kept for context
static const size_t MAX_CONTEXT_CHARS = 6000;  // Context sent per request
static const uint32_t MAX_BUBBLES = 30;        // Labels kept on screen
static const size_t MAX_RECORD_SAMPLES = audio::MIC_RATE * 30;  // 30 s, ~960 KB in PSRAM
static const size_t SKIP_SAMPLES = audio::MIC_RATE / 10;        // First 100 ms: codec start-up click
static const int MIN_VOICE_PEAK = 100;                           // Below this the mic heard nothing
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

// The API key from what was typed or read from a file: skips a UTF-8 byte-order mark and blank
// lines, takes the first line, drops surrounding quotes. Empty (with `problem` set) if invalid.
static std::string clean_key(const std::string& raw, std::string& problem)
{
    std::string text = raw.compare(0, 3, "\xEF\xBB\xBF") == 0 ? raw.substr(3) : raw;
    std::string key;
    size_t pos = 0;
    while (key.empty() && pos < text.size()) {
        size_t nl = text.find('\n', pos);
        key = trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? text.size() : nl + 1;
    }
    if (key.size() >= 2 && (key[0] == '"' || key[0] == '\'') && key.back() == key[0]) {
        key = trim(key.substr(1, key.size() - 2));
    }
    if (key.rfind("sk-", 0) != 0 || key.size() < 20 || key.size() > MAX_KEY_LEN) {
        problem = "That doesn't look like an OpenAI key (sk-..., on the first line).";
        key.clear();
        return key;
    }
    for (char c : key) {
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') {
            problem = "The key contains a character OpenAI keys don't use (only letters, digits, - and _).";
            key.clear();
            break;
        }
    }
    return key;
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
    bool saved_model = nvs_get_str(h, "model", model, &len) == ESP_OK && model[0];
    if (saved_model) {
        chat_model = model;
    }
    uint32_t picked = 0;
    if (nvs_get_u32(h, "picked", &picked) == ESP_OK) {
        chat_model_picked = picked != 0;
    } else {
        chat_model_picked = saved_model;  // Saved by an older version: treat it as the user's choice
    }
    for (auto [name, target] : {std::pair<const char*, std::string*>{"stt", &chat_stt_model}, {"tts", &chat_tts_model}}) {
        len = sizeof(model);
        if (nvs_get_str(h, name, model, &len) == ESP_OK && model[0]) {
            *target = model;
        }
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
            err = nvs_set_u32(h, "picked", chat_model_picked ? 1 : 0);
        }
        if (err == ESP_OK) {
            err = nvs_set_str(h, "stt", chat_stt_model.c_str());
        }
        if (err == ESP_OK) {
            err = nvs_set_str(h, "tts", chat_tts_model.c_str());
        }
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
        // Read the key back and compare, so a bad save or decrypt shows up now, not as a rejected key
        std::string check;
        if (vault::put(KEY_ID, wizard.secret) != ESP_OK) {
            append_text("ERROR: Could not save the API key.\n");
        } else if (vault::get(KEY_ID, check) != ESP_OK || check != wizard.secret) {
            append_text("ERROR: The saved API key didn't read back correctly. Please report this.\n");
        } else {
            append_text(("API key saved on this device (encrypted), " + std::to_string(check.size()) +
                         " characters, ends ..." + check.substr(check.size() - 4) + ".\n").c_str());
            chat_request_models(false);  // Ask OpenAI whether it accepts the key
        }
        vault::wipe(check);
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
// Models: asked from OpenAI (GET /v1/models) in a worker task, which also checks the key
// ---------------------------------------------------------------------------

// for_menu: show the Model menu when the list arrives; otherwise report whether the key works
void SSHTerminal::chat_request_models(bool for_menu)
{
    if (!wifi_connected) {
        append_text(for_menu ? "WiFi is off - can't ask OpenAI for the model list.\n"
                             : "Connect to WiFi to check the key with OpenAI.\n");
        if (for_menu) {
            wizard_goto(WizardStep::ChatModel);
        }
        return;
    }
    if (chat_busy || chat_models_busy) {
        append_text("Still waiting for OpenAI - try again in a moment.\n");
        if (for_menu) {
            wizard_prompt();
        }
        return;
    }
    chat_models_for_menu = for_menu;
    chat_models_busy = true;
    if (for_menu) {
        wizard_goto(WizardStep::ChatModelWait);
    }
    refresh_display_now();
    if (xTaskCreate(chat_models_worker, "models", WORKER_STACK, this, 4, NULL) != pdPASS) {
        chat_models_busy = false;
        append_text("Not enough memory right now - try again.\n");
        if (for_menu) {
            wizard_goto(WizardStep::ChatModel);
        }
    }
}

void SSHTerminal::chat_models_worker(void* param)
{
    SSHTerminal* t = (SSHTerminal*)param;
    std::string key, error;
    openai::Models models;
    esp_err_t err = vault::get(KEY_ID, key);
    if (err == ESP_OK) {
        err = openai::list_models(key, models, error);
    } else {
        error = "Can't read the saved API key";
    }
    vault::wipe(key);
    if (bsp_display_lock(5000)) {
        t->chat_models_done(err == ESP_OK, models, error);
        bsp_display_unlock();
    }
    t->chat_models_busy = false;
    vTaskDelete(NULL);
}

// Display lock held. Takes the newest voice models, and the newest chat model unless the user
// picked one: a model the user picked is never changed here. Returns a note for the user, or "".
std::string SSHTerminal::chat_apply_models(const openai::Models& m)
{
    std::string note;
    chat_models = m.chat;
    chat_models_checked = true;
    if (!m.transcribe.empty()) {
        chat_stt_model = m.transcribe;
    }
    if (!m.speech.empty()) {
        chat_tts_model = m.speech;
    }
    if (!chat_model_picked && !m.chat_default.empty() && m.chat_default != chat_model) {
        chat_model = m.chat_default;
        note = "Using " + chat_model + " (newest; pick another in Model).";
    } else if (chat_model_picked && std::find(m.chat.begin(), m.chat.end(), chat_model) == m.chat.end()) {
        note = "Your model " + chat_model + " isn't in OpenAI's list for this key - pick one in Model.";
    }
    chat_save_settings();
    return note;
}

// Display lock held
void SSHTerminal::chat_models_done(bool ok, const openai::Models& models, const std::string& error)
{
    std::string note = ok ? chat_apply_models(models) : "";

    if (chat_models_for_menu) {
        if (wizard.step != WizardStep::ChatModelWait) {
            return;  // The menu was left while waiting
        }
        if (!ok) {
            append_text(("Couldn't get the model list: " + error + "\n").c_str());
        } else if (!note.empty()) {
            append_text((note + "\n").c_str());
        }
        wizard_goto(WizardStep::ChatModel);
        return;
    }

    std::string msg = ok ? "OpenAI accepted the API key (" + std::to_string(models.chat.size()) + " chat models)." +
                           (note.empty() ? "" : " " + note)
                         : "Key check: " + error;
    if (chat_view) {
        chat_add_bubble(BUBBLE_INFO, msg);
    } else {
        append_text(("\n" + msg + "\n").c_str());
    }
}

// ---------------------------------------------------------------------------
// Chat view
// ---------------------------------------------------------------------------

// Follows new text only while the newest line shows (see append_text)
lv_obj_t* SSHTerminal::chat_add_bubble(int kind, const std::string& text)
{
    bool follow = chat_follow && !touch_scrolling(chat_view);
    int32_t view_y = lv_obj_get_scroll_y(chat_view);
    lv_obj_t* label = lv_label_create(chat_view);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_font(label, board::ui().input, 0);
    lv_obj_set_style_text_align(label, kind == BUBBLE_USER ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(kind == BUBBLE_USER ? 0xFFFF00 : kind == BUBBLE_AI ? 0x00FF00 : 0x8080A0), 0);
    lv_label_set_text(label, text.c_str());
    while (lv_obj_get_child_count(chat_view) > MAX_BUBBLES) {
        lv_obj_t* oldest = lv_obj_get_child(chat_view, 0);
        view_y -= lv_obj_get_height(oldest) + lv_obj_get_style_pad_row(chat_view, LV_PART_MAIN);
        lv_obj_delete(oldest);
    }
    lv_obj_update_layout(chat_view);
    lv_obj_scroll_to_y(chat_view, follow ? LV_COORD_MAX : LV_MAX(view_y, 0), LV_ANIM_OFF);
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
            if (chat_follow && !touch_scrolling(chat_view)) {
                lv_obj_update_layout(chat_view);
                lv_obj_scroll_to_y(chat_view, LV_COORD_MAX, LV_ANIM_OFF);
            }
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
        lv_obj_set_style_pad_all(chat_view, board::ui().gap, 0);
        lv_obj_set_style_pad_row(chat_view, board::ui().gap * 3 / 2, 0);
        lv_obj_set_flex_flow(chat_view, LV_FLEX_FLOW_COLUMN);
        // Drag or roll the trackball to scroll back; a thin bar shows only while scrolling
        lv_obj_set_scroll_dir(chat_view, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(chat_view, LV_SCROLLBAR_MODE_ACTIVE);
        lv_obj_set_style_width(chat_view, 2, LV_PART_SCROLLBAR);
        lv_obj_add_event_cb(chat_view, scroll_end_cb, LV_EVENT_SCROLL_END, this);
        chat_follow = true;
        lv_obj_add_flag(terminal_output, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(side_panel);

        chat_add_bubble(BUBBLE_INFO, "ChatGPT (" + chat_model + ") - type, or " + board::HINT_TALK + " to talk.\n"
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
        chat_follow = true;
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

    // Once per boot, ask OpenAI for its current models so retired ones are replaced by newer ones
    if (!chat_models_checked) {
        openai::Models models;
        std::string list_error;
        if (openai::list_models(key, models, list_error) == ESP_OK && bsp_display_lock(1000)) {
            std::string note = chat_apply_models(models);
            if (!note.empty() && chat_view && gen == chat_generation) {
                chat_add_bubble(BUBBLE_INFO, note);
            }
            bsp_display_unlock();
        }
    }
    // The exact model names used for this whole request
    std::string model, stt_model, tts_model;
    if (bsp_display_lock(1000)) {
        model = chat_model;
        stt_model = chat_stt_model;
        tts_model = chat_tts_model;
        bsp_display_unlock();
    } else {
        chat_recording = false;
        vault::wipe(key);
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

        size_t skip = std::min(n, SKIP_SAMPLES);
        int16_t* voice = pcm + skip;
        n -= skip;
        std::string heard;
        int level = 0;
        if (n < audio::MIC_RATE / 2) {
            chat_ui_bubble(gen, BUBBLE_INFO, std::string("Too short - ") + board::HINT_TALK + " while you speak.");
        } else if ((level = audio::normalize(voice, n)) < MIN_VOICE_PEAK) {
            // Nothing worth uploading: say so instead of sending silence to OpenAI
            chat_ui_bubble(gen, BUBBLE_INFO, "The microphone heard nothing (level " + std::to_string(level) +
                                             ") - speak once 'Recording' shows.");
        } else {
            chat_ui_banner("Transcribing...");
            if (openai::transcribe(key, stt_model, voice, n, audio::MIC_RATE, heard, error) != ESP_OK) {
                chat_ui_bubble(gen, BUBBLE_INFO, error);
            } else if (heard.empty()) {
                chat_ui_bubble(gen, BUBBLE_INFO, "Didn't catch that (mic level " + std::to_string(level) +
                                                 ") - try again.");
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
    esp_err_t err = openai::chat(key, model, context, [&](const std::string& piece) {
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
        openai::speak(key, tts_model, reply, [&](const uint8_t* data, size_t len) {
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
        case WizardStep::ChatModelWait:
            append_text("Asking OpenAI which models your key can use...\n");
            return true;
        case WizardStep::ChatModel: {
            size_t shown = std::min(chat_models.size(), MODELS_SHOWN);
            wizard.choices.clear();
            if (shown == 0) {
                append_text(("Model: " + chat_model + "\n").c_str());
            } else {
                append_text("Models for your key (newest first):\n");
            }
            for (size_t i = 0; i < shown; i++) {
                append_text((" " + std::to_string(i + 1) + ") " + chat_models[i] +
                             (chat_models[i] == chat_model ? " *" : "") + "\n").c_str());
                wizard.choices.push_back(std::to_string(i + 1));
            }
            append_text(" t) Type a model name\n 0) Back\nSelect: ");
            wizard.choices.push_back("t");
            wizard.choices.push_back("0");
            return true;
        }
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
                chat_request_models(true);
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
                // Windows may hide a .txt extension, so openai.key can really be openai.key.txt
                std::string problem = "No openai.key file at the top of the SD card.";
                for (const char* name : {"openai.key", "openai.key.txt", "openai.txt"}) {
                    std::string path = std::string(sdcard::MOUNT_POINT) + "/" + name;
                    FILE* f = fopen(path.c_str(), "rb");
                    if (f) {
                        char buf[1024];
                        size_t n = fread(buf, 1, sizeof(buf), f);
                        fclose(f);
                        std::string raw(buf, n);
                        mbedtls_platform_zeroize(buf, sizeof(buf));
                        key = clean_key(raw, problem);
                        vault::wipe(raw);
                        s_key_file = name;
                        break;
                    }
                }
                sdcard::unmount();
                if (key.empty()) {
                    append_text((s_key_file.empty() ? problem : s_key_file + ": " + problem).c_str());
                    append_text("\nPut the key (sk-...) alone in openai.key at the top of the SD card.\n");
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
            std::string problem;
            std::string key = clean_key(raw_input, problem);
            if (trim(raw_input).empty()) {
                wizard_done();
            } else if (key.empty()) {
                append_text((problem + "\n").c_str());
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
                if (sdcard::mount(true) == ESP_OK) {
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

        case WizardStep::ChatModelWait:
            append_text("Still asking OpenAI - one moment.\n");
            return true;

        case WizardStep::ChatModel: {
            int n = !input.empty() && input.size() <= 2 && std::all_of(input.begin(), input.end(), ::isdigit)
                        ? atoi(input.c_str()) : -1;
            size_t shown = std::min(chat_models.size(), MODELS_SHOWN);
            if (n >= 1 && (size_t)n <= shown) {
                chat_model = chat_models[n - 1];
                chat_model_picked = true;
                chat_save_settings();
                wizard_goto(WizardStep::ChatMenu);
            } else if (lower(input) == "t") {
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
                chat_model_picked = true;
                chat_save_settings();
            }
            wizard_goto(WizardStep::ChatMenu);
            return true;

        default:
            return false;
    }
}
