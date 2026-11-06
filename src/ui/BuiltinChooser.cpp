#include "ui/BuiltinChooser.h"

#include "core/Unicode.h"
#include "ui/ChromeText.h"
#include "ui/Icons.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>
#include <utility>

namespace sashfold::ui {

namespace fs = std::filesystem;

// What the reading thread hands over: the folder it read, its entries, and
// the places (found with the first folder).
struct BuiltinChooser::Listing {
    std::mutex mutex;
    bool ready = false;
    std::uint64_t asked = 0; // the folder last asked for, by number: an older answer is dropped
    std::uint64_t answered = 0;
    std::string folder;
    std::vector<Entry> entries;
    std::string error;
    std::vector<Place> places;
    bool places_found = false;
};

namespace {

std::string lower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string home_folder()
{
#if defined(_WIN32)
    if (char const* const profile = std::getenv("USERPROFILE"))
        return profile;
#endif
    if (char const* const home = std::getenv("HOME"))
        return home;
    return fs::current_path().string();
}

// The desktop's usual folders (the XDG user directories where they are set,
// their English names where not), those that exist.
std::vector<std::pair<std::string, std::string>> usual_folders()
{
    std::string const home = home_folder();
    std::vector<std::pair<std::string, std::string>> places = { { "Home", home } };
    struct Usual {
        char const* key;
        char const* label;
        char const* fallback;
    };
    static constexpr Usual usual[] = { { "XDG_DESKTOP_DIR", "Desktop", "Desktop" }, { "XDG_DOCUMENTS_DIR", "Documents", "Documents" },
        { "XDG_DOWNLOAD_DIR", "Downloads", "Downloads" }, { "XDG_PICTURES_DIR", "Pictures", "Pictures" }, { "XDG_MUSIC_DIR", "Music", "Music" },
        { "XDG_VIDEOS_DIR", "Videos", "Videos" } };
    std::string config;
    if (char const* const xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        config = xdg;
    else
        config = (fs::path(home) / ".config").string();
    std::ifstream dirs(fs::path(config) / "user-dirs.dirs");
    std::vector<std::pair<std::string, std::string>> set;
    for (std::string line; std::getline(dirs, line);) {
        std::size_t const equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos)
            continue;
        std::string value = line.substr(equals + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (value.starts_with("$HOME"))
            value = home + value.substr(5);
        set.emplace_back(line.substr(0, equals), value);
    }
    std::error_code error;
    for (Usual const& one : usual) {
        std::string path = (fs::path(home) / one.fallback).string();
        for (auto const& [key, value] : set) {
            if (key == one.key)
                path = value;
        }
        if (path != home && fs::is_directory(path, error))
            places.emplace_back(one.label, path);
    }
    places.emplace_back("Computer", fs::path(home).root_path().string());
    return places;
}

std::string size_text(std::uint64_t bytes)
{
    char text[32];
    if (bytes < 1000) {
        std::snprintf(text, sizeof text, "%llu bytes", static_cast<unsigned long long>(bytes));
        return text;
    }
    static constexpr char const* units[] = { "kB", "MB", "GB", "TB" };
    double value = static_cast<double>(bytes) / 1000.0;
    std::size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < std::size(units)) {
        value /= 1000.0;
        ++unit;
    }
    std::snprintf(text, sizeof text, value < 10 ? "%.1f %s" : "%.0f %s", value, units[unit]);
    return text;
}

// A date as the desktops write it in a list: the time for today, the day
// and month for this year, the year too for another.
std::string date_text(std::int64_t seconds)
{
    if (seconds <= 0)
        return {};
    std::time_t const when = static_cast<std::time_t>(seconds);
    std::time_t const now = std::time(nullptr);
    std::tm local {};
    std::tm today {};
#if defined(_WIN32)
    localtime_s(&local, &when);
    localtime_s(&today, &now);
#else
    localtime_r(&when, &local);
    localtime_r(&now, &today);
#endif
    char text[32];
    static constexpr char const* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (local.tm_year == today.tm_year && local.tm_yday == today.tm_yday)
        std::snprintf(text, sizeof text, "%02d:%02d", local.tm_hour, local.tm_min);
    else if (local.tm_year == today.tm_year)
        std::snprintf(text, sizeof text, "%d %s", local.tm_mday, months[local.tm_mon % 12]);
    else
        std::snprintf(text, sizeof text, "%d %s %d", local.tm_mday, months[local.tm_mon % 12], local.tm_year + 1900);
    return text;
}

bool glob_matches(std::string const& name, std::string const& pattern)
{
    std::string const lowered = lower(name);
    std::string const wanted = lower(pattern);
    if (wanted == "*" || wanted == "*.*")
        return true;
    if (wanted.starts_with("*"))
        return lowered.ends_with(wanted.substr(1));
    return lowered == wanted;
}

} // namespace

BuiltinChooser::BuiltinChooser(platform::FileChoice choice, std::string start_folder, std::function<void()> wake)
    : m_choice(std::move(choice))
    , m_wake(std::move(wake))
    , m_listing(std::make_shared<Listing>())
{
    std::error_code error;
    if (start_folder.empty() || !fs::is_directory(start_folder, error))
        start_folder = home_folder();
    open(std::move(start_folder));
}

BuiltinChooser::~BuiltinChooser()
{
    for (std::thread& reader : m_readers) {
        if (reader.joinable())
            reader.join();
    }
}

std::optional<std::vector<std::string>> BuiltinChooser::take_result()
{
    return std::exchange(m_result, std::nullopt);
}

bool BuiltinChooser::reading() const
{
    std::lock_guard const lock(m_listing->mutex);
    return m_listing->answered != m_listing->asked;
}

// A folder asked for: read on a thread, its entries sorted as every chooser
// sorts them (folders first, then by name, letters' case aside).
void BuiltinChooser::open(std::string folder)
{
    std::uint64_t number = 0;
    bool places_wanted = false;
    {
        std::lock_guard const lock(m_listing->mutex);
        number = ++m_listing->asked;
        places_wanted = !m_listing->places_found;
    }
    m_folder = folder;
    m_editing_path = false;
    m_readers.emplace_back([listing = m_listing, wake = m_wake, folder = std::move(folder), number, places_wanted] {
        std::vector<Entry> entries;
        std::string why;
        std::error_code error;
        fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, error);
        if (error)
            why = "This folder cannot be read: " + error.message();
        for (; !error && it != fs::directory_iterator(); it.increment(error)) {
            fs::directory_entry const& entry = *it;
            Entry one;
            one.name = entry.path().filename().string();
            std::error_code ignore;
            one.folder = entry.is_directory(ignore);
            if (!one.folder) {
                one.size = entry.file_size(ignore);
                if (ignore)
                    one.size = 0;
            }
            fs::file_time_type const changed = entry.last_write_time(ignore);
            if (!ignore) {
                auto const since = changed - fs::file_time_type::clock::now() + std::chrono::system_clock::now().time_since_epoch();
                one.modified = std::chrono::duration_cast<std::chrono::seconds>(since).count();
            }
            entries.push_back(std::move(one));
        }
        std::sort(entries.begin(), entries.end(), [](Entry const& a, Entry const& b) {
            if (a.folder != b.folder)
                return a.folder;
            std::string const la = lower(a.name);
            std::string const lb = lower(b.name);
            return la != lb ? la < lb : a.name < b.name;
        });
        std::vector<std::pair<std::string, std::string>> places;
        if (places_wanted)
            places = usual_folders();
        {
            std::lock_guard const lock(listing->mutex);
            if (number == listing->asked) {
                listing->folder = folder;
                listing->entries = std::move(entries);
                listing->error = std::move(why);
                listing->ready = true;
            }
            if (places_wanted && !listing->places_found) {
                for (auto const& [label, path] : places)
                    listing->places.push_back(Place { label, path });
                listing->places_found = true;
                listing->ready = true;
            }
            listing->answered = std::max(listing->answered, number);
        }
        if (wake)
            wake();
    });
}

