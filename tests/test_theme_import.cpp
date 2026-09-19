// Firefox and Chrome themes converted into ours: from a folder, an .xpi and
// a .crx; the fallbacks between a theme's keys; Chrome's tints worked into
// colors and pictures; what has nowhere to land, said by key; and what is
// written to disk, read back as a theme with nothing wrong in it.
//
// The numbers are worked out by hand. Chrome's frame (32, 48, 64) under the
// theme's tint of lightness 0.75 moves each channel half way to white:
// (144, 152, 160). Its frame for a window not in front, which the theme
// does not give, is that under Chrome's own tint for one, lightness 0.642 —
// 28.4% of the way to white: (176, 181, 187). Its buttons' glyphs are
// Chrome's gray (95, 99, 104) with the hue turned to 0.5, which at that
// gray's saturation and lightness is (95, 104, 104).

#include "Test.h"

#include "core/Png.h"
#include "ui/Theme.h"
#include "ui/ThemeImport.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace sashfold;
using namespace sashfold::ui;

static bool holds(std::vector<std::string> const& lines, std::string const& what)
{
    return std::any_of(lines.begin(), lines.end(),
        [&](std::string const& line) { return line.find(what) != std::string::npos; });
}

static ImportedTheme::Picture const* picture_named(ImportedTheme const& theme, std::string const& file)
{
    for (ImportedTheme::Picture const& picture : theme.pictures) {
        if (picture.file == file)
            return &picture;
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    std::string const fixtures = argc > 1 ? argv[1] : "tests/fixtures/themes";
    std::string const light_theme = argc > 2 ? argv[2] : "themes/light.json";

    // --- A tint, as Chrome applies one ------------------------------------------
    {
        Color const gray = Color::rgb(95, 99, 104);
        CHECK(apply_tint(gray, HslTint {}) == gray); // -1 three times changes nothing
        CHECK(apply_tint(gray, HslTint { -1, 0.5, 0.5 }) == gray); // the middle leaves each as it is
        CHECK(apply_tint(Color::rgb(32, 48, 64), HslTint { -1, -1, 0.75 }) == Color::rgb(144, 152, 160));
        CHECK(apply_tint(Color::rgb(144, 152, 160), HslTint { -1, -1, 0.642 }) == Color::rgb(176, 181, 187));
        CHECK(apply_tint(Color::rgb(200, 100, 50), HslTint { -1, -1, 0 }) == Color::rgb(0, 0, 0));
        CHECK(apply_tint(Color::rgb(200, 100, 50), HslTint { -1, -1, 1 }) == Color::rgb(255, 255, 255));
        CHECK(apply_tint(Color::rgb(200, 100, 50), HslTint { -1, -1, 0.25 }) == Color::rgb(100, 50, 25));
        // No saturation left is a gray of the color's lightness: (200 + 50) / 2.
        CHECK(apply_tint(Color::rgb(200, 100, 50), HslTint { -1, 0, -1 }) == Color::rgb(125, 125, 125));
        CHECK(apply_tint(gray, HslTint { 0.5, -1, -1 }) == Color::rgb(95, 104, 104));
        CHECK(apply_tint(Color::rgba(32, 48, 64, 77), HslTint { -1, -1, 0.75 }).a == 77); // alpha is kept
    }

    // --- A Firefox theme, from its folder ---------------------------------------
    std::string fox_json;
    {
        std::vector<std::string> problems;
        std::optional<ImportedTheme> const fox = import_browser_theme_from(fixtures + "/firefox-sample", &problems);
        CHECK(fox.has_value());
        CHECK_EQ(problems.size(), std::size_t { 0 });
        if (fox) {
            fox_json = fox->json;
            CHECK(fox->kind == BrowserThemeKind::Firefox);
            CHECK_EQ(fox->name, "Sample Fox");
            std::vector<std::string> theme_problems;
            Theme const theme = Theme::from_json(fox->json, &theme_problems);
            for (std::string const& problem : theme_problems)
                std::cerr << "  " << problem << "\n";
            CHECK_EQ(theme_problems.size(), std::size_t { 0 });
            CHECK_EQ(theme.name, "Sample Fox");
            // What it says, in every way it may be said: hex, rgb(), rgba(),
            // rgb() again, hsl(), an array.
            CHECK(theme.chrome_background == Color::rgb(0x20, 0x30, 0x40));
            CHECK(theme.chrome_background_inactive == Color::rgb(64, 80, 96));
            CHECK(theme.toolbar_background == Color::rgba(16, 32, 48, 128));
            CHECK(theme.address_background == Color::rgb(255, 255, 255));
            CHECK(theme.address_text == Color::rgb(10, 20, 30));
            CHECK(theme.tab_line == Color::rgb(0xff, 0x80, 0x00));
            CHECK(theme.popup_background == Color::rgb(0x30, 0x00, 0x30));
            CHECK(theme.popup_text == Color::rgb(0xf0, 0xf0, 0xf0));
            CHECK(theme.new_tab_background == Color::rgb(0x10, 0x20, 0x30));
            CHECK(theme.new_tab_text == Color::rgb(0xf0, 0xe0, 0x00));
            // What it leaves unsaid, as that browser fills it in: the strip's
            // words one color; the front tab's and the icons' the toolbar's
            // text; the front tab the toolbar's color; background tabs the
            // frame seen through; menus' highlighted text their text.
            CHECK(theme.chrome_text == Color::rgb(255, 255, 255));
            CHECK(theme.chrome_text_muted == Color::rgb(255, 255, 255));
            CHECK(theme.tab_text == Color::rgb(250, 240, 230)); // the toolbar's text, not the strip's
            CHECK(theme.toolbar_icon == Color::rgb(250, 240, 230));
            CHECK(theme.tab_active_background == theme.toolbar_background);
            CHECK_EQ(static_cast<int>(theme.tab_inactive_background.a), 0);
            CHECK(theme.tab_hover_background == Color::rgba(255, 255, 255, 0x1c));
            CHECK(theme.button_hover_background == Color::rgba(250, 240, 230, 0x2b)); // the icons' color, faintly
            CHECK(theme.button_active_background == Color::rgba(250, 240, 230, 0x4d));
            CHECK(theme.popup_highlight_text == Color::rgb(0xf0, 0xf0, 0xf0));
            // Its words are bright, so the rest is the dark palette's — ours.
            CHECK(theme.accent == Color::rgb(0xff, 0x80, 0x00)); // but the accent is its tab line
            CHECK(theme.selection == Theme {}.selection);
            CHECK(theme.address_border == Theme {}.address_border);
            CHECK(theme.popup_border == Theme {}.address_border);
            // Its pictures: theme_frame in front, held at the top right and
            // shown once; the additional one behind, as its properties say.
            CHECK_EQ(theme.frame_pictures.size(), std::size_t { 2 });
            if (theme.frame_pictures.size() == 2) {
                CHECK_EQ(theme.frame_pictures[0].path, "frame-0.png");
                CHECK(theme.frame_pictures[0].across == ThemePicture::Hold::End);
                CHECK(theme.frame_pictures[0].down == ThemePicture::Hold::Start);
                CHECK(!theme.frame_pictures[0].repeat_across);
                CHECK_EQ(theme.frame_pictures[1].path, "frame-1.png");
                CHECK(theme.frame_pictures[1].across == ThemePicture::Hold::Start);
                CHECK(theme.frame_pictures[1].repeat_across);
                CHECK(theme.frame_pictures[1].repeat_down);
            }
            CHECK_EQ(fox->pictures.size(), std::size_t { 2 });
            CHECK(picture_named(*fox, "frame-0.png") != nullptr);
            CHECK(picture_named(*fox, "frame-1.png") != nullptr);
            // What has nowhere to land is said, and only that.
            CHECK_EQ(fox->notes.size(), std::size_t { 1 });
            CHECK(holds(fox->notes, "colors.sidebar: has no surface here yet"));
        }
    }

    // --- The same theme as an .xpi is the same theme ----------------------------
    {
        std::vector<std::string> problems;
        std::optional<ImportedTheme> const fox = import_browser_theme_from(fixtures + "/firefox-sample.xpi", &problems);
        CHECK(fox.has_value());
        CHECK_EQ(problems.size(), std::size_t { 0 });
        if (fox) {
            CHECK(!fox_json.empty());
            CHECK_EQ(fox->json, fox_json);
            CHECK_EQ(fox->pictures.size(), std::size_t { 2 });
            if (ImportedTheme::Picture const* const front = picture_named(*fox, "frame-0.png")) {
                std::optional<Bitmap> const decoded = decode_png(front->bytes);
                CHECK(decoded.has_value());
                if (decoded)
                    CHECK(decoded->pixel(0, 0) == Color::rgb(0x00, 0xff, 0x00));
            } else {
                CHECK(false);
            }
        }
    }

    // --- A Chrome theme, from its folder and from a .crx ------------------------
    for (std::string const source : { "/chrome-sample", "/chrome-sample.crx" }) {
        std::vector<std::string> problems;
        std::optional<ImportedTheme> const chrome = import_browser_theme_from(fixtures + source, &problems);
        CHECK(chrome.has_value());
        CHECK_EQ(problems.size(), std::size_t { 0 });
        if (!chrome)
            continue;
        CHECK(chrome->kind == BrowserThemeKind::Chrome);
        std::vector<std::string> theme_problems;
        Theme const theme = Theme::from_json(chrome->json, &theme_problems);
        for (std::string const& problem : theme_problems)
            std::cerr << "  " << problem << "\n";
        CHECK_EQ(theme_problems.size(), std::size_t { 0 });
        CHECK_EQ(theme.name, "Sample Chrome");
        CHECK(theme.chrome_background == Color::rgb(144, 152, 160)); // the frame under its tint
        CHECK(theme.chrome_background_inactive == Color::rgb(176, 181, 187)); // and under that browser's own for a window behind
        CHECK(theme.toolbar_background == Color::rgb(240, 240, 240));
        CHECK(theme.tab_active_background == Color::rgb(240, 240, 240)); // the front tab is the toolbar's
        CHECK(theme.tab_text == Color::rgb(10, 20, 30));
        CHECK(theme.chrome_text_muted == Color::rgb(250, 250, 250));
        CHECK_EQ(static_cast<int>(theme.tab_inactive_background.a), 0);
        CHECK(theme.toolbar_icon == Color::rgb(95, 104, 104)); // that browser's gray under the buttons' tint
        CHECK(theme.new_tab_background == Color::rgb(16, 32, 48)); // a fourth element is alpha, 0 to 1
        // Its words are dark, so the rest is the light palette's: the shipped
        // light theme's, which this holds the converter's copy equal to.
        std::optional<Theme> const light = Theme::load(light_theme);
        CHECK(light.has_value());
        if (light) {
            CHECK(theme.accent == light->accent);
            CHECK(theme.selection == light->selection);
            CHECK(theme.address_background == light->address_background);
            CHECK(theme.address_text == light->address_text);
            CHECK(theme.address_border == light->address_border);
            CHECK(theme.chrome_border == light->chrome_border);
            CHECK(theme.find_highlight == light->find_highlight);
            CHECK(theme.find_current == light->find_current);
            CHECK(theme.hint_background == light->hint_background);
            CHECK(theme.hint_text == light->hint_text);
            CHECK(theme.button_disabled_text == light->button_disabled_text);
            CHECK(theme.content_background == light->content_background);
            CHECK(theme.secure_indicator == light->secure_indicator);
            CHECK(theme.insecure_indicator == light->insecure_indicator);
            CHECK(theme.popup_background == light->chrome_background);
            CHECK(theme.popup_text == light->chrome_text);
            CHECK(theme.popup_highlight == light->button_hover_background);
        }
        // That browser holds its pictures at the top left and repeats them
        // across; the frame's has the frame's tint in its pixels.
        CHECK_EQ(theme.frame_pictures.size(), std::size_t { 1 });
        if (theme.frame_pictures.size() == 1) {
            CHECK(theme.frame_pictures[0].across == ThemePicture::Hold::Start);
            CHECK(theme.frame_pictures[0].down == ThemePicture::Hold::Start);
            CHECK(theme.frame_pictures[0].repeat_across);
            CHECK(!theme.frame_pictures[0].repeat_down);
            if (ImportedTheme::Picture const* const frame = picture_named(*chrome, theme.frame_pictures[0].path)) {
                std::optional<Bitmap> const decoded = decode_png(frame->bytes);
                CHECK(decoded.has_value());
                if (decoded) {
                    CHECK(decoded->pixel(2, 2) == Color::rgb(228, 178, 153)); // (200, 100, 50) half way to white
                    CHECK_EQ(static_cast<int>(decoded->pixel(0, 0).a), 0); // what was clear stays clear
                }
            } else {
                CHECK(false);
            }
        }
        CHECK_EQ(theme.toolbar_pictures.size(), std::size_t { 1 });
        CHECK(theme.tab_background_pictures.empty());
        CHECK_EQ(chrome->notes.size(), std::size_t { 2 });
        CHECK(holds(chrome->notes, "colors.ntp_link: has no surface here yet"));
        CHECK(holds(chrome->notes, "properties.ntp_background_alignment"));
    }

    // --- Whose theme a bare manifest is ------------------------------------------
    {
        ThemeFileReader const none = [](std::string const&) { return std::nullopt; };
        std::optional<ImportedTheme> const strings = import_browser_theme(
            R"({ "name": "A", "theme": { "colors": { "frame": "#fff", "tab_background_text": "#000" } } })", none, std::nullopt);
        CHECK(strings && strings->kind == BrowserThemeKind::Firefox);
        std::optional<ImportedTheme> const arrays = import_browser_theme(
            R"({ "name": "B", "theme": { "colors": { "frame": [1, 2, 3] } } })", none, std::nullopt);
        CHECK(arrays && arrays->kind == BrowserThemeKind::Firefox); // arrays alone are either's: the older format's
        std::optional<ImportedTheme> const tinted = import_browser_theme(
            R"({ "name": "C", "theme": { "colors": { "frame": [1, 2, 3] }, "tints": { "buttons": [0.1, 0.5, 0.5] } } })", none,
            std::nullopt);
        CHECK(tinted && tinted->kind == BrowserThemeKind::Chrome);
        std::optional<ImportedTheme> const keyed = import_browser_theme(
            R"({ "name": "D", "theme": { "colors": { "toolbar_button_icon": [1, 2, 3] } } })", none, std::nullopt);
        CHECK(keyed && keyed->kind == BrowserThemeKind::Chrome);
        // The file it came in settles it.
        std::optional<ImportedTheme> const told = import_browser_theme(
            R"({ "name": "E", "theme": { "colors": { "frame": [1, 2, 3] } } })", none, BrowserThemeKind::Chrome);
        CHECK(told && told->kind == BrowserThemeKind::Chrome);
        // A light Firefox theme — dark words — takes the light palette.
        if (strings) {
            Theme const theme = Theme::from_json(strings->json);
            std::optional<Theme> const light = Theme::load(light_theme);
            CHECK(light.has_value());
            if (light)
                CHECK(theme.address_background == light->address_background);
            CHECK(theme.toolbar_background == Color::rgba(255, 255, 255, 0x66)); // an unsaid toolbar veils, in white under dark words
        }
        // A name that is a message's key reads as its words...
        std::optional<ImportedTheme> const keyed_name = import_browser_theme(
            R"({ "name": "__MSG_theme_name__", "theme": { "colors": {} } })", none, std::nullopt);
        CHECK(keyed_name && keyed_name->name == "theme name");
        // ...unless the theme keeps the message, in its default locale's
        // file: then it is the message, the key matched whatever its case.
        std::string asked_for;
        ThemeFileReader const locales = [&](std::string const& path) -> std::optional<std::vector<std::uint8_t>> {
            asked_for += path + ";";
            if (path != "_locales/de/messages.json")
                return std::nullopt;
            std::string const text = R"({ "ExtensionName": { "message": "Dunkler Raum", "description": "x" } })";
            return std::vector<std::uint8_t>(text.begin(), text.end());
        };
        std::optional<ImportedTheme> const translated = import_browser_theme(
            R"({ "name": "__MSG_extensionName__", "default_locale": "de", "theme": { "colors": {} } })", locales, std::nullopt);
        CHECK(translated && translated->name == "Dunkler Raum");
        CHECK(asked_for.starts_with("_locales/de/messages.json;")); // the default locale first
    }

    // --- What is no theme, and what a theme may not reach ------------------------
    {
        ThemeFileReader const none = [](std::string const&) { return std::nullopt; };
        std::vector<std::string> problems;
        CHECK(!import_browser_theme("not json", none, std::nullopt, &problems).has_value());
        CHECK(!import_browser_theme(R"({ "name": "An extension", "background": {} })", none, std::nullopt, &problems).has_value());
        CHECK_EQ(problems.size(), std::size_t { 2 });
        CHECK(!import_browser_theme_from(fixtures + "/no-such-theme", &problems).has_value());
        CHECK_EQ(problems.size(), std::size_t { 3 });
        // A picture's path stays inside the theme; a color that is none is said.
        bool asked = false;
        ThemeFileReader const watch = [&](std::string const&) -> std::optional<std::vector<std::uint8_t>> {
            asked = true;
            return std::nullopt;
        };
        std::optional<ImportedTheme> const reaching = import_browser_theme(
            R"({ "name": "R", "theme": { "images": { "theme_frame": "../../secret.png" },
                 "colors": { "frame": "#203040", "toolbar": "no color at all" } } })",
            watch, std::nullopt);
        CHECK(reaching.has_value());
        CHECK(!asked); // never even asked for
        if (reaching) {
            CHECK(holds(reaching->notes, "images.theme_frame: a path outside the theme"));
            CHECK(holds(reaching->notes, "colors.toolbar: not a color this reads"));
            CHECK(reaching->pictures.empty());
            CHECK(Theme::from_json(reaching->json).frame_pictures.empty());
        }
    }

    // --- Written to disk, it is a theme of ours -----------------------------------
    {
        std::filesystem::path const directory = std::filesystem::temp_directory_path() / "sashfold-test-theme-import";
        std::error_code error;
        // What this writes, by name: nothing is swept up that it did not make.
        auto const tidy = [&] {
            for (char const* file : { "theme.json", "import-notes.txt", "frame-0.png", "frame-1.png" })
                std::filesystem::remove(directory / "sample-fox" / file, error);
            std::filesystem::remove(directory / "sample-fox", error);
            std::filesystem::remove(directory, error);
        };
        tidy();
        std::vector<std::string> problems;
        std::optional<ImportedTheme> const fox = import_browser_theme_from(fixtures + "/firefox-sample.xpi", &problems);
        CHECK(fox.has_value());
        if (fox) {
            std::optional<std::string> const written = write_imported_theme(*fox, directory.string(), &problems);
            CHECK(written.has_value());
            CHECK_EQ(problems.size(), std::size_t { 0 });
            if (written) {
                CHECK_EQ(*written, (directory / "sample-fox" / "theme.json").string());
                std::vector<std::string> theme_problems;
                std::optional<Theme> const theme = Theme::load(*written, &theme_problems);
                CHECK(theme.has_value());
                CHECK_EQ(theme_problems.size(), std::size_t { 0 });
                if (theme && theme->frame_pictures.size() == 2) {
                    // Resolved against the theme file, and there.
                    CHECK(std::filesystem::is_regular_file(theme->frame_pictures[0].path, error));
                    CHECK(std::filesystem::is_regular_file(theme->frame_pictures[1].path, error));
                } else {
                    CHECK(false);
                }
                std::string text;
                {
                    // Closed before anything is removed: an open file cannot be, everywhere.
                    std::ifstream notes(directory / "sample-fox" / "import-notes.txt");
                    text.assign((std::istreambuf_iterator<char>(notes)), std::istreambuf_iterator<char>());
                }
                CHECK(text.find("Firefox theme: Sample Fox") != std::string::npos);
                CHECK(text.find("colors.sidebar") != std::string::npos);
            }
        }
        tidy();
        CHECK(!std::filesystem::exists(directory, error)); // and nothing was left that it did not name
    }

    return sashfold::test::report("theme_import");
}
