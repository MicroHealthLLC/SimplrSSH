/*
 * VTerm: the Tab5's full-screen terminal. Output from real programs (shell prompts, vim, htop,
 * less, ncurses line drawing) must land in the right cells, and nothing a server sends may
 * break it (bounded parameters, random bytes).
 */

#include "vterm.hpp"
#include "test.hpp"
#include <cstring>

static void feed(VTerm& t, const char* s)
{
    t.feed(s, strlen(s));
}

// One screen row as text (ASCII; anything else becomes '?'), trailing blanks dropped
static std::string row(const VTerm& t, int r, int back = 0)
{
    const VTerm::Cell* cells = t.view_row(r, back);
    std::string s;
    for (int c = 0; c < t.cols(); c++) {
        uint32_t ch = cells[c].ch;
        s += (ch >= 32 && ch < 127) ? (char)ch : '?';
    }
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}

TEST(vterm_text_and_newlines)
{
    VTerm t(10, 4, 10);
    feed(t, "ab\r\ncd");
    CHECK_EQ(row(t, 0), std::string("ab"));
    CHECK_EQ(row(t, 1), std::string("cd"));
    CHECK_EQ(t.cursor_row(), 1);
    CHECK_EQ(t.cursor_col(), 2);
}

TEST(vterm_wraps_only_when_the_next_character_comes)
{
    VTerm t(10, 4, 10);
    feed(t, "0123456789");
    CHECK_EQ(t.cursor_row(), 0);
    CHECK_EQ(t.cursor_col(), 9);
    feed(t, "\r\nx");   // A full line followed by CRLF: no blank line in between
    CHECK_EQ(row(t, 1), std::string("x"));
    feed(t, "\r\n0123456789A");
    CHECK_EQ(row(t, 2), std::string("0123456789"));
    CHECK_EQ(row(t, 3), std::string("A"));
}

TEST(vterm_cursor_addressing_and_erase)
{
    VTerm t(10, 4, 10);
    feed(t, "aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc");
    feed(t, "\x1b[2;4H");   // Row 2, column 4
    CHECK_EQ(t.cursor_row(), 1);
    CHECK_EQ(t.cursor_col(), 3);
    feed(t, "\x1b[K");
    CHECK_EQ(row(t, 1), std::string("bbb"));
    feed(t, "\x1b[1;3H\x1b[1K");
    CHECK_EQ(row(t, 0), std::string("   aaaaaaa"));
    feed(t, "\x1b[2J");
    CHECK_EQ(row(t, 0), std::string(""));
    CHECK_EQ(row(t, 2), std::string(""));
    feed(t, "\x1b[H\x1b[3Bx\x1b[5Cy\x1b[2Dz\x1b[Aw");
    CHECK_EQ(row(t, 3), std::string("x    zy"));
    CHECK_EQ(row(t, 2), std::string("      w"));
}

TEST(vterm_scrolling_keeps_scrollback)
{
    VTerm t(10, 3, 5);
    feed(t, "1\r\n2\r\n3\r\n4\r\n5");
    CHECK_EQ(row(t, 0), std::string("3"));
    CHECK_EQ(row(t, 2), std::string("5"));
    CHECK_EQ(t.scrollback_count(), 2);
    CHECK_EQ(t.take_scrolled(), 2);
    CHECK_EQ(t.take_scrolled(), 0);
    CHECK_EQ(row(t, 0, 2), std::string("1"));   // Two lines back
    CHECK_EQ(row(t, 2, 2), std::string("3"));
    CHECK_EQ(row(t, 0, 99), std::string("1"));  // Clamped to the oldest line
    for (int i = 0; i < 20; i++) {
        feed(t, "\r\nx");
    }
    CHECK_EQ(t.scrollback_count(), 5);           // Bounded
}

