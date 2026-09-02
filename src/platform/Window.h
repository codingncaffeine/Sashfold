#pragma once

// The window seam: open a window, pump its input into events, present a
// Bitmap into it. Windows first; Wayland spoken directly over its socket and
// AppKit through the Objective-C runtime are not written yet, so create()
// returns null on those OSes and every headless mode keeps working — the
// shell itself never sees an OS type.

#include "core/Bitmap.h"
#include "platform/Input.h"

#include <memory>
#include <string>

namespace sashfold::platform {

struct WindowEvent {
    enum class Kind {
        None,
        Close,
        Resize,
        MouseMove,
        MouseDown,
        MouseUp,
        Wheel,
        KeyDown,
        Text,
    };
    Kind kind = Kind::None;
    int x = 0; // mouse position, client pixels
    int y = 0;
    int width = 0; // Resize: the new client size
    int height = 0;
    int button = 0; // 1 left, 2 middle, 3 right
    int wheel = 0; // notches; positive rolls away from the user
    KeyEvent key; // KeyDown
    char32_t text = 0; // Text: one code point of typed text
};

class Window {
public:
    // Null when this OS has no window backend yet.
    static std::unique_ptr<Window> create(std::string const& title, int width, int height);
    virtual ~Window() = default;

    // Non-blocking: dequeues one event; false when none is pending.
    virtual bool poll(WindowEvent& event) = 0;
    // Blocks until input arrives or the timeout (ms; negative = forever).
    virtual void wait(int timeout_ms) = 0;
    // Shows the frame. It normally matches the client size for a 1:1 blit;
    // between a resize and the next paint it is scaled.
    virtual void present(Bitmap const& frame) = 0;
    virtual void set_title(std::string const& title) = 0;
    virtual void set_cursor(Cursor cursor) = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
};

}
