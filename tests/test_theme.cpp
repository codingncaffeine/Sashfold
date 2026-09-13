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