TEST(vterm_scroll_region_and_reverse_index)
{
    VTerm t(10, 5, 10);
    feed(t, "a\r\nb\r\nc\r\nd\r\ne");
    feed(t, "\x1b[2;4r");          // Region rows 2-4; cursor goes home
    CHECK_EQ(t.cursor_row(), 0);
    feed(t, "\x1b[4;1H\n");        // LF at the region's bottom scrolls only the region
    CHECK_EQ(row(t, 0), std::string("a"));
    CHECK_EQ(row(t, 1), std::string("c"));
    CHECK_EQ(row(t, 2), std::string("d"));
    CHECK_EQ(row(t, 3), std::string(""));
    CHECK_EQ(row(t, 4), std::string("e"));
    CHECK_EQ(t.scrollback_count(), 0);
    feed(t, "\x1b[2;1H\x1bM");     // Reverse index at the region's top scrolls it down
    CHECK_EQ(row(t, 1), std::string(""));
    CHECK_EQ(row(t, 2), std::string("c"));
    CHECK_EQ(row(t, 4), std::string("e"));
}

TEST(vterm_insert_and_delete)
{
    VTerm t(10, 4, 10);
    feed(t, "abcdef\x1b[1;3H\x1b[2@");
    CHECK_EQ(row(t, 0), std::string("ab  cdef"));
    feed(t, "\x1b[3P");
    CHECK_EQ(row(t, 0), std::string("abdef"));
    feed(t, "\x1b[1;2H\x1b[2X");
    CHECK_EQ(row(t, 0), std::string("a  ef"));
    feed(t, "\r\n1\r\n2\r\n3\x1b[2;1H\x1b[L");
    CHECK_EQ(row(t, 1), std::string(""));
    CHECK_EQ(row(t, 2), std::string("1"));
    CHECK_EQ(row(t, 3), std::string("2"));
    feed(t, "\x1b[2M");
    CHECK_EQ(row(t, 1), std::string("2"));
    CHECK_EQ(t.scrollback_count(), 0);   // Deleted lines don't go to the scrollback
    feed(t, "\x1b[4h\x1b[1;1HXY\x1b[4l");
    CHECK_EQ(row(t, 0), std::string("XYa  ef"));
}

TEST(vterm_alternate_screen_restores_the_shell)
{
    VTerm t(10, 4, 10);
    feed(t, "$ vim\r\n");
    feed(t, "\x1b[?1049h");
    CHECK(t.alt_screen());
    CHECK_EQ(row(t, 0), std::string(""));
    feed(t, "\x1b[1;1Hediting\x1b[4;1H~");
    feed(t, "\x1b[?1049l");
    CHECK(!t.alt_screen());
    CHECK_EQ(row(t, 0), std::string("$ vim"));
    CHECK_EQ(t.cursor_row(), 1);
    CHECK_EQ(t.cursor_col(), 0);
}

TEST(vterm_colors_and_attributes)
{
    VTerm t(10, 4, 10);
    feed(t, "\x1b[1;31ma\x1b[38;5;200mb\x1b[38;2;1;2;3mc\x1b[0;7md\x1b[44me\x1b[mf");
    const VTerm::Cell* r = t.view_row(0, 0);
    CHECK_EQ(r[0].fg, VTerm::PALETTE | 1);
    CHECK(r[0].attr & VTerm::BOLD);
    CHECK_EQ(r[1].fg, VTerm::PALETTE | 200);
    CHECK_EQ(r[2].fg, VTerm::RGB | 0x010203);
    CHECK(r[3].attr & VTerm::REVERSE);
    CHECK(!(r[3].attr & VTerm::BOLD));
    CHECK_EQ(r[3].fg, VTerm::DEFAULT_COLOR);
    CHECK_EQ(r[4].bg, VTerm::PALETTE | 4);
    CHECK_EQ(r[5].attr, (uint16_t)0);
    CHECK_EQ(r[5].bg, VTerm::DEFAULT_COLOR);
    feed(t, "\x1b[42m\x1b[2;1H\x1b[K");   // Erasing paints the current background
    CHECK_EQ(t.view_row(1, 0)[5].bg, VTerm::PALETTE | 2);
    feed(t, "\x1b[>4;1m");                // xterm key modifiers: not SGR
    feed(t, "x");
    CHECK_EQ(t.view_row(1, 0)[0].attr, (uint16_t)0);
}

