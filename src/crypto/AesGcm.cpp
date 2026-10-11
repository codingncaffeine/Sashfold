#include "crypto/AesGcm.h"

#include "crypto/Hmac.h"

#include <cstring>
#include <utility>

namespace sashfold::crypto {

namespace {

// GF(2^128) as SP 800-38D §6.3 writes it: bit 0 of byte 0 is the leading
// coefficient, so the product is formed from the top bit down and the
// reduction by x^128 + x^7 + x^2 + x + 1 appears as a right shift feeding
// 0xe1 back into the first byte. The loop runs 128 times whatever the
// operands are, and the conditional XORs are masks rather than branches.
void gf128_multiply(AesBlock& accumulator, AesBlock const& subkey)
{
    std::uint8_t product[16] = {};
    std::uint8_t v[16];
    std::memcpy(v, subkey.data(), sizeof v);
    for (std::size_t bit = 0; bit < 128; ++bit) {
        std::uint8_t const selected = static_cast<std::uint8_t>((accumulator[bit / 8] >> (7 - (bit % 8))) & 1u);
        std::uint8_t const mask = static_cast<std::uint8_t>(0u - static_cast<unsigned>(selected));
        for (std::size_t i = 0; i < 16; ++i)
            product[i] = static_cast<std::uint8_t>(product[i] ^ (v[i] & mask));
        std::uint8_t const carry = static_cast<std::uint8_t>(v[15] & 1u);
        std::uint8_t const feedback = static_cast<std::uint8_t>(0u - static_cast<unsigned>(carry));
        for (std::size_t i = 15; i > 0; --i)
            v[i] = static_cast<std::uint8_t>((v[i] >> 1) | static_cast<std::uint8_t>((v[i - 1] & 1u) << 7));
        v[0] = static_cast<std::uint8_t>((v[0] >> 1) ^ (0xe1u & feedback));
    }
    std::memcpy(accumulator.data(), product, sizeof product);
}

// The authenticator of §6.4: every block is XORed into the accumulator and
// multiplied by the hash subkey; a short final block is zero-padded.
class Ghash {
public:
    explicit Ghash(AesBlock const& subkey)
        : m_subkey(subkey)
    {
    }

    void update(std::span<std::uint8_t const> data)
    {
        std::size_t offset = 0;
        while (offset < data.size()) {
            std::size_t const take = data.size() - offset < 16 ? data.size() - offset : 16;
            std::uint8_t block[16] = {};
            std::memcpy(block, data.data() + offset, take);
            absorb(block);
            offset += take;
        }
    }

    // The lengths block of §6.4: both lengths in bits, 64 bits each.
    AesBlock finish(std::size_t aad_size, std::size_t ciphertext_size)
    {
        std::uint8_t block[16] = {};
        std::uint64_t const aad_bits = static_cast<std::uint64_t>(aad_size) * 8;
        std::uint64_t const ciphertext_bits = static_cast<std::uint64_t>(ciphertext_size) * 8;
        for (std::size_t i = 0; i < 8; ++i) {
            block[i] = static_cast<std::uint8_t>(aad_bits >> (8 * (7 - i)));
            block[8 + i] = static_cast<std::uint8_t>(ciphertext_bits >> (8 * (7 - i)));
        }
        absorb(block);
        return m_accumulator;
    }

private:
    void absorb(std::uint8_t const* block)
    {
        for (std::size_t i = 0; i < 16; ++i)
            m_accumulator[i] = static_cast<std::uint8_t>(m_accumulator[i] ^ block[i]);
        gf128_multiply(m_accumulator, m_subkey);
    }

