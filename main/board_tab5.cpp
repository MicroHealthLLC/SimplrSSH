/*
 * Board: M5Stack Tab5 (ESP32-P4) with the Tab5 Keyboard
 * Power and charging (two PI4IOE5V6408 IO expanders), the 5" MIPI-DSI display shown as
 * 1280x720 landscape with its touch panel (espressif/m5stack_tab5 BSP, which detects the
 * ILI9881C/GT911, ST7123 and ST7121 panel variants), the keyboard (tab5_keyboard.cpp), the
 * ESP32-C6 WiFi co-processor (esp_hosted over SDIO) and the UI metrics for the larger screen.
 */

#include "board.hpp"
#include "tab5_keyboard.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_io_expander.h"
#include "esp_hosted.h"

static const char *TAG = "board";

const char* const board::NAME = "Tab5";
const char* const board::HINT_TALK = "hold Ctrl";
const char* const board::HINT_SCROLL = "Sym+Up/Down or drag: scroll.";
const char* const board::HINT_HISTORY = "Up/Down: history.";
const char* const board::HINT_STOP = "Esc";
const char* const board::SCREEN_DESC = "a 5-inch tablet with a 1280x720 text screen";
const char* const board::HINT_WAKE = "any key or a touch";
const char* const board::HINT_POWER_ON = "Press the power button to turn it on again.";

// IO expander pins (M5Stack Tab5 pin map)
static const uint32_t IO_RF_EXTERNAL = IO_EXPANDER_PIN_NUM_0;  // 0x43 P0: antenna, low = internal
static const uint32_t IO_SPEAKER_EN = IO_EXPANDER_PIN_NUM_1;   // 0x43 P1: NS4150B amplifier
static const uint32_t IO_EXT5V_EN = IO_EXPANDER_PIN_NUM_2;     // 0x43 P2: 5 V to the side port (keyboard)
static const uint32_t IO1_WLAN_PWR_EN = IO_EXPANDER_PIN_NUM_0; // 0x44 P0: ESP32-C6 power
static const uint32_t IO1_USB5V_EN = IO_EXPANDER_PIN_NUM_3;    // 0x44 P3: 5 V out of the USB-A port
static const uint32_t IO1_PWROFF = IO_EXPANDER_PIN_NUM_4;      // 0x44 P4: power-off pulses (power_off())
static const uint32_t IO1_NQC_EN = IO_EXPANDER_PIN_NUM_5;      // 0x44 P5: low = quick charge
static const uint32_t IO1_CHARGE_EN = IO_EXPANDER_PIN_NUM_7;   // 0x44 P7: IP2326 charger on

static bool s_wifi_ready = false;

const board::Ui& board::ui()
{
    // The 5" 1280x720 screen has about twice the T-Deck's pixel density: larger fonts, and
    // the terminal still gets about 115 x 30 characters
    static const Ui ui = {
        &lv_font_montserrat_16, &lv_font_montserrat_18, &lv_font_montserrat_20,
        24, 30,            // Status line, input line
        240, 210, 44,      // Side panel and its buttons
        8,
    };
    return ui;
}

TermScreen* tab5_term_view();   // term_view.cpp

TermScreen* board::term_screen()
{
    return tab5_term_view();   // Full-screen terminal: TUI programs over SSH
}

