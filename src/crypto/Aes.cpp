#include "crypto/Aes.h"

namespace sashfold::crypto {

namespace {

// ---- GF(2^8) on bit planes, modulo x^8 + x^4 + x^3 + x + 1 (§4.2)
//
// Every operation below works on all sixteen bytes of the state at once:
// one plane holds one bit of each byte, so an AND is sixteen parallel ANDs
// and the circuit's shape never depends on a byte's value.

// x^8 ≡ x^4 + x^3 + x + 1, so a term of degree k ≥ 8 folds into the terms
// of degree k−4, k−5, k−7 and k−8, all of which are below k: one pass from
// the top reduces the whole product.
AesPlanes gf_reduce(std::uint16_t (&t)[15])
{
    for (int k = 14; k >= 8; --k) {
        std::uint16_t const c = t[k];
        t[k - 4] = static_cast<std::uint16_t>(t[k - 4] ^ c);
        t[k - 5] = static_cast<std::uint16_t>(t[k - 5] ^ c);
        t[k - 7] = static_cast<std::uint16_t>(t[k - 7] ^ c);
        t[k - 8] = static_cast<std::uint16_t>(t[k - 8] ^ c);
    }
    AesPlanes out {};
    for (std::size_t j = 0; j < 8; ++j)
        out[j] = t[j];
    return out;
}

AesPlanes gf_mul(AesPlanes const& a, AesPlanes const& b)
{
    std::uint16_t t[15] = {};
    for (std::size_t i = 0; i < 8; ++i)
        for (std::size_t j = 0; j < 8; ++j)
            t[i + j] = static_cast<std::uint16_t>(t[i + j] ^ (a[i] & b[j]));
    return gf_reduce(t);
}

// Squaring is linear over GF(2): (Σ a_j x^j)^2 = Σ a_j x^(2j).
AesPlanes gf_square(AesPlanes const& a)
{
    std::uint16_t t[15] = {};
    for (std::size_t j = 0; j < 8; ++j)
        t[2 * j] = a[j];
    return gf_reduce(t);
}

// The multiplicative inverse as x^254, which is 0 for 0 — exactly the
// convention §5.1.1 gives the S-box. The chain costs four multiplications
// and seven squarings: x^2, x^3, x^12, x^14, x^15, x^240, x^254.
AesPlanes gf_inverse(AesPlanes const& x)
{
    AesPlanes const x2 = gf_square(x);
    AesPlanes const x3 = gf_mul(x2, x);
    AesPlanes const x12 = gf_square(gf_square(x3));
    AesPlanes const x14 = gf_mul(x12, x2);
    AesPlanes const x15 = gf_mul(x12, x3);
    AesPlanes const x240 = gf_square(gf_square(gf_square(gf_square(x15))));
    return gf_mul(x240, x14);
}

// ---- the round functions (§5.1)

// SubBytes: the inverse, then b_i = a_i ⊕ a_(i+4) ⊕ a_(i+5) ⊕ a_(i+6) ⊕
// a_(i+7) ⊕ c_i with c = 0x63, the affine transformation of §5.1.1.
AesPlanes sub_bytes(AesPlanes const& state)
{
    AesPlanes const a = gf_inverse(state);
    AesPlanes out {};
    for (std::size_t i = 0; i < 8; ++i) {
        std::uint16_t bit = static_cast<std::uint16_t>(a[i] ^ a[(i + 4) % 8] ^ a[(i + 5) % 8] ^ a[(i + 6) % 8] ^ a[(i + 7) % 8]);
        if (((0x63u >> i) & 1u) != 0u)
            bit = static_cast<std::uint16_t>(~bit);
        out[i] = bit;
    }
    return out;
}

std::uint16_t rotate_right(std::uint16_t value, unsigned count)
{
    return static_cast<std::uint16_t>((value >> count) | (value << (16 - count)));
}

// ShiftRows: state byte r + 4c moves to r + 4((c − r) mod 4), so within
// each row — the bit positions r, r+4, r+8, r+12 of a plane — the bits
// rotate by 4r places. Row 0 stands still.
AesPlanes shift_rows(AesPlanes const& state)
{
    AesPlanes out {};
    for (std::size_t j = 0; j < 8; ++j) {
        std::uint16_t const w = state[j];
        std::uint16_t result = static_cast<std::uint16_t>(w & 0x1111u);
        for (unsigned row = 1; row < 4; ++row) {
            std::uint16_t const mask = static_cast<std::uint16_t>(0x1111u << row);
            result = static_cast<std::uint16_t>(result | rotate_right(static_cast<std::uint16_t>(w & mask), 4 * row));
        }
        out[j] = result;
    }
    return out;
}

// A column is one nibble of a plane (byte r + 4c, so rows are adjacent
// bits); this brings row r+1 to row r inside every column.
std::uint16_t rotate_rows(std::uint16_t w)
{
    return static_cast<std::uint16_t>(((w >> 1) & 0x7777u) | ((w & 0x1111u) << 3));
}

// Multiplication by x: the top plane wraps into planes 0, 1, 3 and 4,
// which is the 0x1b of the reduction polynomial.
AesPlanes xtime(AesPlanes const& a)
{
    AesPlanes out {};
    out[0] = a[7];
    out[1] = static_cast<std::uint16_t>(a[0] ^ a[7]);
    out[2] = a[1];
    out[3] = static_cast<std::uint16_t>(a[2] ^ a[7]);
    out[4] = static_cast<std::uint16_t>(a[3] ^ a[7]);
    out[5] = a[4];
    out[6] = a[5];
    out[7] = a[6];
    return out;
}

// MixColumns: s'_r = 2·s_r ⊕ 3·s_(r+1) ⊕ s_(r+2) ⊕ s_(r+3), which is
// 2·(s_r ⊕ s_(r+1)) ⊕ (s_0 ⊕ s_1 ⊕ s_2 ⊕ s_3) ⊕ s_r — one xtime and two
// XORs per plane, with the rotations done inside the column.
AesPlanes mix_columns(AesPlanes const& state)
{
    AesPlanes up1 {};
    AesPlanes up2 {};
    AesPlanes up3 {};
    AesPlanes column_sum {};
    AesPlanes pair {};
    for (std::size_t j = 0; j < 8; ++j) {
        up1[j] = rotate_rows(state[j]);
        up2[j] = rotate_rows(up1[j]);
        up3[j] = rotate_rows(up2[j]);
        column_sum[j] = static_cast<std::uint16_t>(state[j] ^ up1[j] ^ up2[j] ^ up3[j]);
        pair[j] = static_cast<std::uint16_t>(state[j] ^ up1[j]);
    }
    AesPlanes const doubled = xtime(pair);
    AesPlanes out {};
    for (std::size_t j = 0; j < 8; ++j)
        out[j] = static_cast<std::uint16_t>(doubled[j] ^ column_sum[j] ^ state[j]);
    return out;
}

AesPlanes add_round_key(AesPlanes const& state, AesPlanes const& key)
{
    AesPlanes out {};
    for (std::size_t j = 0; j < 8; ++j)
        out[j] = static_cast<std::uint16_t>(state[j] ^ key[j]);
    return out;
}

// ---- bytes to planes and back

AesPlanes to_planes(std::uint8_t const* bytes, std::size_t count)
{
    AesPlanes out {};
    for (std::size_t i = 0; i < count; ++i)
        for (std::size_t j = 0; j < 8; ++j)
            out[j] = static_cast<std::uint16_t>(out[j] | static_cast<std::uint16_t>(((bytes[i] >> j) & 1u) << i));
    return out;
}

void from_planes(AesPlanes const& planes, std::uint8_t* bytes, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        unsigned value = 0;
        for (std::size_t j = 0; j < 8; ++j)
            value |= ((planes[j] >> i) & 1u) << j;
        bytes[i] = static_cast<std::uint8_t>(value);
    }
}

// SubWord of the key schedule, through the same circuit as SubBytes.
void sub_word(std::uint8_t* bytes)
{
    AesPlanes const planes = sub_bytes(to_planes(bytes, 4));
    from_planes(planes, bytes, 4);
}

}