    AesBlock m_subkey;
    AesBlock m_accumulator {};
};

// §7.1 step 2: with a 96-bit IV the pre-counter block J0 is the IV and a
// one; any other IV is hashed, zero-padded, with its length in bits.
AesBlock pre_counter_block(AesBlock const& subkey, std::span<std::uint8_t const> iv)
{
    AesBlock block {};
    if (iv.size() == 12) {
        std::memcpy(block.data(), iv.data(), iv.size());
        block[15] = 1;
        return block;
    }
    Ghash ghash(subkey);
    ghash.update(iv);
    return ghash.finish(0, iv.size());
}

// inc32 (§6.2): the last four bytes count, the first twelve stay.
AesBlock increment32(AesBlock block, std::uint32_t by)
{
    std::uint32_t counter = 0;
    for (std::size_t i = 12; i < 16; ++i)
        counter = (counter << 8) | block[i];
    counter += by;
    for (std::size_t i = 0; i < 4; ++i)
        block[12 + i] = static_cast<std::uint8_t>(counter >> (8 * (3 - i)));
    return block;
}

// GCTR from inc32(J0): the keystream of the data.
void counter_xor(Aes const& aes, AesBlock const& j0, std::span<std::uint8_t> data)
{
    AesBlock counter = increment32(j0, 1);
    std::size_t offset = 0;
    while (offset < data.size()) {
        AesBlock const keystream = aes.encrypt(counter);
        std::size_t const take = data.size() - offset < 16 ? data.size() - offset : 16;
        for (std::size_t i = 0; i < take; ++i)
            data[offset + i] = static_cast<std::uint8_t>(data[offset + i] ^ keystream[i]);
        offset += take;
        counter = increment32(counter, 1);
    }
}

GcmTag authenticate(Aes const& aes, AesBlock const& subkey, AesBlock const& j0,
    std::span<std::uint8_t const> aad, std::span<std::uint8_t const> ciphertext)
{
    Ghash ghash(subkey);
    ghash.update(aad);
    ghash.update(ciphertext);
    AesBlock const hash = ghash.finish(aad.size(), ciphertext.size());
    AesBlock const mask = aes.encrypt(j0);
    GcmTag tag {};
    for (std::size_t i = 0; i < tag.size(); ++i)
        tag[i] = static_cast<std::uint8_t>(hash[i] ^ mask[i]);
    return tag;
}

}

GcmTag aes128_gcm_seal(AesKey const& key, GcmNonce const& nonce, std::span<std::uint8_t const> aad,
    std::span<std::uint8_t const> plaintext, std::span<std::uint8_t> ciphertext)
{
    Aes128 const aes(key);
    AesBlock const subkey = aes.encrypt(AesBlock {});
    AesBlock const j0 = pre_counter_block(subkey, nonce);
    if (ciphertext.data() != plaintext.data() && !plaintext.empty())
        std::memcpy(ciphertext.data(), plaintext.data(), plaintext.size());
    std::span<std::uint8_t> const body = ciphertext.subspan(0, plaintext.size());
    counter_xor(aes, j0, body);
    return authenticate(aes, subkey, j0, aad, body);
}

bool aes128_gcm_open(AesKey const& key, GcmNonce const& nonce, std::span<std::uint8_t const> aad,
    std::span<std::uint8_t const> ciphertext, GcmTag const& tag, std::span<std::uint8_t> plaintext)
{
    Aes128 const aes(key);
    AesBlock const subkey = aes.encrypt(AesBlock {});
    AesBlock const j0 = pre_counter_block(subkey, nonce);
    GcmTag const expected = authenticate(aes, subkey, j0, aad, ciphertext);
    if (!constant_time_equal(expected, tag))
        return false;
    if (plaintext.data() != ciphertext.data() && !ciphertext.empty())
        std::memcpy(plaintext.data(), ciphertext.data(), ciphertext.size());
    counter_xor(aes, j0, plaintext.subspan(0, ciphertext.size()));
    return true;
}

std::vector<std::uint8_t> aes_gcm_seal(Aes const& aes, std::span<std::uint8_t const> iv, std::span<std::uint8_t const> aad,
    std::span<std::uint8_t const> plaintext, std::size_t tag_size)
{
    AesBlock const subkey = aes.encrypt(AesBlock {});
    AesBlock const j0 = pre_counter_block(subkey, iv);
    std::vector<std::uint8_t> out(plaintext.begin(), plaintext.end());
    counter_xor(aes, j0, out);
    GcmTag const tag = authenticate(aes, subkey, j0, aad, out);
    out.insert(out.end(), tag.begin(), tag.begin() + static_cast<std::ptrdiff_t>(tag_size));
    return out;
}

std::optional<std::vector<std::uint8_t>> aes_gcm_open(Aes const& aes, std::span<std::uint8_t const> iv,
    std::span<std::uint8_t const> aad, std::span<std::uint8_t const> sealed, std::size_t tag_size)
{
    if (sealed.size() < tag_size)
        return std::nullopt;
    std::span<std::uint8_t const> const ciphertext = sealed.subspan(0, sealed.size() - tag_size);
    AesBlock const subkey = aes.encrypt(AesBlock {});
    AesBlock const j0 = pre_counter_block(subkey, iv);
    GcmTag const expected = authenticate(aes, subkey, j0, aad, ciphertext);
    if (!constant_time_equal(std::span<std::uint8_t const>(expected.data(), tag_size), sealed.subspan(ciphertext.size())))
        return std::nullopt;
    std::vector<std::uint8_t> out(ciphertext.begin(), ciphertext.end());
    counter_xor(aes, j0, out);
    return out;
}

}
