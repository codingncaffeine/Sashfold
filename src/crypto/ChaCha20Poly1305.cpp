#include "crypto/ChaCha20Poly1305.h"

#include "crypto/Hmac.h"

#include <bit>
#include <cstring>
#include <vector>

namespace sashfold::crypto {

namespace {

std::uint32_t load32_le(std::uint8_t const* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

void store32_le(std::uint8_t* p, std::uint32_t v)
{
    p[0] = std::uint8_t(v);
    p[1] = std::uint8_t(v >> 8);
    p[2] = std::uint8_t(v >> 16);
    p[3] = std::uint8_t(v >> 24);
}

void store64_le(std::uint8_t* p, std::uint64_t v)
{
    store32_le(p, std::uint32_t(v));
    store32_le(p + 4, std::uint32_t(v >> 32));
}

// §2.1: the quarter round.
void quarter_round(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c, std::uint32_t& d)
{
    a += b;
    d ^= a;
    d = std::rotl(d, 16);
    c += d;
    b ^= c;
    b = std::rotl(b, 12);
    a += b;
    d ^= a;
    d = std::rotl(d, 8);
    c += d;
    b ^= c;
    b = std::rotl(b, 7);
}

// §2.3: the state is the constant "expand 32-byte k", the key, the block
// counter and the nonce, all as little-endian words.
void chacha20_state(std::uint32_t state[16], ChaChaKey const& key, std::uint32_t counter, ChaChaNonce const& nonce)
{
    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;
    for (int i = 0; i < 8; ++i)
        state[4 + i] = load32_le(key.data() + 4 * i);
    state[12] = counter;
    for (int i = 0; i < 3; ++i)
        state[13 + i] = load32_le(nonce.data() + 4 * i);
}

void chacha20_block_words(std::uint32_t const state[16], std::uint32_t out[16])
{
    std::uint32_t x[16];
    for (int i = 0; i < 16; ++i)
        x[i] = state[i];
    for (int round = 0; round < 10; ++round) {
        quarter_round(x[0], x[4], x[8], x[12]);
        quarter_round(x[1], x[5], x[9], x[13]);
        quarter_round(x[2], x[6], x[10], x[14]);
        quarter_round(x[3], x[7], x[11], x[15]);
        quarter_round(x[0], x[5], x[10], x[15]);
        quarter_round(x[1], x[6], x[11], x[12]);
        quarter_round(x[2], x[7], x[8], x[13]);
        quarter_round(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i)
        out[i] = x[i] + state[i];
}

}

std::array<std::uint8_t, 64> chacha20_block(ChaChaKey const& key, std::uint32_t counter, ChaChaNonce const& nonce)
{
    std::uint32_t state[16];
    chacha20_state(state, key, counter, nonce);
    std::uint32_t words[16];
    chacha20_block_words(state, words);
    std::array<std::uint8_t, 64> out;
    for (int i = 0; i < 16; ++i)
        store32_le(out.data() + 4 * i, words[i]);
    return out;
}

void chacha20_xor(ChaChaKey const& key, std::uint32_t counter, ChaChaNonce const& nonce, std::span<std::uint8_t> data)
{
    std::uint32_t state[16];
    chacha20_state(state, key, counter, nonce);
    std::size_t offset = 0;
    while (offset < data.size()) {
        std::uint32_t words[16];
        chacha20_block_words(state, words);
        std::uint8_t keystream[64];
        for (int i = 0; i < 16; ++i)
            store32_le(keystream + 4 * i, words[i]);
        std::size_t const take = std::min<std::size_t>(64, data.size() - offset);
        for (std::size_t i = 0; i < take; ++i)
            data[offset + i] = static_cast<std::uint8_t>(data[offset + i] ^ keystream[i]);
        offset += take;
        state[12] += 1;
    }
}

// §2.5, in the 26-bit-limb form of the public-domain reference: r is
// clamped, every 16-byte block (a partial last one padded with a 1 byte)
// is added to the accumulator and multiplied by r modulo 2^130 − 5, and
// s is added at the end modulo 2^128.
Poly1305Tag poly1305_mac(Poly1305Key const& key, std::span<std::uint8_t const> message)
{
    std::uint32_t const r0 = load32_le(key.data() + 0) & 0x3ffffff;
    std::uint32_t const r1 = (load32_le(key.data() + 3) >> 2) & 0x3ffff03;
    std::uint32_t const r2 = (load32_le(key.data() + 6) >> 4) & 0x3ffc0ff;
    std::uint32_t const r3 = (load32_le(key.data() + 9) >> 6) & 0x3f03fff;
    std::uint32_t const r4 = (load32_le(key.data() + 12) >> 8) & 0x00fffff;
    std::uint32_t const s1 = r1 * 5;
    std::uint32_t const s2 = r2 * 5;
    std::uint32_t const s3 = r3 * 5;
    std::uint32_t const s4 = r4 * 5;
    std::uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    auto absorb = [&](std::uint8_t const* m, std::uint32_t hibit) {
        h0 += load32_le(m + 0) & 0x3ffffff;
        h1 += (load32_le(m + 3) >> 2) & 0x3ffffff;
        h2 += (load32_le(m + 6) >> 4) & 0x3ffffff;
        h3 += (load32_le(m + 9) >> 6) & 0x3ffffff;
        h4 += (load32_le(m + 12) >> 8) | hibit;
        std::uint64_t d0 = std::uint64_t(h0) * r0 + std::uint64_t(h1) * s4 + std::uint64_t(h2) * s3 + std::uint64_t(h3) * s2 + std::uint64_t(h4) * s1;
        std::uint64_t d1 = std::uint64_t(h0) * r1 + std::uint64_t(h1) * r0 + std::uint64_t(h2) * s4 + std::uint64_t(h3) * s3 + std::uint64_t(h4) * s2;
        std::uint64_t d2 = std::uint64_t(h0) * r2 + std::uint64_t(h1) * r1 + std::uint64_t(h2) * r0 + std::uint64_t(h3) * s4 + std::uint64_t(h4) * s3;
        std::uint64_t d3 = std::uint64_t(h0) * r3 + std::uint64_t(h1) * r2 + std::uint64_t(h2) * r1 + std::uint64_t(h3) * r0 + std::uint64_t(h4) * s4;
        std::uint64_t d4 = std::uint64_t(h0) * r4 + std::uint64_t(h1) * r3 + std::uint64_t(h2) * r2 + std::uint64_t(h3) * r1 + std::uint64_t(h4) * r0;
        std::uint32_t c = std::uint32_t(d0 >> 26);
        h0 = std::uint32_t(d0) & 0x3ffffff;
        d1 += c;
        c = std::uint32_t(d1 >> 26);
        h1 = std::uint32_t(d1) & 0x3ffffff;
        d2 += c;
        c = std::uint32_t(d2 >> 26);
        h2 = std::uint32_t(d2) & 0x3ffffff;
        d3 += c;
        c = std::uint32_t(d3 >> 26);
        h3 = std::uint32_t(d3) & 0x3ffffff;
        d4 += c;
        c = std::uint32_t(d4 >> 26);
        h4 = std::uint32_t(d4) & 0x3ffffff;
        h0 += c * 5;
        c = h0 >> 26;
        h0 &= 0x3ffffff;
        h1 += c;
    };

    std::size_t offset = 0;
    for (; offset + 16 <= message.size(); offset += 16)
        absorb(message.data() + offset, 1u << 24);
    if (offset < message.size()) {
        std::uint8_t block[16] = {};
        std::size_t const rest = message.size() - offset;
        std::memcpy(block, message.data() + offset, rest);
        block[rest] = 1;
        absorb(block, 0);
    }

    // Full carry, then h − p selected in constant time when h ≥ p.
    std::uint32_t c = h1 >> 26;
    h1 &= 0x3ffffff;
    h2 += c;
    c = h2 >> 26;
    h2 &= 0x3ffffff;
    h3 += c;
    c = h3 >> 26;
    h3 &= 0x3ffffff;
    h4 += c;
    c = h4 >> 26;
    h4 &= 0x3ffffff;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= 0x3ffffff;
    h1 += c;

    std::uint32_t g0 = h0 + 5;
    c = g0 >> 26;
    g0 &= 0x3ffffff;
    std::uint32_t g1 = h1 + c;
    c = g1 >> 26;
    g1 &= 0x3ffffff;
    std::uint32_t g2 = h2 + c;
    c = g2 >> 26;
    g2 &= 0x3ffffff;
    std::uint32_t g3 = h3 + c;
    c = g3 >> 26;
    g3 &= 0x3ffffff;
    std::uint32_t g4 = h4 + c - (1u << 26);

    std::uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask;
    g1 &= mask;
    g2 &= mask;
    g3 &= mask;
    g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    h0 = (h0 | (h1 << 26)) & 0xffffffff;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff;
    h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;

    std::uint64_t f = std::uint64_t(h0) + load32_le(key.data() + 16);
    h0 = std::uint32_t(f);
    f = std::uint64_t(h1) + load32_le(key.data() + 20) + (f >> 32);
    h1 = std::uint32_t(f);
    f = std::uint64_t(h2) + load32_le(key.data() + 24) + (f >> 32);
    h2 = std::uint32_t(f);
    f = std::uint64_t(h3) + load32_le(key.data() + 28) + (f >> 32);
    h3 = std::uint32_t(f);

    Poly1305Tag tag;
    store32_le(tag.data() + 0, h0);
    store32_le(tag.data() + 4, h1);
    store32_le(tag.data() + 8, h2);
    store32_le(tag.data() + 12, h3);
    return tag;
}

namespace {

// §2.8: the one-time Poly1305 key is the first half of block 0; the MAC
// covers the AAD and the ciphertext, each padded to 16 bytes, then both
// lengths as 64-bit little-endian words.
Poly1305Key poly1305_key_gen(ChaChaKey const& key, ChaChaNonce const& nonce)
{
    std::array<std::uint8_t, 64> const block = chacha20_block(key, 0, nonce);
    Poly1305Key otk;
    std::memcpy(otk.data(), block.data(), otk.size());
    return otk;
}

Poly1305Tag aead_tag(Poly1305Key const& otk, std::span<std::uint8_t const> aad, std::span<std::uint8_t const> ciphertext)
{
    std::vector<std::uint8_t> mac_data;
    mac_data.reserve(aad.size() + ciphertext.size() + 32);
    mac_data.insert(mac_data.end(), aad.begin(), aad.end());
    mac_data.resize((mac_data.size() + 15) / 16 * 16);
    mac_data.insert(mac_data.end(), ciphertext.begin(), ciphertext.end());
    mac_data.resize((mac_data.size() + 15) / 16 * 16);
    std::uint8_t lengths[16];
    store64_le(lengths, aad.size());
    store64_le(lengths + 8, ciphertext.size());
    mac_data.insert(mac_data.end(), lengths, lengths + 16);
    return poly1305_mac(otk, mac_data);
}

}

Poly1305Tag chacha20_poly1305_seal(ChaChaKey const& key, ChaChaNonce const& nonce, std::span<std::uint8_t const> aad,
    std::span<std::uint8_t const> plaintext, std::span<std::uint8_t> ciphertext)
{
    Poly1305Key const otk = poly1305_key_gen(key, nonce);
    if (ciphertext.data() != plaintext.data())
        std::memcpy(ciphertext.data(), plaintext.data(), plaintext.size());
    chacha20_xor(key, 1, nonce, ciphertext.subspan(0, plaintext.size()));
    return aead_tag(otk, aad, ciphertext.subspan(0, plaintext.size()));
}

bool chacha20_poly1305_open(ChaChaKey const& key, ChaChaNonce const& nonce, std::span<std::uint8_t const> aad,
    std::span<std::uint8_t const> ciphertext, Poly1305Tag const& tag, std::span<std::uint8_t> plaintext)
{
    Poly1305Key const otk = poly1305_key_gen(key, nonce);
    Poly1305Tag const expected = aead_tag(otk, aad, ciphertext);
    if (!constant_time_equal(expected, tag))
        return false;
    if (plaintext.data() != ciphertext.data())
        std::memcpy(plaintext.data(), ciphertext.data(), ciphertext.size());
    chacha20_xor(key, 1, nonce, plaintext.subspan(0, ciphertext.size()));
    return true;
}

}
