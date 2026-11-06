#include "crypto/Md5.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace sashfold::crypto {

namespace {

constexpr std::array<std::uint32_t, 64> k_sines = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

constexpr std::array<int, 64> k_shifts = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

std::uint32_t rotate_left(std::uint32_t value, int count)
{
    return (value << count) | (value >> (32 - count));
}

} // namespace

Md5::Md5()
    : m_state { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }
{
}

void Md5::compress(std::uint8_t const* block)
{
    std::array<std::uint32_t, 16> words {};
    for (std::size_t i = 0; i < 16; ++i)
        words[i] = static_cast<std::uint32_t>(block[i * 4]) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8)
            | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) | (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
    std::uint32_t a = m_state[0];
    std::uint32_t b = m_state[1];
    std::uint32_t c = m_state[2];
    std::uint32_t d = m_state[3];
    for (std::size_t i = 0; i < 64; ++i) {
        std::uint32_t f = 0;
        std::size_t g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        std::uint32_t const next = d;
        d = c;
        c = b;
        b = b + rotate_left(a + f + k_sines[i] + words[g], k_shifts[i]);
        a = next;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
}

void Md5::update(std::span<std::uint8_t const> data)
{
    m_length += data.size();
    std::size_t at = 0;
    if (m_buffered > 0) {
        std::size_t const take = std::min(data.size(), m_buffer.size() - m_buffered);
        std::memcpy(m_buffer.data() + m_buffered, data.data(), take);
        m_buffered += take;
        at = take;
        if (m_buffered < m_buffer.size())
            return;
        compress(m_buffer.data());
        m_buffered = 0;
    }
    for (; at + 64 <= data.size(); at += 64)
        compress(data.data() + at);
    if (at < data.size()) {
        std::memcpy(m_buffer.data(), data.data() + at, data.size() - at);
        m_buffered = data.size() - at;
    }
}

Md5::Digest Md5::finish()
{
    std::uint64_t const bits = m_length * 8;
    std::uint8_t const one = 0x80;
    update(std::span(&one, 1));
    std::uint8_t const zero = 0;
    while (m_buffered != 56)
        update(std::span(&zero, 1));
    std::array<std::uint8_t, 8> length {};
    for (std::size_t i = 0; i < 8; ++i)
        length[i] = static_cast<std::uint8_t>(bits >> (8 * i));
    update(length);
    Digest digest {};
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j)
            digest[i * 4 + j] = static_cast<std::uint8_t>(m_state[i] >> (8 * j));
    }
    return digest;
}

Md5::Digest Md5::hash(std::span<std::uint8_t const> data)
{
    Md5 md5;
    md5.update(data);
    return md5.finish();
}

std::vector<std::uint8_t> rc4(std::span<std::uint8_t const> key, std::span<std::uint8_t const> data)
{
    std::vector<std::uint8_t> out(data.begin(), data.end());
    if (key.empty())
        return out;
    std::array<std::uint8_t, 256> state {};
    for (std::size_t i = 0; i < 256; ++i)
        state[i] = static_cast<std::uint8_t>(i);
    std::uint8_t j = 0;
    for (std::size_t i = 0; i < 256; ++i) {
        j = static_cast<std::uint8_t>(j + state[i] + key[i % key.size()]);
        std::swap(state[i], state[j]);
    }
    std::uint8_t i = 0;
    j = 0;
    for (std::uint8_t& byte : out) {
        i = static_cast<std::uint8_t>(i + 1);
        j = static_cast<std::uint8_t>(j + state[i]);
        std::swap(state[i], state[j]);
        byte ^= state[static_cast<std::uint8_t>(state[i] + state[j])];
    }
    return out;
}

}
