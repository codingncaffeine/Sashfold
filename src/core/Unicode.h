#pragma once

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

}
