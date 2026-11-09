#include "media/linux/H264Accelerators.h"
#include "media/linux/VulkanVideo.h"

// H.264 through Vulkan Video (VK_KHR_video_decode_h264) on the shared
// device (VulkanVideo.h): the parameter sets go to the session as
// session parameters, each picture's slices go in the bitstream buffer
// with a start code before each (as the extension reads them), the
// picture's own facts and its references' in the structures the video
// std headers lay out (measured, and held by the asserts), and the
// picture decodes into the buffer slot the host chose.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

namespace sashfold::media {

namespace {

using namespace vulkan;

// vulkan_video_codec_h264std.h and its decode companion.
struct StdVideoH264ScalingLists {
    std::uint16_t scaling_list_present_mask;
    std::uint16_t use_default_scaling_matrix_mask;
    std::uint8_t ScalingList4x4[6][16];
    std::uint8_t ScalingList8x8[6][64];
};

struct StdVideoH264SequenceParameterSet {
    std::uint32_t flags; // constraint_set0..5 bits 0-5, direct_8x8_inference 6, mb_adaptive_frame_field 7,
                         // frame_mbs_only 8, delta_pic_order_always_zero 9, separate_colour_plane 10,
                         // gaps_in_frame_num_value_allowed 11, qpprime_y_zero_transform_bypass 12,
                         // frame_cropping 13, seq_scaling_matrix_present 14, vui_parameters_present 15
    std::int32_t profile_idc;
    std::int32_t level_idc;
    std::int32_t chroma_format_idc;
    std::uint8_t seq_parameter_set_id;
    std::uint8_t bit_depth_luma_minus8;
    std::uint8_t bit_depth_chroma_minus8;
    std::uint8_t log2_max_frame_num_minus4;
    std::int32_t pic_order_cnt_type;
    std::int32_t offset_for_non_ref_pic;
    std::int32_t offset_for_top_to_bottom_field;
    std::uint8_t log2_max_pic_order_cnt_lsb_minus4;
    std::uint8_t num_ref_frames_in_pic_order_cnt_cycle;
    std::uint8_t max_num_ref_frames;
    std::uint8_t reserved1;
    std::uint32_t pic_width_in_mbs_minus1;
    std::uint32_t pic_height_in_map_units_minus1;
    std::uint32_t frame_crop_left_offset;
    std::uint32_t frame_crop_right_offset;
    std::uint32_t frame_crop_top_offset;
    std::uint32_t frame_crop_bottom_offset;
    std::uint32_t reserved2;
    std::int32_t const* pOffsetForRefFrame;
    StdVideoH264ScalingLists const* pScalingLists;
    void const* pSequenceParameterSetVui;
};

struct StdVideoH264PictureParameterSet {
    std::uint32_t flags; // transform_8x8_mode 0, redundant_pic_cnt_present 1, constrained_intra_pred 2,
                         // deblocking_filter_control_present 3, weighted_pred 4,
                         // bottom_field_pic_order_in_frame_present 5, entropy_coding_mode 6, pic_scaling_matrix_present 7
    std::uint8_t seq_parameter_set_id;
    std::uint8_t pic_parameter_set_id;
    std::uint8_t num_ref_idx_l0_default_active_minus1;
    std::uint8_t num_ref_idx_l1_default_active_minus1;
    std::int32_t weighted_bipred_idc;
    std::int8_t pic_init_qp_minus26;
    std::int8_t pic_init_qs_minus26;
    std::int8_t chroma_qp_index_offset;
    std::int8_t second_chroma_qp_index_offset;
    StdVideoH264ScalingLists const* pScalingLists;
};

struct StdVideoDecodeH264PictureInfo {
    std::uint32_t flags; // field_pic 0x1, is_intra 0x2, IdrPicFlag 0x4, bottom_field 0x8, is_reference 0x10, complementary_field_pair 0x20
    std::uint8_t seq_parameter_set_id;
    std::uint8_t pic_parameter_set_id;
    std::uint8_t reserved1;
    std::uint8_t reserved2;
    std::uint16_t frame_num;
    std::uint16_t idr_pic_id;
    std::int32_t PicOrderCnt[2];
};

struct StdVideoDecodeH264ReferenceInfo {
    std::uint32_t flags; // top_field 0x1, bottom_field 0x2, used_for_long_term_reference 0x4, is_non_existing 0x8
    std::uint16_t FrameNum;
    std::uint16_t reserved;
    std::int32_t PicOrderCnt[2];
};

struct VkVideoDecodeH264ProfileInfoKHR {
    VkStructureType sType;
    void const* pNext;
    std::int32_t stdProfileIdc;
    VkFlags pictureLayout;
};

struct VkVideoDecodeH264CapabilitiesKHR {
    VkStructureType sType;
    void* pNext;
    std::int32_t maxLevelIdc;
    VkOffset2D fieldOffsetGranularity;
};

struct VkVideoDecodeH264SessionParametersAddInfoKHR {
    VkStructureType sType;
    void const* pNext;
    std::uint32_t stdSPSCount;
    StdVideoH264SequenceParameterSet const* pStdSPSs;
    std::uint32_t stdPPSCount;
    StdVideoH264PictureParameterSet const* pStdPPSs;
};

struct VkVideoDecodeH264SessionParametersCreateInfoKHR {
    VkStructureType sType;
    void const* pNext;
    std::uint32_t maxStdSPSCount;
    std::uint32_t maxStdPPSCount;
    VkVideoDecodeH264SessionParametersAddInfoKHR const* pParametersAddInfo;
};

struct VkVideoDecodeH264PictureInfoKHR {
    VkStructureType sType;
    void const* pNext;
    StdVideoDecodeH264PictureInfo const* pStdPictureInfo;
    std::uint32_t sliceCount;
    std::uint32_t const* pSliceOffsets;
};

struct VkVideoDecodeH264DpbSlotInfoKHR {
    VkStructureType sType;
    void const* pNext;
    StdVideoDecodeH264ReferenceInfo const* pStdReferenceInfo;
};

static_assert(sizeof(StdVideoH264ScalingLists) == 484 && offsetof(StdVideoH264ScalingLists, ScalingList8x8) == 100, "H.264's scaling lists");
static_assert(sizeof(StdVideoH264SequenceParameterSet) == 88 && offsetof(StdVideoH264SequenceParameterSet, pic_order_cnt_type) == 20
        && offsetof(StdVideoH264SequenceParameterSet, pic_width_in_mbs_minus1) == 36 && offsetof(StdVideoH264SequenceParameterSet, pOffsetForRefFrame) == 64,
    "H.264's sequence parameter set");
static_assert(sizeof(StdVideoH264PictureParameterSet) == 24 && offsetof(StdVideoH264PictureParameterSet, weighted_bipred_idc) == 8
        && offsetof(StdVideoH264PictureParameterSet, pScalingLists) == 16,
    "H.264's picture parameter set");
static_assert(sizeof(StdVideoDecodeH264PictureInfo) == 20 && offsetof(StdVideoDecodeH264PictureInfo, PicOrderCnt) == 12, "H.264's picture information");
static_assert(sizeof(StdVideoDecodeH264ReferenceInfo) == 16 && offsetof(StdVideoDecodeH264ReferenceInfo, PicOrderCnt) == 8, "H.264's reference information");
static_assert(sizeof(VkVideoDecodeH264ProfileInfoKHR) == 24 && sizeof(VkVideoDecodeH264CapabilitiesKHR) == 32, "Vulkan's H.264 profile and capabilities");
static_assert(sizeof(VkVideoDecodeH264SessionParametersAddInfoKHR) == 48 && sizeof(VkVideoDecodeH264SessionParametersCreateInfoKHR) == 32
        && offsetof(VkVideoDecodeH264SessionParametersCreateInfoKHR, pParametersAddInfo) == 24,
    "Vulkan's H.264 session parameters");
static_assert(sizeof(VkVideoDecodeH264PictureInfoKHR) == 40 && offsetof(VkVideoDecodeH264PictureInfoKHR, pSliceOffsets) == 32
        && sizeof(VkVideoDecodeH264DpbSlotInfoKHR) == 24,
    "Vulkan's H.264 picture and slot information");

// Sixteen references and the picture being decoded.
constexpr std::uint32_t picture_count = 17;

// H.264 High profile (which reads Main and Baseline streams too) at 8
// bits, 4:2:0, progressive.
struct Profile {
    Profile() = default;
    Profile(Profile const&) = delete;
    Profile& operator=(Profile const&) = delete;

