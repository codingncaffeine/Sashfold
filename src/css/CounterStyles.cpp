#include "css/ComputedStyle.h"

#include "core/Unicode.h"

#include <cstddef>
#include <iterator>
#include <string>

namespace sashfold::css {

namespace {

// The decimal digits with the sign kept, padded to at least `digits` of
// them: decimal-leading-zero writes 5 as "05" and -9 as "-09", the pad
// going after the sign because the sign is not a digit.
std::string decimal(int value, std::size_t digits = 1)
{
    // -2147483648 has no positive counterpart, so the digits are taken from
    // the unsigned magnitude rather than from a negated int.
    bool const negative = value < 0;
    auto magnitude = static_cast<unsigned int>(negative ? -static_cast<long long>(value) : value);
    std::string body = std::to_string(magnitude);
    while (body.size() < digits)
        body.insert(body.begin(), '0');
    return negative ? "-" + body : body;
}

// A bijective base-N alphabet: 1 is the first letter, N the last, N+1 the
// first letter twice. There is no zero digit, so each step takes one away
// before dividing — that is what makes 26 "z" and 27 "aa".
std::string alphabetic(int value, char32_t first, unsigned int letters)
{
    if (value < 1)
        return decimal(value);
    std::u32string reversed;
    auto remaining = static_cast<unsigned int>(value);
    while (remaining > 0) {
        unsigned int const index = (remaining - 1) % letters;
        reversed.push_back(first + index);
        remaining = (remaining - 1) / letters;
    }
    std::string out;
    for (std::size_t i = reversed.size(); i-- > 0;)
        append_utf8(out, reversed[i]);
    return out;
}

// The Greek alphabet lower-greek numbers with: alpha through omega with
// the final sigma left out, so twenty-four letters from U+03B1 — except
// that final sigma sits between sigma and tau in the block, so the letters
// past rho are one code point further along.
std::string lower_greek(int value)
{
    if (value < 1)
        return decimal(value);
    std::u32string reversed;
    auto remaining = static_cast<unsigned int>(value);
    while (remaining > 0) {
        unsigned int const index = (remaining - 1) % 24;
        reversed.push_back(static_cast<char32_t>(0x03B1 + index + (index >= 17 ? 1 : 0)));
        remaining = (remaining - 1) / 24;
    }
    std::string out;
    for (std::size_t i = reversed.size(); i-- > 0;)
        append_utf8(out, reversed[i]);
    return out;
}

// One numeral of an additive system, and the run of them a value spells.
struct Numeral {
    int weight;
    char32_t code_point;
};

std::string additive(int value, Numeral const* numerals, std::size_t count, int highest)
{
    if (value < 1 || value > highest)
        return decimal(value);
    std::string out;
    int remaining = value;
    for (std::size_t i = 0; i < count && remaining > 0; ++i) {
        while (remaining >= numerals[i].weight) {
            append_utf8(out, numerals[i].code_point);
            remaining -= numerals[i].weight;
        }
    }
    return out;
}

// Roman numerals are additive with the six subtractive pairs written in,
// which is why CM and CD sit in the table beside M, D, C.
std::string roman(int value, bool upper)
{
    if (value < 1 || value > 3999)
        return decimal(value);
    static constexpr int weights[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    static constexpr char const* upper_signs[]
        = { "M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I" };
    static constexpr char const* lower_signs[]
        = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
    std::string out;
    int remaining = value;
    for (std::size_t i = 0; i < 13; ++i) {
        while (remaining >= weights[i]) {
            out += upper ? upper_signs[i] : lower_signs[i];
            remaining -= weights[i];
        }
    }
    return out;
}

// The Armenian numerals are laid out in their block in numeric order — the
// units, then the tens, then the hundreds, then the thousands — so the
// letter for digit d at power p is nine steps per power from Ա (U+0531).
std::string armenian(int value)
{
    if (value < 1 || value > 9999)
        return decimal(value);
    std::string out;
    int remaining = value;
    int power = 1000;
    for (int step = 3; step >= 0; --step) {
        int const digit = remaining / power;
        if (digit > 0) {
            append_utf8(out, static_cast<char32_t>(0x0531 + step * 9 + (digit - 1)));
            remaining -= digit * power;
        }
        power /= 10;
    }
    return out;
}

// Georgian is additive too but its letters are not in numeric order in the
// block — the eight that Mkhedruli added sit past the rest — so the table
// is written out.
std::string georgian(int value)
{
    static constexpr Numeral numerals[] = {
        { 10000, 0x10F5 }, { 9000, 0x10F0 }, { 8000, 0x10EF }, { 7000, 0x10F4 },
        { 6000, 0x10EE }, { 5000, 0x10ED }, { 4000, 0x10EC }, { 3000, 0x10EB },
        { 2000, 0x10EA }, { 1000, 0x10E9 }, { 900, 0x10E8 }, { 800, 0x10E7 },
        { 700, 0x10E6 }, { 600, 0x10E5 }, { 500, 0x10E4 }, { 400, 0x10F3 },
        { 300, 0x10E2 }, { 200, 0x10E1 }, { 100, 0x10E0 }, { 90, 0x10DF },
        { 80, 0x10DE }, { 70, 0x10DD }, { 60, 0x10F2 }, { 50, 0x10DC },
        { 40, 0x10DB }, { 30, 0x10DA }, { 20, 0x10D9 }, { 10, 0x10D8 },
        { 9, 0x10D7 }, { 8, 0x10F1 }, { 7, 0x10D6 }, { 6, 0x10D5 },
        { 5, 0x10D4 }, { 4, 0x10D3 }, { 3, 0x10D2 }, { 2, 0x10D1 },
        { 1, 0x10D0 },
    };
    return additive(value, numerals, std::size(numerals), 19999);
}

}

std::string format_counter(int value, ListStyleType style)
{
    switch (style) {
    case ListStyleType::None:
        return {};
    case ListStyleType::Disc:
        return "•";
    case ListStyleType::Circle:
        return "◦";
    case ListStyleType::Square:
        return "▪";
    case ListStyleType::Decimal:
        return decimal(value);
    case ListStyleType::DecimalLeadingZero:
        return decimal(value, 2);
    case ListStyleType::LowerRoman:
        return roman(value, false);
    case ListStyleType::UpperRoman:
        return roman(value, true);
    case ListStyleType::LowerAlpha:
        return alphabetic(value, U'a', 26);
    case ListStyleType::UpperAlpha:
        return alphabetic(value, U'A', 26);
    case ListStyleType::LowerGreek:
        return lower_greek(value);
    case ListStyleType::Armenian:
        return armenian(value);
    case ListStyleType::Georgian:
        return georgian(value);
    }
    return decimal(value);
}

}
