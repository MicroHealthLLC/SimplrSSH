/*
 * Term View (Tab5)
 * The full-screen SSH terminal (term_screen.hpp) on the Tab5's 1280x720 screen: a VTerm drawn
 * in DejaVu Sans Mono 18 px (font_term_mono_18.c, 11 x 22 px cells, about 116 x 31). Each row is
 * drawn as runs of equal colors; box drawing and block characters are drawn as rectangles so
 * frames join up. Only rows that changed are redrawn. Cells, scrollback and the text of each
 * row live in PSRAM.
 */

#include "term_screen.hpp"
#include "board.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

extern "C" const lv_font_t font_term_mono_18;   // font_term_mono_18.c

static const char* TAG = "term_view";
static const int SCROLLBACK_LINES = 1000;
static const uint32_t DEFAULT_FG = 0x00FF00;   // The terminal's green
static const uint32_t DEFAULT_BG = 0x000000;
static const uint32_t CURSOR_COLOR = 0x00FF00;

// Box drawing U+2500-U+257F: line weight of each arm, 2 bits each (up, right << 2, down << 4,
// left << 6; 1 light, 2 heavy, 3 double); 0 = diagonal, drawn from the font
static const uint8_t BOX_ARMS[128] = {
    0x44, 0x88, 0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x14, 0x18, 0x24, 0x28,  // U+2500
    0x50, 0x90, 0x60, 0xA0, 0x05, 0x09, 0x06, 0x0A, 0x41, 0x81, 0x42, 0x82, 0x15, 0x19, 0x16, 0x25,  // U+2510
    0x26, 0x1A, 0x29, 0x2A, 0x51, 0x91, 0x52, 0x61, 0x62, 0x92, 0xA1, 0xA2, 0x54, 0x94, 0x58, 0x98,  // U+2520
    0x64, 0xA4, 0x68, 0xA8, 0x45, 0x85, 0x49, 0x89, 0x46, 0x86, 0x4A, 0x8A, 0x55, 0x95, 0x59, 0x99,  // U+2530
    0x56, 0x65, 0x66, 0x96, 0x5A, 0xA5, 0x69, 0x9A, 0xA9, 0xA6, 0x6A, 0xAA, 0x44, 0x88, 0x11, 0x22,  // U+2540
    0xCC, 0x33, 0x1C, 0x34, 0x3C, 0xD0, 0x70, 0xF0, 0x0D, 0x07, 0x0F, 0xC1, 0x43, 0xC3, 0x1D, 0x37,  // U+2550
    0x3F, 0xD1, 0x73, 0xF3, 0xDC, 0x74, 0xFC, 0xCD, 0x47, 0xCF, 0xDD, 0x77, 0xFF, 0x14, 0x50, 0x41,  // U+2560
    0x05, 0x00, 0x00, 0x00, 0x40, 0x01, 0x04, 0x10, 0x80, 0x02, 0x08, 0x20, 0x48, 0x21, 0x84, 0x12,  // U+2570
};

// Quadrants of U+2596-U+259F (upper left 1, upper right 2, lower left 4, lower right 8)
static const uint8_t QUADRANTS[10] = {4, 8, 1, 1 | 4 | 8, 1 | 8, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 4 | 8};

