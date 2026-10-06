/*
 * SSHTerminal Implementation
 * Core SSH terminal functionality including WiFi connectivity, libssh2 session management,
 * command execution, terminal display rendering with LVGL, and command history management.
 */

#include "ssh_terminal.hpp"
#include "secret_vault.hpp"
#include "command_redact.hpp"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "settings_nvs.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "bsp/esp-bsp.h"
#include "board.hpp"
#include <cstring>
#include <algorithm>
#include <string_view>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/select.h>

static const char *TAG = "SSH_TERMINAL";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;
#define WIFI_MAXIMUM_RETRY 5

static bool s_wifi_started = false;
static bool s_wifi_stopped = false;       // Radio off while asleep (wifi_sleep())
static bool s_connect_requested = false;  // Reconnect on drop only while a connection is wanted

static esp_event_handler_instance_t s_instance_any_id = NULL;
static esp_event_handler_instance_t s_instance_got_ip = NULL;

SSHTerminal::SSHTerminal() 
    : terminal_screen(NULL), 
      terminal_output(NULL), 
      input_label(NULL),
      status_bar(NULL),
      byte_counter_label(NULL),
      side_panel(NULL),
      cursor_pos(0),
      bytes_received(0),
      history_index(-1),
      cursor_blink_timer(NULL),
      cursor_visible(true),
      battery_update_timer(NULL),
      history_needs_save(false),
      history_save_timer(NULL),
      last_display_update(0),
      wifi_connected(false),
      ssh_connected(false),
      battery_initialized(false),
      ssh_socket(-1),
      session(NULL),
      channel(NULL),
      ssh_mutex(xSemaphoreCreateMutex()),
      ssh_generation(0)
{
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "Initializing battery measurement...");
    esp_err_t battery_ret = battery.init();
    if (battery_ret == ESP_OK) {
        battery_initialized = true;
        ESP_LOGI(TAG, "Battery measurement initialized successfully");
        float test_voltage = battery.readBatteryVoltage();
        ESP_LOGI(TAG, "Test battery read: %.2fV", test_voltage);
    } else {
        battery_initialized = false;
        ESP_LOGE(TAG, "Battery measurement initialization FAILED: %s", esp_err_to_name(battery_ret));
    }
    
    load_history_from_nvs();
    vault_was_reset = vault::init();
    profiles = profile_store::load();
    saved_networks = network_store::load();
    if (vault_was_reset) {
        clear_all_saved_secret_flags();  // Those passwords are gone; ask for them again
    }
    chat_load_settings();
}

SSHTerminal::~SSHTerminal() 
{
    ssh_teardown();
    if (cursor_blink_timer) {
        lv_timer_del(cursor_blink_timer);
    }
    if (battery_update_timer) {
        lv_timer_del(battery_update_timer);
    }
    if (history_save_timer) {
        lv_timer_del(history_save_timer);
        if (history_needs_save) {
            save_history_to_nvs();
        }
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    SSHTerminal* terminal = (SSHTerminal*)arg;
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connect_requested && s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            if (s_connect_requested && terminal) {
                s_connect_requested = false;
                terminal->wifi_link_lost();
            }
        }
        ESP_LOGI(TAG,"Connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        if (terminal) {
            char ip_str[64];
            snprintf(ip_str, sizeof(ip_str), "WiFi Connected! IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
            
            if (bsp_display_lock(0)) {
                terminal->append_text(ip_str);
                bsp_display_unlock();
            }
        }
    }
}

// Brings up the WiFi driver in station mode once; connections are made on request
esp_err_t SSHTerminal::start_wifi_driver()
{
    if (s_wifi_started) {
        if (s_wifi_stopped) {
            esp_err_t err = esp_wifi_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "WiFi restart failed (%s)", esp_err_to_name(err));
                return err;
            }
            s_wifi_stopped = false;
        }
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting WiFi driver...");
    esp_err_t err = board::wifi_prepare();
    if (err != ESP_OK) {
        return err;
    }
    s_wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // Networks are saved by the app (password in the vault): the WiFi driver - on the Tab5, its
    // co-processor - must not keep its own plain-text copy in flash
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        this,
                                                        &s_instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        this,
                                                        &s_instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
    return ESP_OK;
}

esp_err_t SSHTerminal::init_wifi(const char* ssid, const char* password)
{
    ESP_LOGI(TAG, "Connecting WiFi...");
    if (start_wifi_driver() != ESP_OK) {
        if (!wifi_quiet_connect) {
            append_text("WiFi hardware is not responding.\n");
        }
        return ESP_FAIL;
    }
    
    // Leave the current network first (switching networks)
    if (s_connect_requested || wifi_connected) {
        s_connect_requested = false;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
        wifi_connected = false;
        wifi_ssid.clear();
    }
    
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    s_connect_requested = true;
    esp_wifi_connect();

    const int max_wait_ms = 15000;
    const int check_interval_ms = 500;
    int elapsed_ms = 0;
    
    while (elapsed_ms < max_wait_ms) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(check_interval_ms));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi connected");
            wifi_connected = true;
            wifi_ssid = ssid;
            wifi_auto_enabled = true;
            
            if (bsp_display_lock(0)) {
                update_status_bar();
                bsp_display_unlock();
            }
            return ESP_OK;
        } else if (bits & WIFI_FAIL_BIT) {
            ESP_LOGI(TAG, "WiFi connection failed");
            break;
        }
        
        if (!wifi_quiet_connect && bsp_display_lock(0)) {
            append_text(".");
            refresh_display_now();
            bsp_display_unlock();
        }
        elapsed_ms += check_interval_ms;
    }
    
    ESP_LOGE(TAG, "WiFi connection failed");
    s_connect_requested = false;
    esp_wifi_disconnect();
    wifi_connected = false;
    s_retry_num = 0;
    
    if (bsp_display_lock(0)) {
        append_text("\n");
        update_status_bar();
        bsp_display_unlock();
    }
    return ESP_FAIL;
}

void SSHTerminal::disconnect_wifi()
{
    wifi_auto_enabled = false;  // Stay off until the user connects again
    if (wifi_connected) {
        append_text("Disconnecting WiFi...\n");
        s_connect_requested = false;
        esp_wifi_disconnect();
        wifi_connected = false;
        wifi_ssid.clear();
        update_status_bar();
        append_text("WiFi disconnected\n");
    } else {
        append_text("WiFi not connected\n");
    }
}

// Sleep and power off: leaves the network and turns the radio off. wifi_auto_enabled is kept,
// so a saved network is joined again after waking.
void SSHTerminal::wifi_sleep()
{
    if (!s_wifi_started || s_wifi_stopped) {
        return;
    }
    s_connect_requested = false;
    if (wifi_connected) {
        esp_wifi_disconnect();
    }
    esp_wifi_stop();
    s_wifi_stopped = true;
    wifi_connected = false;
    wifi_ssid.clear();
    update_status_bar();
}

void SSHTerminal::print_saved_summary()
{
    std::string line = "Saved on this device: " + std::to_string(saved_networks.size()) + " WiFi network" +
                       (saved_networks.size() == 1 ? "" : "s") + ", " + std::to_string(profiles.size()) +
                       " SSH profile" + (profiles.size() == 1 ? "" : "s") + "\n";
    append_text(line.c_str());
    if (vault_was_reset) {
        append_text("Saved passwords from another device couldn't be used here; they'll be\n"
                    "asked for once and saved again.\n");
    }
}

// Called from the WiFi event task when an established connection is gone for good
void SSHTerminal::wifi_link_lost()
{
    if (!wifi_connected) {
        return;
    }
    wifi_connected = false;
    wifi_ssid.clear();
    // wifi_maintain() reconnects to a saved network shortly
    wifi_retry_delay_s = 0;
    wifi_next_retry_ms = esp_timer_get_time() / 1000 + 5000;
    if (bsp_display_lock(0)) {
        append_text("\nWiFi connection lost - reconnecting automatically...\n");
        update_status_bar();
        bsp_display_unlock();
    }
}

// Scans for access points; results are de-duplicated by SSID, strongest first
esp_err_t SSHTerminal::scan_wifi(std::vector<WifiScanResult>& results)
{
    results.clear();
    esp_err_t err = start_wifi_driver();
    if (err != ESP_OK) {
        return err;
    }

    wifi_scan_config_t scan_config = {};
    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    count = std::min<uint16_t>(count, 30);
    std::vector<wifi_ap_record_t> records(count);
    if (count > 0) {
        err = esp_wifi_scan_get_ap_records(&count, records.data());
        if (err != ESP_OK) {
            return err;
        }
    } else {
        esp_wifi_clear_ap_list();
    }

    for (uint16_t i = 0; i < count; i++) {
        std::string ssid((const char*)records[i].ssid);
        if (ssid.empty()) {
            continue;  // Hidden network
        }
        auto it = std::find_if(results.begin(), results.end(),
                               [&](const WifiScanResult& r) { return r.ssid == ssid; });
        if (it == results.end()) {
            results.push_back({ssid, records[i].rssi, records[i].authmode == WIFI_AUTH_OPEN});
        } else if (records[i].rssi > it->rssi) {
            it->rssi = records[i].rssi;
        }
    }

    std::sort(results.begin(), results.end(),
              [](const WifiScanResult& a, const WifiScanResult& b) { return a.rssi > b.rssi; });
    ESP_LOGI(TAG, "Scan found %d networks", (int)results.size());
    return ESP_OK;
}

