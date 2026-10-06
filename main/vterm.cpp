/*
 * VTerm Implementation
 * The subset of xterm that full-screen programs use (vim, nano, htop, less, mc, tmux):
 * cursor addressing, erase/insert/delete, scroll regions, SGR colors (16, 256, RGB),
 * alternate screen, DEC line drawing, auto-wrap with xterm's pending-wrap rule, UTF-8.
 * Anything else is parsed and ignored.
 */

#include "vterm.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

// DEC Special Graphics ('0' charset) for 0x5F-0x7E: line drawing used by ncurses
static const uint16_t DEC_GRAPHICS[32] = {
    0x0020, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,   // _ ` a b c d e f
    0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,   // g h i j k l m n
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,   // o p q r s t u v
    0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,   // w x y z { | } ~
};

static const int MAX_PARAM_VALUE = 9999;
static const size_t MAX_REPLY = 256;

VTerm::VTerm(int cols, int rows, int scrollback_lines)
    : ncols(std::max(cols, 2)), nrows(std::max(rows, 2)), sb_lines(std::max(scrollback_lines, 0)),
      main_cells((size_t)ncols * nrows), alt_cells((size_t)ncols * nrows),
      sb_cells((size_t)ncols * sb_lines), dirty(nrows), tabs(ncols), screen(main_cells.data())
{
    reset();
}

