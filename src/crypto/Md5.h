#pragma once
// MD5 (RFC 1321) and RC4. Both are broken and neither is used for security
// anywhere else: they are here because PDF's standard security handler is
// built on them (ISO 32000-1 §7.6.3, revisions 2 to 4), and a reader that
// opens the files every viewer opens must speak it.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sashfold::crypto {

class Md5 {
public:
    static constexpr std::size_t digest_size = 16;
    using Digest = std::array<std::uint8_t, digest_size>;

    Md5();
    void update(std::span<std::uint8_t const> data);
    Digest finish();
    static Digest hash(std::span<std::uint8_t const> data);

private:
    void compress(std::uint8_t const* block);

    std::array<std::uint32_t, 4> m_state {};
    std::array<std::uint8_t, 64> m_buffer {};
    std::size_t m_buffered = 0;
    std::uint64_t m_length = 0; // bytes seen
};

// RC4's keystream XORed over `data`; the key is 1 to 256 bytes (an empty
// key leaves the data as it is).
std::vector<std::uint8_t> rc4(std::span<std::uint8_t const> key, std::span<std::uint8_t const> data);

}
