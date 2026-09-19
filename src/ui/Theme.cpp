#include "ui/Theme.h"

#include "core/Ascii.h"
#include "core/Json.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sashfold::ui {

namespace {

template<typename T>
struct Token {
    char const* key;
    T Theme::*member;
};

constexpr Token<Color> color_tokens[] = {
    { "chrome-background", &Theme::chrome_background },
    { "chrome-text", &Theme::chrome_text },
    { "chrome-text-muted", &Theme::chrome_text_muted },
    { "chrome-border", &Theme::chrome_border },
    { "tab-active-background", &Theme::tab_active_background },
    { "tab-inactive-background", &Theme::tab_inactive_background },
    { "tab-hover-background", &Theme::tab_hover_background },
    { "address-background", &Theme::address_background },
    { "address-text", &Theme::address_text },
    { "address-border", &Theme::address_border },
    { "accent", &Theme::accent },
    { "selection", &Theme::selection },
    { "find-highlight", &Theme::find_highlight },
    { "find-current", &Theme::find_current },
    { "hint-background", &Theme::hint_background },
    { "hint-text", &Theme::hint_text },
    { "button-hover-background", &Theme::button_hover_background },
    { "button-disabled-text", &Theme::button_disabled_text },
    { "status-background", &Theme::status_background },
    { "status-text", &Theme::status_text },
    { "content-background", &Theme::content_background },
    { "secure-indicator", &Theme::secure_indicator },
    { "insecure-indicator", &Theme::insecure_indicator },
    { "popup-background", &Theme::popup_background },
    { "popup-border", &Theme::popup_border },
    { "popup-text", &Theme::popup_text },
    { "popup-text-muted", &Theme::popup_text_muted },
    { "popup-disabled-text", &Theme::popup_disabled_text },
    { "popup-highlight", &Theme::popup_highlight },
    { "popup-highlight-text", &Theme::popup_highlight_text },
    { "toolbar-background", &Theme::toolbar_background },
    { "toolbar-icon", &Theme::toolbar_icon },
    { "tab-text", &Theme::tab_text },
    { "tab-line", &Theme::tab_line },
    { "address-background-focus", &Theme::address_background_focus },
    { "address-text-focus", &Theme::address_text_focus },
    { "address-border-focus", &Theme::address_border_focus },
    { "address-selection", &Theme::address_selection },
    { "chrome-background-inactive", &Theme::chrome_background_inactive },
    { "button-active-background", &Theme::button_active_background },
    { "toolbar-top-separator", &Theme::toolbar_top_separator },
    { "address-selection-text", &Theme::address_selection_text },
};

// A token a theme file may leave out takes the theme's own value of
// another, never the built-in default: a light theme written before menus
// existed gets light menus.
struct Derived {
    char const* key;
    Color Theme::*member;
    Color Theme::*source;
};

constexpr Derived derived_colors[] = {
    { "popup-background", &Theme::popup_background, &Theme::chrome_background },
    { "popup-border", &Theme::popup_border, &Theme::address_border },
    { "popup-text", &Theme::popup_text, &Theme::chrome_text },
    { "popup-text-muted", &Theme::popup_text_muted, &Theme::chrome_text_muted },
    { "popup-disabled-text", &Theme::popup_disabled_text, &Theme::button_disabled_text },
    { "popup-highlight", &Theme::popup_highlight, &Theme::button_hover_background },
    { "popup-highlight-text", &Theme::popup_highlight_text, &Theme::chrome_text },
    { "toolbar-background", &Theme::toolbar_background, &Theme::tab_active_background },
    { "toolbar-icon", &Theme::toolbar_icon, &Theme::chrome_text },
    { "tab-text", &Theme::tab_text, &Theme::chrome_text },
    { "address-background-focus", &Theme::address_background_focus, &Theme::address_background },
    { "address-text-focus", &Theme::address_text_focus, &Theme::address_text },
    { "address-border-focus", &Theme::address_border_focus, &Theme::accent },
    { "address-selection", &Theme::address_selection, &Theme::selection },
    { "chrome-background-inactive", &Theme::chrome_background_inactive, &Theme::chrome_background },
    { "button-active-background", &Theme::button_active_background, &Theme::button_hover_background },
    // After address-text-focus, which it follows: the table is read in order.
    { "address-selection-text", &Theme::address_selection_text, &Theme::address_text_focus },
};

// The surfaces a theme may lay pictures over.
struct PictureList {
    char const* key;
    std::vector<ThemePicture> Theme::*member;
};

constexpr PictureList picture_lists[] = {
    { "frame", &Theme::frame_pictures },
    { "toolbar", &Theme::toolbar_pictures },
    { "tab-background", &Theme::tab_background_pictures },
};

// More layers than any theme stacks; each is composited on every paint.
constexpr std::size_t max_pictures_per_surface = 16;

constexpr Token<int> metric_tokens[] = {
    { "tab-strip-height", &Theme::tab_strip_height },
    { "tab-height", &Theme::tab_height },
    { "tab-max-width", &Theme::tab_max_width },
    { "tab-min-width", &Theme::tab_min_width },
    { "tab-corner-radius", &Theme::tab_corner_radius },
    { "tab-gap", &Theme::tab_gap },
    { "toolbar-height", &Theme::toolbar_height },
    { "address-height", &Theme::address_height },
    { "address-corner-radius", &Theme::address_corner_radius },
    { "button-size", &Theme::button_size },
    { "button-corner-radius", &Theme::button_corner_radius },
    { "padding", &Theme::padding },
    { "border-width", &Theme::border_width },
    { "status-height", &Theme::status_height },
    { "find-height", &Theme::find_height },
    { "devtools-height", &Theme::devtools_height },
    { "scroll-step", &Theme::scroll_step },
    { "tab-icon-size", &Theme::tab_icon_size },
};

constexpr Token<float> type_tokens[] = {
    { "font-size", &Theme::font_size },
    { "tab-font-size", &Theme::tab_font_size },
    { "status-font-size", &Theme::status_font_size },
};

constexpr Token<int> timing_tokens[] = {
    { "tab-hover", &Theme::tab_hover_ms },
    { "tab-switch", &Theme::tab_switch_ms },
};

// Visits every member of a section object, applying known tokens and
// reporting the rest. `apply` returns a problem description or nullopt.
template<typename T, std::size_t N, typename Apply>
void read_section(JsonValue const& root, char const* section, Token<T> const (&tokens)[N],
    Apply&& apply, Theme& theme, std::vector<std::string>* problems)
{
    auto const report = [&](std::string const& key, std::string const& what) {
        if (problems)
            problems->push_back(std::string("theme: ") + section + "." + key + ": " + what);
    };
    JsonValue const* const object = root.get(section);
    if (!object)
        return;
    if (!object->is_object()) {
        if (problems)
            problems->push_back(std::string("theme: ") + section + ": expected an object");
        return;
    }
    for (auto const& [key, value] : object->as_object()) {
        Token<T> const* found = nullptr;
        for (Token<T> const& token : tokens) {
            if (key == token.key)
                found = &token;
        }
        if (!found) {
            report(key, "unknown token");
            continue;
        }
        if (std::optional<std::string> const problem = apply(value, theme.*(found->member)))
            report(key, *problem);
    }
}

std::optional<int> integer_in(JsonValue const& value, int low, int high)
{
    if (!value.is_number())
        return std::nullopt;
    double const number = value.as_number();
    if (number < low || number > high)
        return std::nullopt;
    int const truncated = static_cast<int>(number);
    if (static_cast<double>(truncated) != number)
        return std::nullopt;
    return truncated;
}

// Where a picture is held: one or two of left, center, right, top and
// bottom, as a browser theme's alignment is written and as CSS reads a
// background-position — a side named alone leaves the other axis centered.
bool parse_picture_hold(std::string_view text, ThemePicture& picture)
{
    using Hold = ThemePicture::Hold;
    std::optional<Hold> across;
    std::optional<Hold> down;
    int centers = 0;
    int words = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && text[at] == ' ')
            ++at;
        std::size_t const from = at;
        while (at < text.size() && text[at] != ' ')
            ++at;
        if (at == from)
            break;
        std::string_view const word = text.substr(from, at - from);
        ++words;
        if (word == "center") {
            ++centers;
        } else if (word == "left" || word == "right") {
            if (across)
                return false;
            across = word == "left" ? Hold::Start : Hold::End;
        } else if (word == "top" || word == "bottom") {
            if (down)
                return false;
            down = word == "top" ? Hold::Start : Hold::End;
        } else {
            return false;
        }
    }
    if (words == 0 || words > 2)
        return false;
    // Every word found an axis, and a center takes whichever is left.
    if ((across ? 1 : 0) + (down ? 1 : 0) + centers != words)
        return false;
    picture.across = across.value_or(Hold::Center);
    picture.down = down.value_or(Hold::Center);
    return true;
}

