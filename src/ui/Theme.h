#pragma once

// ThemeTokens: every pixel of browser chrome is drawn through this
// set — colors, metrics, type sizes, animation timings — loaded from a
// themes/*.json file by our own JSON reader. Themes are data, not code: a
// theme is one file, shared by posting it anywhere. Every token has a
// built-in default, so a partial or broken theme degrades token by token,
// never to a blank window, and every problem is reported by name.

#include "core/Bitmap.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::ui {

struct Theme {
    std::string name = "Sashfold";

    // Colors.
    Color chrome_background = Color::rgb(0x1f, 0x22, 0x28);
    Color chrome_text = Color::rgb(0xe6, 0xe8, 0xec);
    Color chrome_text_muted = Color::rgb(0x8f, 0x96, 0xa3);
    Color chrome_border = Color::rgb(0x12, 0x14, 0x18);
    Color tab_active_background = Color::rgb(0x2c, 0x31, 0x3a);
    Color tab_inactive_background = Color::rgb(0x1f, 0x22, 0x28);
    Color tab_hover_background = Color::rgb(0x26, 0x2a, 0x32);
    Color address_background = Color::rgb(0x14, 0x16, 0x1b);
    Color address_text = Color::rgb(0xe6, 0xe8, 0xec);
    Color address_border = Color::rgb(0x3a, 0x40, 0x4b);
    Color accent = Color::rgb(0x5b, 0x9c, 0xf6);
    Color selection = Color::rgba(0x5b, 0x9c, 0xf6, 0x66);
    Color find_highlight = Color::rgba(0xff, 0xd5, 0x4f, 0x99); // every match on the page
    Color find_current = Color::rgba(0xff, 0x98, 0x00, 0xcc); // the match the find bar is on
    Color hint_background = Color::rgb(0xff, 0xe0, 0x66); // keyboard link-hint labels
    Color hint_text = Color::rgb(0x1c, 0x1b, 0x19);
    Color button_hover_background = Color::rgb(0x33, 0x39, 0x44);
    Color button_disabled_text = Color::rgb(0x55, 0x5b, 0x66);
    Color status_background = Color::rgb(0x1f, 0x22, 0x28);
    Color status_text = Color::rgb(0x8f, 0x96, 0xa3);
    Color content_background = Color::rgb(0xff, 0xff, 0xff);
    Color secure_indicator = Color::rgb(0x5c, 0xc8, 0x8a);
    Color insecure_indicator = Color::rgb(0xe0, 0x6c, 0x5c);

    // Metrics, px.
    int tab_strip_height = 36;
    int tab_height = 30;
    int tab_max_width = 220;
    int tab_min_width = 80;
    int tab_corner_radius = 6;
    int tab_gap = 2;
    int toolbar_height = 40;
    int address_height = 28;
    int address_corner_radius = 8;
    int button_size = 28;
    int button_corner_radius = 6;
    int padding = 6;
    int border_width = 1;
    int status_height = 22;
    int find_height = 36;
    int scroll_step = 60;

    // Type, px.
    float font_size = 14;
    float tab_font_size = 13;
    float status_font_size = 12;

    // Timings, ms — parsed now so themes can declare them; the animation
    // era consumes them.
    int tab_hover_ms = 120;
    int tab_switch_ms = 160;

    // Parses a theme file's text over the defaults. Every problem — a bad
    // color, a wrong type, an unknown token, malformed JSON — is reported
    // and leaves that token at its default.
    static Theme from_json(std::string_view text, std::vector<std::string>* problems = nullptr);

    // Reads and parses a theme file; nullopt when the file cannot be read.
    static std::optional<Theme> load(std::string const& path,
        std::vector<std::string>* problems = nullptr);

    friend bool operator==(Theme const&, Theme const&) = default;
};

// "#rgb", "#rrggbb", or "#rrggbbaa", case-insensitive; nullopt otherwise.
std::optional<Color> parse_theme_color(std::string_view text);

}