int VTerm::char_width(uint32_t cp)
{
    if (cp < 0x300) {
        return 1;
    }
    // Combining marks and zero-width characters
    if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0x483 && cp <= 0x489) || (cp >= 0x591 && cp <= 0x5BD) ||
        (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
        (cp >= 0xE0100 && cp <= 0xE01EF)) {
        return 0;
    }
    // East Asian wide and emoji
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
        (cp >= 0x1F680 && cp <= 0x1F6FF) || (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

void VTerm::reset()
{
    pen = {' ', DEFAULT_COLOR, DEFAULT_COLOR, 0};
    std::fill(main_cells.begin(), main_cells.end(), pen);
    std::fill(alt_cells.begin(), alt_cells.end(), pen);
    screen = main_cells.data();
    alt_active = false;
    sb_head = sb_count = scrolled = 0;
    cur_row = cur_col = 0;
    wrap_pending = false;
    charset[0] = charset[1] = 'B';
    gl = 0;
    saved = Saved();
    saved_alt = Saved();
    show_cursor = autowrap = true;
    origin_mode = insert_mode = newline_mode = app_cursor = false;
    mouse_mode = 0;
    mouse_sgr = false;
    top = 0;
    bottom = nrows - 1;
    for (int c = 0; c < ncols; c++) {
        tabs[c] = c > 0 && c % 8 == 0;
    }
    last_char = ' ';
    state = State::Ground;
    utf8_left = 0;
    reply.clear();
    mark_all();
}

std::string VTerm::take_reply()
{
    std::string r;
    r.swap(reply);
    return r;
}

void VTerm::mark_all()
{
    std::fill(dirty.begin(), dirty.end(), 1);
}

void VTerm::clear_dirty()
{
    std::fill(dirty.begin(), dirty.end(), 0);
}

int VTerm::take_scrolled()
{
    int n = scrolled;
    scrolled = 0;
    return n;
}

const VTerm::Cell* VTerm::view_row(int row, int back) const
{
    row = std::min(std::max(row, 0), nrows - 1);
    back = alt_active ? 0 : std::min(std::max(back, 0), sb_count);
    int index = sb_count - back + row;   // Line in scrollback + screen
    if (index < sb_count) {
        return &sb_cells[(size_t)((sb_head + index) % sb_lines) * ncols];
    }
    return &screen[(size_t)(index - sb_count) * ncols];
}

VTerm::Cell VTerm::blank() const
{
    // Erased cells take the current background color (xterm "bce")
    return {' ', DEFAULT_COLOR, pen.bg, 0};
}

void VTerm::fix_wide(int row, int col)
{
    Cell* r = row_ptr(row);
    if (r[col].ch == WIDE_TAIL && col > 0) {
        r[col - 1].ch = ' ';
    }
    if (col + 1 < ncols && r[col + 1].ch == WIDE_TAIL) {
        r[col + 1].ch = ' ';
    }
}

void VTerm::clear_cells(int row, int from, int to)
{
    from = std::max(from, 0);
    to = std::min(to, ncols);
    if (from >= to) {
        return;
    }
    fix_wide(row, from);
    fix_wide(row, to - 1);
    std::fill(row_ptr(row) + from, row_ptr(row) + to, blank());
    mark(row);
}

void VTerm::push_scrollback(const Cell* line)
{
    if (sb_lines == 0) {
        return;
    }
    int slot;
    if (sb_count < sb_lines) {
        slot = (sb_head + sb_count) % sb_lines;
        sb_count++;
    } else {
        slot = sb_head;   // Full: the oldest line makes room
        sb_head = (sb_head + 1) % sb_lines;
    }
    std::copy(line, line + ncols, &sb_cells[(size_t)slot * ncols]);
}

// Lines from..to (inclusive) move up n. Output scrolling the whole main screen (to_scrollback)
// keeps the lines leaving the top in the scrollback.
void VTerm::scroll_up(int from, int to, int n, bool to_scrollback)
{
    n = std::min(n, to - from + 1);
    if (n <= 0) {
        return;
    }
    if (to_scrollback && !alt_active && from == 0 && to == nrows - 1) {
        for (int i = 0; i < n; i++) {
            push_scrollback(row_ptr(i));
        }
        scrolled += n;
    }
    std::copy(row_ptr(from + n), row_ptr(to + 1), row_ptr(from));
    for (int r = to - n + 1; r <= to; r++) {
        std::fill(row_ptr(r), row_ptr(r) + ncols, blank());
    }
    for (int r = from; r <= to; r++) {
        mark(r);
    }
}

void VTerm::scroll_down(int from, int to, int n)
{
    n = std::min(n, to - from + 1);
    if (n <= 0) {
        return;
    }
    std::copy_backward(row_ptr(from), row_ptr(to + 1 - n), row_ptr(to + 1));
    for (int r = from; r < from + n; r++) {
        std::fill(row_ptr(r), row_ptr(r) + ncols, blank());
    }
    for (int r = from; r <= to; r++) {
        mark(r);
    }
}

void VTerm::line_feed()
{
    wrap_pending = false;
    if (cur_row == bottom) {
        scroll_up(top, bottom, 1, true);
    } else if (cur_row < nrows - 1) {
        cur_row++;
    }
}

void VTerm::reverse_index()
{
    wrap_pending = false;
    if (cur_row == top) {
        scroll_down(top, bottom, 1);
    } else if (cur_row > 0) {
        cur_row--;
    }
}

void VTerm::clamp_cursor()
{
    cur_row = std::min(std::max(cur_row, 0), nrows - 1);
    cur_col = std::min(std::max(cur_col, 0), ncols - 1);
}

void VTerm::move_to(int row, int col)
{
    if (origin_mode) {
        cur_row = std::min(std::max(row + top, top), bottom);
    } else {
        cur_row = row;
    }
    cur_col = col;
    wrap_pending = false;
    clamp_cursor();
}

void VTerm::save_cursor(Saved& s) const
{
    s.row = cur_row;
    s.col = cur_col;
    s.pen = pen;
    s.origin = origin_mode;
    s.wrap_pending = wrap_pending;
    s.g[0] = charset[0];
    s.g[1] = charset[1];
    s.gl = gl;
}

void VTerm::restore_cursor(const Saved& s)
{
    cur_row = s.row;
    cur_col = s.col;
    pen = s.pen;
    origin_mode = s.origin;
    wrap_pending = s.wrap_pending;
    charset[0] = s.g[0];
    charset[1] = s.g[1];
    gl = s.gl;
    clamp_cursor();
}

void VTerm::switch_screen(bool alt, bool clear)
{
    alt_active = alt;
    screen = alt ? alt_cells.data() : main_cells.data();
    if (clear) {
        std::fill(screen, screen + (size_t)ncols * nrows, blank());
    }
    mark_all();
}

void VTerm::put_char(uint32_t cp)
{
    if (charset[gl] == '0' && cp >= 0x5F && cp <= 0x7E) {
        cp = DEC_GRAPHICS[cp - 0x5F];
    }
    int width = char_width(cp);
    if (width == 0) {
        return;   // Combining marks are not drawn
    }
    if (wrap_pending) {
        if (autowrap) {
            cur_col = 0;
            line_feed();
        }
        wrap_pending = false;
    }
    if (width == 2 && cur_col == ncols - 1) {
        if (autowrap) {
            clear_cells(cur_row, cur_col, ncols);
            cur_col = 0;
            line_feed();
        } else {
            cp = 0xFFFD;
            width = 1;
        }
    }

    Cell* r = row_ptr(cur_row);
    if (insert_mode) {
        fix_wide(cur_row, ncols - width);
        std::copy_backward(r + cur_col, r + ncols - width, r + ncols);
    }
    fix_wide(cur_row, cur_col);
    if (width == 2) {
        fix_wide(cur_row, cur_col + 1);
    }
    r[cur_col] = pen;
    r[cur_col].ch = cp;
    if (width == 2) {
        r[cur_col + 1] = pen;
        r[cur_col + 1].ch = WIDE_TAIL;
    }
    mark(cur_row);
    last_char = cp;

    if (cur_col + width >= ncols) {
        cur_col = ncols - 1;
        wrap_pending = autowrap;
    } else {
        cur_col += width;
    }
}

void VTerm::control(uint8_t c)
{
    switch (c) {
        case 0x08:   // BS
            if (cur_col > 0) {
                cur_col--;
            }
            wrap_pending = false;
            break;
        case 0x09:   // HT
            while (cur_col < ncols - 1 && !tabs[++cur_col]) {
            }
            wrap_pending = false;
            break;
        case 0x0A:   // LF, VT, FF
        case 0x0B:
        case 0x0C:
            line_feed();
            if (newline_mode) {
                cur_col = 0;
            }
            break;
        case 0x0D:   // CR
            cur_col = 0;
            wrap_pending = false;
            break;
        case 0x0E:   // SO: G1
            gl = 1;
            break;
        case 0x0F:   // SI: G0
            gl = 0;
            break;
        case 0x18:   // CAN, SUB: abort a sequence
        case 0x1A:
            state = State::Ground;
            break;
        case 0x1B:
            state = State::Esc;
            esc_inter = 0;
            break;
        default:     // BEL and the rest
            break;
    }
}

void VTerm::esc_dispatch(uint8_t c)
{
    switch (c) {
        case '7': save_cursor(alt_active ? saved_alt : saved); break;
        case '8': restore_cursor(alt_active ? saved_alt : saved); break;
        case 'D': line_feed(); break;
        case 'E': cur_col = 0; line_feed(); break;
        case 'M': reverse_index(); break;
        case 'H': tabs[cur_col] = 1; break;
        case 'c': reset(); break;
        case 'Z':
            if (reply.size() < MAX_REPLY) {
                reply += "\x1b[?62;22c";
            }
            break;
        default: break;   // Keypad modes (= >), ST and others
    }
}

int VTerm::param(int i, int def) const
{
    return (i < nparams && params[i] > 0) ? params[i] : def;
}

void VTerm::sgr()
{
    for (int i = 0; i < nparams; i++) {
        int p = params[i];
        if (p == 0) {
            pen.attr = 0;
            pen.fg = pen.bg = DEFAULT_COLOR;
        } else if (p == 1) {
            pen.attr |= BOLD;
        } else if (p == 2) {
            pen.attr |= DIM;
        } else if (p == 3) {
            pen.attr |= ITALIC;
        } else if (p == 4 || p == 21) {
            pen.attr |= UNDERLINE;
        } else if (p == 5 || p == 6) {
            pen.attr |= BLINK;
        } else if (p == 7) {
            pen.attr |= REVERSE;
        } else if (p == 8) {
            pen.attr |= HIDDEN;
        } else if (p == 9) {
            pen.attr |= STRIKE;
        } else if (p == 22) {
            pen.attr &= ~(BOLD | DIM);
        } else if (p == 23) {
            pen.attr &= ~ITALIC;
        } else if (p == 24) {
            pen.attr &= ~UNDERLINE;
        } else if (p == 25) {
            pen.attr &= ~BLINK;
        } else if (p == 27) {
            pen.attr &= ~REVERSE;
        } else if (p == 28) {
            pen.attr &= ~HIDDEN;
        } else if (p == 29) {
            pen.attr &= ~STRIKE;
        } else if (p >= 30 && p <= 37) {
            pen.fg = PALETTE | (p - 30);
        } else if (p >= 40 && p <= 47) {
            pen.bg = PALETTE | (p - 40);
        } else if (p >= 90 && p <= 97) {
            pen.fg = PALETTE | (p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            pen.bg = PALETTE | (p - 100 + 8);
        } else if (p == 39) {
            pen.fg = DEFAULT_COLOR;
        } else if (p == 49) {
            pen.bg = DEFAULT_COLOR;
        } else if (p == 38 || p == 48) {
            uint32_t color;
            if (i + 2 < nparams && params[i + 1] == 5) {
                color = PALETTE | std::min(params[i + 2], 255);
                i += 2;
            } else if (i + 4 < nparams && params[i + 1] == 2) {
                color = RGB | (uint32_t)std::min(params[i + 2], 255) << 16 |
                        (uint32_t)std::min(params[i + 3], 255) << 8 | (uint32_t)std::min(params[i + 4], 255);
                i += 4;
            } else {
                break;   // Malformed: ignore the rest
            }
            (p == 38 ? pen.fg : pen.bg) = color;
        }
    }
}

void VTerm::set_mode(bool on)
{
    for (int i = 0; i < nparams; i++) {
        int p = params[i];
        if (csi_private != '?') {
            if (p == 4) {
                insert_mode = on;
            } else if (p == 20) {
                newline_mode = on;
            }
            continue;
        }
        switch (p) {
            case 1: app_cursor = on; break;
            case 6: origin_mode = on; move_to(0, 0); break;
            case 7: autowrap = on; if (!on) wrap_pending = false; break;
            case 25: show_cursor = on; mark(cur_row); break;
            case 47:
                if (on != alt_active) {
                    switch_screen(on, false);
                }
                break;
            case 1047:
                if (on != alt_active) {
                    if (!on) {
                        switch_screen(true, true);   // Leaving: the alternate screen is cleared
                    }
                    switch_screen(on, false);
                }
                break;
            case 1048:
                if (on) {
                    save_cursor(saved);
                } else {
                    restore_cursor(saved);
                }
                break;
            case 1049:
                if (on && !alt_active) {
                    save_cursor(saved);
                    switch_screen(true, true);
                } else if (!on && alt_active) {
                    switch_screen(false, false);
                    restore_cursor(saved);
                }
                break;
            case 9:
            case 1000:
            case 1002:
            case 1003:   // Mouse reports: only the wheel is sent (touch drags)
                if (on) {
                    mouse_mode = p;
                } else if (mouse_mode == p) {
                    mouse_mode = 0;
                }
                break;
            case 1006: mouse_sgr = on; break;
            default: break;   // Bracketed paste, focus events...: not used
        }
    }
}

void VTerm::csi_dispatch(uint8_t c)
{
    if (csi_private == '>' || csi_private == '<' || csi_private == '=') {
        return;   // Secondary device attributes, xterm key modifiers...: not supported
    }
    if (csi_inter) {
        if (csi_inter == '!' && c == 'p') {   // DECSTR soft reset
            show_cursor = autowrap = true;
            origin_mode = insert_mode = app_cursor = false;
            top = 0;
            bottom = nrows - 1;
            pen = {' ', DEFAULT_COLOR, DEFAULT_COLOR, 0};
            charset[0] = charset[1] = 'B';
            gl = 0;
        }
        return;   // Cursor shape (SP q) and others
    }
    if (csi_private == '?' && c != 'h' && c != 'l' && c != 'J' && c != 'K') {
        return;
    }

    const int n = param(0, 1);
    const bool keeps_wrap = c == 'm' || c == 'n' || c == 'c' || c == 'h' || c == 'l' || c == 't' || c == 'g';
    if (!keeps_wrap) {
        wrap_pending = false;
    }
    Cell* r = row_ptr(cur_row);

    switch (c) {
        case '@': {   // ICH: insert blanks
            int k = std::min(n, ncols - cur_col);
            fix_wide(cur_row, cur_col);
            fix_wide(cur_row, ncols - k);
            std::copy_backward(r + cur_col, r + ncols - k, r + ncols);
            std::fill(r + cur_col, r + cur_col + k, blank());
            mark(cur_row);
            break;
        }
        case 'A':     // CUU
            cur_row = std::max(cur_row - n, cur_row >= top ? top : 0);
            break;
        case 'B':     // CUD, VPR
        case 'e':
            cur_row = std::min(cur_row + n, cur_row <= bottom ? bottom : nrows - 1);
            break;
        case 'C':     // CUF, HPR
        case 'a':
            cur_col = std::min(cur_col + n, ncols - 1);
            break;
        case 'D':     // CUB
            cur_col = std::max(cur_col - n, 0);
            break;
        case 'E':     // CNL
            cur_row = std::min(cur_row + n, cur_row <= bottom ? bottom : nrows - 1);
            cur_col = 0;
            break;
        case 'F':     // CPL
            cur_row = std::max(cur_row - n, cur_row >= top ? top : 0);
            cur_col = 0;
            break;
        case 'G':     // CHA, HPA
        case '`':
            cur_col = std::min(n, ncols) - 1;
            break;
        case 'H':     // CUP, HVP
        case 'f':
            move_to(param(0, 1) - 1, param(1, 1) - 1);
            break;
        case 'I':     // CHT
            for (int i = 0; i < n && cur_col < ncols - 1; i++) {
                while (cur_col < ncols - 1 && !tabs[++cur_col]) {
                }
            }
            break;
        case 'Z':     // CBT
            for (int i = 0; i < n && cur_col > 0; i++) {
                while (cur_col > 0 && !tabs[--cur_col]) {
                }
            }
            break;
        case 'J':     // ED
            switch (param(0, 0)) {
                case 0:
                    clear_cells(cur_row, cur_col, ncols);
                    for (int i = cur_row + 1; i < nrows; i++) {
                        clear_cells(i, 0, ncols);
                    }
                    break;
                case 1:
                    for (int i = 0; i < cur_row; i++) {
                        clear_cells(i, 0, ncols);
                    }
                    clear_cells(cur_row, 0, cur_col + 1);
                    break;
                case 2:
                    for (int i = 0; i < nrows; i++) {
                        clear_cells(i, 0, ncols);
                    }
                    break;
                case 3:
                    sb_count = sb_head = 0;
                    mark_all();
                    break;
                default: break;
            }
            break;
        case 'K':     // EL
            switch (param(0, 0)) {
                case 0: clear_cells(cur_row, cur_col, ncols); break;
                case 1: clear_cells(cur_row, 0, cur_col + 1); break;
                case 2: clear_cells(cur_row, 0, ncols); break;
                default: break;
            }
            break;
        case 'L':     // IL
            if (cur_row >= top && cur_row <= bottom) {
                scroll_down(cur_row, bottom, n);
                cur_col = 0;
            }
            break;
        case 'M':     // DL
            if (cur_row >= top && cur_row <= bottom) {
                scroll_up(cur_row, bottom, n, false);
                cur_col = 0;
            }
            break;
        case 'P': {   // DCH
            int k = std::min(n, ncols - cur_col);
            fix_wide(cur_row, cur_col);
            fix_wide(cur_row, std::min(cur_col + k, ncols - 1));
            std::copy(r + cur_col + k, r + ncols, r + cur_col);
            std::fill(r + ncols - k, r + ncols, blank());
            mark(cur_row);
            break;
        }
        case 'S':     // SU
            scroll_up(top, bottom, n, false);
            break;
        case 'T':     // SD (with more parameters: mouse tracking, ignored)
            if (nparams <= 1) {
                scroll_down(top, bottom, n);
            }
            break;
        case 'X':     // ECH
            clear_cells(cur_row, cur_col, cur_col + n);
            break;
        case 'b':     // REP
            for (int i = 0; i < std::min(n, ncols * nrows); i++) {
                put_char(last_char);
            }
            break;
        case 'c':     // DA: a VT220 with color
            if (param(0, 0) == 0 && reply.size() < MAX_REPLY) {
                reply += "\x1b[?62;22c";
            }
            break;
        case 'd':     // VPA
            move_to(n - 1, cur_col);
            break;
        case 'g':     // TBC
            if (param(0, 0) == 0) {
                tabs[cur_col] = 0;
            } else if (param(0, 0) == 3) {
                std::fill(tabs.begin(), tabs.end(), 0);
            }
            break;
        case 'h':
            set_mode(true);
            break;
        case 'l':
            set_mode(false);
            break;
        case 'm':
            sgr();
            break;
        case 'n':     // DSR
            if (reply.size() < MAX_REPLY) {
                if (param(0, 0) == 5) {
                    reply += "\x1b[0n";
                } else if (param(0, 0) == 6) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "\x1b[%d;%dR", cur_row + 1 - (origin_mode ? top : 0), cur_col + 1);
                    reply += buf;
                }
            }
            break;
        case 'r': {   // DECSTBM
            int t = param(0, 1) - 1;
            int b = std::min(param(1, nrows), nrows) - 1;
            if (t < b) {
                top = t;
                bottom = b;
                move_to(0, 0);
            }
            break;
        }
        case 's':     // SCOSC
            save_cursor(alt_active ? saved_alt : saved);
            break;
        case 'u':     // SCORC
            restore_cursor(alt_active ? saved_alt : saved);
            break;
        case 't':     // Window operations: only "report the text area size"
            if (param(0, 0) == 18 && reply.size() < MAX_REPLY) {
                char buf[32];
                snprintf(buf, sizeof(buf), "\x1b[8;%d;%dt", nrows, ncols);
                reply += buf;
            }
            break;
        default:
            break;
    }
    clamp_cursor();
}

