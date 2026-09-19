#pragma once

// Firefox and Chrome themes as Sashfold themes. A browser theme is a
// manifest.json — colors, pictures, and for Chrome tints — in a folder, an
// .xpi or a .crx (both zips; the .crx behind a signed header). This reads
// one and writes what it says as one of our theme files with its pictures
// beside it. It is a converter, not a runtime: nothing of theirs runs, a
// Chrome theme's tints are worked into the colors and pictures written, and
// what a theme says that has nowhere to land here yet is listed, by key,
// rather than dropped in silence.
//
// What a theme leaves unsaid is filled in as its own browser would fill it:
// from a light or a dark palette chosen by the theme's text, the fallbacks
// between its keys (a tab's text from the toolbar's, the icons' from the
// text's), transparent background tabs, a toolbar that veils the header's
// picture. Every color token is written out, so that a converted theme
// looks the same whatever our own defaults become.

#include "core/Bitmap.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::ui {

enum class BrowserThemeKind {
    Firefox,
    Chrome,
};

struct ImportedTheme {
    std::string name; // as the manifest gives it
    BrowserThemeKind kind = BrowserThemeKind::Firefox;
    std::string json; // the Sashfold theme file
    struct Picture {
        std::string file; // the name the theme file uses, relative to it
        std::vector<std::uint8_t> bytes;
    };
    std::vector<Picture> pictures;
    std::vector<std::string> notes; // what was said and could not be carried over
};

// A file of the theme, by the path its manifest writes (relative to the
// manifest); nullopt when there is none.
using ThemeFileReader = std::function<std::optional<std::vector<std::uint8_t>>(std::string const& path)>;

// The theme a manifest describes. `container` is what the file it came in
// says of its origin — an .xpi is Firefox's, a .crx Chrome's — and absent,
// the manifest is judged by what only one of the two writes. Nullopt, with
// the reason in `problems`, for what is no theme's manifest.
std::optional<ImportedTheme> import_browser_theme(std::string_view manifest, ThemeFileReader const& read,
    std::optional<BrowserThemeKind> container, std::vector<std::string>* problems = nullptr);

// The same from disk: a folder holding a manifest.json, that manifest.json
// itself, or an .xpi, a .zip or a .crx.
std::optional<ImportedTheme> import_browser_theme_from(std::string const& path,
    std::vector<std::string>* problems = nullptr);

// The same from an archive held in memory — an .xpi, a .zip or a .crx as it
// arrived, never written down. `named` is what to call it in a problem.
std::optional<ImportedTheme> import_browser_theme_archive(std::vector<std::uint8_t> bytes,
    std::optional<BrowserThemeKind> container, std::string const& named,
    std::vector<std::string>* problems = nullptr);

// Writes the theme as `<directory>/<a name made of its name>/theme.json`
// with its pictures beside it, replacing an earlier conversion of the same
// name, and returns the theme file's path.
std::optional<std::string> write_imported_theme(ImportedTheme const& theme, std::string const& directory,
    std::vector<std::string>* problems = nullptr);

// Whether the path looks like something import_browser_theme_from reads.
bool is_browser_theme_path(std::string const& path);

}
