#include "Test.h"

#include "ui/Theme.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// ThemeTokens: the shipped default file equals the built-in defaults, every
// token overrides, and every kind of mistake degrades to a default with a
// named problem rather than a blank window.

using namespace sashfold;
using namespace sashfold::ui;

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: test_theme <themes/default.json>\n";
        return 2;
    }

    // --- Color syntax ---------------------------------------------------------
    CHECK(parse_theme_color("#1f2228") == Color::rgb(0x1f, 0x22, 0x28));
    CHECK(parse_theme_color("#ABC") == Color::rgb(0xaa, 0xbb, 0xcc));
    CHECK(parse_theme_color("#5b9cf666") == Color::rgba(0x5b, 0x9c, 0xf6, 0x66));
    CHECK(!parse_theme_color("1f2228").has_value());
    CHECK(!parse_theme_color("#1f22").has_value());
    CHECK(!parse_theme_color("#gg0000").has_value());
    CHECK(!parse_theme_color("").has_value());

    // --- The shipped default theme IS the defaults ----------------------------
    {
        std::vector<std::string> problems;
        std::optional<Theme> const shipped = Theme::load(argv[1], &problems);
        CHECK(shipped.has_value());
        CHECK_EQ(problems.size(), std::size_t { 0 });
        for (std::string const& problem : problems)
            std::cerr << "  " << problem << "\n";
        if (shipped)
            CHECK(*shipped == Theme {});
    }

    // --- Overrides land token by token ----------------------------------------
    {
        std::vector<std::string> problems;
        Theme const theme = Theme::from_json(
            "{ \"name\": \"Night\", \"colors\": { \"accent\": \"#ff0000\" },"
            "  \"metrics\": { \"tab-strip-height\": 44 }, \"type\": { \"font-size\": 16.5 },"
            "  \"timings\": { \"tab-hover\": 0 }, \"new-tab\": { \"backgrounds\": \"pictures\", \"rotate\": 5000 } }",
            &problems);
        CHECK_EQ(problems.size(), std::size_t { 0 });
        CHECK_EQ(theme.name, "Night");
        CHECK(theme.accent == Color::rgb(255, 0, 0));
        CHECK_EQ(theme.tab_strip_height, 44);
        CHECK_EQ(theme.font_size, 16.5f);
        CHECK_EQ(theme.tab_hover_ms, 0);
        CHECK_EQ(theme.new_tab_backgrounds, "pictures"); // resolved only by load, against the file
        CHECK_EQ(theme.new_tab_rotate_ms, 5000);
        CHECK(theme.chrome_background == Theme {}.chrome_background); // untouched
        CHECK(!(theme == Theme {}));
    }

    // --- Every mistake is named and leaves the default --------------------------
    {
        std::vector<std::string> problems;
        Theme const theme = Theme::from_json(
            "{ \"colors\": { \"accent\": \"red\", \"glow\": \"#000\", \"chrome-text\": 7 },"
            "  \"metrics\": { \"padding\": -1, \"tab-gap\": 2.5, \"toolbar-height\": \"40\" },"
            "  \"type\": { \"font-size\": 1 }, \"timings\": { \"tab-switch\": 99999 },"
            "  \"new-tab\": { \"backgrounds\": 4, \"rotate\": -1, \"glow\": 1 },"
            "  \"sounds\": {}, \"name\": 3 }",
            &problems);
        CHECK(theme == Theme {});
        CHECK_EQ(problems.size(), std::size_t { 13 });
        auto const mentions = [&](std::string const& text) {
            for (std::string const& problem : problems) {
                if (problem.find(text) != std::string::npos)
                    return true;
            }
            return false;
        };
        CHECK(mentions("colors.accent: not a color: red"));
        CHECK(mentions("colors.glow: unknown token"));
        CHECK(mentions("colors.chrome-text: expected a string"));
        CHECK(mentions("metrics.padding"));
        CHECK(mentions("metrics.tab-gap"));
        CHECK(mentions("metrics.toolbar-height"));
        CHECK(mentions("type.font-size"));
        CHECK(mentions("timings.tab-switch"));
        CHECK(mentions("new-tab.backgrounds: expected a folder path"));
        CHECK(mentions("new-tab.rotate"));
        CHECK(mentions("new-tab.glow: unknown token"));
        CHECK(mentions("sounds: unknown section"));
        CHECK(mentions("name: expected a string"));
    }
    // --- A surface's own tokens follow the theme's, unless the file names them ---
    {
        // A theme written before menus existed dresses them in its own
        // colors, never in the built-in dark ones.
        std::vector<std::string> problems;
        Theme const light = Theme::from_json(
            "{ \"colors\": { \"chrome-background\": \"#f1f2f4\", \"chrome-text\": \"#101216\","
            "  \"chrome-text-muted\": \"#5c6370\", \"address-border\": \"#c5c9d2\","
            "  \"button-hover-background\": \"#dfe2e8\", \"button-disabled-text\": \"#a0a6b2\" } }",
            &problems);
        CHECK_EQ(problems.size(), std::size_t { 0 });
        CHECK(light.popup_background == Color::rgb(0xf1, 0xf2, 0xf4));
        CHECK(light.popup_text == Color::rgb(0x10, 0x12, 0x16));
        CHECK(light.popup_text_muted == Color::rgb(0x5c, 0x63, 0x70));
        CHECK(light.popup_border == Color::rgb(0xc5, 0xc9, 0xd2));
        CHECK(light.popup_highlight == Color::rgb(0xdf, 0xe2, 0xe8));
        CHECK(light.popup_highlight_text == Color::rgb(0x10, 0x12, 0x16));
        CHECK(light.popup_disabled_text == Color::rgb(0xa0, 0xa6, 0xb2));
        CHECK(!(light.popup_background == Theme {}.popup_background)); // the control: it moved

        // The toolbar, its icons, the front tab's title and a focused field
        // follow the tokens they were split from; the tab's line is drawn
        // by no theme that does not name it.
        Theme const split = Theme::from_json(
            "{ \"colors\": { \"tab-active-background\": \"#ffffff\", \"chrome-text\": \"#101216\","
            "  \"address-background\": \"#eeeeee\", \"address-text\": \"#222222\", \"accent\": \"#0a84ff\","
            "  \"selection\": \"#0a84ff55\" } }",
            &problems);
        CHECK_EQ(problems.size(), std::size_t { 0 });
        CHECK(split.toolbar_background == Color::rgb(0xff, 0xff, 0xff));
        CHECK(split.toolbar_icon == Color::rgb(0x10, 0x12, 0x16));
        CHECK(split.tab_text == Color::rgb(0x10, 0x12, 0x16));
        CHECK(split.address_background_focus == Color::rgb(0xee, 0xee, 0xee));
        CHECK(split.address_text_focus == Color::rgb(0x22, 0x22, 0x22));
        CHECK(split.address_border_focus == Color::rgb(0x0a, 0x84, 0xff));
        CHECK(split.address_selection == Color::rgba(0x0a, 0x84, 0xff, 0x55));
        CHECK_EQ(static_cast<int>(split.tab_line.a), 0);
        Theme const apart = Theme::from_json(
            "{ \"colors\": { \"tab-active-background\": \"#ffffff\", \"toolbar-background\": \"#f0f0f4cc\","
            "  \"toolbar-icon\": \"#5b5b66\", \"tab-line\": \"#0a84ff\" } }",
            &problems);
        CHECK_EQ(problems.size(), std::size_t { 0 });
        CHECK(apart.toolbar_background == Color::rgba(0xf0, 0xf0, 0xf4, 0xcc));
        CHECK(apart.tab_active_background == Color::rgb(0xff, 0xff, 0xff));
        CHECK(apart.toolbar_icon == Color::rgb(0x5b, 0x5b, 0x66));
        CHECK(apart.tab_line == Color::rgb(0x0a, 0x84, 0xff));

        // Named, a token is what the file says, whatever it would derive from.
        Theme const named = Theme::from_json(
            "{ \"colors\": { \"chrome-background\": \"#f1f2f4\", \"popup-background\": \"#202020\","
            "  \"popup-highlight-text\": \"#ffffff\" } }",
            &problems);
        CHECK_EQ(problems.size(), std::size_t { 0 });
        CHECK(named.popup_background == Color::rgb(0x20, 0x20, 0x20));
        CHECK(named.popup_highlight_text == Color::rgb(0xff, 0xff, 0xff));
        CHECK(named.popup_text == Theme {}.chrome_text); // not named: derived, from a token left at its default

        // Named with something that is no color, it is reported and derived.
        Theme const broken = Theme::from_json(
            "{ \"colors\": { \"chrome-background\": \"#f1f2f4\", \"popup-background\": \"paper\" } }", &problems);
        CHECK_EQ(problems.size(), std::size_t { 1 });
        CHECK(broken.popup_background == Color::rgb(0xf1, 0xf2, 0xf4));
        problems.clear();

        // The frame of a window that is not in front is the theme's frame
        // dimmed, until the theme says otherwise — by the rule Chrome dims
        // its own by: a light frame 28.4% of the way to white, which takes
        // (241, 242, 244) to (245, 246, 247); a dark one a little more
        // saturated and 13.4% of the way, which takes the built-in frame
        // (31, 34, 40) to (59, 63, 71).
        CHECK(light.chrome_background_inactive == Color::rgb(245, 246, 247));
        CHECK(!(light.chrome_background_inactive == Theme {}.chrome_background_inactive)); // the control: it moved
        CHECK(Theme {}.chrome_background_inactive == Color::rgb(59, 63, 71));
        CHECK(Theme {}.chrome_background_inactive == inactive_frame_of(Theme {}.chrome_background)); // the default is the rule's
        CHECK(inactive_frame_of(Color::rgb(0, 0, 0)) == Color::rgb(34, 34, 34));
        CHECK(inactive_frame_of(Color::rgb(255, 255, 255)) == Color::rgb(255, 255, 255));
        Theme const dimmed = Theme::from_json(
            "{ \"colors\": { \"chrome-background\": \"#f1f2f4\", \"chrome-background-inactive\": \"#d0d2d6\" } }",
            &problems);
        CHECK_EQ(problems.size(), std::size_t { 0 });
        CHECK(dimmed.chrome_background == Color::rgb(0xf1, 0xf2, 0xf4));
        CHECK(dimmed.chrome_background_inactive == Color::rgb(0xd0, 0xd2, 0xd6));
    }

    // --- Pictures over the chrome's surfaces ----------------------------------
    {
        using Hold = ThemePicture::Hold;
        std::vector<std::string> problems;
        // A path alone is a picture held at the top right and shown once, as
        // a browser theme's is when it says nothing more.
        Theme const plain = Theme::from_json("{ \"images\": { \"frame\": \"header.png\" } }", &problems);
        CHECK_EQ(problems.size(), std::size_t { 0 });
        CHECK_EQ(plain.frame_pictures.size(), std::size_t { 1 });
        if (plain.frame_pictures.size() == 1) {
            CHECK_EQ(plain.frame_pictures[0].path, "header.png");
            CHECK(plain.frame_pictures[0].across == Hold::End);
            CHECK(plain.frame_pictures[0].down == Hold::Start);
            CHECK(!plain.frame_pictures[0].repeat_across);
            CHECK(!plain.frame_pictures[0].repeat_down);
        }
        CHECK(plain.toolbar_pictures.empty());
        CHECK(plain.tab_background_pictures.empty());
        CHECK(!(plain == Theme {}));

        // A list is front to back; each entry says where it is held and how
        // it repeats, a side named alone leaving the other axis centered.
        Theme const layered = Theme::from_json(
            "{ \"images\": {"
            "  \"frame\": [ \"front.png\","
            "               { \"picture\": \"middle.png\", \"align\": \"left bottom\", \"tile\": \"repeat-x\" },"
            "               { \"picture\": \"back.png\", \"align\": \"center\", \"tile\": \"repeat\" } ],"
            "  \"toolbar\": { \"picture\": \"bar.png\", \"align\": \"top\", \"tile\": \"repeat-y\" },"
            "  \"tab-background\": { \"picture\": \"tab.png\", \"align\": \"bottom right\", \"tile\": \"no-repeat\" } } }",
            &problems);
        CHECK_EQ(problems.size(), std::size_t { 0 });
        CHECK_EQ(layered.frame_pictures.size(), std::size_t { 3 });
        if (layered.frame_pictures.size() == 3) {
            CHECK_EQ(layered.frame_pictures[0].path, "front.png");
            CHECK_EQ(layered.frame_pictures[1].path, "middle.png");
            CHECK(layered.frame_pictures[1].across == Hold::Start);
            CHECK(layered.frame_pictures[1].down == Hold::End);
            CHECK(layered.frame_pictures[1].repeat_across);
            CHECK(!layered.frame_pictures[1].repeat_down);
            CHECK(layered.frame_pictures[2].across == Hold::Center);
            CHECK(layered.frame_pictures[2].down == Hold::Center);
            CHECK(layered.frame_pictures[2].repeat_across);
            CHECK(layered.frame_pictures[2].repeat_down);
        }
        CHECK_EQ(layered.toolbar_pictures.size(), std::size_t { 1 });
        if (layered.toolbar_pictures.size() == 1) {
            CHECK(layered.toolbar_pictures[0].across == Hold::Center);
            CHECK(layered.toolbar_pictures[0].down == Hold::Start);
            CHECK(!layered.toolbar_pictures[0].repeat_across);
            CHECK(layered.toolbar_pictures[0].repeat_down);
        }
        CHECK_EQ(layered.tab_background_pictures.size(), std::size_t { 1 });
        if (layered.tab_background_pictures.size() == 1) {
            CHECK(layered.tab_background_pictures[0].across == Hold::End);
            CHECK(layered.tab_background_pictures[0].down == Hold::End);
        }
        // Scaling a theme leaves its pictures as they are.
        CHECK(layered.scaled(2).frame_pictures == layered.frame_pictures);

        // What is wrong is said by name; a picture with a bad side or a bad
        // repeat keeps the rest of what it says, and one that names no file
        // is no picture.
        Theme const wrong = Theme::from_json(
            "{ \"images\": {"
            "  \"frame\": [ { \"picture\": \"a.png\", \"align\": \"left right\" },"
            "               { \"picture\": \"b.png\", \"align\": \"upper\", \"tile\": \"sometimes\", \"glow\": 1 },"
            "               { \"align\": \"left\" }, 7, { \"picture\": 3 } ],"
            "  \"sidebar\": \"s.png\" } }",
            &problems);
        auto const mentions = [&](std::string const& what) {
            for (std::string const& problem : problems) {
                if (problem.find(what) != std::string::npos)
                    return true;
            }
            return false;
        };
        CHECK_EQ(wrong.frame_pictures.size(), std::size_t { 2 });
        if (wrong.frame_pictures.size() == 2) {
            CHECK_EQ(wrong.frame_pictures[1].path, "b.png");
            CHECK(wrong.frame_pictures[1].across == Hold::End); // as unsaid
            CHECK(wrong.frame_pictures[1].down == Hold::Start);
        }
        CHECK_EQ(problems.size(), std::size_t { 9 });
        CHECK(mentions("images.frame[0].align"));
        CHECK(mentions("images.frame[1].align"));
        CHECK(mentions("images.frame[1].tile"));
        CHECK(mentions("images.frame[1].glow: unknown token"));
        CHECK(mentions("images.frame[2]: names no picture"));
        CHECK(mentions("images.frame[3]: expected a file path"));
        CHECK(mentions("images.frame[4].picture: expected a file path"));
        CHECK(mentions("images.frame[4]: names no picture"));
        CHECK(mentions("images.sidebar: unknown token"));
        problems.clear();
        CHECK(Theme::from_json("{ \"images\": [] }", &problems) == Theme {});
        CHECK_EQ(problems.size(), std::size_t { 1 });
        problems.clear();
        // No surface takes pictures without end.
        std::string many = "{ \"images\": { \"frame\": [";
        for (int i = 0; i < 20; ++i)
            many += std::string(i ? "," : "") + "\"p.png\"";
        many += "] } }";
        Theme const crowded = Theme::from_json(many, &problems);
        CHECK_EQ(crowded.frame_pictures.size(), std::size_t { 16 });
        CHECK_EQ(problems.size(), std::size_t { 1 });
    }

    // --- How the tabs sit in their strip ----------------------------------------
    {
        std::vector<std::string> problems;
        CHECK(Theme {}.tab_shape == TabShape::Attached); // as unsaid
        CHECK(Theme::from_json("{ \"tab-shape\": \"floating\" }", &problems).tab_shape == TabShape::Floating);
        CHECK(Theme::from_json("{ \"tab-shape\": \"attached\" }", &problems).tab_shape == TabShape::Attached);
        CHECK_EQ(problems.size(), std::size_t { 0 });
        CHECK(!(Theme::from_json("{ \"tab-shape\": \"floating\" }") == Theme {}));
        CHECK(Theme::from_json("{ \"tab-shape\": \"round\" }", &problems) == Theme {});
        CHECK(Theme::from_json("{ \"tab-shape\": 3 }", &problems) == Theme {});
        CHECK_EQ(problems.size(), std::size_t { 2 });
        CHECK(Theme::from_json("{ \"tab-shape\": \"floating\" }").scaled(2).tab_shape == TabShape::Floating);
    }

    // --- The face the chrome's words are set in -------------------------------
    {
        std::vector<std::string> problems;
        CHECK(Theme {}.font_family.empty()); // as unsaid: the machine's own interface face
        Theme const named = Theme::from_json(
            "{ \"type\": { \"font-size\": 15, \"font-family\": \"Inter, 'Noto Sans', sans-serif\" } }", &problems);
        CHECK_EQ(named.font_family, std::string("Inter, 'Noto Sans', sans-serif"));
        CHECK_EQ(named.font_size, 15.0f); // a family beside the sizes leaves them as they were read
        CHECK_EQ(problems.size(), std::size_t { 0 }); // and is no unknown token
        CHECK(!(named == Theme {}));
        CHECK_EQ(named.scaled(2).font_family, named.font_family);
        // A family that is no string is said, and the chrome keeps the face it had.
        Theme const numbered = Theme::from_json("{ \"type\": { \"font-family\": 12 } }", &problems);
        CHECK(numbered.font_family.empty());
        CHECK_EQ(problems.size(), std::size_t { 1 });
        CHECK(!problems.empty() && problems.back().find("type.font-family") != std::string::npos);
        // What the section does not know is still said.
        problems.clear();
        (void)Theme::from_json("{ \"type\": { \"font-famly\": \"Inter\" } }", &problems);
        CHECK_EQ(problems.size(), std::size_t { 1 });
    }

    // --- The new-tab page's own colors ---------------------------------------
    {
        std::vector<std::string> problems;
        // Unnamed, there are none: the page wears the chrome's.
        CHECK(!Theme {}.new_tab_background.has_value());
        CHECK(!Theme {}.new_tab_background_end.has_value());
        CHECK(!Theme {}.new_tab_text.has_value());
        CHECK(!Theme {}.new_tab_text_muted.has_value());
        Theme const named = Theme::from_json(
            "{ \"new-tab\": { \"background\": \"#102030\", \"background-end\": \"#405060\","
            "  \"text\": \"#f0f0f0\", \"text-muted\": \"#a0a0a080\", \"rotate\": 1000 } }",
            &problems);
        CHECK_EQ(problems.size(), std::size_t { 0 });
        CHECK(named.new_tab_background == Color::rgb(0x10, 0x20, 0x30));
        CHECK(named.new_tab_background_end == Color::rgb(0x40, 0x50, 0x60));
        CHECK(named.new_tab_text == Color::rgb(0xf0, 0xf0, 0xf0));
        CHECK(named.new_tab_text_muted == Color::rgba(0xa0, 0xa0, 0xa0, 0x80));
        CHECK_EQ(named.new_tab_rotate_ms, 1000);
        CHECK(named.scaled(2).new_tab_background == named.new_tab_background);
        // What is no color is said by name and left unnamed.
        Theme const wrong = Theme::from_json(
            "{ \"new-tab\": { \"background\": \"sky\", \"text\": 7 } }", &problems);
        CHECK_EQ(problems.size(), std::size_t { 2 });
        CHECK(!wrong.new_tab_background.has_value());
        CHECK(!wrong.new_tab_text.has_value());
        CHECK(wrong == Theme {});
    }

    // --- A theme's pictures are named relative to the theme file ---------------
    {
        std::filesystem::path const dir = std::filesystem::temp_directory_path() / "sashfold-test-theme-images";
        std::filesystem::create_directories(dir);
        std::filesystem::path const file = dir / "images.json";
        std::filesystem::path const elsewhere = std::filesystem::temp_directory_path() / "sashfold-elsewhere.png";
        {
            std::ofstream out(file);
            out << "{ \"images\": { \"frame\": [ \"art/header.png\", " << "\"" << elsewhere.generic_string() << "\" ],"
                << " \"toolbar\": \"bar.png\" } }";
        }
        std::vector<std::string> problems;
        std::optional<Theme> const theme = Theme::load(file.string(), &problems);
        CHECK(theme.has_value());
        CHECK_EQ(problems.size(), std::size_t { 0 });
        if (theme && theme->frame_pictures.size() == 2 && theme->toolbar_pictures.size() == 1) {
            CHECK_EQ(theme->frame_pictures[0].path, (dir / "art" / "header.png").lexically_normal().string());
            CHECK_EQ(theme->frame_pictures[1].path, elsewhere.generic_string()); // absolute: as written
            CHECK_EQ(theme->toolbar_pictures[0].path, (dir / "bar.png").lexically_normal().string());
        } else {
            CHECK(false);
        }
        std::filesystem::remove(file);
        std::filesystem::remove(dir);
    }

    // --- The pictures folder is named relative to the theme file ---------------
    {
        std::filesystem::path const dir = std::filesystem::temp_directory_path() / "sashfold-test-theme";
        std::filesystem::create_directories(dir);
        std::filesystem::path const file = dir / "pictures.json";
        {
            std::ofstream out(file);
            out << "{ \"new-tab\": { \"backgrounds\": \"pics\" } }";
        }
        std::vector<std::string> problems;
        std::optional<Theme> const theme = Theme::load(file.string(), &problems);
        CHECK(theme.has_value());
        CHECK_EQ(problems.size(), std::size_t { 0 });
        if (theme)
            CHECK_EQ(theme->new_tab_backgrounds, (dir / "pics").lexically_normal().string());
        std::filesystem::remove(file);
        std::filesystem::remove(dir);
    }
    {
        std::vector<std::string> problems;
        CHECK(Theme::from_json("not json", &problems) == Theme {});
        CHECK_EQ(problems.size(), std::size_t { 1 });
        problems.clear();
        CHECK(Theme::from_json("[1, 2]", &problems) == Theme {});
        CHECK_EQ(problems.size(), std::size_t { 1 });
        problems.clear();
        CHECK(Theme::from_json("{ \"colors\": [] }", &problems) == Theme {});
        CHECK_EQ(problems.size(), std::size_t { 1 });
        CHECK(!Theme::load("themes/does-not-exist.json", &problems).has_value());
    }

    // --- The theme scaled to a display ---------------------------------------
    {
        Theme const one = Theme {}.scaled(1);
        CHECK(one == Theme {});
        Theme const two = Theme {}.scaled(2);
        CHECK_EQ(two.tab_strip_height, 72);
        CHECK_EQ(two.toolbar_height, 80);
        CHECK_EQ(two.border_width, 2);
        CHECK_EQ(two.tab_icon_size, 32);
        CHECK_EQ(two.scroll_step, 120);
        CHECK_EQ(two.font_size, 28.0f);
        CHECK_EQ(two.status_font_size, 24.0f);
        CHECK(two.chrome_background == Theme {}.chrome_background);
        CHECK_EQ(two.tab_hover_ms, Theme {}.tab_hover_ms);
        CHECK_EQ(two.new_tab_rotate_ms, Theme {}.new_tab_rotate_ms);
        // A fraction rounds, and a border never vanishes.
        Theme const half = Theme {}.scaled(0.5f);
        CHECK_EQ(half.tab_strip_height, 18);
        CHECK_EQ(half.border_width, 1);
        Theme const one_and_a_half = Theme {}.scaled(1.5f);
        CHECK_EQ(one_and_a_half.address_height, 42);
        CHECK_EQ(one_and_a_half.tab_font_size, 19.5f);
        // Nonsense leaves the theme as it is.
        CHECK(Theme {}.scaled(0) == Theme {});
        CHECK(Theme {}.scaled(-2) == Theme {});
    }

    // --- The shipped presets parse clean, each under its own name -------------
    for (int i = 2; i < argc; ++i) {
        std::vector<std::string> problems;
        std::optional<Theme> const preset = Theme::load(argv[i], &problems);
        CHECK(preset.has_value());
        CHECK_EQ(problems.size(), std::size_t { 0 });
        for (std::string const& problem : problems)
            std::cerr << "  " << argv[i] << ": " << problem << "\n";
        if (preset) {
            CHECK(!preset->name.empty());
            CHECK(preset->name != Theme {}.name);
            CHECK(!(*preset == Theme {}));
        }
    }

    return sashfold::test::report("theme");
}
