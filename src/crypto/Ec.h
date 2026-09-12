#pragma once
// The NIST curves P-256 and P-384 (FIPS 186-4 §D.1.2) over BigInt: point
// decoding with the on-curve check, arithmetic in Jacobian coordinates,
// ECDSA signature verification (§6.4.2), and the ECDH of a TLS 1.2 key
// exchange over P-256 (RFC 8422), for the servers that will not take
// x25519. A signature's values are public and its arithmetic is not
// constant time. An ECDH scalar is secret: its multiplication is a
// Montgomery ladder, the same doubling and addition at every bit whatever
// the bit is, over the same field arithmetic — which is itself not
// constant time in its data. A fixed-width field implementation for the
// two curves is the way to close that, and is not written.
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

}
