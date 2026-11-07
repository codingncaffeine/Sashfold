#include "media/linux/Vp9Accelerators.h"
#include "media/linux/VulkanVideo.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <vector>

// VP9 through Vulkan Video (VK_KHR_video_decode_vp9): the GPU's video engine
// as NVIDIA's own driver offers it — the only way to it without a
// translation layer the user would have to install — and as Mesa offers
// AMD's and Intel's. The loader (libvulkan.so.1) is opened at run time, as
// the VA-API libraries are, and the types below are Vulkan's own
// (vulkan_core.h and the VP9 video std headers), declared with the layout
// those headers give them on a 64-bit target — measured, and held by the
// asserts — so the build needs none of the headers.
//
// The device is the one with the most to decode with: a discrete GPU
// before an integrated one, never a CPU implementation. Each picture of the
// decoded picture buffer is an image of its own, and the frame decoded is
// written into its reference picture (the DPB and the output coincide, as
// on NVIDIA's decoder); that picture is what read_last() copies out.
// Implementations that keep all references in one layered image, or that
// only write the output apart from the reference, are declined here with
// their reason, and VA-API is tried in their place.

namespace sashfold::media {

namespace {

using namespace vulkan;

// The VP9 video std headers (vulkan_video_codec_vp9std.h and its decode
// companion): the frame header as the application parsed it.
struct StdVideoVP9ColorConfigFlags {
    std::uint32_t color_range : 1;
    std::uint32_t reserved : 31;
};

struct StdVideoVP9ColorConfig {
    StdVideoVP9ColorConfigFlags flags;
    std::uint8_t BitDepth;
    std::uint8_t subsampling_x;
    std::uint8_t subsampling_y;
    std::uint8_t reserved1;
    std::int32_t color_space;
};

struct StdVideoVP9LoopFilterFlags {
    std::uint32_t loop_filter_delta_enabled : 1;
    std::uint32_t loop_filter_delta_update : 1;
    std::uint32_t reserved : 30;
};

struct StdVideoVP9LoopFilter {
    StdVideoVP9LoopFilterFlags flags;
    std::uint8_t loop_filter_level;
    std::uint8_t loop_filter_sharpness;
    std::uint8_t update_ref_delta;
    std::int8_t loop_filter_ref_deltas[4];
    std::uint8_t update_mode_delta;
    std::int8_t loop_filter_mode_deltas[2];
};

struct StdVideoVP9SegmentationFlags {
    std::uint32_t segmentation_update_map : 1;
    std::uint32_t segmentation_temporal_update : 1;
    std::uint32_t segmentation_update_data : 1;
    std::uint32_t segmentation_abs_or_delta_update : 1;
    std::uint32_t reserved : 28;
};

struct StdVideoVP9Segmentation {
    StdVideoVP9SegmentationFlags flags;
    std::uint8_t segmentation_tree_probs[7];
    std::uint8_t segmentation_pred_prob[3];
    std::uint8_t FeatureEnabled[8];
    std::int16_t FeatureData[8][4];
};

struct StdVideoDecodeVP9PictureInfoFlags {
    std::uint32_t error_resilient_mode : 1;
    std::uint32_t intra_only : 1;
    std::uint32_t allow_high_precision_mv : 1;
    std::uint32_t refresh_frame_context : 1;
    std::uint32_t frame_parallel_decoding_mode : 1;
    std::uint32_t segmentation_enabled : 1;
    std::uint32_t show_frame : 1;
    std::uint32_t UsePrevFrameMvs : 1;
    std::uint32_t reserved : 24;
};

struct StdVideoDecodeVP9PictureInfo {
    StdVideoDecodeVP9PictureInfoFlags flags;
    std::int32_t profile;
    std::int32_t frame_type;
    std::uint8_t frame_context_idx;
    std::uint8_t reset_frame_context;
    std::uint8_t refresh_frame_flags;
    std::uint8_t ref_frame_sign_bias_mask;
    std::int32_t interpolation_filter;
    std::uint8_t base_q_idx;
    std::int8_t delta_q_y_dc;
    std::int8_t delta_q_uv_dc;
    std::int8_t delta_q_uv_ac;
    std::uint8_t tile_cols_log2;
    std::uint8_t tile_rows_log2;
    std::uint16_t reserved1[3];
    StdVideoVP9ColorConfig const* pColorConfig;
    StdVideoVP9LoopFilter const* pLoopFilter;
    StdVideoVP9Segmentation const* pSegmentation;
};

struct VkVideoDecodeVP9PictureInfoKHR {
    VkStructureType sType;
    void const* pNext;
    StdVideoDecodeVP9PictureInfo const* pStdPictureInfo;
    std::int32_t referenceNameSlotIndices[3];
    std::uint32_t uncompressedHeaderOffset;
    std::uint32_t compressedHeaderOffset;
    std::uint32_t tilesOffset;
};

// The layout the Vulkan headers give these (1.4.357, measured on x86-64),

static_assert(sizeof(VkVideoDecodeVP9PictureInfoKHR) == 48 && offsetof(VkVideoDecodeVP9PictureInfoKHR, uncompressedHeaderOffset) == 36,
    "Vulkan's VP9 picture information");
static_assert(sizeof(StdVideoVP9ColorConfig) == 12 && sizeof(StdVideoVP9LoopFilter) == 16 && offsetof(StdVideoVP9LoopFilter, update_mode_delta) == 11,
    "VP9's colour configuration and loop filter");
static_assert(sizeof(StdVideoVP9Segmentation) == 88 && offsetof(StdVideoVP9Segmentation, FeatureData) == 22, "VP9's segmentation");
static_assert(sizeof(StdVideoDecodeVP9PictureInfo) == 56 && offsetof(StdVideoDecodeVP9PictureInfo, interpolation_filter) == 16
        && offsetof(StdVideoDecodeVP9PictureInfo, base_q_idx) == 20 && offsetof(StdVideoDecodeVP9PictureInfo, pColorConfig) == 32,
    "VP9's picture information");
static_assert(sizeof(VkPhysicalDeviceSynchronization2Features) == 24 && sizeof(VkPhysicalDeviceVideoDecodeVP9FeaturesKHR) == 24,
    "Vulkan's feature structures");

// Eight references, the frame being decoded, and the frame decoded last,
// which may still be read back while the next is decoded.
constexpr std::uint32_t picture_count = 10;

// VP9 profile 0 at 8 bits, 4:2:0, as every call that names a video profile
// wants it, and the list that carries it to image and buffer creation.
struct Profile {
    Profile() = default;
    Profile(Profile const&) = delete;
    Profile& operator=(Profile const&) = delete;

