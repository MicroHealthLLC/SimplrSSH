/*
 * Deck Base Application
 * Main application entry point for SimplrSSH, shared by every board (board.hpp): starts the
 * hardware through the board layer, shows the terminal and runs keypad_task, the one task
 * that handles keyboard and trackball/touch input and all terminal work.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"

#include "board.hpp"
#include "ssh_terminal.hpp"
#include "sd_card.hpp"
#include "settings_nvs.hpp"

#include "lvgl.h"

static lv_obj_t *ssh_screen;
static SSHTerminal *ssh_terminal = NULL;

// Input events (board::InputEvent), handled by keypad_task so only one task runs the terminal
// (menus, WiFi scans, crypto and SSH connects need a large stack)
static QueueHandle_t input_events = NULL;

void board::post_input(InputEvent event)
{
    uint8_t value = event;
    if (input_events) {
        xQueueSend(input_events, &value, 0);
    }
}

static void handle_input_event(uint8_t event)
{
    switch (event) {
        case board::INPUT_UP:     ssh_terminal->scroll_screen(1); break;      // Scroll back (older text)
        case board::INPUT_DOWN:   ssh_terminal->scroll_screen(-1); break;     // Scroll toward the newest text
        case board::INPUT_LEFT:   ssh_terminal->navigate_history(1); break;   // Older command / previous choice
        case board::INPUT_RIGHT:  ssh_terminal->navigate_history(-1); break;  // Newer command / next choice
        case board::INPUT_ENTER:  ssh_terminal->handle_key_input('\n'); break;
        case board::INPUT_HOLD_START:    ssh_terminal->on_trackball_hold(true, false); break;   // Push-to-talk in chat
        case board::INPUT_HOLD_END:      ssh_terminal->on_trackball_hold(false, false); break;
        case board::INPUT_HOLD_END_LONG: ssh_terminal->on_trackball_hold(false, true); break;   // Else: delete history entry
        case board::INPUT_CURSOR_LEFT:   ssh_terminal->move_cursor_left(); break;
        case board::INPUT_CURSOR_RIGHT:  ssh_terminal->move_cursor_right(); break;
        case board::INPUT_CURSOR_HOME:   ssh_terminal->move_cursor_home(); break;
        case board::INPUT_CURSOR_END:    ssh_terminal->move_cursor_end(); break;
        case board::INPUT_DELETE:        ssh_terminal->delete_at_cursor(); break;
        case board::INPUT_GO_HOME:       ssh_terminal->leave_to_home(); break;   // Ctrl+Del (Tab5)
        default:
            if (event >= board::INPUT_F1 && event <= board::INPUT_F12) {
                ssh_terminal->function_key(event - board::INPUT_F1);
            }
            break;
    }
}

// Sleep (Power menu, 'sleep'): screen and WiFi off until a key, the trackball or a touch.
// Runs in keypad_task without the display lock, so LVGL keeps reading the touch panel.
static const uint32_t SLEEP_POLL_MS = 200;      // Keyboard checks while asleep
static volatile bool s_touch_woke = false;

static void wake_layer_cb(lv_event_t*)
{
    s_touch_woke = true;
}

static bool touch_pressed()
{
    for (lv_indev_t* indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            return true;
        }
    }
    return false;
}

// Reads and drops waiting keys and input events; true if there were any
static bool drop_input()
{
    bool any = false;
    for (int i = 0; i < 64 && board::read_key(); i++) {
        any = true;
    }
    if (uxQueueMessagesWaiting(input_events)) {
        xQueueReset(input_events);
        any = true;
    }
    return any;
}

static void sleep_until_woken()
{
    // A full-screen layer takes every touch while asleep, so the touch that wakes the device
    // doesn't also press whatever is under it on the dark screen
    bsp_display_lock(0);
    lv_obj_t* layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(layer, wake_layer_cb, LV_EVENT_PRESSED, NULL);
    bsp_display_unlock();

    vTaskDelay(pdMS_TO_TICKS(1000));   // Time to read the message
    s_touch_woke = false;
    drop_input();                      // The key that chose Sleep doesn't wake it
    board::display_power(false);
    while (!s_touch_woke) {
        if (board::sleep_wait(SLEEP_POLL_MS) || drop_input()) {
            break;
        }
    }
    board::display_power(true);

    // What woke it does nothing else: keys, trackball and touch are dropped until released
    // (at least 0.5 s, at most 3 s)
    int64_t start = esp_timer_get_time() / 1000;
    int64_t last_input = start;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int64_t now = esp_timer_get_time() / 1000;
        bool busy = drop_input();
        if (bsp_display_lock(0)) {
            busy = touch_pressed() || busy;
            bsp_display_unlock();
        }
        if (busy) {
            last_input = now;
        }
        if ((now - start >= 500 && now - last_input >= 300) || now - start >= 3000) {
            break;
        }
    }
    bsp_display_lock(0);
    lv_obj_delete(layer);
    bsp_display_unlock();
}

// Sleep or power off chosen in the Power menu
static void handle_power_request()
{
    SSHTerminal::PowerRequest request = SSHTerminal::PowerRequest::None;
    if (bsp_display_lock(0)) {
        request = ssh_terminal->take_power_request();
        if (request != SSHTerminal::PowerRequest::None) {
            ssh_terminal->power_prepare(request);
        }
        bsp_display_unlock();
    }
    if (request == SSHTerminal::PowerRequest::None) {
        return;
    }
    if (request == SSHTerminal::PowerRequest::Off) {
        vTaskDelay(pdMS_TO_TICKS(1000));   // Time to read the message
        board::power_off();                // Returns only if the device stayed on
        board::display_power(true);
        if (bsp_display_lock(0)) {
            ssh_terminal->power_off_failed();
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    sleep_until_woken();
    if (bsp_display_lock(0)) {
        ssh_terminal->power_resume();
        bsp_display_unlock();
    }
}

static void keypad_task(void *param)
{
    // Keep running without the keyboard: this task also runs WiFi auto-connect and trackball input
    bool keyboard_ok = board::keyboard_init() == ESP_OK;
    if (!keyboard_ok) {
        ESP_LOGE("KEYPAD", "Failed to initialize keypad!");
    }

    bool startup_pending = true;

    while (1)
    {
        // First pass: startup work (WiFi auto-connect)
        if (startup_pending && bsp_display_lock(0)) {
            startup_pending = false;
            ssh_terminal->run_startup_tasks();
            bsp_display_unlock();
        }

        // Non-blocking display lock: keep rendering responsive, drop input if the display is busy.
        // A few keys per pass, so fast typing on a keyboard with a key queue keeps up.
        for (int i = 0; keyboard_ok && i < 8; i++) {
            uint32_t key = board::read_key();
            if (!key) {
                break;
            }
            if (!bsp_display_lock(0)) {
                continue;
            }
            if (key & board::KEY_EVENT) {
                handle_input_event((uint8_t)key);
            } else if (key & board::KEY_ALT) {
                ssh_terminal->alt_key((char)key);
            } else {
                ssh_terminal->handle_key_input((char)key);
            }
            bsp_display_unlock();
        }

        uint8_t event;
        while (xQueueReceive(input_events, &event, 0) == pdTRUE) {
            if (!bsp_display_lock(0)) {
                continue;
            }
            handle_input_event(event);
            bsp_display_unlock();
        }

        handle_power_request();

        // WiFi reconnect, return to the menu after an SSH session ends
        if (bsp_display_lock(0)) {
            ssh_terminal->background_tick();
            bsp_display_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

extern "C" void app_main(void)
{
    // NVS holds all saved settings; it is only reformatted if unreadable (reported on screen)
    settings_nvs::InitResult settings_state = settings_nvs::init();

    /* Peripheral power and pin levels */
    board::init_power();

    /* Load SSH keys from SD card BEFORE LVGL initialization. This (read-only) mount also
       switches the card to SPI mode, so it ignores the display while its CS is high. */
    // Note: Create a temporary terminal instance just for loading keys
    SSHTerminal* temp_terminal = new SSHTerminal();
    if (sdcard::mount_at_boot() == ESP_OK) {
        sdcard::load_ssh_keys(temp_terminal);
        sdcard::unmount();
    }

    /* Initialize display, touch and LVGL */
    board::start_display();
    sdcard::share_display_bus();  // SD card is reachable at runtime (T-Deck: on the display's SPI bus)

    bsp_display_lock(0);

    // Use the terminal instance that already has loaded keys
    ssh_terminal = temp_terminal;
    ssh_screen = ssh_terminal->create_terminal_screen();
    lv_screen_load(ssh_screen);
    
    // Display version and initial instructions
