#include "media/Cenc.h"

#include "crypto/Aes.h"
#include "crypto/AesModes.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace sashfold::media {

namespace {

// The protected ranges of a sample, in order: the map's, or the whole.
std::optional<std::vector<std::span<std::uint8_t>>> protected_ranges(SampleEncryption const& encryption, std::span<std::uint8_t> data)
{
    std::vector<std::span<std::uint8_t>> ranges;
    if (encryption.subsamples.empty()) {
        ranges.push_back(data);
        return ranges;
    }
    std::size_t at = 0;
    for (Subsample const& entry : encryption.subsamples) {
        if (entry.clear_bytes > data.size() - at)
            return std::nullopt;
        at += entry.clear_bytes;
        if (entry.protected_bytes > data.size() - at)
            return std::nullopt;
        ranges.push_back(data.subspan(at, entry.protected_bytes));
        at += entry.protected_bytes;
    }
    if (at != data.size())
        return std::nullopt;
    return ranges;
}

// §9.6: a protected range in the pattern's whole blocks — `crypt` of them
// encrypted, then `skip` clear, again and again; when fewer than `crypt`
// whole blocks are left they are all encrypted, and a last partial block
// stays clear. A pattern of 0:0 encrypts every whole block.
template<typename F>
void for_each_encrypted_run(std::span<std::uint8_t> range, std::uint8_t crypt, std::uint8_t skip, F&& run)
{
    std::size_t const whole = range.size() / 16 * 16;
    if (crypt == 0 && skip == 0) {
        if (whole > 0)
            run(range.first(whole));
        return;
    }
    std::size_t at = 0;
    while (at < whole) {
        std::size_t const take = std::min<std::size_t>(std::size_t { crypt } * 16, whole - at);
        if (take > 0)
            run(range.subspan(at, take));
        at += take + std::size_t { skip } * 16;
    }
}

crypto::AesBlock counter_block(std::vector<std::uint8_t> const& iv)
{
    // An 8-byte IV is the high half of the block, the low half the block
    // counter from zero (§9.5.2.2); a 16-byte IV is the whole first block.
    crypto::AesBlock block {};
    std::memcpy(block.data(), iv.data(), std::min<std::size_t>(iv.size(), 16));
    return block;
}

bool transform(SampleEncryption const& encryption, std::span<std::uint8_t const> key, std::span<std::uint8_t> data, bool decrypting)
{
    if (key.size() != 16 || (encryption.iv.size() != 8 && encryption.iv.size() != 16))
        return false;
    std::optional<crypto::Aes> const aes = crypto::Aes::with_key(key);
    if (!aes)
        return false;
    std::optional<std::vector<std::span<std::uint8_t>>> const ranges = protected_ranges(encryption, data);
    if (!ranges)
        return false;
    switch (encryption.scheme) {
    case scheme_cenc: {
        // One keystream over every protected byte of the sample, the
        // block counter in the low 64 bits.
        crypto::AesCtrStream stream(*aes, counter_block(encryption.iv), 64);
        for (std::span<std::uint8_t> const range : *ranges)
            stream.apply(range);
        return true;
    }
    case scheme_cens: {
        // The counter runs over the encrypted blocks alone, across the
        // sample's ranges.
        crypto::AesCtrStream stream(*aes, counter_block(encryption.iv), 64);
        for (std::span<std::uint8_t> const range : *ranges) {
            for_each_encrypted_run(range, encryption.crypt_byte_block, encryption.skip_byte_block,
                [&](std::span<std::uint8_t> run) { stream.apply(run); });
        }
        return true;
    }
    case scheme_cbc1: {
        // One chain over every whole protected block of the sample.
        crypto::AesBlock chain = counter_block(encryption.iv);
        for (std::span<std::uint8_t> const range : *ranges) {
            std::span<std::uint8_t> const whole = range.first(range.size() / 16 * 16);
            if (decrypting)
                crypto::aes_cbc_decrypt_blocks(*aes, chain, whole);
            else
                crypto::aes_cbc_encrypt_blocks(*aes, chain, whole);
        }
        return true;
    }
    case scheme_cbcs: {
        // The chain begins again from the IV at every protected range and
        // runs over that range's encrypted blocks, the skipped ones passed
        // by (§10.4.2).
        for (std::span<std::uint8_t> const range : *ranges) {
            crypto::AesBlock chain = counter_block(encryption.iv);
            for_each_encrypted_run(range, encryption.crypt_byte_block, encryption.skip_byte_block, [&](std::span<std::uint8_t> run) {
                if (decrypting)
                    crypto::aes_cbc_decrypt_blocks(*aes, chain, run);
                else
                    crypto::aes_cbc_encrypt_blocks(*aes, chain, run);
            });
        }
        return true;
    }
    default:
        return false;
    }
}

}

bool decrypt_sample(SampleEncryption const& encryption, std::span<std::uint8_t const> key, std::span<std::uint8_t> data)
{
    return transform(encryption, key, data, true);
}

bool encrypt_sample(SampleEncryption const& encryption, std::span<std::uint8_t const> key, std::span<std::uint8_t> data)
{
    return transform(encryption, key, data, false);
}

}