    VkVideoDecodeVP9ProfileInfoKHR vp9 { st_video_decode_vp9_profile_info, nullptr, 0 };
    VkVideoProfileInfoKHR profile { st_video_profile_info, &vp9, codec_decode_vp9, chroma_subsampling_420, component_depth_8, component_depth_8 };
    VkVideoProfileListInfoKHR list { st_video_profile_list_info, nullptr, 1, &profile };
    VkPhysicalDeviceVideoDecodeVP9FeaturesKHR features { st_physical_device_video_decode_vp9_features, nullptr, 0 };
    VkVideoDecodeVP9CapabilitiesKHR capabilities { st_video_decode_vp9_capabilities, nullptr, 0 };
};

class VulkanVp9 final : public Vp9Accelerator {
public:
    VulkanVp9() { m_slots.fill(-1); }

    bool open(std::string& error)
    {
        Codec codec;
        codec.name = "VP9";
        codec.operation = codec_decode_vp9;
        codec.extension = "VK_KHR_video_decode_vp9";
        codec.std_header = "VK_STD_vulkan_video_codec_vp9_decode";
        codec.std_version = vp9_std_version_1_0_0;
        codec.profile = &m_profile.profile;
        codec.profile_list = &m_profile.list;
        codec.features = &m_profile.features;
        codec.codec_capabilities = &m_profile.capabilities;
        codec.pictures = picture_count;
        codec.active_references = 3;
        return m_device.open(codec, error);
    }

    std::string const& device() const override { return m_device.name(); }

    void reset() override
    {
        m_slots.fill(-1);
        m_last = -1;
        m_previous = {};
        m_device.request_reset();
    }

