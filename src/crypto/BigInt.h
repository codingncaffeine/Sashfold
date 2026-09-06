#pragma once
// A fixed-capacity unsigned integer of up to 8192 bits, for RSA and the
// field arithmetic of the ECDSA curves: comparison, add, sub, mul,
// division, modular exponentiation and inversion, big-endian bytes both
// ways. Every value it meets is public (a modulus, a signature, a point),
// so nothing here is constant time, and callers must not hand it a
// secret. Schoolbook arithmetic bounded by the limbs in use: an RSA
// verification costs milliseconds, which a page load never notices.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::crypto {

class BigInt {
public:
    static constexpr std::size_t limb_count = 128; // 64-bit limbs: 8192 bits
    static constexpr std::size_t max_bits = limb_count * 64;

    BigInt() = default;
    static BigInt from_u64(std::uint64_t value);
    // Big-endian bytes, leading zeros allowed; nullopt past 8192 bits.
    static std::optional<BigInt> from_bytes(std::span<std::uint8_t const> bytes);
    static std::optional<BigInt> from_hex(std::string_view hex);

    // Big-endian, exactly `size` bytes, zero-padded on the left; nullopt
    // when the value does not fit.
    std::optional<std::vector<std::uint8_t>> to_bytes(std::size_t size) const;
    // The shortest big-endian form; empty for zero.
    std::vector<std::uint8_t> to_bytes() const;
    std::string to_hex() const;

    std::size_t bit_length() const;
    bool is_zero() const;
    bool is_odd() const { return (m_limbs[0] & 1) != 0; }
    bool bit(std::size_t index) const;
    int compare(BigInt const& other) const; // <0, 0, >0
    bool operator==(BigInt const& other) const { return compare(other) == 0; }
    bool operator<(BigInt const& other) const { return compare(other) < 0; }
    bool operator>=(BigInt const& other) const { return compare(other) >= 0; }

    // Truncated to 8192 bits when a result would exceed them; callers keep
    // their operands within what the reductions below need.
    BigInt add(BigInt const& other) const;
    // Requires *this >= other.
    BigInt sub(BigInt const& other) const;
    BigInt mul(BigInt const& other) const;
    BigInt shift_left(std::size_t bits) const;
    BigInt shift_right(std::size_t bits) const;

    // Long division; the divisor must not be zero.
    void divmod(BigInt const& divisor, BigInt& quotient, BigInt& remainder) const;
    BigInt mod(BigInt const& m) const;
    BigInt mod_add(BigInt const& other, BigInt const& m) const;
    BigInt mod_sub(BigInt const& other, BigInt const& m) const;
    BigInt mod_mul(BigInt const& other, BigInt const& m) const;
    BigInt mod_pow(BigInt const& exponent, BigInt const& m) const;
    // The inverse modulo a prime p, by Fermat: a^(p−2) mod p.
    BigInt mod_inverse_prime(BigInt const& p) const;

private:
    std::size_t used() const; // limbs above the highest nonzero one are ignored
    std::array<std::uint64_t, limb_count> m_limbs {};
};

}
