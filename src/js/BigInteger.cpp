#include "js/BigInteger.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sashfold::js {

namespace {

using Limbs = std::vector<std::uint32_t>;

constexpr std::uint64_t limb_base = std::uint64_t(1) << 32;

void trim(Limbs& limbs)
{
    while (!limbs.empty() && limbs.back() == 0)
        limbs.pop_back();
}

int compare_magnitudes(Limbs const& a, Limbs const& b)
{
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    for (std::size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

Limbs add_magnitudes(Limbs const& a, Limbs const& b)
{
    Limbs const& longer = a.size() >= b.size() ? a : b;
    Limbs const& shorter = a.size() >= b.size() ? b : a;
    Limbs out;
    out.reserve(longer.size() + 1);
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < longer.size(); ++i) {
        std::uint64_t const sum = static_cast<std::uint64_t>(longer[i]) + (i < shorter.size() ? shorter[i] : 0u) + carry;
        out.push_back(static_cast<std::uint32_t>(sum));
        carry = sum >> 32;
    }
    if (carry != 0)
        out.push_back(static_cast<std::uint32_t>(carry));
    return out;
}

// a minus b, for |a| >= |b|.
Limbs subtract_magnitudes(Limbs const& a, Limbs const& b)
{
    Limbs out;
    out.reserve(a.size());
    std::int64_t borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::int64_t diff = static_cast<std::int64_t>(a[i]) - (i < b.size() ? b[i] : 0u) - borrow;
        borrow = 0;
        if (diff < 0) {
            diff += static_cast<std::int64_t>(limb_base);
            borrow = 1;
        }
        out.push_back(static_cast<std::uint32_t>(diff));
    }
    trim(out);
    return out;
}

Limbs multiply_magnitudes(Limbs const& a, Limbs const& b)
{
    if (a.empty() || b.empty())
        return {};
    Limbs out(a.size() + b.size(), 0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < b.size(); ++j) {
            std::uint64_t const cur = static_cast<std::uint64_t>(a[i]) * b[j] + out[i + j] + carry;
            out[i + j] = static_cast<std::uint32_t>(cur);
            carry = cur >> 32;
        }
        std::size_t k = i + b.size();
        while (carry != 0) {
            std::uint64_t const cur = static_cast<std::uint64_t>(out[k]) + carry;
            out[k] = static_cast<std::uint32_t>(cur);
            carry = cur >> 32;
            ++k;
        }
    }
    trim(out);
    return out;
}

// limbs = limbs * factor + addend, in place.
void multiply_add_small(Limbs& limbs, std::uint32_t factor, std::uint32_t addend)
{
    std::uint64_t carry = addend;
    for (std::uint32_t& limb : limbs) {
        std::uint64_t const cur = static_cast<std::uint64_t>(limb) * factor + carry;
        limb = static_cast<std::uint32_t>(cur);
        carry = cur >> 32;
    }
    if (carry != 0)
        limbs.push_back(static_cast<std::uint32_t>(carry));
    trim(limbs);
}

// limbs /= divisor, returning the remainder.
std::uint32_t divide_small(Limbs& limbs, std::uint32_t divisor)
{
    std::uint64_t remainder = 0;
    for (std::size_t i = limbs.size(); i-- > 0;) {
        std::uint64_t const cur = (remainder << 32) | limbs[i];
        limbs[i] = static_cast<std::uint32_t>(cur / divisor);
        remainder = cur % divisor;
    }
    trim(limbs);
    return static_cast<std::uint32_t>(remainder);
}

Limbs shift_magnitude_left(Limbs const& limbs, std::size_t bits)
{
    if (limbs.empty())
        return {};
    std::size_t const whole = bits / 32;
    unsigned const part = bits % 32;
    Limbs out(whole, 0);
    out.reserve(whole + limbs.size() + 1);
    std::uint32_t carry = 0;
    for (std::uint32_t const limb : limbs) {
        if (part == 0) {
            out.push_back(limb);
        } else {
            out.push_back((limb << part) | carry);
            carry = limb >> (32 - part);
        }
    }
    if (carry != 0)
        out.push_back(carry);
    trim(out);
    return out;
}

