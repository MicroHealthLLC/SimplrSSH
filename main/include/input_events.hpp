/*
 * Input Events
 * What the boards' input devices produce for keypad_task (deck_base.cpp). No ESP-IDF or
 * LVGL headers, so key handling logic can be tested on the host (test/host).
 */

#ifndef INPUT_EVENTS_HPP
#define INPUT_EVENTS_HPP

#include <cstdint>

namespace board
{
    // Input events, queued for keypad_task, the one task that runs the terminal. The T-Deck's
    // trackball and the Tab5's keyboard post them.
    enum InputEvent : uint8_t {
        INPUT_UP,             // Scroll back (older text)
        INPUT_DOWN,           // Scroll toward the newest text
        INPUT_LEFT,           // Older command / previous menu choice
        INPUT_RIGHT,          // Newer command / next menu choice
        INPUT_ENTER,
        INPUT_HOLD_START,     // Push-to-talk pressed (ChatGPT)
        INPUT_HOLD_END,       // Push-to-talk released
        INPUT_HOLD_END_LONG,  // Long press released: deletes the shown history entry
        INPUT_CURSOR_LEFT,    // Move the cursor on the input line
        INPUT_CURSOR_RIGHT,
        INPUT_CURSOR_HOME,
        INPUT_CURSOR_END,
        INPUT_DELETE,         // Delete the character at the cursor
        INPUT_F1,             // Function keys F1-F12 (INPUT_F1 + 0..11), full-screen SSH terminal only
        INPUT_F12 = INPUT_F1 + 11,
    };

    // Keyboard values (board::read_key()): 0 when no key is waiting, a byte (printable ASCII,
    // '\n', 8 backspace, 27 Esc, '\t', or a Ctrl+key control code 1..31), or
    // KEY_EVENT | InputEvent for keys that act like the trackball (arrows, scrolling), or
    // KEY_ALT | byte for a key typed with Alt (Meta: Esc first, in the full-screen SSH terminal).
    static const uint32_t KEY_EVENT = 0x100;
    static const uint32_t KEY_ALT = 0x200;
}

#endif
