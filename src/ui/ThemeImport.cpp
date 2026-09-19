#include "ui/ThemeImport.h"

#include "core/Ascii.h"
#include "core/Json.h"
#include "core/Png.h"
#include "core/Zip.h"
#include "css/StyleResolver.h"
#include "ui/PageImages.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

namespace sashfold::ui {

namespace {

// What a theme's archive, its manifest and any one of its files may weigh.
constexpr std::uintmax_t max_archive_bytes = 64u * 1024u * 1024u;
constexpr std::size_t max_manifest_bytes = 1024u * 1024u;
constexpr std::size_t max_picture_bytes = 16u * 1024u * 1024u;

// The palette a theme's unsaid colors come from when its text is dark: the
// shipped light theme's. (A test holds the two equal.) When its text is
// bright, they come from the built-in defaults, which are the dark theme's.
constexpr char const* light_palette = R"({ "colors": {
    "chrome-background": "#f3f4f6", "chrome-text": "#1f2328", "chrome-text-muted": "#6b7280",
    "chrome-border": "#d1d5db", "tab-active-background": "#ffffff", "tab-inactive-background": "#f3f4f6",
    "tab-hover-background": "#e9ebef", "address-background": "#ffffff", "address-text": "#1f2328",
    "address-border": "#cfd4dc", "accent": "#2563eb", "selection": "#2563eb55",
    "find-highlight": "#ffd54f99", "find-current": "#ff9800cc", "hint-background": "#ffe066",
    "hint-text": "#1c1b19", "button-hover-background": "#e3e6ea", "button-disabled-text": "#b0b6c0",
    "status-background": "#f3f4f6", "status-text": "#6b7280", "content-background": "#ffffff",
    "secure-indicator": "#15803d", "insecure-indicator": "#dc2626" } })";

std::uint8_t to_byte(double value)
{
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(value)), 0, 255));
}

// How light a color reads, 0 to 1 (the luma of its sRGB values: what both
// browsers go by when they ask whether a theme's text is bright).
double lightness_of(Color color)
{
    return (0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b) / 255.0;
}

Color with_alpha(Color color, std::uint8_t alpha)
{
    color.a = alpha;
    return color;
}

// `over` composited on an opaque `under`.
Color composite(Color over, Color under)
{
    double const a = over.a / 255.0;
    return Color::rgb(to_byte(over.r * a + under.r * (1 - a)), to_byte(over.g * a + under.g * (1 - a)),
        to_byte(over.b * a + under.b * (1 - a)));
}

// `amount` of the way from `from` to `to`.
Color towards(Color from, Color to, double amount)
{
    return Color::rgba(to_byte(from.r + (to.r - from.r) * amount), to_byte(from.g + (to.g - from.g) * amount),
        to_byte(from.b + (to.b - from.b) * amount), to_byte(from.a + (to.a - from.a) * amount));
}

std::string hex(Color color)
{
    char text[16];
    if (color.a == 255)
        std::snprintf(text, sizeof text, "#%02x%02x%02x", color.r, color.g, color.b);
    else
        std::snprintf(text, sizeof text, "#%02x%02x%02x%02x", color.r, color.g, color.b, color.a);
    return text;
}

