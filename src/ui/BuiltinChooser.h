#pragma once

// A file chooser the window draws itself, for a desktop that has none to
// show (no file chooser portal, or a system with no chooser written): what a
// file input opens there, as a dialog over the page, laid out as the
// desktops' own are — the places down the left (the home folder and its
// usual folders, the whole disk), the folder's path along the top, its
// folders and then its files below with their sizes and dates, the page's
// accept filter at the foot beside Cancel and Open. Folders are read on a
// thread of the chooser's: the window never waits for a disk.
//
// Mouse: a click chooses a row, Ctrl adds one (Shift a run) where the input
// takes several, a double click opens a folder or takes a file, the wheel
// scrolls. Keys: the arrows, Home, End, Page Up and Down move; Enter opens
// or takes; Backspace or Alt+Up goes up a folder; Escape cancels; Ctrl+A
// chooses every file; Ctrl+H shows the hidden ones; Ctrl+L, or typing a
// "/", edits the path, and letters typed jump to the first name they begin.

#include "core/Bitmap.h"
#include "platform/FileChooser.h"
#include "platform/Input.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sashfold::ui {

// The colors and sizes it is drawn in: the theme's popup tokens, as the
// menus are.
struct ChooserLook {
    Color scrim = Color::rgba(0, 0, 0, 0x66);
    Color background;
    Color border;
    Color text;
    Color muted;
    Color disabled;
    Color highlight;
    Color highlight_text;
    Color accent;
    Color field_background;
    float font_size = 13;
    float scale = 1;
};

class BuiltinChooser final : public platform::FileChooser {
public:
    // `wake` is called from the reading thread when a folder has been read.
    BuiltinChooser(platform::FileChoice choice, std::string start_folder, std::function<void()> wake);
    ~BuiltinChooser() override;
    std::optional<std::vector<std::string>> take_result() override;
    bool unavailable() const override { return false; }

    // A folder read in the meantime is taken in: true when it was (draw again).
    bool poll();
    // Laid out over `area` (the window's content, device px).
    void layout(Rect const& area, ChooserLook const& look);
    void paint(Bitmap& target) const;

    // Input in the window's device px. Every event is the chooser's while it
    // is open (it is modal).
    void mouse_move(int x, int y);
    void mouse_down(int x, int y, int button, platform::Modifiers const& modifiers);
    void wheel(int x, int y, int notches);
    void key(platform::KeyEvent const& key);
    void text(char32_t code_point);

    // For scripts and tests: the folder shown, and its rows as shown (a
    // folder's name ends with "/", a chosen row is in [brackets]).
    std::string folder() const { return m_folder; }
    std::string rows_text() const;
    bool reading() const;

private:
    struct Entry {
        std::string name;
        bool folder = false;
        std::uint64_t size = 0;
        std::int64_t modified = 0; // seconds since the epoch
    };
    struct Place {
        std::string label;
        std::string path;
    };
    struct Listing; // shared with the reading thread

    void open(std::string folder);
    void refilter();
    bool shown(Entry const& entry) const;
    void choose_row(std::size_t row, bool add, bool range);
    void activate(std::size_t row);
    void accept();
    void finish(std::vector<std::string> paths);
    void go_up();
    void reveal(std::size_t row);
    std::size_t rows_visible() const;
    std::optional<std::size_t> row_at(int x, int y) const;
    void path_key(platform::KeyEvent const& key);

    platform::FileChoice m_choice;
    std::function<void()> m_wake;
    std::shared_ptr<Listing> m_listing;
    std::vector<std::thread> m_readers;
    std::string m_folder;
    std::string m_error;
    std::vector<Entry> m_entries; // of the folder, as read
    std::vector<std::size_t> m_rows; // the entries shown, in order
    std::vector<bool> m_chosen; // by row
    std::optional<std::size_t> m_anchor; // where a Shift run starts
    std::optional<std::size_t> m_cursor; // the row the keys are on
    std::size_t m_scroll = 0; // the first row shown
    std::vector<Place> m_places;
    std::size_t m_filter = 0; // into the choice's filters; one past them: every file
    bool m_hidden = false;
    bool m_editing_path = false;
    std::u32string m_path_text;
    std::u32string m_typed; // what letters typed have been, for jumping to a name
    std::chrono::steady_clock::time_point m_typed_at;
    std::optional<std::size_t> m_last_click_row;
    std::chrono::steady_clock::time_point m_last_click_at;
    std::optional<std::vector<std::string>> m_result;
    std::optional<int> m_hover_button; // 0 Cancel, 1 Open, 2 the filter

    // Geometry, device px.
    ChooserLook m_look;
    Rect m_dialog;
    Rect m_title;
    Rect m_path_box;
    Rect m_up_button;
    Rect m_sidebar;
    Rect m_list;
    Rect m_header;
    Rect m_footer;
    Rect m_filter_button;
    Rect m_cancel_button;
    Rect m_open_button;
    int m_row_height = 26;
    int m_size_column = 0;
    int m_date_column = 0;
};

}