Limbs shift_magnitude_right(Limbs const& limbs, std::size_t bits)
{
    std::size_t const whole = bits / 32;
    unsigned const part = bits % 32;
    if (whole >= limbs.size())
        return {};
    Limbs out;
    out.reserve(limbs.size() - whole);
    for (std::size_t i = whole; i < limbs.size(); ++i) {
        std::uint32_t limb = limbs[i] >> part;
        if (part != 0 && i + 1 < limbs.size())
            limb |= limbs[i + 1] << (32 - part);
        out.push_back(limb);
    }
    trim(out);
    return out;
}

bool any_bit_below(Limbs const& limbs, std::size_t bits)
{
    std::size_t const whole = bits / 32;
    for (std::size_t i = 0; i < whole && i < limbs.size(); ++i) {
        if (limbs[i] != 0)
            return true;
    }
    unsigned const part = bits % 32;
    if (part != 0 && whole < limbs.size() && (limbs[whole] & ((std::uint32_t(1) << part) - 1)) != 0)
        return true;
    return false;
}

int digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return 99;
}

bool is_string_whitespace(char16_t c)
{
    return c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20 || c == 0xA0 || c == 0x1680
        || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000
        || c == 0xFEFF;
}

} // namespace

// --- construction --------------------------------------------------------

BigInteger BigInteger::make(Limbs limbs, bool negative)
{
    BigInteger out;
    trim(limbs);
    out.m_limbs = std::move(limbs);
    out.m_negative = negative && !out.m_limbs.empty();
    return out;
}

BigInteger BigInteger::from_uint64(std::uint64_t value)
{
    Limbs limbs;
    if (value != 0) {
        limbs.push_back(static_cast<std::uint32_t>(value));
        if ((value >> 32) != 0)
            limbs.push_back(static_cast<std::uint32_t>(value >> 32));
    }
    return make(std::move(limbs), false);
}

BigInteger BigInteger::from_int64(std::int64_t value)
{
    bool const negative = value < 0;
    std::uint64_t const magnitude = negative ? std::uint64_t(0) - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    BigInteger out = from_uint64(magnitude);
    out.m_negative = negative && !out.m_limbs.empty();
    return out;
}

std::optional<BigInteger> BigInteger::from_double(double value)
{
    if (!std::isfinite(value) || std::trunc(value) != value)
        return std::nullopt;
    bool const negative = value < 0;
    double const magnitude = std::fabs(value);
    if (magnitude == 0)
        return BigInteger();
    if (magnitude < 18446744073709551616.0) {
        BigInteger out = from_uint64(static_cast<std::uint64_t>(magnitude));
        out.m_negative = negative;
        return out;
    }
    // The 53-bit significand, then the binary exponent it sits under.
    int exponent = 0;
    double const fraction = std::frexp(magnitude, &exponent); // magnitude = fraction * 2^exponent, fraction in [0.5, 1)
    auto const significand = static_cast<std::uint64_t>(std::ldexp(fraction, 53));
    BigInteger out = from_uint64(significand);
    out.m_limbs = shift_magnitude_left(out.m_limbs, static_cast<std::size_t>(exponent - 53));
    out.m_negative = negative;
    return out;
}

std::optional<BigInteger> BigInteger::parse_digits(std::string_view digits, int radix)
{
    if (digits.empty() || radix < 2 || radix > 36)
        return std::nullopt;
    Limbs limbs;
    for (char const c : digits) {
        int const value = digit_value(c);
        if (value >= radix)
            return std::nullopt;
        multiply_add_small(limbs, static_cast<std::uint32_t>(radix), static_cast<std::uint32_t>(value));
    }
    return make(std::move(limbs), false);
}

std::optional<BigInteger> BigInteger::from_string(std::u16string_view text)
{
    while (!text.empty() && is_string_whitespace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && is_string_whitespace(text.back()))
        text.remove_suffix(1);
    if (text.empty())
        return BigInteger();
    std::string narrow;
    narrow.reserve(text.size());
    for (char16_t const c : text) {
        if (c > 0x7F)
            return std::nullopt;
        narrow.push_back(static_cast<char>(c));
    }
    if (narrow.size() > 2 && narrow[0] == '0') {
        char const prefix = narrow[1];
        int const radix = (prefix == 'x' || prefix == 'X') ? 16 : (prefix == 'o' || prefix == 'O') ? 8 : (prefix == 'b' || prefix == 'B') ? 2 : 0;
        if (radix != 0)
            return parse_digits(std::string_view(narrow).substr(2), radix);
    }
    bool negative = false;
    std::string_view digits = narrow;
    if (digits.front() == '+' || digits.front() == '-') {
        negative = digits.front() == '-';
        digits.remove_prefix(1);
    }
    std::optional<BigInteger> parsed = parse_digits(digits, 10);
    if (!parsed)
        return std::nullopt;
    parsed->m_negative = negative && !parsed->m_limbs.empty();
    return parsed;
}