bool BuiltinChooser::poll()
{
    std::lock_guard const lock(m_listing->mutex);
    if (!m_listing->ready)
        return false;
    m_listing->ready = false;
    if (m_places.empty() && m_listing->places_found)
        m_places = m_listing->places;
    if (m_listing->folder == m_folder) {
        m_entries = std::move(m_listing->entries);
        m_listing->entries.clear();
        m_error = m_listing->error;
        m_scroll = 0;
        m_anchor.reset();
        m_cursor.reset();
        refilter();
    }
    return true;
}

// Whether an entry is listed: hidden ones only when asked for; a file only
// when the filter takes it (a folder always, so the reader can go into it).
bool BuiltinChooser::shown(Entry const& entry) const
{
    if (!m_hidden && entry.name.starts_with("."))
        return false;
    if (entry.folder || m_filter >= m_choice.filters.size())
        return true;
    platform::FileFilter const& filter = m_choice.filters[m_filter];
    for (std::string const& pattern : filter.patterns) {
        if (glob_matches(entry.name, pattern))
            return true;
    }
    std::string const type = platform::type_of_file(entry.name);
    for (std::string const& wanted : filter.mime_types) {
        if (!type.empty() && (type == wanted || (wanted.ends_with("/*") && type.starts_with(wanted.substr(0, wanted.size() - 1)))))
            return true;
    }
    return filter.patterns.empty() && filter.mime_types.empty();
}

