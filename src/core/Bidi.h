#pragma once

// The Unicode bidirectional algorithm (UAX #9) over the generated
// Bidi_Class tables. It answers two questions and no others: what
// embedding level does each character of a paragraph end up at, and in what
// order are those characters drawn? Everything above it — where the
// paragraphs are, which text belongs to which box, how a line is broken —
// belongs to layout.

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace sashfold {

// Which direction a code point carries on its own: Bidi_Class L is
// left-to-right, R and AL are right-to-left, and everything else — digits,
// punctuation, spaces, marks, the formatting characters — carries none.
enum class StrongDirection : std::uint8_t {
    None,
    Ltr,
    Rtl,
};
StrongDirection strong_direction(char32_t);

// Rules P2 and P3: the direction of the first strongly directional
// character, stepping over anything inside an isolate, and left-to-right
// when there is none. This is what `dir=auto` asks of an element's content
// and what `unicode-bidi: plaintext` asks of a paragraph.
bool first_strong_is_rtl(std::u32string_view);

// One paragraph, resolved. `levels` holds an embedding level per character
// of the input; `removed` marks the characters rule X9 takes out — the
// embeddings, the overrides, the pops and the boundary-neutrals — which are
// not drawn and take no part in the ordering.
struct BidiParagraph {
    std::vector<std::uint8_t> levels;
    std::vector<bool> removed;
    std::uint8_t paragraph_level = 0;
};

// Rules P2 through L1, over one paragraph taken as one line. Without a
// `paragraph_level` the base direction comes from P2 and P3.
BidiParagraph bidi_resolve(std::u32string_view text,
    std::optional<std::uint8_t> paragraph_level = std::nullopt);

// Rule L2: the characters in the order they are drawn, as indices into the
// text the paragraph was resolved from, with the removed ones left out.
std::vector<std::size_t> bidi_visual_order(BidiParagraph const&);

}
