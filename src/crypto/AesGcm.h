#pragma once
// AES-GCM (NIST SP 800-38D): counter mode over AES with the GHASH
// authenticator. The fixed-size pair is shaped exactly like the
// ChaCha20-Poly1305 AEAD beside it so the TLS record layer can hold either:
// a 96-bit nonce and a 128-bit tag, which is what TLS_AES_128_GCM_SHA256
// uses (RFC 8446 §5.3). The general pair is Web Crypto's AES-GCM: any key
// AES takes, an IV of any length (one that is not 96 bits is hashed into
// the pre-counter block, §7.1 step 2) and a tag of 4 to 16 bytes.
//
// GHASH multiplies bit by bit rather than through a table of multiples of
// the hash subkey: the subkey is secret, and nothing here indexes a table
// with it or branches on it. Nothing here is of our own design.
#include "crypto/Aes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

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

// The ciphertext followed by the tag's first `tag_size` bytes. The IV must
// not be empty and tag_size must be 4 to 16 (the caller checks both).
std::vector<std::uint8_t> aes_gcm_seal(Aes const& aes, std::span<std::uint8_t const> iv, std::span<std::uint8_t const> aad,
    std::span<std::uint8_t const> plaintext, std::size_t tag_size);

// The plaintext of a ciphertext that ends with its tag, or nullopt when the
// input is shorter than the tag or the tag does not verify.
std::optional<std::vector<std::uint8_t>> aes_gcm_open(Aes const& aes, std::span<std::uint8_t const> iv,
    std::span<std::uint8_t const> aad, std::span<std::uint8_t const> sealed, std::size_t tag_size);

}
