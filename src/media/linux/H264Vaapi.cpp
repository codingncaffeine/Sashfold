#include "media/linux/H264Accelerators.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

// H.264 through VA-API, the way VP9 goes through it (media/linux/Vp9Vaapi.cpp
// has the loading and the render-node search): libva and its DRM front
// loaded at run time, the types below VA-API's own (va.h), their layout
// measured against the installed header and pinned by the static
// assertions. What VA-API wants beyond what Vulkan Video wants is the
// reference picture LISTS of each slice (8.2.4), built here from the
// buffer's references and the slice header's modifications, and the
// explicit prediction weights, read by the header parser.

namespace sashfold::media {

namespace {

using VADisplay = void*;
using VAStatus = int;
using VAGenericID = unsigned int;
using VAConfigID = VAGenericID;
using VAContextID = VAGenericID;
using VASurfaceID = VAGenericID;
using VABufferID = VAGenericID;
using VAImageID = VAGenericID;
using VAMessageCallback = void (*)(void* user, char const* message);

constexpr VAStatus va_success = 0;
constexpr VAGenericID va_invalid_id = 0xffffffffu;
constexpr int va_profile_h264_main = 6;
constexpr int va_profile_h264_high = 7;
constexpr int va_profile_h264_constrained_baseline = 13;
constexpr int va_entrypoint_vld = 1;
constexpr int va_config_attrib_rt_format = 0;
constexpr unsigned va_rt_format_yuv420 = 0x00000001;
constexpr int va_progressive = 0x1;
constexpr int va_picture_parameter_buffer = 0;
constexpr int va_iq_matrix_buffer = 1;
constexpr int va_slice_parameter_buffer = 4;
constexpr int va_slice_data_buffer = 5;
constexpr std::uint32_t va_slice_data_flag_all = 0x00;
constexpr std::uint32_t va_fourcc_nv12 = 0x3231564E;
constexpr std::uint32_t va_picture_h264_invalid = 0x1;
constexpr std::uint32_t va_picture_h264_short_term = 0x8;
constexpr std::uint32_t va_picture_h264_long_term = 0x10;

struct VAConfigAttrib {
    int type;
    std::uint32_t value;
};

struct VAImageFormat {
    std::uint32_t fourcc;
    std::uint32_t byte_order;
    std::uint32_t bits_per_pixel;
    std::uint32_t depth;
    std::uint32_t red_mask;
    std::uint32_t green_mask;
    std::uint32_t blue_mask;
    std::uint32_t alpha_mask;
    std::uint32_t va_reserved[4];
};

struct VAImage {
    VAImageID image_id;
    VAImageFormat format;
    VABufferID buf;
    std::uint16_t width;
    std::uint16_t height;
    std::uint32_t data_size;
    std::uint32_t num_planes;
    std::uint32_t pitches[3];
    std::uint32_t offsets[3];
    std::int32_t num_palette_entries;
    std::int32_t entry_bytes;
    std::int8_t component_order[4];
    std::uint32_t va_reserved[4];
};

struct VAPictureH264 {
    VASurfaceID picture_id;
    std::uint32_t frame_idx;
    std::uint32_t flags;
    std::int32_t top_field_order_cnt;
    std::int32_t bottom_field_order_cnt;
    std::uint32_t va_reserved[4];
};

struct VAPictureParameterBufferH264 {
    VAPictureH264 current;
    VAPictureH264 reference_frames[16];
    std::uint16_t picture_width_in_mbs_minus1;
    std::uint16_t picture_height_in_mbs_minus1;
    std::uint8_t bit_depth_luma_minus8;
    std::uint8_t bit_depth_chroma_minus8;
    std::uint8_t num_ref_frames;
    union {
        struct {
            std::uint32_t chroma_format_idc : 2;
            std::uint32_t residual_colour_transform_flag : 1;
            std::uint32_t gaps_in_frame_num_value_allowed_flag : 1;
            std::uint32_t frame_mbs_only_flag : 1;
            std::uint32_t mb_adaptive_frame_field_flag : 1;
            std::uint32_t direct_8x8_inference_flag : 1;
            std::uint32_t min_luma_bi_pred_size_8x8 : 1;
            std::uint32_t log2_max_frame_num_minus4 : 4;
            std::uint32_t pic_order_cnt_type : 2;
            std::uint32_t log2_max_pic_order_cnt_lsb_minus4 : 4;
            std::uint32_t delta_pic_order_always_zero_flag : 1;
        } bits;
        std::uint32_t value;
    } seq_fields;
    std::uint8_t num_slice_groups_minus1;
    std::uint8_t slice_group_map_type;
    std::uint16_t slice_group_change_rate_minus1;
    std::int8_t pic_init_qp_minus26;
    std::int8_t pic_init_qs_minus26;
    std::int8_t chroma_qp_index_offset;
    std::int8_t second_chroma_qp_index_offset;
    union {
        struct {
            std::uint32_t entropy_coding_mode_flag : 1;
            std::uint32_t weighted_pred_flag : 1;
            std::uint32_t weighted_bipred_idc : 2;
            std::uint32_t transform_8x8_mode_flag : 1;
            std::uint32_t field_pic_flag : 1;
            std::uint32_t constrained_intra_pred_flag : 1;
            std::uint32_t pic_order_present_flag : 1;
            std::uint32_t deblocking_filter_control_present_flag : 1;
            std::uint32_t redundant_pic_cnt_present_flag : 1;
            std::uint32_t reference_pic_flag : 1;
        } bits;
        std::uint32_t value;
    } pic_fields;
    std::uint16_t frame_num;
    std::uint32_t va_reserved[8];
};

struct VAIQMatrixBufferH264 {
    std::uint8_t scaling_list_4x4[6][16];
    std::uint8_t scaling_list_8x8[2][64];
    std::uint32_t va_reserved[4];
};

struct VASliceParameterBufferH264 {
    std::uint32_t slice_data_size;
    std::uint32_t slice_data_offset;
    std::uint32_t slice_data_flag;
    std::uint16_t slice_data_bit_offset;
    std::uint16_t first_mb_in_slice;
    std::uint8_t slice_type;
    std::uint8_t direct_spatial_mv_pred_flag;
    std::uint8_t num_ref_idx_l0_active_minus1;
    std::uint8_t num_ref_idx_l1_active_minus1;
    std::uint8_t cabac_init_idc;
    std::int8_t slice_qp_delta;
    std::uint8_t disable_deblocking_filter_idc;
    std::int8_t slice_alpha_c0_offset_div2;
    std::int8_t slice_beta_offset_div2;
    VAPictureH264 ref_pic_list0[32];
    VAPictureH264 ref_pic_list1[32];
    std::uint8_t luma_log2_weight_denom;
    std::uint8_t chroma_log2_weight_denom;
    std::uint8_t luma_weight_l0_flag;
    std::int16_t luma_weight_l0[32];
    std::int16_t luma_offset_l0[32];
    std::uint8_t chroma_weight_l0_flag;
    std::int16_t chroma_weight_l0[32][2];
    std::int16_t chroma_offset_l0[32][2];
    std::uint8_t luma_weight_l1_flag;
    std::int16_t luma_weight_l1[32];
    std::int16_t luma_offset_l1[32];
    std::uint8_t chroma_weight_l1_flag;
    std::int16_t chroma_weight_l1[32][2];
    std::int16_t chroma_offset_l1[32][2];
    std::uint32_t va_reserved[4];
};

// The layout VA-API's own header gives these (libva 2.24, measured by a
// program built against va.h), which a compiler that laid them out
// otherwise would silently break.
static_assert(sizeof(VAPictureH264) == 36, "VA-API's H.264 picture");
static_assert(sizeof(VAPictureParameterBufferH264) == 672 && offsetof(VAPictureParameterBufferH264, reference_frames) == 36
        && offsetof(VAPictureParameterBufferH264, picture_width_in_mbs_minus1) == 612 && offsetof(VAPictureParameterBufferH264, seq_fields) == 620
        && offsetof(VAPictureParameterBufferH264, num_slice_groups_minus1) == 624 && offsetof(VAPictureParameterBufferH264, pic_fields) == 632
        && offsetof(VAPictureParameterBufferH264, frame_num) == 636,
    "VA-API's H.264 picture parameters");
static_assert(sizeof(VAIQMatrixBufferH264) == 240 && offsetof(VAIQMatrixBufferH264, scaling_list_8x8) == 96, "VA-API's H.264 matrices");
static_assert(sizeof(VASliceParameterBufferH264) == 3128 && offsetof(VASliceParameterBufferH264, slice_data_bit_offset) == 12
        && offsetof(VASliceParameterBufferH264, slice_type) == 16 && offsetof(VASliceParameterBufferH264, slice_beta_offset_div2) == 24
        && offsetof(VASliceParameterBufferH264, ref_pic_list0) == 28 && offsetof(VASliceParameterBufferH264, ref_pic_list1) == 1180
        && offsetof(VASliceParameterBufferH264, luma_log2_weight_denom) == 2332 && offsetof(VASliceParameterBufferH264, luma_weight_l0) == 2336
        && offsetof(VASliceParameterBufferH264, chroma_weight_l0_flag) == 2464 && offsetof(VASliceParameterBufferH264, chroma_weight_l0) == 2466
        && offsetof(VASliceParameterBufferH264, luma_weight_l1_flag) == 2722 && offsetof(VASliceParameterBufferH264, chroma_offset_l1) == 2982,
    "VA-API's H.264 slice parameters");
static_assert(sizeof(VAImageFormat) == 48, "VA-API's image format");
static_assert(sizeof(VAImage) == 120, "VA-API's image");

// The entry points, as libva exports them.
struct Va {
    void* va = nullptr;
    void* drm = nullptr;
    VADisplay (*get_display_drm)(int fd) = nullptr;
    VAStatus (*initialize)(VADisplay, int*, int*) = nullptr;
    VAStatus (*terminate)(VADisplay) = nullptr;
    char const* (*query_vendor_string)(VADisplay) = nullptr;
    VAMessageCallback (*set_error_callback)(VADisplay, VAMessageCallback, void*) = nullptr;
    VAMessageCallback (*set_info_callback)(VADisplay, VAMessageCallback, void*) = nullptr;
    int (*max_num_profiles)(VADisplay) = nullptr;
    VAStatus (*query_config_profiles)(VADisplay, int*, int*) = nullptr;
    int (*max_num_entrypoints)(VADisplay) = nullptr;
    VAStatus (*query_config_entrypoints)(VADisplay, int, int*, int*) = nullptr;
    VAStatus (*create_config)(VADisplay, int, int, VAConfigAttrib*, int, VAConfigID*) = nullptr;
    VAStatus (*destroy_config)(VADisplay, VAConfigID) = nullptr;
    VAStatus (*create_surfaces)(VADisplay, unsigned, unsigned, unsigned, VASurfaceID*, unsigned, void*, unsigned) = nullptr;
    VAStatus (*destroy_surfaces)(VADisplay, VASurfaceID*, int) = nullptr;
    VAStatus (*create_context)(VADisplay, VAConfigID, int, int, int, VASurfaceID*, int, VAContextID*) = nullptr;
    VAStatus (*destroy_context)(VADisplay, VAContextID) = nullptr;
    VAStatus (*create_buffer)(VADisplay, VAContextID, int, unsigned, unsigned, void*, VABufferID*) = nullptr;
    VAStatus (*destroy_buffer)(VADisplay, VABufferID) = nullptr;
    VAStatus (*begin_picture)(VADisplay, VAContextID, VASurfaceID) = nullptr;
    VAStatus (*render_picture)(VADisplay, VAContextID, VABufferID*, int) = nullptr;
    VAStatus (*end_picture)(VADisplay, VAContextID) = nullptr;
    VAStatus (*sync_surface)(VADisplay, VASurfaceID) = nullptr;
    VAStatus (*derive_image)(VADisplay, VASurfaceID, VAImage*) = nullptr;
    VAStatus (*create_image)(VADisplay, VAImageFormat*, int, int, VAImage*) = nullptr;
    VAStatus (*get_image)(VADisplay, VASurfaceID, int, int, unsigned, unsigned, VAImageID) = nullptr;
    VAStatus (*destroy_image)(VADisplay, VAImageID) = nullptr;
    VAStatus (*map_buffer)(VADisplay, VABufferID, void**) = nullptr;
    VAStatus (*unmap_buffer)(VADisplay, VABufferID) = nullptr;
};

template<typename F>
bool load(void* library, char const* name, F& into)
{
    into = reinterpret_cast<F>(dlsym(library, name));
    return into != nullptr;
}

std::optional<Va> load_va(std::string& error)
{
    Va va;
    va.va = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
    va.drm = va.va ? dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (!va.va || !va.drm) {
        error = "VA-API is not installed (libva.so.2, libva-drm.so.2)";
        if (va.va)
            dlclose(va.va);
        return std::nullopt;
    }
    bool const all = load(va.drm, "vaGetDisplayDRM", va.get_display_drm) && load(va.va, "vaInitialize", va.initialize)
        && load(va.va, "vaTerminate", va.terminate) && load(va.va, "vaQueryVendorString", va.query_vendor_string)
        && load(va.va, "vaMaxNumProfiles", va.max_num_profiles) && load(va.va, "vaQueryConfigProfiles", va.query_config_profiles)
        && load(va.va, "vaMaxNumEntrypoints", va.max_num_entrypoints) && load(va.va, "vaQueryConfigEntrypoints", va.query_config_entrypoints)
        && load(va.va, "vaCreateConfig", va.create_config) && load(va.va, "vaDestroyConfig", va.destroy_config)
        && load(va.va, "vaCreateSurfaces", va.create_surfaces) && load(va.va, "vaDestroySurfaces", va.destroy_surfaces)
        && load(va.va, "vaCreateContext", va.create_context) && load(va.va, "vaDestroyContext", va.destroy_context)
        && load(va.va, "vaCreateBuffer", va.create_buffer) && load(va.va, "vaDestroyBuffer", va.destroy_buffer)
        && load(va.va, "vaBeginPicture", va.begin_picture) && load(va.va, "vaRenderPicture", va.render_picture)
        && load(va.va, "vaEndPicture", va.end_picture) && load(va.va, "vaSyncSurface", va.sync_surface)
        && load(va.va, "vaDeriveImage", va.derive_image) && load(va.va, "vaCreateImage", va.create_image)
        && load(va.va, "vaGetImage", va.get_image) && load(va.va, "vaDestroyImage", va.destroy_image)
        && load(va.va, "vaMapBuffer", va.map_buffer) && load(va.va, "vaUnmapBuffer", va.unmap_buffer);
    if (!all) {
        error = "VA-API is too old: an entry point is missing";
        dlclose(va.drm);
        dlclose(va.va);
        return std::nullopt;
    }
    load(va.va, "vaSetErrorCallback", va.set_error_callback);
    load(va.va, "vaSetInfoCallback", va.set_info_callback);
    return va;
}

void quiet(void*, char const*) { }

// Sixteen references and the picture being decoded.
constexpr int slot_count = 17;

// A reference as the lists see it: where it is, and its numbers for
// this picture (8.2.4.1).
struct ListEntry {
    VASurfaceID surface = va_invalid_id;
    bool long_term = false;
    int frame_num = 0; // FrameNum, or LongTermFrameIdx
    int pic_num = 0; // PicNum (FrameNumWrap) for a short-term frame, LongTermPicNum for a long-term one
    int poc = 0; // the frame's PicOrderCnt
    int poc_top = 0;
    int poc_bottom = 0;
};

VAPictureH264 va_picture_of(ListEntry const& e)
{
    VAPictureH264 out {};
    out.picture_id = e.surface;
    out.frame_idx = static_cast<std::uint32_t>(e.frame_num);
    out.flags = e.long_term ? va_picture_h264_long_term : va_picture_h264_short_term;
    out.top_field_order_cnt = e.poc_top;
    out.bottom_field_order_cnt = e.poc_bottom;
    return out;
}

VAPictureH264 va_no_picture()
{
    VAPictureH264 out {};
    out.picture_id = va_invalid_id;
    out.flags = va_picture_h264_invalid;
    return out;
}

class VaapiH264 final : public H264Accelerator {
public:
    VaapiH264(Va va, int fd, VADisplay display, std::string device)
        : m_va(va)
        , m_fd(fd)
        , m_display(display)
        , m_device(std::move(device))
    {
    }