void BuiltinChooser::refilter()
{
    m_rows.clear();
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        if (shown(m_entries[i]))
            m_rows.push_back(i);
    }
    m_chosen.assign(m_rows.size(), false);
    m_anchor.reset();
    m_cursor.reset();
    m_scroll = std::min(m_scroll, m_rows.empty() ? 0 : m_rows.size() - 1);
}

std::string BuiltinChooser::rows_text() const
{
    std::string text;
    for (std::size_t row = 0; row < m_rows.size(); ++row) {
        Entry const& entry = m_entries[m_rows[row]];
        std::string name = entry.name + (entry.folder ? "/" : "");
        if (m_chosen[row])
            name = "[" + name + "]";
        text += (text.empty() ? "" : " ") + name;
    }
    return text.empty() ? std::string("(nothing)") : text;
}

// --- Choosing -----------------------------------------------------------------

void BuiltinChooser::choose_row(std::size_t row, bool add, bool range)
{
    if (row >= m_rows.size())
        return;
    bool const several = m_choice.multiple && !m_choice.directory;
    if (range && several && m_anchor) {
        std::fill(m_chosen.begin(), m_chosen.end(), false);
        for (std::size_t i = std::min(*m_anchor, row); i <= std::max(*m_anchor, row); ++i)
            m_chosen[i] = true;
    } else if (add && several) {
        m_chosen[row] = !m_chosen[row];
        m_anchor = row;
    } else {
        std::fill(m_chosen.begin(), m_chosen.end(), false);
        m_chosen[row] = true;
        m_anchor = row;
    }
    m_cursor = row;
    reveal(row);
}

// Enter or a double click on a row: a folder is gone into, a file taken.
void BuiltinChooser::activate(std::size_t row)
{
    if (row >= m_rows.size())
        return;
    Entry const& entry = m_entries[m_rows[row]];
    if (entry.folder) {
        open((fs::path(m_folder) / entry.name).string());
        return;
    }
    if (m_choice.directory)
        return;
    finish({ (fs::path(m_folder) / entry.name).string() });
}

// The Open (or Select) button: the files chosen; in a folder chooser the
// folder chosen, else the one shown.
void BuiltinChooser::accept()
{
    std::vector<std::string> paths;
    for (std::size_t row = 0; row < m_rows.size(); ++row) {
        if (!m_chosen[row])
            continue;
        Entry const& entry = m_entries[m_rows[row]];
        if (entry.folder != m_choice.directory) {
            if (entry.folder && paths.empty()) {
                open((fs::path(m_folder) / entry.name).string()); // a folder chosen in a file chooser: into it
                return;
            }
            continue;
        }
        paths.push_back((fs::path(m_folder) / entry.name).string());
    }
    if (paths.empty() && m_choice.directory)
        paths.push_back(m_folder);
    if (paths.empty())
        return;
    finish(std::move(paths));
}

