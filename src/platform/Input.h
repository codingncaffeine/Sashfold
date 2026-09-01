#pragma once

// The input vocabulary shared by every platform window and the shell: named
// keys, the letter or digit of a shortcut chord, modifiers, and the cursor
// the shell wants shown. IME hooks reserve their place here (plan §5.1).

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
    Letter, // a letter or digit key; KeyEvent::letter holds it, uppercase
};

struct KeyEvent {
    Key key = Key::None;
    char32_t letter = 0; // 'A'..'Z' or '0'..'9' when key == Key::Letter
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

enum class Cursor {
    Arrow,
    Hand,
    Text,
};

}