    VkVideoDecodeH264ProfileInfoKHR h264 { st_video_decode_h264_profile_info, nullptr, 100, 0 };
    VkVideoProfileInfoKHR profile { st_video_profile_info, &h264, codec_decode_h264, chroma_subsampling_420, component_depth_8, component_depth_8 };
    VkVideoProfileListInfoKHR list { st_video_profile_list_info, nullptr, 1, &profile };
    VkVideoDecodeH264CapabilitiesKHR capabilities { st_video_decode_h264_capabilities, nullptr, 0, { 0, 0 } };
};

class VulkanH264 final : public H264Accelerator {
public:
    ~VulkanH264() override { m_device.destroy_parameters(m_parameters); }

    bool open(std::string& error)
    {
        Codec codec;
        codec.name = "H.264";
        codec.operation = codec_decode_h264;
        codec.extension = "VK_KHR_video_decode_h264";
        codec.std_header = "VK_STD_vulkan_video_codec_h264_decode";
        codec.std_version = h264_std_version_1_0_0;
        codec.profile = &m_profile.profile;
        codec.profile_list = &m_profile.list;
        codec.features = nullptr; // H.264 decoding has no feature bit of its own
        codec.codec_capabilities = &m_profile.capabilities;
        codec.pictures = picture_count;
        codec.active_references = 16;
        return m_device.open(codec, error);
    }

