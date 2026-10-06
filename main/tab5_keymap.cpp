/*
 * Tab5 Keymap Implementation
 * Legends from the keyboard's own firmware (M5Tab5-Keyboard-Internal-FW, user_keyboard_handle.c):
 * BASE is the printed key, SYM the key's Sym-layer legend (0 = none).
 */

#include "tab5_keymap.hpp"

using namespace board;

static const char BASE[Tab5Keymap::ROWS][Tab5Keymap::COLS] = {
    {0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '+', 0},
    {'`', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '[', ']', '\\'},
    {0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', ';', '\'', 0},
    {0, 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0, '_', 0},
    {0, 0, 'z', 'x', 'c', 'v', 'b', 'n', 'm', '.', 0, 0, 0, ' '},
};
static const char SYM[Tab5Keymap::ROWS][Tab5Keymap::COLS] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {'~', '?', 0, 0, 0, 0, 0, 0, '/', '<', '>', '{', '}', '|'},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '"', 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '=', 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, ',', 0, 0, 0, 0},
};

// Key positions (row, column)
static bool at(int row, int col, int r, int c)
{
    return row == r && col == c;
}
static bool is_sym(int r, int c) { return at(r, c, 3, 0); }
static bool is_aa(int r, int c) { return at(r, c, 3, 1); }
static bool is_ctrl(int r, int c) { return at(r, c, 4, 0); }
static bool is_alt(int r, int c) { return at(r, c, 4, 1); }

// a - b >= 0 for wrapping millisecond counters
static bool reached(uint32_t now, uint32_t when)
{
    return (int32_t)(now - when) >= 0;
}

void Tab5Keymap::push(uint32_t value)
{
    if (value && count < QUEUE) {
        queue[(head + count) % QUEUE] = value;
        count++;
    }
}

uint32_t Tab5Keymap::pop()
{
    if (count == 0) {
        return 0;
    }
    uint32_t value = queue[head];
    head = (head + 1) % QUEUE;
    count--;
    return value;
}

uint32_t Tab5Keymap::translate(int row, int col, bool* repeats)
{
    *repeats = true;
    if (at(row, col, 0, 0)) { *repeats = false; return 27; }                    // Esc
    if (at(row, col, 2, 0)) { *repeats = false; return '\t'; }                  // Tab
    if (at(row, col, 3, 13)) { *repeats = false; return '\n'; }                 // Enter
    if (at(row, col, 2, 13)) { return 8; }                                      // Backspace
    if (at(row, col, 0, 13)) { return KEY_EVENT | INPUT_DELETE; }               // Del
    if (at(row, col, 3, 11)) { return KEY_EVENT | (sym ? INPUT_UP : INPUT_LEFT); }      // Up
    if (at(row, col, 4, 11)) { return KEY_EVENT | (sym ? INPUT_DOWN : INPUT_RIGHT); }   // Down
    if (at(row, col, 4, 10)) { return KEY_EVENT | (sym ? INPUT_CURSOR_HOME : INPUT_CURSOR_LEFT); }
    if (at(row, col, 4, 12)) { return KEY_EVENT | (sym ? INPUT_CURSOR_END : INPUT_CURSOR_RIGHT); }
    if (sym && row == 0 && col >= 1 && col <= 12) { *repeats = false; return KEY_EVENT | (INPUT_F1 + col - 1); }

    char c = (sym && SYM[row][col]) ? SYM[row][col] : BASE[row][col];
    if (c >= 'a' && c <= 'z') {
        if (ctrl) {
            *repeats = false;
            return (uint32_t)(c - 'a' + 1);   // Ctrl+A = 1 ... Ctrl+Z = 26
        }
        if (aa_down || one_shot || caps_lock) {
            c = (char)(c - 'a' + 'A');
        }
    } else if (ctrl) {
        *repeats = false;
        switch (c) {
            case '[': return 27;
            case '\\': return 28;
            case ']': return 29;
            case '^': return 30;
            case '_':
            case '/': return 31;
            default: break;
        }
    }
    return (uint8_t)c;
}

void Tab5Keymap::key_event(bool pressed, int row, int col, uint32_t now_ms)
{
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) {
        return;
    }
    if (is_ctrl(row, col)) {
        ctrl = pressed;
        if (pressed) {
            // Ctrl on its own is push-to-talk once held for HOLD_MS (tick)
            talk_down = true;
            talk_holding = false;
            talk_used = false;
            talk_pressed_ms = now_ms;
        } else if (talk_down) {
            talk_down = false;
            if (talk_holding) {
                talk_holding = false;
                // A long hold deletes a history entry outside ChatGPT: only when Ctrl was alone
                bool long_hold = !talk_used && now_ms - talk_pressed_ms >= LONG_HOLD_MS;
                push(KEY_EVENT | (long_hold ? INPUT_HOLD_END_LONG : INPUT_HOLD_END));
            }
        }
        return;
    }
    if (pressed && talk_down) {
        // Another key with Ctrl: a control code (Ctrl+C ...), not talking
        if (talk_holding) {
            talk_used = true;
        } else {
            talk_down = false;
        }
    }
    if (is_sym(row, col)) {
        sym = pressed;
        return;
    }
    if (is_alt(row, col)) {
        alt = pressed;
        return;
    }
    if (is_aa(row, col)) {
        if (pressed) {
            aa_down = true;
            aa_used = false;
            aa_pressed_ms = now_ms;
        } else {
            aa_down = false;
            if (!aa_used && now_ms - aa_pressed_ms < DOUBLE_TAP_MS) {
                // Tap: one capital; second tap soon after: caps lock; tap again: off
                if (caps_lock) {
                    caps_lock = false;
                    one_shot = false;
                } else if (one_shot && now_ms - aa_last_tap_ms < DOUBLE_TAP_MS) {
                    caps_lock = true;
                    one_shot = false;
                } else {
                    one_shot = true;
                }
                aa_last_tap_ms = now_ms;
            }
        }
        return;
    }

    int id = row * COLS + col;
    if (!pressed) {
        if (id == repeat_key) {
            repeat_key = -1;
        }
        return;
    }

    if (aa_down) {
        aa_used = true;
    }
    bool repeats = false;
    uint32_t value = translate(row, col, &repeats);
    if (alt && value && !(value & KEY_EVENT)) {
        value |= KEY_ALT;
    }
    one_shot = false;
    push(value);
    if (value && repeats) {
        repeat_key = id;
        repeat_value = value;
        repeat_next_ms = now_ms + REPEAT_DELAY_MS;
    } else {
        repeat_key = -1;
    }
}

void Tab5Keymap::tick(uint32_t now_ms)
{
    if (talk_down && !talk_holding && now_ms - talk_pressed_ms >= HOLD_MS) {
        talk_holding = true;
        push(KEY_EVENT | INPUT_HOLD_START);
    }
    if (repeat_key >= 0 && reached(now_ms, repeat_next_ms)) {
        push(repeat_value);
        repeat_next_ms = now_ms + REPEAT_INTERVAL_MS;   // Late ticks don't burst
    }
}

void Tab5Keymap::reset()
{
    if (talk_holding) {
        push(KEY_EVENT | INPUT_HOLD_END);   // Stop a recording in progress
    }
    sym = ctrl = alt = aa_down = aa_used = one_shot = caps_lock = false;
    talk_down = talk_holding = talk_used = false;
    repeat_key = -1;
}
