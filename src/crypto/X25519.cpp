#include "crypto/X25519.h"

#include <cstring>

namespace sashfold::crypto {

namespace {

// A field element modulo p = 2^255 − 19: five limbs of 51 bits, carried
// after every multiplication so that a limb never exceeds 2^52 between
// operations (add leaves limbs below 2^52, subtract below 2^53, both
// safe inputs to a multiplication in 128-bit arithmetic).
struct Fe {
    std::uint64_t v[5];
};

constexpr std::uint64_t mask51 = (std::uint64_t(1) << 51) - 1;

std::uint64_t load64_le(std::uint8_t const* p)
{
    std::uint64_t r = 0;
    for (int i = 7; i >= 0; --i)
        r = (r << 8) | p[i];
    return r;
}

void store64_le(std::uint8_t* p, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = std::uint8_t(v >> (8 * i));
}

// The 255-bit little-endian encoding; the top bit is masked off, as §5
// requires of u-coordinates.
Fe fe_frombytes(std::uint8_t const s[32])
{
    Fe h;
    h.v[0] = load64_le(s) & mask51;
    h.v[1] = (load64_le(s + 6) >> 3) & mask51;
    h.v[2] = (load64_le(s + 12) >> 6) & mask51;
    h.v[3] = (load64_le(s + 19) >> 1) & mask51;
    h.v[4] = (load64_le(s + 24) >> 12) & mask51;
    return h;
}

void fe_carry(Fe& h)
{
    h.v[1] += h.v[0] >> 51;
    h.v[0] &= mask51;
    h.v[2] += h.v[1] >> 51;
    h.v[1] &= mask51;
    h.v[3] += h.v[2] >> 51;
    h.v[2] &= mask51;
    h.v[4] += h.v[3] >> 51;
    h.v[3] &= mask51;
    h.v[0] += 19 * (h.v[4] >> 51);
    h.v[4] &= mask51;
}

// The canonical 32 bytes: fully reduced, p subtracted once when needed.
void fe_tobytes(std::uint8_t s[32], Fe f)
{
    fe_carry(f);
    fe_carry(f);
    // f < 2^255 now; subtract p if f ≥ p, by computing the carry out of
    // f + 19 through every limb.
    std::uint64_t q = (f.v[0] + 19) >> 51;
    q = (f.v[1] + q) >> 51;
    q = (f.v[2] + q) >> 51;
    q = (f.v[3] + q) >> 51;
    q = (f.v[4] + q) >> 51;
    f.v[0] += 19 * q;
    f.v[1] += f.v[0] >> 51;
    f.v[0] &= mask51;
    f.v[2] += f.v[1] >> 51;
    f.v[1] &= mask51;
    f.v[3] += f.v[2] >> 51;
    f.v[2] &= mask51;
    f.v[4] += f.v[3] >> 51;
    f.v[3] &= mask51;
    f.v[4] &= mask51;
    store64_le(s, f.v[0] | (f.v[1] << 51));
    store64_le(s + 8, (f.v[1] >> 13) | (f.v[2] << 38));
    store64_le(s + 16, (f.v[2] >> 26) | (f.v[3] << 25));
    store64_le(s + 24, (f.v[3] >> 39) | (f.v[4] << 12));
}

Fe fe_add(Fe const& f, Fe const& g)
{
    Fe h;
    for (int i = 0; i < 5; ++i)
        h.v[i] = f.v[i] + g.v[i];
    return h;
}

// f − g, computed as f + 2p − g so that no limb goes negative.
Fe fe_sub(Fe const& f, Fe const& g)
{
    Fe h;
    h.v[0] = f.v[0] + 0xfffffffffffda - g.v[0];
    for (int i = 1; i < 5; ++i)
        h.v[i] = f.v[i] + 0xffffffffffffe - g.v[i];
    return h;
}

Fe fe_mul(Fe const& f, Fe const& g)
{
    __extension__ typedef unsigned __int128 u128;
    std::uint64_t const f0 = f.v[0], f1 = f.v[1], f2 = f.v[2], f3 = f.v[3], f4 = f.v[4];
    std::uint64_t const g0 = g.v[0], g1 = g.v[1], g2 = g.v[2], g3 = g.v[3], g4 = g.v[4];
    std::uint64_t const g1_19 = 19 * g1, g2_19 = 19 * g2, g3_19 = 19 * g3, g4_19 = 19 * g4;
    u128 r0 = u128(f0) * g0 + u128(f1) * g4_19 + u128(f2) * g3_19 + u128(f3) * g2_19 + u128(f4) * g1_19;
    u128 r1 = u128(f0) * g1 + u128(f1) * g0 + u128(f2) * g4_19 + u128(f3) * g3_19 + u128(f4) * g2_19;
    u128 r2 = u128(f0) * g2 + u128(f1) * g1 + u128(f2) * g0 + u128(f3) * g4_19 + u128(f4) * g3_19;
    u128 r3 = u128(f0) * g3 + u128(f1) * g2 + u128(f2) * g1 + u128(f3) * g0 + u128(f4) * g4_19;
    u128 r4 = u128(f0) * g4 + u128(f1) * g3 + u128(f2) * g2 + u128(f3) * g1 + u128(f4) * g0;
    Fe h;
    std::uint64_t c = std::uint64_t(r0 >> 51);
    h.v[0] = std::uint64_t(r0) & mask51;
    r1 += c;
    c = std::uint64_t(r1 >> 51);
    h.v[1] = std::uint64_t(r1) & mask51;
    r2 += c;
    c = std::uint64_t(r2 >> 51);
    h.v[2] = std::uint64_t(r2) & mask51;
    r3 += c;
    c = std::uint64_t(r3 >> 51);
    h.v[3] = std::uint64_t(r3) & mask51;
    r4 += c;
    c = std::uint64_t(r4 >> 51);
    h.v[4] = std::uint64_t(r4) & mask51;
    h.v[0] += 19 * c;
    h.v[1] += h.v[0] >> 51;
    h.v[0] &= mask51;
    return h;
}

Fe fe_sq(Fe const& f)
{
    return fe_mul(f, f);
}

Fe fe_sq_times(Fe f, int times)
{
    for (int i = 0; i < times; ++i)
        f = fe_sq(f);
    return f;
}

// z^(p − 2) by the fixed addition chain every implementation uses.
Fe fe_invert(Fe const& z)
{
    Fe const z2 = fe_sq(z);
    Fe const z9 = fe_mul(fe_sq_times(z2, 2), z);
    Fe const z11 = fe_mul(z9, z2);
    Fe const z2_5_0 = fe_mul(fe_sq(z11), z9);
    Fe const z2_10_0 = fe_mul(fe_sq_times(z2_5_0, 5), z2_5_0);
    Fe const z2_20_0 = fe_mul(fe_sq_times(z2_10_0, 10), z2_10_0);
    Fe const z2_40_0 = fe_mul(fe_sq_times(z2_20_0, 20), z2_20_0);
    Fe const z2_50_0 = fe_mul(fe_sq_times(z2_40_0, 10), z2_10_0);
    Fe const z2_100_0 = fe_mul(fe_sq_times(z2_50_0, 50), z2_50_0);
    Fe const z2_200_0 = fe_mul(fe_sq_times(z2_100_0, 100), z2_100_0);
    Fe const z2_250_0 = fe_mul(fe_sq_times(z2_200_0, 50), z2_50_0);
    return fe_mul(fe_sq_times(z2_250_0, 5), z11);
}

// Swaps f and g when swap is 1, leaves them when 0, touching the same
// memory in the same way either time.
void fe_cswap(Fe& f, Fe& g, std::uint64_t swap)
{
    std::uint64_t const mask = 0 - swap;
    for (int i = 0; i < 5; ++i) {
        std::uint64_t const t = mask & (f.v[i] ^ g.v[i]);
        f.v[i] ^= t;
        g.v[i] ^= t;
    }
}

}

bool x25519(X25519Key& out, X25519Key const& scalar, X25519Key const& point)
{
    // §5: clamp the scalar; the u-coordinate's top bit is masked by the
    // decoder.
    std::uint8_t k[32];
    std::memcpy(k, scalar.data(), 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    Fe const x1 = fe_frombytes(point.data());
    Fe x2 = { { 1, 0, 0, 0, 0 } };
    Fe z2 = { { 0, 0, 0, 0, 0 } };
    Fe x3 = x1;
    Fe z3 = { { 1, 0, 0, 0, 0 } };
    Fe const a24 = { { 121665, 0, 0, 0, 0 } };
    std::uint64_t swap = 0;
    for (int t = 254; t >= 0; --t) {
        std::uint64_t const k_t = (k[t / 8] >> (t % 8)) & 1;
        swap ^= k_t;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = k_t;
        Fe const a = fe_add(x2, z2);
        Fe const aa = fe_sq(a);
        Fe const b = fe_sub(x2, z2);
        Fe const bb = fe_sq(b);
        Fe const e = fe_sub(aa, bb);
        Fe const c = fe_add(x3, z3);
        Fe const d = fe_sub(x3, z3);
        Fe const da = fe_mul(d, a);
        Fe const cb = fe_mul(c, b);
        x3 = fe_sq(fe_add(da, cb));
        z3 = fe_mul(x1, fe_sq(fe_sub(da, cb)));
        x2 = fe_mul(aa, bb);
        z2 = fe_mul(e, fe_add(aa, fe_mul(a24, e)));
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_tobytes(out.data(), fe_mul(x2, fe_invert(z2)));

    // RFC 7748 §6.1 / RFC 8446 §7.4.2: an all-zero output means a
    // low-order point and must be refused; checked without an early exit.
    std::uint8_t any = 0;
    for (std::uint8_t const byte : out)
        any = static_cast<std::uint8_t>(any | byte);
    return any != 0;
}

X25519Key x25519_public(X25519Key const& scalar)
{
    X25519Key const base = { { 9 } };
    X25519Key out;
    x25519(out, scalar, base);
    return out;
}

}
