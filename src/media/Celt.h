#pragma once

// CELT, the transform layer of Opus (RFC 6716 §4.3): what music is coded
// with, and all that YouTube sends. A frame is the energy of each of 21
// bands, coded coarsely then finely; a split of the remaining bits among
// the bands by a rule the encoder and decoder both follow; and each band's
// shape as a vector of pulses on a sphere, the spectrum rebuilt from them,
// taken back to time through an inverse MDCT, and smoothed by a pitch
// post-filter and a de-emphasis.
//
// The arithmetic is floating point, as a player's decoder is; everything
// that decides which bits mean what — the range decoder, the allocation,
// the splitting angles — is integer and exact, which is what the final
// range of every frame checks.

#include "media/RangeDecoder.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sashfold::media {

class CeltDecoder {
public:
    // A decoder for 48 kHz output of one or two channels.
    explicit CeltDecoder(int channels);

    // Forgets every frame decoded so far, as at the start of a stream.
    void reset();

    // Decodes one frame of `frame_size` samples a channel (120, 240, 480 or
    // 960) from `bytes` bytes read through `range`, into interleaved floats
    // in [-1, 1]. `coded_channels` is what the frame carries, which may
    // differ from what is played; `start_band` is 17 for the upper half of a
    // hybrid frame, and `end_band` narrows the bands for a smaller bandwidth.
    // False for a frame that does not decode.
    bool decode(RangeDecoder& range, int bytes, float* pcm, int frame_size, int coded_channels, int start_band = 0,
        int end_band = 21);
    // A frame that was lost: what the last one leaves behind, fading out.
    void conceal(float* pcm, int frame_size);

    std::uint32_t final_range() const { return m_rng; }
    int channels() const { return m_channels; }

private:
    void synthesize(float* freq, float* pcm, int N, int LM, int short_blocks, int coded_channels, int postfilter_pitch,
        float postfilter_gain, int postfilter_tapset);

    int m_channels;
    // Per channel: the history the post-filter reaches back into and the
    // overlap the next frame's inverse MDCT adds to, in one buffer.
    std::vector<float> m_decode_memory;
    std::array<float, 2 * 21> m_old_band_energy {};
    std::array<float, 2 * 21> m_old_log_energy {};
    std::array<float, 2 * 21> m_old_log_energy2 {};
    std::array<float, 2 * 21> m_background_log_energy {};
    std::array<float, 2> m_preemphasis_memory {};
    std::uint32_t m_rng = 0;
    int m_postfilter_period = 0;
    int m_postfilter_period_old = 0;
    float m_postfilter_gain = 0;
    float m_postfilter_gain_old = 0;
    int m_postfilter_tapset = 0;
    int m_postfilter_tapset_old = 0;
    int m_loss_count = 0;
};

}
