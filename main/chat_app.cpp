/*
 * ChatGPT App
 * One chat session with an OpenAI model: your messages right-aligned, replies
 * left-aligned, the input line at the bottom. Hold the trackball (Tab5: Ctrl) to talk
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
static const char *KEY_ID = "chat:openai";   // Vault id of the OpenAI API key
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

// Message bubbles: a full-width transparent row per message (bubble right for the user, left
// for ChatGPT, notes centred), holding one label styled as the bubble. Shared styles, set up
// once, instead of per-object local styles. No opacity or transforms: they need layer buffers.
static lv_style_t s_row, s_row_user, s_row_ai, s_row_info;
static lv_style_t s_bubble, s_user, s_ai, s_info;
static bool s_styles_ready = false;

static void init_chat_styles()
{
    if (s_styles_ready) {
        return;
    }
    s_styles_ready = true;
    const int32_t gap = board::ui().gap;

    lv_style_init(&s_row);
    lv_style_set_width(&s_row, lv_pct(100));
    lv_style_set_height(&s_row, LV_SIZE_CONTENT);
    lv_style_set_layout(&s_row, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_row, LV_FLEX_FLOW_ROW);
    lv_style_init(&s_row_user);
    lv_style_set_flex_main_place(&s_row_user, LV_FLEX_ALIGN_END);
    lv_style_init(&s_row_ai);
    lv_style_set_flex_main_place(&s_row_ai, LV_FLEX_ALIGN_START);
    lv_style_init(&s_row_info);
    lv_style_set_flex_main_place(&s_row_info, LV_FLEX_ALIGN_CENTER);

    lv_style_init(&s_bubble);
    lv_style_set_radius(&s_bubble, gap * 3);
    lv_style_set_pad_hor(&s_bubble, gap * 2);
    lv_style_set_pad_ver(&s_bubble, gap);
    lv_style_set_bg_opa(&s_bubble, LV_OPA_COVER);
    lv_style_set_text_font(&s_bubble, board::ui().input);
    lv_style_init(&s_user);
    lv_style_set_bg_color(&s_user, lv_color_hex(theme::ACCENT_BG));
    lv_style_set_text_color(&s_user, lv_color_white());
    lv_style_init(&s_ai);
    lv_style_set_bg_color(&s_ai, lv_color_hex(theme::DIVIDER));
    lv_style_set_text_color(&s_ai, lv_color_hex(theme::TEXT));
    lv_style_init(&s_info);
    lv_style_set_width(&s_info, lv_pct(100));
    lv_style_set_bg_opa(&s_info, LV_OPA_TRANSP);
    lv_style_set_text_color(&s_info, lv_color_hex(theme::DIM));
    lv_style_set_text_font(&s_info, board::ui().small);
    lv_style_set_text_align(&s_info, LV_TEXT_ALIGN_CENTER);
}

// Fits a bubble to its text, up to 80% of the view's width. Measured here because LVGL wraps a
// content-sized label at its max width before taking off the padding, which would cut a line.
static void fit_bubble(lv_obj_t* view, lv_obj_t* label)
{
    int32_t pad = lv_obj_get_style_pad_left(label, LV_PART_MAIN) + lv_obj_get_style_pad_right(label, LV_PART_MAIN);
    int32_t view_w = lv_obj_get_style_width(view, LV_PART_MAIN) - 2 * board::ui().gap;
    int32_t max_text = LV_MAX(view_w * 4 / 5 - pad, 1);
    lv_point_t size;
    lv_text_get_size(&size, lv_label_get_text(label), lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     0, 0, max_text, LV_TEXT_FLAG_NONE);
    lv_obj_set_width(label, LV_MIN(size.x, max_text) + pad);
}

// Returns the message's label (chat_ui_update() changes its text). Follows new text only while
// the newest line shows (see append_text).
lv_obj_t* SSHTerminal::chat_add_bubble(int kind, const std::string& text)
{
    init_chat_styles();
    bool follow = chat_follow && !touch_scrolling(chat_view);
    int32_t view_y = lv_obj_get_scroll_y(chat_view);

    lv_obj_t* row = lv_obj_create(chat_view);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));  // Drags scroll the view
    lv_obj_add_style(row, &s_row, 0);
    lv_obj_add_style(row, kind == BUBBLE_USER ? &s_row_user : kind == BUBBLE_AI ? &s_row_ai : &s_row_info, 0);

    lv_obj_t* label = lv_label_create(row);
    lv_obj_remove_style_all(label);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(label, &s_bubble, 0);
    lv_obj_add_style(label, kind == BUBBLE_USER ? &s_user : kind == BUBBLE_AI ? &s_ai : &s_info, 0);
    lv_label_set_text(label, text.c_str());
    if (kind != BUBBLE_INFO) {
        fit_bubble(chat_view, label);
    }

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
            fit_bubble(chat_view, label);
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
        lv_obj_set_style_bg_color(chat_view, lv_color_hex(theme::BG), 0);
        lv_obj_set_style_border_width(chat_view, 0, 0);
        lv_obj_set_style_radius(chat_view, 0, 0);
        lv_obj_set_style_pad_all(chat_view, board::ui().gap, 0);
        lv_obj_set_style_pad_row(chat_view, board::ui().gap, 0);
        lv_obj_set_flex_flow(chat_view, LV_FLEX_FLOW_COLUMN);
        // Drag or roll the trackball to scroll back; a thin bar shows only while scrolling
        lv_obj_set_scroll_dir(chat_view, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(chat_view, LV_SCROLLBAR_MODE_ACTIVE);
        lv_obj_set_style_width(chat_view, 2, LV_PART_SCROLLBAR);
        lv_obj_add_event_cb(chat_view, scroll_end_cb, LV_EVENT_SCROLL_END, this);
        chat_follow = true;

        chat_add_bubble(BUBBLE_INFO, chat_model + "\n'clear' new chat, 'settings', 'exit'. " +
                                     board::HINT_STOP + " stops a reply.");
        for (const auto& m : chat_history) {
            chat_add_bubble(m.from_user ? BUBBLE_USER : BUBBLE_AI, m.text);
        }
    });
}

// An OpenAI API key is saved (the home menu shows "No key" otherwise)
bool SSHTerminal::chat_has_key() const
{
    return vault::has(KEY_ID);
}

void SSHTerminal::close_chat()
{
    chat_recording = false;
    chat_cancel = chat_busy;   // Leaving stops a reply or speech in progress
    chat_generation = chat_generation + 1;  // A running worker stops touching the view
    chat_active = false;
    input_banner.clear();
    if (chat_view) {
        lv_obj_delete(chat_view);
        chat_view = NULL;
    }
    update_input_display();
}

// Esc in the chat (display lock held): stops recording, the reply being written, or speech
bool SSHTerminal::chat_stop()
{
    if (!chat_busy) {
        return false;
    }
    chat_cancel = true;
    chat_recording = false;
    input_banner = "Stopping...";
    update_input_display();
    return true;
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
        chat_add_bubble(BUBBLE_INFO, "Still answering - " + std::string(board::HINT_STOP) + " stops it.");
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
    chat_cancel = false;
    chat_job_voice = voice;
    chat_job_dictate = false;
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
    if (t->chat_job_dictate) {
        t->dictate_job();
        t->dictate_show("", false);   // Ends the progress shown in the title bar
    } else {
        t->chat_job();
        t->chat_ui_banner("");
    }
    t->chat_busy = false;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Voice: shared by the chat and dictation in SSH, so both work the same way.
// Run in the worker task; banner() shows progress, info() a message.
// ---------------------------------------------------------------------------

// Records into PSRAM while the talk key is held (chat_recording), up to 30 s. False if the
// microphone can't start (info() said so) or the user stopped (chat_cancel); otherwise the
// caller frees pcm.
bool SSHTerminal::chat_record(int16_t*& pcm, size_t& samples, const char* release_hint, const Say& banner,
                              const Say& info)
{
    samples = 0;
    pcm = (int16_t*)heap_caps_malloc(MAX_RECORD_SAMPLES * 2, MALLOC_CAP_SPIRAM);
    if (!pcm || audio::mic_start() != ESP_OK) {
        chat_recording = false;
        heap_caps_free(pcm);
        pcm = NULL;
        info("Microphone unavailable.");
        return false;
    }
    size_t n = 0;
    int shown = -1;
    while (chat_recording && !chat_cancel && n < MAX_RECORD_SAMPLES) {
        size_t got = audio::mic_read(pcm + n, std::min<size_t>(1600, MAX_RECORD_SAMPLES - n));
        if (got == 0) {
            break;   // The microphone stopped delivering: end here rather than spin
        }
        n += got;
        int secs = n / audio::MIC_RATE;
        if (secs != shown) {
            shown = secs;
            banner("* Recording " + std::to_string(secs) + "s - " + release_hint);
        }
    }
    chat_recording = false;
    audio::mic_stop();
    if (chat_cancel) {
        heap_caps_free(pcm);
        pcm = NULL;
        return false;
    }
    samples = n;
    return true;
}

// What was said, from OpenAI's speech-to-text model. Empty if there was nothing to send, it
// failed or the user stopped (info() said why).
std::string SSHTerminal::chat_transcribe(const std::string& key, const std::string& stt_model, int16_t* pcm,
                                         size_t samples, const Say& banner, const Say& info)
{
    size_t skip = std::min(samples, SKIP_SAMPLES);
    int16_t* voice = pcm + skip;
    size_t n = samples - skip;
    std::string heard, error;
    int level = 0;
    if (n < audio::MIC_RATE / 2) {
        info(std::string("Too short - ") + board::HINT_TALK + " while you speak.");
    } else if ((level = audio::normalize(voice, n)) < MIN_VOICE_PEAK) {
        // Nothing worth uploading: say so instead of sending silence to OpenAI
        info("The microphone heard nothing (level " + std::to_string(level) + ") - speak once 'Recording' shows.");
    } else {
        banner("Transcribing...");
        if (openai::transcribe(key, stt_model, voice, n, audio::MIC_RATE, heard, error) != ESP_OK) {
            heard.clear();
            info(error);
        } else if (heard.empty()) {
            info("Didn't catch that (mic level " + std::to_string(level) + ") - try again.");
        }
    }
    if (chat_cancel) {
        heard.clear();
        info("Stopped.");
    }
    return heard;
}

// Once per boot, asks OpenAI for its current models so retired ones are replaced by newer
// ones; then the exact model names used for this whole request. False if the display stayed busy.
bool SSHTerminal::chat_job_models(const std::string& key, uint32_t generation, std::string& model,
                                  std::string& stt_model, std::string& tts_model)
{
    if (!chat_models_checked) {
        openai::Models models;
        std::string list_error;
        if (openai::list_models(key, models, list_error) == ESP_OK && bsp_display_lock(1000)) {
            std::string note = chat_apply_models(models);
            if (!note.empty() && chat_view && generation == chat_generation) {
                chat_add_bubble(BUBBLE_INFO, note);
            }
            bsp_display_unlock();
        }
    }
    if (!bsp_display_lock(1000)) {
        return false;
    }
    model = chat_model;
    stt_model = chat_stt_model;
    tts_model = chat_tts_model;
    bsp_display_unlock();
    return true;
}

// Runs in the worker task: record + transcribe (voice), ask the model, speak the reply
void SSHTerminal::chat_job()
{
    const uint32_t gen = chat_generation;
    const Say info = [this, gen](const std::string& text) { chat_ui_bubble(gen, BUBBLE_INFO, text); };
    const Say banner = [this](const std::string& text) { chat_ui_banner(text); };
    std::string key, error;
    if (vault::get(KEY_ID, key) != ESP_OK) {
        chat_recording = false;
        info("Can't read the API key - set it in 'settings'.");
        return;
    }

    // Voice: record first, so nothing said is lost while OpenAI is asked for its models
    int16_t* pcm = NULL;
    size_t samples = 0;
    if (chat_job_voice && !chat_record(pcm, samples, "release to send", banner, info)) {
        if (chat_cancel) {
            info("Stopped.");
        }
        vault::wipe(key);
        return;
    }
    std::string model, stt_model, tts_model;
    if (!chat_job_models(key, gen, model, stt_model, tts_model)) {
        heap_caps_free(pcm);
        vault::wipe(key);
        return;
    }

    if (chat_job_voice) {
        std::string heard = chat_transcribe(key, stt_model, pcm, samples, banner, info);
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
        if (chat_cancel) {
            return false;
        }
        if (reply.size() < 4000) {
            reply += piece;
        }
        int64_t now = esp_timer_get_time();
        if (now - last_draw > 150000) {  // Redraw at most ~7 times a second
            last_draw = now;
            chat_ui_update(gen, label, reply);
        }
        return true;
    }, error);

    if (chat_cancel) {
        // Stopped: keep what was written so far (it is part of the conversation), or nothing
        chat_ui_update(gen, label, reply.empty() ? std::string("[stopped]") : reply + " [stopped]");
        if (bsp_display_lock(1000)) {
            if (!reply.empty()) {
                chat_history.push_back({false, reply});
            } else if (!chat_history.empty() && chat_history.back().from_user) {
                chat_history.pop_back();
            }
            while (chat_history.size() > MAX_HISTORY) {
                chat_history.erase(chat_history.begin());
            }
            bsp_display_unlock();
        }
        vault::wipe(key);
        return;
    }
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
            if (chat_cancel) {
                return false;   // Esc: silence now
            }
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
            return true;
        }, error);
        audio::speaker_stop();
    }
    vault::wipe(key);
    ESP_LOGI(TAG, "Reply: %u chars", (unsigned)reply.size());
}

// ---------------------------------------------------------------------------
// Dictation in an SSH session: hold the talk key, speak, let go. The words are typed at the
// cursor (Tab5: sent to the server like keys; T-Deck: into the input line), without Enter, so
// the user reads them before running anything. Same key, model and checks as the chat.
// ---------------------------------------------------------------------------

static const size_t MAX_DICTATION = 1000;      // Characters typed per recording
static const int64_t STATUS_SHOWN_MS = 5000;   // A message stays in the title bar this long

// Talk key pressed in an SSH session (keypad_task, display lock held). If dictation can't
// start, the reason shows when the key is let go on its own (not for a slow Ctrl+C).
void SSHTerminal::dictate_start()
{
    dictate_problem.clear();
    if (!vault::has(KEY_ID)) {
        dictate_problem = "To talk here, add an OpenAI key in ChatGPT.";
    } else if (!vault::is_unlocked()) {
        dictate_problem = "Saved passwords are locked - open ChatGPT to unlock.";
    } else if (chat_busy || chat_models_busy) {
        dictate_problem = "Still transcribing - try again in a moment.";
    } else if (!wifi_connected) {
        dictate_problem = "WiFi is off.";
    }
    if (!dictate_problem.empty()) {
        return;
    }
    chat_recording = true;
    chat_busy = true;
    chat_cancel = false;
    chat_job_dictate = true;
    dictate_ssh_generation = ssh_generation;
    dictate_status = "* Recording - release to type";
    dictate_status_until_ms = 0;
    sync_view();
    if (xTaskCreate(chat_worker, "chat", WORKER_STACK, this, 4, NULL) != pdPASS) {
        chat_busy = false;
        chat_recording = false;
        chat_job_dictate = false;
        dictate_status.clear();
        dictate_problem = "Not enough memory right now - try again.";
    }
}

// Shown in the title bar (the Tab5's terminal fills the screen). From the worker: a message
// (brief) stays a few seconds; progress until the next one. An empty progress ends it but
// leaves a message showing.
void SSHTerminal::dictate_show(const std::string& text, bool brief)
{
    if (!bsp_display_lock(200)) {
        return;
    }
    if (!text.empty() || dictate_status_until_ms == 0) {
        dictate_status = portal::printable(text, 120);   // Errors may quote OpenAI
        dictate_status_until_ms = brief && !text.empty() ? esp_timer_get_time() / 1000 + STATUS_SHOWN_MS : 0;
        sync_view();
    }
    bsp_display_unlock();
}

// Runs in the worker task: record, transcribe, hand the words to keypad_task (dictate_tick)
void SSHTerminal::dictate_job()
{
    const Say info = [this](const std::string& text) { dictate_show(text, true); };
    const Say banner = [this](const std::string& text) { dictate_show(text, false); };
    std::string key;
    if (vault::get(KEY_ID, key) != ESP_OK) {
        chat_recording = false;
        info("Can't read the OpenAI key - check it in ChatGPT.");
        return;
    }
    int16_t* pcm = NULL;
    size_t samples = 0;
    if (!chat_record(pcm, samples, "release to type", banner, info)) {
        vault::wipe(key);   // Stopped by Ctrl+key (Ctrl+C ...): quietly, nothing is typed
        return;
    }
    std::string model, stt_model, tts_model;
    std::string heard;
    if (chat_job_models(key, chat_generation, model, stt_model, tts_model)) {
        heard = chat_transcribe(key, stt_model, pcm, samples, banner, info);
    }
    heap_caps_free(pcm);
    vault::wipe(key);

    // Typed as keys: printable ASCII on one line (no Enter, no control codes)
    std::string text = portal::printable(heard, MAX_DICTATION);
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    vault::wipe(heard);
    if (text.empty() || !bsp_display_lock(1000)) {
        vault::wipe(text);
        return;
    }
    dictate_text = text;
    dictate_ready = true;
    bsp_display_unlock();
    vault::wipe(text);
}

// keypad_task (display lock held): types the words at the cursor of the session they were
// spoken in, and ends a message shown long enough
void SSHTerminal::dictate_tick()
{
    if (dictate_status_until_ms && esp_timer_get_time() / 1000 >= dictate_status_until_ms) {
        dictate_status.clear();
        dictate_status_until_ms = 0;
        sync_view();
    }
    if (!dictate_ready) {
        return;
    }
    dictate_ready = false;
    if (ssh_connected && ssh_generation == dictate_ssh_generation && !chat_active && !wizard_active()) {
        last_input_ms = esp_timer_get_time() / 1000;
        if (raw_keys()) {
            term->scroll_to_newest();
            write_channel(dictate_text.data(), dictate_text.size());
        } else {
            scroll_to_newest();
            size_t at = std::min(cursor_pos, current_input.size());
            current_input.insert(at, dictate_text);
            cursor_pos = at + dictate_text.size();
            update_input_display();
        }
    } else {
        dictate_status = "Not typed: the SSH session has ended.";
        dictate_status_until_ms = esp_timer_get_time() / 1000 + STATUS_SHOWN_MS;
        sync_view();
    }
    vault::wipe(dictate_text);
}

// ---------------------------------------------------------------------------
// Push-to-talk: trackball (Tab5: Ctrl) held
// ---------------------------------------------------------------------------

void SSHTerminal::on_trackball_hold(bool start, bool long_press)
{
    if (start) {
        // In an SSH session the words are typed at the cursor
        hold_for_dictation = ssh_connected && !chat_active && !wizard_active();
        if (hold_for_dictation) {
            dictate_start();
            return;
        }
        if (!chat_active || wizard_active()) {
            return;
        }
        if (chat_busy) {
            chat_add_bubble(BUBBLE_INFO, "Still answering - " + std::string(board::HINT_STOP) + " stops it.");
        } else if (!wifi_connected) {
            chat_add_bubble(BUBBLE_INFO, "WiFi is off. Type 'exit', then pick WiFi.");
        } else {
            chat_recording = true;
            input_banner = "* Recording - release to send";
            update_input_display();
            chat_start_job(true);
        }
        return;
    }
    bool dictating = hold_for_dictation;
    hold_for_dictation = false;
    if (chat_recording) {
        chat_recording = false;  // The worker stops recording and sends
    } else if (dictating && !dictate_problem.empty()) {
        dictate_status = dictate_problem;   // Why nothing was recorded
        dictate_status_until_ms = esp_timer_get_time() / 1000 + STATUS_SHOWN_MS;
        sync_view();
    } else if (long_press && !dictating && !chat_active && !raw_keys()) {
        delete_current_history_entry();
    }
    dictate_problem.clear();
}

void SSHTerminal::cancel_talk()
{
    hold_for_dictation = false;
    dictate_problem.clear();
    if (chat_recording) {
        chat_cancel = true;      // The worker drops the recording: nothing is sent
        chat_recording = false;
    }
}

// ---------------------------------------------------------------------------
// ChatGPT settings menu
// ---------------------------------------------------------------------------

bool SSHTerminal::chat_step_prompt()
{
    switch (wizard.step) {
        case WizardStep::ChatMenu: {
            ui::Page p;
            p.title = "ChatGPT";
            p.rows = {
                ui::row("1", "Chat", LV_SYMBOL_ENVELOPE),
                ui::row("2", "API key", LV_SYMBOL_EYE_CLOSE, vault::has(KEY_ID) ? "saved" : "not set", ui::Mark::More),
                ui::row("3", "Model", LV_SYMBOL_SETTINGS, chat_model, ui::Mark::More),
                ui::row("4", "Spoken replies", LV_SYMBOL_VOLUME_MAX, "", chat_speak ? ui::Mark::On : ui::Mark::Off),
                ui::row("5", "New chat (clear)", LV_SYMBOL_REFRESH),
                ui::back_row(),
            };
            show_page(p);
            return true;
        }
        case WizardStep::ChatKeySource: {
            ui::Page p;
            p.title = "API key";
            p.info = "Get one at platform.openai.com > API keys";
            p.rows = {
                ui::row("1", "Type it", LV_SYMBOL_KEYBOARD),
                ui::row("2", "Load from SD card", LV_SYMBOL_SD_CARD, "openai.key"),
                ui::back_row(),
            };
            show_page(p);
            return true;
        }
        case WizardStep::ChatKey: {
            ui::Page p;
            p.title = "API key";
            p.question = "API key (sk-...)";
            p.hint = "Enter = back";
            show_page(p);
            return true;
        }
        case WizardStep::ChatKeyDelete: {
            ui::Page p;
            p.title = "API key";
            p.info = "Delete " + s_key_file + " from the SD card now?";
            p.rows = {
                ui::row("y", "Delete it", LV_SYMBOL_TRASH),
                ui::row("n", "Keep it", LV_SYMBOL_CLOSE),
            };
            p.rows[0].tone = ui::Tone::Danger;
            show_page(p);
            return true;
        }
        case WizardStep::ChatModelWait: {
            ui::Page p;
            p.title = "Model";
            p.info = "Asking OpenAI which models your key can use...";
            show_page(p);
            return true;
        }
        case WizardStep::ChatModel: {
            ui::Page p;
            p.title = "Model";
            size_t shown = std::min(chat_models.size(), MODELS_SHOWN);
            p.selected = (int)shown + 1;   // Back, unless the current model is listed
            p.info = shown == 0 ? "Model: " + chat_model : "Models for your key (newest first)";
            for (size_t i = 0; i < shown; i++) {
                bool chosen = chat_models[i] == chat_model;
                if (chosen) {
                    p.selected = (int)i;
                }
                p.rows.push_back(ui::row(std::to_string(i + 1), chat_models[i], nullptr, "",
                                         chosen ? ui::Mark::Chosen : ui::Mark::NotChosen));
            }
            p.rows.push_back(ui::row("t", "Type a model name", LV_SYMBOL_EDIT));
            p.rows.push_back(ui::back_row());
            show_page(p);
            return true;
        }
        case WizardStep::ChatModelOther: {
            ui::Page p;
            p.title = "Model";
            p.question = "Model name (e.g. gpt-4o)";
            p.hint = "Enter = back";
            show_page(p);
            return true;
        }
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
                    // noopengrep: mh.gitlab.c_misc_rule-fopen-open - single-user device, FAT on the SD card has no links or permissions to race; what is read is size-checked and parsed before use
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
