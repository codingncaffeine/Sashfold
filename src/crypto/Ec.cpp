#include "crypto/Ec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace sashfold::crypto {

namespace {

// FIPS 186-4 §D.1.2.3 and §D.1.2.4 as big-endian hex, read once into BigInt
// for the boundary and once, at compile time, into the fixed limbs below.
// The coefficient a is −3 on both curves, which the doubling relies on.
constexpr std::string_view p256_p = "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff";
constexpr std::string_view p256_b = "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b";
constexpr std::string_view p256_n = "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551";
constexpr std::string_view p256_gx = "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296";
constexpr std::string_view p256_gy = "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";

constexpr std::string_view p384_p = "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff";
constexpr std::string_view p384_b = "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef";
constexpr std::string_view p384_n = "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973";
constexpr std::string_view p384_gx = "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7";
constexpr std::string_view p384_gy = "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f";

Curve make_curve(CurveId id, std::size_t field_bytes, std::string_view p, std::string_view b, std::string_view n,
    std::string_view gx, std::string_view gy)
{
    BigInt const prime = *BigInt::from_hex(p);
    return Curve { id, field_bytes, prime, prime.sub(BigInt::from_u64(3)), *BigInt::from_hex(b), *BigInt::from_hex(n),
        *BigInt::from_hex(gx), *BigInt::from_hex(gy) };
}

// ---------------------------------------------------------------------------
// The field: N 64-bit limbs, least significant first, always fully reduced
// below the modulus. Nothing here branches on a limb's value or indexes
// memory by one; where a result depends on a comparison, both candidates
// are computed and a mask chooses between them.

__extension__ typedef unsigned __int128 u128;
using u64 = std::uint64_t;

template<std::size_t N>
using Limbs = std::array<u64, N>;

constexpr u64 low(u128 v) { return static_cast<u64>(v); }
constexpr u64 high(u128 v) { return static_cast<u64>(v >> 64); }

// All ones when v is zero, zero otherwise.
constexpr u64 zero_mask(u64 v) { return ((v | (0 - v)) >> 63) - 1; }

template<std::size_t N>
constexpr u64 zero_mask(Limbs<N> const& a)
{
    u64 any = 0;
    for (u64 const limb : a)
        any |= limb;
    return zero_mask(any);
}

// out = in where mask is all ones, out unchanged where it is zero.
template<std::size_t N>
constexpr void select(Limbs<N>& out, Limbs<N> const& in, u64 mask)
{
    for (std::size_t i = 0; i < N; ++i)
        out[i] = (in[i] & mask) | (out[i] & ~mask);
}

template<std::size_t N>
constexpr Limbs<N> limbs_from_hex(std::string_view hex)
{
    Limbs<N> out {};
    for (std::size_t i = 0; i < hex.size(); ++i) {
        char const c = hex[hex.size() - 1 - i];
        u64 const digit = c <= '9' ? static_cast<u64>(c - '0')
            : c <= 'F'             ? static_cast<u64>(c - 'A' + 10)
                                   : static_cast<u64>(c - 'a' + 10);
        out[i / 16] |= digit << (4 * (i % 16));
    }
    return out;
}

// Exactly 8·N big-endian bytes.
template<std::size_t N>
Limbs<N> limbs_from_bytes(std::span<std::uint8_t const> bytes)
{
    Limbs<N> out {};
    for (std::size_t i = 0; i < 8 * N; ++i) {
        std::size_t const bit = 8 * (8 * N - 1 - i);
        out[bit / 64] |= static_cast<u64>(bytes[i]) << (bit % 64);
    }
    return out;
}

template<std::size_t N>
std::vector<std::uint8_t> bytes_from_limbs(Limbs<N> const& a)
{
    std::vector<std::uint8_t> out(8 * N);
    for (std::size_t i = 0; i < 8 * N; ++i) {
        std::size_t const bit = 8 * (8 * N - 1 - i);
        out[i] = static_cast<std::uint8_t>(a[bit / 64] >> (bit % 64));
    }
    return out;
}

// The value must fit in 64·N bits; every caller has checked it against a
// modulus of that width first.
template<std::size_t N>
Limbs<N> limbs_from_bigint(BigInt const& value)
{
    return limbs_from_bytes<N>(*value.to_bytes(8 * N));
}

template<std::size_t N>
BigInt bigint_from_limbs(Limbs<N> const& a)
{
    return *BigInt::from_bytes(bytes_from_limbs(a));
}

// top·2^(64N) + t, known to be below 2m, reduced below m: the subtraction
// is kept when the value overflowed the limbs or the subtraction did not
// borrow.
template<std::size_t N>
constexpr Limbs<N> reduce_once(Limbs<N> const& t, u64 top, Limbs<N> const& m)
{
    Limbs<N> difference {};
    u64 borrow = 0;
    for (std::size_t i = 0; i < N; ++i) {
        u128 const d = static_cast<u128>(t[i]) - m[i] - borrow;
        difference[i] = low(d);
        borrow = high(d) & 1;
    }
    Limbs<N> out = t;
    select(out, difference, 0 - ((top | (borrow ^ 1)) & 1));
    return out;
}

template<std::size_t N>
constexpr Limbs<N> fe_add(Limbs<N> const& a, Limbs<N> const& b, Limbs<N> const& m)
{
    Limbs<N> sum {};
    u64 carry = 0;
    for (std::size_t i = 0; i < N; ++i) {
        u128 const s = static_cast<u128>(a[i]) + b[i] + carry;
        sum[i] = low(s);
        carry = high(s);
    }
    return reduce_once(sum, carry, m);
}

template<std::size_t N>
constexpr Limbs<N> fe_sub(Limbs<N> const& a, Limbs<N> const& b, Limbs<N> const& m)
{
    Limbs<N> difference {};
    u64 borrow = 0;
    for (std::size_t i = 0; i < N; ++i) {
        u128 const d = static_cast<u128>(a[i]) - b[i] - borrow;
        difference[i] = low(d);
        borrow = high(d) & 1;
    }
    // A borrow means a − b went below zero: add m back, masked.
    u64 const mask = 0 - borrow;
    u64 carry = 0;
    for (std::size_t i = 0; i < N; ++i) {
        u128 const s = static_cast<u128>(difference[i]) + (m[i] & mask) + carry;
        difference[i] = low(s);
        carry = high(s);
    }
    return difference;
}

// A modulus and the constants Montgomery multiplication needs, all derived
// from the modulus itself at compile time.
template<std::size_t N>
struct Modulus {
    Limbs<N> m {};
    u64 m0_inverse {}; // −m⁻¹ mod 2^64
    Limbs<N> one {}; // R mod m, R = 2^(64N): the Montgomery form of 1
    Limbs<N> r2 {}; // R² mod m, which takes a value into Montgomery form
    Limbs<N> m_minus_2 {}; // the Fermat exponent of an inverse
};

// Montgomery multiplication, a·b·R⁻¹ mod m, by coarsely integrated operand
// scanning (Koç, Acar and Kaliski, "Analyzing and Comparing Montgomery
// Multiplication Algorithms", 1996): each round adds a·b[i] and then the
// multiple of m that clears the lowest limb, shifting one limb down. With
// a·b < m·R the running value stays below 2m, so a single masked
// subtraction finishes it.
template<std::size_t N>
constexpr Limbs<N> fe_mul(Limbs<N> const& a, Limbs<N> const& b, Modulus<N> const& mod)
{
    std::array<u64, N + 2> t {};
    for (std::size_t i = 0; i < N; ++i) {
        u64 carry = 0;
        for (std::size_t j = 0; j < N; ++j) {
            u128 const s = static_cast<u128>(a[j]) * b[i] + t[j] + carry;
            t[j] = low(s);
            carry = high(s);
        }
        u128 s = static_cast<u128>(t[N]) + carry;
        t[N] = low(s);
        t[N + 1] = high(s);
        u64 const q = t[0] * mod.m0_inverse;
        s = static_cast<u128>(q) * mod.m[0] + t[0];
        carry = high(s);
        for (std::size_t j = 1; j < N; ++j) {
            s = static_cast<u128>(q) * mod.m[j] + t[j] + carry;
            t[j - 1] = low(s);
            carry = high(s);
        }
        s = static_cast<u128>(t[N]) + carry;
        t[N - 1] = low(s);
        t[N] = t[N + 1] + high(s);
    }
    Limbs<N> r {};
    for (std::size_t i = 0; i < N; ++i)
        r[i] = t[i];
    return reduce_once(r, t[N], mod.m);
}

// Any a below R, times R² and reduced: a·R mod m.
template<std::size_t N>
constexpr Limbs<N> to_montgomery(Limbs<N> const& a, Modulus<N> const& mod)
{
    return fe_mul(a, mod.r2, mod);
}

template<std::size_t N>
constexpr Limbs<N> from_montgomery(Limbs<N> const& a, Modulus<N> const& mod)
{
    Limbs<N> one {};
    one[0] = 1;
    return fe_mul(a, one, mod);
}

// a^e for a public exponent e, by fixed 4-bit windows from the top: the
// sequence of multiplications depends on e alone, never on a. a is in
// Montgomery form and so is the result.
template<std::size_t N>
constexpr Limbs<N> fe_pow(Limbs<N> const& a, Limbs<N> const& e, Modulus<N> const& mod)
{
    std::array<Limbs<N>, 16> table {};
    table[0] = mod.one;
    table[1] = a;
    for (std::size_t i = 2; i < 16; ++i)
        table[i] = fe_mul(table[i - 1], a, mod);
    Limbs<N> r = mod.one;
    for (std::size_t w = 16 * N; w-- > 0;) {
        for (int i = 0; i < 4; ++i)
            r = fe_mul(r, r, mod);
        r = fe_mul(r, table[(e[w / 16] >> (4 * (w % 16))) & 15], mod);
    }
    return r;
}

// The inverse modulo a prime by Fermat, a^(m−2): a few hundred
// multiplications and no division, and constant time in a.
template<std::size_t N>
constexpr Limbs<N> fe_invert(Limbs<N> const& a, Modulus<N> const& mod)
{
    return fe_pow(a, mod.m_minus_2, mod);
}

template<std::size_t N>
constexpr Modulus<N> make_modulus(std::string_view hex)
{
    Modulus<N> mod {};
    mod.m = limbs_from_hex<N>(hex);
    // Newton's iteration x ← x·(2 − m0·x) doubles the correct low bits of
    // an inverse: an odd m0 is its own inverse modulo 8, and five steps
    // take 3 bits past 64.
    u64 inverse = mod.m[0];
    for (int i = 0; i < 5; ++i)
        inverse *= 2 - mod.m[0] * inverse;
    mod.m0_inverse = 0 - inverse;
    // R mod m by doubling 1 64·N times, R² mod m by as many more.
    Limbs<N> x {};
    x[0] = 1;
    for (std::size_t i = 0; i < 64 * N; ++i)
        x = fe_add(x, x, mod.m);
    mod.one = x;
    for (std::size_t i = 0; i < 64 * N; ++i)
        x = fe_add(x, x, mod.m);
    mod.r2 = x;
    u64 borrow = 2;
    for (std::size_t i = 0; i < N; ++i) {
        u128 const d = static_cast<u128>(mod.m[i]) - borrow;
        mod.m_minus_2[i] = low(d);
        borrow = high(d) & 1;
    }
    return mod;
}

// The two curves' constants: the field and the group order as Montgomery
// moduli, b and the generator in Montgomery form.
struct P256 {
    static constexpr std::size_t N = 4;
    static constexpr Modulus<N> p = make_modulus<N>(p256_p);
    static constexpr Modulus<N> n = make_modulus<N>(p256_n);
    static constexpr Limbs<N> b = to_montgomery(limbs_from_hex<N>(p256_b), p);
    static constexpr Limbs<N> gx = to_montgomery(limbs_from_hex<N>(p256_gx), p);
    static constexpr Limbs<N> gy = to_montgomery(limbs_from_hex<N>(p256_gy), p);
};

struct P384 {
    static constexpr std::size_t N = 6;
    static constexpr Modulus<N> p = make_modulus<N>(p384_p);
    static constexpr Modulus<N> n = make_modulus<N>(p384_n);
    static constexpr Limbs<N> b = to_montgomery(limbs_from_hex<N>(p384_b), p);
    static constexpr Limbs<N> gx = to_montgomery(limbs_from_hex<N>(p384_gx), p);
    static constexpr Limbs<N> gy = to_montgomery(limbs_from_hex<N>(p384_gy), p);
};

// The Montgomery constants checked against what they must be, so a wrong
// one fails the build rather than a handshake.
template<typename C>
constexpr bool constants_hold()
{
    constexpr std::size_t N = C::N;
    Limbs<N> one {};
    one[0] = 1;
    for (Modulus<N> const* mod : { &C::p, &C::n }) {
        if (mod->m[0] * (0 - mod->m0_inverse) != 1)
            return false;
        if (from_montgomery(mod->one, *mod) != one || fe_mul(mod->one, mod->one, *mod) != mod->one
            || to_montgomery(one, *mod) != mod->one)
            return false;
    }
    // A scalar of 64·N bits reduces below the order with one subtraction
    // only when the order's top bit is set.
    return (C::n.m[N - 1] >> 63) == 1;
}

static_assert(constants_hold<P256>());
static_assert(constants_hold<P384>());

// ---------------------------------------------------------------------------
// The group, in Jacobian coordinates over the Montgomery field: (X, Y, Z)
// stands for (X/Z², Y/Z³), and Z = 0 is the point at infinity. The formula
// names are those of Bernstein and Lange's Explicit-Formulas Database
// ("short Weierstrass curves, Jacobian coordinates with a = −3").

template<std::size_t N>
struct Jacobian {
    Limbs<N> x;
    Limbs<N> y;
    Limbs<N> z;
};

template<std::size_t N>
struct Affine {
    Limbs<N> x;
    Limbs<N> y;
};

template<std::size_t N>
void select(Jacobian<N>& out, Jacobian<N> const& in, u64 mask)
{
    select(out.x, in.x, mask);
    select(out.y, in.y, mask);
    select(out.z, in.z, mask);
}

template<typename C>
struct Group {
    static constexpr std::size_t N = C::N;
    // 4-bit windows in a scalar of 64·N bits.
    static constexpr std::size_t windows = 16 * N;
    using Fe = Limbs<N>;
    using Point = Jacobian<N>;
    using Entry = Affine<N>;

