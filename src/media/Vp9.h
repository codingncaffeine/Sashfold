#pragma once

// VP9 (the VP9 Bitstream & Decoding Process Specification), as far as a
// decoder needs to read before the arithmetic-coded part of a frame: the
// superframe that packs several frames into one block of the container, and
// each frame's uncompressed header — its size, its type, which references
// it reads and refreshes, and the loop filter, quantizer, segmentation and
// tile parameters. That is everything a hardware decoder is handed besides
// the bytes, and where a software decoder starts.
//
// Some of a header's meaning carries from frame to frame — the reference
// frames' sizes, the loop filter's deltas, the segmentation — so the reader
// is an object that sees the frames of one stream in order.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::media {

// The frames of one block: a superframe's index (a marker byte at both ends
// of a table of sizes, at the end of the block) splits it; any other block
// is one frame.
std::vector<std::span<std::uint8_t const>> split_vp9_superframe(std::span<std::uint8_t const> block);

struct Vp9Segmentation {
    bool enabled = false;
    bool update_map = false;
    bool temporal_update = false;
    bool update_data = false;
    bool abs_or_delta_update = false;
    std::array<std::uint8_t, 7> tree_probs { 255, 255, 255, 255, 255, 255, 255 };
    std::array<std::uint8_t, 3> pred_probs { 255, 255, 255 };
    // Per segment, per feature: the quantizer, the loop filter level, the
    // reference frame and skip.
    std::array<std::array<bool, 4>, 8> feature_enabled {};
    std::array<std::array<int, 4>, 8> feature_data {};
};

struct Vp9LoopFilter {
    int level = 0;
    int sharpness = 0;
    bool delta_enabled = false;
    bool delta_update = false;
    std::array<int, 4> ref_deltas { 1, 0, -1, -1 }; // intra, last, golden, altref
    std::array<int, 2> mode_deltas { 0, 0 };
    // Which of the deltas this frame's own header set (Vulkan Video is
    // told, beside the values carried).
    std::array<bool, 4> ref_deltas_updated {};
    std::array<bool, 2> mode_deltas_updated {};
};

struct Vp9FrameHeader {
    int profile = 0;
    bool show_existing_frame = false;
    int frame_to_show = 0;
    bool key_frame = false;
    bool show_frame = false;
    bool error_resilient = false;
    bool intra_only = false;
    int reset_frame_context = 0;
    int bit_depth = 8;
    int color_space = 0;
    bool color_range = false;
    bool subsampling_x = true;
    bool subsampling_y = true;
    std::uint8_t refresh_frame_flags = 0;
    std::array<int, 3> ref_frame_idx {}; // last, golden, altref
    std::array<bool, 4> ref_sign_bias {}; // by reference: intra, last, golden, altref
    int width = 0;
    int height = 0;
    int render_width = 0;
    int render_height = 0;
    bool allow_high_precision_mv = false;
    int interp_filter = 0; // 0 eighttap, 1 smooth, 2 sharp, 3 bilinear, 4 switchable
    bool refresh_frame_context = false;
    bool frame_parallel_decoding = false;
    int frame_context_idx = 0;
    Vp9LoopFilter loop_filter;
    int base_q_idx = 0;
    int delta_q_y_dc = 0;
    int delta_q_uv_dc = 0;
    int delta_q_uv_ac = 0;
    bool lossless = false;
    Vp9Segmentation segmentation;
    int tile_cols_log2 = 0;
    int tile_rows_log2 = 0;
    int compressed_header_size = 0; // bytes, after the uncompressed header
    int uncompressed_header_size = 0; // bytes

    bool intra() const { return key_frame || intra_only; }
};

class Vp9HeaderReader {
public:
    // Reads the frame's uncompressed header, carrying forward what VP9
    // carries; nothing for a frame that is malformed or of a kind this
    // reader does not take (a profile above 3, a frame referring to a slot
    // never filled). Frames must come in the stream's order.
    std::optional<Vp9FrameHeader> read(std::span<std::uint8_t const> frame);
    // Forgets everything, as at a new stream (a new initialization segment).
    void reset();

    // The size each reference slot holds, as the frames so far have filled them.
    std::array<std::pair<int, int>, 8> const& slot_sizes() const { return m_slot_sizes; }

private:
    std::array<std::pair<int, int>, 8> m_slot_sizes {};
    Vp9LoopFilter m_loop_filter;
    Vp9Segmentation m_segmentation;
    bool m_seen_key_frame = false;
};

// The quantizer's step for a coefficient at the given index (0-255), DC or
// AC, at 8 bits (§8.6.1 dc_q, ac_q).
int vp9_dc_quant(int qindex);
int vp9_ac_quant(int qindex);

// What each segment works with, as the header sets it: its quantizer index
// with the segment's feature applied, and the loop filter level by
// reference frame (intra, last, golden, altref) and mode (zero motion or
// not), as the reference decoder derives them.
int vp9_segment_qindex(Vp9FrameHeader const&, int segment);
std::array<std::array<std::uint8_t, 2>, 4> vp9_segment_filter_levels(Vp9FrameHeader const&, int segment);

}