// Redraws the screen immediately, so progress messages show before a blocking step.
// Callers already hold the display lock.
void SSHTerminal::refresh_display_now()
{
    if (terminal_output) {
        lv_refr_now(NULL);
    }
}

lv_obj_t* SSHTerminal::create_terminal_screen()
{
    terminal_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(terminal_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(terminal_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(terminal_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Full-screen terminal: one thin status line, text area, one input line
    const board::Ui& ui = board::ui();
    const int32_t status_h = ui.status_h;
    const int32_t input_h = ui.input_h;
    const int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    const int32_t screen_h = lv_display_get_vertical_resolution(NULL);

    status_bar = lv_label_create(terminal_screen);
    lv_label_set_text(status_bar, "");
    lv_obj_set_style_text_color(status_bar, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(status_bar, ui.small, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_LEFT, 2, 1);
    
    byte_counter_label = lv_label_create(terminal_screen);
    lv_label_set_text(byte_counter_label, "");
    lv_obj_set_style_text_color(byte_counter_label, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_text_font(byte_counter_label, ui.small, 0);
    lv_obj_align(byte_counter_label, LV_ALIGN_TOP_RIGHT, -2, 1);

    terminal_output = lv_textarea_create(terminal_screen);
    lv_obj_set_size(terminal_output, screen_w, screen_h - status_h - input_h);
    lv_obj_align(terminal_output, LV_ALIGN_TOP_LEFT, 0, status_h);
    lv_obj_set_style_bg_color(terminal_output, lv_color_black(), 0);
    lv_obj_set_style_text_color(terminal_output, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(terminal_output, ui.text, 0);
    lv_obj_set_style_border_width(terminal_output, 0, 0);
    lv_obj_set_style_radius(terminal_output, 0, 0);
    lv_obj_set_style_pad_all(terminal_output, 0, 0);
    lv_obj_set_style_pad_hor(terminal_output, 2, 0);
    lv_textarea_set_cursor_click_pos(terminal_output, false);
    lv_textarea_set_one_line(terminal_output, false);
    // Drag or roll the trackball to scroll back; a thin bar shows only while scrolling
    lv_obj_set_scrollbar_mode(terminal_output, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(terminal_output, 2, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(terminal_output, scroll_end_cb, LV_EVENT_SCROLL_END, this);

    // Tell SSH servers the real screen size so output wraps to fit
    const lv_font_t* font = ui.text;
    int32_t char_w = lv_font_get_glyph_width(font, '0', 0);
    int32_t line_h = lv_font_get_line_height(font);
    pty_cols = char_w > 0 ? (screen_w - 4) / char_w : 80;
    pty_rows = line_h > 0 ? (screen_h - status_h - input_h) / line_h : 24;
    
    lv_obj_clear_flag(terminal_output, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_anim_time(terminal_output, 0, LV_PART_CURSOR);
    lv_obj_set_style_opa(terminal_output, LV_OPA_TRANSP, LV_PART_CURSOR);
    
    lv_obj_set_scroll_snap_x(terminal_output, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scroll_snap_y(terminal_output, LV_SCROLL_SNAP_NONE);
    lv_obj_clear_flag(terminal_output, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(terminal_output, LV_OBJ_FLAG_SCROLL_ELASTIC);

    lv_obj_t* input_container = lv_obj_create(terminal_screen);
    lv_obj_set_size(input_container, screen_w - 4, input_h);
    lv_obj_set_style_bg_opa(input_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_container, 0, 0);
    lv_obj_set_style_radius(input_container, 0, 0);
    lv_obj_set_style_pad_all(input_container, 0, 0);
    lv_obj_set_scrollbar_mode(input_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(input_container, LV_DIR_HOR);
    lv_obj_align(input_container, LV_ALIGN_BOTTOM_LEFT, 2, 0);
    
    input_label = lv_label_create(input_container);
    lv_label_set_text(input_label, "> ");
    lv_obj_set_style_text_color(input_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_set_style_text_font(input_label, ui.input, 0);
    lv_label_set_long_mode(input_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(input_label, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Enable touch events on input label
    lv_obj_add_flag(input_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(input_label, input_touch_event_cb, LV_EVENT_CLICKED, this);

    // Full-screen terminal for SSH sessions, on boards that have one (TUI programs)
    term = board::term_screen();
    if (term) {
        term->create(terminal_screen, 0, status_h, screen_w, screen_h - status_h);
        // Dragging in a full-screen program scrolls it (LVGL's task, display lock held)
        term->set_send([this](const char* data, size_t len) {
            if (ssh_connected) {
                last_input_ms = esp_timer_get_time() / 1000;
                write_channel(data, len);
            }
        });
    }

    create_side_panel();
    
    lv_obj_add_event_cb(terminal_screen, gesture_event_cb, LV_EVENT_GESTURE, this);
    lv_obj_clear_flag(terminal_screen, LV_OBJ_FLAG_GESTURE_BUBBLE);
    
    cursor_blink_timer = lv_timer_create(cursor_blink_cb, 500, this);
    
    battery_update_timer = lv_timer_create(battery_update_cb, 60000, this);
    
    history_save_timer = lv_timer_create(history_save_cb, 5000, this);

    lv_textarea_set_text(terminal_output, "");

    return terminal_screen;
}

// True while a finger is dragging this view (it must not jump under the finger)
bool SSHTerminal::touch_scrolling(lv_obj_t* view)
{
    for (lv_indev_t* indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_scroll_obj(indev) == view) {
            return true;
        }
    }
    return false;
}

static bool at_newest(lv_obj_t* view)
{
    return lv_obj_get_scroll_bottom(view) <= 4;
}

// Scrollback: when full, the oldest lines are dropped (never the new text).
// Follows the new text only if the newest line was showing; otherwise the
// lines being read stay where they are.
void SSHTerminal::append_text(const char* text)
{
    static const size_t SCROLLBACK_MAX = 6144;
    static const size_t SCROLLBACK_KEEP = 4096;
    if (!terminal_output || !text || !*text) {
        return;
    }
    bool follow = terminal_follow && !touch_scrolling(terminal_output);
    int32_t view_y = lv_obj_get_scroll_y(terminal_output);
    const char* current = lv_textarea_get_text(terminal_output);
    size_t current_len = strlen(current);
    size_t add_len = strlen(text);
    if (add_len > SCROLLBACK_KEEP) {
        text += add_len - SCROLLBACK_KEEP;
        add_len = SCROLLBACK_KEEP;
    }
    if (current_len + add_len <= SCROLLBACK_MAX) {
        lv_textarea_add_text(terminal_output, text);
    } else {
        size_t keep = SCROLLBACK_KEEP - add_len;
        const char* tail = current + (current_len > keep ? current_len - keep : 0);
        const char* line_start = strchr(tail, '\n');
        if (line_start && keep > 0) {
            tail = line_start + 1;
        }
        if (!follow) {
            // Height of the dropped lines: where the first kept letter (UTF-8) sits now
            uint32_t letter = 0;
            for (const char* c = current; c < tail; c++) {
                letter += ((uint8_t)*c & 0xC0) != 0x80;
            }
            lv_point_t pos;
            lv_label_get_letter_pos(lv_textarea_get_label(terminal_output), letter, &pos);
            view_y = view_y > pos.y ? view_y - pos.y : 0;
        }
        std::string kept(tail);
        kept += text;
        lv_textarea_set_text(terminal_output, kept.c_str());
    }
    // Replaces the text area's own (animated) scroll to its cursor
    lv_obj_scroll_to_y(terminal_output, follow ? LV_COORD_MAX : view_y, LV_ANIM_OFF);
}

void SSHTerminal::clear_terminal()
{
    if (terminal_output) {
        lv_textarea_set_text(terminal_output, "");
        terminal_follow = true;
    }
}

// The scrollable text on screen: the chat while it is open, else the terminal
lv_obj_t* SSHTerminal::visible_view() const
{
    return chat_view ? chat_view : terminal_output;
}

void SSHTerminal::scroll_screen(int direction)
{
    if (raw_keys() && direction != 0) {
        // Full-screen programs page themselves; at the shell, scroll back through its output
        last_input_ms = esp_timer_get_time() / 1000;
        if (term->alt_screen()) {
            term_key(direction > 0 ? TermScreen::Key::PageUp : TermScreen::Key::PageDown);
        } else {
            term->scroll(direction);
        }
        return;
    }
    lv_obj_t* view = visible_view();
    if (!view || direction == 0) {
        return;
    }
    last_input_ms = esp_timer_get_time() / 1000;
    const int32_t step = 3 * lv_font_get_line_height(lv_obj_get_style_text_font(view, LV_PART_MAIN));
    lv_obj_scroll_by_bounded(view, 0, direction > 0 ? step : -step, LV_ANIM_OFF);
    bool follow = at_newest(view);
    (view == chat_view ? chat_follow : terminal_follow) = follow;
}

// Typing brings the newest text back into view
void SSHTerminal::scroll_to_newest()
{
    lv_obj_t* view = visible_view();
    if (view) {
        lv_obj_scroll_to_y(view, LV_COORD_MAX, LV_ANIM_OFF);
        (view == chat_view ? chat_follow : terminal_follow) = true;
    }
}

// A drag (or programmatic scroll) ended: follow new text again only at the bottom
void SSHTerminal::scroll_end_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    lv_obj_t* view = (lv_obj_t*)lv_event_get_target(e);
    if (view == terminal->terminal_output) {
        terminal->terminal_follow = at_newest(view);
    } else if (view == terminal->chat_view) {
        terminal->chat_follow = at_newest(view);
    }
}

// Runs a command typed on the input line (or at the home menu)
void SSHTerminal::execute_command(const std::string& cmd)
{
    // Home menu numbers work any time the menu isn't on screen (and no SSH session is open)
    if (!ssh_connected && cmd.size() == 1 && cmd[0] >= '1' && cmd[0] <= '6') {
        wizard_reset();
        wizard.menu = WizardStep::HomeMenu;
        wizard.step = WizardStep::HomeMenu;
        home_step_input(cmd, cmd);
        return;
    }
    if (cmd == "connect" || cmd == "wifi" || cmd.rfind("wifi ", 0) == 0) {
        handle_wifi_command(cmd == "connect" ? "wifi scan" : cmd);
    }
    // Not while SSH is connected: there 'shutdown' and 'sleep' are sent to the server
    else if (!ssh_connected && cmd == "power") {
        wizard_reset();
        wizard.menu = WizardStep::PowerMenu;
        wizard_goto(WizardStep::PowerMenu);
    }
    else if (!ssh_connected && cmd == "sleep") {
        request_power(PowerRequest::Sleep);
    }
    else if (!ssh_connected && (cmd == "shutdown" || cmd == "poweroff")) {
        request_power(PowerRequest::Off);
    }
    else if (cmd == "vault" || cmd.rfind("vault ", 0) == 0) {
        handle_vault_command(cmd);
    }
    else if (cmd == "hosts" || cmd.rfind("hosts ", 0) == 0) {
        handle_hosts_command(cmd);
    }
    else if (cmd == "storage" || cmd.rfind("storage ", 0) == 0) {
        handle_storage_command(cmd);
    }
    else if (cmd.rfind("connect ", 0) == 0) {
        // Parse arguments with support for quoted strings (for SSIDs/passwords with spaces)
        std::vector<std::string> args;
        std::string arg;
        bool in_quotes = false;
        
        // Skip "connect " prefix
        for (size_t i = 8; i < cmd.length(); i++) {
            char c = cmd[i];
            
            if (c == '"') {
                in_quotes = !in_quotes;
            } else if (c == ' ' && !in_quotes) {
                if (!arg.empty()) {
                    args.push_back(arg);
                    arg.clear();
                }
            } else {
                arg += c;
            }
        }
        if (!arg.empty()) {
            args.push_back(arg);
        }
        
        if (args.size() >= 2) {
            std::string ssid = args[0];
            std::string password = args[1];
            
            append_text("Connecting to WiFi: ");
            append_text(ssid.c_str());
            append_text("\n");
            
            if (init_wifi(ssid.c_str(), password.c_str()) == ESP_OK) {
                append_text("WiFi connected successfully!\n");
                wifi_remember_network(ssid, password, false);
                go_home();
            } else {
                append_text("WiFi connection failed!\n");
            }
            vault::wipe(password);
        } else {
            append_text("Usage: connect <SSID> <PASSWORD>\n");
            append_text("  Use quotes for SSIDs/passwords with spaces: connect \"My WiFi\" password\n");
        }
        for (auto& a : args) {
            vault::wipe(a);
        }
        vault::wipe(arg);
    }
    else if (cmd.rfind("ssh ", 0) == 0) {
        std::vector<std::string> parts;
        size_t pos = 0;
        std::string temp = cmd;
        
        while ((pos = temp.find(' ')) != std::string::npos) {
            parts.push_back(temp.substr(0, pos));
            temp.erase(0, pos + 1);
        }
        parts.push_back(temp);
        
        if (parts.size() >= 5) {
            std::string host = parts[1];
            int port = std::atoi(parts[2].c_str());
            std::string user = parts[3];
            std::string pass = parts[4];
            
            connect(host.c_str(), port, user.c_str(), pass.c_str());
            vault::wipe(pass);
        } else {
            append_text("Usage: ssh <HOST> <PORT> <USER> <PASS>\n");
        }
        for (auto& part : parts) {
            vault::wipe(part);
        }
        vault::wipe(temp);
    }
    else if (cmd.rfind("sshkey ", 0) == 0) {
        std::vector<std::string> parts;
        size_t pos = 0;
        std::string temp = cmd;
        
        while ((pos = temp.find(' ')) != std::string::npos) {
            parts.push_back(temp.substr(0, pos));
            temp.erase(0, pos + 1);
        }
        parts.push_back(temp);
        
        if (parts.size() >= 5) {
            std::string host = parts[1];
            int port = std::atoi(parts[2].c_str());
            std::string user = parts[3];
            std::string keyfile = parts[4];
            std::string passphrase = parts.size() >= 6 ? parts[5] : "";
            
            // Try to load key from memory
            size_t key_len = 0;
            const char* key_data = get_loaded_key(keyfile.c_str(), &key_len);
            
            if (key_data && key_len > 0) {
                // A key with a passphrase that wasn't given is asked for once connected
                append_text("Using key file: ");
                append_text(keyfile.c_str());
                append_text("\n");
                connect_with_key(host.c_str(), port, user.c_str(), key_data, key_len,
                                 passphrase.empty() ? NULL : passphrase.c_str());
            } else {
                append_text("ERROR: Key file not found: ");
                append_text(keyfile.c_str());
                append_text("\n");
                append_text("Available keys: ");
                for (const auto& kv : loaded_keys) {
                    append_text(kv.first.c_str());
                    append_text(" ");
                }
                append_text("\n");
            }
            vault::wipe(passphrase);
        } else {
            append_text("Usage: sshkey <HOST> <PORT> <USER> <KEYFILE> [PASSPHRASE]\n");
            append_text("  Example: sshkey 192.168.1.100 22 pi default.pem\n");
        }
        for (auto& part : parts) {
            vault::wipe(part);
        }
        vault::wipe(temp);
    }
    else if (cmd == "profile" || cmd == "profiles" ||
             cmd.rfind("profile ", 0) == 0 || cmd.rfind("profiles ", 0) == 0) {
        handle_profile_command(cmd);
    }
    else if (cmd == "disconnect") {
        disconnect_wifi();
    }
    else if (cmd == "exit" || cmd == "menu" || cmd == "home") {
        if (cmd == "exit" && (session || ssh_socket >= 0)) {
            disconnect();
        } else if (ssh_connected) {
            append_text("SSH is connected - type 'exit' to close it first.\n");
            return;
        }
        go_home();
    }
    else if (cmd == "chat" || cmd == "chatgpt" || cmd == "chat settings") {
        handle_chat_command(cmd);
    }
    else if (cmd == "clear") {
        clear_terminal();
    }
    else if (cmd == "help") {
        append_text("Available commands:\n");
        append_text("  menu - Home menu (also 'exit' when not connected)\n");
        append_text((std::string("  chat - ChatGPT (") + board::HINT_TALK + " to talk)\n").c_str());
        append_text("  wifi - WiFi menu: scan, pick a network, save it\n");
        append_text("  wifi scan|saved|list|forget|off\n");
        append_text("  connect - Scan and pick a WiFi network\n");
        append_text("  connect <SSID> <PASSWORD> - Connect to WiFi\n");
        append_text("    Use quotes for spaces: connect \"My WiFi\" password\n");
        append_text("  ssh <HOST> <PORT> <USER> <PASS> - Connect via SSH\n");
        append_text("  sshkey <HOST> <PORT> <USER> <KEYFILE> [PASSPHRASE] - SSH with key\n");
        append_text("    Note: Place .pem keys in /sdcard/ssh_keys/ before use\n");
        append_text("  profile - Saved connection profiles menu\n");
        append_text("  profile list|add - List or create profiles\n");
        append_text("  profile connect|edit|delete [NAME|#]\n");
        append_text("  vault - PIN status | vault lock|unlock|pin|reset\n");
        append_text("  hosts - Trusted server keys | hosts forget [HOST|#]\n");
        append_text("  storage - Saved settings: SD backup/restore, load keys\n");
        append_text("  disconnect - Disconnect WiFi\n");
        append_text((std::string("  sleep - Screen and WiFi off until ") + board::HINT_WAKE + "\n").c_str());
        append_text("  shutdown - Turn the device off (also 'poweroff'; 'power' menu)\n");
        append_text("  exit - Disconnect SSH\n");
        append_text("  clear - Clear terminal\n");
        append_text("  help - Show this help\n");
        append_text((std::string(board::HINT_SCROLL) + " " + board::HINT_HISTORY + "\n").c_str());
    }
    else if (ssh_connected) {
        send_command(cmd.c_str());
    } else {
        append_text("Unknown command. Type 'menu' for the menu or 'help' for commands.\n");
    }
}

// Full-screen SSH terminal: keys go straight to the server, like a desktop terminal's
bool SSHTerminal::raw_keys() const
{
    return term && term->is_open() && ssh_connected && !chat_active && !wizard_active();
}

void SSHTerminal::term_key(TermScreen::Key key)
{
    last_input_ms = esp_timer_get_time() / 1000;
    term->scroll_to_newest();
    std::string seq = term->key(key);
    write_channel(seq.data(), seq.size());
}

// Enter sends CR and Backspace DEL, as terminals do
static char terminal_byte(char key)
{
    return key == '\n' ? '\r' : key == 8 ? 0x7F : key;
}

void SSHTerminal::function_key(int n)
{
    if (raw_keys() && n >= 0 && n < 12) {
        term_key((TermScreen::Key)((int)TermScreen::Key::F1 + n));
    }
}

void SSHTerminal::alt_key(char key)
{
    if (!raw_keys()) {
        handle_key_input(key);   // Menus and the input line: just the key
        return;
    }
    last_input_ms = esp_timer_get_time() / 1000;
    term->scroll_to_newest();
    const char seq[2] = {27, terminal_byte(key)};
    write_channel(seq, sizeof(seq));
}

// Shows the full-screen terminal instead of the text view and input line, or back
void SSHTerminal::show_term_view(bool show)
{
    if (!term || (!show && !term->is_open())) {
        return;
    }
    lv_obj_t* input_container = input_label ? lv_obj_get_parent(input_label) : NULL;
    if (show) {
        term->open();   // Always a blank screen for a new session
        lv_obj_add_flag(terminal_output, LV_OBJ_FLAG_HIDDEN);
        if (input_container) {
            lv_obj_add_flag(input_container, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        term->close();
        lv_obj_clear_flag(terminal_output, LV_OBJ_FLAG_HIDDEN);
        if (input_container) {
            lv_obj_clear_flag(input_container, LV_OBJ_FLAG_HIDDEN);
        }
        scroll_to_newest();
    }
}

void SSHTerminal::handle_key_input(char key)
{
    last_input_ms = esp_timer_get_time() / 1000;
    if (raw_keys()) {
        term->scroll_to_newest();
        char out = terminal_byte(key);
        write_channel(&out, 1);
        return;
    }
    scroll_to_newest();

    if (wizard_active() && (key == '\n' || key == '\r')) {
        // Wizard answers are never added to command history (they may be passwords)
        std::string answer = current_input;
        current_input.clear();
        cursor_pos = 0;
        history_index = -1;
        wizard_handle_input(answer);
    } else if (wizard_active() && key == 27) {
        current_input.clear();
        cursor_pos = 0;
        append_text("\nCancelled.\n");
        if (wizard_holds_connection()) {
            ssh_teardown();
        }
        go_home();
    } else if (chat_active && key == 27) {
        chat_stop();   // Esc stops recording, the reply or speech
    } else if (chat_active && (key == '\n' || key == '\r')) {
        std::string text = current_input;
        vault::wipe(current_input);
        cursor_pos = 0;
        chat_submit(text);
    } else if (key == '\n' || key == '\r') {
        if (!current_input.empty()) {
            bool has_secret = false;
            std::string shown = redact_secrets(current_input, &has_secret);
            append_text("\n> ");
            append_text(shown.c_str());
            append_text("\n");
            
            execute_command(current_input);

            // Commands carrying a password stay out of history (it is stored in NVS)
            if (!has_secret) {
                auto it = std::find(command_history.begin(), command_history.end(), current_input);
                if (it != command_history.end()) {
                    command_history.erase(it);
                }
                command_history.push_back(current_input);
                history_needs_save = true;
            }
            vault::wipe(current_input);
            cursor_pos = 0;
            history_index = -1;
        }
    } else if (ssh_connected && !chat_active && !wizard_active() && key != 8 && key > 0 && key < 32) {
        // Ctrl+letter, Tab and Esc from a full keyboard go straight to the server,
        // like the side panel's keys (the typed line is sent with Enter)
        write_channel(&key, 1);
    } else if (wizard_active() && key == 3) {
        handle_key_input(27);   // Ctrl+C cancels a menu like Esc
        return;
    } else if (key == 8 || key == 127) {
        // Backspace - delete character before cursor
        if (cursor_pos > 0 && !current_input.empty()) {
            current_input.erase(cursor_pos - 1, 1);
            cursor_pos--;
        }
    } else if (key >= 32 && key <= 126) {
        // Insert character at cursor position
        current_input.insert(cursor_pos, 1, key);
        cursor_pos++;
    }
    
    update_input_display();
}

void SSHTerminal::update_input_display()
{
    if (!input_label) {
        return;
    }
    
    // Ensure cursor_pos is within bounds
    if (cursor_pos > current_input.length()) {
        cursor_pos = current_input.length();
    }
    
    if (!input_banner.empty()) {
        lv_label_set_text(input_label, input_banner.c_str());
        return;
    }
    std::string full_text = "> " + input_display_text();
    
    // Insert cursor at correct position
    if (cursor_visible) {
        size_t display_pos = 2 + cursor_pos; // 2 = length of "> "
        full_text.insert(display_pos, "|");
    }
    
    lv_label_set_text(input_label, full_text.c_str());
    
    lv_obj_t* container = lv_obj_get_parent(input_label);
    if (container) {
        lv_obj_scroll_to_x(container, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void SSHTerminal::input_touch_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    lv_obj_t* input_label = (lv_obj_t* )lv_event_get_target(e);
    
    if (!terminal || !input_label) {
        return;
    }
    
    // Get the touch point relative to the label
    lv_point_t point;
    lv_indev_get_point(lv_indev_get_act(), &point);
    
    // Convert to label coordinates
    lv_obj_t* label = input_label;
    lv_area_t label_coords;
    lv_obj_get_coords(label, &label_coords);
    
    int32_t click_x = point.x - label_coords.x1;
    
    // Get font and calculate character position
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    
    // Account for "> " prefix (2 characters)
    const char* prefix = "> ";
    lv_point_t prefix_size;
    lv_txt_get_size(&prefix_size, prefix, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t prefix_width = prefix_size.x;
    
    if (click_x <= prefix_width) {
        // Clicked on prefix, set cursor to start
        terminal->cursor_pos = 0;
    } else {
        // Calculate which character was clicked
        int32_t text_x = click_x - prefix_width;
        size_t best_pos = 0;
        int32_t min_distance = INT32_MAX;
        
        // Check each character position
        std::string shown = terminal->input_display_text();
        for (size_t i = 0; i <= shown.length(); i++) {
            std::string substr = shown.substr(0, i);
            lv_point_t substr_size;
            lv_txt_get_size(&substr_size, substr.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            int32_t char_x = substr_size.x;
            int32_t distance = abs(text_x - char_x);
            
            if (distance < min_distance) {
                min_distance = distance;
                best_pos = i;
            }
        }
        
        terminal->cursor_pos = best_pos;
    }
    
    // Reset cursor blink to make it visible
    terminal->cursor_visible = true;
    terminal->update_input_display();
    
}

void SSHTerminal::cursor_blink_cb(lv_timer_t* timer)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_timer_get_user_data(timer);
    if (terminal) {
        terminal->cursor_visible = !terminal->cursor_visible;
        terminal->update_input_display();
        if (terminal->term && terminal->term->is_open()) {
            terminal->term->blink(terminal->cursor_visible);
        }
    }
}

void SSHTerminal::battery_update_cb(lv_timer_t* timer)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_timer_get_user_data(timer);
    if (terminal) {
        if (bsp_display_lock(0)) {
            terminal->update_status_bar();
            bsp_display_unlock();
        }
    }
}

void SSHTerminal::history_save_cb(lv_timer_t* timer)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_timer_get_user_data(timer);
    if (terminal && terminal->history_needs_save) {
        terminal->history_needs_save = false;
        terminal->save_history_to_nvs();
    }
}

void SSHTerminal::navigate_history(int direction)
{
    if (raw_keys()) {
        term_key(direction > 0 ? TermScreen::Key::Up : TermScreen::Key::Down);
        return;
    }
    if (wizard_active()) {
        // In a menu, trackball left/right cycles the choices instead of history
        wizard_cycle_choice(direction);
        return;
    }
    
    if (command_history.empty()) {
        return;
    }
    
    if (direction > 0) {
        if (history_index < (int)command_history.size() - 1) {
            history_index++;
            current_input = command_history[command_history.size() - 1 - history_index];
        }
    } else if (direction < 0) {
        if (history_index > 0) {
            history_index--;
            current_input = command_history[command_history.size() - 1 - history_index];
        } else if (history_index == 0) {
            history_index = -1;
            current_input.clear();
        }
    }
    
    // Move cursor to end of input
    cursor_pos = current_input.length();
    update_input_display();
}

void SSHTerminal::move_cursor_left()
{
    if (raw_keys()) {
        term_key(TermScreen::Key::Left);
        return;
    }
    if (cursor_pos > 0) {
        cursor_pos--;
        cursor_visible = true;
        update_input_display();
    }
}

void SSHTerminal::move_cursor_right()
{
    if (raw_keys()) {
        term_key(TermScreen::Key::Right);
        return;
    }
    if (cursor_pos < current_input.length()) {
        cursor_pos++;
        cursor_visible = true;
        update_input_display();
    }
}

void SSHTerminal::move_cursor_home()
{
    if (raw_keys()) {
        term_key(TermScreen::Key::Home);
        return;
    }
    cursor_pos = 0;
    cursor_visible = true;
    update_input_display();
}

void SSHTerminal::move_cursor_end()
{
    if (raw_keys()) {
        term_key(TermScreen::Key::End);
        return;
    }
    cursor_pos = current_input.length();
    cursor_visible = true;
    update_input_display();
}

void SSHTerminal::delete_at_cursor()
{
    if (raw_keys()) {
        term_key(TermScreen::Key::Delete);
        return;
    }
    if (cursor_pos < current_input.length()) {
        current_input.erase(cursor_pos, 1);
        cursor_visible = true;
        update_input_display();
    }
}

void SSHTerminal::delete_current_history_entry()
{
    if (command_history.empty() || history_index < 0) {
        ESP_LOGW(TAG, "No history entry to delete (empty or not navigating)");
        return;
    }
    
    size_t actual_index = command_history.size() - 1 - history_index;
    
    command_history.erase(command_history.begin() + actual_index);
    
    history_needs_save = true;
    
    if (command_history.empty()) {
        history_index = -1;
        current_input.clear();
    } else if (history_index >= (int)command_history.size()) {
        history_index = command_history.size() - 1;
        current_input = command_history[command_history.size() - 1 - history_index];
    } else if (actual_index < command_history.size()) {
        current_input = command_history[command_history.size() - 1 - history_index];
    } else {
        history_index = -1;
        current_input.clear();
    }
    
    std::string display_text = "> " + current_input;
    if (input_label) {
        lv_label_set_text(input_label, display_text.c_str());
    }
    
    ESP_LOGI(TAG, "History entry deleted. Remaining entries: %d", (int)command_history.size());
}

void SSHTerminal::load_history_from_nvs()
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = settings_nvs::open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading history: %s", esp_err_to_name(err));
        return;
    }
    
    uint32_t history_count = 0;
    err = nvs_get_u32(nvs_handle, "hist_count", &history_count);
    if (err != ESP_OK || history_count == 0) {
        ESP_LOGI(TAG, "No command history found in NVS");
        nvs_close(nvs_handle);
        return;
    }
    
    ESP_LOGI(TAG, "Loading %lu commands from NVS...", (unsigned long)history_count);
    
    command_history.clear();
    for (uint32_t i = 0; i < history_count && i < 100; i++) {
        char key[16];
        snprintf(key, sizeof(key), "hist_%lu", (unsigned long)i);
        
        size_t required_size = 0;
        err = nvs_get_str(nvs_handle, key, NULL, &required_size);
        if (err != ESP_OK) {
            continue;
        }
        
        char* cmd = (char*)malloc(required_size);
        if (cmd) {
            err = nvs_get_str(nvs_handle, key, cmd, &required_size);
            if (err == ESP_OK) {
                bool has_secret = false;
                redact_secrets(cmd, &has_secret);
                if (has_secret) {
                    history_needs_save = true;  // Rewrite history without it
                } else {
                    command_history.push_back(std::string(cmd));
                }
            }
            memset(cmd, 0, required_size);
            free(cmd);
        }
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Loaded %d commands from NVS", (int)command_history.size());
}

void SSHTerminal::save_history_to_nvs()
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = settings_nvs::open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing history: %s", esp_err_to_name(err));
        return;
    }
    
    size_t start_idx = 0;
    if (command_history.size() > 100) {
        start_idx = command_history.size() - 100;
    }
    
    uint32_t history_count = command_history.size() - start_idx;
    err = nvs_set_u32(nvs_handle, "hist_count", history_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save history count: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return;
    }
    
    int saved_count = 0;
    for (size_t i = start_idx; i < command_history.size() && saved_count < 100; i++) {
        char key[16];
        snprintf(key, sizeof(key), "hist_%lu", (unsigned long)(i - start_idx));
        
        err = nvs_set_str(nvs_handle, key, command_history[i].c_str());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save command %zu: %s", i, esp_err_to_name(err));
        } else {
            saved_count++;
        }
        
        if (saved_count % 10 == 0) {
            vTaskDelay(1);
        }
    }
    
    // Erase entries beyond the new count, so deleted commands don't linger in flash
    for (uint32_t i = history_count; i < 100; i++) {
        char key[16];
        snprintf(key, sizeof(key), "hist_%lu", (unsigned long)i);
        nvs_erase_key(nvs_handle, key);
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved %d commands to NVS", saved_count);
    }
    
    nvs_close(nvs_handle);
}

// Accepts a dotted IPv4 address or a host name (resolved via DNS)
static esp_err_t resolve_host(const char* host, struct in_addr* addr)
{
    if (inet_aton(host, addr)) {
        return ESP_OK;
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;

    int err = getaddrinfo(host, NULL, &hints, &res);
    if (err != 0 || !res) {
        ESP_LOGE(TAG, "DNS lookup failed for %s: %d", host, err);
        return ESP_FAIL;
    }

    *addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return ESP_OK;
}

// Opens a TCP connection, waiting at most timeout_s. Returns the socket (>= 0) or -errno.
static int tcp_connect(const struct sockaddr_in& sin, int timeout_s)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -(errno ? errno : ENOMEM);
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int err = 0;
    if (::connect(fd, (const struct sockaddr*)&sin, sizeof(sin)) != 0) {
        err = errno;
        if (err == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv = {timeout_s, 0};
            int rc = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (rc == 0) {
                err = ETIMEDOUT;
            } else if (rc < 0) {
                err = errno;
            } else {
                socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
                    err = errno;
                }
            }
        }
    }
    if (err) {
        close(fd);
        return -err;
    }
    fcntl(fd, F_SETFL, flags);   // Back to blocking; libssh2 sets its own mode
    struct timeval timeout = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

// What a failed connection means, for the user
static std::string connect_error_text(int err)
{
    switch (err) {
        case ECONNREFUSED:
            return "connection refused - is SSH running on that port, or is the device blocked (e.g. fail2ban)?";
        case ETIMEDOUT:
            return "no answer in 10 s - check the address, and that the server is reachable from this WiFi";
        case EHOSTUNREACH:
        case ENETUNREACH:
            return "no route to the server - check the address and the WiFi connection";
        case ECONNRESET:
        case ECONNABORTED:
            return "the connection was reset";
        case ENFILE:
        case EMFILE:
        case ENOMEM:
        case ENOBUFS:
            return "out of network resources - try again, or restart the device";
        default:
            return "error " + std::to_string(err) + " (" + strerror(err) + ")";
    }
}

int SSHTerminal::waitsocket(int socket_fd, LIBSSH2_SESSION *session)
{
    struct timeval timeout;
    int rc;
    fd_set fd;
    fd_set *writefd = NULL;
    fd_set *readfd = NULL;
    int dir;

    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    FD_ZERO(&fd);
    FD_SET(socket_fd, &fd);

    dir = libssh2_session_block_directions(session);

    if(dir & LIBSSH2_SESSION_BLOCK_INBOUND)
        readfd = &fd;

    if(dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
        writefd = &fd;

    rc = select(socket_fd + 1, readfd, writefd, NULL, &timeout);

    return rc;
}

// Upper bound on waitsocket() rounds (2 s each) for one SSH protocol step
static const int SSH_MAX_WAITS = 15;

// libssh2 global state is initialized once and kept for the life of the app
static bool ensure_libssh2_init()
{
    static bool initialized = false;
    if (!initialized) {
        initialized = libssh2_init(0) == 0;
    }
    return initialized;
}

esp_err_t SSHTerminal::connect(const char* host, int port, const char* username, const char* password)
{
    PendingSsh target;
    target.host = host;
    target.port = port;
    target.username = username;
    target.password = password ? password : "";
    return ssh_begin(std::move(target));
}

esp_err_t SSHTerminal::connect_with_key(const char* host, int port, const char* username, const char* privkey_data, size_t privkey_len,
                                        const char* passphrase)
{
    PendingSsh target;
    target.host = host;
    target.port = port;
    target.username = username;
    target.use_key = true;
    target.key_data = privkey_data;
    target.key_len = privkey_len;
    target.passphrase = passphrase ? passphrase : "";
    return ssh_begin(std::move(target));
}

// Opens the socket, runs the SSH handshake and checks the server's host key.
// A known key continues straight to authentication; a new key waits for the user.
esp_err_t SSHTerminal::ssh_begin(PendingSsh&& target)
{
    wipe_pending_ssh();
    pending_ssh = std::move(target);
    PendingSsh& t = pending_ssh;

    if (!wifi_connected) {
        append_text("ERROR: WiFi not connected. Type 'wifi' to connect.\n");
        wipe_pending_ssh();
        return ESP_FAIL;
    }
    if (ssh_connected || session) {
        append_text("Already connected. Type 'exit' to disconnect first.\n");
        wipe_pending_ssh();
        return ESP_FAIL;
    }
    if (!ensure_libssh2_init()) {
        append_text("ERROR: libssh2 init failed\n");
        wipe_pending_ssh();
        return ESP_FAIL;
    }

    append_text(("Connecting to " + t.host + ":" + std::to_string(t.port) + "...\n").c_str());
    refresh_display_now();

    struct sockaddr_in sin = {};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(t.port);
    if (resolve_host(t.host.c_str(), &sin.sin_addr) != ESP_OK) {
        append_text("ERROR: Could not resolve host\n");
        wipe_pending_ssh();
        return ESP_FAIL;
    }
    char ip[16];
    inet_ntoa_r(sin.sin_addr, ip, sizeof(ip));
    if (t.host != ip) {
        append_text(("  (" + t.host + " is " + ip + ")\n").c_str());   // Shows a name resolving wrongly
    }
    refresh_display_now();

    // One retry for errors a freshly joined network can give (the route or ARP not ready yet)
    int fd = tcp_connect(sin, 10);
    if (fd == -EHOSTUNREACH || fd == -ENETUNREACH || fd == -ECONNRESET || fd == -ECONNABORTED) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        fd = tcp_connect(sin, 10);
    }
    if (fd < 0) {
        ESP_LOGE(TAG, "TCP connect failed: errno %d", -fd);
        append_text(("ERROR: Could not connect to " + std::string(ip) + ":" + std::to_string(t.port) + ": " +
                     connect_error_text(-fd) + "\n").c_str());
        wipe_pending_ssh();
        return ESP_FAIL;
    }
    ssh_socket = fd;

    session = libssh2_session_init();
    if (!session) {
        append_text("ERROR: Failed to create SSH session\n");
        ssh_teardown();
        return ESP_FAIL;
    }
    libssh2_session_set_blocking(session, 0);

    append_text("Performing SSH handshake...\n");
    int rc;
    int waits = 0;
    while ((rc = libssh2_session_handshake(session, ssh_socket)) == LIBSSH2_ERROR_EAGAIN &&
           (waitsocket(ssh_socket, session) > 0 || ++waits < SSH_MAX_WAITS)) {
    }
    if (rc) {
        ESP_LOGE(TAG, "SSH handshake failed: %d", rc);
        append_text("ERROR: SSH handshake failed\n");
        ssh_teardown();
        return ESP_FAIL;
    }

    switch (check_host_key(t.host, t.port, t.fingerprint, t.key_type)) {
        case HostKeyStatus::Match:
            return ssh_finish();

        case HostKeyStatus::Unknown:
            append_text(("New server " + t.host + ":" + std::to_string(t.port) + "\n  " + t.key_type + " " +
                         t.fingerprint + "\n").c_str());
            append_text("Check it on the server: ssh-keygen -lf /etc/ssh/ssh_host_*_key.pub\n");
            wizard_reset();
            wizard_goto(WizardStep::HostTrust);
            return ESP_OK;

        case HostKeyStatus::Mismatch:
            append_text("\n!!! WARNING: HOST KEY HAS CHANGED !!!\n");
            append_text(("  " + t.host + ":" + std::to_string(t.port) + " now presents\n  " + t.key_type + " " +
                         t.fingerprint + "\n").c_str());
            append_text("Someone may be intercepting the connection. Refused.\n");
            append_text("If the server was reinstalled, remove the old key: hosts forget\n");
            ssh_teardown();
            return ESP_FAIL;

        case HostKeyStatus::Error:
        default:
            append_text("ERROR: Could not read the server host key\n");
            ssh_teardown();
            return ESP_FAIL;
    }
}

// Authenticates with the pending credentials and opens the interactive shell
esp_err_t SSHTerminal::ssh_finish()
{
    PendingSsh& t = pending_ssh;
    esp_err_t err;

    if (t.use_key) {
        err = ssh_authenticate_pubkey(t.username.c_str(), t.key_data, t.key_len,
                                      t.passphrase.empty() ? NULL : t.passphrase.c_str());
        if (err == ESP_ERR_INVALID_ARG && t.passphrase_prompts < 3) {
            // The key is locked: ask for its passphrase and try again on this connection
            append_text(t.passphrase.empty() ? "The key can't be read without its passphrase.\n" : "Wrong passphrase.\n");
            vault::wipe(t.passphrase);
            t.passphrase_prompts++;
            wizard_reset();
            wizard_goto(WizardStep::SshKeyPassphrase);
            return ESP_OK;
        }
        if (err == ESP_ERR_INVALID_ARG) {
            append_text("ERROR: Could not read the key - wrong passphrase, or not an RSA key in PEM format\n");
        }
    } else {
        err = ssh_authenticate(t.username.c_str(), t.password.c_str());
    }
    wipe_pending_ssh();

    if (err != ESP_OK) {
        append_text("ERROR: Authentication failed\n");
        ssh_teardown();
        return ESP_FAIL;
    }
    append_text("Authentication successful\n");

    if (ssh_open_channel() != ESP_OK) {
        append_text("ERROR: Failed to open channel\n");
        ssh_teardown();
        return ESP_FAIL;
    }

    append_text("SSH channel opened - connected!\n");
    ssh_connected = true;
    show_term_view(true);
    update_status_bar();

    xTaskCreate(ssh_receive_task, "ssh_rx", 8192, this, 5, NULL);
    return ESP_OK;
}

esp_err_t SSHTerminal::ssh_authenticate(const char* username, const char* password)
{
    append_text("Authenticating as ");
    append_text(username);
    append_text("...\n");
    refresh_display_now();

    int rc;
    int waits = 0;
    while ((rc = libssh2_userauth_password(session, username, password)) == LIBSSH2_ERROR_EAGAIN &&
           (waitsocket(ssh_socket, session) > 0 || ++waits < SSH_MAX_WAITS)) {
    }

    if (rc) {
        ESP_LOGE(TAG, "Password authentication failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t SSHTerminal::ssh_authenticate_pubkey(const char* username, const char* privkey_data, size_t privkey_len,
                                               const char* passphrase)
{
    append_text("Authenticating as ");
    append_text(username);
    append_text(" with public key...\n");
    refresh_display_now();

    int rc;
    int waits = 0;
    while ((rc = libssh2_userauth_publickey_frommemory(session, username, strlen(username),
                                                         NULL, 0,  // public key (derived from the private key)
                                                         privkey_data, privkey_len,
                                                         passphrase)) == LIBSSH2_ERROR_EAGAIN &&
           (waitsocket(ssh_socket, session) > 0 || ++waits < SSH_MAX_WAITS)) {
    }

    if (rc) {
        ESP_LOGE(TAG, "Public key authentication failed: %d", rc);
        if (rc == LIBSSH2_ERROR_FILE) {
            // Searched in place: the key is not copied
            if (std::string_view(privkey_data, privkey_len).find("OPENSSH PRIVATE KEY") != std::string_view::npos) {
                append_text("ERROR: This key is in OpenSSH format, which isn't supported. Convert it\n"
                            "on your computer (keeps the passphrase): ssh-keygen -p -m PEM -f KEYFILE\n");
                return ESP_FAIL;
            }
            return ESP_ERR_INVALID_ARG;   // Locked (needs a passphrase) or the passphrase is wrong
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t SSHTerminal::ssh_open_channel()
{
    append_text("Opening SSH channel...\n");

    int rc;
    int waits = 0;
    while ((channel = libssh2_channel_open_session(session)) == NULL &&
           libssh2_session_last_error(session, NULL, NULL, 0) == LIBSSH2_ERROR_EAGAIN &&
           (waitsocket(ssh_socket, session) > 0 || ++waits < SSH_MAX_WAITS)) {
    }
    if (channel == NULL) {
        ESP_LOGE(TAG, "Failed to open channel");
        return ESP_FAIL;
    }

    // A full-screen terminal is an xterm with 256 colors; the T-Deck's scrolling text a plain vt100
    const char* term_type = term ? "xterm-256color" : "vt100";
    const int cols = term ? term->cols() : pty_cols;
    const int rows = term ? term->rows() : pty_rows;
    waits = 0;
    while ((rc = libssh2_channel_request_pty_ex(channel, term_type, strlen(term_type), NULL, 0, cols, rows, 0, 0)) ==
               LIBSSH2_ERROR_EAGAIN &&
           (waitsocket(ssh_socket, session) > 0 || ++waits < SSH_MAX_WAITS)) {
    }
    if (rc) {
        ESP_LOGE(TAG, "Failed to request PTY");
        return ESP_FAIL;
    }

    waits = 0;
    while ((rc = libssh2_channel_shell(channel)) == LIBSSH2_ERROR_EAGAIN &&
           (waitsocket(ssh_socket, session) > 0 || ++waits < SSH_MAX_WAITS)) {
    }
    if (rc) {
        ESP_LOGE(TAG, "Failed to start shell");
        return ESP_FAIL;
    }

    libssh2_channel_set_blocking(channel, 0);
    return ESP_OK;
}

void SSHTerminal::wipe_pending_ssh()
{
    vault::wipe(pending_ssh.password);
    vault::wipe(pending_ssh.passphrase);
    pending_ssh = PendingSsh();
}

// Frees the channel, session and socket. Safe to call from any task: the mutex
// keeps the receive task from using the session while it is being freed.
void SSHTerminal::ssh_teardown()
{
    xSemaphoreTake(ssh_mutex, portMAX_DELAY);
    ssh_generation++;
    ssh_connected = false;
    if (channel) {
        libssh2_channel_free(channel);
        channel = NULL;
    }
    if (session) {
        libssh2_session_disconnect(session, "Normal Shutdown");
        libssh2_session_free(session);
        session = NULL;
    }
    if (ssh_socket >= 0) {
        close(ssh_socket);
        ssh_socket = -1;
    }
    xSemaphoreGive(ssh_mutex);
    wipe_pending_ssh();
}

esp_err_t SSHTerminal::disconnect()
{
    bool was_open = session != NULL || ssh_socket >= 0;
    ssh_teardown();

    if (was_open && bsp_display_lock(0)) {
        update_status_bar();
        append_text("\nDisconnected\n");
        bsp_display_unlock();
    }
    return ESP_OK;
}

void SSHTerminal::send_command(const char* cmd)
{
    bytes_received = 0;
    if (byte_counter_label && bsp_display_lock(0)) {
        lv_label_set_text(byte_counter_label, "0 B");
        bsp_display_unlock();
    }

    std::string full_cmd = std::string(cmd) + "\n";
    ssize_t nwritten = 0;
    int waits = 0;

    xSemaphoreTake(ssh_mutex, portMAX_DELAY);
    while (channel && nwritten < (ssize_t)full_cmd.length()) {
        ssize_t n = libssh2_channel_write(channel, full_cmd.c_str() + nwritten, full_cmd.length() - nwritten);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            if (waitsocket(ssh_socket, session) <= 0 && ++waits >= SSH_MAX_WAITS) {
                ESP_LOGE(TAG, "Send timed out");
                break;
            }
            continue;
        }
        if (n < 0) {
            ESP_LOGE(TAG, "Failed to write to channel: %d", (int)n);
            break;
        }
        nwritten += n;
        waits = 0;
    }
    xSemaphoreGive(ssh_mutex);
}

void SSHTerminal::ssh_receive_task(void* param)
{
    SSHTerminal* terminal = (SSHTerminal*)param;
    const uint32_t generation = terminal->ssh_generation;
    char buffer[1024];
    bool closed_by_server = false;

    while (true) {
        xSemaphoreTake(terminal->ssh_mutex, portMAX_DELAY);
        if (generation != terminal->ssh_generation || !terminal->channel) {
            xSemaphoreGive(terminal->ssh_mutex);
            break;  // Disconnected elsewhere
        }
        ssize_t rc = libssh2_channel_read(terminal->channel, buffer, sizeof(buffer) - 1);
        bool eof = libssh2_channel_eof(terminal->channel);
        xSemaphoreGive(terminal->ssh_mutex);

        if (rc > 0) {
            buffer[rc] = '\0';
            terminal->process_received_data(buffer, rc);
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            terminal->flush_display_buffer();
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (rc < 0) {
            ESP_LOGE(TAG, "Read error: %d", (int)rc);
            closed_by_server = true;
            break;
        }

        if (eof) {
            terminal->flush_display_buffer();
            closed_by_server = true;
            break;
        }
        vTaskDelay(1);
    }

    if (closed_by_server && generation == terminal->ssh_generation) {
        terminal->disconnect();
        terminal->home_pending = true;
    }
    vTaskDelete(NULL);
}

std::string SSHTerminal::strip_ansi_codes(const char* data, size_t len)
{
    std::string result;
    result.reserve(len);
    
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && i % 1024 == 0) {
            vTaskDelay(1);
        }
        
        if (data[i] == '\x1B' || data[i] == '\033') {
            i++;
            if (i >= len) break;
            
            if (data[i] == '[') {
                i++;
                while (i < len && !((data[i] >= 'A' && data[i] <= 'Z') || 
                                    (data[i] >= 'a' && data[i] <= 'z'))) {
                    i++;
                }
            } else if (data[i] == ']') {
                i++;
                while (i < len) {
                    if (data[i] == '\007') break;
                    if (data[i] == '\x1B' && i + 1 < len && data[i + 1] == '\\') {
                        i++;
                        break;
                    }
                    i++;
                }
            } else if (data[i] == '(' || data[i] == ')') {
                i++;
            }
        } else if (data[i] == '\r') {
            continue;
        } else {
            result += data[i];
        }
    }
    
    return result;
}

void SSHTerminal::process_received_data(const char* data, size_t len)
{
    bytes_received += len;

    if (term) {
        // Full-screen terminal: LVGL draws from its cells, so they change under the display lock
        std::string reply;
        bsp_display_lock(0);
        term->feed(data, len, reply);
        bsp_display_unlock();
        if (!reply.empty()) {
            write_channel(reply.data(), reply.size());   // Answers to the program's queries
        }
        return;
    }
    
    std::string cleaned = strip_ansi_codes(data, len);
    text_buffer += cleaned;
    
    int64_t current_time = esp_timer_get_time() / 1000;
    
    if (text_buffer.size() > 2048) {
        text_buffer = text_buffer.substr(text_buffer.size() - 1024);
    }
    
    if (current_time - last_display_update >= 1000) {
        flush_display_buffer();
    }
    
    vTaskDelay(1);
}

void SSHTerminal::flush_display_buffer()
{
    if (text_buffer.empty() && bytes_received == 0) {
        return;
    }
    
    const size_t CHUNK_SIZE = 256;
    size_t offset = 0;
    
    while (offset < text_buffer.size()) {
        if (bsp_display_lock(0)) {
            size_t chunk_len = std::min(CHUNK_SIZE, text_buffer.size() - offset);
            std::string chunk = text_buffer.substr(offset, chunk_len);
            append_text(chunk.c_str());
            bsp_display_unlock();
            offset += chunk_len;
            
            vTaskDelay(1);
        } else {
            break;
        }
    }
    
    if (offset > 0) {
        text_buffer = text_buffer.substr(offset);
    }
    
    if (bytes_received > 0 && byte_counter_label && bsp_display_lock(0)) {
        char counter_text[32];
        if (bytes_received < 1024) {
            snprintf(counter_text, sizeof(counter_text), "%zu B", bytes_received);
        } else if (bytes_received < 1024 * 1024) {
            snprintf(counter_text, sizeof(counter_text), "%.1f KB", bytes_received / 1024.0);
        } else {
            snprintf(counter_text, sizeof(counter_text), "%.2f MB", bytes_received / (1024.0 * 1024.0));
        }
        lv_label_set_text(byte_counter_label, counter_text);
        bsp_display_unlock();
    }
    
    if (text_buffer.empty()) {
        last_display_update = esp_timer_get_time() / 1000;
    }
    
    vTaskDelay(1);
}

void SSHTerminal::update_status_bar()
{
    if (!status_bar) return;
    
    if (!bsp_display_lock(0)) {
        return;
    }

    std::string status;
    
    if (battery_initialized) {
        float voltage = battery.readBatteryVoltage();
        if (voltage > 0.1f) {
            char voltage_text[32];
            snprintf(voltage_text, sizeof(voltage_text), "%.2fV | ", voltage);
            status = voltage_text;
            ESP_LOGD(TAG, "Battery voltage displayed: %.2fV", voltage);
        } else {
            ESP_LOGW(TAG, "Battery voltage too low or invalid: %.2fV", voltage);
        }
    } else {
        ESP_LOGD(TAG, "Battery not initialized, skipping voltage display");
    }
    
    if (!wifi_connected) {
        status += LV_SYMBOL_WIFI " OFF";
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0xFF0000), 0);
    } else if (!ssh_connected) {
        status += LV_SYMBOL_WIFI " | " LV_SYMBOL_CLOSE " SSH";
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0xFFFF00), 0);
    } else {
        status += LV_SYMBOL_WIFI " | " LV_SYMBOL_OK " SSH";
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0x00FF00), 0);
    }
    
    lv_label_set_text(status_bar, status.c_str());
    
    bsp_display_unlock();
}

void SSHTerminal::create_side_panel()
{
    const board::Ui& ui = board::ui();
    side_panel = lv_obj_create(terminal_screen);
    lv_obj_set_size(side_panel, ui.panel_w, lv_pct(100));
    lv_obj_set_style_bg_color(side_panel, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(side_panel, LV_OPA_80, 0);
    lv_obj_set_style_border_color(side_panel, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(side_panel, 2, 0);
    lv_obj_set_scrollbar_mode(side_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(side_panel, LV_DIR_VER);
    lv_obj_align(side_panel, LV_ALIGN_TOP_RIGHT, ui.panel_w, 0);
    lv_obj_add_flag(side_panel, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* title = lv_label_create(side_panel);
    lv_label_set_text(title, "Keys");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(title, ui.input, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, ui.gap);
    
    // Buttons stacked below the title, one row (button + gap) apart
    int row = 0;
    auto create_key_button = [this, &ui, &row](const char* label, const char* key_seq) {
        lv_obj_t* btn = lv_btn_create(side_panel);
        lv_obj_set_size(btn, ui.button_w, ui.button_h);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, (row++ + 1) * (ui.button_h + ui.gap + 1));
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x00CC00), LV_STATE_PRESSED);
        
        lv_obj_t* btn_label = lv_label_create(btn);
        lv_label_set_text(btn_label, label);
        lv_obj_set_style_text_color(btn_label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_text_font(btn_label, ui.small, 0);
        lv_obj_center(btn_label);
        
        lv_obj_set_user_data(btn, (void*)key_seq);
        lv_obj_add_event_cb(btn, special_key_event_cb, LV_EVENT_CLICKED, this);
        
        return btn;
    };

    create_key_button("<-", "LEFT");
    create_key_button("->", "RIGHT");
    create_key_button("Line <", "HOME");
    create_key_button("> Line", "END");
    create_key_button("Ctrl+C", "\x03");
    create_key_button("Ctrl+Z", "\x1A");
    create_key_button("Ctrl+D", "\x04");
    create_key_button("Ctrl+L", "\x0C");
    create_key_button("Tab", "\t");
    create_key_button("Esc", "\x1B");
    create_key_button("Exit SSH", "EXIT");
    create_key_button("Clear", "CLEAR");
}

void SSHTerminal::toggle_side_panel()
{
    if (!side_panel) return;
    
    if (lv_obj_has_flag(side_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(side_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(side_panel, LV_ALIGN_TOP_RIGHT, 0, 0);
    } else {
        lv_obj_add_flag(side_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(side_panel, LV_ALIGN_TOP_RIGHT, board::ui().panel_w, 0);
    }
}

void SSHTerminal::send_special_key(const char* sequence)
{
    if (!sequence || strlen(sequence) == 0) {
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "EXIT") == 0) {
        disconnect();
        home_pending = true;
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "CLEAR") == 0) {
        if (raw_keys()) {
            write_channel("\x0c", 1);   // Ctrl+L: the shell or program redraws a clean screen
        } else if (chat_active) {
            chat_clear();
        } else {
            clear_terminal();
        }
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "\x1B") == 0 && chat_active && !wizard_active()) {
        chat_stop();
        toggle_side_panel();
        return;
    }

    if (strcmp(sequence, "\x1B") == 0 && wizard_active()) {
        handle_key_input(27);
        toggle_side_panel();
        return;
    }
    
    // Handle cursor movement keys
    if (strcmp(sequence, "LEFT") == 0) {
        move_cursor_left();
        return;
    }
    
    if (strcmp(sequence, "RIGHT") == 0) {
        move_cursor_right();
        return;
    }
    
    if (strcmp(sequence, "HOME") == 0) {
        move_cursor_home();
        return;
    }
    
    if (strcmp(sequence, "END") == 0) {
        move_cursor_end();
        return;
    }
    
    if (ssh_connected && channel) {
        write_channel(sequence, strlen(sequence));
    } else {
        ESP_LOGW(TAG, "Cannot send special key - not connected");
    }
    
    toggle_side_panel();
}

// Sends raw bytes (control keys) to the SSH session
void SSHTerminal::write_channel(const char* data, size_t len)
{
    xSemaphoreTake(ssh_mutex, portMAX_DELAY);
    if (channel) {
        libssh2_channel_write(channel, data, len);
    }
    xSemaphoreGive(ssh_mutex);
}

void SSHTerminal::gesture_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    
    if (dir == LV_DIR_LEFT) {
        ESP_LOGI(TAG, "Swipe left detected - showing special keys panel");
        if (terminal->side_panel && lv_obj_has_flag(terminal->side_panel, LV_OBJ_FLAG_HIDDEN)) {
            terminal->toggle_side_panel();
        }
    } else if (dir == LV_DIR_RIGHT) {
        ESP_LOGI(TAG, "Swipe right detected - hiding special keys panel");
        if (terminal->side_panel && !lv_obj_has_flag(terminal->side_panel, LV_OBJ_FLAG_HIDDEN)) {
            terminal->toggle_side_panel();
        }
    }
}

void SSHTerminal::special_key_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    lv_obj_t* btn = (lv_obj_t* ) lv_event_get_target(e);
    const char* key_seq = (const char*)lv_obj_get_user_data(btn);
    
    if (key_seq) {
        terminal->send_special_key(key_seq);
    }
}

void SSHTerminal::load_key_from_memory(const char* keyname, const char* key_data, size_t key_len)
{
    if (!keyname || !key_data || key_len == 0) {
        ESP_LOGE(TAG, "Invalid key parameters");
        return;
    }
    
    // Convert keyname to lowercase for case-insensitive lookup
    std::string keyname_str(keyname);
    std::transform(keyname_str.begin(), keyname_str.end(), keyname_str.begin(), ::tolower);
    
    // Keys stay in RAM for the whole session; cap them so a crowded SD card can't exhaust the heap
    static const size_t MAX_KEYS = 8;
    static const size_t MAX_TOTAL_BYTES = 48 * 1024;
    size_t total = key_len;
    for (const auto& kv : loaded_keys) {
        total += kv.second.size();
    }
    if (loaded_keys.size() >= MAX_KEYS || total > MAX_TOTAL_BYTES) {
        ESP_LOGW(TAG, "Key limit reached, skipping %s", keyname);
        return;
    }
    
    loaded_keys[keyname_str] = std::string(key_data, key_len);
    
    ESP_LOGI(TAG, "Loaded SSH key: %s (%d bytes)", keyname, key_len);
}

const char* SSHTerminal::get_loaded_key(const char* keyname, size_t* len)
{
    if (!keyname) {
        return NULL;
    }
    
    // Convert keyname to lowercase for case-insensitive lookup
    std::string keyname_str(keyname);
    std::transform(keyname_str.begin(), keyname_str.end(), keyname_str.begin(), ::tolower);
    
    auto it = loaded_keys.find(keyname_str);
    
    if (it != loaded_keys.end()) {
        if (len) {
            *len = it->second.length();
        }
        return it->second.c_str();
    }
    
    return NULL;
}

std::vector<std::string> SSHTerminal::get_loaded_key_names()
{
    std::vector<std::string> key_names;
    
    for (const auto& pair : loaded_keys) {
        key_names.push_back(pair.first);
    }
    
    return key_names;
}
