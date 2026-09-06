#pragma once
// SHA-256, SHA-384 and SHA-512 as FIPS 180-4 writes them: a streaming
// form (update, finish) and one-shot helpers. First consumers: the TLS 1.3
// client's transcript hash and key schedule, HMAC, and the signatures on
// certificates. Nothing here is of our own design; the vectors in
// tests/test_crypto.cpp are the standard's.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sashfold::crypto {

class Sha256 {
public:
    static constexpr std::size_t block_size = 64;
    static constexpr std::size_t digest_size = 32;
    using Digest = std::array<std::uint8_t, digest_size>;

    Sha256();
    void update(std::span<std::uint8_t const> data);
    // Pads, finishes and returns the digest; the object starts over.
    Digest finish();
    static Digest hash(std::span<std::uint8_t const> data);

private:
    void compress(std::uint8_t const* block);
    std::array<std::uint32_t, 8> m_state {};
    std::array<std::uint8_t, block_size> m_buffer {};
    std::size_t m_buffered = 0;
    std::uint64_t m_length = 0; // bytes seen
};

// SHA-512 and SHA-384 share one compression function and differ in the
// initial state and the digest length (FIPS 180-4 §5.3.4, §5.3.5).
class Sha512 {
public:
    static constexpr std::size_t block_size = 128;
    static constexpr std::size_t digest_size = 64;
    using Digest = std::array<std::uint8_t, digest_size>;

    Sha512();
    void update(std::span<std::uint8_t const> data);
    Digest finish();
    static Digest hash(std::span<std::uint8_t const> data);

protected:
    explicit Sha512(std::array<std::uint64_t, 8> const& initial);
    void finish_into(std::uint8_t* out, std::size_t out_size);
    void reset();

private:
    void compress(std::uint8_t const* block);
    std::array<std::uint64_t, 8> const m_initial;
    std::array<std::uint64_t, 8> m_state {};
    std::array<std::uint8_t, block_size> m_buffer {};
    std::size_t m_buffered = 0;
    std::uint64_t m_length = 0; // bytes seen (2^64 bytes is beyond any use here)
};

class Sha384 : private Sha512 {
public:
    static constexpr std::size_t block_size = 128;
    static constexpr std::size_t digest_size = 48;
    using Digest = std::array<std::uint8_t, digest_size>;

    Sha384();
    using Sha512::update;
    Digest finish();
    static Digest hash(std::span<std::uint8_t const> data);
};

// The hash named at runtime, for signature algorithms that carry one.
enum class HashId : std::uint8_t { Sha256, Sha384, Sha512 };
std::size_t digest_size(HashId id);
std::vector<std::uint8_t> hash_with(HashId id, std::span<std::uint8_t const> data);

}