TEST(vterm_line_drawing_and_unicode)
{
    VTerm t(10, 4, 10);
    feed(t, "\x1b(0lqk\x1b(Bq");
    const VTerm::Cell* r = t.view_row(0, 0);
    CHECK_EQ(r[0].ch, 0x250Cu);
    CHECK_EQ(r[1].ch, 0x2500u);
    CHECK_EQ(r[2].ch, 0x2510u);
    CHECK_EQ(r[3].ch, (uint32_t)'q');
    feed(t, "\r\n\xc3\xa9\xe4\xb8\xad!\xff\xe2\x94");   // e-acute, a wide CJK character, junk
    r = t.view_row(1, 0);
    CHECK_EQ(r[0].ch, 0xE9u);
    CHECK_EQ(r[1].ch, 0x4E2Du);
    CHECK_EQ(r[2].ch, VTerm::WIDE_TAIL);
    CHECK_EQ(r[3].ch, (uint32_t)'!');
    CHECK_EQ(r[4].ch, 0xFFFDu);
    feed(t, "x");                                         // Ends the truncated sequence
    CHECK_EQ(r[5].ch, 0xFFFDu);
    CHECK_EQ(r[6].ch, (uint32_t)'x');
    feed(t, "\x1b[2;3Hy");                                // Over half of the wide character
    CHECK_EQ(r[1].ch, (uint32_t)' ');
    CHECK_EQ(r[2].ch, (uint32_t)'y');
}

TEST(vterm_answers_queries)
{
    VTerm t(10, 4, 10);
    feed(t, "\x1b[3;5H\x1b[6n");
    CHECK_EQ(t.take_reply(), std::string("\x1b[3;5R"));
    feed(t, "\x1b[c");
    CHECK_EQ(t.take_reply(), std::string("\x1b[?62;22c"));
    feed(t, "\x1b[>c");   // Secondary attributes: no answer
    CHECK_EQ(t.take_reply(), std::string(""));
    for (int i = 0; i < 1000; i++) {
        feed(t, "\x1b[6n");
    }
    CHECK(t.take_reply().size() <= 300);   // A flood of queries can't grow it without bound
}

TEST(vterm_keys_follow_cursor_mode)
{
    VTerm t(10, 4, 10);
    CHECK_EQ(t.key(VTerm::Key::Up), std::string("\x1b[A"));
    feed(t, "\x1b[?1h");
    CHECK_EQ(t.key(VTerm::Key::Up), std::string("\x1bOA"));
    CHECK_EQ(t.key(VTerm::Key::Home), std::string("\x1bOH"));
    CHECK_EQ(t.key(VTerm::Key::PageDown), std::string("\x1b[6~"));
    CHECK_EQ(t.key(VTerm::Key::F10), std::string("\x1b[21~"));
    feed(t, "\x1b[?1l");
    CHECK_EQ(t.key(VTerm::Key::Left), std::string("\x1b[D"));
}

TEST(vterm_ignores_titles_and_device_strings)
{
    VTerm t(10, 4, 10);
    feed(t, "\x1b]0;user@host: ~\x07" "a\x1b]2;title\x1b\\b\x1bP+q544e\x1b\\c");
    CHECK_EQ(row(t, 0), std::string("abc"));
    feed(t, "\x1b[?25l");
    CHECK(!t.cursor_visible());
    feed(t, "\x1b[?25h\x1b[?2004h\x1b[?1000h\x1b[ q");
    CHECK(t.cursor_visible());
    CHECK_EQ(row(t, 0), std::string("abc"));
}

TEST(vterm_tabs_and_repeat)
{
    VTerm t(20, 4, 10);
    feed(t, "a\tb\x1b[3gc\td");
    CHECK_EQ(row(t, 0), std::string("a       bc         d"));
    feed(t, "\r\n-\x1b[4b");
    CHECK_EQ(row(t, 1), std::string("-----"));
}

