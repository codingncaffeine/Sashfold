#include "media/H264.h"

#include <algorithm>
#include <cstring>

namespace sashfold::media {

namespace {

// --- Bits of an RBSP: fixed-length fields and Exp-Golomb codes (9.1) ----

class Bits {
public:
    explicit Bits(std::span<std::uint8_t const> data)
        : m_data(data)
    {
    }
    bool failed() const { return m_failed; }
    std::size_t position() const { return m_position; }
    std::size_t bits_left() const { return m_data.size() * 8 > m_position ? m_data.size() * 8 - m_position : 0; }

    std::uint32_t u(int n)
    {
        std::uint32_t value = 0;
        for (int i = 0; i < n; ++i)
            value = (value << 1) | bit();
        return value;
    }
    bool flag() { return u(1) != 0; }

    // ue(v): leading zeros, a one, then that many bits; 32 zeros or more is no code.
    std::uint32_t ue()
    {
        int zeros = 0;
        while (zeros < 32 && !m_failed && bit() == 0)
            ++zeros;
        if (zeros >= 32) {
            m_failed = true;
            return 0;
        }
        return ((1u << zeros) - 1) + u(zeros);
    }
    std::int32_t se()
    {
        std::uint32_t const k = ue();
        return (k & 1) ? static_cast<std::int32_t>((k + 1) / 2) : -static_cast<std::int32_t>(k / 2);
    }
    // 7.2: more data before the trailing bits (a one, then zeros to the end).
    bool more_rbsp_data() const
    {
        if (m_failed || m_position >= m_data.size() * 8)
            return false;
        // The last set bit of the data is the stop bit; data remains while
        // the position is before it.
        std::size_t last = m_data.size();
        while (last > 0 && m_data[last - 1] == 0)
            --last;
        if (last == 0)
            return false;
        std::uint8_t const byte = m_data[last - 1];
        int trailing = 0;
        while (!((byte >> trailing) & 1))
            ++trailing;
        std::size_t const stop_bit = (last - 1) * 8 + static_cast<std::size_t>(7 - trailing);
        return m_position < stop_bit;
    }

private:
    std::uint32_t bit()
    {
        if (m_position >= m_data.size() * 8) {
            m_failed = true;
            return 0;
        }
        std::uint32_t const b = (m_data[m_position >> 3] >> (7 - (m_position & 7))) & 1;
        ++m_position;
        return b;
    }

