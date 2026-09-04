#include "js/Strings.h"

#include "core/Unicode.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace sashfold::js {

namespace {

constexpr char digit_characters[] = "0123456789abcdefghijklmnopqrstuvwxyz";

bool is_ascii_digit(char16_t c)
{
    return c >= u'0' && c <= u'9';
}

// The value of a digit in any radix up to 36, letters in either case
// (§19.2.5 step 11), or −1 for anything else.
int digit_value(char16_t c)
{
    if (c >= u'0' && c <= u'9')
        return c - u'0';
    if (c >= u'a' && c <= u'z')
        return c - u'a' + 10;
    if (c >= u'A' && c <= u'Z')
        return c - u'A' + 10;
    return -1;
}

std::u16string ascii_to_utf16(std::string_view text)
{
    std::u16string out;
    out.reserve(text.size());
    for (char const c : text)
        out.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
    return out;
}

// A positive number as 0.digits × 10^exponent: the digits carry no
// leading or trailing zeros, and are empty only for zero. This is the
// (s, k, n) of §6.1.6.1.20 with n = exponent and k = digits.size().
struct Decimal {
    std::string digits;
    int exponent = 0;
};

// The shortest digits that round-trip (§6.1.6.1.20 step 5: k as small
// as possible, and the closest such s), which is exactly what the
// library's shortest scientific form is. x is finite and positive.
Decimal shortest_decimal(double x)
{
    char buffer[64];
    auto const result = std::to_chars(buffer, buffer + sizeof buffer, x, std::chars_format::scientific);
    std::string_view const text(buffer, static_cast<std::size_t>(result.ptr - buffer));
    std::size_t const e = text.find('e');
    Decimal d;
    for (char const c : text.substr(0, e))
        if (c != '.')
            d.digits.push_back(c);
    while (d.digits.size() > 1 && d.digits.back() == '0')
        d.digits.pop_back();
    std::size_t i = e + 1;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    int exponent = 0;
    for (; i < text.size(); ++i)
        exponent = exponent * 10 + (text[i] - '0');
    d.exponent = (negative ? -exponent : exponent) + 1;
    return d;
}

// The exact decimal expansion of a double. Every double is a dyadic
// rational whose expansion ends within 1074 fractional places (2^−1074
// is the smallest denormal), so asking the library for 1100 places
// prints the binary value exactly; toFixed and friends round that
// string themselves. x is finite and non-negative.
Decimal exact_decimal(double x)
{
    static constexpr int fraction_places = 1100;
    char buffer[1600]; // 309 integer digits, the point, the fraction
    // fabs: −0 would otherwise print its sign and land it in the digits.
    auto const result = std::to_chars(buffer, buffer + sizeof buffer, std::fabs(x), std::chars_format::fixed, fraction_places);
    std::string_view const text(buffer, static_cast<std::size_t>(result.ptr - buffer));
    std::size_t const point = text.find('.');
    std::string all(text.substr(0, point));
    int const integer_places = static_cast<int>(all.size());
    if (point != std::string_view::npos)
        all += text.substr(point + 1);
    std::size_t const first = all.find_first_not_of('0');
    if (first == std::string::npos)
        return {};
    std::size_t const last = all.find_last_not_of('0');
    Decimal d;
    d.digits = all.substr(first, last - first + 1);
    d.exponent = integer_places - static_cast<int>(first);
    return d;
}

// Rounds to `count` significant digits: to the nearest, and on a tie to
// the larger value — the "pick the larger n" of §21.1.3.3–.5. On an
// exact expansion that is simply "round up when the first dropped digit
// is 5 or more". A carry out of the top digit leaves count + 1 digits (a
// 1 and zeros) and a raised exponent; callers wanting exactly `count`
// trim the zero.
void round_significant(Decimal& d, std::size_t count)
{
    if (d.digits.size() <= count)
        return;
    bool const round_up = d.digits[count] >= '5';
    d.digits.resize(count);
    if (!round_up)
        return;
    std::size_t i = count;
    while (i > 0) {
        --i;
        if (d.digits[i] != '9') {
            ++d.digits[i];
            return;
        }
        d.digits[i] = '0';
    }
    d.digits.insert(d.digits.begin(), '1');
    ++d.exponent;
}

void append_exponent(std::string& out, int e)
{
    out += 'e';
    out += e < 0 ? '-' : '+';
    out += std::to_string(std::abs(e));
}

// Number::toString(x, radix) for radix ≠ 10 — the algorithm every
// engine shares (V8's DoubleToRadixCString): fraction digits are emitted
// while the remaining fraction still exceeds half an ulp of the input,
// so the output is as long as the double's precision warrants and no
// longer, and the last digit is rounded; integer digits the double
// cannot represent come out as zeros. x is finite and positive.
std::string radix_digits(double x, int radix)
{
    double integer = std::floor(x);
    double fraction = x - integer;
    double delta = 0.5 * (std::nextafter(x, std::numeric_limits<double>::infinity()) - x);
    delta = std::max(std::numeric_limits<double>::denorm_min(), delta);

    std::string fraction_digits;
    if (fraction >= delta) {
        do {
            fraction *= radix;
            delta *= radix;
            int const digit = static_cast<int>(fraction);
            fraction_digits.push_back(digit_characters[digit]);
            fraction -= digit;
            if (fraction > 0.5 || (fraction == 0.5 && (digit & 1) != 0)) {
                if (fraction + delta > 1) {
                    // Round up, carrying leftward; a carry out of the
                    // first fraction digit lands on the integer part.
                    while (true) {
                        if (fraction_digits.empty()) {
                            integer += 1;
                            break;
                        }
                        char const c = fraction_digits.back();
                        fraction_digits.pop_back();
                        int const previous = c > '9' ? c - 'a' + 10 : c - '0';
                        if (previous + 1 < radix) {
                            fraction_digits.push_back(digit_characters[previous + 1]);
                            break;
                        }
                    }
                    break;
                }
            }
        } while (fraction >= delta);
    }

    // Past 2^53 the quotient has no exact low digits; they are zeros.
    std::string integer_digits;
    while (integer / radix >= 9007199254740992.0) {
        integer /= radix;
        integer_digits.push_back('0');
    }
    do {
        double const remainder = std::fmod(integer, static_cast<double>(radix));
        integer_digits.push_back(digit_characters[static_cast<int>(remainder)]);
        integer = (integer - remainder) / radix;
    } while (integer > 0);
    std::reverse(integer_digits.begin(), integer_digits.end());

    if (fraction_digits.empty())
        return integer_digits;
    return integer_digits + "." + fraction_digits;
}

std::string number_to_ascii(double x, int radix)
{
    if (std::isnan(x))
        return "NaN";
    if (x == 0)
        return "0"; // both zeros (§6.1.6.1.20 step 2)
    std::string out;
    if (x < 0) {
        out.push_back('-');
        x = -x;
    }
    if (std::isinf(x)) {
        out += "Infinity";
        return out;
    }
    if (radix != 10) {
        out += radix_digits(x, radix);
        return out;
    }

    // §6.1.6.1.20 steps 6–10: the thresholds that decide between a plain
    // integer, a point, leading zeros, or the exponent form.
    Decimal const d = shortest_decimal(x);
    int const k = static_cast<int>(d.digits.size());
    int const n = d.exponent;
    if (k <= n && n <= 21) {
        out += d.digits;
        out.append(static_cast<std::size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out += d.digits.substr(0, static_cast<std::size_t>(n));
        out += '.';
        out += d.digits.substr(static_cast<std::size_t>(n));
    } else if (-6 < n && n <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-n), '0');
        out += d.digits;
    } else {
        out += d.digits[0];
        if (k > 1) {
            out += '.';
            out += d.digits.substr(1);
        }
        append_exponent(out, n - 1);
    }
    return out;
}