    static Fe mul(Fe const& a, Fe const& b) { return fe_mul(a, b, C::p); }
    static Fe sqr(Fe const& a) { return fe_mul(a, a, C::p); }
    static Fe add(Fe const& a, Fe const& b) { return fe_add(a, b, C::p.m); }
    static Fe sub(Fe const& a, Fe const& b) { return fe_sub(a, b, C::p.m); }
    static Fe invert(Fe const& a) { return fe_invert(a, C::p); }
    // A value below p into Montgomery form and back.
    static Fe to_field(BigInt const& a) { return to_montgomery(limbs_from_bigint<N>(a), C::p); }
    static BigInt from_field(Fe const& a) { return bigint_from_limbs(from_montgomery(a, C::p)); }

    static Point infinity() { return Point { C::p.one, C::p.one, Fe {} }; }
    static Point from_affine(Entry const& a) { return Point { a.x, a.y, C::p.one }; }
    static Entry generator() { return Entry { C::gx, C::gy }; }

    // dbl-2001-b, 3M + 5S. The point at infinity doubles to itself with no
    // special case (Z3 = (Y + 0)² − Y² − 0 = 0), and a prime-order curve has
    // no point with Y = 0 for the formula to meet.
    static Point dbl(Point const& q)
    {
        Fe const delta = sqr(q.z);
        Fe const gamma = sqr(q.y);
        Fe const beta = mul(q.x, gamma);
        Fe const t = mul(sub(q.x, delta), add(q.x, delta));
        Fe const alpha = add(add(t, t), t);
        Fe const beta2 = add(beta, beta);
        Fe const beta4 = add(beta2, beta2);
        Point r;
        r.x = sub(sqr(alpha), add(beta4, beta4));
        r.z = sub(sub(sqr(add(q.y, q.z)), gamma), delta);
        Fe const gamma2 = sqr(gamma);
        Fe const gamma4 = add(add(gamma2, gamma2), add(gamma2, gamma2));
        r.y = sub(mul(alpha, sub(beta4, r.x)), add(gamma4, gamma4));
        return r;
    }

