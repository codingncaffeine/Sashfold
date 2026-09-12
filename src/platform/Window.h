#pragma once

// The window seam: open a window, pump its input into events, present a
// Bitmap into it. Win32 on Windows, Wayland spoken directly over its socket
// on Linux; AppKit through the Objective-C runtime is not written yet, so
// create() returns null on macOS and every headless mode keeps working —
// the shell itself never sees an OS type.

#include "core/Bitmap.h"
#include "platform/Input.h"

#include <memory>
#include <optional>
#include <string>

namespace sashfold::platform {

struct WindowEvent {
    enum class Kind {
        None,
        Close,
        Resize,
        Scale,
        MouseMove,
        MouseDown,
        MouseUp,
        Wheel,
        Scroll,
        KeyDown,
        Text,
        Preedit,
    };
    Kind kind = Kind::None;
    int x = 0; // mouse position, client pixels
    int y = 0;
    int width = 0; // Resize: the new client size, device pixels
    int height = 0;
    float scale = 1; // Scale: the display's device pixels per CSS px, from now on
    int button = 0; // 1 left, 2 middle, 3 right
    int wheel = 0; // notches; positive rolls away from the user
    // Scroll: the content under (x, y) moves by this many device pixels —
    // positive scroll_y brings what is below into view, as a wheel rolled
    // toward the user does — from a finger or a touchpad, not a wheel.
    int scroll_x = 0;
    int scroll_y = 0;
    KeyEvent key; // KeyDown
    char32_t text = 0; // Text: one code point of typed text
    // Preedit: an input method's composing text (UTF-8), to show at the
    // caret until it is committed as Text or replaced; empty clears it.
    std::string preedit;
};

// An edge or corner of the window, for a resize the reader starts by
// dragging the frame the shell draws.
enum class WindowEdge {
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

class Window {
public:
    // Null when this OS has no window backend yet, or the display cannot
    // be reached (the reason goes to stderr). `icon` is the window's own
    // icon where the OS takes one from the client; null leaves it to the OS.
    static std::unique_ptr<Window> create(std::string const& title, int width, int height,
        Bitmap const* icon = nullptr);
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
    // The client size, in device pixels: what a frame is presented at.
    virtual int width() const = 0;
    virtual int height() const = 0;
    // Device pixels per CSS px on the display the window is on; a change
    // arrives as a Scale event followed by the Resize it implies.
    virtual float scale() const { return 1.0f; }
    // Where text is being typed, in client pixels — the caret's box in the
    // focused field — or nothing when no field has focus: what an input
    // method is told, so it can compose beside the caret. The shell says so
    // whenever it changes.
    virtual void set_text_input(std::optional<Rect> const& caret) { static_cast<void>(caret); }

    // Whether the frame — title bar, its buttons, the resize edges — is the
    // shell's to draw, because the system draws none (a compositor without
    // server-side decorations). The requests below are what the shell's
    // frame asks of the window; the OS handles them where it draws the
    // frame itself, so they do nothing there.
    virtual bool wants_client_decorations() const { return false; }
    virtual void begin_move() { }
    virtual void begin_resize(WindowEdge edge) { static_cast<void>(edge); }
    virtual void minimize() { }
    virtual void toggle_maximize() { }
};

}