    std::span<std::uint8_t const> m_data;
    std::size_t m_position = 0;
    bool m_failed = false;
};

// Tables 7-3 and 7-4, in the zig-zag order the lists are coded in.
constexpr std::uint8_t default_4x4_intra[16] = { 6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42 };
constexpr std::uint8_t default_4x4_inter[16] = { 10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34 };
constexpr std::uint8_t default_8x8_intra[64] = { 6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23, 23, 23, 23, 23, 23,
    25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31, 31, 33, 33, 33, 33,
    33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42 };
constexpr std::uint8_t default_8x8_inter[64] = { 9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21, 21, 21, 21, 21, 21,
    22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27, 27, 28, 28, 28, 28,
    28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35 };

// 7.3.2.1.1.1: one list; `use_default` when its first delta asks for Table 7-3's.
template <std::size_t N>
void read_scaling_list(Bits& bits, std::array<std::uint8_t, N>& list, bool& use_default)
{
    int last = 8;
    int next = 8;
    use_default = false;
    for (std::size_t j = 0; j < N; ++j) {
        if (next != 0) {
            int const delta = bits.se();
            next = (last + delta + 256) % 256;
            use_default = j == 0 && next == 0;
        }
        list[j] = static_cast<std::uint8_t>(next == 0 ? last : next);
        last = list[j];
    }
}

// The lists in force: those given, the defaults or the fall-backs for the
// rest (Table 7-2; rule A from the defaults, rule B from the SPS's lists).
void read_scaling_matrix(Bits& bits, H264ScalingLists& out, int list_count, H264ScalingLists const* fallback_b)
{
    out.present = true;
    for (int i = 0; i < list_count; ++i) {
        bool const given = bits.flag();
        bool use_default = false;
        if (given) {
            out.present_mask |= static_cast<std::uint16_t>(1u << i);
            if (i < 6)
                read_scaling_list(bits, out.list_4x4[static_cast<std::size_t>(i)], use_default);
            else
                read_scaling_list(bits, out.list_8x8[static_cast<std::size_t>(i) - 6], use_default);
            if (use_default)
                out.default_mask |= static_cast<std::uint16_t>(1u << i);
        }
        if (given && !use_default)
            continue;
        // Absent or default: the fall-back.
        bool const from_default = use_default || (!fallback_b && (i == 0 || i == 3 || i == 6 || i == 7));
        if (i < 6) {
            auto& list = out.list_4x4[static_cast<std::size_t>(i)];
            if (from_default)
                std::copy_n(i < 3 ? default_4x4_intra : default_4x4_inter, 16, list.begin());
            else if (!given && fallback_b && (i == 0 || i == 3))
                list = fallback_b->list_4x4[static_cast<std::size_t>(i)];
            else
                list = out.list_4x4[static_cast<std::size_t>(i) - 1];
        } else {
            auto& list = out.list_8x8[static_cast<std::size_t>(i) - 6];
            if (from_default)
                std::copy_n((i % 2 == 0) ? default_8x8_intra : default_8x8_inter, 64, list.begin());
            else if (!given && fallback_b && (i == 6 || i == 7))
                list = fallback_b->list_8x8[static_cast<std::size_t>(i) - 6];
            else
                list = out.list_8x8[static_cast<std::size_t>(i) - 8];
        }
    }
}

// Flat 16: what applies when no matrix is given at all (7.4.2.1.1.1).
void flat_scaling(H264ScalingLists& out)
{
    for (auto& l : out.list_4x4)
        l.fill(16);
    for (auto& l : out.list_8x8)
        l.fill(16);
}

// E.1.2: the HRD parameters, passed over.
bool skip_hrd(Bits& bits)
{
    std::uint32_t const cpb_cnt = bits.ue() + 1;
    if (cpb_cnt > 32)
        return false;
    bits.u(4); // bit_rate_scale
    bits.u(4); // cpb_size_scale
    for (std::uint32_t i = 0; i < cpb_cnt; ++i) {
        bits.ue(); // bit_rate_value_minus1
        bits.ue(); // cpb_size_value_minus1
        bits.flag(); // cbr_flag
    }
    bits.u(5); // initial_cpb_removal_delay_length_minus1
    bits.u(5);
    bits.u(5);
    bits.u(5);
    return !bits.failed();
}

// E.1.1: the VUI, the parts a player reads kept.
bool read_vui(Bits& bits, H264Vui& vui)
{
    vui.present = true;
    vui.aspect_ratio_present = bits.flag();
    if (vui.aspect_ratio_present) {
        vui.aspect_ratio_idc = static_cast<int>(bits.u(8));
        if (vui.aspect_ratio_idc == 255) {
            vui.sar_width = static_cast<int>(bits.u(16));
            vui.sar_height = static_cast<int>(bits.u(16));
        }
    }
    if (bits.flag()) // overscan_info_present
        bits.flag();
    vui.video_signal_type_present = bits.flag();
    if (vui.video_signal_type_present) {
        vui.video_format = static_cast<int>(bits.u(3));
        vui.video_full_range = bits.flag();
        vui.colour_description_present = bits.flag();
        if (vui.colour_description_present) {
            vui.colour_primaries = static_cast<int>(bits.u(8));
            vui.transfer_characteristics = static_cast<int>(bits.u(8));
            vui.matrix_coefficients = static_cast<int>(bits.u(8));
        }
    }
    if (bits.flag()) { // chroma_loc_info_present
        bits.ue();
        bits.ue();
    }
    vui.timing_present = bits.flag();
    if (vui.timing_present) {
        vui.num_units_in_tick = bits.u(32);
        vui.time_scale = bits.u(32);
        vui.fixed_frame_rate = bits.flag();
    }
    bool const nal_hrd = bits.flag();
    if (nal_hrd && !skip_hrd(bits))
        return false;
    bool const vcl_hrd = bits.flag();
    if (vcl_hrd && !skip_hrd(bits))
        return false;
    if (nal_hrd || vcl_hrd)
        bits.flag(); // low_delay_hrd
    bits.flag(); // pic_struct_present
    vui.bitstream_restriction = bits.flag();
    if (vui.bitstream_restriction) {
        bits.flag(); // motion_vectors_over_pic_boundaries
        bits.ue(); // max_bytes_per_pic_denom
        bits.ue(); // max_bits_per_mb_denom
        bits.ue(); // log2_max_mv_length_horizontal
        bits.ue(); // log2_max_mv_length_vertical
        vui.max_num_reorder_frames = static_cast<int>(bits.ue());
        vui.max_dec_frame_buffering = static_cast<int>(bits.ue());
    }
    return !bits.failed();
}

bool has_chroma_format(int profile_idc)
{
    switch (profile_idc) {
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135:
        return true;
    default:
        return false;
    }
}

} // namespace

// --- NAL units ------------------------------------------------------------

std::vector<H264Nal> split_h264_annex_b(std::span<std::uint8_t const> stream)
{
    std::vector<H264Nal> out;
    std::size_t at = 0;
    std::optional<std::size_t> start;
    auto const finish = [&](std::size_t end) {
        if (!start || end <= *start)
            return;
        // Trailing zero bytes before a start code belong to no unit.
        while (end > *start && stream[end - 1] == 0)
            --end;
        if (end > *start) {
            H264Nal nal;
            nal.bytes = stream.subspan(*start, end - *start);
            nal.ref_idc = (nal.bytes[0] >> 5) & 3;
            nal.type = nal.bytes[0] & 31;
            out.push_back(nal);
        }
    };
    while (at + 3 <= stream.size()) {
        if (stream[at] == 0 && stream[at + 1] == 0 && stream[at + 2] == 1) {
            finish(at);
            at += 3;
            start = at;
            continue;
        }
        ++at;
    }
    finish(stream.size());
    return out;
}

std::vector<H264Nal> split_h264_length_prefixed(std::span<std::uint8_t const> sample, int length_size)
{
    std::vector<H264Nal> out;
    if (length_size < 1 || length_size > 4)
        return out;
    std::size_t at = 0;
    while (at + static_cast<std::size_t>(length_size) <= sample.size()) {
        std::uint32_t length = 0;
        for (int i = 0; i < length_size; ++i)
            length = (length << 8) | sample[at + static_cast<std::size_t>(i)];
        at += static_cast<std::size_t>(length_size);
        if (length == 0 || length > sample.size() - at)
            break;
        H264Nal nal;
        nal.bytes = sample.subspan(at, length);
        nal.ref_idc = (nal.bytes[0] >> 5) & 3;
        nal.type = nal.bytes[0] & 31;
        out.push_back(nal);
        at += length;
    }
    return out;
}

std::vector<std::uint8_t> h264_rbsp(std::span<std::uint8_t const> nal)
{
    std::vector<std::uint8_t> out;
    if (nal.size() <= 1)
        return out;
    out.reserve(nal.size());
    int zeros = 0;
    for (std::size_t i = 1; i < nal.size(); ++i) {
        std::uint8_t const b = nal[i];
        if (zeros >= 2 && b == 3) {
            zeros = 0; // the emulation prevention byte itself
            continue;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

std::optional<H264DecoderConfiguration> parse_avcc(std::span<std::uint8_t const> avcc)
{
    if (avcc.size() < 7 || avcc[0] != 1)
        return std::nullopt;
    H264DecoderConfiguration out;
    out.profile_idc = avcc[1];
    out.level_idc = avcc[3];
    out.length_size = (avcc[4] & 3) + 1;
    std::size_t at = 5;
    int const sps_count = avcc[at++] & 31;
    for (int i = 0; i < sps_count; ++i) {
        if (at + 2 > avcc.size())
            return std::nullopt;
        std::size_t const length = (avcc[at] << 8) | avcc[at + 1];
        at += 2;
        if (at + length > avcc.size())
            return std::nullopt;
        out.sps.emplace_back(avcc.begin() + static_cast<std::ptrdiff_t>(at), avcc.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    }
    if (at >= avcc.size())
        return std::nullopt;
    int const pps_count = avcc[at++];
    for (int i = 0; i < pps_count; ++i) {
        if (at + 2 > avcc.size())
            return std::nullopt;
        std::size_t const length = (avcc[at] << 8) | avcc[at + 1];
        at += 2;
        if (at + length > avcc.size())
            return std::nullopt;
        out.pps.emplace_back(avcc.begin() + static_cast<std::ptrdiff_t>(at), avcc.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    }
    return out;
}

// --- Parameter sets ---------------------------------------------------------

int H264Sps::width() const
{
    int const crop_unit_x = chroma_format_idc == 0 || separate_colour_plane ? 1 : (chroma_format_idc == 3 ? 1 : 2);
    return coded_width() - (frame_cropping ? crop_unit_x * (crop_left + crop_right) : 0);
}

int H264Sps::height() const
{
    int const sub_height = chroma_format_idc == 1 ? 2 : 1;
    int const crop_unit_y = (chroma_format_idc == 0 || separate_colour_plane ? 1 : sub_height) * (2 - (frame_mbs_only ? 1 : 0));
    return coded_height() - (frame_cropping ? crop_unit_y * (crop_top + crop_bottom) : 0);
}

int H264Sps::dpb_frames() const
{
    // Table A-1's MaxDpbMbs by level, over the frame's macroblocks, at most 16.
    static constexpr struct {
        int level;
        int max_dpb_mbs;
    } levels[] = { { 9, 396 }, { 10, 396 }, { 11, 900 }, { 12, 2376 }, { 13, 2376 }, { 20, 2376 }, { 21, 4752 }, { 22, 8100 },
        { 30, 8100 }, { 31, 18000 }, { 32, 20480 }, { 40, 32768 }, { 41, 32768 }, { 42, 34816 }, { 50, 110400 }, { 51, 184320 },
        { 52, 184320 }, { 60, 696320 }, { 61, 696320 }, { 62, 696320 } };
    int level = level_idc;
    if (level == 11 && (constraint_set_flags & 0x10))
        level = 9; // level 1b
    int max_dpb_mbs = 696320;
    for (auto const& l : levels) {
        if (l.level == level) {
            max_dpb_mbs = l.max_dpb_mbs;
            break;
        }
    }
    int const frame_mbs = std::max(1, pic_width_in_mbs * frame_height_in_mbs());
    int frames = std::clamp(max_dpb_mbs / frame_mbs, 1, 16);
    if (vui.max_dec_frame_buffering >= 0)
        frames = std::clamp(vui.max_dec_frame_buffering, 1, 16);
    return std::max(frames, std::min(16, max_num_ref_frames));
}

std::optional<H264Sps> parse_h264_sps(std::span<std::uint8_t const> nal)
{
    if (nal.empty() || (nal[0] & 31) != 7)
        return std::nullopt;
    std::vector<std::uint8_t> const rbsp = h264_rbsp(nal);
    Bits bits(rbsp);
    H264Sps sps;
    sps.profile_idc = static_cast<int>(bits.u(8));
    sps.constraint_set_flags = static_cast<int>(bits.u(8));
    sps.level_idc = static_cast<int>(bits.u(8));
    sps.id = static_cast<int>(bits.ue());
    if (sps.id > 31)
        return std::nullopt;
    flat_scaling(sps.scaling);
    if (has_chroma_format(sps.profile_idc)) {
        sps.chroma_format_idc = static_cast<int>(bits.ue());
        if (sps.chroma_format_idc > 3)
            return std::nullopt;
        if (sps.chroma_format_idc == 3)
            sps.separate_colour_plane = bits.flag();
        sps.bit_depth_luma = 8 + static_cast<int>(bits.ue());
        sps.bit_depth_chroma = 8 + static_cast<int>(bits.ue());
        if (sps.bit_depth_luma > 14 || sps.bit_depth_chroma > 14)
            return std::nullopt;
        sps.qpprime_y_zero_transform_bypass = bits.flag();
        if (bits.flag()) // seq_scaling_matrix_present
            read_scaling_matrix(bits, sps.scaling, sps.chroma_format_idc != 3 ? 8 : 12, nullptr);
    }
    sps.log2_max_frame_num = 4 + static_cast<int>(bits.ue());
    sps.pic_order_cnt_type = static_cast<int>(bits.ue());
    if (sps.log2_max_frame_num > 16 || sps.pic_order_cnt_type > 2)
        return std::nullopt;
    if (sps.pic_order_cnt_type == 0) {
        sps.log2_max_pic_order_cnt_lsb = 4 + static_cast<int>(bits.ue());
        if (sps.log2_max_pic_order_cnt_lsb > 16)
            return std::nullopt;
    } else if (sps.pic_order_cnt_type == 1) {
        sps.delta_pic_order_always_zero = bits.flag();
        sps.offset_for_non_ref_pic = bits.se();
        sps.offset_for_top_to_bottom_field = bits.se();
        std::uint32_t const count = bits.ue();
        if (count > 255)
            return std::nullopt;
        for (std::uint32_t i = 0; i < count; ++i)
            sps.offset_for_ref_frame.push_back(bits.se());
    }
    sps.max_num_ref_frames = static_cast<int>(bits.ue());
    sps.gaps_in_frame_num_allowed = bits.flag();
    sps.pic_width_in_mbs = static_cast<int>(bits.ue()) + 1;
    sps.pic_height_in_map_units = static_cast<int>(bits.ue()) + 1;
    if (sps.pic_width_in_mbs > 1024 || sps.pic_height_in_map_units > 1024)
        return std::nullopt;
    sps.frame_mbs_only = bits.flag();
    if (!sps.frame_mbs_only)
        sps.mb_adaptive_frame_field = bits.flag();
    sps.direct_8x8_inference = bits.flag();
    sps.frame_cropping = bits.flag();
    if (sps.frame_cropping) {
        sps.crop_left = static_cast<int>(bits.ue());
        sps.crop_right = static_cast<int>(bits.ue());
        sps.crop_top = static_cast<int>(bits.ue());
        sps.crop_bottom = static_cast<int>(bits.ue());
    }
    if (bits.flag() && !read_vui(bits, sps.vui))
        return std::nullopt;
    if (bits.failed() || sps.width() <= 0 || sps.height() <= 0)
        return std::nullopt;
    return sps;
}

std::optional<H264Pps> parse_h264_pps(std::span<std::uint8_t const> nal, H264Sps const* sps)
{
    if (nal.empty() || (nal[0] & 31) != 8)
        return std::nullopt;
    std::vector<std::uint8_t> const rbsp = h264_rbsp(nal);
    Bits bits(rbsp);
    H264Pps pps;
    pps.id = static_cast<int>(bits.ue());
    pps.sps_id = static_cast<int>(bits.ue());
    if (pps.id > 255 || pps.sps_id > 31)
        return std::nullopt;
    pps.entropy_coding_mode = bits.flag();
    pps.bottom_field_pic_order_in_frame_present = bits.flag();
    pps.num_slice_groups = static_cast<int>(bits.ue()) + 1;
    if (pps.num_slice_groups > 8)
        return std::nullopt;
    if (pps.num_slice_groups > 1) {
        // 7.3.2.2: slice groups (FMO), read past; the baseline's only.
        pps.slice_group_map_type = static_cast<int>(bits.ue());
        if (pps.slice_group_map_type == 0) {
            for (int i = 0; i < pps.num_slice_groups; ++i)
                bits.ue(); // run_length_minus1
        } else if (pps.slice_group_map_type == 2) {
            for (int i = 0; i < pps.num_slice_groups - 1; ++i) {
                bits.ue(); // top_left
                bits.ue(); // bottom_right
            }
        } else if (pps.slice_group_map_type >= 3 && pps.slice_group_map_type <= 5) {
            bits.flag(); // slice_group_change_direction
            bits.ue(); // slice_group_change_rate_minus1
        } else if (pps.slice_group_map_type == 6) {
            std::uint32_t const count = bits.ue() + 1;
            if (count > 1u << 20)
                return std::nullopt;
            int width = 1;
            while ((1u << width) < static_cast<std::uint32_t>(pps.num_slice_groups))
                ++width;
            for (std::uint32_t i = 0; i < count; ++i)
                bits.u(width);
        }
    }
    pps.num_ref_idx_l0_default_active = static_cast<int>(bits.ue()) + 1;
    pps.num_ref_idx_l1_default_active = static_cast<int>(bits.ue()) + 1;
    if (pps.num_ref_idx_l0_default_active > 32 || pps.num_ref_idx_l1_default_active > 32)
        return std::nullopt;
    pps.weighted_pred = bits.flag();
    pps.weighted_bipred_idc = static_cast<int>(bits.u(2));
    pps.pic_init_qp = 26 + bits.se();
    pps.pic_init_qs = 26 + bits.se();
    pps.chroma_qp_index_offset = bits.se();
    pps.deblocking_filter_control_present = bits.flag();
    pps.constrained_intra_pred = bits.flag();
    pps.redundant_pic_cnt_present = bits.flag();
    pps.second_chroma_qp_index_offset = pps.chroma_qp_index_offset;
    pps.scaling = sps ? sps->scaling : H264ScalingLists {};
    if (!sps)
        flat_scaling(pps.scaling);
    if (bits.more_rbsp_data()) {
        pps.transform_8x8_mode = bits.flag();
        pps.pic_scaling_matrix_present = bits.flag();
        if (pps.pic_scaling_matrix_present) {
            int const chroma = sps ? sps->chroma_format_idc : 1;
            H264ScalingLists lists;
            flat_scaling(lists);
            read_scaling_matrix(bits, lists, 6 + (chroma != 3 ? 2 : 6) * (pps.transform_8x8_mode ? 1 : 0),
                sps && sps->scaling.present ? &sps->scaling : nullptr);
            pps.scaling = lists;
        }
        pps.second_chroma_qp_index_offset = bits.se();
    }
    if (bits.failed())
        return std::nullopt;
    return pps;
}

// --- Slice headers -----------------------------------------------------------

bool H264SliceHeader::has_mmco5() const
{
    return std::any_of(mmco.begin(), mmco.end(), [](H264Mmco const& m) { return m.op == 5; });
}

std::optional<H264SliceHeader> parse_h264_slice_header(std::span<std::uint8_t const> nal, H264Sps const& sps, H264Pps const& pps)
{
    if (nal.empty())
        return std::nullopt;
    H264SliceHeader h;
    h.nal_type = nal[0] & 31;
    h.nal_ref_idc = (nal[0] >> 5) & 3;
    if (h.nal_type != 1 && h.nal_type != 5)
        return std::nullopt;
    h.idr = h.nal_type == 5;
    std::vector<std::uint8_t> const rbsp = h264_rbsp(nal);
    Bits bits(rbsp);
    h.first_mb_in_slice = static_cast<int>(bits.ue());
    std::uint32_t const slice_type = bits.ue();
    if (slice_type > 9)
        return std::nullopt;
    h.slice_type = static_cast<H264SliceType>(slice_type % 5);
    h.pps_id = static_cast<int>(bits.ue());
    if (sps.separate_colour_plane)
        h.colour_plane_id = static_cast<int>(bits.u(2));
    h.frame_num = static_cast<int>(bits.u(sps.log2_max_frame_num));
    if (!sps.frame_mbs_only) {
        h.field_pic = bits.flag();
        if (h.field_pic)
            h.bottom_field = bits.flag();
    }
    if (h.idr)
        h.idr_pic_id = static_cast<int>(bits.ue());
    if (sps.pic_order_cnt_type == 0) {
        h.pic_order_cnt_lsb = static_cast<int>(bits.u(sps.log2_max_pic_order_cnt_lsb));
        if (pps.bottom_field_pic_order_in_frame_present && !h.field_pic)
            h.delta_pic_order_cnt_bottom = bits.se();
    }
    if (sps.pic_order_cnt_type == 1 && !sps.delta_pic_order_always_zero) {
        h.delta_pic_order_cnt[0] = bits.se();
        if (pps.bottom_field_pic_order_in_frame_present && !h.field_pic)
            h.delta_pic_order_cnt[1] = bits.se();
    }
    if (pps.redundant_pic_cnt_present)
        h.redundant_pic_cnt = static_cast<int>(bits.ue());
    bool const is_b = h.slice_type == H264SliceType::B;
    bool const is_p = h.slice_type == H264SliceType::P || h.slice_type == H264SliceType::SP;
    if (is_b)
        h.direct_spatial_mv_pred = bits.flag();
    h.num_ref_idx_l0_active = pps.num_ref_idx_l0_default_active;
    h.num_ref_idx_l1_active = pps.num_ref_idx_l1_default_active;
    if (is_p || is_b) {
        h.num_ref_idx_active_override = bits.flag();
        if (h.num_ref_idx_active_override) {
            h.num_ref_idx_l0_active = static_cast<int>(bits.ue()) + 1;
            if (is_b)
                h.num_ref_idx_l1_active = static_cast<int>(bits.ue()) + 1;
        }
    }
    if (h.num_ref_idx_l0_active > 32 || h.num_ref_idx_l1_active > 32)
        return std::nullopt;
    // 7.3.3.1: the reference lists' modifications, passed over.
    if (h.slice_type != H264SliceType::I && h.slice_type != H264SliceType::SI) {
        for (int list = 0; list < (is_b ? 2 : 1); ++list) {
            if (!bits.flag())
                continue;
            for (int guard = 0; guard < 66; ++guard) {
                std::uint32_t const idc = bits.ue();
                if (idc == 3)
                    break;
                if (idc > 5 || bits.failed())
                    return std::nullopt;
                bits.ue(); // abs_diff_pic_num_minus1 or long_term_pic_num (or view index)
            }
        }
    }
    // 7.3.3.2: the prediction weights, passed over.
    if ((pps.weighted_pred && is_p) || (pps.weighted_bipred_idc == 1 && is_b)) {
        bits.ue(); // luma_log2_weight_denom
        bool const chroma = sps.chroma_format_idc != 0 && !sps.separate_colour_plane;
        if (chroma)
            bits.ue(); // chroma_log2_weight_denom
        for (int list = 0; list < (is_b ? 2 : 1); ++list) {
            int const count = list == 0 ? h.num_ref_idx_l0_active : h.num_ref_idx_l1_active;
            for (int i = 0; i < count; ++i) {
                if (bits.flag()) {
                    bits.se();
                    bits.se();
                }
                if (chroma && bits.flag()) {
                    for (int j = 0; j < 2; ++j) {
                        bits.se();
                        bits.se();
                    }
                }
            }
        }
    }
    // 7.3.3.3: how the buffer is marked after this picture.
    if (h.nal_ref_idc != 0) {
        if (h.idr) {
            h.no_output_of_prior_pics = bits.flag();
            h.long_term_reference = bits.flag();
        } else {
            h.adaptive_ref_pic_marking = bits.flag();
            if (h.adaptive_ref_pic_marking) {
                for (int guard = 0; guard < 66; ++guard) {
                    H264Mmco m;
                    m.op = static_cast<int>(bits.ue());
                    if (m.op == 0)
                        break;
                    if (m.op > 6 || bits.failed())
                        return std::nullopt;
                    if (m.op == 1 || m.op == 3)
                        m.difference_of_pic_nums = static_cast<int>(bits.ue()) + 1;
                    if (m.op == 2)
                        m.long_term_pic_num = static_cast<int>(bits.ue());
                    if (m.op == 3 || m.op == 6)
                        m.long_term_frame_idx = static_cast<int>(bits.ue());
                    if (m.op == 4)
                        m.max_long_term_frame_idx_plus1 = static_cast<int>(bits.ue());
                    h.mmco.push_back(m);
                }
            }
        }
    }
    if (pps.entropy_coding_mode && h.slice_type != H264SliceType::I && h.slice_type != H264SliceType::SI)
        h.cabac_init_idc = static_cast<int>(bits.ue());
    h.slice_qp_delta = bits.se();
    if (h.slice_type == H264SliceType::SP || h.slice_type == H264SliceType::SI) {
        if (h.slice_type == H264SliceType::SP)
            bits.flag(); // sp_for_switch_flag
        bits.se(); // slice_qs_delta
    }
    if (pps.deblocking_filter_control_present) {
        h.disable_deblocking_filter_idc = static_cast<int>(bits.ue());
        if (h.disable_deblocking_filter_idc != 1) {
            h.slice_alpha_c0_offset_div2 = bits.se();
            h.slice_beta_offset_div2 = bits.se();
        }
    }
    if (pps.num_slice_groups > 1 && pps.slice_group_map_type >= 3 && pps.slice_group_map_type <= 5) {
        // Ceil(Log2(PicSizeInMapUnits ÷ SliceGroupChangeRate + 1)) bits: the
        // rate is not kept, so the field is counted as the most it can be.
        int const map_units = sps.pic_width_in_mbs * sps.pic_height_in_map_units;
        int width = 0;
        while ((1 << width) < map_units + 1)
            ++width;
        bits.u(width);
    }
    if (bits.failed())
        return std::nullopt;
    h.header_bits = static_cast<int>(bits.position()) + 8;
    return h;
}

// --- 8.2.1: picture order counts ------------------------------------------

void H264PocState::reset()
{
    *this = H264PocState {};
}

H264PocState::Poc H264PocState::next(H264SliceHeader const& s, H264Sps const& sps)
{
    Poc poc;
    int const max_frame_num = 1 << sps.log2_max_frame_num;
    if (sps.pic_order_cnt_type == 0) {
        // 8.2.1.1
        int const max_lsb = 1 << sps.log2_max_pic_order_cnt_lsb;
        int prev_msb = m_prev_poc_msb;
        int prev_lsb = m_prev_poc_lsb;
        if (s.idr) {
            prev_msb = 0;
            prev_lsb = 0;
        } else if (m_prev_had_mmco5) {
            prev_msb = 0;
            prev_lsb = m_prev_top_for_mmco5; // a frame's: its top field count
        }
        int msb;
        if (s.pic_order_cnt_lsb < prev_lsb && prev_lsb - s.pic_order_cnt_lsb >= max_lsb / 2)
            msb = prev_msb + max_lsb;
        else if (s.pic_order_cnt_lsb > prev_lsb && s.pic_order_cnt_lsb - prev_lsb > max_lsb / 2)
            msb = prev_msb - max_lsb;
        else
            msb = prev_msb;
        if (!s.field_pic || !s.bottom_field)
            poc.top = msb + s.pic_order_cnt_lsb;
        if (!s.field_pic)
            poc.bottom = poc.top + s.delta_pic_order_cnt_bottom;
        else if (s.bottom_field)
            poc.bottom = msb + s.pic_order_cnt_lsb;
        else
            poc.bottom = poc.top;
        if (s.nal_ref_idc != 0) {
            if (s.has_mmco5()) {
                m_prev_poc_msb = 0;
                m_prev_poc_lsb = 0; // 8.2.1: tempPicOrderCnt, set below as the top count
                m_prev_had_mmco5 = true;
                m_prev_top_for_mmco5 = s.bottom_field ? 0 : poc.top - std::min(poc.top, poc.bottom);
            } else {
                m_prev_poc_msb = msb;
                m_prev_poc_lsb = s.pic_order_cnt_lsb;
                m_prev_had_mmco5 = false;
            }
        }
    } else {
        // 8.2.1.2 and 8.2.1.3, from the frame number's offset.
        int frame_num_offset;
        if (s.idr)
            frame_num_offset = 0;
        else if (m_prev_frame_num > s.frame_num)
            frame_num_offset = (m_prev_had_mmco5 ? 0 : m_prev_frame_num_offset) + max_frame_num;
        else
            frame_num_offset = m_prev_had_mmco5 ? 0 : m_prev_frame_num_offset;
        if (sps.pic_order_cnt_type == 1) {
            int const cycle = static_cast<int>(sps.offset_for_ref_frame.size());
            int abs_frame_num = cycle != 0 ? frame_num_offset + s.frame_num : 0;
            if (s.nal_ref_idc == 0 && abs_frame_num > 0)
                --abs_frame_num;
            int expected = 0;
            if (abs_frame_num > 0) {
                int const cycle_count = (abs_frame_num - 1) / cycle;
                int const in_cycle = (abs_frame_num - 1) % cycle;
                int per_cycle = 0;
                for (int v : sps.offset_for_ref_frame)
                    per_cycle += v;
                expected = cycle_count * per_cycle;
                for (int i = 0; i <= in_cycle; ++i)
                    expected += sps.offset_for_ref_frame[static_cast<std::size_t>(i)];
            }
            if (s.nal_ref_idc == 0)
                expected += sps.offset_for_non_ref_pic;
            if (!s.field_pic) {
                poc.top = expected + s.delta_pic_order_cnt[0];
                poc.bottom = poc.top + sps.offset_for_top_to_bottom_field + s.delta_pic_order_cnt[1];
            } else if (!s.bottom_field) {
                poc.top = expected + s.delta_pic_order_cnt[0];
                poc.bottom = poc.top;
            } else {
                poc.bottom = expected + sps.offset_for_top_to_bottom_field + s.delta_pic_order_cnt[0];
                poc.top = poc.bottom;
            }
        } else {
            int temp;
            if (s.idr)
                temp = 0;
            else if (s.nal_ref_idc == 0)
                temp = 2 * (frame_num_offset + s.frame_num) - 1;
            else
                temp = 2 * (frame_num_offset + s.frame_num);
            poc.top = temp;
            poc.bottom = temp;
        }
        m_prev_frame_num_offset = frame_num_offset;
        m_prev_frame_num = s.frame_num;
        m_prev_had_mmco5 = s.nal_ref_idc != 0 && s.has_mmco5();
        if (m_prev_had_mmco5)
            m_prev_frame_num = 0;
    }
    if (sps.pic_order_cnt_type == 0) {
        m_prev_frame_num = s.has_mmco5() && s.nal_ref_idc != 0 ? 0 : s.frame_num;
    }
    return poc;
}

}
