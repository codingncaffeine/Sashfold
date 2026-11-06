#pragma once
// What the keys and the pointer do to a text control's value and selection
// (input and textarea): where a caret goes by a character, a word, a line;
// what a double click selects; what an edit leaves. Pure functions of the
// text, in code points, so the window and the tests share one model.
#include "dom/Forms.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <optional>
#include <utility>

namespace sashfold::ui::field_edit {

// One character back or on: a code point, with the combining marks after
// it kept with it, as a caret never stands between a letter and its accent.
std::size_t previous_character(std::u32string_view text, std::size_t at);
std::size_t next_character(std::u32string_view text, std::size_t at);

// Word moves as Ctrl+Left and Ctrl+Right make them on Linux and Windows:
// back to the start of the word before the caret (or the one it is in), and
// on to the end of the word after it. Words are found by the Unicode word
// boundaries (UAX #29); a boundary between two runs of spaces or of
// punctuation is no stopping place.
std::size_t previous_word_start(std::u32string_view text, std::size_t at);
std::size_t next_word_end(std::u32string_view text, std::size_t at);

// What a double click at `at` selects: the word there, or the run of spaces
// or the punctuation mark there, [first, second).
std::pair<std::size_t, std::size_t> word_at(std::u32string_view text, std::size_t at);

// The hard line around `at` (between line feeds): what a triple click in a
// textarea selects, and Home/End's reach where there is no wrapping.
std::pair<std::size_t, std::size_t> line_at(std::u32string_view text, std::size_t at);

// A selection moved by a key: the focus put at `to`, the anchor kept when
// extending (Shift), else collapsed there.
dom::forms::TextSelection moved(dom::forms::TextSelection const& from, std::size_t to, bool extend);

// The text with [start, end) replaced by `with`, and the caret after it.
struct Edit {
    std::u32string text;
    std::size_t caret = 0;
};
Edit replace(std::u32string_view text, std::size_t start, std::size_t end, std::u32string_view with);

// How much of `inserted` fits under a maxlength of `limit` UTF-16 code
// units once [start, end) of `text` is gone (HTML §4.10.18.4: the user
// cannot type past it); all of it when there is no limit.
std::u32string_view within_max_length(std::u32string_view text, std::size_t start, std::size_t end,
    std::u32string_view inserted, std::optional<std::size_t> limit);

}
