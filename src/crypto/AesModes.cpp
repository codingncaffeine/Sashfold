#include "crypto/AesModes.h"

#include <cstring>

namespace sashfold::crypto {

std::vector<std::uint8_t> aes_cbc_encrypt(Aes const& aes, AesBlock const& iv, std::span<std::uint8_t const> plaintext)
{
    std::size_t const padding = 16 - plaintext.size() % 16;
    std::vector<std::uint8_t> padded(plaintext.begin(), plaintext.end());
    padded.insert(padded.end(), padding, static_cast<std::uint8_t>(padding));
    AesBlock chain = iv;
    for (std::size_t offset = 0; offset < padded.size(); offset += 16) {
        AesBlock block {};
        for (std::size_t i = 0; i < 16; ++i)
            block[i] = static_cast<std::uint8_t>(padded[offset + i] ^ chain[i]);
        chain = aes.encrypt(block);
        std::memcpy(padded.data() + offset, chain.data(), 16);
    }
    return padded;
}

// The padding is checked without a branch on its bytes: every one of the
// last 16 is compared under a mask of whether it falls in the padding.
std::optional<std::vector<std::uint8_t>> aes_cbc_decrypt(Aes const& aes, AesBlock const& iv, std::span<std::uint8_t const> ciphertext)
{
    if (ciphertext.empty() || ciphertext.size() % 16 != 0)
        return std::nullopt;
    std::vector<std::uint8_t> out(ciphertext.size());
    AesBlock chain = iv;
    for (std::size_t offset = 0; offset < ciphertext.size(); offset += 16) {
        AesBlock block {};
        std::memcpy(block.data(), ciphertext.data() + offset, 16);
        AesBlock const plain = aes.decrypt(block);
        for (std::size_t i = 0; i < 16; ++i)
            out[offset + i] = static_cast<std::uint8_t>(plain[i] ^ chain[i]);
        chain = block;
    }
    unsigned const padding = out.back();
    unsigned bad = static_cast<unsigned>(padding == 0) | static_cast<unsigned>(padding > 16);
    for (unsigned i = 1; i <= 16; ++i) {
        unsigned const in_padding = static_cast<unsigned>(i <= padding);
        bad |= in_padding & static_cast<unsigned>(out[out.size() - i] != padding);
    }
    if (bad != 0)
        return std::nullopt;
    out.resize(out.size() - padding);
    return out;
}

std::optional<std::vector<std::uint8_t>> aes_ctr(Aes const& aes, AesBlock const& counter, std::size_t counter_bits,
    std::span<std::uint8_t const> data)
{
    if (counter_bits == 0 || counter_bits > 128)
        return std::nullopt;
    std::size_t const blocks = (data.size() + 15) / 16;
    if (counter_bits < 64 && blocks > (std::size_t { 1 } << counter_bits))
        return std::nullopt;
    std::vector<std::uint8_t> out(data.begin(), data.end());
    AesBlock block = counter;
    for (std::size_t offset = 0; offset < out.size(); offset += 16) {
        AesBlock const keystream = aes.encrypt(block);
        std::size_t const take = out.size() - offset < 16 ? out.size() - offset : 16;
        for (std::size_t i = 0; i < take; ++i)
            out[offset + i] = static_cast<std::uint8_t>(out[offset + i] ^ keystream[i]);
        // Add one to the low counter_bits bits; a carry out of them is lost.
        unsigned carry = 1;
        for (std::size_t bit_base = 0; bit_base < counter_bits && carry != 0; bit_base += 8) {
            std::size_t const index = 15 - bit_base / 8;
            std::size_t const width = counter_bits - bit_base < 8 ? counter_bits - bit_base : 8;
            unsigned const mask = (1u << width) - 1u;
            unsigned const field = (block[index] & mask) + carry;
            carry = field >> width;
            block[index] = static_cast<std::uint8_t>((block[index] & ~mask) | (field & mask));
        }
    }
    return out;
}

namespace {

constexpr std::uint8_t default_iv[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };

}

std::optional<std::vector<std::uint8_t>> aes_kw_wrap(Aes const& aes, std::span<std::uint8_t const> data)
{
    if (data.size() < 16 || data.size() % 8 != 0)
        return std::nullopt;
    std::size_t const n = data.size() / 8;
    std::vector<std::uint8_t> r(data.begin(), data.end());
    std::uint8_t a[8];
    std::memcpy(a, default_iv, 8);
    for (std::size_t j = 0; j < 6; ++j) {
        for (std::size_t i = 0; i < n; ++i) {
            AesBlock block {};
            std::memcpy(block.data(), a, 8);
            std::memcpy(block.data() + 8, r.data() + 8 * i, 8);
            AesBlock const b = aes.encrypt(block);
            std::uint64_t const t = static_cast<std::uint64_t>(n * j + i + 1);
            for (std::size_t k = 0; k < 8; ++k)
                a[k] = static_cast<std::uint8_t>(b[k] ^ static_cast<std::uint8_t>(t >> (8 * (7 - k))));
            std::memcpy(r.data() + 8 * i, b.data() + 8, 8);
        }
    }
    std::vector<std::uint8_t> out(a, a + 8);
    out.insert(out.end(), r.begin(), r.end());
    return out;
}

std::optional<std::vector<std::uint8_t>> aes_kw_unwrap(Aes const& aes, std::span<std::uint8_t const> wrapped)
{
    if (wrapped.size() < 24 || wrapped.size() % 8 != 0)
        return std::nullopt;
    std::size_t const n = wrapped.size() / 8 - 1;
    std::uint8_t a[8];
    std::memcpy(a, wrapped.data(), 8);
    std::vector<std::uint8_t> r(wrapped.begin() + 8, wrapped.end());
    for (std::size_t j = 6; j-- > 0;) {
        for (std::size_t i = n; i-- > 0;) {
            std::uint64_t const t = static_cast<std::uint64_t>(n * j + i + 1);
            AesBlock block {};
            for (std::size_t k = 0; k < 8; ++k)
                block[k] = static_cast<std::uint8_t>(a[k] ^ static_cast<std::uint8_t>(t >> (8 * (7 - k))));
            std::memcpy(block.data() + 8, r.data() + 8 * i, 8);
            AesBlock const b = aes.decrypt(block);
            std::memcpy(a, b.data(), 8);
            std::memcpy(r.data() + 8 * i, b.data() + 8, 8);
        }
    }
    unsigned difference = 0;
    for (std::size_t k = 0; k < 8; ++k)
        difference |= static_cast<unsigned>(a[k] ^ default_iv[k]);
    if (difference != 0)
        return std::nullopt;
    return r;
}

}
