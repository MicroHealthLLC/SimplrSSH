/*
 * Tab5Keymap: the Tab5 Keyboard's raw key positions become the keys a terminal needs
 * (legends, Sym layer, Aa shift / caps lock, Ctrl codes, arrows, auto-repeat, push-to-talk).
 */

#include "tab5_keymap.hpp"
#include "test.hpp"

using namespace board;

// Key positions (row, column) on the Tab5 Keyboard
struct Pos { int row; int col; };
static const Pos ESC{0, 0}, DEL{0, 13}, ONE{0, 1}, BANG{1, 1}, STAR{1, 8}, LBRACKET{1, 11}, TAB{2, 0};
static const Pos Q{2, 1}, BACKSPACE{2, 13}, SYM{3, 0}, AA{3, 1}, A{3, 2}, UP{3, 11}, UNDERSCORE{3, 12};
static const Pos ENTER{3, 13}, CTRL{4, 0}, ALT{4, 1}, C{4, 4}, DOT{4, 9}, LEFT{4, 10}, DOWN{4, 11};
static const Pos RIGHT{4, 12}, SPACE{4, 13};

static void press(Tab5Keymap& k, Pos p, uint32_t t) { k.key_event(true, p.row, p.col, t); }
static void release(Tab5Keymap& k, Pos p, uint32_t t) { k.key_event(false, p.row, p.col, t); }
static uint32_t tap(Tab5Keymap& k, Pos p, uint32_t t = 0)
{
    press(k, p, t);
    release(k, p, t + 10);
    return k.pop();
}

TEST(keymap_letters_digits_and_punctuation)
{
    Tab5Keymap k;
    CHECK_EQ(tap(k, Q), (uint32_t)'q');
    CHECK_EQ(tap(k, ONE), (uint32_t)'1');
    CHECK_EQ(tap(k, BANG), (uint32_t)'!');
    CHECK_EQ(tap(k, DOT), (uint32_t)'.');
    CHECK_EQ(tap(k, SPACE), (uint32_t)' ');
    CHECK_EQ(k.pop(), 0u);
}

TEST(keymap_special_keys)
{
    Tab5Keymap k;
    CHECK_EQ(tap(k, ESC), 27u);
    CHECK_EQ(tap(k, TAB), (uint32_t)'\t');
    CHECK_EQ(tap(k, ENTER), (uint32_t)'\n');
    CHECK_EQ(tap(k, BACKSPACE), 8u);
    CHECK_EQ(tap(k, DEL), KEY_EVENT | INPUT_DELETE);
}

TEST(keymap_sym_layer)
{
    Tab5Keymap k;
    press(k, SYM, 0);
    CHECK_EQ(tap(k, BANG), (uint32_t)'?');
    CHECK_EQ(tap(k, STAR), (uint32_t)'/');
    CHECK_EQ(tap(k, UNDERSCORE), (uint32_t)'=');
    CHECK_EQ(tap(k, DOT), (uint32_t)',');
    CHECK_EQ(tap(k, Q), (uint32_t)'q');            // No Sym legend: the key itself
    release(k, SYM, 50);
    CHECK_EQ(tap(k, BANG), (uint32_t)'!');
}

TEST(keymap_arrows_history_cursor_and_scroll)
{
    Tab5Keymap k;
    CHECK_EQ(tap(k, UP), KEY_EVENT | INPUT_LEFT);          // Older command / previous choice
    CHECK_EQ(tap(k, DOWN), KEY_EVENT | INPUT_RIGHT);
    CHECK_EQ(tap(k, LEFT), KEY_EVENT | INPUT_CURSOR_LEFT);
    CHECK_EQ(tap(k, RIGHT), KEY_EVENT | INPUT_CURSOR_RIGHT);
    press(k, SYM, 0);
    CHECK_EQ(tap(k, UP), KEY_EVENT | INPUT_UP);            // Scroll back
    CHECK_EQ(tap(k, DOWN), KEY_EVENT | INPUT_DOWN);
    CHECK_EQ(tap(k, LEFT), KEY_EVENT | INPUT_CURSOR_HOME);
    CHECK_EQ(tap(k, RIGHT), KEY_EVENT | INPUT_CURSOR_END);
}

TEST(keymap_ctrl_codes)
{
    Tab5Keymap k;
    press(k, CTRL, 0);
    CHECK_EQ(tap(k, C), 3u);
    CHECK_EQ(tap(k, A), 1u);
    CHECK_EQ(tap(k, LBRACKET), 27u);
    release(k, CTRL, 50);
    CHECK_EQ(tap(k, C), (uint32_t)'c');
}

TEST(keymap_alt_does_nothing_on_its_own)
{
    Tab5Keymap k;
    press(k, ALT, 0);
    CHECK_EQ(k.pop(), 0u);
    CHECK_EQ(tap(k, Q), (uint32_t)'q');
}

