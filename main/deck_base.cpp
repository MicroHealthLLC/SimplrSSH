/*
 * Deck Base Application
 * Main application entry point for PocketSSH. Initializes hardware peripherals including
 * GPIO, display, touch input, and manages FreeRTOS tasks for keyboard input and trackball navigation.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"

#include "esp_lcd_touch_gt911.h"
#include "driver/gpio.h"

#include "utilities.h"
#include "c3_keyboard.hpp"
#include "ssh_terminal.hpp"
#include "sd_card.hpp"
#include "settings_nvs.hpp"

#include "lvgl.h"

#if defined(BSP_LCD_DRAW_BUFF_SIZE)
#define DRAW_BUF_SIZE BSP_LCD_DRAW_BUFF_SIZE
#else
#define DRAW_BUF_SIZE (BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT)
#endif

static const char *TAG = "main";

// GT911 touch controller I2C clock (was CONFIG_BSP_I2C_CLK_SPEED_HZ, which the BSP no longer provides)
#define TOUCH_I2C_CLK_SPEED_HZ 100000

static i2c_master_bus_handle_t i2c_handle;
static lv_obj_t *ssh_screen;
static SSHTerminal *ssh_terminal = NULL;

// Trackball events, handled by keypad_task so only one task runs the terminal
// (menus, WiFi scans, crypto and SSH connects need a large stack)
enum InputEvent : uint8_t { INPUT_UP, INPUT_DOWN, INPUT_ENTER, INPUT_HOLD_START, INPUT_HOLD_END, INPUT_HOLD_END_LONG };
static QueueHandle_t input_events = NULL;

void keypad_task(void *param)
{
    C3Keyboard keyboard(i2c_handle);
    // Keep running without the keyboard: this task also runs WiFi auto-connect and trackball input
    bool keyboard_ok = keyboard.init() == ESP_OK;
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

        // Non-blocking display lock: keep rendering responsive, drop input if the display is busy
        uint32_t key = keyboard_ok ? keyboard.get_key() : 0;
        if (key && bsp_display_lock(0)) {
            ssh_terminal->handle_key_input((char)key);
            bsp_display_unlock();
        }

        uint8_t event;
        while (xQueueReceive(input_events, &event, 0) == pdTRUE) {
            if (!bsp_display_lock(0)) {
                continue;
            }
            switch (event) {
                case INPUT_UP:     ssh_terminal->navigate_history(1); break;   // Older command / previous choice
                case INPUT_DOWN:   ssh_terminal->navigate_history(-1); break;  // Newer command / next choice
                case INPUT_ENTER:  ssh_terminal->handle_key_input('\n'); break;
                case INPUT_HOLD_START:    ssh_terminal->on_trackball_hold(true, false); break;   // Push-to-talk in chat
                case INPUT_HOLD_END:      ssh_terminal->on_trackball_hold(false, false); break;
                case INPUT_HOLD_END_LONG: ssh_terminal->on_trackball_hold(false, true); break;   // Else: delete history entry
            }
            bsp_display_unlock();
        }

        // WiFi reconnect, return to the menu after an SSH session ends
        if (bsp_display_lock(0)) {
            ssh_terminal->background_tick();
            bsp_display_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Polls the trackball and queues events; the work happens in keypad_task.
// Press: Enter. Hold (> 0.4 s): HOLD_START, then HOLD_END(_LONG) on release.
void trackball_task(void *param)
{
    bool last_up = true;
    bool last_down = true;
    bool last_press = true;
    bool holding = false;
    TickType_t press_start = 0;
    const TickType_t HOLD = pdMS_TO_TICKS(400);
    const TickType_t LONG_HOLD = pdMS_TO_TICKS(1000);

    while (1) {
        bool up = gpio_get_level(BOARD_TBOX_G01);
        bool down = gpio_get_level(BOARD_TBOX_G03);
        bool press = gpio_get_level(BOARD_BOOT_PIN);
        TickType_t now = xTaskGetTickCount();
        uint8_t event;

        if (!up && last_up) {
            event = INPUT_UP;
            xQueueSend(input_events, &event, 0);
        }
        if (!down && last_down) {
            event = INPUT_DOWN;
            xQueueSend(input_events, &event, 0);
        }
        if (!press && last_press) {
            press_start = now;
        } else if (!press && !holding && now - press_start >= HOLD) {
            holding = true;
            event = INPUT_HOLD_START;
            xQueueSend(input_events, &event, 0);
        } else if (press && !last_press) {
            event = !holding ? INPUT_ENTER : (now - press_start >= LONG_HOLD ? INPUT_HOLD_END_LONG : INPUT_HOLD_END);
            holding = false;
            xQueueSend(input_events, &event, 0);
        }

        last_up = up;
        last_down = down;
        last_press = press;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

esp_err_t _bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch)
{
    /* Initilize I2C */
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Initialize I2C Fail");
        return ret;
    }

    /* Initialize touch */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_V_RES,
        .y_max = BSP_LCD_H_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_16,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .process_coordinates = NULL,
        .interrupt_callback = NULL,
        .user_data = NULL,
        .driver_data = NULL,
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    ESP_LOGI("Touch", "Initialize LCD Touch: GT911");
    // Same settings as ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG(), assigned field by field:
    // that macro's designated initializers are out of declaration order, which C++ rejects.
    esp_lcd_panel_io_i2c_config_t tp_io_config = {};
    tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    tp_io_config.control_phase_bytes = 1;
    tp_io_config.dc_bit_offset = 0;
    tp_io_config.lcd_cmd_bits = 16;
    tp_io_config.flags.disable_control_phase = 1;
    tp_io_config.scl_speed_hz = TOUCH_I2C_CLK_SPEED_HZ;

    i2c_handle = bsp_i2c_get_handle();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), "TOuch", "");
    return esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, ret_touch);
}