    ~VaapiH264() override
    {
        release_context();
        m_va.terminate(m_display);
        close(m_fd);
        dlclose(m_va.drm);
        dlclose(m_va.va);
    }

    std::string const& device() const override { return m_device; }
    int slots() const override { return slot_count; }
    void reset() override { m_sizes.assign(m_sizes.size(), { 0, 0 }); }

    bool set_parameter_sets(std::vector<H264Sps> const&, std::vector<H264Pps> const&) override
    {
        return true; // each picture brings its own sets
    }

    bool decode(H264PictureInfo const& picture) override
    {
        if (!picture.sps || !picture.pps || !picture.first_slice || picture.slices.empty() || picture.headers.size() != picture.slices.size())
            return false;
        H264Sps const& sps = *picture.sps;
        H264Pps const& pps = *picture.pps;
        if (picture.slot < 0 || picture.slot >= slot_count)
            return false;
        int const width = sps.coded_width();
        int const height = sps.coded_height();
        // Surfaces as large as the sequence needs; a larger IDR starts them afresh.
        if (m_context == va_invalid_id || width > m_surface_width || height > m_surface_height) {
            if (!picture.idr && m_context != va_invalid_id)
                return false;
            if (!create_context(sps, width, height))
                return false;
        }
        auto surface_of = [&](int slot) { return m_surfaces[static_cast<std::size_t>(slot)]; };

        // 8.2.4.1: the references' numbers for this picture.
        int const max_frame_num = 1 << sps.log2_max_frame_num;
        std::vector<ListEntry> references;
        for (H264Reference const& r : picture.references) {
            if (r.slot < 0 || r.slot >= slot_count || r.slot == picture.slot)
                return false;
            ListEntry e;
            e.surface = surface_of(r.slot);
            e.long_term = r.long_term;
            e.frame_num = r.frame_num;
            e.pic_num = r.long_term ? r.frame_num : (r.frame_num > picture.frame_num ? r.frame_num - max_frame_num : r.frame_num);
            e.poc_top = r.poc_top;
            e.poc_bottom = r.poc_bottom;
            e.poc = std::min(r.poc_top, r.poc_bottom);
            references.push_back(e);
        }
        int const current_poc = std::min(picture.poc_top, picture.poc_bottom);

        VAPictureParameterBufferH264 pp {};
        pp.current.picture_id = surface_of(picture.slot);
        pp.current.frame_idx = static_cast<std::uint32_t>(picture.long_term ? picture.long_term_frame_idx : picture.frame_num);
        pp.current.flags = picture.is_reference ? (picture.long_term ? va_picture_h264_long_term : va_picture_h264_short_term) : 0;
        pp.current.top_field_order_cnt = picture.poc_top;
        pp.current.bottom_field_order_cnt = picture.poc_bottom;
        for (std::size_t i = 0; i < 16; ++i)
            pp.reference_frames[i] = i < references.size() ? va_picture_of(references[i]) : va_no_picture();
        pp.picture_width_in_mbs_minus1 = static_cast<std::uint16_t>(sps.pic_width_in_mbs - 1);
        pp.picture_height_in_mbs_minus1 = static_cast<std::uint16_t>(sps.frame_height_in_mbs() - 1);
        pp.bit_depth_luma_minus8 = static_cast<std::uint8_t>(sps.bit_depth_luma - 8);
        pp.bit_depth_chroma_minus8 = static_cast<std::uint8_t>(sps.bit_depth_chroma - 8);
        pp.num_ref_frames = static_cast<std::uint8_t>(sps.max_num_ref_frames);
        pp.seq_fields.bits.chroma_format_idc = static_cast<std::uint32_t>(sps.chroma_format_idc);
        pp.seq_fields.bits.residual_colour_transform_flag = sps.separate_colour_plane;
        pp.seq_fields.bits.gaps_in_frame_num_value_allowed_flag = sps.gaps_in_frame_num_allowed;
        pp.seq_fields.bits.frame_mbs_only_flag = sps.frame_mbs_only;
        pp.seq_fields.bits.mb_adaptive_frame_field_flag = sps.mb_adaptive_frame_field;
        pp.seq_fields.bits.direct_8x8_inference_flag = sps.direct_8x8_inference;
        pp.seq_fields.bits.min_luma_bi_pred_size_8x8 = sps.level_idc >= 31; // A.3.3.2: levels 3.1 and up
        pp.seq_fields.bits.log2_max_frame_num_minus4 = static_cast<std::uint32_t>(sps.log2_max_frame_num - 4);
        pp.seq_fields.bits.pic_order_cnt_type = static_cast<std::uint32_t>(sps.pic_order_cnt_type);
        pp.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = static_cast<std::uint32_t>(std::max(0, sps.log2_max_pic_order_cnt_lsb - 4));
        pp.seq_fields.bits.delta_pic_order_always_zero_flag = sps.delta_pic_order_always_zero;
        pp.num_slice_groups_minus1 = static_cast<std::uint8_t>(std::max(0, pps.num_slice_groups - 1));
        pp.slice_group_map_type = static_cast<std::uint8_t>(pps.slice_group_map_type);
        pp.pic_init_qp_minus26 = static_cast<std::int8_t>(pps.pic_init_qp - 26);
        pp.pic_init_qs_minus26 = static_cast<std::int8_t>(pps.pic_init_qs - 26);
        pp.chroma_qp_index_offset = static_cast<std::int8_t>(pps.chroma_qp_index_offset);
        pp.second_chroma_qp_index_offset = static_cast<std::int8_t>(pps.second_chroma_qp_index_offset);
        pp.pic_fields.bits.entropy_coding_mode_flag = pps.entropy_coding_mode;
        pp.pic_fields.bits.weighted_pred_flag = pps.weighted_pred;
        pp.pic_fields.bits.weighted_bipred_idc = static_cast<std::uint32_t>(pps.weighted_bipred_idc);
        pp.pic_fields.bits.transform_8x8_mode_flag = pps.transform_8x8_mode;
        pp.pic_fields.bits.field_pic_flag = 0;
        pp.pic_fields.bits.constrained_intra_pred_flag = pps.constrained_intra_pred;
        pp.pic_fields.bits.pic_order_present_flag = pps.bottom_field_pic_order_in_frame_present;
        pp.pic_fields.bits.deblocking_filter_control_present_flag = pps.deblocking_filter_control_present;
        pp.pic_fields.bits.redundant_pic_cnt_present_flag = pps.redundant_pic_cnt_present;
        pp.pic_fields.bits.reference_pic_flag = picture.is_reference;
        pp.frame_num = static_cast<std::uint16_t>(picture.frame_num);

        // The scaling lists in force (the PPS's hold the SPS's where it has none).
        VAIQMatrixBufferH264 iq {};
        for (std::size_t i = 0; i < 6; ++i)
            std::copy(pps.scaling.list_4x4[i].begin(), pps.scaling.list_4x4[i].end(), iq.scaling_list_4x4[i]);
        for (std::size_t i = 0; i < 2; ++i)
            std::copy(pps.scaling.list_8x8[i].begin(), pps.scaling.list_8x8[i].end(), iq.scaling_list_8x8[i]);

        // The slices, each with its parameters: the whole unit (its header
        // byte first) is the data, the header's end the bit offset.
        std::vector<VASliceParameterBufferH264> slice_params(picture.slices.size());
        std::size_t data_size = 0;
        for (auto const& slice : picture.slices)
            data_size += slice.size();
        std::vector<std::uint8_t> data;
        data.reserve(data_size);
        for (std::size_t i = 0; i < picture.slices.size(); ++i) {
            H264SliceHeader const& h = picture.headers[i];
            VASliceParameterBufferH264& sp = slice_params[i];
            sp.slice_data_size = static_cast<std::uint32_t>(picture.slices[i].size());
            sp.slice_data_offset = static_cast<std::uint32_t>(data.size());
            sp.slice_data_flag = va_slice_data_flag_all;
            sp.slice_data_bit_offset = static_cast<std::uint16_t>(h.header_bits);
            sp.first_mb_in_slice = static_cast<std::uint16_t>(h.first_mb_in_slice);
            sp.slice_type = static_cast<std::uint8_t>(h.slice_type);
            sp.direct_spatial_mv_pred_flag = h.direct_spatial_mv_pred;
            bool const is_b = h.slice_type == H264SliceType::B;
            bool const is_p = h.slice_type == H264SliceType::P || h.slice_type == H264SliceType::SP;
            sp.num_ref_idx_l0_active_minus1 = static_cast<std::uint8_t>(is_p || is_b ? std::max(0, h.num_ref_idx_l0_active - 1) : 0);
            sp.num_ref_idx_l1_active_minus1 = static_cast<std::uint8_t>(is_b ? std::max(0, h.num_ref_idx_l1_active - 1) : 0);
            sp.cabac_init_idc = static_cast<std::uint8_t>(h.cabac_init_idc);
            sp.slice_qp_delta = static_cast<std::int8_t>(h.slice_qp_delta);
            sp.disable_deblocking_filter_idc = static_cast<std::uint8_t>(h.disable_deblocking_filter_idc);
            sp.slice_alpha_c0_offset_div2 = static_cast<std::int8_t>(h.slice_alpha_c0_offset_div2);
            sp.slice_beta_offset_div2 = static_cast<std::int8_t>(h.slice_beta_offset_div2);
            for (std::size_t j = 0; j < 32; ++j) {
                sp.ref_pic_list0[j] = va_no_picture();
                sp.ref_pic_list1[j] = va_no_picture();
            }
            if (is_p || is_b) {
                std::vector<ListEntry> const l0 = reference_list(references, h, 0, is_b, current_poc, max_frame_num, picture.frame_num);
                for (std::size_t j = 0; j < l0.size() && j < 32; ++j)
                    sp.ref_pic_list0[j] = va_picture_of(l0[j]);
                if (is_b) {
                    std::vector<ListEntry> const l1 = reference_list(references, h, 1, true, current_poc, max_frame_num, picture.frame_num);
                    for (std::size_t j = 0; j < l1.size() && j < 32; ++j)
                        sp.ref_pic_list1[j] = va_picture_of(l1[j]);
                }
            }
            if (h.weights.present) {
                sp.luma_log2_weight_denom = static_cast<std::uint8_t>(h.weights.luma_log2_denom);
                sp.chroma_log2_weight_denom = static_cast<std::uint8_t>(h.weights.chroma_log2_denom);
                auto const fill = [&](std::size_t list, std::uint8_t& luma_flag, std::int16_t (&lw)[32], std::int16_t (&lo)[32], std::uint8_t& chroma_flag,
                                      std::int16_t (&cw)[32][2], std::int16_t (&co)[32][2]) {
                    int const count = list == 0 ? h.num_ref_idx_l0_active : h.num_ref_idx_l1_active;
                    for (int j = 0; j < count && j < 32; ++j) {
                        H264PredWeights::Entry const& e = h.weights.list[list][static_cast<std::size_t>(j)];
                        luma_flag |= e.luma_present;
                        chroma_flag |= e.chroma_present;
                        lw[j] = static_cast<std::int16_t>(e.luma_weight);
                        lo[j] = static_cast<std::int16_t>(e.luma_offset);
                        for (std::size_t c = 0; c < 2; ++c) {
                            cw[j][c] = static_cast<std::int16_t>(e.chroma_weight[c]);
                            co[j][c] = static_cast<std::int16_t>(e.chroma_offset[c]);
                        }
                    }
                };
                fill(0, sp.luma_weight_l0_flag, sp.luma_weight_l0, sp.luma_offset_l0, sp.chroma_weight_l0_flag, sp.chroma_weight_l0, sp.chroma_offset_l0);
                if (is_b)
                    fill(1, sp.luma_weight_l1_flag, sp.luma_weight_l1, sp.luma_offset_l1, sp.chroma_weight_l1_flag, sp.chroma_weight_l1, sp.chroma_offset_l1);
            }
            data.insert(data.end(), picture.slices[i].begin(), picture.slices[i].end());
        }

        std::vector<VABufferID> buffers;
        auto const make = [&](int type, unsigned size, unsigned count, void* bytes) {
            VABufferID id = va_invalid_id;
            if (m_va.create_buffer(m_display, m_context, type, size, count, bytes, &id) != va_success)
                return false;
            buffers.push_back(id);
            return true;
        };
        bool ok = make(va_picture_parameter_buffer, sizeof pp, 1, &pp) && make(va_iq_matrix_buffer, sizeof iq, 1, &iq)
            && make(va_slice_parameter_buffer, sizeof(VASliceParameterBufferH264), static_cast<unsigned>(slice_params.size()), slice_params.data())
            && make(va_slice_data_buffer, static_cast<unsigned>(data.size()), 1, data.data());
        VASurfaceID const target = surface_of(picture.slot);
        if (ok)
            ok = m_va.begin_picture(m_display, m_context, target) == va_success;
        if (ok) {
            ok = m_va.render_picture(m_display, m_context, buffers.data(), static_cast<int>(buffers.size())) == va_success;
            ok = m_va.end_picture(m_display, m_context) == va_success && ok;
        }
        for (VABufferID const buffer : buffers)
            m_va.destroy_buffer(m_display, buffer);
        if (!ok)
            return false;
        Size& size = m_sizes[static_cast<std::size_t>(picture.slot)];
        size.width = sps.width();
        size.height = sps.height();
        size.crop_x = sps.frame_cropping ? sps.crop_left * 2 : 0;
        size.crop_y = sps.frame_cropping ? sps.crop_top * 2 : 0;
        return true;
    }

