/*
 * VTerm
 * The screen of an xterm-compatible terminal, for full-screen (TUI) programs over SSH on the
 * Tab5: a grid of cells with colors and attributes, the cursor, scroll regions, the alternate
 * screen and a bounded scrollback. Data from the server is untrusted: every parameter is
 * clamped and sequences are length-limited. No ESP-IDF or LVGL headers: tested on the host.
 *
 * The cell arrays are allocated once (>16 KB each, so the ESP heap puts them in PSRAM) and
 * reused; feed() never allocates.
 */

#ifndef VTERM_HPP
#define VTERM_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class VTerm
{
public:
    struct Cell {
        uint32_t ch;     // Unicode code point; WIDE_TAIL for the right half of a wide character
        uint32_t fg;     // DEFAULT_COLOR, PALETTE | index (0-255) or RGB | 0xRRGGBB
        uint32_t bg;
        uint16_t attr;   // Attr bits
    };
    enum Attr : uint16_t {
        BOLD = 1, DIM = 2, ITALIC = 4, UNDERLINE = 8, BLINK = 16, REVERSE = 32, HIDDEN = 64, STRIKE = 128,
    };
    static const uint32_t DEFAULT_COLOR = 0;
    static const uint32_t PALETTE = 0x1000000;
    static const uint32_t RGB = 0x2000000;
    static const uint32_t WIDE_TAIL = 0x110000;

    // Keys that send escape sequences (key())
    enum class Key {
        Up, Down, Right, Left, Home, End, Insert, Delete, PageUp, PageDown,
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    };

    VTerm(int cols, int rows, int scrollback_lines);

    void reset();                              // Power-on state, scrollback cleared
    void feed(const char* data, size_t len);   // Output from the server
    std::string take_reply();                  // Answers to queries (cursor position, device type)
    std::string key(Key k) const;              // Bytes to send for a key in the current modes
    // Scrolling one step over cell (row, col), up = toward older text: a mouse wheel event if the
    // program asked for mouse reports, else Up/Down arrows in a full-screen program (as desktop
    // terminals do), else "" (the caller scrolls back through the scrollback)
    std::string wheel(bool up, int row, int col) const;

    int cols() const { return ncols; }
    int rows() const { return nrows; }

    // Row `row` of the screen as seen `back` lines up into the scrollback (0 = live screen).
    // The alternate screen (full-screen programs) has no scrollback.
    const Cell* view_row(int row, int back) const;
    int scrollback_count() const { return alt_active ? 0 : sb_count; }

    int cursor_row() const { return cur_row; }
    int cursor_col() const { return cur_col; }
    bool cursor_visible() const { return show_cursor; }
    bool alt_screen() const { return alt_active; }

    // Change tracking for redraws: rows changed since clear_dirty(), and lines that scrolled
    // into the scrollback (so a view scrolled back can stay on the same text)
    bool row_dirty(int row) const { return dirty[row] != 0; }
    void clear_dirty();
    int take_scrolled();

    static int char_width(uint32_t cp);        // 0 (combining), 1 or 2 columns

private:
    int ncols, nrows, sb_lines;
    std::vector<Cell> main_cells, alt_cells, sb_cells;
    std::vector<uint8_t> dirty;
    std::vector<uint8_t> tabs;
    Cell* screen;                    // main_cells or alt_cells
    bool alt_active = false;
    int sb_head = 0;                 // Ring: index of the oldest scrollback line
    int sb_count = 0;
    int scrolled = 0;

    // Cursor and its saved copy (DECSC / DECRC, and 1048/1049)
    struct Saved {
        int row = 0, col = 0;
        Cell pen = {' ', DEFAULT_COLOR, DEFAULT_COLOR, 0};
        bool origin = false;
        bool wrap_pending = false;
        char g[2] = {'B', 'B'};
        int gl = 0;
    };
    int cur_row = 0, cur_col = 0;
    bool wrap_pending = false;
    Cell pen = {' ', DEFAULT_COLOR, DEFAULT_COLOR, 0};   // Colors and attributes for new text
    char charset[2] = {'B', 'B'};    // G0, G1: 'B' ASCII, '0' DEC line drawing
    int gl = 0;                      // Charset in use (SO / SI)
    Saved saved, saved_alt;

    // Modes
    bool show_cursor = true;
    bool autowrap = true;
    bool origin_mode = false;
    bool insert_mode = false;
    bool newline_mode = false;
    bool app_cursor = false;
    int mouse_mode = 0;              // 0 off, else 9 / 1000 / 1002 / 1003: the program wants mouse reports
    bool mouse_sgr = false;          // 1006: SGR encoding of mouse reports
    int top = 0, bottom = 0;         // Scroll region (inclusive)
    uint32_t last_char = ' ';        // For REP

    // Parser
    enum class State { Ground, Esc, EscInter, Csi, Osc, OscEsc, Str, StrEsc };
    State state = State::Ground;
    static const int MAX_PARAMS = 16;
    int params[MAX_PARAMS] = {};
    int nparams = 0;
    bool param_started = false;
    char csi_private = 0;            // '?', '>', '<', '=' or 0
    char csi_inter = 0;              // ' ', '!', '"', '$' ... or 0
    char esc_inter = 0;
    size_t str_len = 0;              // Length of an ignored OSC / DCS string
    uint32_t utf8_cp = 0;
    uint32_t utf8_min = 0;           // Smallest code point for the sequence length (no overlong forms)
    int utf8_left = 0;
    std::string reply;

    Cell* row_ptr(int row) { return &screen[(size_t)row * ncols]; }
    Cell blank() const;
    void mark(int row) { dirty[row] = 1; }
    void mark_all();
    void clear_cells(int row, int from, int to);   // [from, to) with the current background
    void fix_wide(int row, int col);               // Writing over half of a wide character
    void put_char(uint32_t cp);
    void control(uint8_t c);
    void line_feed();
    void reverse_index();
    void scroll_up(int from, int to, int n, bool to_scrollback);
    void scroll_down(int from, int to, int n);
    void push_scrollback(const Cell* line);
    void move_to(int row, int col);                // Absolute; honors origin mode
    void clamp_cursor();
    void save_cursor(Saved& s) const;
    void restore_cursor(const Saved& s);
    void switch_screen(bool alt, bool clear);
    void esc_dispatch(uint8_t c);
    void csi_dispatch(uint8_t c);
    void set_mode(bool on);
    void sgr();
    int param(int i, int def) const;
    void utf8(uint8_t b);
    void byte(uint8_t b);
};

#endif