void VTerm::utf8(uint8_t b)
{
    if (utf8_left == 0) {
        if (b >= 0xC2 && b <= 0xDF) {
            utf8_cp = b & 0x1F;
            utf8_left = 1;
            utf8_min = 0x80;
        } else if (b >= 0xE0 && b <= 0xEF) {
            utf8_cp = b & 0x0F;
            utf8_left = 2;
            utf8_min = 0x800;
        } else if (b >= 0xF0 && b <= 0xF4) {
            utf8_cp = b & 0x07;
            utf8_left = 3;
            utf8_min = 0x10000;
        } else {
            put_char(0xFFFD);
        }
        return;
    }
    utf8_cp = (utf8_cp << 6) | (b & 0x3F);
    if (--utf8_left == 0) {
        bool valid = utf8_cp >= utf8_min && utf8_cp <= 0x10FFFF && !(utf8_cp >= 0xD800 && utf8_cp <= 0xDFFF);
        put_char(valid ? utf8_cp : 0xFFFD);
    }
}

void VTerm::byte(uint8_t b)
{
    switch (state) {
        case State::Ground:
            if (utf8_left > 0 && (b & 0xC0) != 0x80) {
                utf8_left = 0;   // Truncated sequence
                put_char(0xFFFD);
            }
            if (utf8_left > 0 || b >= 0x80) {
                utf8(b);
            } else if (b < 0x20 || b == 0x7F) {
                control(b);
            } else {
                put_char(b);
            }
            break;

        case State::Esc:
            if (b < 0x20) {
                control(b);
            } else if (b < 0x30) {
                esc_inter = (char)b;
                state = State::EscInter;
            } else if (b == '[') {
                nparams = 1;
                params[0] = 0;
                csi_private = csi_inter = 0;
                state = State::Csi;
            } else if (b == ']') {
                str_len = 0;
                state = State::Osc;
            } else if (b == 'P' || b == 'X' || b == '^' || b == '_') {
                str_len = 0;
                state = State::Str;   // DCS, SOS, PM, APC: ignored
            } else {
                state = State::Ground;
                esc_dispatch(b);
            }
            break;

        case State::EscInter:
            if (b < 0x20) {
                control(b);
            } else if (b < 0x30) {
                esc_inter = (char)b;
            } else {
                state = State::Ground;
                if (esc_inter == '(' || esc_inter == ')') {
                    charset[esc_inter == ')'] = (char)b;   // Designate G0 / G1
                } else if (esc_inter == '#' && b == '8') {   // DECALN: screen of E
                    for (int r = 0; r < nrows; r++) {
                        for (int c = 0; c < ncols; c++) {
                            row_ptr(r)[c] = {'E', DEFAULT_COLOR, DEFAULT_COLOR, 0};
                        }
                    }
                    mark_all();
                }
            }
            break;

        case State::Csi:
            if (b >= '0' && b <= '9') {
                int& p = params[nparams - 1];
                p = std::min(p * 10 + (b - '0'), MAX_PARAM_VALUE);
            } else if (b == ';' || b == ':') {
                if (nparams < MAX_PARAMS) {
                    params[nparams++] = 0;
                }
            } else if (b >= 0x3C && b <= 0x3F) {
                csi_private = (char)b;
            } else if (b >= 0x20 && b <= 0x2F) {
                csi_inter = (char)b;
            } else if (b >= 0x40 && b <= 0x7E) {
                state = State::Ground;
                csi_dispatch(b);
            } else if (b < 0x20) {
                control(b);
            }
            break;

        case State::Osc:   // Window title etc.: ignored up to BEL or ST
        case State::Str:
            if (b == 0x07 && state == State::Osc) {
                state = State::Ground;
            } else if (b == 0x1B) {
                state = state == State::Osc ? State::OscEsc : State::StrEsc;
            } else if (b == 0x18 || b == 0x1A) {
                state = State::Ground;
            } else {
                str_len++;
            }
            break;

        case State::OscEsc:
        case State::StrEsc:
            if (b == '\\') {
                state = State::Ground;
            } else {
                // Not ST: the string ended without one; this ESC starts a new sequence
                state = State::Esc;
                esc_inter = 0;
                byte(b);
            }
            break;
    }
}