    std::optional<Nv12Picture> read(int slot) override
    {
        if (slot < 0 || slot >= slot_count || m_surfaces.empty())
            return std::nullopt;
        VASurfaceID const surface = m_surfaces[static_cast<std::size_t>(slot)];
        Size const size = m_sizes[static_cast<std::size_t>(slot)];
        if (size.width <= 0 || size.height <= 0 || m_va.sync_surface(m_display, surface) != va_success)
            return std::nullopt;
        VAImage image {};
        bool derived = m_va.derive_image(m_display, surface, &image) == va_success;
        if (derived && image.format.fourcc != va_fourcc_nv12) {
            m_va.destroy_image(m_display, image.image_id);
            derived = false;
        }
        if (!derived) {
            VAImageFormat format {};
            format.fourcc = va_fourcc_nv12;
            format.byte_order = 1;
            format.bits_per_pixel = 12;
            if (m_va.create_image(m_display, &format, m_surface_width, m_surface_height, &image) != va_success)
                return std::nullopt;
            if (m_va.get_image(m_display, surface, 0, 0, static_cast<unsigned>(m_surface_width), static_cast<unsigned>(m_surface_height), image.image_id)
                != va_success) {
                m_va.destroy_image(m_display, image.image_id);
                return std::nullopt;
            }
        }
        void* mapped = nullptr;
        if (m_va.map_buffer(m_display, image.buf, &mapped) != va_success || mapped == nullptr) {
            m_va.destroy_image(m_display, image.image_id);
            return std::nullopt;
        }
        Nv12Picture picture;
        picture.width = size.width;
        picture.height = size.height;
        auto const* base = static_cast<std::uint8_t const*>(mapped);
        auto const w = static_cast<std::size_t>(size.width);
        picture.luma.resize(w * static_cast<std::size_t>(size.height));
        for (int y = 0; y < size.height; ++y)
            std::memcpy(picture.luma.data() + static_cast<std::size_t>(y) * w,
                base + image.offsets[0] + static_cast<std::size_t>(y + size.crop_y) * image.pitches[0] + static_cast<std::size_t>(size.crop_x), w);
        std::size_t const chroma_row = static_cast<std::size_t>(picture.chroma_width()) * 2;
        picture.chroma.resize(chroma_row * static_cast<std::size_t>(picture.chroma_height()));
        for (int y = 0; y < picture.chroma_height(); ++y)
            std::memcpy(picture.chroma.data() + static_cast<std::size_t>(y) * chroma_row,
                base + image.offsets[1] + static_cast<std::size_t>(y + size.crop_y / 2) * image.pitches[1] + static_cast<std::size_t>(size.crop_x / 2 * 2), chroma_row);
        m_va.unmap_buffer(m_display, image.buf);
        m_va.destroy_image(m_display, image.image_id);
        return picture;
    }

private:
    struct Size {
        int width = 0;
        int height = 0;
        int crop_x = 0;
        int crop_y = 0;
    };

