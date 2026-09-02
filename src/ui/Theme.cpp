#include "ui/Theme.h"

#include "core/Ascii.h"
#include "core/Json.h"

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
    { "button-hover-background", &Theme::button_hover_background },
    { "button-disabled-text", &Theme::button_disabled_text },
    { "status-background", &Theme::status_background },
    { "status-text", &Theme::status_text },
    { "content-background", &Theme::content_background },
    { "secure-indicator", &Theme::secure_indicator },
    { "insecure-indicator", &Theme::insecure_indicator },
};

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
    { "scroll-step", &Theme::scroll_step },
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

    for (auto const& [key, value] : root->as_object()) {
        (void)value;
        if (key != "name" && key != "colors" && key != "metrics" && key != "type"
            && key != "timings" && problems)
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
    return from_json(std::move(stream).str(), problems);
}

}