// --- inspection ----------------------------------------------------------

std::size_t BigInteger::bit_length() const
{
    if (m_limbs.empty())
        return 0;
    std::uint32_t top = m_limbs.back();
    std::size_t bits = (m_limbs.size() - 1) * 32;
    while (top != 0) {
        ++bits;
        top >>= 1;
    }
    return bits;
}

std::string BigInteger::to_string(int radix) const
{
    if (m_limbs.empty())
        return "0";
    static constexpr char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    // The largest power of the radix in a limb, so each division yields a
    // chunk of digits.
    std::uint32_t chunk = static_cast<std::uint32_t>(radix);
    int chunk_digits = 1;
    while (static_cast<std::uint64_t>(chunk) * static_cast<std::uint64_t>(radix) < limb_base) {
        chunk *= static_cast<std::uint32_t>(radix);
        ++chunk_digits;
    }
    Limbs working = m_limbs;
    std::string out;
    while (!working.empty()) {
        std::uint32_t rest = divide_small(working, chunk);
        for (int i = 0; i < chunk_digits; ++i) {
            out.push_back(digits[rest % static_cast<std::uint32_t>(radix)]);
            rest /= static_cast<std::uint32_t>(radix);
            if (working.empty() && rest == 0)
                break;
        }
    }
    while (out.size() > 1 && out.back() == '0')
        out.pop_back();
    if (m_negative)
        out.push_back('-');
    std::reverse(out.begin(), out.end());
    return out;
}

double BigInteger::to_double() const
{
    if (m_limbs.empty())
        return 0.0;
    std::size_t const bits = bit_length();
    double magnitude;
    if (bits > 1024) {
        magnitude = std::numeric_limits<double>::infinity();
    } else if (bits <= 64) {
        std::uint64_t const low = m_limbs[0] | (m_limbs.size() > 1 ? static_cast<std::uint64_t>(m_limbs[1]) << 32 : 0);
        magnitude = static_cast<double>(low);
    } else {
        // The top 64 bits, the lowest of them made sticky for what lies
        // below, so the hardware's rounding to 53 bits breaks ties right.
        std::size_t const shift = bits - 64;
        Limbs const top = shift_magnitude_right(m_limbs, shift);
        std::uint64_t value = top[0] | (top.size() > 1 ? static_cast<std::uint64_t>(top[1]) << 32 : 0);
        if (any_bit_below(m_limbs, shift))
            value |= 1;
        magnitude = std::ldexp(static_cast<double>(value), static_cast<int>(shift));
    }
    return m_negative ? -magnitude : magnitude;
}

std::uint64_t BigInteger::to_uint64_wrapping() const
{
    std::uint64_t low = 0;
    if (!m_limbs.empty())
        low = m_limbs[0];
    if (m_limbs.size() > 1)
        low |= static_cast<std::uint64_t>(m_limbs[1]) << 32;
    return m_negative ? std::uint64_t(0) - low : low;
}

std::optional<std::int64_t> BigInteger::to_int64() const
{
    if (m_limbs.size() > 2)
        return std::nullopt;
    std::uint64_t const magnitude = m_limbs.empty() ? 0 : (m_limbs[0] | (m_limbs.size() > 1 ? static_cast<std::uint64_t>(m_limbs[1]) << 32 : 0));
    if (m_negative) {
        if (magnitude > (std::uint64_t(1) << 63))
            return std::nullopt;
        return static_cast<std::int64_t>(std::uint64_t(0) - magnitude);
    }
    if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return static_cast<std::int64_t>(magnitude);
}

std::optional<std::uint64_t> BigInteger::to_uint64() const
{
    if (m_negative || m_limbs.size() > 2)
        return std::nullopt;
    return to_uint64_wrapping();
}

std::size_t BigInteger::hash() const
{
    std::size_t h = m_negative ? 0x9e3779b97f4a7c15ull : 0;
    for (std::uint32_t const limb : m_limbs)
        h = h * 1099511628211ull ^ limb;
    return h;
}

