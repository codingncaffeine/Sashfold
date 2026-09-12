#pragma once

// The mathematical integers behind the language's BigInt values
// (§6.1.6.2): a sign and a magnitude of 32-bit limbs, least significant
// first, with the arithmetic the operators need — exact at every size a
// script cares to reach, up to a limit that keeps a runaway `**` or `<<`
// from taking the machine — and the conversions to and from Numbers and
// strings in any radix. A plain value type; the heap wraps one in a cell.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::js {

class BigInteger {
public:
    BigInteger() = default; // zero

    static BigInteger from_int64(std::int64_t);
    static BigInteger from_uint64(std::uint64_t);
    // An integral, finite Number; nullopt for any other.
    static std::optional<BigInteger> from_double(double);
    // Digits in a radix from 2 to 36, no sign and no prefix; nullopt on a
    // digit outside the radix or on no digits at all.
    static std::optional<BigInteger> parse_digits(std::string_view digits, int radix);
    // StringToBigInt (§7.1.14): the string trimmed of whitespace, then a
    // 0x, 0o or 0b integer (no sign), or a decimal integer with an
    // optional sign; the empty string is zero. Nullopt is "undefined":
    // the string spells no integer.
    static std::optional<BigInteger> from_string(std::u16string_view text);

    bool is_zero() const { return m_limbs.empty(); }
    bool is_negative() const { return m_negative; }
    bool is_odd() const { return !m_limbs.empty() && (m_limbs.front() & 1u) != 0; }
    std::size_t bit_length() const; // of the magnitude; 0 for zero
    std::size_t limb_count() const { return m_limbs.size(); }

    std::string to_string(int radix = 10) const;
    // Number(bigint): the nearest double, ties to even; ±infinity past the range.
    double to_double() const;
    // The low 64 bits, as two's complement.
    std::uint64_t to_uint64_wrapping() const;
    std::int64_t to_int64_wrapping() const { return static_cast<std::int64_t>(to_uint64_wrapping()); }
    // The value when it fits the type; nullopt when it does not.
    std::optional<std::int64_t> to_int64() const;
    std::optional<std::uint64_t> to_uint64() const;
    std::size_t hash() const;

    friend bool operator==(BigInteger const&, BigInteger const&) = default;
    // -1, 0 or 1.
    friend int compare(BigInteger const& a, BigInteger const& b);
    // Against a finite Number (the caller settles NaN and the infinities).
    int compare_double(double number) const;

    friend BigInteger operator+(BigInteger const&, BigInteger const&);
    friend BigInteger operator-(BigInteger const&, BigInteger const&);
    friend BigInteger operator*(BigInteger const&, BigInteger const&);
    BigInteger negated() const;
    // Truncating division and its remainder (§6.1.6.2.5, §6.1.6.2.6);
    // nullopt on a zero divisor.
    static std::optional<BigInteger> divide(BigInteger const& dividend, BigInteger const& divisor);
    static std::optional<BigInteger> remainder(BigInteger const& dividend, BigInteger const& divisor);
    // §6.1.6.2.3: the result, or why there is none (defined below the class).
    struct Power;
    static Power power(BigInteger const& base, BigInteger const& exponent);
    // §6.1.6.2.9 and .10: by a count that may be negative (the other
    // direction) or beyond any width (zero or -1 rightward); nullopt when
    // the result would pass the size limit.
    static std::optional<BigInteger> shift_left(BigInteger const&, BigInteger const& count);
    static std::optional<BigInteger> shift_right(BigInteger const&, BigInteger const& count);
    // §6.1.6.2.17–20, over the two's complement of infinite width.
    friend BigInteger operator&(BigInteger const&, BigInteger const&);
    friend BigInteger operator|(BigInteger const&, BigInteger const&);
    friend BigInteger operator^(BigInteger const&, BigInteger const&);
    BigInteger bitwise_not() const; // -x - 1
    // BigInt.asIntN and asUintN (§21.2.2.1, §21.2.2.2).
    static BigInteger as_int_n(std::size_t bits, BigInteger const&);
    static BigInteger as_uint_n(std::size_t bits, BigInteger const&);

    // Past this many limbs a result is refused (a RangeError to the
    // script): 64 million bits.
    static constexpr std::size_t max_limbs = std::size_t(1) << 21;

private:
    using Limbs = std::vector<std::uint32_t>;
    static BigInteger make(Limbs limbs, bool negative);
    static void divide_magnitudes(Limbs const& dividend, Limbs const& divisor, Limbs& quotient, Limbs& remainder);
    static Limbs twos_complement(BigInteger const&, std::size_t width);
    static BigInteger from_twos_complement(Limbs limbs);

    Limbs m_limbs; // no leading zero limbs; empty is zero
    bool m_negative = false; // never for zero
};

struct BigInteger::Power {
    std::optional<BigInteger> value;
    bool negative_exponent = false;
    bool too_large = false;
};

}
