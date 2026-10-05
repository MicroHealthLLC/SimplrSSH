/*
 * Tab5 Keyboard
 * The M5Stack Tab5 Keyboard (A164): an STM32 on its own I2C bus (SDA G0, SCL G1, address
 * 0x6D) with an interrupt line (G50, low while key events are queued). Used in its raw
 * "normal" mode (key press/release by row and column); the key map, Sym layer, Aa (shift,
 * one-shot, caps lock), Ctrl codes and auto-repeat are done here. Polled from keypad_task,
 * no task of its own. The keyboard may be attached or removed at any time.
 */

#ifndef TAB5_KEYBOARD_HPP
#define TAB5_KEYBOARD_HPP

#include "esp_err.h"
#include <cstdint>

namespace tab5_keyboard
{
    esp_err_t init();       // Opens the keyboard's I2C bus; OK even when no keyboard is attached
    uint32_t read_key();    // Same values as board::read_key()
}

#endif
