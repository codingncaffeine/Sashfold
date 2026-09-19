#include "ui/Script.h"

#include "core/Ascii.h"
#include "core/Png.h"
#include "core/Unicode.h"
#include "platform/Clipboard.h"
#include "ui/Theme.h"
#include "ui/ThemeImport.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <sstream>
#include <vector>

namespace sashfold::ui {

namespace {

using platform::Key;
using platform::KeyEvent;

std::string trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return std::string(text);
}

std::vector<std::string> words(std::string const& text)
{
    std::vector<std::string> out;
    std::istringstream stream(text);
    std::string word;
    while (stream >> word)
        out.push_back(word);
    return out;
}

std::optional<int> to_int(std::string const& text)
{
    int value = 0;
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

std::optional<std::vector<std::uint8_t>> read_file(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::ostringstream stream;
    stream << file.rdbuf();
    std::string const text = std::move(stream).str();
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

bool write_file(std::filesystem::path const& path, std::vector<std::uint8_t> const& bytes)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

std::optional<KeyEvent> parse_chord(std::string const& chord)
{
    KeyEvent event;
    std::string name;
    std::size_t start = 0;
    while (true) {
        std::size_t const plus = chord.find('+', start);
        std::string part = chord.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        for (char& c : part)
            c = static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
        if (plus == std::string::npos) {
            name = part;
            break;
        }
        if (part == "ctrl" || part == "control")
            event.ctrl = true;
        else if (part == "shift")
            event.shift = true;
        else if (part == "alt")
            event.alt = true;
        else
            return std::nullopt;
        start = plus + 1;
    }
    static constexpr std::pair<char const*, Key> named[] = {
        { "enter", Key::Enter }, { "return", Key::Enter }, { "escape", Key::Escape },
        { "esc", Key::Escape }, { "backspace", Key::Backspace }, { "delete", Key::Delete },
        { "tab", Key::Tab }, { "space", Key::Space }, { "left", Key::Left },
        { "right", Key::Right }, { "up", Key::Up }, { "down", Key::Down }, { "home", Key::Home },
        { "end", Key::End }, { "pageup", Key::PageUp }, { "pagedown", Key::PageDown },
        { "f5", Key::F5 }, { "f10", Key::F10 }, { "f12", Key::F12 }, { "menu", Key::Menu },
    };
    for (auto const& [text, key] : named) {
        if (name == text) {
            event.key = key;
            return event;
        }
    }
    if (name.size() == 1 && is_ascii_alphanumeric(static_cast<unsigned char>(name[0]))) {
        event.key = Key::Letter;
        event.letter = static_cast<char32_t>(to_ascii_lowercase(static_cast<unsigned char>(name[0])))
            - (is_ascii_alpha(static_cast<unsigned char>(name[0])) ? 0x20 : 0);
        return event;
    }
    return std::nullopt;
}

struct Runner {
    Browser& browser;
    std::filesystem::path directory;
    bool update_goldens;
    std::ostream& out;
    ScriptResult result;
    int line_number = 0;
    // The pages' clock: virtual, moved by `advance`, so a timer fires when
    // the script says and the run is the same on every machine.
    double clock_ms = 0;
    std::string held_session; // the last session-save without a path
    Profile marked; // the shell's counters when the script last marked them

    void fail(std::string const& what)
    {
        ++result.failures;
        out << "FAIL line " << line_number << ": " << what << "\n";
    }

    // Loads, then the scripts they set going, then the loads those queued.
    void settle()
    {
        for (int round = 0; round < 4; ++round) {
            while (browser.has_pending_load())
                browser.tick();
            browser.run_scripts();
            if (!browser.has_pending_load())
                break;
        }
    }

    std::filesystem::path resolve(std::string const& text) const
    {
        std::filesystem::path const path(text);
        return path.is_absolute() ? path : directory / path;
    }

    std::optional<net::Url> url_from(std::string const& text) const
    {
        if (std::optional<net::Url> const url = net::parse_url(text)) {
            std::string const& scheme = url->scheme;
            if (scheme == "http" || scheme == "https" || scheme == "data" || scheme == "about"
                || scheme == "file" || scheme == "view-source")
                return url;
        }
        std::string generic = resolve(text).generic_string();
        if (!generic.starts_with("/"))
            generic = "/" + generic;
        return net::parse_url("file://" + generic);
    }

    void click(int x, int y, int button, platform::Modifiers modifiers = {})
    {
        browser.mouse_move(x, y);
        browser.mouse_down(x, y, button, modifiers);
        browser.mouse_up(x, y, button);
        settle();
    }

    void expect_equal(std::string const& what, std::string const& actual, std::string const& expected)
    {
        if (actual != expected)
            fail(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
    }

    void run_line(std::string const& raw)
    {
        std::string const line = trim(raw);
        if (line.empty() || line[0] == '#')
            return;
        ++result.commands;
        std::size_t const space = line.find(' ');
        std::string const command = line.substr(0, space);
        std::string const argument = space == std::string::npos ? "" : trim(line.substr(space + 1));
        std::vector<std::string> const args = words(argument);

        auto const int_arg = [&](std::size_t index) -> std::optional<int> {
            return index < args.size() ? to_int(args[index]) : std::nullopt;
        };

        if (command == "open") {
            std::optional<net::Url> const url = url_from(argument);
            if (!url)
                return fail("open: not a URL: " + argument);
            browser.open(*url);
            settle();
        } else if (command == "navigate") {
            browser.navigate(argument);
            settle();
        } else if (command == "click" || command == "middle-click") {
            auto const x = int_arg(0);
            auto const y = int_arg(1);
            if (!x || !y)
                return fail(command + ": needs x y");
            click(*x, *y, command == "click" ? 1 : 2);
        } else if (command == "click-text") {
            std::optional<std::pair<int, int>> const at = browser.find_text(argument);
            if (!at)
                return fail("click-text: no text run contains \"" + argument + "\"");
            click(at->first, at->second, 1);
        } else if (command == "right-click" || command == "shift-right-click") {
            // `right-click <x> <y>`: the menu of what is there; with Shift,
            // the shell's menu whatever the page says.
            auto const x = int_arg(0);
            auto const y = int_arg(1);
            if (!x || !y)
                return fail(command + ": needs x y");
            platform::Modifiers modifiers;
            modifiers.shift = command == "shift-right-click";
            click(*x, *y, 3, modifiers);
        } else if (command == "right-click-text" || command == "shift-right-click-text" || command == "ctrl-click-text"
            || command == "ctrl-shift-click-text") {
            std::optional<std::pair<int, int>> const at = browser.find_text(argument);
            if (!at)
                return fail(command + ": no text run contains \"" + argument + "\"");
            bool const left = command == "ctrl-click-text" || command == "ctrl-shift-click-text";
            platform::Modifiers modifiers;
            modifiers.shift = command == "shift-right-click-text" || command == "ctrl-shift-click-text";
            modifiers.ctrl = left;
            click(at->first, at->second, left ? 1 : 3, modifiers);
        } else if (command == "right-click-tab") {
            // `right-click-tab <index>`: the menu of that tab in the strip.
            auto const index = int_arg(0);
            ChromeLayout const chrome = browser.chrome_layout();
            if (!index || *index < 0 || static_cast<std::size_t>(*index) >= chrome.tabs.size())
                return fail("right-click-tab: no such tab");
            Rect const tab = chrome.tabs[static_cast<std::size_t>(*index)];
            click(tab.x + tab.width / 3, tab.y + tab.height / 2, 3);
        } else if (command == "right-click-address") {
            Rect const address = browser.chrome_layout().address;
            click(address.x + address.width / 2, address.y + address.height / 2, 3);
        } else if (command == "main-menu") {
            // `main-menu`: a click on the toolbar's menu button.
            Rect const button = browser.chrome_layout().menu_button;
            click(button.x + button.width / 2, button.y + button.height / 2, 1);
        } else if (command == "menu") {
            // `menu <label>`: chooses that item of the open menus, the
            // innermost first — it runs, or its submenu opens.
            if (!browser.choose_menu_item(argument))
                return fail("menu: no item \"" + argument + "\" can be chosen; the menu is \"" + browser.menu_text() + "\"");
            settle();
        } else if (command == "menu-hover") {
            // `menu-hover <level> <row>`: the pointer over that row of that
            // open menu, the outermost being 0.
            auto const level = int_arg(0);
            auto const row = int_arg(1);
            ChromeLayout const chrome = browser.chrome_layout();
            if (!level || !row || *level < 0 || *row < 0 || static_cast<std::size_t>(*level) >= chrome.menus.size()
                || static_cast<std::size_t>(*row) >= chrome.menus[static_cast<std::size_t>(*level)].rows.size())
                return fail("menu-hover: no such row of an open menu");
            Rect const at = chrome.menus[static_cast<std::size_t>(*level)].rows[static_cast<std::size_t>(*row)];
            browser.mouse_move(at.x + at.width / 2, at.y + at.height / 2);
        } else if (command == "menu-click") {
            // `menu-click <level> <row>`: a press on that row, as a reader's.
            auto const level = int_arg(0);
            auto const row = int_arg(1);
            ChromeLayout const chrome = browser.chrome_layout();
            if (!level || !row || *level < 0 || *row < 0 || static_cast<std::size_t>(*level) >= chrome.menus.size()
                || static_cast<std::size_t>(*row) >= chrome.menus[static_cast<std::size_t>(*level)].rows.size())
                return fail("menu-click: no such row of an open menu");
            Rect const at = chrome.menus[static_cast<std::size_t>(*level)].rows[static_cast<std::size_t>(*row)];
            click(at.x + at.width / 2, at.y + at.height / 2, 1);
        } else if (command == "press") {
            // `press <x> <y> [<button>]` and, later, `release [<button>]`:
            // the two halves of a click, for a button held while the
            // pointer moves; the button is 1 when not given. The release
            // lands wherever the pointer was last moved to.
            auto const x = int_arg(0);
            auto const y = int_arg(1);
            int const button = int_arg(2).value_or(1);
            if (!x || !y || button < 1 || button > 3)
                return fail("press: needs x y and a button from 1 to 3");
            browser.mouse_move(*x, *y);
            browser.mouse_down(*x, *y, button);
            settle();
        } else if (command == "release") {
            int const button = int_arg(0).value_or(1);
            if (button < 1 || button > 3)
                return fail("release: the button is 1, 2 or 3");
            browser.mouse_up(0, 0, button); // the shell knows where the pointer is
            settle();
        } else if (command == "assert-menu") {
            // `assert-menu <items>`: the innermost open menu, as
            // Browser::menu_text writes it; nothing when none is open.
            expect_equal("assert-menu", browser.menu_text(), argument);
        } else if (command == "assert-menus") {
            // `assert-menus <count>`: how many menus are open, submenus counted.
            auto const count = int_arg(0);
            if (!count)
                return fail("assert-menus: needs a count");
            expect_equal("assert-menus", std::to_string(browser.chrome_layout().menus.size()), std::to_string(*count));
        } else if (command == "assert-clipboard") {
            // `assert-clipboard <text>`: what the process's clipboard holds.
            expect_equal("assert-clipboard", platform::read_clipboard_text().value_or(""), argument);
        } else if (command == "set-clipboard") {
            platform::write_clipboard_text(argument);
        } else if (command == "move") {
            auto const x = int_arg(0);
            auto const y = int_arg(1);
            if (!x || !y)
                return fail("move: needs x y");
            browser.mouse_move(*x, *y);
        } else if (command == "wheel") {
            auto const notches = int_arg(0);
            if (!notches)
                return fail("wheel: needs a notch count");
            Rect const content = browser.chrome_layout().content;
            browser.wheel(content.x + content.width / 2, content.y + content.height / 2, *notches);
        } else if (command == "wheel-at") {
            auto const x = int_arg(0);
            auto const y = int_arg(1);
            auto const notches = int_arg(2);
            if (!x || !y || !notches)
                return fail("wheel-at: needs x y notches");
            browser.wheel(*x, *y, *notches);
        } else if (command == "scroll-pixels") {
            // `scroll-pixels <dy>`: the content moves by that many device px
            // at the content's center, as a finger's drag moves it.
            auto const dy = int_arg(0);
            if (!dy)
                return fail("scroll-pixels: needs a pixel count");
            Rect const content = browser.chrome_layout().content;
            browser.scroll_pixels(content.x + content.width / 2, content.y + content.height / 2, 0, *dy);
        } else if (command == "preedit") {
            // `preedit <text>`: an input method's composing text at the
            // caret, as the window would relay it; no text clears it.
            browser.preedit(argument);
        } else if (command == "assert-text-input") {
            // `assert-text-input on|off`: whether a field has the caret an
            // input method would be told about.
            expect_equal("assert-text-input", browser.text_input_area() ? "on" : "off", argument);
        } else if (command == "window-controls") {
            // `window-controls on|off`: the frame is the shell's to draw, as
            // on a compositor without server-side decorations.
            if (argument != "on" && argument != "off")
                return fail("window-controls: on or off");
            browser.set_window_controls(argument == "on");
        } else if (command == "window-active") {
            // `window-active on|off`: whether the window is the one in
            // front, as the system would say when another takes its place.
            if (argument != "on" && argument != "off")
                return fail("window-active: on or off");
            browser.set_window_active(argument == "on");
        } else if (command == "import-theme") {
            // `import-theme <path>`: a Firefox or Chrome theme — its folder,
            // its .xpi, its .crx, named relative to the script — converted
            // and put on, as one dropped into the profile's themes folder
            // is. The conversion goes into a folder of the system's
            // temporary ones, named for this script.
            std::vector<std::string> problems;
            std::optional<ImportedTheme> const imported = import_browser_theme_from(resolve(argument).string(), &problems);
            std::filesystem::path const into = std::filesystem::temp_directory_path() / "sashfold-script-themes";
            std::optional<std::string> const written
                = imported ? write_imported_theme(*imported, into.string(), &problems) : std::nullopt;
            std::optional<Theme> const theme = written ? Theme::load(*written, &problems) : std::nullopt;
            if (!theme || !problems.empty()) {
                std::string said;
                for (std::string const& problem : problems)
                    said += " " + problem + ";";
                return fail("import-theme: " + argument + " was not converted:" + said);
            }
            browser.set_theme(*theme);
        } else if (command == "assert-theme-problem") {
            // `assert-theme-problem none`, or `assert-theme-problem <text>`:
            // what the theme in use could not put on — a picture it names
            // that is not there — holds the text somewhere, or is nothing.
            std::string said;
            for (std::string const& problem : browser.theme_problems())
                said += problem + "\n";
            if (argument == "none" ? !said.empty() : said.find(argument) == std::string::npos)
                fail("assert-theme-problem: wanted " + argument + ", the theme says: " + (said.empty() ? "nothing" : said));
        } else if (command == "assert-window-request") {
            // `assert-window-request <what>`: what the last press asked of
            // the window — none, move, minimize, maximize, close, or
            // resize-<edge> — and takes it.
            using Request = Browser::WindowRequest;
            char const* name = "none";
            switch (browser.take_window_request()) {
            case Request::None: name = "none"; break;
            case Request::Move: name = "move"; break;
            case Request::Minimize: name = "minimize"; break;
            case Request::ToggleMaximize: name = "maximize"; break;
            case Request::Close: name = "close"; break;
            case Request::ResizeTop: name = "resize-top"; break;
            case Request::ResizeBottom: name = "resize-bottom"; break;
            case Request::ResizeLeft: name = "resize-left"; break;
            case Request::ResizeRight: name = "resize-right"; break;
            case Request::ResizeTopLeft: name = "resize-top-left"; break;
            case Request::ResizeTopRight: name = "resize-top-right"; break;
            case Request::ResizeBottomLeft: name = "resize-bottom-left"; break;
            case Request::ResizeBottomRight: name = "resize-bottom-right"; break;
            }
            expect_equal("assert-window-request", name, argument);
        } else if (command == "assert-box-scroll") {
            auto const x = int_arg(0);
            auto const y = int_arg(1);
            auto const dx = int_arg(2);
            auto const dy = int_arg(3);
            if (!x || !y || !dx || !dy)
                return fail("assert-box-scroll: needs x y dx dy");
            auto const [at_x, at_y] = browser.box_scroll_at(*x, *y);
            expect_equal("assert-box-scroll", std::to_string(at_x) + " " + std::to_string(at_y),
                std::to_string(*dx) + " " + std::to_string(*dy));
        } else if (command == "type") {
            for (char32_t const c : decode_utf8(argument))
                browser.text_input(c);
        } else if (command == "key") {
            std::optional<KeyEvent> const key = parse_chord(argument);
            if (!key)
                return fail("key: unknown chord " + argument);
            browser.key_down(*key);
            settle();
        } else if (command == "back") {
            browser.back();
            settle();
        } else if (command == "forward") {
            browser.forward();
            settle();
        } else if (command == "reload") {
            browser.reload();
            settle();
        } else if (command == "new-tab") {
            browser.new_tab_in(argument); // in the container named, the default when none
        } else if (command == "assert-container") {
            expect_equal("assert-container", browser.active_container(), argument);
        } else if (command == "palette") {
            browser.open_palette(argument);
        } else if (command == "assert-palette") {
            expect_equal("assert-palette", browser.palette_selection(), argument);
        } else if (command == "assert-theme") {
            expect_equal("assert-theme", browser.theme().name, argument);
        } else if (command == "close-tab") {
            std::optional<int> const index = int_arg(0);
            if (!args.empty() && (!index || *index < 0))
                return fail("close-tab: the index must be a non-negative number");
            browser.close_tab(index ? static_cast<std::size_t>(*index) : browser.active_tab());
        } else if (command == "select-tab") {
            auto const index = int_arg(0);
            if (!index || *index < 0)
                return fail("select-tab: needs an index");
            browser.select_tab(static_cast<std::size_t>(*index));
            settle(); // a restored tab fetches its page when shown
        } else if (command == "session-save") {
            held_session = browser.session_json();
            if (!argument.empty()) {
                std::ofstream file(resolve(argument), std::ios::binary);
                file << held_session;
                if (!file)
                    return fail("session-save: could not write " + argument);
            }
        } else if (command == "session-restore") {
            std::string session = held_session;
            if (!argument.empty()) {
                std::ifstream file(resolve(argument), std::ios::binary);
                if (!file)
                    return fail("session-restore: could not read " + argument);
                std::ostringstream text;
                text << file.rdbuf();
                session = std::move(text).str();
            }
            if (!browser.restore_session(session))
                return fail("session-restore: not a session");
            settle();
        } else if (command == "resize") {
            auto const width = int_arg(0);
            auto const height = int_arg(1);
            if (!width || !height)
                return fail("resize: needs width height");
            browser.resize(*width, *height);
        } else if (command == "scale") {
            // `scale <factor>`: the display's device px per CSS px from here
            // on, as a window on a scaled display would report it.
            char* end = nullptr;
            double const factor = std::strtod(argument.c_str(), &end);
            if (argument.empty() || end == argument.c_str() || *end != '\0' || !(factor > 0))
                return fail("scale: needs a factor above zero");
            browser.set_scale(static_cast<float>(factor));
        } else if (command == "screenshot") {
            if (!write_file(resolve(argument), encode_png(browser.frame())))
                fail("screenshot: cannot write " + argument);
        } else if (command == "assert-golden") {
            std::filesystem::path const golden = resolve(argument);
            std::vector<std::uint8_t> const actual = encode_png(browser.frame());
            if (update_goldens) {
                if (write_file(golden, actual))
                    out << "blessed " << golden.string() << " (" << actual.size() << " bytes)\n";
                else
                    fail("assert-golden: cannot write " + golden.string());
                return;
            }
            std::optional<std::vector<std::uint8_t>> const expected = read_file(golden);
            if (!expected)
                return fail("assert-golden: missing " + golden.string() + " (run with --update-goldens)");
            if (*expected != actual) {
                std::filesystem::path const actual_path
                    = std::filesystem::path("shell-actual") / golden.filename();
                write_file(actual_path, actual);
                fail("assert-golden: " + golden.filename().string()
                    + " differs; actual written to " + actual_path.string());
            }
        } else if (command == "assert-url") {
            net::Url const* const url = browser.current_url();
            expect_equal("assert-url", url ? url->serialize() : "", argument);
        } else if (command == "assert-address") {
            expect_equal("assert-address", browser.address_text(), argument);
        } else if (command == "assert-title") {
            expect_equal("assert-title", browser.page_title(), argument);
        } else if (command == "assert-text") {
            if (browser.page_text().find(argument) == std::string::npos)
                fail("assert-text: page does not contain \"" + argument + "\"");
        } else if (command == "assert-no-text") {
            if (browser.page_text().find(argument) != std::string::npos)
                fail("assert-no-text: page contains \"" + argument + "\"");
        } else if (command == "assert-status") {
            if (browser.status_text().find(argument) == std::string::npos)
                fail("assert-status: status is \"" + browser.status_text() + "\", expected it to contain \""
                    + argument + "\"");
        } else if (command == "assert-tabs") {
            expect_equal("assert-tabs", std::to_string(browser.tab_count()), argument);
        } else if (command == "assert-tab-titles") {
            // `assert-tab-titles A | B | C`: every tab's title, in the
            // strip's order, the one in front in [brackets], a pinned one
            // with a ^ before it.
            std::string titles;
            for (std::size_t i = 0; i < browser.tab_count(); ++i) {
                if (i > 0)
                    titles += " | ";
                std::string const title = browser.tab_title(i);
                if (browser.tab_pinned(i))
                    titles += '^';
                titles += i == browser.active_tab() ? "[" + title + "]" : title;
            }
            expect_equal("assert-tab-titles", titles, argument);
        } else if (command == "assert-closed-tabs") {
            // `assert-closed-tabs <count>`: how many closed tabs could come back.
            expect_equal("assert-closed-tabs", std::to_string(browser.closed_tab_count()), argument);
        } else if (command == "duplicate-tab") {
            // `duplicate-tab [index]`: a copy beside the tab, the one in front unsaid.
            std::optional<int> const index = int_arg(0);
            if (!args.empty() && (!index || *index < 0))
                return fail("duplicate-tab: the index must be a non-negative number");
            browser.duplicate_tab(index ? static_cast<std::size_t>(*index) : browser.active_tab());
            settle();
        } else if (command == "assert-scroll") {
            expect_equal("assert-scroll", std::to_string(browser.scroll_y()), argument);
        } else if (command == "assert-scrolled") {
            if (browser.scroll_y() <= 0)
                fail("assert-scrolled: scroll offset is 0");
        } else if (command == "mark") {
            // The counters as they stand; the assertions below read what
            // the shell did since.
            marked = browser.profile();
        } else if (command == "assert-restyles") {
            expect_equal("assert-restyles", std::to_string(browser.profile().restyles - marked.restyles), argument);
        } else if (command == "assert-relayouts") {
            expect_equal("assert-relayouts", std::to_string(browser.profile().relayouts - marked.relayouts), argument);
        } else if (command == "assert-paints") {
            expect_equal("assert-paints", std::to_string(browser.profile().paints - marked.paints), argument);
        } else if (command == "assert-pictures") {
            expect_equal("assert-pictures", std::to_string(browser.pictures()), argument);
        } else if (command == "assert-pixel") {
            auto const x = int_arg(0);
            auto const y = int_arg(1);
            std::optional<Color> const color = args.size() > 2 ? parse_theme_color(args[2]) : std::nullopt;
            if (!x || !y || !color)
                return fail("assert-pixel: needs x y #rrggbb");
            Color const actual = browser.frame().pixel(*x, *y);
            if (!(actual == *color)) {
                char text[16];
                std::snprintf(text, sizeof text, "#%02x%02x%02x%02x", actual.r, actual.g, actual.b, actual.a);
                fail("assert-pixel: (" + std::to_string(*x) + ", " + std::to_string(*y) + ") is " + text
                    + ", expected " + args[2]);
            }
        } else if (command == "assert-focus") {
            bool const focused = browser.address_focused();
            if ((argument == "address") != focused)
                fail("assert-focus: focus is on the " + std::string(focused ? "address bar" : "page"));
        } else if (command == "focus") {
            if (!browser.focus_control(argument))
                return fail("focus: no control named \"" + argument + "\"");
        } else if (command == "assert-value") {
            std::size_t const separator = argument.find(' ');
            std::string const name = argument.substr(0, separator);
            std::string const expected
                = separator == std::string::npos ? "" : argument.substr(separator + 1);
            std::optional<std::string> const value = browser.control_value(name);
            if (!value)
                return fail("assert-value: no control named \"" + name + "\"");
            expect_equal("assert-value " + name, *value, expected);
        } else if (command == "assert-focused") {
            expect_equal("assert-focused", browser.focused_control_name(), argument);
        } else if (command == "drag") {
            auto const x1 = int_arg(0);
            auto const y1 = int_arg(1);
            auto const x2 = int_arg(2);
            auto const y2 = int_arg(3);
            if (!x1 || !y1 || !x2 || !y2)
                return fail("drag: needs x1 y1 x2 y2");
            browser.mouse_move(*x1, *y1);
            browser.mouse_down(*x1, *y1, 1);
            browser.mouse_move(*x2, *y2);
            browser.mouse_up(*x2, *y2, 1);
            settle();
        } else if (command == "select-text") {
            if (!browser.select_text(argument))
                return fail("select-text: no text run contains \"" + argument + "\"");
        } else if (command == "assert-selection") {
            expect_equal("assert-selection", browser.selected_text(), argument);
        } else if (command == "reader") {
            browser.toggle_reader();
            settle();
        } else if (command == "inspect-text") {
            if (!browser.inspect_text(argument))
                return fail("inspect-text: no text run contains \"" + argument + "\"");
        } else if (command == "assert-inspected") {
            expect_equal("assert-inspected", browser.inspected_summary(), argument);
        } else if (command == "assert-hints") {
            expect_equal("assert-hints", std::to_string(browser.hint_count()), argument);
        } else if (command == "assert-find") {
            expect_equal("assert-find", browser.find_status(), argument);
        } else if (command == "assert-blocked") {
            expect_equal("assert-blocked", std::to_string(browser.blocked_requests()), argument);
        } else if (command == "advance") {
            // `advance <ms> [<times>]`: the clock moves and the timers due
            // run, `times` over, so a chain of timers each set by the last
            // (a fade in steps) runs through as it would in a window.
            auto const ms = int_arg(0);
            if (!ms || *ms < 0)
                return fail("advance: needs a number of milliseconds");
            auto const times = args.size() > 1 ? int_arg(1) : std::optional<int> { 1 };
            if (!times || *times < 1)
                return fail("advance: the second number is how many times to advance");
            for (int i = 0; i < *times; ++i) {
                clock_ms += *ms;
                settle();
            }
        } else if (command == "assert-console") {
            if (browser.console_text().find(argument) == std::string::npos)
                fail("assert-console: \"" + argument + "\" not found in:\n" + browser.console_text());
        } else if (command == "echo") {
            out << argument << "\n";
        } else if (command == "print-pixel") {
            // `print-pixel <x> <y>`: what the frame holds there, said and
            // never judged — for whoever is writing a test, to read a
            // pixel before asserting it.
            auto const x = int_arg(0);
            auto const y = int_arg(1);
            if (!x || !y)
                return fail("print-pixel: needs x y");
            Color const actual = browser.frame().pixel(*x, *y);
            char text[16];
            std::snprintf(text, sizeof text, "#%02x%02x%02x%02x", actual.r, actual.g, actual.b, actual.a);
            out << "pixel (" << *x << ", " << *y << ") is " << text << "\n";
        } else if (command == "find-pixel") {
            // `find-pixel <#rrggbb> [<x> <y> <w> <h>]`: where the frame is
            // exactly that color — in the box given, else anywhere — the
            // first few places and how many in all. Said, never judged: it
            // finds the one pixel of a glyph its color fully covers, or
            // shows that a color is nowhere, before an assertion is written.
            std::optional<Color> const wanted = !args.empty() ? parse_theme_color(args[0]) : std::nullopt;
            if (!wanted)
                return fail("find-pixel: needs #rrggbb, then perhaps x y w h");
            Bitmap const& frame = browser.frame();
            int const from_x = std::max(0, int_arg(1).value_or(0));
            int const from_y = std::max(0, int_arg(2).value_or(0));
            int const to_x = std::min(frame.width(), from_x + int_arg(3).value_or(frame.width()));
            int const to_y = std::min(frame.height(), from_y + int_arg(4).value_or(frame.height()));
            int found = 0;
            std::string places;
            for (int y = from_y; y < to_y; ++y) {
                for (int x = from_x; x < to_x; ++x) {
                    if (!(frame.pixel(x, y) == *wanted))
                        continue;
                    if (found < 8)
                        places += " (" + std::to_string(x) + ", " + std::to_string(y) + ")";
                    ++found;
                }
            }
            out << "find-pixel " << args[0] << ": " << found << " in all" << (found ? ";" : "") << places
                << (found > 8 ? " ..." : "") << "\n";
        } else {
            fail("unknown command: " + command);
        }
    }
};

} // namespace

ScriptResult run_script(Browser& browser, std::string const& path, bool update_goldens,
    std::ostream& out)
{
    std::ifstream file(path);
    Runner runner { browser, std::filesystem::absolute(std::filesystem::path(path)).parent_path(),
        update_goldens, out, {}, 0, 0, {}, {} };
    if (!file) {
        runner.fail("cannot read script " + path);
        return runner.result;
    }
    browser.set_clock([&runner] { return runner.clock_ms; });
    // The new-tab page's time: a Monday morning, the same on every machine.
    browser.set_wall_clock([] { return WallTime { 2026, 1, 5, 1, 9, 41, 20 }; });
    std::string line;
    while (std::getline(file, line)) {
        ++runner.line_number;
        runner.run_line(line);
    }
    if (runner.result.failures == 0)
        out << "PASS " << std::filesystem::path(path).filename().string() << " (" << runner.result.commands
            << " commands)\n";
    else
        out << "FAILED " << std::filesystem::path(path).filename().string() << ": "
            << runner.result.failures << " failure(s) in " << runner.result.commands << " commands\n";
    return runner.result;
}

}
