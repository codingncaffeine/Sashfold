#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sashfold {

inline constexpr char32_t replacement_character = 0xFFFD;

constexpr bool is_surrogate(char32_t c) { return c >= 0xD800 && c <= 0xDFFF; }
constexpr bool is_scalar_value(char32_t c) { return c <= 0x10FFFF && !is_surrogate(c); }

constexpr bool is_noncharacter(char32_t c)
{
    if (c >= 0xFDD0 && c <= 0xFDEF)
        return true;
    char32_t const low = c & 0xFFFF;
    return c <= 0x10FFFF && (low == 0xFFFE || low == 0xFFFF);
}

constexpr bool is_c0_control(char32_t c) { return c <= 0x1F; }
constexpr bool is_control(char32_t c) { return is_c0_control(c) || (c >= 0x7F && c <= 0x9F); }

// Encodes a code point as UTF-8. Deliberately permissive about surrogates
// (WTF-8): internal strings must round-trip conformance-test inputs that
// contain lone surrogates. Never use for bytes leaving the engine.
void append_utf8(std::string& out, char32_t code_point);
std::string to_utf8(std::u32string_view);

// Decodes UTF-8 to code points, replacing invalid sequences with U+FFFD.
// permit_surrogates selects WTF-8 leniency for internal round-trips; leave it
// false for bytes that came from outside.
std::u32string decode_utf8(std::string_view, bool permit_surrogates = false);

// Canonical composition (UAX #15) over the generated tables; Normalize.cpp.
std::u32string nfc(std::u32string_view);

// General category Mn, Mc, or Me.
bool is_combining_mark(char32_t);

// General category Ps, Pe, Pi, Pf or Po: the punctuation a ::first-letter
// keeps on either side of its letter (CSS 2.1 §5.12.2).
bool is_first_letter_punctuation(char32_t);

// General category Zs, Zl, Zp, Cc or Cf: a space of any width, a separator,
// a control or a formatting character — what a ::first-letter steps over on
// its way to the letter, and never selects.
bool is_first_letter_skipped(char32_t);

// The fully expanded canonical decomposition of a precomposed code point
// (Hangul excluded: it is algorithmic), or empty when it is its own.
std::u32string_view canonical_decomposition(char32_t);

// The simple case mappings, one code point in and one out — what
// text-transform's `uppercase`, `lowercase` and `capitalize` are written in
// terms of. A code point with no mapping of that kind comes back unchanged.
// SIMPLE is the word that matters: ß stays ß rather than becoming SS, and
// the Turkish dotted i and the Greek final sigma, which need a language or a
// neighbour to decide, are not spelled here.
char32_t to_uppercase(char32_t);
char32_t to_lowercase(char32_t);
char32_t to_titlecase(char32_t);

// Which direction a code point carries on its own (UAX #9): Bidi_Class L is
// left-to-right, R and AL are right-to-left, and everything else — digits,
// punctuation, spaces, marks, the formatting characters — carries none.
enum class StrongDirection : std::uint8_t {
    None,
    Ltr,
    Rtl,
};
StrongDirection strong_direction(char32_t);

// UAX #9's P2 and P3: the direction of the first strongly directional
// character, stepping over anything inside an isolate, and left-to-right
// when there is none. This is what `dir=auto` asks of an element's content
// and what `unicode-bidi: plaintext` asks of a paragraph; Bidi.cpp.
bool first_strong_is_rtl(std::u32string_view);

// Default_Ignorable_Code_Point (Unicode 16): characters that render as
// nothing — the soft hyphen, zero-width spaces and joiners, bidi marks,
// variation selectors, tags. Layout drops them before line building.
constexpr bool is_default_ignorable(char32_t c)
{
    return c == 0x00AD || c == 0x034F || c == 0x061C || (c >= 0x115F && c <= 0x1160)
        || (c >= 0x17B4 && c <= 0x17B5) || (c >= 0x180B && c <= 0x180F)
        || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E)
        || (c >= 0x2060 && c <= 0x206F) || c == 0x3164 || (c >= 0xFE00 && c <= 0xFE0F)
        || c == 0xFEFF || c == 0xFFA0 || (c >= 0xFFF0 && c <= 0xFFF8)
        || (c >= 0x1BCA0 && c <= 0x1BCA3) || (c >= 0x1D173 && c <= 0x1D17A)
        || (c >= 0xE0000 && c <= 0xE0FFF);
}

}
