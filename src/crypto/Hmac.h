#pragma once
// HMAC (RFC 2104, FIPS 198-1) over any of the SHA-2 classes, and the
// constant-time comparison every MAC check uses. An Hmac object is used
// once: finish() leaves it in no state worth reusing.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sashfold::crypto {

template <class Hash>
class Hmac {
public:
    using Digest = typename Hash::Digest;

    explicit Hmac(std::span<std::uint8_t const> key)
    {
        // A key longer than a block is hashed first; a shorter one is
        // padded with zeros to the block size (§2 of the RFC).
        std::array<std::uint8_t, Hash::block_size> block {};
        if (key.size() > Hash::block_size) {
            Digest const shortened = Hash::hash(key);
            std::copy(shortened.begin(), shortened.end(), block.begin());
        } else {
            std::copy(key.begin(), key.end(), block.begin());
        }
        std::array<std::uint8_t, Hash::block_size> ipad;
        std::array<std::uint8_t, Hash::block_size> opad;
        for (std::size_t i = 0; i < Hash::block_size; ++i) {
            ipad[i] = static_cast<std::uint8_t>(block[i] ^ 0x36);
            opad[i] = static_cast<std::uint8_t>(block[i] ^ 0x5c);
        }
        m_inner.update(ipad);
        m_outer.update(opad);
    }

    void update(std::span<std::uint8_t const> data) { m_inner.update(data); }

    Digest finish()
    {
        Digest const inner = m_inner.finish();
        m_outer.update(inner);
        return m_outer.finish();
    }

    static Digest mac(std::span<std::uint8_t const> key, std::span<std::uint8_t const> data)
    {
        Hmac h(key);
        h.update(data);
        return h.finish();
    }

private:
    Hash m_inner;
    Hash m_outer;
};

// True when the two byte strings are equal; the time taken depends on the
// length alone, never on where they first differ. Lengths are not secret.
inline bool constant_time_equal(std::span<std::uint8_t const> a, std::span<std::uint8_t const> b)
{
    if (a.size() != b.size())
        return false;
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        difference = static_cast<std::uint8_t>(difference | (a[i] ^ b[i]));
    return difference == 0;
}

}
