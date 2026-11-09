#pragma once

// An H.264 stream decoded picture by picture through the hardware: the
// host's half of the decoder (ITU-T H.264 §8), which the accelerator does
// not do — the parameter sets kept by id, each access unit's slices read,
// the picture order count (8.2.1), gaps in frame numbers (8.2.5.2), the
// decoded picture buffer's slots and the marking of its references
// (8.2.5: sliding window and the memory management control operations)
// — and the accelerator's half asked for with all of that.
//
// Pictures come out in decoding order, each read back as it is decoded;
// the container's presentation times order them for showing, which is
// what a player presents by.

#include "media/H264.h"
#include "media/H264Accelerator.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sashfold::media {

class H264Decoder {
public:
    explicit H264Decoder(std::unique_ptr<H264Accelerator> accelerator);

    std::string const& device() const { return m_accelerator->device(); }

    // The track's configuration record (avcC): its parameter sets and the
    // length prefix its samples carry. Without one, samples are Annex B.
    bool set_configuration(std::span<std::uint8_t const> avcc);

    // One access unit — a container's sample — decoded: the picture, or
    // nothing when it could not be (a missing set, a reference the buffer
    // does not hold, the hardware's refusal). Parameter sets in the unit
    // are taken up; units with no slice decode nothing and give nothing.
    std::optional<Nv12Picture> decode(std::span<std::uint8_t const> access_unit);

    // Forgets the buffer and the counts, as at a seek to a key frame; the
    // sets stay.
    void reset();

    struct Counts {
        std::uint64_t decoded = 0;
        std::uint64_t failed = 0;
        std::uint64_t gaps = 0; // frames the stream skipped (8.2.5.2)
        std::uint64_t declined = 0; // pictures kept from a driver known to get them wrong
    };
    Counts const& counts() const { return m_counts; }

private:
    struct Entry {
        int slot = -1; // -1: a frame the stream skipped, never decoded
        int frame_num = 0;
        int frame_num_wrap = 0;
        bool long_term = false;
        int long_term_frame_idx = 0;
        int poc_top = 0;
        int poc_bottom = 0;
    };

    bool take_parameter_sets(std::vector<H264Nal> const& nals);
    int free_slot() const;
    void sliding_window(H264Sps const& sps);
    bool apply_mmco(H264SliceHeader const& slice, H264Sps const& sps, H264PocState::Poc& poc, bool& current_long_term, int& current_long_term_idx);
    void fill_frame_num_gap(H264SliceHeader const& slice, H264Sps const& sps);

    std::unique_ptr<H264Accelerator> m_accelerator;
    int m_length_size = 0; // 0: Annex B
    std::map<int, H264Sps> m_sps;
    std::map<int, H264Pps> m_pps;
    bool m_sets_changed = true;
    H264PocState m_poc;
    std::vector<Entry> m_dpb;
    int m_prev_ref_frame_num = 0;
    int m_max_long_term_frame_idx = -1; // "no long-term frame indices"
    bool m_started = false; // an IDR has been seen since the last reset
    Counts m_counts;
};

}
