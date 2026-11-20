#pragma once

// Text as a page sets it: a string in the faces a style resolved to
// becomes glyphs, each with the face that draws it, the character its
// cluster starts at, and where it goes — what layout measures, paint draws,
// and carets, selections and hit tests are placed by, so that the three
// can never disagree.
//
// FontStack::shape (FontManager.h) does the work: the text is split where
// its bidi level changes (UAX #9, when it is a paragraph rather than a run
// of one level), where its script changes (UAX #24, itemize_scripts), and
// where its face changes — a whole cluster at a time, a base and its marks
// in one face — and each piece is shaped (Shaper.h) with the characters
// around it as context, so a letter of a joining script takes the form its
// neighbors give it even across a face, element or line boundary. A
// cluster the face has no glyph for (glyph 0 in the shaper's answer) is
// shaped again with the next face that has it: the rest of the stack, the
// manager's fallback chain, and last the built-in face, which draws the
// box. letter-spacing goes after every cluster and word-spacing after every
// word separator (css-text-3 §8.1, §8.2).

#include "text/Shaper.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::text {

class Face;

// What besides the text and its fonts decides the glyphs and their places.
struct TextShaping {
    float size = 16; // px
    float letter_spacing = 0; // px after every cluster
    float word_spacing = 0; // px after every word separator
    // The direction of the text: the one it is drawn in when it is a run of
    // one bidi level (a piece of a line, which layout cuts where the level
    // changes), or the base direction of a paragraph whose levels are to be
    // resolved, when `paragraph` is set (a canvas's text, an SVG text
    // chunk, the window's own words).
    TextDirection direction = TextDirection::LeftToRight;
    bool paragraph = false;
    // BCP 47, the element's language: what a font's language system
    // (`locl` forms) is chosen by. Empty for the font's default.
    std::string language;
    // In the order they apply, later settings of a tag over earlier ones:
    // shaping_features_for_css's, then font-variant's, then
    // font-feature-settings.
    std::vector<FeatureSetting> features;
};

// One glyph as it is set: its face, its id in that face, the index into
// the text of the first character of its cluster, and its place — the pen
// position from the text's left edge, the advance to the next glyph's pen
// (letter-spacing included), the word-spacing the pen takes after that
// when the glyph ends a word separator, and the offset from the pen it is
// drawn at (y down the page). In px.
struct PlacedGlyph {
    Face const* face = nullptr;
    std::uint32_t glyph = 0;
    std::uint32_t cluster = 0;
    float x = 0;
    float advance = 0;
    float word_space = 0;
    float x_offset = 0;
    float y_offset = 0;
};

struct ShapedText {
    // Left to right on the line.
    std::vector<PlacedGlyph> glyphs;
    // What the text takes on its line. In the built-in face alone, a count
    // times one advance (and the spacing), which is exact where the pen's
    // running sum is not; else the pen's travel.
    float width = 0;
    // The pen's travel, stepped glyph by glyph as a painter steps it.
    float pen_end = 0;
    // How many characters the text has, and where its clusters start, in
    // ascending order (0 first, when there are any): a line never ends and
    // a caret never stops inside a cluster.
    std::uint32_t length = 0;
    std::vector<std::uint32_t> cluster_starts;

    bool starts_cluster(std::size_t index) const;
    // The start of the cluster the character at `index` is in, and the
    // index one past its end.
    std::size_t cluster_start_of(std::size_t index) const;
    std::size_t cluster_end_of(std::size_t index) const;
    // Where each character stands on the line, from the text's left edge:
    // `left[i]` and `right[i]` for character i of the text (logical order).
    // A cluster's room is shared out evenly among its characters, in the
    // direction it reads: the "f" and the "i" of an fi ligature each get
    // half of it, as browsers place a caret inside one.
    void character_edges(std::vector<float>& left, std::vector<float>& right) const;
    // Whether any of the text reads right to left: the glyphs of a cluster
    // then run against its characters.
    std::vector<bool> right_to_left; // per character; empty when none does
};

// The word separators of css-text-3 §8.1: what word-spacing goes after,
// and what justification stretches.
bool is_word_separator(char32_t);

// The part of a text shaped whole that its characters [from, to) take: the
// glyphs whose clusters start among them, at their places from the part's
// own left edge. A word split by inline boxes that change nothing of its
// formatting is shaped whole (css-text-3 §7.3) and drawn a part per box:
// a ligature over a box's edge goes with the box its cluster starts in.
std::shared_ptr<ShapedText const> part_of(ShapedText const& whole, std::size_t from, std::size_t to);

}
