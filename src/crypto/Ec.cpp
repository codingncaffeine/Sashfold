#include "crypto/Ec.h"

namespace sashfold::crypto {

namespace {

Curve make_p256()
{
    return Curve {
        CurveId::P256,
        32,
        *BigInt::from_hex("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff"),
        *BigInt::from_hex("ffffffff00000001000000000000000000000000fffffffffffffffffffffffc"),
        *BigInt::from_hex("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b"),
        *BigInt::from_hex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551"),
        *BigInt::from_hex("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"),
        *BigInt::from_hex("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"),
    };
}

Curve make_p384()
{
    return Curve {
        CurveId::P384,
        48,
        *BigInt::from_hex("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff"),
        *BigInt::from_hex("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000fffffffc"),
        *BigInt::from_hex("b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef"),
        *BigInt::from_hex("ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973"),
        *BigInt::from_hex("aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7"),
        *BigInt::from_hex("3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f"),
    };
}

// Jacobian coordinates: (X, Y, Z) stands for (X/Z², Y/Z³); Z = 0 is the
// point at infinity.
struct Jacobian {
    BigInt x;
    BigInt y;
    BigInt z;
    bool infinity() const { return z.is_zero(); }
};

Jacobian jacobian_infinity()
{
    return Jacobian { BigInt::from_u64(1), BigInt::from_u64(1), BigInt() };
}

Jacobian to_jacobian(EcPoint const& point)
{
    return Jacobian { point.x, point.y, BigInt::from_u64(1) };
}

// Doubling with the general-a formulas (Guide to Elliptic Curve
// Cryptography, algorithm 3.21 in Jacobian form).
Jacobian ec_double(Curve const& c, Jacobian const& q)
{
    if (q.infinity() || q.y.is_zero())
        return jacobian_infinity();
    BigInt const& p = c.p;
    BigInt const y2 = q.y.mod_mul(q.y, p);
    BigInt const s = BigInt::from_u64(4).mod_mul(q.x, p).mod_mul(y2, p);
    BigInt const z2 = q.z.mod_mul(q.z, p);
    BigInt const m = BigInt::from_u64(3).mod_mul(q.x.mod_mul(q.x, p), p).mod_add(c.a.mod_mul(z2.mod_mul(z2, p), p), p);
    BigInt const x3 = m.mod_mul(m, p).mod_sub(s.mod_add(s, p), p);
    BigInt const y4_8 = BigInt::from_u64(8).mod_mul(y2.mod_mul(y2, p), p);
    BigInt const y3 = m.mod_mul(s.mod_sub(x3, p), p).mod_sub(y4_8, p);
    BigInt const z3 = BigInt::from_u64(2).mod_mul(q.y, p).mod_mul(q.z, p);
    return Jacobian { x3, y3, z3 };
}

Jacobian ec_add(Curve const& c, Jacobian const& q1, Jacobian const& q2)
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
            return jacobian_infinity();
        return ec_double(c, q1);
    }
    BigInt const h = u2.mod_sub(u1, p);
    BigInt const r = s2.mod_sub(s1, p);
    BigInt const h2 = h.mod_mul(h, p);
    BigInt const h3 = h2.mod_mul(h, p);
    BigInt const u1h2 = u1.mod_mul(h2, p);
    BigInt const x3 = r.mod_mul(r, p).mod_sub(h3, p).mod_sub(u1h2.mod_add(u1h2, p), p);
    BigInt const y3 = r.mod_mul(u1h2.mod_sub(x3, p), p).mod_sub(s1.mod_mul(h3, p), p);
    BigInt const z3 = h.mod_mul(q1.z, p).mod_mul(q2.z, p);
    return Jacobian { x3, y3, z3 };
}

// Double-and-add from the top bit; public scalars only.
Jacobian ec_multiply(Curve const& c, Jacobian const& q, BigInt const& k)
{
    Jacobian result = jacobian_infinity();
    for (std::size_t i = k.bit_length(); i-- > 0;) {
        result = ec_double(c, result);
        if (k.bit(i))
            result = ec_add(c, result, q);
    }
    return result;
}

