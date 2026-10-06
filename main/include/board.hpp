/*
 * Board
 * The parts of SimplrSSH that differ between the supported devices. The build target picks
 * the board (main/CMakeLists.txt): esp32s3 builds the LilyGO T-Deck (board_tdeck.cpp),
 * esp32p4 builds the M5Stack Tab5 with its keyboard (board_tab5.cpp). Everything else -
 * terminal, menus, WiFi, SSH, vault, settings, ChatGPT - is shared.
 */

#ifndef BOARD_HPP
#define BOARD_HPP

#include "esp_err.h"
#include "lvgl.h"
#include "input_events.hpp"
#include <cstdint>

class TermScreen;

namespace board
{
    extern const char* const NAME;          // "T-Deck" or "Tab5", for messages

    // Queues an input event (input_events.hpp) for keypad_task; safe from any task
    // (dropped if the queue is full)
    void post_input(InputEvent event);

    // Keyboard: values as described in input_events.hpp
    esp_err_t keyboard_init();
    uint32_t read_key();

    void init_power();                // First: peripheral power and pin levels
    lv_display_t* start_display();    // Display, touch and backlight; LVGL running
    void start_input_tasks();         // Board input pollers (the T-Deck's trackball)
    esp_err_t wifi_prepare();         // Before the WiFi driver starts (Tab5: link to its WiFi co-processor)

    // Power (home menu 'Power', 'sleep' and 'shutdown'), from keypad_task without the display lock
    void display_power(bool on);      // Backlight off/on; touch keeps working
    // Waits up to ms while asleep. T-Deck: CPU light sleep, ended early by the trackball or a
    // touch (returns true). Tab5: an idle wait (returns false); keys and touch are polled by the caller.
    bool sleep_wait(uint32_t ms);
    // Turns the device off and does not return (T-Deck: peripheral power off and deep sleep until
    // the trackball is pressed, which starts it again). Returns only if the device stayed on
    // (Tab5 on USB power).
    void power_off();

    // Text sizes and spacing for the screen's size and pixel density
    struct Ui {
        const lv_font_t* small;       // Status line, side panel buttons
        const lv_font_t* text;        // Terminal text (its glyph size sets the SSH pty size)
        const lv_font_t* input;       // Input line, chat bubbles, titles
        int32_t status_h;             // Status line height
        int32_t input_h;              // Input line height
        int32_t panel_w;              // Special-keys side panel
        int32_t button_w;
        int32_t button_h;
        int32_t gap;                  // Small padding (chat bubbles, panel)
    };
    const Ui& ui();

    // Full-screen terminal for SSH sessions (term_screen.hpp), or NULL: SSH output scrolls as
    // text and lines are sent with Enter (T-Deck)
    TermScreen* term_screen();

    // Words for the help texts: how to talk to ChatGPT, scroll, and recall history
    extern const char* const HINT_TALK;     // e.g. "hold the trackball"
    extern const char* const HINT_SCROLL;
    extern const char* const HINT_HISTORY;
    extern const char* const HINT_STOP;     // How to press Esc (stops a ChatGPT reply)
    extern const char* const SCREEN_DESC;   // For the ChatGPT system prompt
    extern const char* const HINT_WAKE;     // What wakes the device from sleep
    extern const char* const HINT_POWER_ON; // How to turn it on again after 'shutdown'
}

#endif