std::string json_text(std::string_view text)
{
    std::string out = "\"";
    for (char const c : text) {
        auto const byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (byte < 0x20) {
            char escape[8];
            std::snprintf(escape, sizeof escape, "\\u%04x", byte);
            out += escape;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

// A color as either browser's manifest writes one: [r, g, b], with a
// fourth element for alpha — 0 to 1 — or, in Firefox's, any CSS color.
std::optional<Color> color_of(JsonValue const& value)
{
    if (value.is_string())
        return css::parse_color_text(value.as_string());
    if (!value.is_array())
        return std::nullopt;
    JsonValue::Array const& parts = value.as_array();
    if (parts.size() != 3 && parts.size() != 4)
        return std::nullopt;
    for (JsonValue const& part : parts) {
        if (!part.is_number())
            return std::nullopt;
    }
    Color color = Color::rgb(to_byte(parts[0].as_number()), to_byte(parts[1].as_number()), to_byte(parts[2].as_number()));
    if (parts.size() == 4)
        color.a = to_byte(std::clamp(parts[3].as_number(), 0.0, 1.0) * 255.0);
    return color;
}

std::optional<HslTint> tint_of(JsonValue const& value)
{
    if (!value.is_array() || value.as_array().size() != 3)
        return std::nullopt;
    JsonValue::Array const& parts = value.as_array();
    for (JsonValue const& part : parts) {
        if (!part.is_number())
            return std::nullopt;
    }
    auto const part = [&](std::size_t i) {
        double const number = parts[i].as_number();
        return number < 0 ? -1.0 : std::min(number, 1.0);
    };
    return HslTint { part(0), part(1), part(2) };
}

// A picture with a tint worked into every pixel, as a PNG.
std::optional<std::vector<std::uint8_t>> tinted_picture(std::vector<std::uint8_t> const& bytes, HslTint const& tint)
{
    std::optional<Bitmap> decoded = decode_image_bytes(bytes);
    if (!decoded)
        return std::nullopt;
    for (int y = 0; y < decoded->height(); ++y) {
        for (int x = 0; x < decoded->width(); ++x)
            decoded->set_pixel(x, y, apply_tint(decoded->pixel(x, y), tint));
    }
    return encode_png(*decoded);
}

// The extension a picture is written under: the manifest's own, when it is
// one a picture has.
std::string extension_of(std::string const& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    for (char& c : extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char const* known : { ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".svg" }) {
        if (extension == known)
            return extension;
    }
    return ".png";
}

// A path a manifest writes, kept inside the theme: no scheme, no drive, no
// way up out of the folder.
std::optional<std::string> inside_path(std::string_view written)
{
    std::string path(written);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.starts_with("./"))
        path.erase(0, 2);
    while (path.starts_with("/"))
        path.erase(0, 1);
    if (path.empty() || path.find(':') != std::string::npos)
        return std::nullopt;
    std::size_t at = 0;
    while (at <= path.size()) {
        std::size_t const end = std::min(path.find('/', at), path.size());
        if (path.compare(at, end - at, "..") == 0)
            return std::nullopt;
        at = end + 1;
    }
    return path;
}

// Everything one conversion gathers on its way to a theme file.
struct Conversion {
    explicit Conversion(ThemeFileReader const& reader)
        : read(reader)
    {
    }

    ImportedTheme out;
    ThemeFileReader const& read;
    JsonValue const* colors = nullptr;
    JsonValue const* images = nullptr;
    JsonValue const* properties = nullptr;
    JsonValue const* tints = nullptr;
    std::map<std::string, std::string> tokens; // color tokens, in the order the file lists them
    std::vector<std::string> frame_layers; // each a JSON object, front to back
    std::string toolbar_layer;
    std::string tab_layer;
    std::string new_tab; // members of the new-tab section

    std::optional<Color> color(char const* key)
    {
        JsonValue const* const value = colors ? colors->get(key) : nullptr;
        if (!value)
            return std::nullopt;
        std::optional<Color> const parsed = color_of(*value);
        if (!parsed)
            out.notes.push_back(std::string("colors.") + key + ": not a color this reads");
        return parsed;
    }

    HslTint tint(char const* key, HslTint fallback = {})
    {
        JsonValue const* const value = tints ? tints->get(key) : nullptr;
        if (!value)
            return fallback;
        std::optional<HslTint> const parsed = tint_of(*value);
        if (!parsed) {
            out.notes.push_back(std::string("tints.") + key + ": expected three numbers");
            return fallback;
        }
        return *parsed;
    }

    void set(char const* token, Color value) { tokens[token] = hex(value); }

    // One of the theme's pictures, read, tinted if it is to be, kept under
    // `name` to be written beside the theme file. Empty when it cannot be had.
    std::string picture(std::string const& key, JsonValue const& value, std::string const& name, HslTint const& tint = {})
    {
        if (value.is_object()) {
            out.notes.push_back(key + ": a gradient for a picture has nowhere to land yet");
            return {};
        }
        if (!value.is_string()) {
            out.notes.push_back(key + ": expected a file path");
            return {};
        }
        std::optional<std::string> const path = inside_path(value.as_string());
        if (!path) {
            out.notes.push_back(key + ": a path outside the theme: " + value.as_string());
            return {};
        }
        std::optional<std::vector<std::uint8_t>> bytes = read(*path);
        if (!bytes || bytes->empty() || bytes->size() > max_picture_bytes) {
            out.notes.push_back(key + ": cannot read " + *path);
            return {};
        }
        std::string file = name + extension_of(*path);
        if (!tint.changes_nothing()) {
            std::optional<std::vector<std::uint8_t>> tinted = tinted_picture(*bytes, tint);
            if (tinted) {
                bytes = std::move(tinted);
                file = name + ".png";
            } else {
                out.notes.push_back(key + ": its tint could not be applied: no decoder here reads " + *path);
            }
        }
        out.pictures.push_back({ file, std::move(*bytes) });
        return file;
    }

    static std::string layer(std::string const& file, std::string_view align, std::string_view tile)
    {
        return "{ \"picture\": " + json_text(file) + ", \"align\": " + json_text(align) + ", \"tile\": "
            + json_text(tile) + " }";
    }
};

bool is_picture_alignment(std::string_view text)
{
    for (char const* known : { "bottom", "center", "left", "right", "top", "center bottom", "center center",
             "center top", "left bottom", "left center", "left top", "right bottom", "right center", "right top" }) {
        if (text == known)
            return true;
    }
    return false;
}

bool is_picture_tiling(std::string_view text)
{
    return text == "no-repeat" || text == "repeat" || text == "repeat-x" || text == "repeat-y";
}

// Whether the theme's words are bright, and its unsaid colors therefore
// the dark palette's: what the theme says of itself, else the text it
// gives, else the surface the text stands on.
bool wants_dark_palette(Conversion& c, std::initializer_list<char const*> texts, std::initializer_list<char const*> surfaces)
{
    if (c.properties) {
        if (JsonValue const* const scheme = c.properties->get("color_scheme"); scheme && scheme->is_string()) {
            if (scheme->as_string() == "dark")
                return true;
            if (scheme->as_string() == "light")
                return false;
        }
    }
    for (char const* key : texts) {
        JsonValue const* const value = c.colors ? c.colors->get(key) : nullptr;
        if (std::optional<Color> const color = value ? color_of(*value) : std::nullopt)
            return lightness_of(*color) > 0.5;
    }
    for (char const* key : surfaces) {
        JsonValue const* const value = c.colors ? c.colors->get(key) : nullptr;
        if (std::optional<Color> const color = value ? color_of(*value) : std::nullopt)
            return lightness_of(*color) < 0.5;
    }
    return false;
}

// What is said under `section` and is none of `known`: noted, so that
// nothing a theme says goes missing without a word.
void note_unknown(Conversion& c, JsonValue const* section, char const* name, std::initializer_list<char const*> known,
    std::initializer_list<char const*> homeless)
{
    if (!section || !section->is_object())
        return;
    for (auto const& [key, value] : section->as_object()) {
        (void)value;
        bool found = false;
        for (char const* one : known)
            found = found || key == one;
        if (found)
            continue;
        bool waiting = false;
        for (char const* one : homeless)
            waiting = waiting || key == one;
        c.out.notes.push_back(std::string(name) + "." + key
            + (waiting ? ": has no surface here yet" : ": not a key of this kind of theme"));
    }
}

// The tokens every converted theme writes out from its palette, so that it
// does not change when our defaults do.
void write_palette(Conversion& c, Theme const& palette)
{
    c.set("chrome-border", palette.chrome_border);
    c.set("accent", palette.accent);
    c.set("selection", palette.selection);
    c.set("find-highlight", palette.find_highlight);
    c.set("find-current", palette.find_current);
    c.set("hint-background", palette.hint_background);
    c.set("hint-text", palette.hint_text);
    c.set("button-disabled-text", palette.button_disabled_text);
    c.set("content-background", palette.content_background);
    c.set("secure-indicator", palette.secure_indicator);
    c.set("insecure-indicator", palette.insecure_indicator);
    c.set("address-background", palette.address_background);
    c.set("address-text", palette.address_text);
    c.set("address-border", palette.address_border);
    // A browser's menus are its own, not its frame's: the palette's, unless
    // the theme dresses them.
    c.set("popup-background", palette.chrome_background);
    c.set("popup-border", palette.address_border);
    c.set("popup-text", palette.chrome_text);
    c.set("popup-highlight", palette.button_hover_background);
    c.set("popup-highlight-text", palette.chrome_text);
}

// The two fainter texts of a menu, which a theme gives no key for: its
// text faded towards what it stands on, as those browsers fade it.
void fade_popup_texts(Conversion& c, Color text, Color background)
{
    c.set("popup-text-muted", towards(background, text, 0.62));
    c.set("popup-disabled-text", towards(background, text, 0.38));
}

void convert_firefox(Conversion& c)
{
    bool const dark = wants_dark_palette(c, { "toolbar_text", "bookmark_text", "tab_background_text" }, { "frame", "toolbar" });
    Theme const palette = dark ? Theme {} : Theme::from_json(light_palette);
    write_palette(c, palette);

    Color const frame = c.color("frame").value_or(palette.chrome_background);
    c.set("chrome-background", frame);
    // That browser leaves a frame that says nothing of a window not in
    // front as it is; ours would dim it, so it is said.
    c.set("chrome-background-inactive", c.color("frame_inactive").value_or(frame));
    // The words of the strip — the background tabs' titles, the glyphs
    // beside them — are one color in that browser.
    Color const strip_text = c.color("tab_background_text").value_or(palette.chrome_text);
    c.set("chrome-text", strip_text);
    c.set("chrome-text-muted", strip_text);
    std::optional<Color> const toolbar_text = [&] {
        std::optional<Color> text = c.color("toolbar_text");
        return text ? text : c.color("bookmark_text");
    }();
    c.set("tab-text", c.color("tab_text").value_or(toolbar_text.value_or(strip_text)));
    // A toolbar the theme does not color veils the header's picture just
    // enough to keep its own words readable.
    Color const toolbar = c.color("toolbar").value_or(dark ? Color::rgba(0, 0, 0, 0x66) : Color::rgba(255, 255, 255, 0x66));
    c.set("toolbar-background", toolbar);
    c.set("tab-active-background", c.color("tab_selected").value_or(toolbar));
    // Background tabs are the frame seen through; the pointer lightens one
    // with its own text's color, faintly.
    c.set("tab-inactive-background", Color::rgba(0, 0, 0, 0));
    c.set("tab-hover-background", with_alpha(strip_text, 0x1c));
    if (std::optional<Color> const line = c.color("tab_line"))
        c.set("tab-line", *line);
    Color const icons = c.color("icons").value_or(toolbar_text.value_or(strip_text));
    c.set("toolbar-icon", icons);
    c.set("button-hover-background", c.color("button_background_hover").value_or(with_alpha(icons, 0x2b)));
    if (std::optional<Color> const pressed = c.color("button_background_active"))
        c.set("button-active-background", *pressed);
    else
        c.set("button-active-background", with_alpha(icons, 0x4d));
    if (std::optional<Color> const separator = c.color("toolbar_bottom_separator"))
        c.set("chrome-border", *separator);
    if (std::optional<Color> const separator = c.color("toolbar_top_separator"))
        c.set("toolbar-top-separator", *separator);

    if (std::optional<Color> const field = c.color("toolbar_field"))
        c.set("address-background", *field);
    if (std::optional<Color> const text = c.color("toolbar_field_text"))
        c.set("address-text", *text);
    if (std::optional<Color> const border = c.color("toolbar_field_border"))
        c.set("address-border", *border);
    if (std::optional<Color> const field = c.color("toolbar_field_focus"))
        c.set("address-background-focus", *field);
    if (std::optional<Color> const text = c.color("toolbar_field_text_focus"))
        c.set("address-text-focus", *text);
    if (std::optional<Color> const border = c.color("toolbar_field_border_focus"))
        c.set("address-border-focus", *border);
    if (std::optional<Color> const band = c.color("toolbar_field_highlight"))
        c.set("address-selection", *band);
    if (std::optional<Color> const text = c.color("toolbar_field_highlight_text"))
        c.set("address-selection-text", *text);

    Color popup = palette.chrome_background;
    Color popup_text = palette.chrome_text;
    if (std::optional<Color> const named = c.color("popup")) {
        popup = *named;
        c.set("popup-background", popup);
    }
    if (std::optional<Color> const named = c.color("popup_text")) {
        popup_text = *named;
        c.set("popup-text", popup_text);
        c.set("popup-highlight-text", popup_text);
    }
    if (std::optional<Color> const border = c.color("popup_border"))
        c.set("popup-border", *border);
    if (std::optional<Color> const band = c.color("popup_highlight"))
        c.set("popup-highlight", *band);
    if (std::optional<Color> const text = c.color("popup_highlight_text"))
        c.set("popup-highlight-text", *text);
    fade_popup_texts(c, popup_text, composite(popup, palette.content_background));

    // The status line is chrome that browser does not have: it wears the frame.
    c.set("status-background", composite(frame, palette.chrome_background));
    c.set("status-text", strip_text);
    if (std::optional<Color> const accent = c.color("tab_line"))
        c.set("accent", with_alpha(*accent, 255));

    if (std::optional<Color> const background = c.color("ntp_background"))
        c.new_tab += "    \"background\": " + json_text(hex(*background)) + ",\n";
    if (std::optional<Color> const text = c.color("ntp_text")) {
        c.new_tab += "    \"text\": " + json_text(hex(*text)) + ",\n";
        Color const under = c.color("ntp_background").value_or(frame);
        c.new_tab += "    \"text-muted\": " + json_text(hex(towards(with_alpha(under, 255), with_alpha(*text, 255), 0.62))) + ",\n";
    }

    // The header's pictures: theme_frame in front, the additional ones
    // behind it in their order, each held and repeated as the properties
    // say — a list shorter than the pictures is gone round again.
    if (c.images) {
        if (JsonValue const* const front = c.images->get("theme_frame")) {
            std::string const file = c.picture("images.theme_frame", *front, "frame-0");
            if (!file.empty())
                c.frame_layers.push_back(Conversion::layer(file, "right top", "no-repeat"));
        }
        if (JsonValue const* const more = c.images->get("additional_backgrounds"); more && more->is_array()) {
            auto const list_of = [&](char const* key) -> JsonValue::Array const* {
                JsonValue const* const value = c.properties ? c.properties->get(key) : nullptr;
                return value && value->is_array() && !value->as_array().empty() ? &value->as_array() : nullptr;
            };
            JsonValue::Array const* const alignments = list_of("additional_backgrounds_alignment");
            JsonValue::Array const* const tilings = list_of("additional_backgrounds_tiling");
            JsonValue::Array const* const sizes = list_of("additional_backgrounds_size");
            std::size_t index = 0;
            for (JsonValue const& entry : more->as_array()) {
                std::string const key = "images.additional_backgrounds[" + std::to_string(index) + "]";
                std::string const file = c.picture(key, entry, "frame-" + std::to_string(index + 1));
                std::string align = "right top";
                std::string tile = "no-repeat";
                if (alignments) {
                    JsonValue const& said = (*alignments)[index % alignments->size()];
                    if (said.is_string() && is_picture_alignment(said.as_string()))
                        align = said.as_string();
                    else
                        c.out.notes.push_back(key + ": an alignment this does not read");
                }
                if (tilings) {
                    JsonValue const& said = (*tilings)[index % tilings->size()];
                    if (said.is_string() && is_picture_tiling(said.as_string()))
                        tile = said.as_string();
                    else
                        c.out.notes.push_back(key + ": a tiling this does not read");
                }
                if (sizes) {
                    JsonValue const& said = (*sizes)[index % sizes->size()];
                    if (!said.is_string() || said.as_string() != "auto")
                        c.out.notes.push_back(key + ": a size other than its own has nowhere to land yet");
                }
                if (!file.empty())
                    c.frame_layers.push_back(Conversion::layer(file, align, tile));
                ++index;
            }
        }
    }
    if (c.properties) {
        if (JsonValue const* const area = c.properties->get("backgrounds_area");
            area && area->is_string() && area->as_string() == "window")
            c.out.notes.push_back("properties.backgrounds_area: pictures behind the whole window have nowhere to land yet");
    }

    note_unknown(c, c.colors, "colors",
        { "frame", "frame_inactive", "tab_background_text", "tab_text", "toolbar_text", "bookmark_text", "toolbar",
            "tab_selected", "tab_line", "icons", "button_background_hover", "button_background_active",
            "toolbar_bottom_separator", "toolbar_top_separator", "toolbar_field", "toolbar_field_text",
            "toolbar_field_border", "toolbar_field_focus", "toolbar_field_text_focus", "toolbar_field_border_focus",
            "toolbar_field_highlight", "toolbar_field_highlight_text", "popup", "popup_text", "popup_border",
            "popup_highlight", "popup_highlight_text", "ntp_background", "ntp_text" },
        { "icons_attention", "tab_loading", "tab_background_separator", "toolbar_vertical_separator",
            "toolbar_field_separator", "sidebar", "sidebar_border", "sidebar_text", "sidebar_highlight",
            "sidebar_highlight_text", "ntp_card_background" });
    note_unknown(c, c.images, "images", { "theme_frame", "additional_backgrounds" }, {});
    note_unknown(c, c.properties, "properties",
        { "additional_backgrounds_alignment", "additional_backgrounds_tiling", "additional_backgrounds_size",
            "backgrounds_area", "color_scheme" },
        { "content_color_scheme" });
}

void convert_chrome(Conversion& c)
{
    // That browser's own colors, where a theme of its says nothing: the
    // frame and the toolbar of its light look.
    Color const given_frame = c.color("frame").value_or(Color::rgb(0xDE, 0xE1, 0xE6));
    Color const given_toolbar = c.color("toolbar").value_or(Color::rgb(0xFF, 0xFF, 0xFF));
    bool const dark = wants_dark_palette(c, { "bookmark_text", "toolbar_text", "tab_text" }, { "toolbar", "frame" });
    Theme const palette = dark ? Theme {} : Theme::from_json(light_palette);
    write_palette(c, palette);

    HslTint const frame_tint = c.tint("frame");
    Color const frame = apply_tint(given_frame, frame_tint);
    c.set("chrome-background", frame);
    // A frame not in front that the theme does not color is its frame
    // under that browser's tint for one — its own default where the theme
    // gives none: a little lighter.
    c.set("chrome-background-inactive",
        c.color("frame_inactive").value_or(apply_tint(frame, c.tint("frame_inactive", HslTint { -1, -1, 0.642 }))));
    c.set("toolbar-background", given_toolbar);
    c.set("tab-active-background", given_toolbar);
    Color const toolbar_text = [&] {
        std::optional<Color> text = c.color("toolbar_text");
        if (!text)
            text = c.color("bookmark_text");
        return text.value_or(palette.chrome_text);
    }();
    c.set("tab-text", c.color("tab_text").value_or(toolbar_text));
    Color const strip_text = c.color("tab_background_text").value_or(lightness_of(frame) < 0.5 ? Color::rgb(0xff, 0xff, 0xff) : Color::rgb(0x3c, 0x40, 0x43));
    c.set("chrome-text", strip_text);
    c.set("chrome-text-muted", strip_text);
    // A background tab is the frame seen through unless the theme colors
    // it; the tint for one goes into whatever it wears.
    HslTint const tab_tint = c.tint("background_tab");
    if (std::optional<Color> const tab = c.color("background_tab"))
        c.set("tab-inactive-background", apply_tint(*tab, tab_tint));
    else
        c.set("tab-inactive-background", Color::rgba(0, 0, 0, 0));
    c.set("tab-hover-background", with_alpha(strip_text, 0x1c));
    // The buttons' glyphs: the theme's color for them, else that browser's
    // own gray under the theme's tint for buttons, else the toolbar's text.
    HslTint const buttons = c.tint("buttons");
    Color icons = toolbar_text;
    if (std::optional<Color> const named = c.color("toolbar_button_icon"))
        icons = *named;
    else if (!buttons.changes_nothing())
        icons = apply_tint(Color::rgb(0x5f, 0x63, 0x68), buttons);
    c.set("toolbar-icon", icons);
    c.set("button-hover-background", with_alpha(icons, 0x2b));
    c.set("button-active-background", with_alpha(icons, 0x4d));
    if (std::optional<Color> const field = c.color("omnibox_background"))
        c.set("address-background", *field);
    if (std::optional<Color> const text = c.color("omnibox_text"))
        c.set("address-text", *text);
    fade_popup_texts(c, palette.chrome_text, palette.chrome_background);
    c.set("status-background", composite(frame, palette.chrome_background));
    c.set("status-text", strip_text);

    if (std::optional<Color> const background = c.color("ntp_background"))
        c.new_tab += "    \"background\": " + json_text(hex(*background)) + ",\n";
    if (std::optional<Color> const text = c.color("ntp_text")) {
        c.new_tab += "    \"text\": " + json_text(hex(*text)) + ",\n";
        Color const under = c.color("ntp_background").value_or(Color::rgb(0xff, 0xff, 0xff));
        c.new_tab += "    \"text-muted\": " + json_text(hex(towards(with_alpha(under, 255), with_alpha(*text, 255), 0.62))) + ",\n";
    }

    // That browser holds its pictures at the top left and repeats them
    // across; the overlay lies over the frame's, once.
    if (c.images) {
        if (JsonValue const* const overlay = c.images->get("theme_frame_overlay")) {
            std::string const file = c.picture("images.theme_frame_overlay", *overlay, "frame-0");
            if (!file.empty())
                c.frame_layers.push_back(Conversion::layer(file, "left top", "no-repeat"));
        }
        if (JsonValue const* const picture = c.images->get("theme_frame")) {
            std::string const file = c.picture("images.theme_frame", *picture, "frame-1", frame_tint);
            if (!file.empty())
                c.frame_layers.push_back(Conversion::layer(file, "left top", "repeat-x"));
        }
        if (JsonValue const* const picture = c.images->get("theme_toolbar")) {
            std::string const file = c.picture("images.theme_toolbar", *picture, "toolbar");
            if (!file.empty())
                c.toolbar_layer = Conversion::layer(file, "left top", "repeat-x");
        }
        if (JsonValue const* const picture = c.images->get("theme_tab_background")) {
            std::string const file = c.picture("images.theme_tab_background", *picture, "tab-background", tab_tint);
            if (!file.empty())
                c.tab_layer = Conversion::layer(file, "left top", "repeat-x");
        }
        if (JsonValue const* const picture = c.images->get("theme_ntp_background")) {
            std::string const file = c.picture("images.theme_ntp_background", *picture, "new-tab/background");
            if (!file.empty())
                c.new_tab += "    \"backgrounds\": \"new-tab\",\n    \"rotate\": 0,\n";
        }
    }
    if (c.properties) {
        for (char const* key : { "ntp_background_alignment", "ntp_background_repeat" }) {
            if (c.properties->get(key))
                c.out.notes.push_back(std::string("properties.") + key
                    + ": the new-tab page fills itself with its picture; holding and repeating it has nowhere to land yet");
        }
    }

    note_unknown(c, c.colors, "colors",
        { "frame", "frame_inactive", "toolbar", "toolbar_text", "bookmark_text", "tab_text", "tab_background_text",
            "background_tab", "toolbar_button_icon", "omnibox_background", "omnibox_text", "ntp_background", "ntp_text" },
        { "frame_incognito", "frame_incognito_inactive", "background_tab_inactive", "background_tab_incognito",
            "background_tab_incognito_inactive", "tab_background_text_inactive", "tab_background_text_incognito",
            "tab_background_text_incognito_inactive", "button_background", "ntp_header", "ntp_link", "ntp_section",
            "ntp_section_text", "ntp_section_link", "control_background" });
    note_unknown(c, c.images, "images",
        { "theme_frame", "theme_frame_overlay", "theme_toolbar", "theme_tab_background", "theme_ntp_background" },
        { "theme_frame_inactive", "theme_frame_incognito", "theme_frame_incognito_inactive",
            "theme_frame_overlay_inactive", "theme_tab_background_inactive", "theme_tab_background_incognito",
            "theme_tab_background_incognito_inactive", "theme_tab_background_v", "theme_button_background",
            "theme_ntp_attribution", "theme_window_control_background" });
    note_unknown(c, c.tints, "tints", { "frame", "frame_inactive", "background_tab", "buttons" },
        { "frame_incognito", "frame_incognito_inactive" });
    note_unknown(c, c.properties, "properties", { "ntp_background_alignment", "ntp_background_repeat" },
        { "ntp_logo_alternate" });
}

// Whether a manifest with no container to say so is Chrome's: by what only
// that browser writes. A CSS color string is only Firefox's.
bool looks_like_chrome(JsonValue const& theme)
{
    if (theme.get("tints"))
        return true;
    JsonValue const* const colors = theme.get("colors");
    if (colors && colors->is_object()) {
        for (auto const& [key, value] : colors->as_object()) {
            if (value.is_string())
                return false;
            for (char const* only : { "toolbar_button_icon", "omnibox_background", "omnibox_text", "background_tab",
                     "ntp_link", "ntp_header", "button_background", "control_background" }) {
                if (key == only)
                    return true;
            }
        }
    }
    JsonValue const* const images = theme.get("images");
    if (images && images->is_object()) {
        for (char const* only : { "theme_toolbar", "theme_tab_background", "theme_ntp_background", "theme_frame_overlay" }) {
            if (images->get(only))
                return true;
        }
    }
    JsonValue const* const properties = theme.get("properties");
    return properties && properties->is_object()
        && (properties->get("ntp_background_alignment") || properties->get("ntp_background_repeat"));
}

std::optional<std::vector<std::uint8_t>> read_whole(std::filesystem::path const& path, std::uintmax_t most)
{
    std::error_code error;
    std::uintmax_t const size = std::filesystem::file_size(path, error);
    if (error || size > most)
        return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string lowered_extension(std::filesystem::path const& path)
{
    std::string extension = path.extension().string();
    for (char& c : extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension;
}

}

std::optional<ImportedTheme> import_browser_theme(std::string_view manifest, ThemeFileReader const& read,
    std::optional<BrowserThemeKind> container, std::vector<std::string>* problems)
{
    auto const refuse = [&](std::string const& why) -> std::optional<ImportedTheme> {
        if (problems)
            problems->push_back("theme import: " + why);
        return std::nullopt;
    };
    std::optional<JsonValue> const root = JsonValue::parse(manifest);
    if (!root || !root->is_object())
        return refuse("manifest.json is not a JSON object");
    JsonValue const* const theme = root->get("theme");
    if (!theme || !theme->is_object())
        return refuse("manifest.json describes no theme (an extension of another kind?)");

    Conversion c(read);
    if (JsonValue const* const name = root->get("name"); name && name->is_string() && !name->as_string().empty())
        c.out.name = name->as_string();
    else
        c.out.name = "Imported theme";
    // A name that is a message's key — __MSG_extensionName__ — is looked
    // up where an extension keeps its words: _locales/<locale>/messages.json,
    // the manifest's default locale first, the key matched whatever its
    // case. With no such message the key's own words stand.
    if (c.out.name.starts_with("__MSG_") && c.out.name.ends_with("__") && c.out.name.size() > 8) {
        std::string const key = c.out.name.substr(6, c.out.name.size() - 8);
        std::vector<std::string> locales;
        if (JsonValue const* const locale = root->get("default_locale"); locale && locale->is_string())
            locales.push_back(locale->as_string());
        for (char const* usual : { "en", "en_US", "en_GB" })
            locales.push_back(usual);
        std::string found;
        for (std::string const& locale : locales) {
            std::optional<std::string> const path = inside_path("_locales/" + locale + "/messages.json");
            std::optional<std::vector<std::uint8_t>> const bytes = path ? read(*path) : std::nullopt;
            if (!bytes || bytes->size() > max_manifest_bytes)
                continue;
            std::optional<JsonValue> const messages
                = JsonValue::parse(std::string_view(reinterpret_cast<char const*>(bytes->data()), bytes->size()));
            if (!messages || !messages->is_object())
                continue;
            for (auto const& [name, entry] : messages->as_object()) {
                JsonValue const* const message = entry.is_object() ? entry.get("message") : nullptr;
                if (ascii_ci_equals(name, key) && message && message->is_string() && !message->as_string().empty())
                    found = message->as_string();
            }
            if (!found.empty())
                break;
        }
        if (found.empty()) {
            found = key;
            std::replace(found.begin(), found.end(), '_', ' ');
        }
        c.out.name = found;
    }
    auto const section = [&](char const* key) -> JsonValue const* {
        JsonValue const* const value = theme->get(key);
        return value && value->is_object() ? value : nullptr;
    };
    c.colors = section("colors");
    c.images = section("images");
    c.properties = section("properties");
    c.tints = section("tints");
    c.out.kind = container.value_or(looks_like_chrome(*theme) ? BrowserThemeKind::Chrome : BrowserThemeKind::Firefox);
    if (c.out.kind == BrowserThemeKind::Chrome)
        convert_chrome(c);
    else
        convert_firefox(c);

    // The tabs sit as that theme's browser sets them: Firefox's float in the
    // strip, Chrome's stand on the toolbar — which is what makes a front
    // tab that is not the toolbar's color look meant, not broken.
    std::string json = "{\n  \"name\": " + json_text(c.out.name) + ",\n  \"tab-shape\": "
        + (c.out.kind == BrowserThemeKind::Firefox ? "\"floating\"" : "\"attached\"") + ",\n  \"colors\": {\n";
    std::size_t written = 0;
    for (auto const& [token, value] : c.tokens)
        json += "    " + json_text(token) + ": " + json_text(value) + (++written < c.tokens.size() ? ",\n" : "\n");
    json += "  }";
    if (!c.frame_layers.empty() || !c.toolbar_layer.empty() || !c.tab_layer.empty()) {
        json += ",\n  \"images\": {\n";
        std::vector<std::string> members;
        if (!c.frame_layers.empty()) {
            std::string list = "    \"frame\": [\n";
            for (std::size_t i = 0; i < c.frame_layers.size(); ++i)
                list += "      " + c.frame_layers[i] + (i + 1 < c.frame_layers.size() ? ",\n" : "\n");
            members.push_back(list + "    ]");
        }
        if (!c.toolbar_layer.empty())
            members.push_back("    \"toolbar\": " + c.toolbar_layer);
        if (!c.tab_layer.empty())
            members.push_back("    \"tab-background\": " + c.tab_layer);
        for (std::size_t i = 0; i < members.size(); ++i)
            json += members[i] + (i + 1 < members.size() ? ",\n" : "\n");
        json += "  }";
    }
    if (!c.new_tab.empty()) {
        std::string members = c.new_tab;
        members.erase(members.size() - 2); // the last comma and its newline
        json += ",\n  \"new-tab\": {\n" + members + "\n  }";
    }
    json += "\n}\n";
    c.out.json = std::move(json);
    return std::move(c.out);
}

bool is_browser_theme_path(std::string const& path)
{
    std::error_code error;
    std::filesystem::path const given(path);
    if (std::filesystem::is_directory(given, error))
        return std::filesystem::is_regular_file(given / "manifest.json", error);
    std::string const extension = lowered_extension(given);
    return extension == ".xpi" || extension == ".crx" || extension == ".zip"
        || given.filename() == "manifest.json";
}

std::optional<ImportedTheme> import_browser_theme_from(std::string const& path, std::vector<std::string>* problems)
{
    auto const refuse = [&](std::string const& why) -> std::optional<ImportedTheme> {
        if (problems)
            problems->push_back("theme import: " + why + ": " + path);
        return std::nullopt;
    };
    std::error_code error;
    std::filesystem::path given(path);
    if (std::filesystem::is_regular_file(given, error) && given.filename() == "manifest.json")
        given = given.parent_path();
    if (std::filesystem::is_directory(given, error)) {
        std::optional<std::vector<std::uint8_t>> const manifest = read_whole(given / "manifest.json", max_manifest_bytes);
        if (!manifest)
            return refuse("no manifest.json to read in");
        ThemeFileReader const read = [&](std::string const& inside) {
            return read_whole(given / std::filesystem::path(inside), max_picture_bytes);
        };
        return import_browser_theme(std::string_view(reinterpret_cast<char const*>(manifest->data()), manifest->size()),
            read, std::nullopt, problems);
    }
    std::optional<std::vector<std::uint8_t>> bytes = read_whole(given, max_archive_bytes);
    if (!bytes)
        return refuse("cannot read");
    std::string const extension = lowered_extension(given);
    std::optional<BrowserThemeKind> container;
    if (extension == ".xpi")
        container = BrowserThemeKind::Firefox;
    else if (extension == ".crx")
        container = BrowserThemeKind::Chrome;
    return import_browser_theme_archive(std::move(*bytes), container, path, problems);
}

std::optional<ImportedTheme> import_browser_theme_archive(std::vector<std::uint8_t> bytes,
    std::optional<BrowserThemeKind> container, std::string const& named, std::vector<std::string>* problems)
{
    auto const refuse = [&](std::string const& why) -> std::optional<ImportedTheme> {
        if (problems)
            problems->push_back("theme import: " + why + ": " + named);
        return std::nullopt;
    };
    if (bytes.size() > max_archive_bytes)
        return refuse("too large an archive");
    std::optional<ZipArchive> const archive = ZipArchive::open(std::move(bytes));
    if (!archive)
        return refuse("not an archive this reads");
    ZipEntry const* const entry = archive->find("manifest.json");
    std::optional<std::vector<std::uint8_t>> const manifest
        = entry ? archive->read(*entry, max_manifest_bytes) : std::nullopt;
    if (!manifest)
        return refuse("no manifest.json to read in");
    ThemeFileReader const read = [&](std::string const& inside) -> std::optional<std::vector<std::uint8_t>> {
        ZipEntry const* const file = archive->find(inside);
        return file ? archive->read(*file, max_picture_bytes) : std::nullopt;
    };
    return import_browser_theme(std::string_view(reinterpret_cast<char const*>(manifest->data()), manifest->size()),
        read, container, problems);
}

std::optional<std::string> write_imported_theme(ImportedTheme const& theme, std::string const& directory,
    std::vector<std::string>* problems)
{
    // The folder's name: the theme's, in what every file system takes.
    std::string slug;
    for (char const c : theme.name) {
        auto const byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte))
            slug += static_cast<char>(std::tolower(byte));
        else if (!slug.empty() && slug.back() != '-')
            slug += '-';
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    if (slug.empty())
        slug = "imported-theme";
    slug.resize(std::min<std::size_t>(slug.size(), 64));
    std::filesystem::path const folder = std::filesystem::path(directory) / slug;
    auto const refuse = [&](std::string const& why) -> std::optional<std::string> {
        if (problems)
            problems->push_back("theme import: " + why + ": " + folder.string());
        return std::nullopt;
    };
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error)
        return refuse("cannot make the folder");
    auto const write = [&](std::filesystem::path const& file, char const* data, std::size_t size) {
        std::filesystem::create_directories(file.parent_path(), error);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(data, static_cast<std::streamsize>(size));
        return static_cast<bool>(out);
    };
    for (ImportedTheme::Picture const& picture : theme.pictures) {
        if (!write(folder / std::filesystem::path(picture.file), reinterpret_cast<char const*>(picture.bytes.data()),
                picture.bytes.size()))
            return refuse("cannot write " + picture.file + " in");
    }
    std::string notes = "Converted from a " + std::string(theme.kind == BrowserThemeKind::Chrome ? "Chrome" : "Firefox")
        + " theme: " + theme.name + "\n";
    if (theme.notes.empty())
        notes += "Everything it says was carried over.\n";
    else
        notes += "What it says that was not carried over:\n";
    for (std::string const& note : theme.notes)
        notes += "  " + note + "\n";
    if (!write(folder / "import-notes.txt", notes.data(), notes.size()))
        return refuse("cannot write the notes in");
    std::filesystem::path const file = folder / "theme.json";
    if (!write(file, theme.json.data(), theme.json.size()))
        return refuse("cannot write theme.json in");
    return file.string();
}

}