// The end of the longest match of StrUnsignedDecimalLiteral's digit
// forms (§7.1.4.1) starting at `pos` — DecimalDigits . DecimalDigits_opt,
// . DecimalDigits, or DecimalDigits, each with an optional ExponentPart
// — or nullopt when nothing matches. An incomplete exponent ("1e",
// "1e+") is not part of the match, which is what parseFloat's
// longest-prefix rule needs. Infinity is the caller's to check.
std::optional<std::size_t> scan_unsigned_decimal(std::u16string_view s, std::size_t pos)
{
    std::size_t i = pos;
    std::size_t integer_digits = 0;
    while (i < s.size() && is_ascii_digit(s[i])) {
        ++i;
        ++integer_digits;
    }
    if (i < s.size() && s[i] == u'.') {
        std::size_t j = i + 1;
        std::size_t fraction_digits = 0;
        while (j < s.size() && is_ascii_digit(s[j])) {
            ++j;
            ++fraction_digits;
        }
        if (integer_digits == 0 && fraction_digits == 0)
            return std::nullopt;
        i = j;
    } else if (integer_digits == 0) {
        return std::nullopt;
    }
    if (i < s.size() && (s[i] == u'e' || s[i] == u'E')) {
        std::size_t j = i + 1;
        if (j < s.size() && (s[j] == u'+' || s[j] == u'-'))
            ++j;
        std::size_t exponent_digits = 0;
        while (j < s.size() && is_ascii_digit(s[j])) {
            ++j;
            ++exponent_digits;
        }
        if (exponent_digits > 0)
            i = j;
    }
    return i;
}

