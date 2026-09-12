#pragma once
// AES-128-GCM (NIST SP 800-38D): counter mode over AES-128 with the GHASH
// authenticator, shaped exactly like the ChaCha20-Poly1305 AEAD beside it
// so the TLS record layer can hold either. A 96-bit nonce and a 128-bit
// tag, which is what TLS_AES_128_GCM_SHA256 uses (RFC 8446 §5.3).
//
// GHASH multiplies bit by bit rather than through a table of multiples of
// the hash subkey: the subkey is secret, and nothing here indexes a table
// with it or branches on it. Nothing here is of our own design.
#include "crypto/Aes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sashfold::crypto {

using GcmNonce = std::array<std::uint8_t, 12>;
using GcmTag = std::array<std::uint8_t, 16>;

// `ciphertext` receives plaintext.size() bytes (the two spans may be the
// same memory) and the tag is returned.
GcmTag aes128_gcm_seal(AesKey const& key, GcmNonce const& nonce, std::span<std::uint8_t const> aad,
    std::span<std::uint8_t const> plaintext, std::span<std::uint8_t> ciphertext);

// The reverse: false when the tag does not verify, and then nothing is
// written to `plaintext` (which may be the ciphertext's own memory).
bool aes128_gcm_open(AesKey const& key, GcmNonce const& nonce, std::span<std::uint8_t const> aad,
    std::span<std::uint8_t const> ciphertext, GcmTag const& tag, std::span<std::uint8_t> plaintext);

}