    // The generic sum, with the two differences whose zeros mark the cases
    // it gets wrong: H = 0 and R = 0 when the inputs are the same point
    // (the result must be the doubling), H = 0 alone when they are
    // opposite (Z3 comes out 0, correctly the point at infinity). An input
    // at infinity is also wrong and is the caller's to handle.
    struct Sum {
        Point point;
        Fe h;
        Fe r;
    };

    // add-2007-bl, 11M + 5S.
    static Sum add_formula(Point const& a, Point const& b)
    {
        Fe const z1z1 = sqr(a.z);
        Fe const z2z2 = sqr(b.z);
        Fe const u1 = mul(a.x, z2z2);
        Fe const u2 = mul(b.x, z1z1);
        Fe const s1 = mul(mul(a.y, b.z), z2z2);
        Fe const s2 = mul(mul(b.y, a.z), z1z1);
        Fe const h = sub(u2, u1);
        Fe const i = sqr(add(h, h));
        Fe const j = mul(h, i);
        Fe const half_r = sub(s2, s1);
        Fe const r = add(half_r, half_r);
        Fe const v = mul(u1, i);
        Point out;
        out.x = sub(sub(sqr(r), j), add(v, v));
        Fe const s1j = mul(s1, j);
        out.y = sub(mul(r, sub(v, out.x)), add(s1j, s1j));
        out.z = mul(sub(sub(sqr(add(a.z, b.z)), z1z1), z2z2), h);
        return Sum { out, h, r };
    }

