#pragma once

// Line breaking (UAX #14): the Line_Break property of every code point,
// resolved as LB1 has it, and the break opportunities of a text — where a
// line may be broken between two characters and where it must be — by
// the rules LB2 to LB31 of Unicode 16.0, nothing tailored. Layout wraps
// at what this allows; a word that fits nowhere is sliced by the line,
// not here. The conformance file LineBreakTest.txt is the test.

#include "core/LineBreakData.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sashfold {

LineBreakClass line_break_class(char32_t code_point);
// The flags beside the class: East_Asian_Width F, W or H; a Pi or Pf
// quotation mark; an unassigned Extended_Pictographic code point.
std::uint8_t line_break_flags(char32_t code_point);

enum class LineBreak : std::uint8_t {
    None, // the two characters stay together
    Allowed, // a line may end before this character
    Mandatory, // a line ends before this character
};

// How CSS tailors the rules for a character (css-text-3 §3 white-space,
// §5.2 word-break, §5.3 line-break): flags, one set per character of the
// text. None is the algorithm as Unicode writes it, which takes a small
// kana (CJ) as NS — the strict resolution, and the default.
constexpr std::uint8_t line_break_tailor_break_all = 1; // word-break: break-all — a letter or a digit breaks like an ideograph
constexpr std::uint8_t line_break_tailor_keep_all = 2; // word-break: keep-all — no break between letters, digits and ideographs
// line-break: normal and loose — a small kana may start a line, so may an
// iteration mark, and a line may end between two inseparable characters.
constexpr std::uint8_t line_break_tailor_normal = 4;
constexpr std::uint8_t line_break_tailor_loose = 8; // line-break: loose — a hyphen may start a line after an ideograph
constexpr std::uint8_t line_break_tailor_anywhere = 16; // line-break: anywhere — a break around every character
constexpr std::uint8_t line_break_tailor_break_spaces = 32; // white-space: break-spaces — a break after every space
// Thai, Lao, Khmer and Myanmar (class SA) write no spaces between their
// words, which take a dictionary to find; with none, a line may end
// between any two of their clusters rather than nowhere at all. LB1 alone
// takes them as AL, whole from space to space.
constexpr std::uint8_t line_break_tailor_clusters = 64;

// CSS's "other space separators": the space separators (Zs) other than
// the space and the no-break space, which hang past a line's end the way
// preserved spaces do.
bool is_other_space_separator(char32_t code_point);

// One entry per boundary of the text: [0] is before the first character
// (never a break), [i] is before character i, [size] is the end of the
// text, which is always a break. `tailoring` holds a character's flags
// at its index, or nothing at all; a break the flags of both neighbours
// ask for is honoured, a class one character's flags change is changed
// for that character alone.
std::vector<LineBreak> line_break_opportunities(std::u32string_view text,
    std::span<std::uint8_t const> tailoring = {});

}