TEST(vterm_clamps_hostile_parameters)
{
    VTerm t(10, 4, 10);
    feed(t, "\x1b[99999999;99999999H");
    CHECK_EQ(t.cursor_row(), 3);
    CHECK_EQ(t.cursor_col(), 9);
    feed(t, "\x1b[99999999A\x1b[99999999@\x1b[99999999L\x1b[99999999b\x1b[0;0r\x1b[5;2r");
    CHECK(t.cursor_row() >= 0 && t.cursor_row() < 4);
    std::string many = "\x1b[";
    for (int i = 0; i < 100; i++) {
        many += "1;";
    }
    feed(t, (many + "m").c_str());
    CHECK(t.view_row(0, 0) != nullptr);
}

TEST(vterm_survives_random_bytes)
{
    VTerm t(13, 7, 20);
    uint32_t seed = 12345;
    std::string chunk;
    for (int round = 0; round < 200; round++) {
        chunk.clear();
        for (int i = 0; i < 1000; i++) {
            seed = seed * 1103515245 + 12345;
            uint8_t b = (uint8_t)(seed >> 16);
            // Mostly escape-sequence material, so the parser's states all get exercised
            static const char pick[] = "\x1b[]?;0123456789HJKLMPm@hlrABCD\r\n\x0e\x0f(0Bab\xe4\xb8\xad";
            chunk += (b & 1) ? (char)b : pick[b % (sizeof(pick) - 1)];
        }
        t.feed(chunk.data(), chunk.size());
        t.take_reply();
        for (int r = 0; r < t.rows(); r++) {
            CHECK(t.view_row(r, round % 25) != nullptr);
        }
        CHECK(t.cursor_row() >= 0 && t.cursor_row() < t.rows());
        CHECK(t.cursor_col() >= 0 && t.cursor_col() < t.cols());
    }
}

TEST(vterm_tracks_changed_rows)
{
    VTerm t(10, 4, 10);
    t.clear_dirty();
    feed(t, "\x1b[3;1Hx");
    CHECK(!t.row_dirty(0));
    CHECK(t.row_dirty(2));
    t.clear_dirty();
    feed(t, "\x1b[?1049h");
    CHECK(t.row_dirty(0) && t.row_dirty(3));
}

TEST(vterm_wheel_scrolls_the_program_or_the_scrollback)
{
    VTerm t(10, 4, 10);
    CHECK_EQ(t.wheel(true, 0, 0), std::string(""));                  // Shell: the caller scrolls back
    feed(t, "\x1b[?1049h");
    CHECK_EQ(t.wheel(true, 0, 0), std::string("\x1b[A"));            // Full-screen program: arrows
    feed(t, "\x1b[?1h");
    CHECK_EQ(t.wheel(false, 0, 0), std::string("\x1bOB"));
    feed(t, "\x1b[?1000h\x1b[?1006h");                                 // Program asked for the mouse
    CHECK_EQ(t.wheel(true, 2, 4), std::string("\x1b[<64;5;3M"));
    CHECK_EQ(t.wheel(false, 99, 99), std::string("\x1b[<65;10;4M"));  // Clamped to the screen
    feed(t, "\x1b[?1006l");
    CHECK_EQ(t.wheel(true, 0, 1), std::string("\x1b[M\x60\x22\x21"));  // Legacy: 32+64, 32+2, 32+1
    feed(t, "\x1b[?1000l");
    CHECK_EQ(t.wheel(true, 0, 0), std::string("\x1bOA"));
    feed(t, "\x1b[?1049l\x1b[?1002h");
    CHECK_EQ(t.wheel(true, 0, 0), std::string("\x1b[M\x60\x21\x21"));  // Mouse mode at the shell (tmux)
    feed(t, "\x1b" "c");
    CHECK_EQ(t.wheel(true, 0, 0), std::string(""));                   // Reset: mouse off
}