    // madd-2007-bl, 7M + 4S: the second input affine (Z2 = 1).
    static Sum madd_formula(Point const& a, Entry const& b)
    {
        Fe const z1z1 = sqr(a.z);
        Fe const u2 = mul(b.x, z1z1);
        Fe const s2 = mul(mul(b.y, a.z), z1z1);
        Fe const h = sub(u2, a.x);
        Fe const hh = sqr(h);
        Fe const hh2 = add(hh, hh);
        Fe const i = add(hh2, hh2);
        Fe const j = mul(h, i);
        Fe const half_r = sub(s2, a.y);
        Fe const r = add(half_r, half_r);
        Fe const v = mul(a.x, i);
        Point out;
        out.x = sub(sub(sqr(r), j), add(v, v));
        Fe const y1j = mul(a.y, j);
        out.y = sub(mul(r, sub(v, out.x)), add(y1j, y1j));
        out.z = sub(sub(sqr(add(a.z, h)), z1z1), hh);
        return Sum { out, h, r };
    }

    // Public inputs: the special cases are branched on.
    static Point add_vt(Point const& a, Point const& b)
    {
        if (zero_mask(a.z))
            return b;
        if (zero_mask(b.z))
            return a;
        Sum const s = add_formula(a, b);
        if (zero_mask(s.h) & zero_mask(s.r))
            return dbl(a);
        return s.point;
    }

    static Point madd_vt(Point const& a, Entry const& b)
    {
        if (zero_mask(a.z))
            return from_affine(b);
        Sum const s = madd_formula(a, b);
        if (zero_mask(s.h) & zero_mask(s.r))
            return dbl(a);
        return s.point;
    }

    // Secret inputs: complete, every case computed and chosen by mask, so
    // the same field operations run whatever the points are.
    static Point add_ct(Point const& a, Point const& b)
    {
        Sum const s = add_formula(a, b);
        Point out = s.point;
        u64 const a_infinite = zero_mask(a.z);
        u64 const b_infinite = zero_mask(b.z);
        select(out, dbl(a), zero_mask(s.h) & zero_mask(s.r) & ~a_infinite & ~b_infinite);
        select(out, b, a_infinite);
        select(out, a, b_infinite);
        return out;
    }

