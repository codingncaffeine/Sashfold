#pragma once
// The NIST curves P-256 and P-384 (FIPS 186-4 §D.1.2): point decoding with
// the on-curve check, ECDSA signature verification (§6.4.2), and the ECDH
// of a TLS 1.2 key exchange (RFC 8422), for the servers that will not take
// x25519. The boundary speaks BigInt; underneath, the arithmetic is on
// fixed-width field elements (four 64-bit limbs for P-256, six for P-384)
// in Montgomery form, points in Jacobian coordinates. A signature's values
// are public, so verification takes the faster variable-time path. An ECDH
// scalar is secret: its multiplication runs the same operations and
// touches the same memory whatever the scalar is, over field arithmetic
// that has no branch or index on its data.
#include "crypto/BigInt.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::crypto {

enum class CurveId : std::uint8_t { P256, P384 };

struct Curve {
    CurveId id;
    std::size_t field_bytes; // 32 or 48
    BigInt p; // the prime field
    BigInt a; // −3
    BigInt b;
    BigInt n; // the group order
    BigInt gx;
    BigInt gy;
};

Curve const& curve(CurveId id);

struct EcPoint {
    BigInt x;
    BigInt y;
};

// An uncompressed SEC 1 point, 0x04 || X || Y, each coordinate
// field_bytes long, checked to lie on the curve.
std::optional<EcPoint> ec_decode_point(CurveId id, std::span<std::uint8_t const> encoded);

// The digest is truncated to the order's bit length as §6.4.2 says.
bool ecdsa_verify(CurveId id, EcPoint const& public_key, std::span<std::uint8_t const> digest, BigInt const& r, BigInt const& s);

// 0x04 || X || Y, each coordinate field_bytes long: the uncompressed form
// a key exchange sends.
std::vector<std::uint8_t> ec_encode_point(CurveId id, EcPoint const& point);

// ECDH (RFC 8422 §5.7, SP 800-56A §5.7.1.2). The scalar is field_bytes of
// random, reduced into [1, n − 1]; nullopt for the zero it leaves with
// negligible probability, or, for the shared secret, a peer point that is
// not on the curve or whose product is the point at infinity.
std::optional<EcPoint> ec_public_point(CurveId id, std::span<std::uint8_t const> scalar);
std::optional<std::vector<std::uint8_t>> ecdh_shared_x(CurveId id, std::span<std::uint8_t const> scalar, EcPoint const& peer);

// For the tests and the benchmark only: the same products reached by every
// path the implementation has, so that each can be held against the others
// and against the textbook formulas over BigInt the fixed-width field
// replaced.
namespace ec_test {

enum class Method : std::uint8_t {
    Reference, // the textbook Jacobian formulas over BigInt, double-and-add
    DoubleAndAdd, // the fixed-width field, one bit at a time
    Window, // the variable-time 4-bit window a public key takes in verification
    ConstantTime, // the constant-time window an ECDH peer point takes
    BaseTable, // the generator's precomputed table, variable time (verification)
    BaseTableConstantTime, // the same table read in constant time (a key pair)
};

// k·point for k in [0, n); nullopt for the point at infinity or a point not
// on the curve. The two base-table methods multiply the generator and
// ignore point.
std::optional<EcPoint> multiply(CurveId id, Method method, EcPoint const& point, BigInt const& k);

// point + other on the fixed-width field, through the variable-time or the
// complete constant-time addition; nullopt for the point at infinity.
std::optional<EcPoint> add(CurveId id, EcPoint const& point, EcPoint const& other, bool constant_time);

// count chained field multiplications, a ← a·b mod p, and a field inverse.
BigInt field_multiply_chain(CurveId id, BigInt const& a, BigInt const& b, std::size_t count);
BigInt field_inverse(CurveId id, BigInt const& a);

}

}
