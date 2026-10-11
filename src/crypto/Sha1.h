#pragma once
// SHA-1 (FIPS 180-4 §6.1). Broken for collision resistance and kept only
// because Web Crypto still names it — for HMAC, PBKDF2, HKDF, RSA and
// digest() — so pages that ask for it get the answer every browser gives.
// Nothing in the TLS stack accepts it.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sashfold::crypto {

class Sha1 {
public:
    static constexpr std::size_t block_size = 64;
    static constexpr std::size_t digest_size = 20;
    using Digest = std::array<std::uint8_t, digest_size>;

    Sha1();
    void update(std::span<std::uint8_t const> data);
    Digest finish();
    static Digest hash(std::span<std::uint8_t const> data);

private:
    void compress(std::uint8_t const* block);

    std::array<std::uint32_t, 5> m_state {};
    std::array<std::uint8_t, block_size> m_buffer {};
    std::size_t m_buffered = 0;
    std::uint64_t m_length = 0; // bytes seen
};

}