#ifdef SIMPLRSSH_VERSION
    ssh_terminal->append_text("SimplrSSH v" SIMPLRSSH_VERSION "\n");
#else
    ssh_terminal->append_text("SimplrSSH Terminal Ready\n");
#endif

    ssh_terminal->print_saved_summary();

    // Display loaded SSH keys
    auto loaded_keys = ssh_terminal->get_loaded_key_names();
    if (!loaded_keys.empty()) {
        ssh_terminal->append_text("\nLoaded SSH keys:\n");
        for (const auto& keyname : loaded_keys) {
            ssh_terminal->append_text("  - ");
            ssh_terminal->append_text(keyname.c_str());
            ssh_terminal->append_text("\n");
        }
        ssh_terminal->append_text("\n");
    } else {
        ssh_terminal->append_text("\nNo SSH keys found on SD card.\n");
        ssh_terminal->append_text("Place .pem files in /sdcard/ssh_keys/\n\n");
    }
    
    if (settings_state == settings_nvs::InitResult::REFORMATTED) {
        ssh_terminal->append_text("WARNING: Settings storage was unreadable and has been reset.\n"
                                  "Restore a backup with 'storage' if you have one.\n\n");
    } else if (settings_state == settings_nvs::InitResult::SHARED) {
        ssh_terminal->append_text("WARNING: Installed without its settings partition; saving in the\n"
                                  "launcher's small storage. Install SimplrSSH-*-launcher.bin instead.\n\n");
    } else if (settings_state == settings_nvs::InitResult::UNAVAILABLE) {
        ssh_terminal->append_text("WARNING: Settings storage is unavailable; nothing will be saved.\n\n");
    }

    // Update status bar to show initial battery voltage
    ssh_terminal->update_status_bar();

    bsp_display_unlock();

    input_events = xQueueCreate(16, sizeof(uint8_t));  // A fast roll queues several steps

    // keypad_task runs the terminal: menus, WiFi scans, encryption and SSH connects
    xTaskCreate(keypad_task, "keypad_task", 8192, NULL, 5, NULL);
    board::start_input_tasks();
}

