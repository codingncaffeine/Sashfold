#pragma once

// The sheet a page's Web Authentication request shows: anchored under the
// toolbar, over the page of the tab that asked, and modal to that tab
// alone (as the browsers' own are). It says which site asks (the RP ID,
// never text the page chose), and what the ceremony needs of the reader
// now: insert and touch a key, touch it again, its PIN (or a new one, with
// the key's minimum length and the tries left), which account, that the
// key is already registered or not registered, that the site would see
// the key's make and model. Cancel is always there; Escape cancels.
//
// Keyboard: Tab and Shift+Tab go round the fields, the account list and
// the buttons; the arrows move in the list; Enter presses the button with
// the focus, or the default one from a field; Backspace edits a field.
// Drawn in the popup tokens of the theme, as the menus and the window's
// own file chooser are.

#include "auth/Client.h"
#include "core/Bitmap.h"
#include "platform/Input.h"
#include "ui/SecurityKeys.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::ui {

struct KeySheetLook {
    Color background;
    Color border;
    Color text;
    Color muted;
    Color highlight;
    Color highlight_text;
    Color accent;
    Color field_background;
    Color danger;
    float font_size = 13;
    float scale = 1;
};

// What the reader did, for the window to tell the requests.
struct KeySheetAction {
    enum class Kind : std::uint8_t { None, Cancel, Pin, Account, Consent };
    Kind kind = Kind::None;
    auth::PinAnswer pin;
    std::size_t account = 0;
    bool allow = false;
};

class KeySheet {
public:
    // Brought to `state`: a request or a question new to it empties its
    // fields and puts the focus on the first thing to answer.
    void update(KeySheetState const& state);
    std::uint64_t id() const { return m_state.id; }

    // Laid out under the toolbar: its left edge at `anchor_x` (the address
    // field's), inside `area` (the window's content, device px).
    void layout(Rect const& area, int anchor_x, KeySheetLook const& look);
    void paint(Bitmap& target) const;
    Rect bounds() const { return m_box; }

    void mouse_move(int x, int y);
    KeySheetAction mouse_down(int x, int y);
    // A key the sheet takes (true), with what it did.
    bool key(platform::KeyEvent const& key, KeySheetAction& action);
    void text(char32_t code_point);

    // For scripts and tests: what it shows ("pin-retry example.org 2"),
    // where the keyboard is ("field 0", "account 1", "button cancel"),
    // the fields as their lengths, a button's box by its name.
    std::string shows() const;
    std::string focus_name() const;
    std::optional<Rect> button(std::string_view name) const;
    std::optional<Rect> account_row(std::size_t index) const;

private:
    enum class Button : std::uint8_t { Cancel, Continue, Close, Block, Allow };
    struct Focusable {
        enum class Kind : std::uint8_t { Field, Accounts, Button } kind;
        std::size_t index = 0;
    };

    static std::u32string button_label(Button button);
    std::vector<Button> buttons() const;
    std::vector<Focusable> focusables() const;
    std::size_t field_count() const;
    std::vector<std::u32string> field_labels() const;
    std::u32string message() const;
    KeySheetAction press(Button button);
    KeySheetAction submit_pin();
    void move_focus(int by);

    KeySheetState m_state;
    std::uint64_t m_question = ~std::uint64_t { 0 };
    std::vector<std::u32string> m_fields;
    std::u32string m_error;
    std::size_t m_focus = 0; // into focusables()
    std::size_t m_account = 0; // the row the keys are on
    std::optional<Button> m_hover;
    std::optional<std::size_t> m_hover_account;

    KeySheetLook m_look;
    Rect m_box;
    Rect m_icon;
    Rect m_title;
    Rect m_subtitle;
    std::vector<std::u32string> m_lines; // the message, wrapped
    int m_message_y = 0;
    int m_line_height = 0;
    std::vector<Rect> m_field_boxes;
    std::vector<Rect> m_label_boxes;
    std::vector<Rect> m_account_boxes;
    Rect m_error_box;
    std::u32string m_error_note; // the error, or the tries left, as laid out
    std::vector<std::pair<Button, Rect>> m_button_boxes;
};

}
