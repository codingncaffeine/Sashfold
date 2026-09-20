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

// A picture a theme lays over a surface of the chrome: the file, where it
// is held against the area it is placed in, and whether it repeats from
// there. Browser themes say the same of theirs — Firefox's theme_frame and
// each of its additional_backgrounds with their alignment and tiling,
// Chrome's theme_frame, theme_toolbar and theme_tab_background — and what
// they leave unsaid is what is unsaid here: the top right corner, once.
struct ThemePicture {
    enum class Hold {
        Start, // the left, the top
        Center,
        End, // the right, the bottom
    };
    std::string path; // as written; resolved against the theme file when it is loaded from one
    Hold across = Hold::End;
    Hold down = Hold::Start;
    bool repeat_across = false;
    bool repeat_down = false;

    friend bool operator==(ThemePicture const&, ThemePicture const&) = default;
};

// A tint of a color as Chrome's themes apply one: a hue to take (0 to 1), a
// saturation and a lightness to move towards (0.5 leaves each as it is), and
// -1 for any of the three to leave it alone. Alpha is kept.
struct HslTint {
    double hue = -1;
    double saturation = -1;
    double lightness = -1;
    bool changes_nothing() const { return hue < 0 && saturation < 0 && lightness < 0; }
};
Color apply_tint(Color color, HslTint const& tint);

// The frame of a window that is not the one in front, for a theme that
// does not say: its frame under the tint Chrome gives one by default — a
// little lighter, and for a dark frame a little more saturated — so that
// which window has the keyboard can be seen where the shell draws its own
// title bar. A theme that wants its frame the same either way names it so.
Color inactive_frame_of(Color frame);

// How the tabs sit in their strip. Attached, a tab's foot runs into the
// toolbar and only its top corners are round — the tab in front and the
// toolbar are one surface, as Chrome draws them. Floating, a tab is a pill
// of its own, all four corners round, with the strip showing above and
// below it, as Firefox draws them: what a theme of that browser's was drawn
// for, whose tab in front may be white over a blue toolbar.
enum class TabShape {
    Attached,
    Floating,
};

struct Theme {
    std::string name = "Sashfold";
    // "tab-shape": "attached" (as unsaid) or "floating".
    TabShape tab_shape = TabShape::Attached;

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

    // Surfaces with tokens of their own, each derived from one of the
    // tokens above when a theme file does not name it (so a theme written
    // before the surface existed still dresses it, in its own colors), and
    // each the counterpart of a key browser themes carry: a Firefox or
    // Chrome theme's colors land on these one to one.
    //
    // Whatever pops up over the window — a menu, the command palette:
    // Firefox's popup, popup_border, popup_text, popup_highlight and
    // popup_highlight_text, and the two dimmer texts those themes get by
    // fading popup_text.
    Color popup_background = Color::rgb(0x1f, 0x22, 0x28); // chrome-background
    Color popup_border = Color::rgb(0x3a, 0x40, 0x4b); // address-border
    Color popup_text = Color::rgb(0xe6, 0xe8, 0xec); // chrome-text
    Color popup_text_muted = Color::rgb(0x8f, 0x96, 0xa3); // chrome-text-muted: a shortcut, a row not highlighted
    Color popup_disabled_text = Color::rgb(0x55, 0x5b, 0x66); // button-disabled-text
    Color popup_highlight = Color::rgb(0x33, 0x39, 0x44); // button-hover-background
    Color popup_highlight_text = Color::rgb(0xe6, 0xe8, 0xec); // chrome-text
    // The toolbar apart from the tab that is in front, and its buttons'
    // glyphs apart from the chrome's text: toolbar and icons (Chrome's
    // toolbar and toolbar_button_icon).
    Color toolbar_background = Color::rgb(0x2c, 0x31, 0x3a); // tab-active-background
    Color toolbar_icon = Color::rgb(0xe6, 0xe8, 0xec); // chrome-text
    // The words on the toolbar's own surface — the bookmarks bar's titles:
    // Firefox's toolbar_text and bookmark_text, Chrome's bookmark_text.
    Color toolbar_text = Color::rgb(0xe6, 0xe8, 0xec); // chrome-text
    // The title of the tab in front — tab_text; the others' is
    // chrome-text-muted, their tab_background_text — and a line along that
    // tab's top edge, tab_line, which no theme of ours draws: transparent
    // unless a file names it, and a container's stripe goes over it.
    Color tab_text = Color::rgb(0xe6, 0xe8, 0xec); // chrome-text
    Color tab_line = Color::rgba(0, 0, 0, 0);
    // A field with the focus — the address bar, the find box, the
    // palette's: toolbar_field_focus, toolbar_field_text_focus,
    // toolbar_field_border_focus — and the band over its selected text,
    // toolbar_field_highlight.
    Color address_background_focus = Color::rgb(0x14, 0x16, 0x1b); // address-background
    Color address_text_focus = Color::rgb(0xe6, 0xe8, 0xec); // address-text
    Color address_border_focus = Color::rgb(0x5b, 0x9c, 0xf6); // accent
    Color address_selection = Color::rgba(0x5b, 0x9c, 0xf6, 0x66); // selection
    // The frame — what shows behind the tabs — of a window that is not the
    // one in front: frame_inactive. Unsaid, it is not the frame's color but
    // inactive_frame_of it.
    Color chrome_background_inactive = Color::rgb(0x3b, 0x3f, 0x47); // inactive_frame_of(chrome-background)
    // A toolbar button while it is held down — button_background_active —
    // the line between the tab strip and the toolbar —
    // toolbar_top_separator, drawn by no theme that does not name it — and
    // the text of a field over its selection band:
    // toolbar_field_highlight_text.
    Color button_active_background = Color::rgb(0x33, 0x39, 0x44); // button-hover-background
    Color toolbar_top_separator = Color::rgba(0, 0, 0, 0);
    Color address_selection_text = Color::rgb(0xe6, 0xe8, 0xec); // address-text-focus

