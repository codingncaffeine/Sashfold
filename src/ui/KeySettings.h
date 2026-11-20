#pragma once

// The window's "Security keys" settings, opened from the main menu: a sheet
// in the drawn language of a page's key sheet (ui/KeySheet.h), anchored
// under the toolbar at the window's right, over the page and modal to the
// window's content. The reader plugs in and touches a key; then sets a PIN
// (a key without one) or changes it, sees how many passkeys it holds and
// has room for, lists them by site with their user and display names, and
// deletes one after a confirmation. Every error the key can give is said in
// plain words: a wrong PIN with the tries left, a blocked PIN, too many
// wrong in a row, the key pulled out, a key that cannot list its passkeys.
// Resetting a key is not offered.
//
// The key is spoken to by auth::KeyManagement on threads of its own; the
// sheet reads its published state and gives it the reader's commands, and
// nothing here waits on a device. A closed sheet's session is cancelled and
// kept until its threads are done (asked each tick, never waited for),
// so that closing never makes the window wait.
//
// Keyboard: Escape closes the sheet from anywhere; Tab and Shift+Tab go
// round the fields, the list and the buttons; the arrows move in the list;
// Enter or Space presses the focused button (Enter in a field goes to the
// next field, then submits); Delete on the list asks to delete that passkey.

#include "auth/KeyManagement.h"
#include "core/Bitmap.h"
#include "platform/Input.h"
#include "ui/KeySheet.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::ui {

class KeySettings {
public:
    KeySettings();
    ~KeySettings();
    KeySettings(KeySettings const&) = delete;
    KeySettings& operator=(KeySettings const&) = delete;

    // Where keys are found and how the window is woken from another
    // thread. A new transport ends an open sheet: its key is another's.
    void set_transport(std::shared_ptr<auth::Transport> transport, std::function<void()> wake);

    // Opens the sheet and starts looking for a key (again, if it was open).
    void open();
    // Closes it; its session ends on its own threads.
    void close();
    bool shown() const { return m_session != nullptr; }

    // The window's tick: the state of the session taken up, ended sessions
    // let go. True when the sheet needs to be drawn again.
    bool hear();

    void layout(Rect const& area, int anchor_right, KeySheetLook const& look);
    void paint(Bitmap& target) const;
    Rect bounds() const { return m_box; }

    void mouse_move(int x, int y);
    void mouse_down(int x, int y);
    // A key the sheet takes (true).
    bool key(platform::KeyEvent const& key);
    void text(char32_t code_point);

    // For scripts and tests: what it shows ("home | pin set | passkeys
    // yes"), where the keyboard is, a button's or a list row's box.
    std::string shows() const;
    std::string focus_name() const;
    std::optional<Rect> button(std::string_view name) const;
    std::optional<Rect> row(std::size_t index) const;

    enum class View : std::uint8_t {
        Waiting, // insert, touch, or the chosen key being read
        Working, // a command is with the key
        Fingerprint, // the key's own user verification waits
        Home,
        Unlock, // the PIN, to see the passkeys
        SetPin,
        ChangePin,
        Passkeys,
        ConfirmDelete,
        Ended,
    };
    enum class Button : std::uint8_t { Cancel, Close, Back, Continue, Save, SetPin, ChangePin, Passkeys, Delete, ConfirmDelete };

private:
    struct Focusable {
        enum class Kind : std::uint8_t { Field, List, Button } kind;
        std::size_t index = 0;
    };
    struct Signal {
        std::atomic<std::uint64_t> generation { 0 };
        std::function<void()> wake;
    };

    View view() const;
    void go(View form);
    void take_outcome();
    std::size_t field_count() const;
    std::vector<std::u32string> field_labels() const;
    std::vector<Button> buttons() const;
    std::vector<Focusable> focusables() const;
    std::u32string message() const;
    void press(Button button);
    void submit();
    void move_focus(int by);
    void focus_default();
    void reap();

    std::shared_ptr<auth::Transport> m_transport;
    std::function<void()> m_wake;
    std::shared_ptr<Signal> m_signal;
    std::unique_ptr<auth::KeyManagement> m_session;
    std::vector<std::unique_ptr<auth::KeyManagement>> m_ending;
    std::uint64_t m_heard = ~std::uint64_t { 0 };

    auth::KeyManagementState m_state;
    View m_form = View::Home; // the reader's place once a key is chosen
    View m_shown = View::Waiting; // last laid out, to reset the focus on a change
    std::uint64_t m_commands_seen = 0;
    std::vector<std::u32string> m_fields;
    std::u32string m_error; // under the fields, in the danger color
    std::u32string m_note; // a word on what just happened
    std::size_t m_focus = 0;
    std::size_t m_selected = 0; // the passkey the list is on
    std::optional<auth::Passkey> m_deleting;
    std::optional<Button> m_hover;
    std::optional<std::size_t> m_hover_row;

    KeySheetLook m_look;
    Rect m_box;
    Rect m_icon;
    Rect m_title;
    Rect m_subtitle;
    std::vector<std::u32string> m_lines;
    int m_message_y = 0;
    int m_line_height = 0;
    std::vector<Rect> m_field_boxes;
    std::vector<Rect> m_label_boxes;
    std::vector<Rect> m_row_boxes;
    std::vector<std::u32string> m_note_lines; // the error or the note, wrapped
    int m_note_y = 0;
    bool m_note_danger = false;
    std::vector<std::pair<Button, Rect>> m_button_boxes;
};

}