// The Number value (§6.1.6.1: nearest, ties to even) of text that
// scan_unsigned_decimal matched whole. The text is re-laid as
// "I.Fe±X" so std::from_chars sees only the one shape it is documented
// to take, whatever else a library's strtod heritage might accept.
double decimal_value(std::u16string_view text)
{
    std::string integer_part;
    std::string fraction_part;
    std::size_t i = 0;
    while (i < text.size() && is_ascii_digit(text[i]))
        integer_part.push_back(static_cast<char>(text[i++]));
    if (i < text.size() && text[i] == u'.') {
        ++i;
        while (i < text.size() && is_ascii_digit(text[i]))
            fraction_part.push_back(static_cast<char>(text[i++]));
    }
    bool exponent_negative = false;
    long long exponent = 0;
    if (i < text.size() && (text[i] == u'e' || text[i] == u'E')) {
        ++i;
        if (i < text.size() && (text[i] == u'+' || text[i] == u'-'))
            exponent_negative = text[i++] == u'-';
        for (; i < text.size(); ++i) {
            // Saturate well past any exponent a double can carry, so an
            // absurdly long exponent still reads as "huge", not as garbage.
            if (exponent < 1000000000)
                exponent = exponent * 10 + (text[i] - u'0');
        }
    }

    std::string ascii = integer_part.empty() ? std::string("0") : integer_part;
    ascii += '.';
    ascii += fraction_part.empty() ? std::string("0") : fraction_part;
    ascii += 'e';
    if (exponent_negative)
        ascii += '-';
    ascii += std::to_string(exponent);

    double value = 0;
    bool out_of_range = false;
#if defined(__cpp_lib_to_chars)
    auto const result = std::from_chars(ascii.data(), ascii.data() + ascii.size(), value, std::chars_format::scientific);
    out_of_range = result.ec == std::errc::result_out_of_range;
#else
    // libc++ before LLVM 20 prints doubles but does not parse them; the
    // feature macro is withheld exactly until from_chars exists. strtod
    // is correctly rounded too, and the engine never touches the locale,
    // so the "C" decimal point holds. ERANGE alone is not the signal: a
    // denormal result raises it as well and is the right answer.
    errno = 0;
    char* end = nullptr;
    value = std::strtod(ascii.c_str(), &end);
    out_of_range = errno == ERANGE && (value == 0 || std::isinf(value));
#endif
    if (out_of_range) {
        // Overflow reads as Infinity and underflow as 0; the two are told
        // apart by where the leading nonzero digit sits once the exponent
        // is applied.
        std::string const all = integer_part + fraction_part;
        std::size_t const first = all.find_first_not_of('0');
        if (first == std::string::npos)
            return 0;
        long long const magnitude = static_cast<long long>(integer_part.size()) - static_cast<long long>(first) - 1
            + (exponent_negative ? -exponent : exponent);
        return magnitude >= 0 ? std::numeric_limits<double>::infinity() : 0;
    }
    return value;
}

