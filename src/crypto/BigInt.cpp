#include "crypto/BigInt.h"

#include <algorithm>
#include <cstdio>

namespace sashfold::crypto {

__extension__ typedef unsigned __int128 u128;

BigInt BigInt::from_u64(std::uint64_t value)
{
    BigInt result;
    result.m_limbs[0] = value;
    return result;
}

std::optional<BigInt> BigInt::from_bytes(std::span<std::uint8_t const> bytes)
{
    std::size_t start = 0;
    while (start < bytes.size() && bytes[start] == 0)
        ++start;
    std::size_t const significant = bytes.size() - start;
    if (significant > limb_count * 8)
        return std::nullopt;
    BigInt result;
    for (std::size_t i = 0; i < significant; ++i) {
        std::uint8_t const byte = bytes[bytes.size() - 1 - i];
        result.m_limbs[i / 8] |= std::uint64_t(byte) << (8 * (i % 8));
    }
    return result;
}

std::optional<BigInt> BigInt::from_hex(std::string_view hex)
{
    std::vector<std::uint8_t> bytes;
    std::string digits;
    for (char const c : hex) {
        if (c == ' ' || c == ':' || c == '\n')
            continue;
        digits += c;
    }
    if (digits.size() % 2 == 1)
        digits.insert(digits.begin(), '0');
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        int const high = nibble(digits[i]);
        int const low = nibble(digits[i + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return from_bytes(bytes);
}

std::size_t BigInt::used() const
{
    std::size_t n = limb_count;
    while (n > 0 && m_limbs[n - 1] == 0)
        --n;
    return n;
}

std::size_t BigInt::bit_length() const
{
    std::size_t const n = used();
    if (n == 0)
        return 0;
    std::uint64_t top = m_limbs[n - 1];
    std::size_t bits = 0;
    while (top != 0) {
        ++bits;
        top >>= 1;
    }
    return (n - 1) * 64 + bits;
}

bool BigInt::is_zero() const
{
    return used() == 0;
}

bool BigInt::bit(std::size_t index) const
{
    if (index >= max_bits)
        return false;
    return ((m_limbs[index / 64] >> (index % 64)) & 1) != 0;
}

std::optional<std::vector<std::uint8_t>> BigInt::to_bytes(std::size_t size) const
{
    if (bit_length() > size * 8)
        return std::nullopt;
    std::vector<std::uint8_t> out(size, 0);
    for (std::size_t i = 0; i < size && i < limb_count * 8; ++i)
        out[size - 1 - i] = static_cast<std::uint8_t>(m_limbs[i / 8] >> (8 * (i % 8)));
    return out;
}

std::vector<std::uint8_t> BigInt::to_bytes() const
{
    std::size_t const size = (bit_length() + 7) / 8;
    return *to_bytes(size);
}

std::string BigInt::to_hex() const
{
    std::string out;
    char buffer[3];
    for (std::uint8_t const byte : to_bytes()) {
        std::snprintf(buffer, sizeof buffer, "%02x", byte);
        out += buffer;
    }
    return out.empty() ? "0" : out;
}

int BigInt::compare(BigInt const& other) const
{
    for (std::size_t i = limb_count; i-- > 0;) {
        if (m_limbs[i] != other.m_limbs[i])
            return m_limbs[i] < other.m_limbs[i] ? -1 : 1;
    }
    return 0;
}

BigInt BigInt::add(BigInt const& other) const
{
    BigInt result;
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < limb_count; ++i) {
        u128 const sum = u128(m_limbs[i]) + other.m_limbs[i] + carry;
        result.m_limbs[i] = std::uint64_t(sum);
        carry = std::uint64_t(sum >> 64);
    }
    return result;
}

BigInt BigInt::sub(BigInt const& other) const
{
    BigInt result;
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < limb_count; ++i) {
        u128 const a = m_limbs[i];
        u128 const b = u128(other.m_limbs[i]) + borrow;
        if (a >= b) {
            result.m_limbs[i] = std::uint64_t(a - b);
            borrow = 0;
        } else {
            result.m_limbs[i] = std::uint64_t((u128(1) << 64) + a - b);
            borrow = 1;
        }
    }
    return result;
}

BigInt BigInt::mul(BigInt const& other) const
{
    BigInt result;
    std::size_t const n = used();
    std::size_t const m = other.used();
    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < m && i + j < limb_count; ++j) {
            u128 const product = u128(m_limbs[i]) * other.m_limbs[j] + result.m_limbs[i + j] + carry;
            result.m_limbs[i + j] = std::uint64_t(product);
            carry = std::uint64_t(product >> 64);
        }
        for (std::size_t k = i + m; carry != 0 && k < limb_count; ++k) {
            u128 const sum = u128(result.m_limbs[k]) + carry;
            result.m_limbs[k] = std::uint64_t(sum);
            carry = std::uint64_t(sum >> 64);
        }
    }
    return result;
}