int compare(BigInteger const& a, BigInteger const& b)
{
    if (a.m_negative != b.m_negative)
        return a.m_negative ? -1 : 1;
    int const magnitudes = compare_magnitudes(a.m_limbs, b.m_limbs);
    return a.m_negative ? -magnitudes : magnitudes;
}

int BigInteger::compare_double(double number) const
{
    double const whole = std::floor(number);
    std::optional<BigInteger> const floor_value = from_double(whole);
    if (!floor_value)
        return number > 0 ? -1 : 1; // an infinity
    int const c = compare(*this, *floor_value);
    if (c != 0)
        return c;
    return number > whole ? -1 : 0; // equal to the floor: below the number when it has a fraction
}

// --- arithmetic ----------------------------------------------------------

BigInteger BigInteger::negated() const
{
    BigInteger out = *this;
    out.m_negative = !m_negative && !m_limbs.empty();
    return out;
}

BigInteger operator+(BigInteger const& a, BigInteger const& b)
{
    if (a.m_negative == b.m_negative)
        return BigInteger::make(add_magnitudes(a.m_limbs, b.m_limbs), a.m_negative);
    int const c = compare_magnitudes(a.m_limbs, b.m_limbs);
    if (c == 0)
        return BigInteger();
    if (c > 0)
        return BigInteger::make(subtract_magnitudes(a.m_limbs, b.m_limbs), a.m_negative);
    return BigInteger::make(subtract_magnitudes(b.m_limbs, a.m_limbs), b.m_negative);
}

BigInteger operator-(BigInteger const& a, BigInteger const& b)
{
    return a + b.negated();
}

BigInteger operator*(BigInteger const& a, BigInteger const& b)
{
    return BigInteger::make(multiply_magnitudes(a.m_limbs, b.m_limbs), a.m_negative != b.m_negative);
}

// Knuth's Algorithm D over 32-bit limbs.
void BigInteger::divide_magnitudes(Limbs const& dividend, Limbs const& divisor, Limbs& quotient, Limbs& remainder)
{
    quotient.clear();
    remainder.clear();
    if (compare_magnitudes(dividend, divisor) < 0) {
        remainder = dividend;
        return;
    }
    if (divisor.size() == 1) {
        quotient = dividend;
        std::uint32_t const rest = divide_small(quotient, divisor[0]);
        if (rest != 0)
            remainder.push_back(rest);
        return;
    }
    // Normalize so the divisor's top limb has its high bit set.
    unsigned shift = 0;
    for (std::uint32_t top = divisor.back(); (top & 0x80000000u) == 0; top <<= 1)
        ++shift;
    Limbs v = shift_magnitude_left(divisor, shift);
    Limbs u = shift_magnitude_left(dividend, shift);
    u.push_back(0);
    if (u.size() < v.size() + 1)
        u.resize(v.size() + 1, 0);
    std::size_t const n = v.size();
    std::size_t const m = u.size() - n - 1;
    quotient.assign(m + 1, 0);
    std::uint64_t const vtop = v[n - 1];
    std::uint64_t const vnext = v[n - 2];
    for (std::size_t j = m + 1; j-- > 0;) {
        std::uint64_t const numerator = (static_cast<std::uint64_t>(u[j + n]) << 32) | u[j + n - 1];
        std::uint64_t qhat = numerator / vtop;
        std::uint64_t rhat = numerator % vtop;
        while (qhat >= limb_base || qhat * vnext > ((rhat << 32) | u[j + n - 2])) {
            --qhat;
            rhat += vtop;
            if (rhat >= limb_base)
                break;
        }
        // Multiply and subtract.
        std::int64_t borrow = 0;
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < n; ++i) {
            std::uint64_t const product = qhat * v[i] + carry;
            carry = product >> 32;
            std::int64_t const diff = static_cast<std::int64_t>(u[i + j]) - static_cast<std::int64_t>(product & 0xFFFFFFFFu) - borrow;
            u[i + j] = static_cast<std::uint32_t>(diff);
            borrow = diff < 0 ? 1 : 0;
        }
        std::int64_t const diff = static_cast<std::int64_t>(u[j + n]) - static_cast<std::int64_t>(carry) - borrow;
        u[j + n] = static_cast<std::uint32_t>(diff);
        if (diff < 0) {
            // Too big by one: add the divisor back.
            --qhat;
            std::uint64_t add_carry = 0;
            for (std::size_t i = 0; i < n; ++i) {
                std::uint64_t const sum = static_cast<std::uint64_t>(u[i + j]) + v[i] + add_carry;
                u[i + j] = static_cast<std::uint32_t>(sum);
                add_carry = sum >> 32;
            }
            u[j + n] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(u[j + n]) + add_carry);
        }
        quotient[j] = static_cast<std::uint32_t>(qhat);
    }
    trim(quotient);
    u.resize(n);
    remainder = shift_magnitude_right(u, shift);
}