    bool decode(std::span<std::uint8_t const> frame, Vp9FrameHeader const& header) override
    {
        if (m_device.lost() || header.show_existing_frame || header.profile != 0 || header.bit_depth != 8 || !header.subsampling_x || !header.subsampling_y)
            return false;
        auto const width = static_cast<std::uint32_t>(header.width);
        auto const height = static_cast<std::uint32_t>(header.height);
        // Pictures as large as the stream needs; a key frame of a larger
        // size starts them afresh, which it may, since it needs no
        // references.
        if (m_device.has_session() && (width > m_device.extent().width || height > m_device.extent().height) && !header.key_frame)
            return false;
        bool recreated = false;
        if (!m_device.ensure_session(width, height, recreated))
            return false;
        if (recreated)
            reset();
        int const target = free_picture();
        if (target < 0)
            return false;

        // The references this frame reads, by name, each DPB slot listed
        // once; the slot index of a picture is its index.
        std::array<std::int32_t, 3> names { -1, -1, -1 };
        std::array<VkVideoReferenceSlotInfoKHR, 4> slots {};
        std::uint32_t reference_count = 0;
        if (!header.intra()) {
            for (std::size_t i = 0; i < names.size(); ++i) {
                int const picture = m_slots[static_cast<std::size_t>(header.ref_frame_idx[i])];
                if (picture < 0)
                    return false; // a reference slot never filled: the stream is broken
                names[i] = picture;
                bool const listed = std::any_of(slots.begin(), slots.begin() + reference_count,
                    [picture](VkVideoReferenceSlotInfoKHR const& slot) { return slot.slotIndex == picture; });
                if (!listed)
                    slots[reference_count++] = { st_video_reference_slot_info, nullptr, picture, &m_device.resource(static_cast<std::size_t>(picture)) };
            }
        }
        VkVideoPictureResourceInfoKHR& resource = m_device.resource(static_cast<std::size_t>(target));
        resource.codedExtent = { width, height };
        VkVideoReferenceSlotInfoKHR const setup { st_video_reference_slot_info, nullptr, target, &resource };
        // Bound for the coding scope: the references, and the target, which
        // has no slot until the decode activates one.
        std::array<VkVideoReferenceSlotInfoKHR, 4> bound = slots;
        bound[reference_count] = setup;
        bound[reference_count].slotIndex = -1;

        // The bitstream, padded with zeros to the size the decoder reads in.
        std::uint64_t const range = align_up(frame.size(), m_device.capabilities().minBitstreamBufferSizeAlignment);
        if (!m_device.ensure_bitstream(range))
            return false;
        std::memcpy(m_device.bitstream(), frame.data(), frame.size());
        std::memset(m_device.bitstream() + frame.size(), 0, static_cast<std::size_t>(range - frame.size()));

        StdVideoVP9ColorConfig color {};
        color.flags.color_range = header.color_range;
        color.BitDepth = 8;
        color.subsampling_x = 1;
        color.subsampling_y = 1;
        color.color_space = header.color_space;

        Vp9LoopFilter const& lf = header.loop_filter;
        StdVideoVP9LoopFilter filter {};
        filter.flags.loop_filter_delta_enabled = lf.delta_enabled;
        filter.flags.loop_filter_delta_update = lf.delta_update;
        filter.loop_filter_level = static_cast<std::uint8_t>(lf.level);
        filter.loop_filter_sharpness = static_cast<std::uint8_t>(lf.sharpness);
        for (std::size_t i = 0; i < lf.ref_deltas.size(); ++i) {
            filter.loop_filter_ref_deltas[i] = static_cast<std::int8_t>(lf.ref_deltas[i]);
            if (lf.ref_deltas_updated[i])
                filter.update_ref_delta = static_cast<std::uint8_t>(filter.update_ref_delta | (1u << i));
        }
        for (std::size_t i = 0; i < lf.mode_deltas.size(); ++i) {
            filter.loop_filter_mode_deltas[i] = static_cast<std::int8_t>(lf.mode_deltas[i]);
            if (lf.mode_deltas_updated[i])
                filter.update_mode_delta = static_cast<std::uint8_t>(filter.update_mode_delta | (1u << i));
        }

        Vp9Segmentation const& seg = header.segmentation;
        StdVideoVP9Segmentation segmentation {};
        segmentation.flags.segmentation_update_map = seg.update_map;
        segmentation.flags.segmentation_temporal_update = seg.temporal_update;
        segmentation.flags.segmentation_update_data = seg.update_data;
        segmentation.flags.segmentation_abs_or_delta_update = seg.abs_or_delta_update;
        std::copy(seg.tree_probs.begin(), seg.tree_probs.end(), segmentation.segmentation_tree_probs);
        std::copy(seg.pred_probs.begin(), seg.pred_probs.end(), segmentation.segmentation_pred_prob);
        for (std::size_t s = 0; s < 8; ++s) {
            for (std::size_t f = 0; f < 4; ++f) {
                if (seg.feature_enabled[s][f])
                    segmentation.FeatureEnabled[s] = static_cast<std::uint8_t>(segmentation.FeatureEnabled[s] | (1u << f));
                segmentation.FeatureData[s][f] = static_cast<std::int16_t>(seg.feature_data[s][f]);
            }
        }

        StdVideoDecodeVP9PictureInfo picture {};
        picture.flags.error_resilient_mode = header.error_resilient;
        picture.flags.intra_only = header.intra_only;
        picture.flags.allow_high_precision_mv = header.allow_high_precision_mv;
        picture.flags.refresh_frame_context = header.refresh_frame_context;
        picture.flags.frame_parallel_decoding_mode = header.frame_parallel_decoding;
        picture.flags.segmentation_enabled = seg.enabled;
        picture.flags.show_frame = header.show_frame;
        // The motion vectors of the frame decoded before this one (not one
        // shown again, which decodes nothing) are candidates only when it
        // was shown, had this size, and had motion to give (§7.2). NVIDIA's
        // decoder works this out for itself — forced either way, its
        // pictures do not change (measured 2026-09-23) — so only a driver
        // that reads the flag tests this line.
        picture.flags.UsePrevFrameMvs = !header.intra() && !header.error_resilient && m_previous.decoded && m_previous.shown
            && !m_previous.intra && m_previous.width == header.width && m_previous.height == header.height;
        picture.profile = 0;
        picture.frame_type = header.key_frame ? 0 : 1;
        picture.frame_context_idx = static_cast<std::uint8_t>(header.frame_context_idx);
        picture.reset_frame_context = static_cast<std::uint8_t>(header.reset_frame_context);
        picture.refresh_frame_flags = header.refresh_frame_flags;
        for (std::size_t reference = 1; reference < header.ref_sign_bias.size(); ++reference) {
            if (header.ref_sign_bias[reference])
                picture.ref_frame_sign_bias_mask = static_cast<std::uint8_t>(picture.ref_frame_sign_bias_mask | (1u << reference));
        }
        picture.interpolation_filter = header.interp_filter;
        picture.base_q_idx = static_cast<std::uint8_t>(header.base_q_idx);
        picture.delta_q_y_dc = static_cast<std::int8_t>(header.delta_q_y_dc);
        picture.delta_q_uv_dc = static_cast<std::int8_t>(header.delta_q_uv_dc);
        picture.delta_q_uv_ac = static_cast<std::int8_t>(header.delta_q_uv_ac);
        picture.tile_cols_log2 = static_cast<std::uint8_t>(header.tile_cols_log2);
        picture.tile_rows_log2 = static_cast<std::uint8_t>(header.tile_rows_log2);
        picture.pColorConfig = &color;
        picture.pLoopFilter = &filter;
        picture.pSegmentation = &segmentation;

        VkVideoDecodeVP9PictureInfoKHR vp9 {};
        vp9.sType = st_video_decode_vp9_picture_info;
        vp9.pStdPictureInfo = &picture;
        std::copy(names.begin(), names.end(), vp9.referenceNameSlotIndices);
        vp9.uncompressedHeaderOffset = 0;
        vp9.compressedHeaderOffset = static_cast<std::uint32_t>(header.uncompressed_header_size);
        vp9.tilesOffset = static_cast<std::uint32_t>(header.uncompressed_header_size + header.compressed_header_size);

        VkVideoDecodeInfoKHR info {};
        info.sType = st_video_decode_info;
        info.pNext = &vp9;
        info.srcBuffer = m_device.bitstream_buffer();
        info.srcBufferOffset = 0;
        info.srcBufferRange = range;
        info.dstPictureResource = resource;
        info.pSetupReferenceSlot = &setup;
        info.referenceSlotCount = reference_count;
        info.pReferenceSlots = slots.data();
        if (!m_device.decode(info, target, std::span<VkVideoReferenceSlotInfoKHR const>(bound.data(), reference_count + 1), 0))
            return false;

        Picture& decoded = m_device.picture(static_cast<std::size_t>(target));
        decoded.width = header.width;
        decoded.height = header.height;
        for (std::size_t slot = 0; slot < m_slots.size(); ++slot) {
            if (header.refresh_frame_flags & (1u << slot))
                m_slots[slot] = target;
        }
        m_last = target;
        m_previous = { true, header.show_frame, header.intra(), header.width, header.height };
        return true;
    }