    // Pictures, each list front to back. All three are placed against one
    // area — the tab strip and the toolbar together, from the window's top
    // left corner — so that a picture runs on unbroken from the tab in
    // front into the toolbar; what differs is where each shows. The frame's
    // show behind everything in that area (theme_frame, and behind it the
    // additional_backgrounds), over the frame's color; every fill above
    // them composites, so a theme that gives its toolbar an alpha lets them
    // through. The toolbar's show over the toolbar's color and the front
    // tab's (theme_toolbar); the background tabs' over each other tab
    // (theme_tab_background).
    std::vector<ThemePicture> frame_pictures;
    std::vector<ThemePicture> toolbar_pictures;
    std::vector<ThemePicture> tab_background_pictures;

    // Metrics, px.
    int tab_strip_height = 36;
    int tab_height = 30;
    int tab_max_width = 220;
    int tab_min_width = 80;
    int tab_corner_radius = 6;
    int tab_gap = 2;
    int toolbar_height = 40;
    int bookmarks_bar_height = 30; // the bar under the toolbar, while it is shown
    int bookmark_max_width = 160; // one bookmark on it, its icon and its title
    int address_height = 28;
    int address_corner_radius = 8;
    int button_size = 28;
    int button_corner_radius = 6;
    int padding = 6;
    int border_width = 1;
    int status_height = 22;
    int find_height = 36;
    int devtools_height = 220;
    int scroll_step = 60;
    int tab_icon_size = 16; // a page's icon in its tab, drawn before the title

    // Type, px — and the face the chrome's words are set in: "font-family",
    // a list as CSS writes one, first choice first ("Inter, sans-serif").
    // Unsaid, it is the machine's own interface face, then its sans-serif.
    // Either way the list ends at the built-in face, which is all there is
    // where the machine's fonts are turned off, as they are for a script.
    float font_size = 14;
    float tab_font_size = 13;
    float status_font_size = 12;
    std::string font_family;

    // Timings, ms — parsed now so themes can declare them; the animation
    // era consumes them.
    int tab_hover_ms = 120;
    int tab_switch_ms = 160;

    // The new-tab page: a folder of pictures it rotates through — a path
    // written relative to the theme file and resolved when the file is
    // loaded; empty, and the page draws a gradient from the colors above —
    // and how long each picture stays before the next fades in (ms; 0
    // never rotates).
    std::string new_tab_backgrounds;
    int new_tab_rotate_ms = 20000;
    // The new-tab page's own colors — a browser theme's ntp_background and
    // ntp_text. Unnamed, the page wears the chrome's as it always has: a
    // gradient from chrome-background towards the accent, chrome-text and
    // chrome-text-muted. A theme that names the background alone gets that
    // color flat, as those browsers show it; one that names both ends gets
    // the gradient between them.
    std::optional<Color> new_tab_background;
    std::optional<Color> new_tab_background_end;
    std::optional<Color> new_tab_text;
    std::optional<Color> new_tab_text_muted;

    // Parses a theme file's text over the defaults. Every problem — a bad
    // color, a wrong type, an unknown token, malformed JSON — is reported
    // and leaves that token at its default.
    static Theme from_json(std::string_view text, std::vector<std::string>* problems = nullptr);

    // Reads and parses a theme file; nullopt when the file cannot be read.
    // The paths it names — its pictures, its new-tab folder — are resolved
    // against the folder the file is in.
    static std::optional<Theme> load(std::string const& path,
        std::vector<std::string>* problems = nullptr);

    // This theme for a display of `factor` device px per CSS px: every
    // metric and type size multiplied (a border stays at least one px),
    // colors, timings, pictures and the new-tab settings as they are — a
    // picture's size is in CSS px too, and whoever draws it multiplies. The
    // shell keeps the theme as written and draws with the scaled one.
    Theme scaled(float factor) const;

    friend bool operator==(Theme const&, Theme const&) = default;
};

// "#rgb", "#rrggbb", or "#rrggbbaa", case-insensitive; nullopt otherwise.
std::optional<Color> parse_theme_color(std::string_view text);

}