std::optional<BigInteger> BigInteger::divide(BigInteger const& dividend, BigInteger const& divisor)
{
    if (divisor.is_zero())
        return std::nullopt;
    Limbs quotient;
    Limbs remainder;
    divide_magnitudes(dividend.m_limbs, divisor.m_limbs, quotient, remainder);
    return make(std::move(quotient), dividend.m_negative != divisor.m_negative);
}

std::optional<BigInteger> BigInteger::remainder(BigInteger const& dividend, BigInteger const& divisor)
{
    if (divisor.is_zero())
        return std::nullopt;
    Limbs quotient;
    Limbs rest;
    divide_magnitudes(dividend.m_limbs, divisor.m_limbs, quotient, rest);
    return make(std::move(rest), dividend.m_negative); // the sign of the dividend
}

BigInteger::Power BigInteger::power(BigInteger const& base, BigInteger const& exponent)
{
    Power result;
    if (exponent.is_negative()) {
        result.negative_exponent = true;
        return result;
    }
    if (exponent.is_zero()) {
        result.value = from_int64(1);
        return result;
    }
    if (base.is_zero() || (base.m_limbs.size() == 1 && base.m_limbs[0] == 1)) {
        // 0, 1 and -1 to any power: -1 flips with the exponent's parity.
        result.value = base.m_negative && !exponent.is_odd() ? base.negated() : base;
        return result;
    }
    std::optional<std::uint64_t> const count = exponent.to_uint64();
    if (!count || *count > (std::uint64_t(1) << 26)) {
        result.too_large = true;
        return result;
    }
    // Square and multiply, refusing a result past the limit before it is made.
    if (base.bit_length() * *count > max_limbs * 32) {
        result.too_large = true;
        return result;
    }
    BigInteger acc = from_int64(1);
    BigInteger square = base;
    std::uint64_t remaining = *count;
    while (remaining != 0) {
        if ((remaining & 1) != 0)
            acc = acc * square;
        remaining >>= 1;
        if (remaining != 0)
            square = square * square;
        if (acc.m_limbs.size() > max_limbs || square.m_limbs.size() > max_limbs) {
            result.too_large = true;
            return result;
        }
    }
    result.value = std::move(acc);
    return result;
}

std::optional<BigInteger> BigInteger::shift_left(BigInteger const& value, BigInteger const& count)
{
    if (count.is_negative())
        return shift_right(value, count.negated());
    if (value.is_zero())
        return BigInteger();
    std::optional<std::uint64_t> const bits = count.to_uint64();
    if (!bits || *bits / 32 + value.m_limbs.size() > max_limbs)
        return std::nullopt;
    return make(shift_magnitude_left(value.m_limbs, static_cast<std::size_t>(*bits)), value.m_negative);
}

std::optional<BigInteger> BigInteger::shift_right(BigInteger const& value, BigInteger const& count)
{
    if (count.is_negative())
        return shift_left(value, count.negated());
    if (value.is_zero())
        return BigInteger();
    std::optional<std::uint64_t> const bits = count.to_uint64();
    if (!bits || *bits >= value.bit_length())
        return value.m_negative ? from_int64(-1) : BigInteger();
    if (!value.m_negative)
        return make(shift_magnitude_right(value.m_limbs, static_cast<std::size_t>(*bits)), false);
    // Floor for a negative value: -((|x| - 1) >> n) - 1.
    Limbs const less = subtract_magnitudes(value.m_limbs, Limbs { 1 });
    BigInteger shifted = make(shift_magnitude_right(less, static_cast<std::size_t>(*bits)), false);
    return (shifted + from_int64(1)).negated();
}