void VTerm::feed(const char* data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        byte((uint8_t)data[i]);
    }
}

std::string VTerm::wheel(bool up, int row, int col) const
{
    if (mouse_mode != 0 && mouse_mode != 9) {   // X10 mode (9) reports only button presses
        row = std::min(std::max(row, 0), nrows - 1) + 1;
        col = std::min(std::max(col, 0), ncols - 1) + 1;
        const int button = up ? 64 : 65;
        char buf[32];
        if (mouse_sgr) {
            snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%dM", button, col, row);
        } else {
            // Legacy encoding: values + 32 in one byte each (columns past 223 can't be sent)
            snprintf(buf, sizeof(buf), "\x1b[M%c%c%c", (char)(32 + button), (char)(32 + std::min(col, 223)),
                     (char)(32 + std::min(row, 223)));
        }
        return buf;
    }
    if (alt_active) {
        return key(up ? Key::Up : Key::Down);
    }
    return "";
}

std::string VTerm::key(Key k) const
{
    switch (k) {
        case Key::Up:       return app_cursor ? "\x1bOA" : "\x1b[A";
        case Key::Down:     return app_cursor ? "\x1bOB" : "\x1b[B";
        case Key::Right:    return app_cursor ? "\x1bOC" : "\x1b[C";
        case Key::Left:     return app_cursor ? "\x1bOD" : "\x1b[D";
        case Key::Home:     return app_cursor ? "\x1bOH" : "\x1b[H";
        case Key::End:      return app_cursor ? "\x1bOF" : "\x1b[F";
        case Key::Insert:   return "\x1b[2~";
        case Key::Delete:   return "\x1b[3~";
        case Key::PageUp:   return "\x1b[5~";
        case Key::PageDown: return "\x1b[6~";
        case Key::F1:       return "\x1bOP";
        case Key::F2:       return "\x1bOQ";
        case Key::F3:       return "\x1bOR";
        case Key::F4:       return "\x1bOS";
        case Key::F5:       return "\x1b[15~";
        case Key::F6:       return "\x1b[17~";
        case Key::F7:       return "\x1b[18~";
        case Key::F8:       return "\x1b[19~";
        case Key::F9:       return "\x1b[20~";
        case Key::F10:      return "\x1b[21~";
        case Key::F11:      return "\x1b[23~";
        case Key::F12:      return "\x1b[24~";
    }
    return "";
}
