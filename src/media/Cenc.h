#pragma once

// Common Encryption (ISO/IEC 23001-7): how one protected sample is laid
// out and taken back to the clear. A container's demuxer — MP4's sample
// encryption boxes, WebM's signal byte and partitions — describes each
// encrypted sample with a SampleEncryption; whoever holds the key (the
// Clear Key session of Encrypted Media Extensions) decrypts it here, just
// before the sample is decoded.
//
// The four schemes of §10: 'cenc' (AES-CTR over every protected byte),
// 'cens' (AES-CTR over a pattern of 16-byte blocks), 'cbc1' (AES-CBC over
// every whole protected block) and 'cbcs' (AES-CBC over a pattern, the
// chain begun again at every subsample).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sashfold::media {

using KeyId = std::array<std::uint8_t, 16>;

constexpr std::uint32_t fourcc(std::string_view code)
{
    return (std::uint32_t(static_cast<std::uint8_t>(code[0])) << 24) | (std::uint32_t(static_cast<std::uint8_t>(code[1])) << 16)
        | (std::uint32_t(static_cast<std::uint8_t>(code[2])) << 8) | std::uint32_t(static_cast<std::uint8_t>(code[3]));
}

constexpr std::uint32_t scheme_cenc = fourcc("cenc");
constexpr std::uint32_t scheme_cens = fourcc("cens");
constexpr std::uint32_t scheme_cbc1 = fourcc("cbc1");
constexpr std::uint32_t scheme_cbcs = fourcc("cbcs");

// One entry of a sample's subsample map (§7.2.2): so many bytes in the
// clear, then so many protected.
struct Subsample {
    std::uint32_t clear_bytes = 0;
    std::uint32_t protected_bytes = 0;
    bool operator==(Subsample const&) const = default;
};

struct SampleEncryption {
    std::uint32_t scheme = scheme_cenc;
    // Sixteen bytes in MP4; a WebM track's ContentEncKeyID may be longer.
    std::vector<std::uint8_t> key_id;
    // 8 or 16 bytes: the sample's own IV, or the track's constant one.
    std::vector<std::uint8_t> iv;
    // Empty: the whole sample is one protected range.
    std::vector<Subsample> subsamples;
    // The pattern of §9.6 ('cens', 'cbcs'): so many 16-byte blocks
    // encrypted, then so many left clear, over each protected range. Both
    // zero: every whole block is encrypted.
    std::uint8_t crypt_byte_block = 0;
    std::uint8_t skip_byte_block = 0;
};

// Decrypts `data` in place with the 16-byte `key`. False, with `data` as it
// was, when the description does not fit the sample: an unknown scheme, an
// IV of the wrong size, or a subsample map that runs past the sample's end
// or leaves bytes over (§7.2.2: the map covers the sample exactly).
bool decrypt_sample(SampleEncryption const& encryption, std::span<std::uint8_t const> key, std::span<std::uint8_t> data);

// The same transform the other way, for whoever writes protected samples
// (the tests' fixtures).
bool encrypt_sample(SampleEncryption const& encryption, std::span<std::uint8_t const> key, std::span<std::uint8_t> data);

}