bool parse_picture_repeat(std::string_view text, ThemePicture& picture)
{
    if (text == "no-repeat") {
        picture.repeat_across = false;
        picture.repeat_down = false;
    } else if (text == "repeat") {
        picture.repeat_across = true;
        picture.repeat_down = true;
    } else if (text == "repeat-x") {
        picture.repeat_across = true;
        picture.repeat_down = false;
    } else if (text == "repeat-y") {
        picture.repeat_across = false;
        picture.repeat_down = true;
    } else {
        return false;
    }
    return true;
}

// One picture of a surface: its path alone, or an object naming the path
// and how it is held and repeated. Nullopt, and the reason reported, for
// what names no picture.
std::optional<ThemePicture> read_picture(JsonValue const& value, std::string const& where,
    std::vector<std::string>* problems)
{
    auto const report = [&](std::string const& what) {
        if (problems)
            problems->push_back("theme: " + where + what);
    };
    ThemePicture picture;
    if (value.is_string()) {
        picture.path = value.as_string();
    } else if (value.is_object()) {
        for (auto const& [key, member] : value.as_object()) {
            if (key == "picture") {
                if (member.is_string())
                    picture.path = member.as_string();
                else
                    report(".picture: expected a file path");
            } else if (key == "align") {
                if (!member.is_string() || !parse_picture_hold(member.as_string(), picture))
                    report(".align: expected a side or two, like \"right top\" or \"center\"");
            } else if (key == "tile") {
                if (!member.is_string() || !parse_picture_repeat(member.as_string(), picture))
                    report(".tile: expected no-repeat, repeat, repeat-x or repeat-y");
            } else {
                report("." + key + ": unknown token");
            }
        }
    } else {
        report(": expected a file path, or an object with a \"picture\"");
        return std::nullopt;
    }
    if (picture.path.empty()) {
        report(": names no picture");
        return std::nullopt;
    }
    return picture;
}

