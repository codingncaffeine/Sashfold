#pragma once

// A laid-out run of text as glyphs: what painting draws, and what carets,
// selections, range rectangles and hit tests measure a run's characters
// by, so that every one of them agrees with what layout measured. Layout
// puts its shaping on the run (TextRun::shaped); a run made without it is
// shaped here from its text, the same way.

#include "layout/Layout.h"
#include "text/ShapedText.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::css {
struct ComputedStyle;
}

namespace sashfold::layout {

// What a style asks of its text's shaping: the size, letter- and
// word-spacing, and the features font-kerning and a non-zero
// letter-spacing come to (css-fonts-4 §7, css-text-3 §8.2). The text
// reads right to left when `right_to_left`; `paragraph` has its bidi
// levels resolved from it instead (with that as the base direction).
text::TextShaping shaping_for(css::ComputedStyle const&, bool right_to_left, bool paragraph = false);

// The run's characters in reading order: its text, or, for a run drawn
// right to left, its text back to front with the mirrored brackets put
// back (layout stores such a run as drawn).
std::u32string logical_text_of(TextRun const&);

// The run's glyphs: the shaping layout gave it, or, for a run without
// one, its text shaped now in its fonts — a run of one right-to-left level
// when `source_reversed`, a paragraph of its own otherwise.
std::shared_ptr<text::ShapedText const> shaped_of(TextRun const&);

// The run's characters from `from` to `to` in `text` order (the part of it
// text-overflow keeps), shaped with the rest of the run around them as
// context.
std::shared_ptr<text::ShapedText const> shaped_slice(TextRun const&, std::size_t from, std::size_t to);

// Where each character of `run.text` begins, from the run's left edge, and
// where the run's pen ends: `text.size() + 1` stops in `text` order. A
// cluster's room is shared out evenly among its characters, so a caret
// inside a ligature stands part of the way through it.
std::vector<float> run_character_stops(TextRun const&);

// Whether `run.text` may be cut in front of its character `at`: only at a
// cluster's edge.
bool run_cut_allowed(TextRun const&, std::size_t at);

}