    std::string const& device() const override { return m_device.name(); }
    // NVIDIA's driver (seen on 615.78.08): see H264Accelerator.
    bool mishandles_long_term_and_wrap() const override { return m_device.name().find("NVIDIA") != std::string::npos; }
    int slots() const override { return static_cast<int>(picture_count); }

    void reset() override { m_device.request_reset(); }

    bool set_parameter_sets(std::vector<H264Sps> const& sps, std::vector<H264Pps> const& pps) override
    {
        m_sps_std.clear();
        m_pps_std.clear();
        m_scaling.clear();
        m_offsets.clear();
        m_scaling.reserve(sps.size() + pps.size());
        m_offsets.reserve(sps.size());
        for (H264Sps const& s : sps) {
            StdVideoH264SequenceParameterSet out {};
            out.flags = static_cast<std::uint32_t>((s.constraint_set_flags >> 7) & 1) | static_cast<std::uint32_t>(((s.constraint_set_flags >> 6) & 1) << 1)
                | static_cast<std::uint32_t>(((s.constraint_set_flags >> 5) & 1) << 2) | static_cast<std::uint32_t>(((s.constraint_set_flags >> 4) & 1) << 3)
                | static_cast<std::uint32_t>(((s.constraint_set_flags >> 3) & 1) << 4) | static_cast<std::uint32_t>(((s.constraint_set_flags >> 2) & 1) << 5)
                | (s.direct_8x8_inference ? 1u << 6 : 0) | (s.mb_adaptive_frame_field ? 1u << 7 : 0) | (s.frame_mbs_only ? 1u << 8 : 0)
                | (s.delta_pic_order_always_zero ? 1u << 9 : 0) | (s.separate_colour_plane ? 1u << 10 : 0)
                | (s.gaps_in_frame_num_allowed ? 1u << 11 : 0) | (s.qpprime_y_zero_transform_bypass ? 1u << 12 : 0)
                | (s.frame_cropping ? 1u << 13 : 0) | (s.scaling.present ? 1u << 14 : 0);
            // The VUI is not handed over (it says nothing a decoder needs).
            out.profile_idc = s.profile_idc;
            out.level_idc = s.level_idc;
            out.chroma_format_idc = s.chroma_format_idc;
            out.seq_parameter_set_id = static_cast<std::uint8_t>(s.id);
            out.bit_depth_luma_minus8 = static_cast<std::uint8_t>(s.bit_depth_luma - 8);
            out.bit_depth_chroma_minus8 = static_cast<std::uint8_t>(s.bit_depth_chroma - 8);
            out.log2_max_frame_num_minus4 = static_cast<std::uint8_t>(s.log2_max_frame_num - 4);
            out.pic_order_cnt_type = s.pic_order_cnt_type;
            out.offset_for_non_ref_pic = s.offset_for_non_ref_pic;
            out.offset_for_top_to_bottom_field = s.offset_for_top_to_bottom_field;
            out.log2_max_pic_order_cnt_lsb_minus4 = static_cast<std::uint8_t>(s.log2_max_pic_order_cnt_lsb - 4);
            out.num_ref_frames_in_pic_order_cnt_cycle = static_cast<std::uint8_t>(s.offset_for_ref_frame.size());
            out.max_num_ref_frames = static_cast<std::uint8_t>(s.max_num_ref_frames);
            out.pic_width_in_mbs_minus1 = static_cast<std::uint32_t>(s.pic_width_in_mbs - 1);
            out.pic_height_in_map_units_minus1 = static_cast<std::uint32_t>(s.pic_height_in_map_units - 1);
            out.frame_crop_left_offset = static_cast<std::uint32_t>(s.crop_left);
            out.frame_crop_right_offset = static_cast<std::uint32_t>(s.crop_right);
            out.frame_crop_top_offset = static_cast<std::uint32_t>(s.crop_top);
            out.frame_crop_bottom_offset = static_cast<std::uint32_t>(s.crop_bottom);
            m_offsets.push_back(std::vector<std::int32_t>(s.offset_for_ref_frame.begin(), s.offset_for_ref_frame.end()));
            out.pOffsetForRefFrame = m_offsets.back().empty() ? nullptr : m_offsets.back().data();
            m_scaling.push_back(scaling_of(s.scaling));
            out.pScalingLists = &m_scaling.back();
            m_sps_std.push_back(out);
        }
        for (H264Pps const& p : pps) {
            StdVideoH264PictureParameterSet out {};
            out.flags = (p.transform_8x8_mode ? 1u : 0) | (p.redundant_pic_cnt_present ? 1u << 1 : 0) | (p.constrained_intra_pred ? 1u << 2 : 0)
                | (p.deblocking_filter_control_present ? 1u << 3 : 0) | (p.weighted_pred ? 1u << 4 : 0)
                | (p.bottom_field_pic_order_in_frame_present ? 1u << 5 : 0) | (p.entropy_coding_mode ? 1u << 6 : 0)
                | (p.pic_scaling_matrix_present ? 1u << 7 : 0);
            out.seq_parameter_set_id = static_cast<std::uint8_t>(p.sps_id);
            out.pic_parameter_set_id = static_cast<std::uint8_t>(p.id);
            out.num_ref_idx_l0_default_active_minus1 = static_cast<std::uint8_t>(p.num_ref_idx_l0_default_active - 1);
            out.num_ref_idx_l1_default_active_minus1 = static_cast<std::uint8_t>(p.num_ref_idx_l1_default_active - 1);
            out.weighted_bipred_idc = p.weighted_bipred_idc;
            out.pic_init_qp_minus26 = static_cast<std::int8_t>(p.pic_init_qp - 26);
            out.pic_init_qs_minus26 = static_cast<std::int8_t>(p.pic_init_qs - 26);
            out.chroma_qp_index_offset = static_cast<std::int8_t>(p.chroma_qp_index_offset);
            out.second_chroma_qp_index_offset = static_cast<std::int8_t>(p.second_chroma_qp_index_offset);
            m_scaling.push_back(scaling_of(p.scaling));
            out.pScalingLists = &m_scaling.back();
            m_pps_std.push_back(out);
        }
        m_parameters_stale = true;
        return !m_sps_std.empty() && !m_pps_std.empty();
    }