void BuiltinChooser::finish(std::vector<std::string> paths)
{
    m_result = std::move(paths);
}

void BuiltinChooser::go_up()
{
    fs::path const here(m_folder);
    fs::path const parent = here.parent_path();
    if (!parent.empty() && parent != here)
        open(parent.string());
}

std::size_t BuiltinChooser::rows_visible() const
{
    return static_cast<std::size_t>(std::max(1, (m_list.height - m_header.height) / std::max(1, m_row_height)));
}

void BuiltinChooser::reveal(std::size_t row)
{
    std::size_t const visible = rows_visible();
    if (row < m_scroll)
        m_scroll = row;
    else if (row >= m_scroll + visible)
        m_scroll = row + 1 - visible;
}

std::optional<std::size_t> BuiltinChooser::row_at(int x, int y) const
{
    int const top = m_list.y + m_header.height;
    if (!m_list.contains(x, y) || y < top)
        return std::nullopt;
    std::size_t const row = m_scroll + static_cast<std::size_t>((y - top) / std::max(1, m_row_height));
    return row < m_rows.size() ? std::optional<std::size_t>(row) : std::nullopt;
}

// --- Input --------------------------------------------------------------------

void BuiltinChooser::mouse_move(int x, int y)
{
    m_hover_button.reset();
    if (m_cancel_button.contains(x, y))
        m_hover_button = 0;
    else if (m_open_button.contains(x, y))
        m_hover_button = 1;
    else if (m_filter_button.contains(x, y))
        m_hover_button = 2;
}

void BuiltinChooser::mouse_down(int x, int y, int button, platform::Modifiers const& modifiers)
{
    if (button != 1)
        return;
    if (!m_dialog.contains(x, y))
        return; // modal: the page behind is not pressed
    if (m_cancel_button.contains(x, y)) {
        finish({});
        return;
    }
    if (m_open_button.contains(x, y)) {
        accept();
        return;
    }
    if (m_filter_button.contains(x, y)) {
        // The page's filters in turn, then every file.
        if (!m_choice.filters.empty()) {
            m_filter = (m_filter + 1) % (m_choice.filters.size() + 1);
            refilter();
        }
        return;
    }
    if (m_up_button.contains(x, y)) {
        go_up();
        return;
    }
    if (m_path_box.contains(x, y)) {
        m_editing_path = true;
        m_path_text = decode_utf8(m_folder);
        return;
    }
    m_editing_path = false;
    if (m_sidebar.contains(x, y)) {
        int const row = (y - m_sidebar.y - m_row_height / 2) / std::max(1, m_row_height);
        if (row >= 0 && static_cast<std::size_t>(row) < m_places.size())
            open(m_places[static_cast<std::size_t>(row)].path);
        return;
    }
    if (std::optional<std::size_t> const row = row_at(x, y)) {
        auto const now = std::chrono::steady_clock::now();
        bool const twice = m_last_click_row == row && now - m_last_click_at < std::chrono::milliseconds(500);
        m_last_click_row = row;
        m_last_click_at = now;
        if (twice && !modifiers.ctrl && !modifiers.shift) {
            m_last_click_row.reset();
            activate(*row);
            return;
        }
        choose_row(*row, modifiers.ctrl, modifiers.shift);
    }
}

void BuiltinChooser::wheel(int, int, int notches)
{
    std::size_t const visible = rows_visible();
    std::size_t const most = m_rows.size() > visible ? m_rows.size() - visible : 0;
    long long const moved = static_cast<long long>(m_scroll) - static_cast<long long>(notches) * 3;
    m_scroll = static_cast<std::size_t>(std::clamp<long long>(moved, 0, static_cast<long long>(most)));
}