TEST(keymap_aa_hold_tap_and_caps_lock)
{
    Tab5Keymap k;
    // Held: shift while down
    press(k, AA, 0);
    CHECK_EQ(tap(k, Q, 10), (uint32_t)'Q');
    release(k, AA, 30);
    CHECK_EQ(tap(k, Q, 40), (uint32_t)'q');
    // Tap: one capital
    tap(k, AA, 1000);
    CHECK_EQ(tap(k, Q, 1100), (uint32_t)'Q');
    CHECK_EQ(tap(k, Q, 1200), (uint32_t)'q');
    // Double tap: caps lock, until the next tap
    tap(k, AA, 3000);
    tap(k, AA, 3200);
    CHECK_EQ(tap(k, Q, 3300), (uint32_t)'Q');
    CHECK_EQ(tap(k, A, 3400), (uint32_t)'A');
    CHECK_EQ(tap(k, ONE, 3500), (uint32_t)'1');
    tap(k, AA, 5000);
    CHECK_EQ(tap(k, Q, 5100), (uint32_t)'q');
}

TEST(keymap_auto_repeat)
{
    Tab5Keymap k;
    press(k, BACKSPACE, 0);
    CHECK_EQ(k.pop(), 8u);
    k.tick(Tab5Keymap::REPEAT_DELAY_MS - 1);
    CHECK_EQ(k.pop(), 0u);
    k.tick(Tab5Keymap::REPEAT_DELAY_MS);
    CHECK_EQ(k.pop(), 8u);
    k.tick(Tab5Keymap::REPEAT_DELAY_MS + Tab5Keymap::REPEAT_INTERVAL_MS);
    CHECK_EQ(k.pop(), 8u);
    k.tick(Tab5Keymap::REPEAT_DELAY_MS + 10000);           // A late tick repeats once, no burst
    CHECK_EQ(k.pop(), 8u);
    CHECK_EQ(k.pop(), 0u);
    release(k, BACKSPACE, 20000);
    k.tick(30000);
    CHECK_EQ(k.pop(), 0u);
}

TEST(keymap_enter_and_esc_do_not_repeat)
{
    Tab5Keymap k;
    press(k, ENTER, 0);
    CHECK_EQ(k.pop(), (uint32_t)'\n');
    k.tick(5000);
    CHECK_EQ(k.pop(), 0u);
}

TEST(keymap_ctrl_space_is_push_to_talk)
{
    Tab5Keymap k;
    press(k, CTRL, 0);
    press(k, SPACE, 0);
    CHECK_EQ(k.pop(), 0u);                                  // Nothing typed
    k.tick(Tab5Keymap::HOLD_MS);
    CHECK_EQ(k.pop(), KEY_EVENT | INPUT_HOLD_START);
    release(k, CTRL, 500);                                  // Ctrl may go first
    release(k, SPACE, 800);
    CHECK_EQ(k.pop(), KEY_EVENT | INPUT_HOLD_END);

    press(k, CTRL, 2000);
    press(k, SPACE, 2000);
    k.tick(2000 + Tab5Keymap::HOLD_MS);
    CHECK_EQ(k.pop(), KEY_EVENT | INPUT_HOLD_START);
    release(k, SPACE, 2000 + Tab5Keymap::LONG_HOLD_MS);
    CHECK_EQ(k.pop(), KEY_EVENT | INPUT_HOLD_END_LONG);

    press(k, SPACE, 4000);                                  // Short Ctrl+Space: nothing
    release(k, SPACE, 4100);
    k.tick(5000);
    CHECK_EQ(k.pop(), 0u);
}

TEST(keymap_reset_stops_talking_and_modifiers)
{
    Tab5Keymap k;
    press(k, CTRL, 0);
    press(k, SPACE, 0);
    k.tick(Tab5Keymap::HOLD_MS);
    CHECK_EQ(k.pop(), KEY_EVENT | INPUT_HOLD_START);
    k.reset();                                              // Keyboard pulled off while talking
    CHECK_EQ(k.pop(), KEY_EVENT | INPUT_HOLD_END);
    CHECK_EQ(tap(k, C), (uint32_t)'c');                     // Ctrl no longer held
}

TEST(keymap_ignores_positions_outside_the_matrix)
{
    Tab5Keymap k;
    k.key_event(true, 5, 0, 0);
    k.key_event(true, 0, 14, 0);
    k.key_event(true, -1, 0, 0);
    CHECK_EQ(k.pop(), 0u);
}

TEST(keymap_queue_is_bounded)
{
    Tab5Keymap k;
    for (int i = 0; i < 100; i++) {
        press(k, Q, 0);
        release(k, Q, 1);
    }
    int n = 0;
    while (k.pop()) {
        n++;
    }
    CHECK(n <= 16);
}