    // b_infinite is a mask: all ones when b stands for the point at
    // infinity, which an affine entry cannot hold.
    static Point madd_ct(Point const& a, Entry const& b, u64 b_infinite)
    {
        Sum const s = madd_formula(a, b);
        Point out = s.point;
        u64 const a_infinite = zero_mask(a.z);
        select(out, dbl(a), zero_mask(s.h) & zero_mask(s.r) & ~a_infinite & ~b_infinite);
        select(out, from_affine(b), a_infinite);
        select(out, a, b_infinite);
        return out;
    }

    static u64 nibble(Fe const& k, std::size_t window) { return (k[window / 16] >> (4 * (window % 16))) & 15; }

    // Affine forms of many points for the price of one inversion
    // (Montgomery's trick): invert the product of every Z, then peel the
    // factors off from the end. No point may be at infinity.
    static std::vector<Entry> to_affine_batch(std::vector<Point> const& points)
    {
        std::vector<Fe> prefix(points.size());
        Fe product = C::p.one;
        for (std::size_t i = 0; i < points.size(); ++i) {
            product = mul(product, points[i].z);
            prefix[i] = product;
        }
        Fe inverse = fe_invert(product, C::p);
        std::vector<Entry> out(points.size());
        for (std::size_t i = points.size(); i-- > 0;) {
            Fe const z_inverse = i == 0 ? inverse : mul(inverse, prefix[i - 1]);
            inverse = mul(inverse, points[i].z);
            Fe const z_inverse2 = sqr(z_inverse);
            out[i] = Entry { mul(points[i].x, z_inverse2), mul(points[i].y, mul(z_inverse2, z_inverse)) };
        }
        return out;
    }

    // The generator's table is a comb: row i holds d·16^(4i)·G for the
    // digits d in 1..15, affine, so that window 4i + j of a scalar is row
    // i's entry scaled by 16^j. A product is four passes over the rows, one
    // mixed addition per window, with four doublings between passes:
    //   k·G = Σ_j 16^j · Σ_i row_i[digit(4i + j)].
    // Twelve doublings buy a table a quarter of the size of one row per
    // window: 15 KB for P-256 and 35 KB for P-384, computed at first use.
    static constexpr std::size_t comb_spacing = 4;
    static constexpr std::size_t comb_rows = windows / comb_spacing;
    static_assert(windows % comb_spacing == 0);

    static std::vector<Entry> build_base_table()
    {
        std::vector<Point> points(comb_rows * 15);
        Point base = from_affine(generator());
        for (std::size_t row = 0; row < comb_rows; ++row) {
            points[row * 15] = base;
            for (std::size_t d = 1; d < 15; ++d)
                points[row * 15 + d] = add_vt(points[row * 15 + d - 1], base);
            for (std::size_t i = 0; i < 4 * comb_spacing; ++i)
                base = dbl(base);
        }
        return to_affine_batch(points);
    }

    static std::vector<Entry> const& base_table()
    {
        static std::vector<Entry> const table = build_base_table();
        return table;
    }

    static Point base_multiply_vt(Fe const& k)
    {
        std::vector<Entry> const& table = base_table();
        Point acc = infinity();
        for (std::size_t j = comb_spacing; j-- > 0;) {
            if (j + 1 < comb_spacing) {
                for (int i = 0; i < 4; ++i)
                    acc = dbl(acc);
            }
            for (std::size_t row = 0; row < comb_rows; ++row) {
                u64 const d = nibble(k, row * comb_spacing + j);
                if (d != 0)
                    acc = madd_vt(acc, table[row * 15 + d - 1]);
            }
        }
        return acc;
    }

    // Every window reads all fifteen entries of its row and keeps the one
    // its digit names by mask, and adds it through the complete addition
    // whether the digit is zero or not: the time and the memory touched
    // are the same for every scalar.
    static Point base_multiply_ct(Fe const& k)
    {
        std::vector<Entry> const& table = base_table();
        Point acc = infinity();
        for (std::size_t j = comb_spacing; j-- > 0;) {
            if (j + 1 < comb_spacing) {
                for (int i = 0; i < 4; ++i)
                    acc = dbl(acc);
            }
            for (std::size_t row = 0; row < comb_rows; ++row) {
                u64 const d = nibble(k, row * comb_spacing + j);
                Entry entry {};
                for (std::size_t e = 0; e < 15; ++e) {
                    u64 const hit = zero_mask(d ^ (e + 1));
                    select(entry.x, table[row * 15 + e].x, hit);
                    select(entry.y, table[row * 15 + e].y, hit);
                }
                acc = madd_ct(acc, entry, zero_mask(d));
            }
        }
        return acc;
    }

    // 0·P through 15·P; P is public (a peer's point or a signer's key).
    static std::array<Point, 16> multiples(Entry const& p)
    {
        std::array<Point, 16> t;
        t[0] = infinity();
        t[1] = from_affine(p);
        t[2] = dbl(t[1]);
        for (std::size_t i = 3; i < 16; ++i)
            t[i] = madd_vt(t[i - 1], p);
        return t;
    }

    static Point multiply_vt(Entry const& p, Fe const& k)
    {
        std::array<Point, 16> const t = multiples(p);
        Point acc = infinity();
        for (std::size_t w = windows; w-- > 0;) {
            for (int i = 0; i < 4; ++i)
                acc = dbl(acc);
            u64 const d = nibble(k, w);
            if (d != 0)
                acc = add_vt(acc, t[d]);
        }
        return acc;
    }

