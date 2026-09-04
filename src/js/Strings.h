#pragma once

// String and number conversions the language pins down exactly:
// Number::toString's shortest round-trip digits (§6.1.6.1.20),
// StringToNumber's grammar (§7.1.4.1.1), array-index recognition, and the
// UTF-16 ⇄ WTF-8 crossing every string makes at the DOM boundary.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sashfold::js {

// WTF-8 both ways: a lone surrogate maps to a lone surrogate; an invalid
// byte sequence becomes U+FFFD.
std::u16string utf16_from_utf8(std::string_view);
std::string utf8_from_utf16(std::u16string_view);
void append_code_point(std::u16string&, char32_t); // as one unit or a surrogate pair
// The code point at `index` (a pair is joined; a lone surrogate is itself)
// and how many units it spans.
char32_t code_point_at(std::u16string_view, std::size_t index, std::size_t* units = nullptr);

// Number::toString(x, radix). Radix 10 gives the shortest digits that
// round-trip, laid out by the specification's thresholds (1e21, 1e-7).
std::u16string number_to_string(double, int radix = 10);
std::string number_to_utf8(double); // radix 10, for messages
// StringToNumber: whitespace trimmed, "" → 0, decimal with optional
// exponent, 0x/0o/0b integers, ±Infinity; anything else NaN.
double string_to_number(std::u16string_view);
// The canonical numeric string of an array index 0 … 2^32 − 2, if it is one.
std::optional<std::uint32_t> array_index_of(std::u16string_view);
// Is this the canonical string of some Number (CanonicalNumericIndexString)?
bool is_canonical_numeric_string(std::u16string_view);

// §12.2 whitespace and line terminators as a string trim sees them.
bool is_string_whitespace(char16_t);
std::u16string_view trim_string(std::u16string_view, bool start = true, bool end = true);

// Number.prototype's fixed-form conversions (§21.1.3.3–.5); the digit
// count is already validated by the caller.
std::u16string number_to_fixed(double, int fraction_digits);
std::u16string number_to_exponential(double, std::optional<int> fraction_digits);
std::u16string number_to_precision(double, int precision);

// parseInt / parseFloat (§19.2.4, §19.2.5).
double parse_int(std::u16string_view, int radix);
double parse_float(std::u16string_view);

// Simple case mapping over code points (String.prototype.toUpperCase does
// not do the special casings; ß stays ß).
std::u16string to_upper(std::u16string_view);
std::u16string to_lower(std::u16string_view);

}
