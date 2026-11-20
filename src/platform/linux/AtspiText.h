#pragma once

// The text a node offers through AT-SPI's Text interface, as web engines
// give it on this platform: its runs of text with an embedded object
// character (U+FFFC) in place of each child object, offsets in characters
// (code points), and the boundaries Text asks for (AtspiTextBoundaryType,
// AtspiTextGranularity) found in it.

#include "a11y/Tree.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::platform::atspi {

inline constexpr std::string_view embedded_object = "\xEF\xBF\xBC"; // U+FFFC

std::size_t code_point_count(std::string_view text);
// The byte where character `index` begins (the length past the end).
std::size_t byte_of(std::string_view text, std::size_t index);
// Characters `from` to `to` (to past the end: to the end).
std::string slice(std::string_view text, std::size_t from, std::size_t to);
char32_t character_at(std::string_view text, std::size_t index);
// The characters before UTF-16 offset `utf16` (the model counts UTF-16 code
// units, AT-SPI characters), and back.
std::uint32_t code_points_before(std::string_view text, std::uint32_t utf16);
std::uint32_t utf16_before(std::string_view text, std::uint32_t code_points);

struct FlatText {
    std::string text;
    std::size_t length = 0; // in characters
    struct Piece {
        a11y::NodeId node = 0; // the run of text, or the child object
        std::size_t start = 0;
        std::size_t end = 0;
        bool object = false;
    };
    std::vector<Piece> pieces;
    void append_text(std::string_view run, a11y::NodeId node, std::size_t);
    void append_object(a11y::NodeId node);
    // The piece holding character `offset`; null past the end.
    Piece const* piece_at(std::size_t offset) const;
    // The child objects, in order (Hypertext's links).
    std::vector<Piece const*> objects() const;
};

// What changed between two texts: one removed stretch and one inserted, at
// the same character offset (the common start and end kept).
struct TextChange {
    std::size_t start = 0;
    std::size_t removed_length = 0;
    std::string removed;
    std::size_t inserted_length = 0;
    std::string inserted;
};
TextChange diff_text(std::string_view before, std::string_view after);

// Boundaries in a text, characters: the range [start, end) Text's
// GetTextAtOffset / Before / After answer for a boundary type (0 char,
// 1 word start, 2 word end, 3 sentence start, 4 sentence end, 5 line
// start, 6 line end), given the text's lines (from its layout, or its
// newlines when it has none).
struct Range {
    std::size_t start = 0;
    std::size_t end = 0;
};
std::vector<Range> lines_by_newline(std::string_view text);
Range boundary_at(std::string_view text, std::vector<Range> const& lines, std::size_t offset, std::uint32_t boundary);
Range boundary_before(std::string_view text, std::vector<Range> const& lines, std::size_t offset, std::uint32_t boundary);
Range boundary_after(std::string_view text, std::vector<Range> const& lines, std::size_t offset, std::uint32_t boundary);
// GetStringAtOffset's granularity (0 char, 1 word, 2 sentence, 3 line,
// 4 paragraph): the unit holding the offset, without what separates it.
Range granule_at(std::string_view text, std::vector<Range> const& lines, std::size_t offset, std::uint32_t granularity);

}