// 16 ANSI colors, readable on black
static const uint32_t ANSI[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

static uint32_t palette(uint32_t index)
{
    if (index < 16) {
        return ANSI[index];
    }
    if (index < 232) {   // 6 x 6 x 6 color cube
        static const uint8_t LEVEL[6] = {0, 95, 135, 175, 215, 255};
        index -= 16;
        return LEVEL[index / 36] << 16 | LEVEL[index / 6 % 6] << 8 | LEVEL[index % 6];
    }
    uint32_t gray = 8 + (index - 232) * 10;
    return gray << 16 | gray << 8 | gray;
}

static bool drawn_as_shape(uint32_t cp)
{
    return (cp >= 0x2500 && cp <= 0x257F && BOX_ARMS[cp - 0x2500]) || (cp >= 0x2580 && cp <= 0x259F) ||
           (cp >= 0x23BA && cp <= 0x23BD);
}

static size_t put_utf8(char* out, uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

class TermView : public TermScreen
{
public:
    void create(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h) override;
    int cols() const override { return vt ? vt->cols() : 80; }
    int rows() const override { return vt ? vt->rows() : 24; }
    void open() override;
    void close() override;
    bool is_open() const override { return obj && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN); }
    void feed(const char* data, size_t len, std::string& reply) override;
    std::string key(Key k) const override { return vt ? vt->key(k) : ""; }
    bool alt_screen() const override { return vt && vt->alt_screen(); }
    void scroll(int direction) override;
    void scroll_to_newest() override;
    void blink(bool on) override;
    void set_send(Send s) override { send = s; }

private:
    lv_obj_t* obj = nullptr;
    VTerm* vt = nullptr;
    const lv_font_t* font = &font_term_mono_18;
    int32_t cell_w = 0;
    int32_t cell_h = 0;
    int back = 0;                  // Lines scrolled back into the scrollback
    bool blink_on = true;
    int cursor_row = 0;            // Where the cursor was drawn
    char* text = nullptr;          // Per row: the text of its runs, valid while LVGL draws
    size_t text_stride = 0;
    Send send;
    int32_t drag_y = 0;            // Touch: last finger position, and movement not yet scrolled
    int32_t drag_rest = 0;

    struct Colors {
        lv_color_t fg;
        lv_color_t bg;
        bool bg_default;
    };
    Colors colors(const VTerm::Cell& c) const;
    bool has_glyph(uint32_t cp) const;
    void invalidate_rows(int first, int last);
    void scroll_lines(int lines);
    void drag(const lv_point_t& p);
    static void touch_cb(lv_event_t* e);
    void draw(lv_layer_t* layer);
    void draw_row(lv_layer_t* layer, int32_t x0, int32_t y, int row);
    void draw_shape(lv_layer_t* layer, int32_t x, int32_t y, uint32_t cp, lv_color_t color);
    void fill(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t color,
              lv_opa_t opa = LV_OPA_COVER);
    static void draw_cb(lv_event_t* e);
};

void TermView::create(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    cell_w = lv_font_get_glyph_width(font, 'M', 0);
    cell_h = lv_font_get_line_height(font);
    int ncols = std::max<int>(w / cell_w, 20);
    int nrows = std::max<int>(h / cell_h, 5);
    vt = new VTerm(ncols, nrows, SCROLLBACK_LINES);

    text_stride = (size_t)ncols * 5 + 1;   // UTF-8 (up to 4 bytes a cell) and run terminators
    text = (char*)heap_caps_malloc(text_stride * nrows, MALLOC_CAP_SPIRAM);
    if (!text) {
        ESP_LOGE(TAG, "No memory for the terminal");
        delete vt;
        vt = nullptr;
        return;
    }

    obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(DEFAULT_BG), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    // Centered: the grid rarely fills the area exactly
    lv_obj_set_size(obj, ncols * cell_w, nrows * cell_h);
    lv_obj_set_pos(obj, x + (w - ncols * cell_w) / 2, y);
    // Dragging scrolls (touch_cb); sideways swipes still bubble up to the screen (special-keys panel)
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(obj, touch_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(obj, touch_cb, LV_EVENT_PRESSING, this);
}

void TermView::open()
{
    if (!obj) {
        return;
    }
    vt->reset();
    back = 0;
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(obj);
}

void TermView::close()
{
    if (obj) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void TermView::invalidate_rows(int first, int last)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int32_t top = area.y1;
    area.y1 = top + first * cell_h;
    area.y2 = top + (last + 1) * cell_h - 1;
    lv_obj_invalidate_area(obj, &area);
}

void TermView::feed(const char* data, size_t len, std::string& reply)
{
    if (!vt) {
        return;
    }
    vt->feed(data, len);
    reply = vt->take_reply();
    int scrolled = vt->take_scrolled();
    if (!is_open()) {
        vt->clear_dirty();
        return;
    }
    if (back > 0) {
        // Scrolled back: stay on the same lines while output arrives
        back = std::min(back + scrolled, vt->scrollback_count());
        lv_obj_invalidate(obj);
    } else {
        int first = vt->rows(), last = -1;
        for (int r = 0; r < vt->rows(); r++) {
            if (vt->row_dirty(r)) {
                first = std::min(first, r);
                last = r;
            }
        }
        // The cursor's old and new rows
        int now = vt->cursor_row();
        first = std::min({first, cursor_row, now});
        last = std::max({last, cursor_row, now});
        invalidate_rows(first, last);
    }
    vt->clear_dirty();
}

void TermView::scroll_lines(int lines)
{
    int target = std::min(std::max(back + lines, 0), vt->scrollback_count());
    if (target != back) {
        back = target;
        lv_obj_invalidate(obj);
    }
}

void TermView::scroll(int direction)
{
    if (vt && !vt->alt_screen()) {
        int step = std::max(vt->rows() / 3, 1);
        scroll_lines(direction > 0 ? step : -step);
    }
}

// One line per row height dragged, finger down = older text, like scrolling a page. A program
// that reads the mouse gets wheel events, other full-screen programs arrow keys; at the shell
// the view scrolls back through the scrollback.
void TermView::drag(const lv_point_t& p)
{
    drag_rest += p.y - drag_y;
    drag_y = p.y;
    int lines = drag_rest / cell_h;
    if (lines == 0) {
        return;
    }
    drag_rest -= lines * cell_h;
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    std::string seq = vt->wheel(lines > 0, (p.y - c.y1) / cell_h, (p.x - c.x1) / cell_w);
    if (seq.empty()) {
        scroll_lines(lines);
    } else if (send) {
        for (int i = 0; i < std::abs(lines); i++) {
            send(seq.data(), seq.size());
        }
    }
}

void TermView::touch_cb(lv_event_t* e)
{
    TermView* view = (TermView*)lv_event_get_user_data(e);
    lv_indev_t* indev = lv_indev_active();
    if (!view->vt || !indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        view->drag_y = p.y;
        view->drag_rest = 0;
    } else {
        view->drag(p);
    }
}

void TermView::scroll_to_newest()
{
    if (back != 0) {
        back = 0;
        lv_obj_invalidate(obj);
    }
}

void TermView::blink(bool on)
{
    blink_on = on;
    if (is_open() && vt->cursor_visible()) {
        invalidate_rows(vt->cursor_row(), vt->cursor_row());
    }
}

TermView::Colors TermView::colors(const VTerm::Cell& c) const
{
    auto resolve = [](uint32_t color, bool fg, bool bold) -> uint32_t {
        if (color == VTerm::DEFAULT_COLOR) {
            return fg ? DEFAULT_FG : DEFAULT_BG;
        }
        if (color & VTerm::RGB) {
            return color & 0xFFFFFF;
        }
        uint32_t index = color & 0xFF;
        return palette(fg && bold && index < 8 ? index + 8 : index);   // Bold: the bright color
    };
    bool bold = c.attr & VTerm::BOLD;
    uint32_t fg = resolve(c.fg, true, bold);
    uint32_t bg = resolve(c.bg, false, false);
    bool bg_default = c.bg == VTerm::DEFAULT_COLOR;
    if (c.attr & VTerm::REVERSE) {
        std::swap(fg, bg);
        bg_default = false;
    }
    Colors out = {lv_color_hex(fg), lv_color_hex(bg), bg_default};
    if (c.attr & VTerm::DIM) {
        out.fg = lv_color_mix(out.fg, out.bg, LV_OPA_60);
    }
    return out;
}

bool TermView::has_glyph(uint32_t cp) const
{
    if (cp >= 0x20 && cp < 0x7F) {
        return true;
    }
    lv_font_glyph_dsc_t g;
    return lv_font_get_glyph_dsc(font, &g, cp, 0);
}

void TermView::fill(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t color,
                    lv_opa_t opa)
{
    lv_draw_fill_dsc_t dsc;
    lv_draw_fill_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = opa;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_fill(layer, &dsc, &a);
}

// Box drawing, blocks and scan lines fill their cell edge to edge, so they join their neighbors
void TermView::draw_shape(lv_layer_t* layer, int32_t x, int32_t y, uint32_t cp, lv_color_t color)
{
    const int32_t w = cell_w, h = cell_h;
    const int32_t x2 = x + w - 1, y2 = y + h - 1;

    if (cp >= 0x23BA && cp <= 0x23BD) {   // Scan lines 1, 3, 7, 9
        static const int TENTHS[4] = {1, 3, 7, 9};
        int32_t ly = y + h * TENTHS[cp - 0x23BA] / 10;
        fill(layer, x, ly, x2, ly + 1, color);
        return;
    }
    if (cp >= 0x2580) {
        if (cp == 0x2580) {
            fill(layer, x, y, x2, y + h / 2 - 1, color);
        } else if (cp <= 0x2588) {                      // Lower 1/8 ... full
            fill(layer, x, y2 - h * (int32_t)(cp - 0x2580) / 8 + 1, x2, y2, color);
        } else if (cp <= 0x258F) {                      // Left 7/8 ... 1/8
            fill(layer, x, y, x + w * (int32_t)(0x2590 - cp) / 8 - 1, y2, color);
        } else if (cp == 0x2590) {
            fill(layer, x + w / 2, y, x2, y2, color);
        } else if (cp <= 0x2593) {                      // Shades
            fill(layer, x, y, x2, y2, color, (lv_opa_t)(64 * (cp - 0x2590)));
        } else if (cp == 0x2594) {
            fill(layer, x, y, x2, y + h / 8 - 1, color);
        } else if (cp == 0x2595) {
            fill(layer, x2 - w / 8 + 1, y, x2, y2, color);
        } else {
            uint8_t q = QUADRANTS[cp - 0x2596];
            int32_t mx = x + w / 2, my = y + h / 2;
            if (q & 1) fill(layer, x, y, mx - 1, my - 1, color);
            if (q & 2) fill(layer, mx, y, x2, my - 1, color);
            if (q & 4) fill(layer, x, my, mx - 1, y2, color);
            if (q & 8) fill(layer, mx, my, x2, y2, color);
        }
        return;
    }

    // Lines: light 2 px, heavy twice that, double two light lines
    const uint8_t arms = BOX_ARMS[cp - 0x2500];
    const int32_t t = std::max<int32_t>((w + 4) / 6, 1);
    const int32_t cx = x + w / 2 - t / 2, cy = y + h / 2 - t / 2;   // Top-left of the center square
    for (int arm = 0; arm < 4; arm++) {
        int weight = arms >> (arm * 2) & 3;
        if (!weight) {
            continue;
        }
        int32_t th = weight == 2 ? t * 2 : t;
        int32_t shift = weight == 2 ? t / 2 : 0;
        // Double lines: two light lines t apart on each side of the center line
        int lines = weight == 3 ? 2 : 1;
        for (int i = 0; i < lines; i++) {
            int32_t off = weight == 3 ? (i ? t : -t) : -shift;
            switch (arm) {
                case 0: fill(layer, cx + off, y, cx + off + th - 1, cy + t - 1 + (weight == 3 ? t : 0), color); break;
                case 1: fill(layer, cx - (weight == 3 ? t : 0), cy + off, x2, cy + off + th - 1, color); break;
                case 2: fill(layer, cx + off, cy - (weight == 3 ? t : 0), cx + off + th - 1, y2, color); break;
                case 3: fill(layer, x, cy + off, cx + t - 1 + (weight == 3 ? t : 0), cy + off + th - 1, color); break;
            }
        }
    }
}

void TermView::draw_row(lv_layer_t* layer, int32_t x0, int32_t y, int row)
{
    const VTerm::Cell* cells = vt->view_row(row, back);
    const int n = vt->cols();
    const int32_t y2 = y + cell_h - 1;
    const bool cursor_here = back == 0 && blink_on && vt->cursor_visible() && row == vt->cursor_row();
    const int cursor_col = vt->cursor_col();

    // Backgrounds: runs of one color; the view's own black needs nothing
    for (int c = 0; c < n;) {
        Colors k = colors(cells[c]);
        int end = c + 1;
        while (end < n && lv_color_eq(colors(cells[end]).bg, k.bg)) {
            end++;
        }
        if (!k.bg_default) {
            fill(layer, x0 + c * cell_w, y, x0 + end * cell_w - 1, y2, k.bg);
        }
        c = end;
    }
    if (cursor_here) {
        fill(layer, x0 + cursor_col * cell_w, y, x0 + (cursor_col + 1) * cell_w - 1, y2, lv_color_hex(CURSOR_COLOR));
    }

    // Text: runs of one color and decoration, drawn as one label each. Every glyph of the font is
    // one cell wide; characters it lacks are shown as U+FFFD so the columns stay aligned.
    char* buf = text + text_stride * row;
    size_t used = 0;
    for (int c = 0; c < n;) {
        const VTerm::Cell& first = cells[c];
        Colors k = colors(first);
        bool on_cursor = cursor_here && c == cursor_col;
        lv_color_t fg = on_cursor ? k.bg : k.fg;
        if (on_cursor && k.bg_default) {
            fg = lv_color_hex(DEFAULT_BG);
        }
        if (drawn_as_shape(first.ch)) {
            if (!(first.attr & VTerm::HIDDEN)) {
                draw_shape(layer, x0 + c * cell_w, y, first.ch, fg);
            }
            c++;
            continue;
        }
        const uint16_t decor = first.attr & (VTerm::UNDERLINE | VTerm::STRIKE | VTerm::HIDDEN);
        size_t start = used;
        int end = c;
        while (end < n) {
            const VTerm::Cell& cell = cells[end];
            bool cell_cursor = cursor_here && end == cursor_col;
            if (end > c && (cell_cursor || on_cursor || drawn_as_shape(cell.ch) ||
                            (cell.attr & (VTerm::UNDERLINE | VTerm::STRIKE | VTerm::HIDDEN)) != decor ||
                            !lv_color_eq(colors(cell).fg, k.fg))) {
                break;
            }
            uint32_t cp = cell.ch;
            if (cp == VTerm::WIDE_TAIL) {
                cp = ' ';   // The right half of a wide character: keeps the next cell in place
            } else if (!has_glyph(cp)) {
                cp = 0xFFFD;
            }
            used += put_utf8(buf + used, cp);
            end++;
        }
        buf[used++] = '\0';
        if (!(decor & VTerm::HIDDEN)) {
            lv_draw_label_dsc_t dsc;
            lv_draw_label_dsc_init(&dsc);
            dsc.font = font;
            dsc.color = fg;
            dsc.text = buf + start;   // Valid until the next feed(), which needs the display lock
            lv_area_t a = {x0 + c * cell_w, y, x0 + (end + 1) * cell_w, y2};
            lv_draw_label(layer, &dsc, &a);
            if (decor & VTerm::UNDERLINE) {
                fill(layer, a.x1, y2 - 2, x0 + end * cell_w - 1, y2 - 1, fg);
            }
            if (decor & VTerm::STRIKE) {
                fill(layer, a.x1, y + cell_h / 2, x0 + end * cell_w - 1, y + cell_h / 2, fg);
            }
        }
        c = end;
    }
}

void TermView::draw(lv_layer_t* layer)
{
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    const lv_area_t& clip = layer->_clip_area;
    int first = std::max<int>(0, (clip.y1 - coords.y1) / cell_h);
    int last = std::min<int>(vt->rows() - 1, (clip.y2 - coords.y1) / cell_h);
    for (int r = first; r <= last; r++) {
        draw_row(layer, coords.x1, coords.y1 + r * cell_h, r);
    }
    cursor_row = vt->cursor_row();

    if (back > 0) {   // Scrolled back: how far, top right
        static char marker[24];
        snprintf(marker, sizeof(marker), " -%d ", back);
        lv_draw_label_dsc_t dsc;
        lv_draw_label_dsc_init(&dsc);
        dsc.font = font;
        dsc.color = lv_color_black();
        dsc.text = marker;
        int32_t w = (int32_t)strlen(marker) * cell_w;
        lv_area_t a = {coords.x2 - w + 1, coords.y1, coords.x2, coords.y1 + cell_h - 1};
        fill(layer, a.x1, a.y1, a.x2, a.y2, lv_color_hex(0xFFFF00));
        lv_draw_label(layer, &dsc, &a);
    }
}

void TermView::draw_cb(lv_event_t* e)
{
    TermView* view = (TermView*)lv_event_get_user_data(e);
    if (view->vt) {
        view->draw(lv_event_get_layer(e));
    }
}

TermScreen* tab5_term_view()
{
    static TermView view;
    return &view;
}
