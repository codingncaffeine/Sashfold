#pragma once
// X25519 (RFC 7748 §5): the Diffie-Hellman function over Curve25519 for
// TLS 1.3's key share. The field 2^255 − 19 in five 51-bit limbs, the
// Montgomery ladder with a constant-time conditional swap, the RFC's
// clamping and decoding. Constant time by construction; nothing here is
// of our own design.
#include <array>
#include <cstdint>

namespace sashfold::crypto {

using X25519Key = std::array<std::uint8_t, 32>;

// out = scalar · point (the RFC's X25519(k, u)); false only when the result
// is the all-zero point, which a TLS client must refuse (RFC 8446 §7.4.2).
bool x25519(X25519Key& out, X25519Key const& scalar, X25519Key const& point);

// The public key for a private scalar: X25519(k, 9).
X25519Key x25519_public(X25519Key const& scalar);

}
