#pragma once

// The text shaper: a run of characters in one script, one direction and
// one face becomes glyphs with positions — the letters of a script that
// joins, stacks or reorders as they are written. The pipeline is the one
// the OpenType specification's script development documents describe
// (normalization as shapers do it, cmap with variation sequences,
// mirroring, the script's features in their stages through GSUB, then
// GPOS, mark and cursive attachment), with a shaper per script family:
// the default (Latin, Greek, Cyrillic and every script without rules of
// its own), Arabic and the scripts that join like it, Hebrew, Thai and
// Lao, and Hangul. The tables are read by OpenTypeLayout.h.
//
// Layout calls `shape` once per run of one font, script and direction —
// CharacterProperties.h itemizes text into script runs — and measures and
// paints the run from the result: advances and offsets in font units,
// glyphs in visual order, each with the index of the character its
// cluster starts at.

#include "text/CharacterProperties.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sashfold::text {

class Face;

// An OpenType feature asked for by tag ('liga', 'smcp', ...) with its value
// (0 turns it off, 1 on, n the n-th alternate), over the characters
// [start, end) of the run, or all of it when the range is the default.
struct FeatureSetting {
    std::uint32_t tag = 0;
    std::uint32_t value = 1;
    std::size_t start = 0;
    std::size_t end = static_cast<std::size_t>(-1);
    bool global() const { return start == 0 && end == static_cast<std::size_t>(-1); }
};

constexpr std::uint32_t feature_tag(char a, char b, char c, char d)
{
    return script_code(a, b, c, d);
}

enum class TextDirection : std::uint8_t {
    LeftToRight,
    RightToLeft,
};

struct ShapeRequest {
    std::u32string_view text;
    // The characters around the run in its paragraph, when the run is a
    // piece of a word (a font change, a span boundary): they join with the
    // run's letters (Arabic joining reads them) but are not shaped.
    std::u32string_view before;
    std::u32string_view after;
    ScriptCode script = scripts::common;
    // BCP 47 ("fa", "sr-Latn") or an OpenType language system tag spelled
    // "x-ot-URD"; empty for the font's default.
    std::string_view language;
    TextDirection direction = TextDirection::LeftToRight;
    // CSS font-feature-settings, font-variant-* and font-kerning map to
    // these (see shaping_features_for_css below); later settings of a tag
    // override earlier ones.
    std::span<FeatureSetting const> features;
    // Whether the run starts the text (a mark with nothing before it is
    // then drawn on a dotted circle, as Unicode §2.11 shows a lone mark).
    bool beginning_of_text = false;
    bool end_of_text = false;
};

struct ShapedGlyph {
    std::uint32_t glyph = 0; // the face's glyph id; 0 is .notdef
    std::uint32_t cluster = 0; // index into the request's text of the first character of its cluster
    std::int32_t x_advance = 0; // font units
    std::int32_t y_advance = 0;
    std::int32_t x_offset = 0; // from the pen, font units, y up
    std::int32_t y_offset = 0;
};

struct ShapedRun {
    std::vector<ShapedGlyph> glyphs; // visual order: left to right on the line
    std::uint16_t units_per_em = 1000; // what the font units are per em: scale by size / units_per_em
    // The run's advance, font units: the sum of its glyphs' x advances.
    std::int64_t advance() const
    {
        std::int64_t sum = 0;
        for (ShapedGlyph const& g : glyphs)
            sum += g.x_advance;
        return sum;
    }
};

// Shapes one run with one face. Thread-safe: the face's layout tables are
// parsed once and shared, and the plans made for it are cached per face.
// A face that is not an OpenType font (the built-in face) gets one glyph
// per character at its own advance.
ShapedRun shape(Face const& face, ShapeRequest const& request);
// The same into a vector the caller keeps, to spare the allocation.
void shape(Face const& face, ShapeRequest const& request, ShapedRun& out);

// The feature settings the CSS Fonts properties of a run come to
// (css-fonts-4 §7): font-kerning, the ligature switches of
// font-variant-ligatures, and the rule that a non-zero letter-spacing
// turns the optional ligatures off (css-text-3 §8.2). Appended to `out`
// ahead of the run's own font-feature-settings, which come last so they
// win.
struct CssFontFeatures {
    enum class Kerning : std::uint8_t {
        Auto,
        Normal,
        None,
    } kerning = Kerning::Auto;
    bool common_ligatures = true; // liga, clig
    bool discretionary_ligatures = false; // dlig
    bool historical_ligatures = false; // hlig
    bool contextual_alternates = true; // calt
    bool letter_spacing = false; // a non-zero letter-spacing
};
void shaping_features_for_css(CssFontFeatures const&, std::vector<FeatureSetting>& out);

// Whether the face's GSUB has a feature with this tag in the language
// system the shaper selects for the script and language: what CSS asks
// before it synthesizes forms a font lacks (css-fonts-4 §6.6 decides small
// capitals per font and script, never per character).
bool face_has_substitution_feature(Face const&, ScriptCode, std::string_view language, std::uint32_t tag);

}