// The Montgomery ladder: R0 and R1 always differ by q, and every bit costs
// one addition and one doubling whichever way it falls, so the sequence of
// operations does not spell out the scalar. Secret scalars only.
Jacobian ec_multiply_ladder(Curve const& c, Jacobian const& q, BigInt const& k)
{
    Jacobian r0 = jacobian_infinity();
    Jacobian r1 = q;
    for (std::size_t i = c.n.bit_length(); i-- > 0;) {
        if (k.bit(i)) {
            r0 = ec_add(c, r0, r1);
            r1 = ec_double(c, r1);
        } else {
            r1 = ec_add(c, r0, r1);
            r0 = ec_double(c, r0);
        }
    }
    return r0;
}

// A scalar in [1, n − 1] from the bytes a key exchange drew.
std::optional<BigInt> ecdh_scalar(Curve const& c, std::span<std::uint8_t const> scalar)
{
    if (scalar.size() != c.field_bytes)
        return std::nullopt;
    std::optional<BigInt> k = BigInt::from_bytes(scalar);
    if (!k)
        return std::nullopt;
    BigInt const reduced = k->mod(c.n);
    if (reduced.is_zero())
        return std::nullopt;
    return reduced;
}

std::optional<EcPoint> to_affine(Curve const& c, Jacobian const& q)
{
    if (q.infinity())
        return std::nullopt;
    BigInt const z_inv = q.z.mod_inverse_prime(c.p);
    BigInt const z_inv2 = z_inv.mod_mul(z_inv, c.p);
    return EcPoint { q.x.mod_mul(z_inv2, c.p), q.y.mod_mul(z_inv2.mod_mul(z_inv, c.p), c.p) };
}

bool on_curve(Curve const& c, EcPoint const& point)
{
    if (point.x.compare(c.p) >= 0 || point.y.compare(c.p) >= 0)
        return false;
    BigInt const lhs = point.y.mod_mul(point.y, c.p);
    BigInt const x2 = point.x.mod_mul(point.x, c.p);
    BigInt const rhs = x2.mod_mul(point.x, c.p).mod_add(c.a.mod_mul(point.x, c.p), c.p).mod_add(c.b, c.p);
    return lhs == rhs;
}

}

Curve const& curve(CurveId id)
{
    static Curve const p256 = make_p256();
    static Curve const p384 = make_p384();
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
    if (!on_curve(c, point))
        return std::nullopt;
    return point;
}

bool ecdsa_verify(CurveId id, EcPoint const& public_key, std::span<std::uint8_t const> digest, BigInt const& r, BigInt const& s)
{
    Curve const& c = curve(id);
    if (!on_curve(c, public_key))
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
    BigInt const w = s.mod_inverse_prime(c.n);
    BigInt const u1 = e->mod(c.n).mod_mul(w, c.n);
    BigInt const u2 = r.mod_mul(w, c.n);
    Jacobian const generator { c.gx, c.gy, one };
    Jacobian const sum = ec_add(c, ec_multiply(c, generator, u1), ec_multiply(c, to_jacobian(public_key), u2));
    std::optional<EcPoint> const point = to_affine(c, sum);
    if (!point)
        return false;
    return point->x.mod(c.n) == r;
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
    Curve const& c = curve(id);
    std::optional<BigInt> const k = ecdh_scalar(c, scalar);
    if (!k)
        return std::nullopt;
    Jacobian const generator { c.gx, c.gy, BigInt::from_u64(1) };
    return to_affine(c, ec_multiply_ladder(c, generator, *k));
}

std::optional<std::vector<std::uint8_t>> ecdh_shared_x(CurveId id, std::span<std::uint8_t const> scalar, EcPoint const& peer)
{
    Curve const& c = curve(id);
    if (!on_curve(c, peer))
        return std::nullopt;
    std::optional<BigInt> const k = ecdh_scalar(c, scalar);
    if (!k)
        return std::nullopt;
    std::optional<EcPoint> const product = to_affine(c, ec_multiply_ladder(c, to_jacobian(peer), *k));
    if (!product)
        return std::nullopt;
    return product->x.to_bytes(c.field_bytes);
}

}