void BuiltinChooser::path_key(platform::KeyEvent const& key)
{
    using platform::Key;
    switch (key.key) {
    case Key::Escape:
        m_editing_path = false;
        return;
    case Key::Backspace:
        if (!m_path_text.empty())
            m_path_text.pop_back();
        return;
    case Key::Enter: {
        // A folder is gone into; a file is taken; anything else stays to be put right.
        std::string const typed = to_utf8(m_path_text);
        fs::path path(typed);
        if (path.is_relative())
            path = fs::path(m_folder) / path;
        std::error_code error;
        if (fs::is_directory(path, error)) {
            open(path.lexically_normal().string());
        } else if (fs::is_regular_file(path, error) && !m_choice.directory) {
            finish({ path.lexically_normal().string() });
        }
        return;
    }
    default:
        return;
    }
}

void BuiltinChooser::key(platform::KeyEvent const& key)
{
    using platform::Key;
    if (m_editing_path) {
        path_key(key);
        return;
    }
    if (key.ctrl && key.key == Key::Letter) {
        if (key.letter == U'H') {
            m_hidden = !m_hidden;
            refilter();
        } else if (key.letter == U'L') {
            m_editing_path = true;
            m_path_text = decode_utf8(m_folder);
        } else if (key.letter == U'A' && m_choice.multiple && !m_choice.directory) {
            for (std::size_t row = 0; row < m_rows.size(); ++row)
                m_chosen[row] = !m_entries[m_rows[row]].folder;
        }
        return;
    }
    if (key.alt && key.key == Key::Up) {
        go_up();
        return;
    }
    std::size_t const last = m_rows.empty() ? 0 : m_rows.size() - 1;
    std::size_t const at = m_cursor.value_or(0);
    std::size_t const page = rows_visible();
    switch (key.key) {
    case Key::Escape: finish({}); return;
    case Key::Backspace: go_up(); return;
    case Key::Enter:
        if (m_cursor && std::count(m_chosen.begin(), m_chosen.end(), true) <= 1)
            activate(*m_cursor);
        else
            accept();
        return;
    case Key::Up: choose_row(m_cursor ? (at == 0 ? 0 : at - 1) : 0, false, key.shift); return;
    case Key::Down: choose_row(m_cursor ? std::min(last, at + 1) : 0, false, key.shift); return;
    case Key::Home: choose_row(0, false, key.shift); return;
    case Key::End: choose_row(last, false, key.shift); return;
    case Key::PageUp: choose_row(at > page ? at - page : 0, false, key.shift); return;
    case Key::PageDown: choose_row(std::min(last, at + page), false, key.shift); return;
    default: return;
    }
}

void BuiltinChooser::text(char32_t code_point)
{
    if (m_editing_path) {
        m_path_text.push_back(code_point);
        return;
    }
    // A path begun: the field takes it from here.
    if (code_point == U'/' || code_point == U'~') {
        m_editing_path = true;
        m_path_text = code_point == U'~' ? decode_utf8(home_folder()) : std::u32string(1, code_point);
        return;
    }
    // Letters in a row find the first name they begin; a pause starts afresh.
    auto const now = std::chrono::steady_clock::now();
    if (now - m_typed_at > std::chrono::seconds(1))
        m_typed.clear();
    m_typed_at = now;
    m_typed.push_back(code_point);
    std::string const prefix = lower(to_utf8(m_typed));
    for (std::size_t row = 0; row < m_rows.size(); ++row) {
        if (lower(m_entries[m_rows[row]].name).starts_with(prefix)) {
            choose_row(row, false, false);
            return;
        }
    }
}

// --- Layout and drawing ----------------------------------------------------------

