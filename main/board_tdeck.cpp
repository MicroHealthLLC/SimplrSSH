/*
 * Board: LilyGO T-Deck / T-Deck Plus (ESP32-S3)
 * Peripheral power and pin levels, the ST7789 display and GT911 touch (esp_bsp_generic),
 * the ESP32-C3 keyboard and the trackball, and the UI metrics for its 320x240 screen.
 */

#include "board.hpp"

#include <initializer_list>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lcd_touch_gt911.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

#include "utilities.h"
#include "c3_keyboard.hpp"

static const char *TAG = "board";

// GT911 touch controller I2C clock (was CONFIG_BSP_I2C_CLK_SPEED_HZ, which the BSP no longer provides)
#define TOUCH_I2C_CLK_SPEED_HZ 100000

const char* const board::NAME = "T-Deck";
const char* const board::HINT_TALK = "hold the trackball";
const char* const board::HINT_SCROLL = "Trackball up/down or drag: scroll.";
const char* const board::HINT_HISTORY = "Left/right: history.";
const char* const board::HINT_STOP = "Esc (swipe left)";
const char* const board::SCREEN_DESC = "a tiny handheld with a 320x240 text screen";
const char* const board::HINT_WAKE = "any key, the trackball or a touch";
const char* const board::HINT_POWER_ON = "Press the trackball to turn it on again (the power switch disconnects the battery).";

static C3Keyboard* s_keyboard = NULL;

const board::Ui& board::ui()
{
    static const Ui ui = {
        &lv_font_montserrat_10, &lv_font_montserrat_10, &lv_font_montserrat_12,
        13, 16,            // Status line, input line
        100, 85, 30,       // Side panel and its buttons
        4,
    };
    return ui;
}

TermScreen* board::term_screen()
{
    return NULL;   // SSH output scrolls as text on the small screen
}

void board::init_power()
{
    // After 'shutdown' the peripheral power pin is held low through deep sleep: release it
    gpio_hold_dis(BOARD_POWERON);

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

    // Trackball directions: up/down scroll, left/right command history and menu choices
    for (gpio_num_t pin : {BOARD_TBOX_G01, BOARD_TBOX_G02, BOARD_TBOX_G03, BOARD_TBOX_G04}) {
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    }

    // Initialize trackball press button (BOOT button on GPIO 0)
    gpio_reset_pin(BOARD_BOOT_PIN);
    gpio_set_direction(BOARD_BOOT_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_BOOT_PIN, GPIO_PULLUP_ONLY);
}

static esp_err_t touch_new(esp_lcd_touch_handle_t *ret_touch)
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

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bsp_i2c_get_handle(), &tp_io_config, &tp_io_handle), "TOuch", "");
    return esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, ret_touch);
}

lv_display_t* board::start_display()
{
    lv_display_t *disp = bsp_display_start();

    /* Set display brightness to 100% */
    bsp_display_backlight_on();

    /* Initialize touch */
    esp_lcd_touch_handle_t touch_handle = NULL;
    touch_new(&touch_handle);

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = touch_handle,
        .scale = {.x = 0, .y = 0},
    };

    lvgl_port_add_touch(&touch_cfg);
    return disp;
}

// Keyboard on the shared I2C bus (opened by start_display())
esp_err_t board::keyboard_init()
{
    s_keyboard = new C3Keyboard(bsp_i2c_get_handle());
    return s_keyboard->init();
}

uint32_t board::read_key()
{
    return s_keyboard ? s_keyboard->get_key() : 0;
}

