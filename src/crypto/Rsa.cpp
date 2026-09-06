#include "crypto/Rsa.h"

#include <vector>

namespace sashfold::crypto {

namespace {

// The DER DigestInfo prefixes of §9.2 note 1, one per hash.
std::span<std::uint8_t const> digest_info_prefix(HashId hash)
{
    static constexpr std::uint8_t sha256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    static constexpr std::uint8_t sha384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
    static constexpr std::uint8_t sha512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };
    switch (hash) {
    case HashId::Sha256:
        return sha256;
    case HashId::Sha384:
        return sha384;
    case HashId::Sha512:
        return sha512;
    }
    return {};
}

// RSAVP1 (§5.2.2) followed by I2OSP to `size` bytes; nullopt when the
// signature is out of range.
std::optional<std::vector<std::uint8_t>> encoded_message(RsaPublicKey const& key, std::span<std::uint8_t const> signature, std::size_t size)
{
    std::optional<BigInt> const s = BigInt::from_bytes(signature);
    if (!s || s->compare(key.n) >= 0)
        return std::nullopt;
    return s->mod_pow(key.e, key.n).to_bytes(size);
}

// MGF1 (§B.2.1): Hash(seed || counter) for counter = 0, 1, … until `length` bytes.
std::vector<std::uint8_t> mgf1(HashId hash, std::span<std::uint8_t const> seed, std::size_t length)
{
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> input(seed.begin(), seed.end());
    input.resize(seed.size() + 4);
    for (std::uint32_t counter = 0; out.size() < length; ++counter) {
        input[seed.size()] = static_cast<std::uint8_t>(counter >> 24);
        input[seed.size() + 1] = static_cast<std::uint8_t>(counter >> 16);
        input[seed.size() + 2] = static_cast<std::uint8_t>(counter >> 8);
        input[seed.size() + 3] = static_cast<std::uint8_t>(counter);
        std::vector<std::uint8_t> const block = hash_with(hash, input);
        out.insert(out.end(), block.begin(), block.end());
    }
    out.resize(length);
    return out;
}

}

bool rsa_key_acceptable(RsaPublicKey const& key)
{
    std::size_t const bits = key.n.bit_length();
    if (bits < 2048 || bits > BigInt::max_bits / 2)
        return false;
    if (!key.n.is_odd())
        return false;
    if (!key.e.is_odd() || key.e.bit_length() > 64 || key.e.compare(BigInt::from_u64(3)) < 0)
        return false;
    return true;
}

bool rsa_verify_pkcs1_v15(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature)
{
    if (!rsa_key_acceptable(key) || digest.size() != digest_size(hash))
        return false;
    std::size_t const k = (key.n.bit_length() + 7) / 8;
    if (signature.size() != k)
        return false;
    std::optional<std::vector<std::uint8_t>> const em = encoded_message(key, signature, k);
    if (!em)
        return false;
    // EM = 0x00 || 0x01 || PS (at least eight 0xff) || 0x00 || DigestInfo
    std::span<std::uint8_t const> const prefix = digest_info_prefix(hash);
    std::size_t const t_length = prefix.size() + digest.size();
    if (k < t_length + 11)
        return false;
    std::vector<std::uint8_t> expected;
    expected.reserve(k);
    expected.push_back(0x00);
    expected.push_back(0x01);
    expected.insert(expected.end(), k - t_length - 3, 0xff);
    expected.push_back(0x00);
    expected.insert(expected.end(), prefix.begin(), prefix.end());
    expected.insert(expected.end(), digest.begin(), digest.end());
    return *em == expected;
}

bool rsa_verify_pss(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature, std::size_t salt_length)
{
    if (!rsa_key_acceptable(key) || digest.size() != digest_size(hash))
        return false;
    std::size_t const mod_bits = key.n.bit_length();
    std::size_t const k = (mod_bits + 7) / 8;
    if (signature.size() != k)
        return false;
    // EMSA-PSS-VERIFY (§9.1.2) over emBits = modBits − 1.
    std::size_t const em_bits = mod_bits - 1;
    std::size_t const em_len = (em_bits + 7) / 8;
    std::optional<std::vector<std::uint8_t>> const em_full = encoded_message(key, signature, k);
    if (!em_full)
        return false;
    std::span<std::uint8_t const> em(*em_full);
    if (em_len < k) {
        if (em[0] != 0)
            return false;
        em = em.subspan(k - em_len);
    }
    std::size_t const h_len = digest.size();
    if (em_len < h_len + salt_length + 2)
        return false;
    if (em[em_len - 1] != 0xbc)
        return false;
    std::span<std::uint8_t const> const masked_db = em.subspan(0, em_len - h_len - 1);
    std::span<std::uint8_t const> const h = em.subspan(em_len - h_len - 1, h_len);
    std::size_t const unused_bits = 8 * em_len - em_bits;
    if (unused_bits > 0 && (masked_db[0] >> (8 - unused_bits)) != 0)
        return false;
    std::vector<std::uint8_t> db = mgf1(hash, h, masked_db.size());
    for (std::size_t i = 0; i < db.size(); ++i)
        db[i] = static_cast<std::uint8_t>(db[i] ^ masked_db[i]);
    if (unused_bits > 0)
        db[0] = static_cast<std::uint8_t>(db[0] & (0xff >> unused_bits));
    std::size_t const ps_length = em_len - h_len - salt_length - 2;
    for (std::size_t i = 0; i < ps_length; ++i) {
        if (db[i] != 0)
            return false;
    }
    if (db[ps_length] != 0x01)
        return false;
    std::span<std::uint8_t const> const salt = std::span<std::uint8_t const>(db).subspan(ps_length + 1, salt_length);
    // M' = eight zero bytes || mHash || salt; H must equal Hash(M').
    std::vector<std::uint8_t> m_prime(8, 0);
    m_prime.insert(m_prime.end(), digest.begin(), digest.end());
    m_prime.insert(m_prime.end(), salt.begin(), salt.end());
    std::vector<std::uint8_t> const h_prime = hash_with(hash, m_prime);
    return std::equal(h.begin(), h.end(), h_prime.begin(), h_prime.end());
}

}
