#pragma once

// AAC (ISO/IEC 14496-3 subpart 4), the sound the web's MP4 video carries,
// decoded by a reader of our own: the AAC-LC profile — the raw data block's
// elements (single channels, channel pairs, low-frequency channels, the
// fill and data elements passed over), the Huffman-coded sections,
// scalefactors and spectra, pulses, temporal noise shaping, mid/side and
// intensity stereo, perceptual noise substitution, the inverse MDCT in
// long and short windows with sine and Kaiser-Bessel shapes and the
// overlap-add — from the AudioSpecificConfig an MP4's esds carries, and
// HE-AAC's spectral band replication (media/Sbr.h) on top of it, which
// doubles the rate and the samples a block gives, and HE-AAC v2's
// parametric stereo (media/Ps.h), which makes a single channel two.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::media {

struct AacConfig {
    int object_type = 2; // 2 AAC-LC; 5 and 29 say SBR/PS wrap an LC core
    int sample_rate = 0;
    int channel_configuration = 0; // 1 mono, 2 stereo, 3 to 7 the standard layouts
    int channels = 0;
    bool sbr_signalled = false;
    bool ps_signalled = false; // parametric stereo: type 29, or the 0x548 sync extension
    int extension_sample_rate = 0; // the rate after SBR, when signalled
    bool frame_960 = false; // 960-sample frames (not read)
};

// The AudioSpecificConfig (1.6.2.1); nothing when it is not AAC-LC (or an
// SBR/PS wrapping of one) at a rate and layout this decoder reads.
std::optional<AacConfig> parse_aac_config(std::span<std::uint8_t const> config);

class AacDecoder {
public:
    explicit AacDecoder(AacConfig const& config);
    ~AacDecoder();
    AacDecoder(AacDecoder const&) = delete;
    AacDecoder& operator=(AacDecoder const&) = delete;

    int sample_rate() const; // the core's
    // The channels the blocks come out with: the configuration's, or two
    // for a single channel once parametric stereo is signalled or met.
    int channels() const;
    // The rate and the samples a channel the blocks come out at: the
    // core's and 1024, or twice both once spectral band replication is in
    // play (signalled by the configuration, or met in the stream).
    int output_rate() const;
    int frames_per_block() const;

    // One raw data block (an MP4 sample) decoded to frames_per_block()
    // samples a channel, interleaved, in [-1, 1]. False when the block
    // could not be read; the decoder goes on with the next.
    bool decode(std::span<std::uint8_t const> block, std::vector<float>& out);

    // Forgets the overlap, as at a seek.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