    // As base_multiply_ct: four doublings and one complete addition per
    // window, the entry chosen by mask from a full pass over the table.
    static Point multiply_ct(Entry const& p, Fe const& k)
    {
        std::array<Point, 16> const t = multiples(p);
        Point acc = infinity();
        for (std::size_t w = windows; w-- > 0;) {
            for (int i = 0; i < 4; ++i)
                acc = dbl(acc);
            u64 const d = nibble(k, w);
            Point entry = t[0];
            for (std::size_t e = 1; e < 16; ++e)
                select(entry, t[e], zero_mask(d ^ e));
            acc = add_ct(acc, entry);
        }
        return acc;
    }

    // One bit at a time from the top: the plain method the windows are
    // checked against.
    static Point multiply_double_add(Entry const& p, Fe const& k)
    {
        Point acc = infinity();
        for (std::size_t i = 64 * N; i-- > 0;) {
            acc = dbl(acc);
            if ((k[i / 64] >> (i % 64)) & 1)
                acc = madd_vt(acc, p);
        }
        return acc;
    }

    static std::optional<Entry> to_affine(Point const& q)
    {
        if (zero_mask(q.z))
            return std::nullopt;
        Fe const z_inverse = fe_invert(q.z, C::p);
        Fe const z_inverse2 = sqr(z_inverse);
        return Entry { mul(q.x, z_inverse2), mul(q.y, mul(z_inverse2, z_inverse)) };
    }

    // The caller has checked both coordinates are below p.
    static Entry from_point(EcPoint const& point)
    {
        return Entry { to_montgomery(limbs_from_bigint<N>(point.x), C::p), to_montgomery(limbs_from_bigint<N>(point.y), C::p) };
    }

    static EcPoint to_point(Entry const& a)
    {
        return EcPoint { bigint_from_limbs(from_montgomery(a.x, C::p)), bigint_from_limbs(from_montgomery(a.y, C::p)) };
    }

    static std::vector<std::uint8_t> x_bytes(Entry const& a) { return bytes_from_limbs(from_montgomery(a.x, C::p)); }

    static std::optional<EcPoint> to_point(Point const& q)
    {
        std::optional<Entry> const a = to_affine(q);
        if (!a)
            return std::nullopt;
        return to_point(*a);
    }

    // Both coordinates below p and y² = x³ − 3x + b. The point at infinity
    // has no affine form, and (0, 0) is not on either curve (b ≠ 0), so a
    // point that passes is a finite point of the group.
    static bool on_curve(Curve const& c, EcPoint const& point)
    {
        if (point.x.compare(c.p) >= 0 || point.y.compare(c.p) >= 0)
            return false;
        Entry const a = from_point(point);
        Fe const x3 = mul(sqr(a.x), a.x);
        Fe const rhs = add(sub(x3, add(add(a.x, a.x), a.x)), C::b);
        return sqr(a.y) == rhs;
    }

    // SEC 1 §4.1.4 steps 4 to 8, r and s already in [1, n − 1] and e the
    // truncated digest.
    static bool verify(EcPoint const& key, BigInt const& e, BigInt const& r, BigInt const& s)
    {
        // e < 2^(64N) < 2n, so one subtraction reduces it.
        Fe const e_reduced = reduce_once(limbs_from_bigint<N>(e), 0, C::n.m);
        Fe const r_limbs = limbs_from_bigint<N>(r);
        // w = s⁻¹ in Montgomery form; a plain value times it comes out plain.
        Fe const w = fe_invert(to_montgomery(limbs_from_bigint<N>(s), C::n), C::n);
        Fe const u1 = fe_mul(e_reduced, w, C::n);
        Fe const u2 = fe_mul(r_limbs, w, C::n);
        Point const sum = add_vt(base_multiply_vt(u1), multiply_vt(from_point(key), u2));
        if (zero_mask(sum.z))
            return false;
        // x mod n = r with x = X/Z² < p < 2n means x is r or r + n, which
        // is asked as X = r·Z² without inverting Z.
        Fe const z2 = sqr(sum.z);
        if (mul(to_montgomery(r_limbs, C::p), z2) == sum.x)
            return true;
        Fe r_plus_n {};
        u64 carry = 0;
        for (std::size_t i = 0; i < N; ++i) {
            u128 const t = static_cast<u128>(r_limbs[i]) + C::n.m[i] + carry;
            r_plus_n[i] = low(t);
            carry = high(t);
        }
        if (carry != 0 || reduce_once(r_plus_n, 0, C::p.m) != r_plus_n)
            return false;
        return mul(to_montgomery(r_plus_n, C::p), z2) == sum.x;
    }

