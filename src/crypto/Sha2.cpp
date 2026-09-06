#include "crypto/Sha2.h"

#include <bit>
#include <cstring>

namespace sashfold::crypto {

namespace {

// FIPS 180-4 §4.2.2: the first thirty-two bits of the fractional parts of
// the cube roots of the first sixty-four primes.
constexpr std::uint32_t k256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// §4.2.3: the first sixty-four bits of the same fractional parts for the
// first eighty primes.
constexpr std::uint64_t k512[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc,
    0x3956c25bf348b538, 0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118,
    0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694,
    0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4,
    0xc6e00bf33da88fc2, 0xd5a79147930aa725, 0x06ca6351e003826f, 0x142929670a0e6e70,
    0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30,
    0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8,
    0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3,
    0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b,
    0xca273eceea26619c, 0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178,
    0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c,
    0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
};

constexpr std::array<std::uint32_t, 8> h256_initial = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};
constexpr std::array<std::uint64_t, 8> h512_initial = {
    0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
    0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179,
};
constexpr std::array<std::uint64_t, 8> h384_initial = {
    0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
    0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4,
};

std::uint32_t load32(std::uint8_t const* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

std::uint64_t load64(std::uint8_t const* p)
{
    return (std::uint64_t(load32(p)) << 32) | std::uint64_t(load32(p + 4));
}

void store32(std::uint8_t* p, std::uint32_t v)
{
    p[0] = std::uint8_t(v >> 24);
    p[1] = std::uint8_t(v >> 16);
    p[2] = std::uint8_t(v >> 8);
    p[3] = std::uint8_t(v);
}

void store64(std::uint8_t* p, std::uint64_t v)
{
    store32(p, std::uint32_t(v >> 32));
    store32(p + 4, std::uint32_t(v));
}

}

// ---- SHA-256 (§6.2)

Sha256::Sha256()
    : m_state(h256_initial)
{
}

void Sha256::compress(std::uint8_t const* block)
{
    std::uint32_t w[64];
    for (int t = 0; t < 16; ++t)
        w[t] = load32(block + 4 * t);
    for (int t = 16; t < 64; ++t) {
        std::uint32_t const s0 = std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        std::uint32_t const s1 = std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
    for (int t = 0; t < 64; ++t) {
        std::uint32_t const big_s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        std::uint32_t const ch = (e & f) ^ (~e & g);
        std::uint32_t const t1 = h + big_s1 + ch + k256[t] + w[t];
        std::uint32_t const big_s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        std::uint32_t const maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t const t2 = big_s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::update(std::span<std::uint8_t const> data)
{
    if (data.empty())
        return;
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

Sha256::Digest Sha256::finish()
{
    // §5.1.1: a 1 bit, zeros to 56 mod 64, then the bit length in 64 bits.
    std::uint64_t const bits = m_length * 8;
    std::uint8_t const one = 0x80;
    update(std::span<std::uint8_t const>(&one, 1));
    std::uint8_t const zero = 0;
    while (m_buffered != 56)
        update(std::span<std::uint8_t const>(&zero, 1));
    std::uint8_t length[8];
    store64(length, bits);
    update(length);
    Digest digest;
    for (int i = 0; i < 8; ++i)
        store32(digest.data() + 4 * i, m_state[i]);
    m_state = h256_initial;
    m_buffered = 0;
    m_length = 0;
    return digest;
}

Sha256::Digest Sha256::hash(std::span<std::uint8_t const> data)
{
    Sha256 h;
    h.update(data);
    return h.finish();
}

// ---- SHA-512 and SHA-384 (§6.4, §6.5)

Sha512::Sha512()
    : Sha512(h512_initial)
{
}

Sha512::Sha512(std::array<std::uint64_t, 8> const& initial)
    : m_initial(initial)
    , m_state(initial)
{
}

void Sha512::reset()
{
    m_state = m_initial;
    m_buffered = 0;
    m_length = 0;
}

void Sha512::compress(std::uint8_t const* block)
{
    std::uint64_t w[80];
    for (int t = 0; t < 16; ++t)
        w[t] = load64(block + 8 * t);
    for (int t = 16; t < 80; ++t) {
        std::uint64_t const s0 = std::rotr(w[t - 15], 1) ^ std::rotr(w[t - 15], 8) ^ (w[t - 15] >> 7);
        std::uint64_t const s1 = std::rotr(w[t - 2], 19) ^ std::rotr(w[t - 2], 61) ^ (w[t - 2] >> 6);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    std::uint64_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    std::uint64_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
    for (int t = 0; t < 80; ++t) {
        std::uint64_t const big_s1 = std::rotr(e, 14) ^ std::rotr(e, 18) ^ std::rotr(e, 41);
        std::uint64_t const ch = (e & f) ^ (~e & g);
        std::uint64_t const t1 = h + big_s1 + ch + k512[t] + w[t];
        std::uint64_t const big_s0 = std::rotr(a, 28) ^ std::rotr(a, 34) ^ std::rotr(a, 39);
        std::uint64_t const maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint64_t const t2 = big_s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha512::update(std::span<std::uint8_t const> data)
{
    if (data.empty())
        return;
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

void Sha512::finish_into(std::uint8_t* out, std::size_t out_size)
{
    // §5.1.2: a 1 bit, zeros to 112 mod 128, then the bit length in 128
    // bits (the high 64 hold what 2^64 bytes would need — nothing here).
    std::uint64_t const bytes = m_length;
    std::uint8_t const one = 0x80;
    update(std::span<std::uint8_t const>(&one, 1));
    std::uint8_t const zero = 0;
    while (m_buffered != 112)
        update(std::span<std::uint8_t const>(&zero, 1));
    std::uint8_t length[16];
    store64(length, bytes >> 61);
    store64(length + 8, bytes << 3);
    update(length);
    std::uint8_t full[64];
    for (int i = 0; i < 8; ++i)
        store64(full + 8 * i, m_state[i]);
    std::memcpy(out, full, out_size);
    reset();
}

Sha512::Digest Sha512::finish()
{
    Digest digest;
    finish_into(digest.data(), digest.size());
    return digest;
}

Sha512::Digest Sha512::hash(std::span<std::uint8_t const> data)
{
    Sha512 h;
    h.update(data);
    return h.finish();
}

Sha384::Sha384()
    : Sha512(h384_initial)
{
}

Sha384::Digest Sha384::finish()
{
    Digest digest;
    finish_into(digest.data(), digest.size());
    return digest;
}

Sha384::Digest Sha384::hash(std::span<std::uint8_t const> data)
{
    Sha384 h;
    h.update(data);
    return h.finish();
}

// ---- by name

std::size_t digest_size(HashId id)
{
    switch (id) {
    case HashId::Sha256:
        return Sha256::digest_size;
    case HashId::Sha384:
        return Sha384::digest_size;
    case HashId::Sha512:
        return Sha512::digest_size;
    }
    return 0;
}

std::vector<std::uint8_t> hash_with(HashId id, std::span<std::uint8_t const> data)
{
    switch (id) {
    case HashId::Sha256: {
        Sha256::Digest const d = Sha256::hash(data);
        return std::vector<std::uint8_t>(d.begin(), d.end());
    }
    case HashId::Sha384: {
        Sha384::Digest const d = Sha384::hash(data);
        return std::vector<std::uint8_t>(d.begin(), d.end());
    }
    case HashId::Sha512: {
        Sha512::Digest const d = Sha512::hash(data);
        return std::vector<std::uint8_t>(d.begin(), d.end());
    }
    }
    return {};
}

}
