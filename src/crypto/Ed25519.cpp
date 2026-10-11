#include "crypto/Ed25519.h"

#include "crypto/BigInt.h"
#include "crypto/Field25519.h"
#include "crypto/Sha2.h"

#include <cstring>
#include <optional>
#include <vector>

namespace sashfold::crypto {

using namespace field25519;

namespace {

Fe fe_from_hex_le(char const* hex)
{
    std::uint8_t bytes[32] = {};
    for (std::size_t i = 0; i < 32; ++i) {
        auto const nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        bytes[i] = static_cast<std::uint8_t>(nibble(hex[2 * i]) * 16 + nibble(hex[2 * i + 1]));
    }
    return fe_frombytes(bytes);
}

Fe fe_zero()
{
    return Fe { { 0, 0, 0, 0, 0 } };
}

Fe fe_one()
{
    return Fe { { 1, 0, 0, 0, 0 } };
}

// d = −121665/121666, 2d and √−1, little-endian.
Fe const& curve_d()
{
    static Fe const d = fe_from_hex_le("a3785913ca4deb75abd841414d0a700098e879777940c78c73fe6f2bee6c0352");
    return d;
}

Fe const& curve_2d()
{
    static Fe const d2 = fe_from_hex_le("59f1b226949bd6eb56b183829a14e00030d1f3eef2808e19e7fcdf56dcd90624");
    return d2;
}

Fe const& sqrt_minus_one()
{
    static Fe const i = fe_from_hex_le("b0a00e4a271beec478e42fad0618432fa7d7fb3d99004d2b0bdfc14f8024832b");
    return i;
}

bool fe_equal(Fe const& a, Fe const& b)
{
    std::uint8_t x[32];
    std::uint8_t y[32];
    fe_tobytes(x, a);
    fe_tobytes(y, b);
    return std::memcmp(x, y, 32) == 0;
}

bool fe_is_negative(Fe const& a)
{
    std::uint8_t x[32];
    fe_tobytes(x, a);
    return (x[0] & 1) != 0;
}

// −a. fe_sub computes 2p − a, which wraps for a limb above 2p's: the
// input is carried first, since a difference can carry limbs past 2^52.
Fe fe_neg(Fe a)
{
    fe_carry(a);
    return fe_sub(fe_zero(), a);
}

// z^((p − 5)/8) = z^(2^252 − 3), the chain of RFC 8032's reference code.
Fe fe_pow22523(Fe const& z)
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
    return fe_mul(fe_sq_times(z2_250_0, 2), z);
}

// A point in extended coordinates (§5.1.4): x = X/Z, y = Y/Z, xy = T/Z.
struct Point {
    Fe x;
    Fe y;
    Fe z;
    Fe t;
};

Point identity()
{
    return Point { fe_zero(), fe_one(), fe_one(), fe_zero() };
}

// §5.1.4's addition and doubling, complete for every pair of points.
Point add(Point const& p, Point const& q)
{
    Fe const a = fe_mul(fe_sub(p.y, p.x), fe_sub(q.y, q.x));
    Fe const b = fe_mul(fe_add(p.y, p.x), fe_add(q.y, q.x));
    Fe const c = fe_mul(fe_mul(p.t, curve_2d()), q.t);
    Fe const zz = fe_mul(p.z, q.z);
    Fe const d = fe_add(zz, zz);
    Fe const e = fe_sub(b, a);
    Fe const f = fe_sub(d, c);
    Fe const g = fe_add(d, c);
    Fe const h = fe_add(b, a);
    return Point { fe_mul(e, f), fe_mul(g, h), fe_mul(f, g), fe_mul(e, h) };
}

Point dbl(Point const& p)
{
    Fe const a = fe_sq(p.x);
    Fe const b = fe_sq(p.y);
    Fe const zz = fe_sq(p.z);
    Fe const c = fe_add(zz, zz);
    Fe const h = fe_add(a, b);
    Fe const e = fe_sub(h, fe_sq(fe_add(p.x, p.y)));
    Fe const g = fe_sub(a, b);
    Fe const f = fe_add(c, g);
    return Point { fe_mul(e, f), fe_mul(g, h), fe_mul(f, g), fe_mul(e, h) };
}

Point negate(Point const& p)
{
    return Point { fe_neg(p.x), p.y, p.z, fe_neg(p.t) };
}

void select(Point& out, Point const& candidate, std::uint64_t take)
{
    Point copy = candidate;
    fe_cswap(out.x, copy.x, take);
    fe_cswap(out.y, copy.y, take);
    fe_cswap(out.z, copy.z, take);
    fe_cswap(out.t, copy.t, take);
}

// §5.1.2: y with the sign of x in the top bit.
std::array<std::uint8_t, 32> encode(Point const& p)
{
    Fe const z_inverse = fe_invert(p.z);
    Fe const x = fe_mul(p.x, z_inverse);
    Fe const y = fe_mul(p.y, z_inverse);
    std::array<std::uint8_t, 32> out {};
    fe_tobytes(out.data(), y);
    out[31] = static_cast<std::uint8_t>(out[31] | (fe_is_negative(x) ? 0x80 : 0));
    return out;
}

// §5.1.3: y must be below p, x is the root of (y² − 1)/(dy² + 1) with the
// encoded sign, and x = 0 with the sign bit set is refused.
std::optional<Point> decode(std::span<std::uint8_t const> bytes)
{
    if (bytes.size() != 32)
        return std::nullopt;
    std::uint8_t y_bytes[32];
    std::memcpy(y_bytes, bytes.data(), 32);
    bool const sign = (y_bytes[31] & 0x80) != 0;
    y_bytes[31] &= 0x7f;
    Fe const y = fe_frombytes(y_bytes);
    std::uint8_t canonical[32];
    fe_tobytes(canonical, y);
    if (std::memcmp(canonical, y_bytes, 32) != 0)
        return std::nullopt;
    Fe const yy = fe_sq(y);
    Fe const u = fe_sub(yy, fe_one());
    Fe const v = fe_add(fe_mul(yy, curve_d()), fe_one());
    Fe const v3 = fe_mul(fe_sq(v), v);
    Fe const v7 = fe_mul(fe_sq(v3), v);
    Fe x = fe_mul(fe_mul(u, v3), fe_pow22523(fe_mul(u, v7)));
    Fe const vxx = fe_mul(v, fe_sq(x));
    if (!fe_equal(vxx, u)) {
        if (!fe_equal(vxx, fe_neg(u)))
            return std::nullopt;
        x = fe_mul(x, sqrt_minus_one());
    }
    bool const x_zero = fe_equal(x, fe_zero());
    if (x_zero && sign)
        return std::nullopt;
    if (fe_is_negative(x) != sign)
        x = fe_neg(x);
    return Point { x, y, fe_one(), fe_mul(x, y) };
}

Point const& base_point()
{
    static Point const b = [] {
        std::uint8_t encoded[32];
        encoded[0] = 0x58;
        for (std::size_t i = 1; i < 32; ++i)
            encoded[i] = 0x66;
        return *decode(encoded);
    }();
    return b;
}

// k·p for a secret k (32 little-endian bytes): double, add, and keep the
// sum under a mask, from the top bit down.
Point multiply_ct(Point const& p, std::span<std::uint8_t const> k)
{
    Point r = identity();
    for (std::size_t bit = 256; bit-- > 0;) {
        r = dbl(r);
        Point const sum = add(r, p);
        std::uint64_t const take = (k[bit / 8] >> (bit % 8)) & 1u;
        select(r, sum, take);
    }
    return r;
}

// k·p for a public k, adding only where a bit is set.
Point multiply_vt(Point const& p, std::span<std::uint8_t const> k)
{
    Point r = identity();
    for (std::size_t bit = 256; bit-- > 0;) {
        r = dbl(r);
        if (((k[bit / 8] >> (bit % 8)) & 1u) != 0)
            r = add(r, p);
    }
    return r;
}

bool is_identity(Point const& p)
{
    return fe_equal(p.x, fe_zero()) && fe_equal(p.y, p.z);
}

bool small_order(Point const& p)
{
    return is_identity(dbl(dbl(dbl(p))));
}

BigInt const& group_order()
{
    static BigInt const l = *BigInt::from_hex("1000000000000000000000000000000014def9dea2f79cd65812631a5cf5d3ed");
    return l;
}

BigInt from_le(std::span<std::uint8_t const> bytes)
{
    std::vector<std::uint8_t> big(bytes.rbegin(), bytes.rend());
    return *BigInt::from_bytes(big);
}

std::array<std::uint8_t, 32> to_le(BigInt const& value)
{
    std::vector<std::uint8_t> const big = *value.to_bytes(32);
    std::array<std::uint8_t, 32> out {};
    for (std::size_t i = 0; i < 32; ++i)
        out[i] = big[31 - i];
    return out;
}

// SHA-512 of the parts, read as a little-endian number modulo L.
BigInt hash_mod_l(std::span<std::uint8_t const> a, std::span<std::uint8_t const> b, std::span<std::uint8_t const> c)
{
    Sha512 h;
    h.update(a);
    h.update(b);
    h.update(c);
    Sha512::Digest const digest = h.finish();
    return from_le(digest).mod(group_order());
}

struct Expanded {
    std::array<std::uint8_t, 32> scalar; // s, clamped (§5.1.5 step 2)
    std::array<std::uint8_t, 32> prefix;
};

Expanded expand(Ed25519Seed const& seed)
{
    Sha512::Digest const h = Sha512::hash(seed);
    Expanded out {};
    std::memcpy(out.scalar.data(), h.data(), 32);
    std::memcpy(out.prefix.data(), h.data() + 32, 32);
    out.scalar[0] = static_cast<std::uint8_t>(out.scalar[0] & 248);
    out.scalar[31] = static_cast<std::uint8_t>((out.scalar[31] & 127) | 64);
    return out;
}

}