// §5.2: the key schedule, expanded once per key. The round constants are
// the powers of x in GF(2^8).
Aes128::Aes128(AesKey const& key)
{
    constexpr std::uint8_t rcon[10] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36 };
    std::uint8_t words[176] = {};
    for (std::size_t i = 0; i < key.size(); ++i)
        words[i] = key[i];
    for (std::size_t word = 4; word < 44; ++word) {
        std::uint8_t temp[4] = {
            words[4 * (word - 1) + 0],
            words[4 * (word - 1) + 1],
            words[4 * (word - 1) + 2],
            words[4 * (word - 1) + 3],
        };
        if (word % 4 == 0) {
            std::uint8_t const first = temp[0];
            temp[0] = temp[1];
            temp[1] = temp[2];
            temp[2] = temp[3];
            temp[3] = first;
            sub_word(temp);
            temp[0] = static_cast<std::uint8_t>(temp[0] ^ rcon[word / 4 - 1]);
        }
        for (std::size_t j = 0; j < 4; ++j)
            words[4 * word + j] = static_cast<std::uint8_t>(words[4 * (word - 4) + j] ^ temp[j]);
    }
    for (std::size_t round = 0; round < 11; ++round)
        m_round_keys[round] = to_planes(words + 16 * round, 16);
}

AesBlock Aes128::encrypt(AesBlock const& block) const
{
    AesPlanes state = add_round_key(to_planes(block.data(), block.size()), m_round_keys[0]);
    for (std::size_t round = 1; round < 10; ++round)
        state = add_round_key(mix_columns(shift_rows(sub_bytes(state))), m_round_keys[round]);
    state = add_round_key(shift_rows(sub_bytes(state)), m_round_keys[10]);
    AesBlock out {};
    from_planes(state, out.data(), out.size());
    return out;
}

}