    // A secret scalar in [1, n − 1] from the 8·N bytes a key exchange drew:
    // a value below 2^(64N) < 2n reduces with one masked subtraction, and
    // the only branch is on the zero it leaves with negligible probability,
    // which abandons the exchange anyway.
    static std::optional<Fe> secret_scalar(std::span<std::uint8_t const> bytes)
    {
        if (bytes.size() != 8 * N)
            return std::nullopt;
        Fe const k = reduce_once(limbs_from_bytes<N>(bytes), 0, C::n.m);
        if (zero_mask(k))
            return std::nullopt;
        return k;
    }
};

template<typename F>
auto with_group(CurveId id, F&& f)
{
    if (id == CurveId::P256)
        return f(Group<P256> {});
    return f(Group<P384> {});
}

// ---------------------------------------------------------------------------
// The textbook formulas over BigInt the fixed-width field replaced, kept as
// the independent reference the tests compare it with: general-a Jacobian
// doubling (Guide to Elliptic Curve Cryptography, algorithm 3.21) and
// addition, every product reduced by long division.
namespace reference {

struct Point {
    BigInt x;
    BigInt y;
    BigInt z;
    bool infinity() const { return z.is_zero(); }
};

Point infinity()
{
    return Point { BigInt::from_u64(1), BigInt::from_u64(1), BigInt() };
}

Point dbl(Curve const& c, Point const& q)
{
    if (q.infinity() || q.y.is_zero())
        return infinity();
    BigInt const& p = c.p;
    BigInt const y2 = q.y.mod_mul(q.y, p);
    BigInt const s = BigInt::from_u64(4).mod_mul(q.x, p).mod_mul(y2, p);
    BigInt const z2 = q.z.mod_mul(q.z, p);
    BigInt const m = BigInt::from_u64(3).mod_mul(q.x.mod_mul(q.x, p), p).mod_add(c.a.mod_mul(z2.mod_mul(z2, p), p), p);
    BigInt const x3 = m.mod_mul(m, p).mod_sub(s.mod_add(s, p), p);
    BigInt const y4_8 = BigInt::from_u64(8).mod_mul(y2.mod_mul(y2, p), p);
    BigInt const y3 = m.mod_mul(s.mod_sub(x3, p), p).mod_sub(y4_8, p);
    BigInt const z3 = BigInt::from_u64(2).mod_mul(q.y, p).mod_mul(q.z, p);
    return Point { x3, y3, z3 };
}

Point add(Curve const& c, Point const& q1, Point const& q2)
{
    if (q1.infinity())
        return q2;
    if (q2.infinity())
        return q1;
    BigInt const& p = c.p;
    BigInt const z1_2 = q1.z.mod_mul(q1.z, p);
    BigInt const z2_2 = q2.z.mod_mul(q2.z, p);
    BigInt const u1 = q1.x.mod_mul(z2_2, p);
    BigInt const u2 = q2.x.mod_mul(z1_2, p);
    BigInt const s1 = q1.y.mod_mul(z2_2.mod_mul(q2.z, p), p);
    BigInt const s2 = q2.y.mod_mul(z1_2.mod_mul(q1.z, p), p);
    if (u1 == u2) {
        if (!(s1 == s2))
            return infinity();
        return dbl(c, q1);
    }
    BigInt const h = u2.mod_sub(u1, p);
    BigInt const r = s2.mod_sub(s1, p);
    BigInt const h2 = h.mod_mul(h, p);
    BigInt const h3 = h2.mod_mul(h, p);
    BigInt const u1h2 = u1.mod_mul(h2, p);
    BigInt const x3 = r.mod_mul(r, p).mod_sub(h3, p).mod_sub(u1h2.mod_add(u1h2, p), p);
    BigInt const y3 = r.mod_mul(u1h2.mod_sub(x3, p), p).mod_sub(s1.mod_mul(h3, p), p);
    BigInt const z3 = h.mod_mul(q1.z, p).mod_mul(q2.z, p);
    return Point { x3, y3, z3 };
}

std::optional<EcPoint> multiply(Curve const& c, EcPoint const& point, BigInt const& k)
{
    Point const q { point.x, point.y, BigInt::from_u64(1) };
    Point result = infinity();
    for (std::size_t i = k.bit_length(); i-- > 0;) {
        result = dbl(c, result);
        if (k.bit(i))
            result = add(c, result, q);
    }
    if (result.infinity())
        return std::nullopt;
    BigInt const z_inv = result.z.mod_inverse_prime(c.p);
    BigInt const z_inv2 = z_inv.mod_mul(z_inv, c.p);
    return EcPoint { result.x.mod_mul(z_inv2, c.p), result.y.mod_mul(z_inv2.mod_mul(z_inv, c.p), c.p) };
}

}

}

Curve const& curve(CurveId id)
{
    static Curve const p256 = make_curve(CurveId::P256, 32, p256_p, p256_b, p256_n, p256_gx, p256_gy);
    static Curve const p384 = make_curve(CurveId::P384, 48, p384_p, p384_b, p384_n, p384_gx, p384_gy);
    return id == CurveId::P256 ? p256 : p384;
}

std::optional<EcPoint> ec_decode_point(CurveId id, std::span<std::uint8_t const> encoded)
{
    Curve const& c = curve(id);
    if (encoded.size() != 1 + 2 * c.field_bytes || encoded[0] != 0x04)
        return std::nullopt;
    std::optional<BigInt> const x = BigInt::from_bytes(encoded.subspan(1, c.field_bytes));
    std::optional<BigInt> const y = BigInt::from_bytes(encoded.subspan(1 + c.field_bytes, c.field_bytes));
    if (!x || !y)
        return std::nullopt;
    EcPoint const point { *x, *y };
    bool const valid = with_group(id, [&](auto group) { return decltype(group)::on_curve(c, point); });
    if (!valid)
        return std::nullopt;
    return point;
}

