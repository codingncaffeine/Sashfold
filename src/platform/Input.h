#pragma once

// The input vocabulary shared by every platform window and the shell: named
// keys, the letter or digit of a shortcut chord, modifiers, and the cursor
// the shell wants shown. IME hooks reserve their place here.

namespace sashfold::platform {

enum class Key {
    None,
    Enter,
    Escape,
    Backspace,
    Delete,
    Tab,
    Space,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    F5,
    F10,
    F12,
    Menu, // the key that asks for a context menu
    Letter, // a character key; KeyEvent::letter holds it
};

struct KeyEvent {
    Key key = Key::None;
    // When key == Key::Letter: 'A'..'Z', '0'..'9', or the ASCII punctuation
    // the key gives with no modifier held ('=' for the key that gives '+'
    // with Shift) — the key a chord names, whatever the layout's level.
    char32_t letter = 0;
    // The character the key gives with the modifiers held, where the window
    // knows it ('+' for Shift and the '=' key on a US layout); 0 otherwise.
    char32_t produced = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

// The keys held while a mouse button went down.
struct Modifiers {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

enum class Cursor {
    Arrow,
    Hand,
    Text,
    // Over the band along a window's edges that resizes it, when the
    // shell draws the frame: the two-headed arrows, by the axis they pull.
    ResizeVertical, // the top or bottom edge
    ResizeHorizontal, // the left or right edge
    ResizeDiagonalDown, // the top-left or bottom-right corner
    ResizeDiagonalUp, // the top-right or bottom-left corner
    // While something is dragged: what a drop here would do with it.
    DragCopy,
    DragMove,
    DragLink,
    NoDrop,
};

}