// The images section: for each surface one picture or a list of them,
// front to back.
void read_pictures(JsonValue const& root, Theme& theme, std::vector<std::string>* problems)
{
    JsonValue const* const section = root.get("images");
    if (!section)
        return;
    if (!section->is_object()) {
        if (problems)
            problems->push_back("theme: images: expected an object");
        return;
    }
    for (auto const& [key, value] : section->as_object()) {
        PictureList const* found = nullptr;
        for (PictureList const& list : picture_lists) {
            if (key == list.key)
                found = &list;
        }
        if (!found) {
            if (problems)
                problems->push_back("theme: images." + key + ": unknown token");
            continue;
        }
        std::vector<ThemePicture>& pictures = theme.*(found->member);
        pictures.clear();
        if (!value.is_array()) {
            if (std::optional<ThemePicture> picture = read_picture(value, "images." + key, problems))
                pictures.push_back(std::move(*picture));
            continue;
        }
        std::size_t index = 0;
        for (JsonValue const& entry : value.as_array()) {
            std::string const where = "images." + key + "[" + std::to_string(index++) + "]";
            if (pictures.size() == max_pictures_per_surface) {
                if (problems)
                    problems->push_back("theme: " + where + ": more pictures than the "
                        + std::to_string(max_pictures_per_surface) + " a surface takes");
                break;
            }
            if (std::optional<ThemePicture> picture = read_picture(entry, where, problems))
                pictures.push_back(std::move(*picture));
        }
    }
}

