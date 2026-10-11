#include "crypto/Sha1.h"

#include <algorithm>
#include <cstring>

namespace sashfold::crypto {

namespace {

std::uint32_t rotl(std::uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32 - n));
}

}

Sha1::Sha1()
    : m_state { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u }
{
}

void Sha1::compress(std::uint8_t const* block)
{
    std::uint32_t w[80];
    for (std::size_t t = 0; t < 16; ++t)
        w[t] = (std::uint32_t { block[4 * t] } << 24) | (std::uint32_t { block[4 * t + 1] } << 16)
            | (std::uint32_t { block[4 * t + 2] } << 8) | std::uint32_t { block[4 * t + 3] };
    for (std::size_t t = 16; t < 80; ++t)
        w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], e = m_state[4];
    for (std::size_t t = 0; t < 80; ++t) {
        std::uint32_t f;
        std::uint32_t k;
        if (t < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999u;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        std::uint32_t const temp = rotl(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
}

void Sha1::update(std::span<std::uint8_t const> data)
{
    m_length += data.size();
    std::size_t offset = 0;
    if (m_buffered > 0) {
        std::size_t const take = std::min(block_size - m_buffered, data.size());
        std::memcpy(m_buffer.data() + m_buffered, data.data(), take);
        m_buffered += take;
        offset = take;
        if (m_buffered < block_size)
            return;
        compress(m_buffer.data());
        m_buffered = 0;
    }
    for (; offset + block_size <= data.size(); offset += block_size)
        compress(data.data() + offset);
    if (offset < data.size()) {
        m_buffered = data.size() - offset;
        std::memcpy(m_buffer.data(), data.data() + offset, m_buffered);
    }
}

Sha1::Digest Sha1::finish()
{
    std::uint64_t const bits = m_length * 8;
    std::uint8_t padding[block_size + 8] = { 0x80 };
    std::size_t const pad = (m_buffered < 56 ? 56 - m_buffered : 120 - m_buffered);
    for (std::size_t i = 0; i < 8; ++i)
        padding[pad + i] = static_cast<std::uint8_t>(bits >> (8 * (7 - i)));
    update(std::span<std::uint8_t const>(padding, pad + 8));
    Digest out {};
    for (std::size_t i = 0; i < 5; ++i)
        for (std::size_t j = 0; j < 4; ++j)
            out[4 * i + j] = static_cast<std::uint8_t>(m_state[i] >> (8 * (3 - j)));
    return out;
}

Sha1::Digest Sha1::hash(std::span<std::uint8_t const> data)
{
    Sha1 h;
    h.update(data);
    return h.finish();
}

}