    bool decode(H264PictureInfo const& picture) override
    {
        if (m_device.lost() || !picture.sps || !picture.pps || !picture.first_slice || picture.slices.empty())
            return false;
        H264Sps const& sps = *picture.sps;
        if (picture.slot < 0 || picture.slot >= static_cast<int>(picture_count))
            return false;
        auto const width = static_cast<std::uint32_t>(sps.coded_width());
        auto const height = static_cast<std::uint32_t>(sps.coded_height());
        // Pictures as large as the sequence needs: a larger one starts
        // them afresh, which an IDR may.
        if (m_device.has_session() && (width > m_device.extent().width || height > m_device.extent().height) && !picture.idr)
            return false;
        bool recreated = false;
        if (!m_device.ensure_session(width, height, recreated))
            return false;
        if (recreated) {
            m_device.destroy_parameters(m_parameters);
            m_parameters = 0;
            m_parameters_stale = true;
        }
        if (m_parameters_stale && !make_parameters())
            return false;

        // The slices, each after a start code, the bitstream padded to the
        // size the decoder reads in.
        std::size_t total = 0;
        for (auto const& slice : picture.slices)
            total += 3 + slice.size();
        std::uint64_t const range = align_up(total, m_device.capabilities().minBitstreamBufferSizeAlignment);
        if (!m_device.ensure_bitstream(range))
            return false;
        std::vector<std::uint32_t> offsets;
        std::uint8_t* out = m_device.bitstream();
        std::size_t at = 0;
        for (auto const& slice : picture.slices) {
            offsets.push_back(static_cast<std::uint32_t>(at));
            out[at] = 0;
            out[at + 1] = 0;
            out[at + 2] = 1;
            std::memcpy(out + at + 3, slice.data(), slice.size());
            at += 3 + slice.size();
        }
        std::memset(out + at, 0, static_cast<std::size_t>(range - at));

        // The references, each a slot with the facts the decoder reads.
        std::vector<StdVideoDecodeH264ReferenceInfo> reference_infos(picture.references.size());
        std::vector<VkVideoDecodeH264DpbSlotInfoKHR> slot_infos(picture.references.size());
        std::vector<VkVideoReferenceSlotInfoKHR> slots(picture.references.size());
        for (std::size_t i = 0; i < picture.references.size(); ++i) {
            H264Reference const& r = picture.references[i];
            if (r.slot < 0 || r.slot >= static_cast<int>(picture_count) || r.slot == picture.slot)
                return false;
            // A frame's reference: neither field flag (those name a field of a
            // field pair), long-term when it is.
            reference_infos[i] = { r.long_term ? 0x4u : 0u, static_cast<std::uint16_t>(r.frame_num), 0, { r.poc_top, r.poc_bottom } };
            slot_infos[i] = { st_video_decode_h264_dpb_slot_info, nullptr, &reference_infos[i] };
            slots[i] = { st_video_reference_slot_info, &slot_infos[i], r.slot, &m_device.resource(static_cast<std::size_t>(r.slot)) };
        }
        VkVideoPictureResourceInfoKHR& resource = m_device.resource(static_cast<std::size_t>(picture.slot));
        resource.codedExtent = { width, height };
        StdVideoDecodeH264ReferenceInfo const setup_info { picture.long_term ? 0x4u : 0u,
            static_cast<std::uint16_t>(picture.long_term ? picture.long_term_frame_idx : picture.frame_num), 0,
            { picture.poc_top, picture.poc_bottom } };
        VkVideoDecodeH264DpbSlotInfoKHR const setup_slot_info { st_video_decode_h264_dpb_slot_info, nullptr, &setup_info };
        VkVideoReferenceSlotInfoKHR const setup { st_video_reference_slot_info, &setup_slot_info, picture.slot, &resource };
        // Bound for the coding scope: the references, and the target, which
        // has no slot until the decode activates one.
        std::vector<VkVideoReferenceSlotInfoKHR> bound = slots;
        bound.push_back(setup);
        bound.back().slotIndex = -1;
        // A picture nothing will refer to activates no slot: it is decoded
        // into its picture and read back, and the slot stays free.
        bool const keep = picture.is_reference;

        H264SliceHeader const& first = *picture.first_slice;
        StdVideoDecodeH264PictureInfo info_std {};
        info_std.flags = (picture.intra ? 0x2u : 0u) | (picture.idr ? 0x4u : 0u) | (picture.is_reference ? 0x10u : 0u);
        info_std.seq_parameter_set_id = static_cast<std::uint8_t>(sps.id);
        info_std.pic_parameter_set_id = static_cast<std::uint8_t>(picture.pps->id);
        info_std.frame_num = static_cast<std::uint16_t>(picture.frame_num);
        info_std.idr_pic_id = static_cast<std::uint16_t>(first.idr_pic_id);
        info_std.PicOrderCnt[0] = picture.poc_top;
        info_std.PicOrderCnt[1] = picture.poc_bottom;
        VkVideoDecodeH264PictureInfoKHR h264 { st_video_decode_h264_picture_info, nullptr, &info_std, static_cast<std::uint32_t>(offsets.size()),
            offsets.data() };

        VkVideoDecodeInfoKHR info {};
        info.sType = st_video_decode_info;
        info.pNext = &h264;
        info.srcBuffer = m_device.bitstream_buffer();
        info.srcBufferOffset = 0;
        info.srcBufferRange = range;
        info.dstPictureResource = resource;
        info.pSetupReferenceSlot = keep ? &setup : nullptr;
        info.referenceSlotCount = static_cast<std::uint32_t>(slots.size());
        info.pReferenceSlots = slots.data();
        if (!m_device.decode(info, picture.slot, bound, m_parameters))
            return false;
        Picture& decoded = m_device.picture(static_cast<std::size_t>(picture.slot));
        decoded.width = sps.width();
        decoded.height = sps.height();
        decoded.crop_x = sps.frame_cropping ? sps.crop_left * (sps.chroma_format_idc == 3 ? 1 : 2) : 0;
        decoded.crop_y = sps.frame_cropping ? sps.crop_top * 2 : 0;
        return true;
    }