// A path a theme file names, against the folder the file is in.
std::string beside(std::filesystem::path const& base, std::string const& named)
{
    std::filesystem::path const path(named);
    return path.is_relative() ? (base / path).lexically_normal().string() : named;
}

} // namespace

std::optional<Color> parse_theme_color(std::string_view text)
{
    if (text.size() < 2 || text[0] != '#')
        return std::nullopt;
    std::string_view const digits = text.substr(1);
    for (char const c : digits) {
        if (!is_ascii_hex_digit(static_cast<unsigned char>(c)))
            return std::nullopt;
    }
    auto const value = [&](std::size_t at) {
        return static_cast<std::uint8_t>(hex_digit_value(static_cast<unsigned char>(digits[at])));
    };
    if (digits.size() == 3) {
        auto const doubled = [&](std::size_t at) {
            return static_cast<std::uint8_t>(value(at) * 17u);
        };
        return Color::rgb(doubled(0), doubled(1), doubled(2));
    }
    if (digits.size() == 6 || digits.size() == 8) {
        auto const byte = [&](std::size_t at) {
            return static_cast<std::uint8_t>(value(at) * 16u + value(at + 1));
        };
        std::uint8_t const alpha = digits.size() == 8 ? byte(6) : std::uint8_t { 255 };
        return Color::rgba(byte(0), byte(2), byte(4), alpha);
    }
    return std::nullopt;
}

Theme Theme::from_json(std::string_view text, std::vector<std::string>* problems)
{
    Theme theme;
    std::optional<JsonValue> const root = JsonValue::parse(text);
    if (!root || !root->is_object()) {
        if (problems)
            problems->push_back("theme: not a JSON object");
        return theme;
    }
    if (JsonValue const* const name = root->get("name")) {
        if (name->is_string())
            theme.name = name->as_string();
        else if (problems)
            problems->push_back("theme: name: expected a string");
    }
    if (JsonValue const* const shape = root->get("tab-shape")) {
        if (shape->is_string() && shape->as_string() == "floating")
            theme.tab_shape = TabShape::Floating;
        else if (shape->is_string() && shape->as_string() == "attached")
            theme.tab_shape = TabShape::Attached;
        else if (problems)
            problems->push_back("theme: tab-shape: expected \"attached\" or \"floating\"");
    }

    read_section(*root, "colors", color_tokens,
        [](JsonValue const& value, Color& target) -> std::optional<std::string> {
            if (!value.is_string())
                return "expected a string like \"#rrggbb\"";
            std::optional<Color> const color = parse_theme_color(value.as_string());
            if (!color)
                return "not a color: " + value.as_string();
            target = *color;
            return std::nullopt;
        },
        theme, problems);
    // What the file did not name — or named with something that is no
    // color — follows the token it derives from, as the file set that one.
    JsonValue const* const colors = root->get("colors");
    for (Derived const& derived : derived_colors) {
        JsonValue const* const named = colors && colors->is_object() ? colors->get(derived.key) : nullptr;
        if (!named || !named->is_string() || !parse_theme_color(named->as_string()))
            theme.*(derived.member) = theme.*(derived.source);
    }
    read_section(*root, "metrics", metric_tokens,
        [](JsonValue const& value, int& target) -> std::optional<std::string> {
            std::optional<int> const pixels = integer_in(value, 0, 4096);
            if (!pixels)
                return "expected a whole number of pixels, 0 to 4096";
            target = *pixels;
            return std::nullopt;
        },
        theme, problems);
    read_section(*root, "type", type_tokens,
        [](JsonValue const& value, float& target) -> std::optional<std::string> {
            if (!value.is_number() || value.as_number() < 4 || value.as_number() > 200)
                return "expected a size in pixels, 4 to 200";
            target = static_cast<float>(value.as_number());
            return std::nullopt;
        },
        theme, problems);
    read_section(*root, "timings", timing_tokens,
        [](JsonValue const& value, int& target) -> std::optional<std::string> {
            std::optional<int> const ms = integer_in(value, 0, 10000);
            if (!ms)
                return "expected a whole number of milliseconds, 0 to 10000";
            target = *ms;
            return std::nullopt;
        },
        theme, problems);

    // The new-tab section holds a path and a duration, so it is read by hand.
    if (JsonValue const* const section = root->get("new-tab")) {
        if (!section->is_object()) {
            if (problems)
                problems->push_back("theme: new-tab: expected an object");
        } else {
            for (auto const& [key, value] : section->as_object()) {
                if (key == "backgrounds") {
                    if (value.is_string())
                        theme.new_tab_backgrounds = value.as_string();
                    else if (problems)
                        problems->push_back("theme: new-tab.backgrounds: expected a folder path");
                } else if (key == "rotate") {
                    if (std::optional<int> const ms = integer_in(value, 0, 3600000))
                        theme.new_tab_rotate_ms = *ms;
                    else if (problems)
                        problems->push_back("theme: new-tab.rotate: expected a whole number of milliseconds, 0 to 3600000");
                } else if (key == "background" || key == "background-end" || key == "text" || key == "text-muted") {
                    std::optional<Color>& target = key == "background" ? theme.new_tab_background
                        : key == "background-end"                      ? theme.new_tab_background_end
                        : key == "text"                                ? theme.new_tab_text
                                                                       : theme.new_tab_text_muted;
                    std::optional<Color> const color
                        = value.is_string() ? parse_theme_color(value.as_string()) : std::nullopt;
                    if (color)
                        target = color;
                    else if (problems)
                        problems->push_back("theme: new-tab." + key + ": expected a color like \"#rrggbb\"");
                } else if (problems) {
                    problems->push_back("theme: new-tab." + key + ": unknown token");
                }
            }
        }
    }

    read_pictures(*root, theme, problems);

    for (auto const& [key, value] : root->as_object()) {
        (void)value;
        if (key != "name" && key != "colors" && key != "metrics" && key != "type"
            && key != "timings" && key != "new-tab" && key != "images" && key != "tab-shape" && problems)
            problems->push_back("theme: " + key + ": unknown section");
    }
    return theme;
}