BigInt BigInt::shift_left(std::size_t bits) const
{
    BigInt result;
    std::size_t const limbs = bits / 64;
    std::size_t const rest = bits % 64;
    for (std::size_t i = limb_count; i-- > limbs;) {
        std::uint64_t value = m_limbs[i - limbs] << rest;
        if (rest != 0 && i - limbs > 0)
            value |= m_limbs[i - limbs - 1] >> (64 - rest);
        result.m_limbs[i] = value;
    }
    return result;
}

BigInt BigInt::shift_right(std::size_t bits) const
{
    BigInt result;
    std::size_t const limbs = bits / 64;
    std::size_t const rest = bits % 64;
    for (std::size_t i = 0; i + limbs < limb_count; ++i) {
        std::uint64_t value = m_limbs[i + limbs] >> rest;
        if (rest != 0 && i + limbs + 1 < limb_count)
            value |= m_limbs[i + limbs + 1] << (64 - rest);
        result.m_limbs[i] = value;
    }
    return result;
}

// Binary long division: the divisor is aligned under the dividend's top
// bit and subtracted wherever it fits while it walks down. Bounded by
// the dividend's bit length times the divisor's limbs.
void BigInt::divmod(BigInt const& divisor, BigInt& quotient, BigInt& remainder) const
{
    quotient = BigInt();
    remainder = *this;
    if (divisor.is_zero())
        return;
    std::size_t const dividend_bits = bit_length();
    std::size_t const divisor_bits = divisor.bit_length();
    if (dividend_bits < divisor_bits)
        return;
    std::size_t shift = dividend_bits - divisor_bits;
    BigInt shifted = divisor.shift_left(shift);
    for (;;) {
        if (remainder.compare(shifted) >= 0) {
            remainder = remainder.sub(shifted);
            quotient.m_limbs[shift / 64] |= std::uint64_t(1) << (shift % 64);
        }
        if (shift == 0)
            break;
        --shift;
        shifted = shifted.shift_right(1);
    }
}

BigInt BigInt::mod(BigInt const& m) const
{
    BigInt quotient;
    BigInt remainder;
    divmod(m, quotient, remainder);
    return remainder;
}

BigInt BigInt::mod_add(BigInt const& other, BigInt const& m) const
{
    BigInt sum = add(other);
    if (sum.compare(m) >= 0)
        sum = sum.sub(m);
    return sum;
}

BigInt BigInt::mod_sub(BigInt const& other, BigInt const& m) const
{
    if (compare(other) >= 0)
        return sub(other);
    return add(m).sub(other);
}

BigInt BigInt::mod_mul(BigInt const& other, BigInt const& m) const
{
    return mul(other).mod(m);
}

BigInt BigInt::mod_pow(BigInt const& exponent, BigInt const& m) const
{
    BigInt result = from_u64(1).mod(m);
    BigInt base = mod(m);
    std::size_t const bits = exponent.bit_length();
    for (std::size_t i = bits; i-- > 0;) {
        result = result.mod_mul(result, m);
        if (exponent.bit(i))
            result = result.mod_mul(base, m);
    }
    return result;
}

BigInt BigInt::mod_inverse_prime(BigInt const& p) const
{
    return mod_pow(p.sub(from_u64(2)), p);
}

}
