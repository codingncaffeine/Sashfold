#pragma once
// The NIST curves P-256 and P-384 (FIPS 186-4 §D.1.2) over BigInt: point
// decoding with the on-curve check, arithmetic in Jacobian coordinates,
// and ECDSA signature verification (§6.4.2). Every value here is public —
// a key, a signature, a digest — so nothing is constant time; the curves
// do not serve key exchange here (TLS uses X25519 for that).
#include "crypto/BigInt.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

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

}