// --- bitwise, over the two's complement of infinite width ------------------

BigInteger::Limbs BigInteger::twos_complement(BigInteger const& value, std::size_t width)
{
    Limbs out = value.m_limbs;
    out.resize(width, 0);
    if (!value.m_negative)
        return out;
    std::uint64_t carry = 1;
    for (std::uint32_t& limb : out) {
        std::uint64_t const sum = static_cast<std::uint64_t>(~limb) + carry;
        limb = static_cast<std::uint32_t>(sum);
        carry = sum >> 32;
    }
    return out;
}

BigInteger BigInteger::from_twos_complement(Limbs limbs)
{
    bool const negative = !limbs.empty() && (limbs.back() & 0x80000000u) != 0;
    if (negative) {
        std::uint64_t carry = 1;
        for (std::uint32_t& limb : limbs) {
            std::uint64_t const sum = static_cast<std::uint64_t>(~limb) + carry;
            limb = static_cast<std::uint32_t>(sum);
            carry = sum >> 32;
        }
    }
    return make(std::move(limbs), negative);
}

BigInteger operator&(BigInteger const& a, BigInteger const& b)
{
    std::size_t const width = std::max(a.m_limbs.size(), b.m_limbs.size()) + 1;
    BigInteger::Limbs x = BigInteger::twos_complement(a, width);
    BigInteger::Limbs const y = BigInteger::twos_complement(b, width);
    for (std::size_t i = 0; i < width; ++i)
        x[i] &= y[i];
    return BigInteger::from_twos_complement(std::move(x));
}

BigInteger operator|(BigInteger const& a, BigInteger const& b)
{
    std::size_t const width = std::max(a.m_limbs.size(), b.m_limbs.size()) + 1;
    BigInteger::Limbs x = BigInteger::twos_complement(a, width);
    BigInteger::Limbs const y = BigInteger::twos_complement(b, width);
    for (std::size_t i = 0; i < width; ++i)
        x[i] |= y[i];
    return BigInteger::from_twos_complement(std::move(x));
}

BigInteger operator^(BigInteger const& a, BigInteger const& b)
{
    std::size_t const width = std::max(a.m_limbs.size(), b.m_limbs.size()) + 1;
    BigInteger::Limbs x = BigInteger::twos_complement(a, width);
    BigInteger::Limbs const y = BigInteger::twos_complement(b, width);
    for (std::size_t i = 0; i < width; ++i)
        x[i] ^= y[i];
    return BigInteger::from_twos_complement(std::move(x));
}

BigInteger BigInteger::bitwise_not() const
{
    return (*this + from_int64(1)).negated();
}

BigInteger BigInteger::as_uint_n(std::size_t bits, BigInteger const& value)
{
    if (bits == 0 || value.is_zero())
        return BigInteger();
    // The magnitude modulo 2^bits, then the complement for a negative value.
    Limbs modulo = value.m_limbs;
    std::size_t const whole = bits / 32;
    unsigned const part = bits % 32;
    if (modulo.size() > whole + (part != 0 ? 1 : 0))
        modulo.resize(whole + (part != 0 ? 1 : 0));
    if (part != 0 && modulo.size() == whole + 1)
        modulo[whole] &= (std::uint32_t(1) << part) - 1;
    trim(modulo);
    if (!value.m_negative || modulo.empty())
        return make(std::move(modulo), false);
    // 2^bits - modulo.
    Limbs power(whole + 1, 0);
    power[whole] = std::uint32_t(1) << part;
    if (part == 0) {
        power.resize(whole + 1, 0);
        power[whole] = 1;
    }
    return make(subtract_magnitudes(power, modulo), false);
}

BigInteger BigInteger::as_int_n(std::size_t bits, BigInteger const& value)
{
    if (bits == 0)
        return BigInteger();
    BigInteger unsigned_value = as_uint_n(bits, value);
    if (unsigned_value.bit_length() == bits) {
        // The top bit set: the value is negative, 2^bits below.
        Limbs power((bits - 1) / 32 + 1, 0);
        std::size_t const whole = bits / 32;
        unsigned const part = bits % 32;
        power.resize(whole + 1, 0);
        power[whole] = part == 0 ? 1 : (std::uint32_t(1) << part);
        return make(subtract_magnitudes(power, unsigned_value.m_limbs), true);
    }
    return unsigned_value;
}

}