void BuiltinChooser::layout(Rect const& area, ChooserLook const& look)
{
    m_look = look;
    auto const px = [&](float css) { return static_cast<int>(std::lround(css * look.scale)); };
    int const width = std::min(px(760), std::max(px(320), area.width - px(48)));
    int const height = std::min(px(520), std::max(px(260), area.height - px(48)));
    m_dialog = Rect { area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height };
    int const pad = px(12);
    m_row_height = px(26);
    m_title = Rect { m_dialog.x + pad, m_dialog.y + pad, m_dialog.width - 2 * pad, px(20) };
    int const bar_y = m_title.bottom() + px(8);
    m_up_button = Rect { m_dialog.x + pad, bar_y, px(30), px(30) };
    m_path_box = Rect { m_up_button.right() + px(6), bar_y, m_dialog.right() - pad - m_up_button.right() - px(6), px(30) };
    int const body_y = m_path_box.bottom() + px(10);
    int const footer_height = px(52);
    m_footer = Rect { m_dialog.x, m_dialog.bottom() - footer_height, m_dialog.width, footer_height };
    m_sidebar = Rect { m_dialog.x + pad, body_y, px(150), m_footer.y - body_y - px(4) };
    m_list = Rect { m_sidebar.right() + px(10), body_y, m_dialog.right() - pad - m_sidebar.right() - px(10), m_sidebar.height };
    m_header = Rect { m_list.x, m_list.y, m_list.width, px(24) };
    m_date_column = px(110);
    m_size_column = px(90);
    int const button_height = px(30);
    int const button_y = m_footer.y + (m_footer.height - button_height) / 2;
    auto const button_width = [&](std::u32string_view label) {
        return std::max(px(84), static_cast<int>(chrome_text_width(label, look.font_size * look.scale)) + px(28));
    };
    std::u32string const open_label = m_choice.directory ? U"Select" : U"Open";
    int const open_width = button_width(open_label);
    m_open_button = Rect { m_dialog.right() - pad - open_width, button_y, open_width, button_height };
    int const cancel_width = button_width(U"Cancel");
    m_cancel_button = Rect { m_open_button.x - px(8) - cancel_width, button_y, cancel_width, button_height };
    m_filter_button = Rect { m_dialog.x + pad, button_y, std::max(px(120), m_cancel_button.x - px(16) - m_dialog.x - pad), button_height };
    if (m_choice.filters.empty())
        m_filter_button = Rect {};
}