// The Number value of digits in a power-of-two radix, rounded once:
// nearest, ties to even (§6.1.6.1). The digits are read straight into
// bits — the top 58 or so kept, the rest folded into a sticky bit — so
// a long hex literal never rounds twice on the way to a double.
double power_of_two_digits_value(std::u16string_view digits, int radix)
{
    int const bits_per_digit = std::countr_zero(static_cast<unsigned>(radix));
    std::uint64_t acc = 0;
    int dropped = 0;
    bool sticky = false;
    for (char16_t const c : digits) {
        auto const v = static_cast<std::uint64_t>(digit_value(c));
        if (acc == 0) {
            acc = v; // leading zeros contribute nothing
            continue;
        }
        if ((acc >> 58) == 0) {
            acc = (acc << bits_per_digit) | v;
        } else {
            dropped += bits_per_digit;
            sticky = sticky || v != 0;
        }
    }
    if (acc == 0)
        return 0;
    int const length = 64 - std::countl_zero(acc);
    if (length <= 53)
        return std::ldexp(static_cast<double>(acc), dropped);
    int const shift = length - 53;
    std::uint64_t mantissa = acc >> shift;
    bool const guard = ((acc >> (shift - 1)) & 1u) != 0;
    bool const below = (acc & ((std::uint64_t { 1 } << (shift - 1)) - 1)) != 0 || sticky;
    if (guard && (below || (mantissa & 1u) != 0))
        ++mantissa; // 2^53 is itself exact, so no renormalising
    return std::ldexp(static_cast<double>(mantissa), shift + dropped);
}

// Digits in a radix that is neither 10 nor a power of two: exact while
// the value fits 64 bits, and past that the implementation-approximated
// value §19.2.5 step 13 permits, accumulated in double as the shipping
// engines do.
double approximate_digits_value(std::u16string_view digits, int radix)
{
    auto const r = static_cast<std::uint64_t>(radix);
    std::uint64_t exact = 0;
    std::size_t i = 0;
    for (; i < digits.size(); ++i) {
        auto const v = static_cast<std::uint64_t>(digit_value(digits[i]));
        if (exact > (std::numeric_limits<std::uint64_t>::max() - v) / r)
            break;
        exact = exact * r + v;
    }
    double value = static_cast<double>(exact);
    for (; i < digits.size(); ++i)
        value = value * radix + digit_value(digits[i]);
    return value;
}

}

// ---------------------------------------------------------------- UTF-16 ⇄ WTF-8

std::u16string utf16_from_utf8(std::string_view bytes)
{
    std::u16string out;
    out.reserve(bytes.size());
    std::size_t i = 0;
    std::size_t const n = bytes.size();
    while (i < n) {
        auto const b = static_cast<unsigned char>(bytes[i]);
        if (b < 0x80) {
            out.push_back(static_cast<char16_t>(b));
            ++i;
            continue;
        }
        int needed = 0;
        char32_t code_point = 0;
        unsigned char lower = 0x80;
        unsigned char upper = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) {
            needed = 1;
            code_point = b & 0x1Fu;
        } else if (b >= 0xE0 && b <= 0xEF) {
            // WTF-8 differs from UTF-8 in one place: after 0xED the
            // second byte may run to 0xBF, which is how a lone surrogate
            // is written; UTF-8 proper stops it at 0x9F.
            needed = 2;
            code_point = b & 0x0Fu;
            if (b == 0xE0)
                lower = 0xA0;
        } else if (b >= 0xF0 && b <= 0xF4) {
            needed = 3;
            code_point = b & 0x07u;
            if (b == 0xF0)
                lower = 0x90;
            else if (b == 0xF4)
                upper = 0x8F;
        } else {
            out.push_back(replacement_character);
            ++i;
            continue;
        }
        ++i;
        bool complete = true;
        for (int k = 0; k < needed; ++k) {
            if (i >= n) {
                complete = false;
                break;
            }
            auto const c = static_cast<unsigned char>(bytes[i]);
            if (c < lower || c > upper) {
                // Not consumed: the byte starts the next sequence, so a
                // maximal invalid subpart costs exactly one U+FFFD.
                complete = false;
                break;
            }
            code_point = (code_point << 6) | (c & 0x3Fu);
            ++i;
            lower = 0x80;
            upper = 0xBF;
        }
        if (!complete) {
            out.push_back(replacement_character);
            continue;
        }
        append_code_point(out, code_point);
    }
    return out;
}