Ed25519PublicKey ed25519_public_key(Ed25519Seed const& seed)
{
    return encode(multiply_ct(base_point(), expand(seed).scalar));
}

Ed25519Signature ed25519_sign(Ed25519Seed const& seed, std::span<std::uint8_t const> message)
{
    Expanded const expanded = expand(seed);
    Ed25519PublicKey const public_key = encode(multiply_ct(base_point(), expanded.scalar));
    BigInt const r = hash_mod_l(expanded.prefix, message, {});
    std::array<std::uint8_t, 32> const r_bytes = to_le(r);
    std::array<std::uint8_t, 32> const big_r = encode(multiply_ct(base_point(), r_bytes));
    BigInt const k = hash_mod_l(big_r, public_key, message);
    BigInt const s = r.add(k.mod_mul(from_le(expanded.scalar), group_order())).mod(group_order());
    Ed25519Signature signature {};
    std::memcpy(signature.data(), big_r.data(), 32);
    std::array<std::uint8_t, 32> const s_bytes = to_le(s);
    std::memcpy(signature.data() + 32, s_bytes.data(), 32);
    return signature;
}

bool ed25519_verify(Ed25519PublicKey const& key, std::span<std::uint8_t const> message, std::span<std::uint8_t const> signature)
{
    if (signature.size() != 64)
        return false;
    std::optional<Point> const a = decode(key);
    std::optional<Point> const r = decode(signature.subspan(0, 32));
    if (!a || !r || small_order(*a) || small_order(*r))
        return false;
    BigInt const s = from_le(signature.subspan(32));
    if (s.compare(group_order()) >= 0)
        return false;
    BigInt const k = hash_mod_l(signature.subspan(0, 32), key, message);
    // R' = [S]B − [k]A, compared with R by encoding: cofactorless.
    Point const check = add(multiply_vt(base_point(), to_le(s)), negate(multiply_vt(*a, to_le(k))));
    std::array<std::uint8_t, 32> const encoded = encode(check);
    return std::memcmp(encoded.data(), signature.data(), 32) == 0;
}

bool ed25519_point_valid(Ed25519PublicKey const& key)
{
    return decode(key).has_value();
}

}
