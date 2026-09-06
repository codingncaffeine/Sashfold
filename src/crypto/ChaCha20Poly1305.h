#pragma once
// ChaCha20 (RFC 8439 §2.4), Poly1305 (§2.5) and their AEAD construction
// (§2.8), as the RFC writes them: the record protection of
// TLS_CHACHA20_POLY1305_SHA256. Constant time by construction — no table
// indexed by a secret, no branch on one — and the tag is compared with
// constant_time_equal. Nothing here is of our own design.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sashfold::crypto {

using ChaChaKey = std::array<std::uint8_t, 32>;
using ChaChaNonce = std::array<std::uint8_t, 12>;
using Poly1305Key = std::array<std::uint8_t, 32>;
using Poly1305Tag = std::array<std::uint8_t, 16>;

// One 64-byte block of keystream for `counter` (the RFC's block function).
std::array<std::uint8_t, 64> chacha20_block(ChaChaKey const& key, std::uint32_t counter, ChaChaNonce const& nonce);

// The keystream from block `counter` on, xor'd over `data` in place: the
// same call encrypts and decrypts.
void chacha20_xor(ChaChaKey const& key, std::uint32_t counter, ChaChaNonce const& nonce, std::span<std::uint8_t> data);

Poly1305Tag poly1305_mac(Poly1305Key const& key, std::span<std::uint8_t const> message);

// AEAD_CHACHA20_POLY1305: `ciphertext` receives plaintext.size() bytes
// (the two spans may be the same memory) and the tag is returned.
Poly1305Tag chacha20_poly1305_seal(ChaChaKey const& key, ChaChaNonce const& nonce, std::span<std::uint8_t const> aad,
    std::span<std::uint8_t const> plaintext, std::span<std::uint8_t> ciphertext);

// The reverse: false when the tag does not verify, and then nothing is
// written to `plaintext` (which may be the ciphertext's own memory).
bool chacha20_poly1305_open(ChaChaKey const& key, ChaChaNonce const& nonce, std::span<std::uint8_t const> aad,
    std::span<std::uint8_t const> ciphertext, Poly1305Tag const& tag, std::span<std::uint8_t> plaintext);

}