    std::optional<Nv12Picture> read(int slot) override { return m_device.read(slot); }

private:
    static StdVideoH264ScalingLists scaling_of(H264ScalingLists const& lists)
    {
        StdVideoH264ScalingLists out {};
        // The lists in force are handed over whole, each marked present.
        out.scaling_list_present_mask = 0x0FFF;
        out.use_default_scaling_matrix_mask = 0;
        for (std::size_t i = 0; i < 6; ++i)
            std::copy(lists.list_4x4[i].begin(), lists.list_4x4[i].end(), out.ScalingList4x4[i]);
        for (std::size_t i = 0; i < 6; ++i)
            std::copy(lists.list_8x8[i].begin(), lists.list_8x8[i].end(), out.ScalingList8x8[i]);
        return out;
    }

    bool make_parameters()
    {
        m_device.destroy_parameters(m_parameters);
        m_parameters = 0;
        if (m_sps_std.empty() || m_pps_std.empty())
            return false;
        VkVideoDecodeH264SessionParametersAddInfoKHR const add { st_video_decode_h264_session_parameters_add_info, nullptr,
            static_cast<std::uint32_t>(m_sps_std.size()), m_sps_std.data(), static_cast<std::uint32_t>(m_pps_std.size()), m_pps_std.data() };
        VkVideoDecodeH264SessionParametersCreateInfoKHR const create { st_video_decode_h264_session_parameters_create_info, nullptr, 32, 256, &add };
        m_parameters = m_device.create_parameters(&create);
        m_parameters_stale = m_parameters == 0;
        return m_parameters != 0;
    }

    Device m_device;
    Profile m_profile;
    std::vector<StdVideoH264SequenceParameterSet> m_sps_std;
    std::vector<StdVideoH264PictureParameterSet> m_pps_std;
    std::vector<StdVideoH264ScalingLists> m_scaling;
    std::vector<std::vector<std::int32_t>> m_offsets;
    VkVideoSessionParametersKHR m_parameters = 0;
    bool m_parameters_stale = true;
};

}

std::unique_ptr<H264Accelerator> open_vulkan_h264(std::string& error)
{
    auto accelerator = std::make_unique<VulkanH264>();
    if (!accelerator->open(error))
        return nullptr;
    return accelerator;
}

}
