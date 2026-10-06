/*
 * Tab5 Keymap
 * Turns the Tab5 Keyboard's raw key presses and releases (row 0-4, column 0-13) into
 * SimplrSSH keys (input_events.hpp). The keyboard firmware reports only positions; this does
 * the legends, the Sym layer, Aa (hold = shift, tap = one capital, double tap = caps lock),
 * Ctrl codes, auto-repeat and Ctrl push-to-talk. No ESP-IDF headers: tested on the host.
 *
 *   Up / Down             older / newer command, previous / next menu choice
 *   Left / Right          move the cursor; Sym+Left / Sym+Right: start / end of the line
 *   Sym+Up / Sym+Down     scroll the screen
 *   Del                   delete at the cursor
 *   Ctrl+letter           control code (Ctrl+C = 3 ...), also Ctrl+[ \ ] ^ _ /
 *   Alt+key               the key with Meta (KEY_ALT), for programs over SSH
 *   Sym+1 ... Sym+0, -, + F1 ... F10, F11, F12
 *   Ctrl (hold alone)     push-to-talk in ChatGPT; held 1 s elsewhere: delete history entry
 */

#ifndef TAB5_KEYMAP_HPP
#define TAB5_KEYMAP_HPP

#include "input_events.hpp"
#include <cstdint>

class Tab5Keymap
{
public:
    static const int ROWS = 5;
    static const int COLS = 14;
    static const uint32_t REPEAT_DELAY_MS = 450;
    static const uint32_t REPEAT_INTERVAL_MS = 50;
    static const uint32_t HOLD_MS = 400;        // Same as the T-Deck's trackball
    static const uint32_t LONG_HOLD_MS = 1000;
    static const uint32_t DOUBLE_TAP_MS = 500;

    void key_event(bool pressed, int row, int col, uint32_t now_ms);
    void tick(uint32_t now_ms);   // Auto-repeat and push-to-talk timing
    uint32_t pop();               // Next key, 0 if none
    void reset();                 // Keyboard removed: release everything

private:
    static const int QUEUE = 16;
    uint32_t queue[QUEUE] = {};
    int head = 0;
    int count = 0;

    bool sym = false;
    bool ctrl = false;
    bool alt = false;
    bool aa_down = false;
    bool aa_used = false;          // Another key was pressed while Aa was held
    bool one_shot = false;
    bool caps_lock = false;
    uint32_t aa_pressed_ms = 0;
    uint32_t aa_last_tap_ms = 0;

    int repeat_key = -1;           // row * COLS + col of the auto-repeating key
    uint32_t repeat_value = 0;
    uint32_t repeat_next_ms = 0;

    bool talk_down = false;        // Ctrl held, no other key pressed before HOLD_MS
    bool talk_used = false;        // Another key pressed while talking
    bool talk_holding = false;     // HOLD_START sent
    uint32_t talk_pressed_ms = 0;

    void push(uint32_t value);
    uint32_t translate(int row, int col, bool* repeats);
};

#endif