void BuiltinChooser::paint(Bitmap& target) const
{
    ChooserLook const& look = m_look;
    auto const px = [&](float css) { return static_cast<int>(std::lround(css * look.scale)); };
    float const size = look.font_size * look.scale;
    int const radius = px(10);
    // The page behind, dimmed: nothing there can be pressed while it is open.
    target.fill_rect(Rect { 0, 0, target.width(), target.height() }, look.scrim);
    // A soft shadow, then the dialog itself.
    for (int i = 1; i <= 4; ++i)
        target.fill_round_rect(Rect { m_dialog.x - i, m_dialog.y - i + px(2), m_dialog.width + 2 * i, m_dialog.height + 2 * i }, radius + i,
            Color::rgba(0, 0, 0, static_cast<std::uint8_t>(0x18 - i * 4)));
    target.fill_round_box(m_dialog, radius, std::max(1, px(1)), look.border, look.background);

    std::u32string const title = decode_utf8(!m_choice.title.empty() ? m_choice.title
            : m_choice.directory                                         ? std::string("Select a folder")
            : m_choice.multiple                                          ? std::string("Open files")
                                                                         : std::string("Open a file"));
    chrome_draw_text(target, title, static_cast<float>(m_title.x), chrome_centered_baseline(m_title, size * 1.08f), size * 1.08f, look.text, true);

    // The folder's path, or what is being typed into it.
    target.fill_round_box(m_up_button, px(6), std::max(1, px(1)), look.border, look.field_background);
    draw_icon(target, Icon::Back, m_up_button, px(14), look.text);
    target.fill_round_box(m_path_box, px(6), std::max(1, px(1)), m_editing_path ? look.accent : look.border, look.field_background);
    Rect const path_text { m_path_box.x + px(10), m_path_box.y, m_path_box.width - px(20), m_path_box.height };
    std::u32string const shown_path = m_editing_path ? m_path_text : decode_utf8(m_folder);
    // A path too long for its box keeps its end (where the reader is), the
    // start given up for an ellipsis.
    std::u32string fitted = shown_path;
    if (chrome_text_width(fitted, size) > static_cast<float>(path_text.width)) {
        std::size_t cut = 0;
        while (cut < shown_path.size() && chrome_text_width(U"…" + shown_path.substr(cut), size) > static_cast<float>(path_text.width))
            ++cut;
        fitted = U"…" + shown_path.substr(cut);
    }
    chrome_draw_text(target, fitted, static_cast<float>(path_text.x), chrome_centered_baseline(path_text, size), size, look.text);
    if (m_editing_path) {
        float const caret_x = static_cast<float>(path_text.x) + chrome_text_width(fitted, size);
        target.fill_rect(Rect { static_cast<int>(caret_x) + 1, m_path_box.y + px(7), std::max(1, px(1)), m_path_box.height - px(14) }, look.accent);
    }

    // The places.
    for (std::size_t i = 0; i < m_places.size(); ++i) {
        Rect const row { m_sidebar.x, m_sidebar.y + static_cast<int>(i) * m_row_height, m_sidebar.width, m_row_height };
        if (row.bottom() > m_sidebar.bottom())
            break;
        bool const here = m_places[i].path == m_folder;
        if (here)
            target.fill_round_rect(row, px(6), look.highlight);
        draw_icon(target, Icon::Folder, Rect { row.x + px(6), row.y, px(18), row.height }, px(14), here ? look.highlight_text : look.muted);
        Rect const label { row.x + px(30), row.y, row.width - px(34), row.height };
        chrome_draw_text(target, chrome_ellipsize(decode_utf8(m_places[i].label), static_cast<float>(label.width), size),
            static_cast<float>(label.x), chrome_centered_baseline(label, size), size, here ? look.highlight_text : look.text);
    }

    // The list: its frame, the column heads, the rows.
    target.fill_round_box(m_list, px(8), std::max(1, px(1)), look.border, look.field_background);
    int const name_x = m_list.x + px(34);
    int const size_x = m_list.right() - m_date_column - m_size_column;
    int const date_x = m_list.right() - m_date_column;
    float const small = size * 0.92f;
    chrome_draw_text(target, U"Name", static_cast<float>(name_x), chrome_centered_baseline(m_header, small), small, look.muted);
    chrome_draw_text(target, U"Size", static_cast<float>(size_x), chrome_centered_baseline(m_header, small), small, look.muted);
    chrome_draw_text(target, U"Modified", static_cast<float>(date_x), chrome_centered_baseline(m_header, small), small, look.muted);
    target.fill_rect(Rect { m_list.x + px(1), m_header.bottom() - 1, m_list.width - px(2), std::max(1, px(1)) }, look.border);
    Rect const rows_area { m_list.x + px(1), m_header.bottom(), m_list.width - px(2), m_list.height - m_header.height - px(1) };
    std::optional<Rect> const was_clip = target.clip();
    target.set_clip(rows_area);
    if (!m_error.empty()) {
        chrome_draw_text(target, decode_utf8(m_error), static_cast<float>(name_x), static_cast<float>(rows_area.y) + size * 1.8f, size, look.muted);
    } else if (m_rows.empty() && !reading()) {
        chrome_draw_text(target, U"Nothing here", static_cast<float>(name_x), static_cast<float>(rows_area.y) + size * 1.8f, size, look.muted);
    }
    std::size_t const visible = rows_visible() + 1;
    // On the accent, whichever ink reads: dark on a light one, white on a dark.
    int const accent_light = (2126 * look.accent.r + 7152 * look.accent.g + 722 * look.accent.b) / 10000;
    Color const on_accent = accent_light > 140 ? Color::rgb(0x10, 0x12, 0x16) : Color::rgb(0xff, 0xff, 0xff);
    for (std::size_t row = m_scroll; row < m_rows.size() && row < m_scroll + visible; ++row) {
        Entry const& entry = m_entries[m_rows[row]];
        Rect const box { rows_area.x, rows_area.y + static_cast<int>(row - m_scroll) * m_row_height, rows_area.width, m_row_height };
        bool const chosen = m_chosen[row];
        // In a folder chooser the files are shown, dimmed, for the reader's bearings.
        bool const takeable = m_choice.directory ? entry.folder : true;
        if (chosen)
            target.fill_rect(box, look.accent);
        else if (m_cursor == row)
            target.fill_rect(box, look.highlight);
        Color const ink = chosen ? on_accent : takeable ? look.text : look.disabled;
        draw_icon(target, entry.folder ? Icon::Folder : Icon::Page, Rect { m_list.x + px(8), box.y, px(20), box.height }, px(15),
            chosen ? on_accent : entry.folder ? look.accent : look.muted);
        Rect const name_box { name_x, box.y, size_x - name_x - px(10), box.height };
        chrome_draw_text(target, chrome_ellipsize(decode_utf8(entry.name), static_cast<float>(name_box.width), size), static_cast<float>(name_box.x),
            chrome_centered_baseline(name_box, size), size, ink);
        Color const detail = chosen ? on_accent : look.muted;
        if (!entry.folder)
            chrome_draw_text(target, decode_utf8(size_text(entry.size)), static_cast<float>(size_x), chrome_centered_baseline(box, small), small, detail);
        chrome_draw_text(target, decode_utf8(date_text(entry.modified)), static_cast<float>(date_x), chrome_centered_baseline(box, small), small, detail);
    }
    target.set_clip(was_clip);
    // A bar that says how far down the list is, when it does not all show.
    if (m_rows.size() + 1 > visible) {
        int const track = rows_area.height;
        int const thumb = std::max(px(24), static_cast<int>(static_cast<double>(track) * static_cast<double>(visible - 1) / static_cast<double>(m_rows.size())));
        std::size_t const most = m_rows.size() - (visible - 1);
        int const offset = most == 0 ? 0 : static_cast<int>(static_cast<double>(track - thumb) * static_cast<double>(m_scroll) / static_cast<double>(most));
        target.fill_round_rect(Rect { rows_area.right() - px(6), rows_area.y + offset, px(4), thumb }, px(2), look.muted);
    }

    // The foot: the filter, Cancel, Open.
    if (!m_filter_button.is_empty()) {
        std::string const label = m_filter < m_choice.filters.size() ? m_choice.filters[m_filter].name : std::string("All files");
        target.fill_round_box(m_filter_button, px(6), std::max(1, px(1)), m_hover_button == 2 ? look.accent : look.border, look.field_background);
        Rect const inner { m_filter_button.x + px(10), m_filter_button.y, m_filter_button.width - px(30), m_filter_button.height };
        chrome_draw_text(target, chrome_ellipsize(decode_utf8(label), static_cast<float>(inner.width), size), static_cast<float>(inner.x),
            chrome_centered_baseline(inner, size), size, look.text);
        // A small triangle pointing down: there is more than this to choose from.
        int const cx = m_filter_button.right() - px(16);
        int const cy = m_filter_button.y + m_filter_button.height / 2 - px(2);
        for (int row = 0; row < px(4); ++row)
            target.fill_rect(Rect { cx - px(4) + row, cy + row, std::max(1, 2 * (px(4) - row)), 1 }, look.muted);
    }
    auto const button = [&](Rect const& box, std::u32string_view label, bool primary, bool hovered, bool enabled) {
        Color const fill = primary ? look.accent : hovered ? look.highlight : look.field_background;
        target.fill_round_box(box, px(6), std::max(1, px(1)), primary ? look.accent : look.border, fill);
        if (primary && hovered)
            target.fill_round_rect(box, px(6), Color::rgba(0xff, 0xff, 0xff, 0x26));
        int const light = (2126 * look.accent.r + 7152 * look.accent.g + 722 * look.accent.b) / 10000;
        Color const ink = !enabled ? look.disabled : !primary ? look.text : light > 140 ? Color::rgb(0x10, 0x12, 0x16) : Color::rgb(0xff, 0xff, 0xff);
        float const width = chrome_text_width(label, size);
        chrome_draw_text(target, label, static_cast<float>(box.x) + (static_cast<float>(box.width) - width) / 2, chrome_centered_baseline(box, size), size, ink);
    };
    bool const any = std::find(m_chosen.begin(), m_chosen.end(), true) != m_chosen.end();
    button(m_cancel_button, U"Cancel", false, m_hover_button == 0, true);
    button(m_open_button, m_choice.directory ? U"Select" : U"Open", true, m_hover_button == 1, any || m_choice.directory);
}

}