std::optional<Theme> Theme::load(std::string const& path, std::vector<std::string>* problems)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (problems)
            problems->push_back("theme: cannot read " + path);
        return std::nullopt;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    Theme theme = from_json(std::move(stream).str(), problems);
    // What the file names — the new-tab page's folder, the chrome's
    // pictures — it names relative to itself.
    std::error_code error;
    std::filesystem::path const base = std::filesystem::absolute(path, error).parent_path();
    if (!theme.new_tab_backgrounds.empty())
        theme.new_tab_backgrounds = beside(base, theme.new_tab_backgrounds);
    for (PictureList const& list : picture_lists) {
        for (ThemePicture& picture : theme.*(list.member))
            picture.path = beside(base, picture.path);
    }
    return theme;
}

Theme Theme::scaled(float factor) const
{
    Theme out = *this;
    if (!(factor > 0) || factor == 1)
        return out;
    auto const metric = [factor](int value) { return static_cast<int>(std::lround(static_cast<float>(value) * factor)); };
    for (int Theme::* const member : { &Theme::tab_strip_height, &Theme::tab_height, &Theme::tab_max_width,
             &Theme::tab_min_width, &Theme::tab_corner_radius, &Theme::tab_gap, &Theme::toolbar_height,
             &Theme::address_height, &Theme::address_corner_radius, &Theme::button_size,
             &Theme::button_corner_radius, &Theme::padding, &Theme::status_height, &Theme::find_height,
             &Theme::devtools_height, &Theme::scroll_step, &Theme::tab_icon_size })
        out.*member = metric(this->*member);
    out.border_width = std::max(1, metric(border_width));
    out.font_size = font_size * factor;
    out.tab_font_size = tab_font_size * factor;
    out.status_font_size = status_font_size * factor;
    return out;
}

}