bool ecdsa_verify(CurveId id, EcPoint const& public_key, std::span<std::uint8_t const> digest, BigInt const& r, BigInt const& s)
{
    Curve const& c = curve(id);
    if (!with_group(id, [&](auto group) { return decltype(group)::on_curve(c, public_key); }))
        return false;
    BigInt const one = BigInt::from_u64(1);
    if (r.compare(one) < 0 || r.compare(c.n) >= 0 || s.compare(one) < 0 || s.compare(c.n) >= 0)
        return false;
    // e is the leftmost min(N, bitlen(digest)) bits of the digest.
    std::optional<BigInt> e = BigInt::from_bytes(digest);
    if (!e)
        return false;
    std::size_t const order_bits = c.n.bit_length();
    std::size_t const digest_bits = digest.size() * 8;
    if (digest_bits > order_bits)
        e = e->shift_right(digest_bits - order_bits);
    return with_group(id, [&](auto group) { return decltype(group)::verify(public_key, *e, r, s); });
}

std::vector<std::uint8_t> ec_encode_point(CurveId id, EcPoint const& point)
{
    Curve const& c = curve(id);
    std::vector<std::uint8_t> out;
    out.push_back(0x04);
    std::vector<std::uint8_t> const x = *point.x.to_bytes(c.field_bytes);
    std::vector<std::uint8_t> const y = *point.y.to_bytes(c.field_bytes);
    out.insert(out.end(), x.begin(), x.end());
    out.insert(out.end(), y.begin(), y.end());
    return out;
}

std::optional<EcPoint> ec_public_point(CurveId id, std::span<std::uint8_t const> scalar)
{
    return with_group(id, [&](auto group) -> std::optional<EcPoint> {
        using G = decltype(group);
        std::optional<typename G::Fe> const k = G::secret_scalar(scalar);
        if (!k)
            return std::nullopt;
        return G::to_point(G::base_multiply_ct(*k));
    });
}

std::optional<std::vector<std::uint8_t>> ecdh_shared_x(CurveId id, std::span<std::uint8_t const> scalar, EcPoint const& peer)
{
    Curve const& c = curve(id);
    return with_group(id, [&](auto group) -> std::optional<std::vector<std::uint8_t>> {
        using G = decltype(group);
        if (!G::on_curve(c, peer))
            return std::nullopt;
        std::optional<typename G::Fe> const k = G::secret_scalar(scalar);
        if (!k)
            return std::nullopt;
        std::optional<typename G::Entry> const product = G::to_affine(G::multiply_ct(G::from_point(peer), *k));
        if (!product)
            return std::nullopt;
        return G::x_bytes(*product);
    });
}

namespace ec_test {

std::optional<EcPoint> multiply(CurveId id, Method method, EcPoint const& point, BigInt const& k)
{
    Curve const& c = curve(id);
    if (k.compare(c.n) >= 0)
        return std::nullopt;
    if (method == Method::Reference)
        return reference::multiply(c, point, k);
    return with_group(id, [&](auto group) -> std::optional<EcPoint> {
        using G = decltype(group);
        typename G::Fe const scalar = limbs_from_bigint<G::N>(k);
        if (method == Method::BaseTable)
            return G::to_point(G::base_multiply_vt(scalar));
        if (method == Method::BaseTableConstantTime)
            return G::to_point(G::base_multiply_ct(scalar));
        if (!G::on_curve(c, point))
            return std::nullopt;
        typename G::Entry const p = G::from_point(point);
        if (method == Method::DoubleAndAdd)
            return G::to_point(G::multiply_double_add(p, scalar));
        if (method == Method::Window)
            return G::to_point(G::multiply_vt(p, scalar));
        return G::to_point(G::multiply_ct(p, scalar));
    });
}

std::optional<EcPoint> add(CurveId id, EcPoint const& point, EcPoint const& other, bool constant_time)
{
    Curve const& c = curve(id);
    return with_group(id, [&](auto group) -> std::optional<EcPoint> {
        using G = decltype(group);
        if (!G::on_curve(c, point) || !G::on_curve(c, other))
            return std::nullopt;
        typename G::Point const a = G::from_affine(G::from_point(point));
        typename G::Point const b = G::from_affine(G::from_point(other));
        return G::to_point(constant_time ? G::add_ct(a, b) : G::add_vt(a, b));
    });
}

BigInt field_multiply_chain(CurveId id, BigInt const& a, BigInt const& b, std::size_t count)
{
    return with_group(id, [&](auto group) {
        using G = decltype(group);
        typename G::Fe x = G::to_field(a);
        typename G::Fe const y = G::to_field(b);
        for (std::size_t i = 0; i < count; ++i)
            x = G::mul(x, y);
        return G::from_field(x);
    });
}

BigInt field_inverse(CurveId id, BigInt const& a)
{
    return with_group(id, [&](auto group) {
        using G = decltype(group);
        return G::from_field(G::invert(G::to_field(a)));
    });
}

}

}
