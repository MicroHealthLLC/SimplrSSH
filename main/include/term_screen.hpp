/*
 * Term Screen
 * A full-screen terminal for SSH sessions, so full-screen (TUI) programs work: the server's
 * output is drawn as a grid of character cells (VTerm) and every key goes straight to the
 * server. Boards that have one (board::term_screen(): the Tab5) show it while SSH is
 * connected; the others (the T-Deck) show SSH output as scrolling text and send a typed
 * line with Enter.
 */

#ifndef TERM_SCREEN_HPP
#define TERM_SCREEN_HPP

#include "lvgl.h"
#include "vterm.hpp"
#include <cstddef>
#include <functional>
#include <string>

class TermScreen
{
public:
    using Key = VTerm::Key;
    virtual ~TermScreen() = default;

    // Builds the (hidden) view over w x h pixels at x, y of `parent`. All calls below are made
    // with the display lock held.
    virtual void create(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h) = 0;
    virtual int cols() const = 0;                 // Size reported to the SSH server
    virtual int rows() const = 0;
    // Where dragging on the screen sends scroll input for a full-screen program (wheel events or
    // arrows); called from LVGL's task
    using Send = std::function<void(const char* data, size_t len)>;
    virtual void set_send(Send send) = 0;

    virtual void open() = 0;                      // New session: blank screen, shown
    virtual void close() = 0;                     // Hidden
    virtual bool is_open() const = 0;

    // Output from the server; `reply` gets the terminal's answers to queries
    virtual void feed(const char* data, size_t len, std::string& reply) = 0;
    virtual std::string key(Key k) const = 0;     // Bytes for a key in the current modes
    virtual bool alt_screen() const = 0;          // A full-screen program is running

    virtual void scroll(int direction) = 0;       // Keys: a third of a screen, > 0 older
                                                  // (a full-screen program: see key()); dragging
                                                  // scrolls by the line, see set_send()
    virtual void scroll_to_newest() = 0;
    virtual void blink(bool on) = 0;              // Cursor blink phase
};

#endif