void device_init(void)
{
    // Level is set before the pin becomes an output, so the peripheral power and the
    // chip selects never glitch low (a low pulse on the SD card's CS or supply can corrupt it)
    gpio_config_t out = {};
    out.mode = GPIO_MODE_OUTPUT;
    out.pull_up_en = GPIO_PULLUP_ENABLE;
    for (gpio_num_t pin : {BOARD_POWERON, BOARD_SDCARD_CS, RADIO_CS_PIN, BOARD_TFT_CS}) {
        gpio_set_level(pin, 1);
        out.pin_bit_mask = 1ULL << pin;
        gpio_config(&out);
    }

    // Let the peripheral supply settle before the SD card is used. A card that is talked to
    // while its supply is still coming up (or dipping from the display/radio inrush) can
    // damage its own internal tables and become unreadable. A card that misses the init that
    // puts it in SPI mode listens to the display's traffic whatever its CS pin says.
    vTaskDelay(pdMS_TO_TICKS(250));

    /* Configure MISO with pull-up for SD card (must be done before SPI bus init) */
    gpio_reset_pin(BOARD_SPI_MISO);
    gpio_set_direction(BOARD_SPI_MISO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_SPI_MISO, GPIO_PULLUP_ONLY);

    // Initialize trackball GPIOs for command history navigation
    gpio_reset_pin(BOARD_TBOX_G01);
    gpio_set_direction(BOARD_TBOX_G01, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_TBOX_G01, GPIO_PULLUP_ONLY);
    
    gpio_reset_pin(BOARD_TBOX_G03);
    gpio_set_direction(BOARD_TBOX_G03, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_TBOX_G03, GPIO_PULLUP_ONLY);
    
    // Initialize trackball press button (BOOT button on GPIO 0)
    gpio_reset_pin(BOARD_BOOT_PIN);
    gpio_set_direction(BOARD_BOOT_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_BOOT_PIN, GPIO_PULLUP_ONLY);
}

extern "C" void app_main(void)
{
    // NVS holds all saved settings; it is only reformatted if unreadable (reported on screen)
    settings_nvs::InitResult settings_state = settings_nvs::init();

    /* Initialize device GPIOs */
    device_init();

    /* Load SSH keys from SD card BEFORE LVGL initialization. This (read-only) mount also
       switches the card to SPI mode, so it ignores the display while its CS is high. */
    // Note: Create a temporary terminal instance just for loading keys
    SSHTerminal* temp_terminal = new SSHTerminal();
    if (sdcard::mount_at_boot() == ESP_OK) {
        sdcard::load_ssh_keys(temp_terminal);
        sdcard::unmount();
    }

    /* Initialize display and LVGL */
    lv_display_t *disp = bsp_display_start();
    sdcard::share_display_bus();  // SD card is reachable at runtime on the display's SPI bus

    /* Set display brightness to 100% */
    bsp_display_backlight_on();

    /* Initialize touch */
    esp_lcd_touch_handle_t touch_handle = NULL;
    const bsp_touch_config_t bsp_touch_cfg = {};
    _bsp_touch_new(&bsp_touch_cfg, &touch_handle);

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = touch_handle,
        .scale = {.x = 0, .y = 0},
    };

    lvgl_port_add_touch(&touch_cfg);

    bsp_display_lock(0);

    // Use the terminal instance that already has loaded keys
    ssh_terminal = temp_terminal;
    ssh_screen = ssh_terminal->create_terminal_screen();
    lv_screen_load(ssh_screen);
    
    // Display version and initial instructions
#ifdef POCKETSSH_VERSION
    ssh_terminal->append_text("PocketSSH v" POCKETSSH_VERSION "\n");
#else
    ssh_terminal->append_text("PocketSSH Terminal Ready\n");
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
                                  "launcher's small storage. Install PocketSSH-*-launcher.bin instead.\n\n");
    } else if (settings_state == settings_nvs::InitResult::UNAVAILABLE) {
        ssh_terminal->append_text("WARNING: Settings storage is unavailable; nothing will be saved.\n\n");
    }

    // Update status bar to show initial battery voltage
    ssh_terminal->update_status_bar();

    bsp_display_unlock();

    input_events = xQueueCreate(8, sizeof(uint8_t));

    // keypad_task runs the terminal: menus, WiFi scans, encryption and SSH connects
    xTaskCreate(keypad_task, "keypad_task", 8192, NULL, 5, NULL);
    xTaskCreate(trackball_task, "trackball_task", 2048, NULL, 5, NULL);
}

