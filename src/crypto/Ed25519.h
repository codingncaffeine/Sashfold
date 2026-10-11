#pragma once
// Ed25519 (RFC 8032 §5.1), the signature scheme Web Crypto names: a key
// pair from a 32-byte seed, signing, and verification as the browsers run
// it — cofactorless, S below L, encodings canonical, and a public key or R
// of small order refused (WPT's WebCryptoAPI small-order cases).
//
// The secret scalar multiplies the base point by a ladder that adds and
// selects under masks, the same steps for every scalar; verification works
// on public values only and takes the variable-time path.
#include <array>
#include <cstdint>
#include <span>

namespace sashfold::crypto {

using Ed25519Seed = std::array<std::uint8_t, 32>;
using Ed25519PublicKey = std::array<std::uint8_t, 32>;
using Ed25519Signature = std::array<std::uint8_t, 64>;

Ed25519PublicKey ed25519_public_key(Ed25519Seed const& seed);
Ed25519Signature ed25519_sign(Ed25519Seed const& seed, std::span<std::uint8_t const> message);
bool ed25519_verify(Ed25519PublicKey const& key, std::span<std::uint8_t const> message, std::span<std::uint8_t const> signature);

// Whether 32 bytes decode to a point of the curve (§5.1.3), for import.
bool ed25519_point_valid(Ed25519PublicKey const& key);

}