void board::init_power()
{
    // Both expanders are reset by their driver on first use: set what SimplrSSH needs
    esp_io_expander_handle_t io = bsp_io_expander_init();
    esp_io_expander_handle_t io1 = bsp_io_expander1_init();
    if (!io || !io1) {
        ESP_LOGE(TAG, "IO expanders not reachable");
        return;
    }
    // The driver's reset leaves every pin high-impedance (pulled low): each output must be set
    // push-pull, or it never drives. Levels as M5Stack's own firmware sets them.
    // Internal antenna; speaker amplifier off until audio_tab5.cpp plays; side-port 5 V on for the keyboard
    const uint32_t outs = IO_RF_EXTERNAL | IO_SPEAKER_EN | IO_EXT5V_EN;
    esp_io_expander_set_dir(io, outs, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io, IO_RF_EXTERNAL | IO_SPEAKER_EN, 0);
    esp_io_expander_set_level(io, IO_EXT5V_EN, 1);
    esp_io_expander_set_output_mode(io, outs, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);

    // Power the WiFi co-processor now, so it has booted by the time WiFi is first used.
    // The battery only charges while the firmware enables the charger (standard charging, as
    // the factory firmware does). USB-A 5 V off, no power-off pulse.
    const uint32_t outs1 = IO1_WLAN_PWR_EN | IO1_USB5V_EN | IO1_PWROFF | IO1_NQC_EN | IO1_CHARGE_EN;
    esp_io_expander_set_dir(io1, outs1, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io1, IO1_USB5V_EN | IO1_PWROFF | IO1_CHARGE_EN, 0);
    esp_io_expander_set_level(io1, IO1_WLAN_PWR_EN | IO1_NQC_EN, 1);
    esp_io_expander_set_output_mode(io1, outs1, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_io_expander_set_level(io1, IO1_CHARGE_EN, 1);
}

lv_display_t* board::start_display()
{
    lvgl_port_cfg_t port_cfg = {};
    port_cfg.task_priority = 4;
    port_cfg.task_stack = 7168;
    port_cfg.task_affinity = -1;
    port_cfg.task_max_sleep_ms = 500;
    port_cfg.task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT;
    port_cfg.timer_period_ms = 5;

    bsp_display_cfg_t cfg = {};
    cfg.lvgl_port_cfg = port_cfg;
    // Quarter-screen draw buffers in PSRAM (2 x 460 KB), never internal RAM
    cfg.buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES / 4;
    cfg.double_buffer = true;
    cfg.flags.buff_dma = true;
    cfg.flags.buff_spiram = true;
    cfg.flags.sw_rotate = true;   // Rotated by the PPA (CONFIG_LVGL_PORT_ENABLE_PPA)
    lv_display_t* disp = bsp_display_start_with_config(&cfg);
    if (!disp) {
        ESP_LOGE(TAG, "Display start failed");
        return NULL;
    }

    // The panel is 720x1280 portrait; with the keyboard attached the Tab5 is used landscape
    if (bsp_display_lock(0)) {
        bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90);
        bsp_display_unlock();
    }
    bsp_display_brightness_set(100);
    return disp;
}

esp_err_t board::keyboard_init()
{
    return tab5_keyboard::init();
}

uint32_t board::read_key()
{
    return tab5_keyboard::read_key();
}

void board::start_input_tasks()
{
    // Nothing to poll: keypad_task reads the keyboard; touch is LVGL's
}

// esp_hosted links to the C6 over SDIO once the C6 has power (init_power); then the usual
// esp_wifi_* calls are forwarded to it (esp_wifi_remote)
esp_err_t board::wifi_prepare()
{
    if (s_wifi_ready) {
        return ESP_OK;
    }
    if (esp_hosted_init() != 0 || esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(TAG, "WiFi co-processor not responding");
        return ESP_FAIL;
    }
    s_wifi_ready = true;
    return ESP_OK;
}

void board::display_power(bool on)
{
    bsp_display_brightness_set(on ? 100 : 0);
}

// The display, WiFi co-processor link and PSRAM stay as they are (no CPU light sleep): the
// backlight is off and WiFi stopped, and the CPU idles between the caller's key and touch checks
bool board::sleep_wait(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
    return false;
}

// The power circuit switches off on a train of pulses on 0x44 P4, as M5Stack's M5Unified does.
// The power button is not wired to the ESP32-P4: a single press turns the Tab5 on, a double press
// off, in hardware. On USB power the Tab5 may stay on; then this returns.
void board::power_off()
{
    esp_io_expander_handle_t io1 = bsp_io_expander1_init();
    if (!io1) {
        return;
    }
    bsp_display_brightness_set(0);
    for (int i = 0; i < 10; i++) {
        esp_io_expander_set_level(io1, IO1_PWROFF, i & 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_io_expander_set_level(io1, IO1_PWROFF, 0);
}
