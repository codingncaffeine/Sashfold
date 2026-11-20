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
// what a player presents by. A stream coded in fields gives its frames
// once both fields are decoded, the two woven line by line (what a
// browser shows of interlaced video when no deinterlacing is asked for).

#include "media/H264.h"
#include "media/H264Accelerator.h"

#include <array>
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
    // A sample holding both fields of a frame decodes both (7.4.1.2.4
    // says where the second picture begins) and gives the frame; a unit
    // holding a first field alone gives nothing until its second comes.
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
    // A frame store holding references: a frame, a complementary field
    // pair, or one field, each field marked on its own (8.2.5).
    struct Entry {
        int slot = -1; // -1: a frame the stream skipped, never decoded
        int frame_num = 0;
        int frame_num_wrap = 0;
        int long_term_frame_idx = 0;
        std::array<std::uint8_t, 2> marking { 0, 0 }; // top, bottom: 0 unused, 1 short-term, 2 long-term
        int poc_top = 0;
        int poc_bottom = 0;
        std::uint8_t fields = 3; // decoded: 1 top, 2 bottom, 3 both
        bool has(std::uint8_t m) const { return marking[0] == m || marking[1] == m; }
        bool referenced() const { return marking[0] != 0 || marking[1] != 0; }
    };

    // The slices of one picture (a frame or a field), with the sets they name.
    struct Picture {
        std::vector<std::span<std::uint8_t const>> slices;
        std::vector<H264SliceHeader> headers;
        H264Sps const* sps = nullptr;
        H264Pps const* pps = nullptr;
    };

    bool take_parameter_sets(std::vector<H264Nal> const& nals);
    std::optional<Nv12Picture> decode_picture(Picture const& unit);
    int free_slot() const;
    void sliding_window(H264Sps const& sps, int current_slot);
    bool apply_mmco(H264SliceHeader const& slice, H264PocState::Poc& poc, int current_slot, bool& current_long_term, int& current_long_term_idx);
    void fill_frame_num_gap(H264SliceHeader const& slice, H264Sps const& sps);
    void drop_unreferenced();

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
    // The first field of a frame whose second has not come yet.
    struct FirstField {
        bool active = false;
        int slot = -1;
        int frame_num = 0;
        bool bottom = false;
        bool reference = false;
        bool idr = false;
    } m_first_field;
    Counts m_counts;
};

}