// Polls the trackball and queues events; the work happens in keypad_task.
// Up/down: scroll. Left/right: history / menu choices.
// Press: Enter. Hold (> 0.4 s): HOLD_START, then HOLD_END(_LONG) on release.
static void trackball_task(void *param)
{
    bool last_up = true;
    bool last_down = true;
    bool last_left = true;
    bool last_right = true;
    bool last_press = true;
    bool holding = false;
    TickType_t press_start = 0;
    const TickType_t HOLD = pdMS_TO_TICKS(400);
    const TickType_t LONG_HOLD = pdMS_TO_TICKS(1000);

    while (1) {
        bool up = gpio_get_level(BOARD_TBOX_G01);
        bool down = gpio_get_level(BOARD_TBOX_G03);
        bool left = gpio_get_level(BOARD_TBOX_G04);
        bool right = gpio_get_level(BOARD_TBOX_G02);
        bool press = gpio_get_level(BOARD_BOOT_PIN);
        TickType_t now = xTaskGetTickCount();

        if (!up && last_up) {
            board::post_input(board::INPUT_UP);
        }
        if (!down && last_down) {
            board::post_input(board::INPUT_DOWN);
        }
        if (!left && last_left) {
            board::post_input(board::INPUT_LEFT);
        }
        if (!right && last_right) {
            board::post_input(board::INPUT_RIGHT);
        }
        if (!press && last_press) {
            press_start = now;
        } else if (!press && !holding && now - press_start >= HOLD) {
            holding = true;
            board::post_input(board::INPUT_HOLD_START);
        } else if (press && !last_press) {
            board::post_input(!holding ? board::INPUT_ENTER
                              : (now - press_start >= LONG_HOLD ? board::INPUT_HOLD_END_LONG : board::INPUT_HOLD_END));
            holding = false;
        }

        last_up = up;
        last_down = down;
        last_left = left;
        last_right = right;
        last_press = press;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void board::start_input_tasks()
{
    xTaskCreate(trackball_task, "trackball_task", 2048, NULL, 5, NULL);
}

esp_err_t board::wifi_prepare()
{
    return ESP_OK;   // WiFi is built into the ESP32-S3
}

void board::display_power(bool on)
{
    if (on) {
        bsp_display_backlight_on();
    } else {
        bsp_display_backlight_off();
    }
}

// Light sleep (WiFi is already off). Wakes on the trackball (press or roll: each line wakes on
// the level opposite to its current one), the touch interrupt, or after ms. The keyboard has no
// usable wake line, so the caller polls it between sleeps. The display lock is held while
// asleep, so the CPU never stops in the middle of a display transfer or touch read.
bool board::sleep_wait(uint32_t ms)
{
    if (!bsp_display_lock(ms)) {
        return false;
    }
    const gpio_num_t pins[] = {BOARD_BOOT_PIN, BOARD_TBOX_G01, BOARD_TBOX_G02, BOARD_TBOX_G03,
                               BOARD_TBOX_G04, BOARD_TOUCH_INT};
    for (gpio_num_t pin : pins) {
        int level = gpio_get_level(pin);
        if (pin == BOARD_TOUCH_INT && level == 0) {
            continue;   // Interrupt already active: the touch is read by LVGL between sleeps
        }
        gpio_wakeup_enable(pin, level ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    }
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
    esp_light_sleep_start();
    bool woken = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    for (gpio_num_t pin : pins) {
        gpio_wakeup_disable(pin);
    }
    bsp_display_unlock();
    return woken;
}

// The battery is switched only by the power switch: the lowest state software can reach is deep
// sleep with the peripherals (display, keyboard, touch, SD card, radio) unpowered. Pressing the
// trackball wakes the ESP32-S3, which then boots as after power-on.
void board::power_off()
{
    bsp_display_backlight_off();
    // A trackball still held down would wake it at once
    for (int i = 0; i < 100 && gpio_get_level(BOARD_BOOT_PIN) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    gpio_set_level(BOARD_POWERON, 0);
    gpio_hold_en(BOARD_POWERON);
    gpio_deep_sleep_hold_en();

    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   // Keeps the pull-up below
    rtc_gpio_pullup_en(BOARD_BOOT_PIN);
    rtc_gpio_pulldown_dis(BOARD_BOOT_PIN);
    esp_sleep_enable_ext1_wakeup_io(1ULL << BOARD_BOOT_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}
