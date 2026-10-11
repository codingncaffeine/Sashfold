#pragma once
// The other modes Web Crypto names over AES: CBC with PKCS #7 padding and
// CTR (NIST SP 800-38A), and the key wrap of RFC 3394 (SP 800-38F's KW).
// Nothing here is of our own design.
#include "crypto/Aes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::crypto {

// CBC (§6.2) with the padding of RFC 5652 §6.3: the ciphertext is the next
// multiple of 16 bytes above the plaintext. Decryption answers nullopt for
// a length that is not a positive multiple of 16 or padding that is wrong.
std::vector<std::uint8_t> aes_cbc_encrypt(Aes const& aes, AesBlock const& iv, std::span<std::uint8_t const> plaintext);
std::optional<std::vector<std::uint8_t>> aes_cbc_decrypt(Aes const& aes, AesBlock const& iv, std::span<std::uint8_t const> ciphertext);

// CTR (§6.5) as Web Crypto runs it: the rightmost `counter_bits` bits of the
// block (1 to 128) count up and wrap, the rest stay. Nullopt when the data
// needs more blocks than the counter has values, which would reuse one.
std::optional<std::vector<std::uint8_t>> aes_ctr(Aes const& aes, AesBlock const& counter, std::size_t counter_bits,
    std::span<std::uint8_t const> data);

// RFC 3394 §2.2.1 and §2.2.2 with the default IV A6A6A6A6A6A6A6A6: the data
// is a multiple of 8 bytes, at least 16; the wrapped form is 8 bytes longer.
// Unwrapping answers nullopt when the integrity check fails.
std::optional<std::vector<std::uint8_t>> aes_kw_wrap(Aes const& aes, std::span<std::uint8_t const> data);
std::optional<std::vector<std::uint8_t>> aes_kw_unwrap(Aes const& aes, std::span<std::uint8_t const> wrapped);

}