std::string utf8_from_utf16(std::u16string_view units)
{
    std::string out;
    out.reserve(units.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        char32_t code_point = units[i];
        if (code_point >= 0xD800 && code_point <= 0xDBFF && i + 1 < units.size()) {
            char32_t const next = units[i + 1];
            if (next >= 0xDC00 && next <= 0xDFFF) {
                code_point = 0x10000 + ((code_point - 0xD800) << 10) + (next - 0xDC00);
                ++i;
            }
        }
        // A surrogate left over takes the three-byte form (WTF-8).
        if (code_point < 0x80) {
            out.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }
    return out;
}

void append_code_point(std::u16string& out, char32_t code_point)
{
    // A surrogate code point is one unit: this is what keeps a lone
    // surrogate a lone surrogate across the WTF-8 crossing.
    if (code_point < 0x10000) {
        out.push_back(static_cast<char16_t>(code_point));
        return;
    }
    if (code_point > 0x10FFFF) {
        out.push_back(static_cast<char16_t>(replacement_character));
        return;
    }
    code_point -= 0x10000;
    out.push_back(static_cast<char16_t>(0xD800 + (code_point >> 10)));
    out.push_back(static_cast<char16_t>(0xDC00 + (code_point & 0x3FF)));
}

char32_t code_point_at(std::u16string_view s, std::size_t index, std::size_t* units)
{
    if (index >= s.size()) {
        if (units != nullptr)
            *units = 0;
        return 0;
    }
    char32_t const first = s[index];
    if (first >= 0xD800 && first <= 0xDBFF && index + 1 < s.size()) {
        char32_t const second = s[index + 1];
        if (second >= 0xDC00 && second <= 0xDFFF) {
            if (units != nullptr)
                *units = 2;
            return 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
        }
    }
    if (units != nullptr)
        *units = 1;
    return first;
}

// ------------------------------------------------------------- Number → String

std::u16string number_to_string(double x, int radix)
{
    return ascii_to_utf16(number_to_ascii(x, radix));
}

std::string number_to_utf8(double x)
{
    return number_to_ascii(x, 10);
}

std::u16string number_to_fixed(double x, int fraction_digits)
{
    // §21.1.3.3. The digit count was validated by the caller (0 … 100).
    if (!std::isfinite(x))
        return number_to_string(x);
    std::string out;
    if (x < 0) {
        out.push_back('-');
        x = -x;
    }
    if (x >= 1e21) {
        std::u16string result = ascii_to_utf16(out);
        result += number_to_string(x);
        return result;
    }
    auto const f = static_cast<std::size_t>(fraction_digits);
    // n is the integer nearest x × 10^f (the larger on a tie): the exact
    // digits above the 10^−f place, rounded there.
    Decimal d = exact_decimal(x);
    std::string m;
    if (!d.digits.empty() && d.exponent + fraction_digits >= 0) {
        auto const count = static_cast<std::size_t>(d.exponent + fraction_digits);
        round_significant(d, count);
        m = d.digits;
        if (m.size() < count)
            m.append(count - m.size(), '0');
    }
    if (m.empty())
        m = "0";
    if (fraction_digits != 0) {
        std::size_t k = m.size();
        if (k <= f) {
            m.insert(0, f + 1 - k, '0');
            k = f + 1;
        }
        m.insert(k - f, ".");
    }
    out += m;
    return ascii_to_utf16(out);
}

std::u16string number_to_exponential(double x, std::optional<int> fraction_digits)
{
    // §21.1.3.2. With no digit count the shortest round-trip digits are
    // used (step 10.b); with one, the exact expansion is rounded to
    // f + 1 significant digits (step 10.a).
    if (!std::isfinite(x))
        return number_to_string(x);
    std::string out;
    if (x < 0) {
        out.push_back('-');
        x = -x;
    }
    int f = fraction_digits.value_or(0);
    std::string m;
    int e = 0;
    if (x == 0) {
        m.assign(static_cast<std::size_t>(f) + 1, '0');
    } else if (!fraction_digits.has_value()) {
        Decimal const d = shortest_decimal(x);
        m = d.digits;
        f = static_cast<int>(m.size()) - 1;
        e = d.exponent - 1;
    } else {
        Decimal d = exact_decimal(x);
        auto const count = static_cast<std::size_t>(f) + 1;
        round_significant(d, count);
        d.digits.resize(count, '0');
        m = d.digits;
        e = d.exponent - 1;
    }
    if (f != 0)
        m.insert(1, ".");
    out += m;
    append_exponent(out, e);
    return ascii_to_utf16(out);
}

std::u16string number_to_precision(double x, int precision)
{
    // §21.1.3.5. The precision was validated by the caller (1 … 100).
    if (!std::isfinite(x))
        return number_to_string(x);
    std::string out;
    if (x < 0) {
        out.push_back('-');
        x = -x;
    }
    auto const p = static_cast<std::size_t>(precision);
    std::string m;
    int e = 0;
    if (x == 0) {
        m.assign(p, '0');
    } else {
        Decimal d = exact_decimal(x);
        round_significant(d, p);
        d.digits.resize(p, '0');
        m = d.digits;
        e = d.exponent - 1;
    }
    if (e < -6 || e >= precision) {
        if (p != 1)
            m.insert(1, ".");
        out += m;
        append_exponent(out, e);
        return ascii_to_utf16(out);
    }
    if (e == precision - 1) {
        out += m;
        return ascii_to_utf16(out);
    }
    if (e >= 0)
        m.insert(static_cast<std::size_t>(e) + 1, ".");
    else
        m.insert(0, "0." + std::string(static_cast<std::size_t>(-(e + 1)), '0'));
    out += m;
    return ascii_to_utf16(out);
}

// ------------------------------------------------------------- String → Number

double string_to_number(std::u16string_view input)
{
    // §7.1.4.1.1 StringToNumber over the StringNumericLiteral grammar.
    std::u16string_view const s = trim_string(input);
    if (s.empty())
        return 0;

    // NonDecimalIntegerLiteral: 0x, 0o, 0b, with no sign and no
    // separators (§7.1.4.1 StrNumericLiteral).
    if (s.size() > 2 && s[0] == u'0') {
        int radix = 0;
        if (s[1] == u'x' || s[1] == u'X')
            radix = 16;
        else if (s[1] == u'o' || s[1] == u'O')
            radix = 8;
        else if (s[1] == u'b' || s[1] == u'B')
            radix = 2;
        if (radix != 0) {
            std::u16string_view const digits = s.substr(2);
            for (char16_t const c : digits) {
                int const v = digit_value(c);
                if (v < 0 || v >= radix)
                    return std::numeric_limits<double>::quiet_NaN();
            }
            return power_of_two_digits_value(digits, radix);
        }
    }

    bool negative = false;
    std::size_t pos = 0;
    if (s[0] == u'+' || s[0] == u'-') {
        negative = s[0] == u'-';
        pos = 1;
    }
    std::u16string_view const body = s.substr(pos);
    double magnitude = 0;
    if (body == u"Infinity") {
        magnitude = std::numeric_limits<double>::infinity();
    } else {
        auto const end = scan_unsigned_decimal(body, 0);
        if (!end.has_value() || *end != body.size())
            return std::numeric_limits<double>::quiet_NaN();
        magnitude = decimal_value(body);
    }
    // "-0" is −0: the sign survives a zero magnitude (§7.1.4.1.1 note).
    return negative ? -magnitude : magnitude;
}

std::optional<std::uint32_t> array_index_of(std::u16string_view s)
{
    // The canonical numeric string of an integer 0 … 2^32 − 2 (§6.1.7):
    // no sign, no leading zero except "0" itself, at most ten digits.
    if (s.empty() || s.size() > 10)
        return std::nullopt;
    if (s[0] == u'0')
        return s.size() == 1 ? std::optional<std::uint32_t>(0) : std::nullopt;
    std::uint64_t value = 0;
    for (char16_t const c : s) {
        if (!is_ascii_digit(c))
            return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - u'0');
    }
    if (value > 4294967294ull)
        return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

bool is_canonical_numeric_string(std::u16string_view s)
{
    // CanonicalNumericIndexString (§7.1.21): "-0" is one by fiat, and
    // otherwise the string must survive a round trip through ToNumber
    // and ToString — which makes "NaN" and "Infinity" canonical too.
    if (s == u"-0")
        return true;
    return number_to_string(string_to_number(s)) == s;
}

bool is_string_whitespace(char16_t c)
{
    // WhiteSpace (§12.2: TAB, VT, FF, ZWNBSP and every Zs) and
    // LineTerminator (§12.3). Zs is a closed list in Unicode 16: SPACE,
    // NBSP, OGHAM SPACE MARK, the EN QUAD … HAIR SPACE run, NARROW NBSP,
    // MEDIUM MATHEMATICAL SPACE and IDEOGRAPHIC SPACE.
    switch (c) {
    case 0x0009:
    case 0x000B:
    case 0x000C:
    case 0x0020:
    case 0x00A0:
    case 0xFEFF:
    case 0x1680:
    case 0x202F:
    case 0x205F:
    case 0x3000:
    case 0x000A:
    case 0x000D:
    case 0x2028:
    case 0x2029:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

std::u16string_view trim_string(std::u16string_view s, bool start, bool end)
{
    if (start)
        while (!s.empty() && is_string_whitespace(s.front()))
            s.remove_prefix(1);
    if (end)
        while (!s.empty() && is_string_whitespace(s.back()))
            s.remove_suffix(1);
    return s;
}

double parse_int(std::u16string_view input, int radix)
{
    // §19.2.5. Only the start is trimmed (step 2); trailing whitespace
    // simply ends the digits.
    std::u16string_view s = trim_string(input, true, false);
    bool negative = false;
    if (!s.empty() && (s[0] == u'+' || s[0] == u'-')) {
        negative = s[0] == u'-';
        s.remove_prefix(1);
    }
    bool strip_prefix = true;
    if (radix != 0) {
        if (radix < 2 || radix > 36)
            return std::numeric_limits<double>::quiet_NaN();
        if (radix != 16)
            strip_prefix = false;
    } else {
        radix = 10;
    }
    if (strip_prefix && s.size() >= 2 && s[0] == u'0' && (s[1] == u'x' || s[1] == u'X')) {
        s.remove_prefix(2);
        radix = 16;
    }
    std::size_t end = 0;
    while (end < s.size()) {
        int const v = digit_value(s[end]);
        if (v < 0 || v >= radix)
            break;
        ++end;
    }
    if (end == 0)
        return std::numeric_limits<double>::quiet_NaN();
    std::u16string_view const z = s.substr(0, end);
    double value = 0;
    if (radix == 10)
        value = decimal_value(z);
    else if ((radix & (radix - 1)) == 0)
        value = power_of_two_digits_value(z, radix);
    else
        value = approximate_digits_value(z, radix);
    // Step 14: a zero keeps the sign it was written with, so "-0" is −0.
    return negative ? -value : value;
}

double parse_float(std::u16string_view input)
{
    // §19.2.4: the longest prefix that is a StrDecimalLiteral — a sign,
    // then Infinity or the decimal digit forms; no hex, no separators.
    std::u16string_view const s = trim_string(input, true, false);
    bool negative = false;
    std::size_t pos = 0;
    if (!s.empty() && (s[0] == u'+' || s[0] == u'-')) {
        negative = s[0] == u'-';
        pos = 1;
    }
    std::u16string_view const body = s.substr(pos);
    if (body.starts_with(u"Infinity"))
        return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    auto const end = scan_unsigned_decimal(body, 0);
    if (!end.has_value())
        return std::numeric_limits<double>::quiet_NaN();
    double const magnitude = decimal_value(body.substr(0, *end));
    return negative ? -magnitude : magnitude;
}

// ------------------------------------------------------------------ Case

// Simple case mapping only: the full mappings (ß → SS, ŉ → ʼN, final
// sigma, the Turkish dotted i) change the length or need a neighbour,
// and String.prototype.toUpperCase is specified over them — a gap this
// v0 leaves open on purpose rather than half-fills.

std::u16string to_upper(std::u16string_view s)
{
    std::u16string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        std::size_t units = 0;
        char32_t const code_point = code_point_at(s, i, &units);
        append_code_point(out, to_uppercase(code_point));
        i += units;
    }
    return out;
}

std::u16string to_lower(std::u16string_view s)
{
    std::u16string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        std::size_t units = 0;
        char32_t const code_point = code_point_at(s, i, &units);
        append_code_point(out, to_lowercase(code_point));
        i += units;
    }
    return out;
}

}