    std::optional<Nv12Picture> read_last() override { return m_last >= 0 ? m_device.read(m_last) : std::nullopt; }

    std::optional<Nv12Picture> read_slot(int slot) override
    {
        if (slot < 0 || slot > 7 || m_slots[static_cast<std::size_t>(slot)] < 0)
            return std::nullopt;
        return m_device.read(m_slots[static_cast<std::size_t>(slot)]);
    }

private:
    // What the frame before told about itself (UsePrevFrameMvs).
    struct Previous {
        bool decoded = false;
        bool shown = false;
        bool intra = false;
        int width = 0;
        int height = 0;
    };

    // A picture no reference slot holds, other than the one decoded last
    // (which may still be read back).
    int free_picture() const
    {
        for (int i = 0; i < static_cast<int>(m_device.picture_count()); ++i) {
            if (i != m_last && std::find(m_slots.begin(), m_slots.end(), i) == m_slots.end())
                return i;
        }
        return -1;
    }

    Device m_device;
    Profile m_profile;
    std::array<int, 8> m_slots {};
    int m_last = -1;
    Previous m_previous;
};

}

std::unique_ptr<Vp9Accelerator> open_vulkan_vp9(std::string& error)
{
    auto accelerator = std::make_unique<VulkanVp9>();
    if (!accelerator->open(error))
        return nullptr;
    return accelerator;
}

}
