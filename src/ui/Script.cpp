#include "ui/Script.h"

#include "core/Ascii.h"
#include "core/Png.h"
#include "core/Unicode.h"
#include "ui/Theme.h"

#include <charconv>
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
        { "f5", Key::F5 }, { "f12", Key::F12 },
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

    void fail(std::string const& what)
    {
        ++result.failures;
        out << "FAIL line " << line_number << ": " << what << "\n";
    }

    void settle()
    {
        while (browser.has_pending_load())
            browser.tick();
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

    void click(int x, int y, int button)
    {
        browser.mouse_move(x, y);
        browser.mouse_down(x, y, button);
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
            browser.new_tab();
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
        } else if (command == "resize") {
            auto const width = int_arg(0);
            auto const height = int_arg(1);
            if (!width || !height)
                return fail("resize: needs width height");
            browser.resize(*width, *height);
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
        } else if (command == "assert-scroll") {
            expect_equal("assert-scroll", std::to_string(browser.scroll_y()), argument);
        } else if (command == "assert-scrolled") {
            if (browser.scroll_y() <= 0)
                fail("assert-scrolled: scroll offset is 0");
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
        } else if (command == "echo") {
            out << argument << "\n";
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
        update_goldens, out, {}, 0 };
    if (!file) {
        runner.fail("cannot read script " + path);
        return runner.result;
    }
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
