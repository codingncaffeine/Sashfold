#pragma once
// H.264 (ITU-T H.264 | ISO/IEC 14496-10), as far as a decoder must read
// before handing a picture to the video hardware: the NAL units a stream or
// a container's sample is cut into (Annex B start codes, or the length
// prefixes of MP4's avcC), the sequence and picture parameter sets, each
// slice's header, and the picture order count that says where a picture
// stands in presentation (8.2.1). That is what Vulkan Video and VA-API are
// told beside the bytes; the slice data itself is theirs to read.
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::media {

struct H264Nal {
    int ref_idc = 0; // nal_ref_idc: 0 for a picture nothing refers to
    int type = 0; // nal_unit_type: 1 slice, 5 IDR slice, 6 SEI, 7 SPS, 8 PPS, 9 AU delimiter...
    std::span<std::uint8_t const> bytes; // the whole unit, its header byte first, as the stream has it
};

// The units of an Annex B stream (start codes 00 00 01) or of a sample
// whose units carry `length_size` bytes of length each (avcC's, 1 to 4).
std::vector<H264Nal> split_h264_annex_b(std::span<std::uint8_t const> stream);
std::vector<H264Nal> split_h264_length_prefixed(std::span<std::uint8_t const> sample, int length_size);

// The unit's payload with the emulation prevention bytes taken out (7.4.1).
std::vector<std::uint8_t> h264_rbsp(std::span<std::uint8_t const> nal);

// avcC (14496-15 §5.3.3.1): the parameter sets a track's samples share,
// and the length prefix's size.
struct H264DecoderConfiguration {
    int profile_idc = 0;
    int level_idc = 0;
    int length_size = 4;
    std::vector<std::vector<std::uint8_t>> sps; // each a whole NAL unit
    std::vector<std::vector<std::uint8_t>> pps;
};
std::optional<H264DecoderConfiguration> parse_avcc(std::span<std::uint8_t const> avcc);

// Table 7-3 and 7-4's defaults are applied where a list is absent (the
// fall-back rules of Table 7-2), so the lists here are the ones in force.
struct H264ScalingLists {
    bool present = false; // a matrix was given at all
    std::array<std::array<std::uint8_t, 16>, 6> list_4x4 {};
    std::array<std::array<std::uint8_t, 64>, 6> list_8x8 {};
    std::uint16_t present_mask = 0; // which lists the stream gave itself
    std::uint16_t default_mask = 0; // which of those said "use the default"
};

struct H264Vui {
    bool present = false;
    bool aspect_ratio_present = false;
    int aspect_ratio_idc = 0;
    int sar_width = 0;
    int sar_height = 0;
    bool video_signal_type_present = false;
    int video_format = 5;
    bool video_full_range = false;
    bool colour_description_present = false;
    int colour_primaries = 2;
    int transfer_characteristics = 2;
    int matrix_coefficients = 2;
    bool timing_present = false;
    std::uint32_t num_units_in_tick = 0;
    std::uint32_t time_scale = 0;
    bool fixed_frame_rate = false;
    bool bitstream_restriction = false;
    int max_num_reorder_frames = -1; // -1: not said
    int max_dec_frame_buffering = -1;
};

struct H264Sps {
    int profile_idc = 0;
    int constraint_set_flags = 0; // the six flags, constraint_set0 as bit 7
    int level_idc = 0;
    int id = 0;
    int chroma_format_idc = 1;
    bool separate_colour_plane = false;
    int bit_depth_luma = 8;
    int bit_depth_chroma = 8;
    bool qpprime_y_zero_transform_bypass = false;
    H264ScalingLists scaling;
    int log2_max_frame_num = 4;
    int pic_order_cnt_type = 0;
    int log2_max_pic_order_cnt_lsb = 4;
    bool delta_pic_order_always_zero = false;
    int offset_for_non_ref_pic = 0;
    int offset_for_top_to_bottom_field = 0;
    std::vector<int> offset_for_ref_frame;
    int max_num_ref_frames = 0;
    bool gaps_in_frame_num_allowed = false;
    int pic_width_in_mbs = 0;
    int pic_height_in_map_units = 0;
    bool frame_mbs_only = true;
    bool mb_adaptive_frame_field = false;
    bool direct_8x8_inference = false;
    bool frame_cropping = false;
    int crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0; // in the units of 7.4.2.1.1
    H264Vui vui;

    int frame_height_in_mbs() const { return (2 - (frame_mbs_only ? 1 : 0)) * pic_height_in_map_units; }
    int coded_width() const { return pic_width_in_mbs * 16; }
    int coded_height() const { return frame_height_in_mbs() * 16; }
    // The picture as shown, the cropping applied.
    int width() const;
    int height() const;
    // How many frames the decoded picture buffer holds at this level (A.3.1).
    int dpb_frames() const;
};

struct H264Pps {
    int id = 0;
    int sps_id = 0;
    bool entropy_coding_mode = false; // CABAC
    bool bottom_field_pic_order_in_frame_present = false;
    int num_slice_groups = 1;
    int slice_group_map_type = 0;
    int num_ref_idx_l0_default_active = 1;
    int num_ref_idx_l1_default_active = 1;
    bool weighted_pred = false;
    int weighted_bipred_idc = 0;
    int pic_init_qp = 26;
    int pic_init_qs = 26;
    int chroma_qp_index_offset = 0;
    bool deblocking_filter_control_present = false;
    bool constrained_intra_pred = false;
    bool redundant_pic_cnt_present = false;
    bool transform_8x8_mode = false;
    bool pic_scaling_matrix_present = false;
    H264ScalingLists scaling; // in force for pictures using this set (the SPS's where it has none)
    int second_chroma_qp_index_offset = 0;
};

enum class H264SliceType { P = 0,
    B = 1,
    I = 2,
    SP = 3,
    SI = 4 };

struct H264Mmco {
    int op = 0; // memory_management_control_operation 1..6
    int difference_of_pic_nums = 0; // op 1, 3: difference_of_pic_nums_minus1 + 1
    int long_term_pic_num = 0; // op 2
    int long_term_frame_idx = 0; // op 3, 6
    int max_long_term_frame_idx_plus1 = 0; // op 4
};

struct H264SliceHeader {
    int nal_type = 0;
    int nal_ref_idc = 0;
    bool idr = false;
    int first_mb_in_slice = 0;
    H264SliceType slice_type = H264SliceType::I;
    int pps_id = 0;
    int colour_plane_id = 0;
    int frame_num = 0;
    bool field_pic = false;
    bool bottom_field = false;
    int idr_pic_id = 0;
    int pic_order_cnt_lsb = 0;
    int delta_pic_order_cnt_bottom = 0;
    std::array<int, 2> delta_pic_order_cnt { 0, 0 };
    int redundant_pic_cnt = 0;
    bool direct_spatial_mv_pred = false;
    bool num_ref_idx_active_override = false;
    int num_ref_idx_l0_active = 0;
    int num_ref_idx_l1_active = 0;
    // 7.3.3.3: how the decoded picture buffer is marked after this picture.
    bool no_output_of_prior_pics = false;
    bool long_term_reference = false; // an IDR kept as a long-term reference
    bool adaptive_ref_pic_marking = false;
    std::vector<H264Mmco> mmco;
    int cabac_init_idc = 0;
    int slice_qp_delta = 0;
    int disable_deblocking_filter_idc = 0;
    int slice_alpha_c0_offset_div2 = 0;
    int slice_beta_offset_div2 = 0;
    // Where the slice data begins: bits from the unit's first byte, the
    // emulation prevention bytes not counted (what VA-API asks for).
    int header_bits = 0;

    bool is_reference() const { return nal_ref_idc != 0; }
    bool has_mmco5() const;
};

std::optional<H264Sps> parse_h264_sps(std::span<std::uint8_t const> nal);
std::optional<H264Pps> parse_h264_pps(std::span<std::uint8_t const> nal, H264Sps const* sps_for_scaling);
// The header of a slice NAL unit (types 1 and 5), read with its picture's
// sets; the reference lists' modifications and the weight tables are
// passed over (the hardware reads them itself).
std::optional<H264SliceHeader> parse_h264_slice_header(std::span<std::uint8_t const> nal, H264Sps const& sps, H264Pps const& pps);

// 8.2.1: the picture order count of each picture of a stream in decoding
// order, from what the headers carry forward.
class H264PocState {
public:
    struct Poc {
        int top = 0;
        int bottom = 0;
        int frame() const { return top < bottom ? top : bottom; }
    };
    Poc next(H264SliceHeader const& slice, H264Sps const& sps);
    void reset();

private:
    int m_prev_poc_msb = 0;
    int m_prev_poc_lsb = 0;
    int m_prev_frame_num_offset = 0;
    int m_prev_frame_num = 0;
    bool m_prev_had_mmco5 = false;
    int m_prev_top_for_mmco5 = 0;
};

}