    // 8.2.4.2 and 8.2.4.3: a slice's reference list — the initial order
    // (P: short-term by descending PicNum then long-term by ascending
    // LongTermPicNum; B: by order count around the current picture), then
    // the modifications the header carries, cut to the active count.
    static std::vector<ListEntry> reference_list(std::vector<ListEntry> const& references, H264SliceHeader const& h, std::size_t list, bool is_b,
        int current_poc, int max_frame_num, int current_frame_num)
    {
        std::vector<ListEntry> short_term;
        std::vector<ListEntry> long_term;
        for (ListEntry const& e : references)
            (e.long_term ? long_term : short_term).push_back(e);
        std::vector<ListEntry> out;
        if (!is_b) {
            std::sort(short_term.begin(), short_term.end(), [](ListEntry const& a, ListEntry const& b) { return a.pic_num > b.pic_num; });
            std::sort(long_term.begin(), long_term.end(), [](ListEntry const& a, ListEntry const& b) { return a.pic_num < b.pic_num; });
            out = short_term;
            out.insert(out.end(), long_term.begin(), long_term.end());
        } else {
            std::vector<ListEntry> before;
            std::vector<ListEntry> after;
            for (ListEntry const& e : short_term)
                (e.poc <= current_poc ? before : after).push_back(e);
            std::sort(before.begin(), before.end(), [](ListEntry const& a, ListEntry const& b) { return a.poc > b.poc; });
            std::sort(after.begin(), after.end(), [](ListEntry const& a, ListEntry const& b) { return a.poc < b.poc; });
            std::sort(long_term.begin(), long_term.end(), [](ListEntry const& a, ListEntry const& b) { return a.pic_num < b.pic_num; });
            if (list == 0) {
                out = before;
                out.insert(out.end(), after.begin(), after.end());
            } else {
                out = after;
                out.insert(out.end(), before.begin(), before.end());
            }
            out.insert(out.end(), long_term.begin(), long_term.end());
            if (list == 1 && out.size() > 1) {
                // 8.2.4.2.3: a list 1 that equals list 0 swaps its first two.
                std::vector<ListEntry> l0 = before;
                l0.insert(l0.end(), after.begin(), after.end());
                l0.insert(l0.end(), long_term.begin(), long_term.end());
                bool same = l0.size() == out.size();
                for (std::size_t i = 0; same && i < out.size(); ++i)
                    same = l0[i].surface == out[i].surface;
                if (same)
                    std::swap(out[0], out[1]);
            }
        }
        int const active = list == 0 ? h.num_ref_idx_l0_active : h.num_ref_idx_l1_active;
        // 8.2.4.3: each modification puts a named picture at the next index,
        // the rest sliding along (a picture named twice is kept once).
        int const max_pic_num = max_frame_num; // frames
        int pic_num_pred = current_frame_num;
        std::size_t ref_idx = 0;
        if (out.size() < static_cast<std::size_t>(active))
            out.resize(static_cast<std::size_t>(active)); // room for the moves; empty entries drop out below
        for (H264RefListModification const& m : h.ref_list_modifications[list]) {
            if (m.idc > 2)
                break;
            std::optional<ListEntry> wanted;
            if (m.idc == 0 || m.idc == 1) {
                int pic_num_no_wrap = m.idc == 0 ? pic_num_pred - m.value : pic_num_pred + m.value;
                if (pic_num_no_wrap < 0)
                    pic_num_no_wrap += max_pic_num;
                else if (pic_num_no_wrap >= max_pic_num)
                    pic_num_no_wrap -= max_pic_num;
                pic_num_pred = pic_num_no_wrap;
                int const pic_num = pic_num_no_wrap > current_frame_num ? pic_num_no_wrap - max_pic_num : pic_num_no_wrap;
                for (ListEntry const& e : references) {
                    if (!e.long_term && e.pic_num == pic_num)
                        wanted = e;
                }
            } else {
                for (ListEntry const& e : references) {
                    if (e.long_term && e.pic_num == m.value)
                        wanted = e;
                }
            }
            if (!wanted || ref_idx >= out.size())
                continue;
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(ref_idx), *wanted);
            ++ref_idx;
            // The same picture further along goes.
            for (std::size_t i = ref_idx; i < out.size(); ++i) {
                if (out[i].surface == wanted->surface) {
                    out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }
        std::erase_if(out, [](ListEntry const& e) { return e.surface == va_invalid_id; });
        if (out.size() > static_cast<std::size_t>(active))
            out.resize(static_cast<std::size_t>(active));
        return out;
    }

    void release_context()
    {
        if (m_context != va_invalid_id)
            m_va.destroy_context(m_display, m_context);
        if (!m_surfaces.empty())
            m_va.destroy_surfaces(m_display, m_surfaces.data(), static_cast<int>(m_surfaces.size()));
        if (m_config != va_invalid_id)
            m_va.destroy_config(m_display, m_config);
        m_context = m_config = va_invalid_id;
        m_surfaces.clear();
        m_sizes.clear();
    }

    bool create_context(H264Sps const& sps, int width, int height)
    {
        release_context();
        // The profile the stream names, where the device has it; High reads them all.
        int profile = va_profile_h264_high;
        if (sps.profile_idc == 77 && has_profile(va_profile_h264_main))
            profile = va_profile_h264_main;
        else if ((sps.profile_idc == 66 || (sps.constraint_set_flags & 0x40)) && has_profile(va_profile_h264_constrained_baseline))
            profile = va_profile_h264_constrained_baseline;
        VAConfigAttrib attribute { va_config_attrib_rt_format, va_rt_format_yuv420 };
        if (m_va.create_config(m_display, profile, va_entrypoint_vld, &attribute, 1, &m_config) != va_success) {
            m_config = va_invalid_id;
            if (profile == va_profile_h264_high || m_va.create_config(m_display, va_profile_h264_high, va_entrypoint_vld, &attribute, 1, &m_config) != va_success) {
                m_config = va_invalid_id;
                return false;
            }
        }
        m_surfaces.assign(slot_count, va_invalid_id);
        if (m_va.create_surfaces(m_display, va_rt_format_yuv420, static_cast<unsigned>(width), static_cast<unsigned>(height), m_surfaces.data(), slot_count,
                nullptr, 0)
            != va_success) {
            m_surfaces.clear();
            release_context();
            return false;
        }
        if (m_va.create_context(m_display, m_config, width, height, va_progressive, m_surfaces.data(), slot_count, &m_context) != va_success) {
            m_context = va_invalid_id;
            release_context();
            return false;
        }
        m_surface_width = width;
        m_surface_height = height;
        m_sizes.assign(slot_count, Size {});
        return true;
    }

    bool has_profile(int profile) const { return std::find(m_profiles.begin(), m_profiles.end(), profile) != m_profiles.end(); }

public:
    std::vector<int> m_profiles; // the device's, with a decoding entry point

private:
    Va m_va;
    int m_fd;
    VADisplay m_display;
    std::string m_device;
    VAConfigID m_config = va_invalid_id;
    VAContextID m_context = va_invalid_id;
    std::vector<VASurfaceID> m_surfaces;
    std::vector<Size> m_sizes;
    int m_surface_width = 0;
    int m_surface_height = 0;
};

}

std::unique_ptr<H264Accelerator> open_vaapi_h264(std::string& error)
{
    std::optional<Va> va = load_va(error);
    if (!va)
        return nullptr;
    std::string tried;
    for (int node = 128; node < 192; ++node) {
        std::string const path = "/dev/dri/renderD" + std::to_string(node);
        int const fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        VADisplay const display = va->get_display_drm(fd);
        if (display == nullptr) {
            close(fd);
            continue;
        }
        if (va->set_error_callback)
            va->set_error_callback(display, quiet, nullptr);
        if (va->set_info_callback)
            va->set_info_callback(display, quiet, nullptr);
        int major = 0;
        int minor = 0;
        if (va->initialize(display, &major, &minor) != va_success) {
            va->terminate(display);
            close(fd);
            tried += (tried.empty() ? "" : ", ") + path + " (no VA-API driver)";
            continue;
        }
        // The H.264 profiles with a decoding entry point; High is what a
        // stream of any of them is decoded under when it is the only one.
        std::vector<int> profiles(static_cast<std::size_t>(std::max(0, va->max_num_profiles(display))));
        int profile_count = 0;
        std::vector<int> decodes;
        if (!profiles.empty() && va->query_config_profiles(display, profiles.data(), &profile_count) == va_success) {
            for (int const profile : { va_profile_h264_high, va_profile_h264_main, va_profile_h264_constrained_baseline }) {
                if (std::find(profiles.begin(), profiles.begin() + profile_count, profile) == profiles.begin() + profile_count)
                    continue;
                std::vector<int> entrypoints(static_cast<std::size_t>(std::max(0, va->max_num_entrypoints(display))));
                int entrypoint_count = 0;
                if (!entrypoints.empty() && va->query_config_entrypoints(display, profile, entrypoints.data(), &entrypoint_count) == va_success
                    && std::find(entrypoints.begin(), entrypoints.begin() + entrypoint_count, va_entrypoint_vld) != entrypoints.begin() + entrypoint_count)
                    decodes.push_back(profile);
            }
        }
        if (std::find(decodes.begin(), decodes.end(), va_profile_h264_high) == decodes.end()) {
            va->terminate(display);
            close(fd);
            tried += (tried.empty() ? "" : ", ") + path + " (no H.264 High)";
            continue;
        }
        char const* const vendor = va->query_vendor_string(display);
        std::string const description = path + " (" + (vendor ? vendor : "VA-API") + ")";
        auto accelerator = std::make_unique<VaapiH264>(*va, fd, display, description);
        accelerator->m_profiles = std::move(decodes);
        return accelerator;
    }
    error = tried.empty() ? "no GPU render node to decode on" : "no device decodes H.264: " + tried;
    dlclose(va->drm);
    dlclose(va->va);
    return nullptr;
}

}
