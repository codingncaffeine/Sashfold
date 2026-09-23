#include "media/linux/Vp9Accelerators.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

// VP9 through VA-API. libva and its DRM front are loaded at run time: the
// binary imports nothing new, and a machine without them — or without a
// device whose driver decodes VP9 — has no accelerator and says why. The
// types below are VA-API's own (va.h, va_dec_vp9.h), whose layout is its
// stable ABI; declaring them here keeps the build free of their headers.
// Every render node is tried in turn: a machine with two GPUs may have a
// VA-API driver for only one of them.

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
constexpr int va_profile_vp9_profile0 = 19;
constexpr int va_entrypoint_vld = 1;
constexpr int va_config_attrib_rt_format = 0;
constexpr unsigned va_rt_format_yuv420 = 0x00000001;
constexpr int va_progressive = 0x1;
constexpr int va_picture_parameter_buffer = 0;
constexpr int va_slice_parameter_buffer = 4;
constexpr int va_slice_data_buffer = 5;
constexpr std::uint32_t va_slice_data_flag_all = 0x00;
constexpr std::uint32_t va_fourcc_nv12 = 0x3231564E;

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

struct VADecPictureParameterBufferVP9 {
    std::uint16_t frame_width;
    std::uint16_t frame_height;
    VASurfaceID reference_frames[8];
    union {
        struct {
            std::uint32_t subsampling_x : 1;
            std::uint32_t subsampling_y : 1;
            std::uint32_t frame_type : 1;
            std::uint32_t show_frame : 1;
            std::uint32_t error_resilient_mode : 1;
            std::uint32_t intra_only : 1;
            std::uint32_t allow_high_precision_mv : 1;
            std::uint32_t mcomp_filter_type : 3;
            std::uint32_t frame_parallel_decoding_mode : 1;
            std::uint32_t reset_frame_context : 2;
            std::uint32_t refresh_frame_context : 1;
            std::uint32_t frame_context_idx : 2;
            std::uint32_t segmentation_enabled : 1;
            std::uint32_t segmentation_temporal_update : 1;
            std::uint32_t segmentation_update_map : 1;
            std::uint32_t last_ref_frame : 3;
            std::uint32_t last_ref_frame_sign_bias : 1;
            std::uint32_t golden_ref_frame : 3;
            std::uint32_t golden_ref_frame_sign_bias : 1;
            std::uint32_t alt_ref_frame : 3;
            std::uint32_t alt_ref_frame_sign_bias : 1;
            std::uint32_t lossless_flag : 1;
        } bits;
        std::uint32_t value;
    } pic_fields;
    std::uint8_t filter_level;
    std::uint8_t sharpness_level;
    std::uint8_t log2_tile_rows;
    std::uint8_t log2_tile_columns;
    std::uint8_t frame_header_length_in_bytes;
    std::uint16_t first_partition_size;
    std::uint8_t mb_segment_tree_probs[7];
    std::uint8_t segment_pred_probs[3];
    std::uint8_t profile;
    std::uint8_t bit_depth;
    std::uint32_t va_reserved[8];
};

struct VASegmentParameterVP9 {
    union {
        struct {
            std::uint16_t segment_reference_enabled : 1;
            std::uint16_t segment_reference : 2;
            std::uint16_t segment_reference_skipped : 1;
        } fields;
        std::uint16_t value;
    } segment_flags;
    std::uint8_t filter_level[4][2];
    std::int16_t luma_ac_quant_scale;
    std::int16_t luma_dc_quant_scale;
    std::int16_t chroma_ac_quant_scale;
    std::int16_t chroma_dc_quant_scale;
    std::uint32_t va_reserved[4];
};

struct VASliceParameterBufferVP9 {
    std::uint32_t slice_data_size;
    std::uint32_t slice_data_offset;
    std::uint32_t slice_data_flag;
    VASegmentParameterVP9 seg_param[8];
    std::uint32_t va_reserved[4];
};

// The layout VA-API's own headers give these (measured against va.h and
// va_dec_vp9.h), which a compiler that laid them out otherwise would
// silently break.
static_assert(sizeof(VADecPictureParameterBufferVP9) == 92, "VA-API's VP9 picture parameters");
static_assert(offsetof(VADecPictureParameterBufferVP9, first_partition_size) == 46, "VA-API's VP9 picture parameters");
static_assert(offsetof(VADecPictureParameterBufferVP9, mb_segment_tree_probs) == 48, "VA-API's VP9 picture parameters");
static_assert(sizeof(VASegmentParameterVP9) == 36, "VA-API's VP9 segment parameters");
static_assert(sizeof(VASliceParameterBufferVP9) == 316, "VA-API's VP9 slice parameters");
static_assert(sizeof(VAImageFormat) == 48, "VA-API's image format");
static_assert(sizeof(VAImage) == 120, "VA-API's image");

// The entry points, as libva exports them.
struct Va {
    void* va = nullptr;
    void* drm = nullptr;
    VADisplay (*get_display_drm)(int fd) = nullptr;
    VAStatus (*initialize)(VADisplay, int*, int*) = nullptr;
    VAStatus (*terminate)(VADisplay) = nullptr;
    char const* (*error_str)(VAStatus) = nullptr;
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
        && load(va.va, "vaTerminate", va.terminate) && load(va.va, "vaErrorStr", va.error_str)
        && load(va.va, "vaQueryVendorString", va.query_vendor_string) && load(va.va, "vaMaxNumProfiles", va.max_num_profiles)
        && load(va.va, "vaQueryConfigProfiles", va.query_config_profiles) && load(va.va, "vaMaxNumEntrypoints", va.max_num_entrypoints)
        && load(va.va, "vaQueryConfigEntrypoints", va.query_config_entrypoints) && load(va.va, "vaCreateConfig", va.create_config)
        && load(va.va, "vaDestroyConfig", va.destroy_config) && load(va.va, "vaCreateSurfaces", va.create_surfaces)
        && load(va.va, "vaDestroySurfaces", va.destroy_surfaces) && load(va.va, "vaCreateContext", va.create_context)
        && load(va.va, "vaDestroyContext", va.destroy_context) && load(va.va, "vaCreateBuffer", va.create_buffer)
        && load(va.va, "vaDestroyBuffer", va.destroy_buffer) && load(va.va, "vaBeginPicture", va.begin_picture)
        && load(va.va, "vaRenderPicture", va.render_picture) && load(va.va, "vaEndPicture", va.end_picture)
        && load(va.va, "vaSyncSurface", va.sync_surface) && load(va.va, "vaDeriveImage", va.derive_image)
        && load(va.va, "vaCreateImage", va.create_image) && load(va.va, "vaGetImage", va.get_image)
        && load(va.va, "vaDestroyImage", va.destroy_image) && load(va.va, "vaMapBuffer", va.map_buffer)
        && load(va.va, "vaUnmapBuffer", va.unmap_buffer);
    if (!all) {
        error = "VA-API is too old: an entry point is missing";
        dlclose(va.drm);
        dlclose(va.va);
        return std::nullopt;
    }
    // Present since libva 2.0; without them its chatter goes to stderr.
    load(va.va, "vaSetErrorCallback", va.set_error_callback);
    load(va.va, "vaSetInfoCallback", va.set_info_callback);
    return va;
}

void quiet(void*, char const*) { }

class VaapiVp9 final : public Vp9Accelerator {
public:
    VaapiVp9(Va va, int fd, VADisplay display, std::string device)
        : m_va(va)
        , m_fd(fd)
        , m_display(display)
        , m_device(std::move(device))
    {
        m_slots.fill(-1);
    }

    ~VaapiVp9() override
    {
        release_context();
        m_va.terminate(m_display);
        close(m_fd);
        dlclose(m_va.drm);
        dlclose(m_va.va);
    }

    std::string const& device() const override { return m_device; }

    void reset() override
    {
        m_slots.fill(-1);
        m_last = -1;
    }

    bool decode(std::span<std::uint8_t const> frame, Vp9FrameHeader const& header) override
    {
        if (header.show_existing_frame || header.profile != 0 || header.bit_depth != 8)
            return false;
        // Surfaces as large as the stream needs; a key frame of a new size
        // starts them afresh, which it may, since it needs no references.
        if (m_context == va_invalid_id || header.width > m_surface_width || header.height > m_surface_height) {
            if (!header.key_frame && m_context != va_invalid_id)
                return false;
            if (!create_context(header.width, header.height))
                return false;
        }
        int const target = free_surface();
        if (target < 0)
            return false;

        VADecPictureParameterBufferVP9 picture {};
        picture.frame_width = static_cast<std::uint16_t>(header.width);
        picture.frame_height = static_cast<std::uint16_t>(header.height);
        for (std::size_t i = 0; i < 8; ++i)
            picture.reference_frames[i] = m_slots[i] >= 0 ? m_surfaces[static_cast<std::size_t>(m_slots[i])] : va_invalid_id;
        auto& bits = picture.pic_fields.bits;
        bits.subsampling_x = header.subsampling_x;
        bits.subsampling_y = header.subsampling_y;
        bits.frame_type = header.key_frame ? 0 : 1;
        bits.show_frame = header.show_frame;
        bits.error_resilient_mode = header.error_resilient;
        bits.intra_only = header.intra_only;
        bits.allow_high_precision_mv = header.allow_high_precision_mv;
        bits.mcomp_filter_type = static_cast<std::uint32_t>(header.interp_filter);
        bits.frame_parallel_decoding_mode = header.frame_parallel_decoding;
        bits.reset_frame_context = static_cast<std::uint32_t>(header.reset_frame_context);
        bits.refresh_frame_context = header.refresh_frame_context;
        bits.frame_context_idx = static_cast<std::uint32_t>(header.frame_context_idx);
        bits.segmentation_enabled = header.segmentation.enabled;
        bits.segmentation_temporal_update = header.segmentation.temporal_update;
        bits.segmentation_update_map = header.segmentation.update_map;
        bits.last_ref_frame = static_cast<std::uint32_t>(header.ref_frame_idx[0]);
        bits.last_ref_frame_sign_bias = header.ref_sign_bias[1];
        bits.golden_ref_frame = static_cast<std::uint32_t>(header.ref_frame_idx[1]);
        bits.golden_ref_frame_sign_bias = header.ref_sign_bias[2];
        bits.alt_ref_frame = static_cast<std::uint32_t>(header.ref_frame_idx[2]);
        bits.alt_ref_frame_sign_bias = header.ref_sign_bias[3];
        bits.lossless_flag = header.lossless;
        picture.filter_level = static_cast<std::uint8_t>(header.loop_filter.level);
        picture.sharpness_level = static_cast<std::uint8_t>(header.loop_filter.sharpness);
        picture.log2_tile_rows = static_cast<std::uint8_t>(header.tile_rows_log2);
        picture.log2_tile_columns = static_cast<std::uint8_t>(header.tile_cols_log2);
        picture.frame_header_length_in_bytes = static_cast<std::uint8_t>(header.uncompressed_header_size);
        picture.first_partition_size = static_cast<std::uint16_t>(header.compressed_header_size);
        std::copy(header.segmentation.tree_probs.begin(), header.segmentation.tree_probs.end(), picture.mb_segment_tree_probs);
        std::copy(header.segmentation.pred_probs.begin(), header.segmentation.pred_probs.end(), picture.segment_pred_probs);
        picture.profile = static_cast<std::uint8_t>(header.profile);
        picture.bit_depth = 8;

        VASliceParameterBufferVP9 slice {};
        slice.slice_data_size = static_cast<std::uint32_t>(frame.size());
        slice.slice_data_offset = 0;
        slice.slice_data_flag = va_slice_data_flag_all;
        for (int segment = 0; segment < 8; ++segment) {
            auto const s = static_cast<std::size_t>(segment);
            VASegmentParameterVP9& out = slice.seg_param[s];
            Vp9Segmentation const& seg = header.segmentation;
            bool const active = seg.enabled;
            out.segment_flags.fields.segment_reference_enabled = active && seg.feature_enabled[s][2];
            out.segment_flags.fields.segment_reference = static_cast<std::uint16_t>(active ? seg.feature_data[s][2] & 3 : 0);
            out.segment_flags.fields.segment_reference_skipped = active && seg.feature_enabled[s][3];
            auto const levels = vp9_segment_filter_levels(header, segment);
            for (std::size_t ref = 0; ref < 4; ++ref)
                for (std::size_t mode = 0; mode < 2; ++mode)
                    out.filter_level[ref][mode] = levels[ref][mode];
            int const q = vp9_segment_qindex(header, segment);
            out.luma_dc_quant_scale = static_cast<std::int16_t>(vp9_dc_quant(q + header.delta_q_y_dc));
            out.luma_ac_quant_scale = static_cast<std::int16_t>(vp9_ac_quant(q));
            out.chroma_dc_quant_scale = static_cast<std::int16_t>(vp9_dc_quant(q + header.delta_q_uv_dc));
            out.chroma_ac_quant_scale = static_cast<std::int16_t>(vp9_ac_quant(q + header.delta_q_uv_ac));
        }

        std::array<VABufferID, 3> buffers { va_invalid_id, va_invalid_id, va_invalid_id };
        bool ok = m_va.create_buffer(m_display, m_context, va_picture_parameter_buffer, sizeof(picture), 1, &picture, &buffers[0]) == va_success
            && m_va.create_buffer(m_display, m_context, va_slice_parameter_buffer, sizeof(slice), 1, &slice, &buffers[1]) == va_success
            && m_va.create_buffer(m_display, m_context, va_slice_data_buffer, static_cast<unsigned>(frame.size()), 1,
                   const_cast<std::uint8_t*>(frame.data()), &buffers[2])
                == va_success;
        VASurfaceID const surface = m_surfaces[static_cast<std::size_t>(target)];
        if (ok)
            ok = m_va.begin_picture(m_display, m_context, surface) == va_success;
        if (ok) {
            ok = m_va.render_picture(m_display, m_context, buffers.data(), 3) == va_success;
            ok = m_va.end_picture(m_display, m_context) == va_success && ok;
        }
        for (VABufferID const buffer : buffers) {
            if (buffer != va_invalid_id)
                m_va.destroy_buffer(m_display, buffer);
        }
        if (!ok)
            return false;
        m_sizes[static_cast<std::size_t>(target)] = { header.width, header.height };
        for (std::size_t slot = 0; slot < 8; ++slot) {
            if (header.refresh_frame_flags & (1u << slot))
                m_slots[slot] = target;
        }
        m_last = target;
        return true;
    }

    std::optional<Nv12Picture> read_last() override { return m_last >= 0 ? read(m_last) : std::nullopt; }

    std::optional<Nv12Picture> read_slot(int slot) override
    {
        if (slot < 0 || slot > 7 || m_slots[static_cast<std::size_t>(slot)] < 0)
            return std::nullopt;
        return read(m_slots[static_cast<std::size_t>(slot)]);
    }

private:
    static constexpr int surface_count = 12; // eight references, the frame decoded, and room

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
        m_slots.fill(-1);
        m_last = -1;
    }

    bool create_context(int width, int height)
    {
        release_context();
        VAConfigAttrib attribute { va_config_attrib_rt_format, va_rt_format_yuv420 };
        if (m_va.create_config(m_display, va_profile_vp9_profile0, va_entrypoint_vld, &attribute, 1, &m_config) != va_success) {
            m_config = va_invalid_id;
            return false;
        }
        m_surfaces.assign(surface_count, va_invalid_id);
        if (m_va.create_surfaces(m_display, va_rt_format_yuv420, static_cast<unsigned>(width), static_cast<unsigned>(height), m_surfaces.data(),
                surface_count, nullptr, 0)
            != va_success) {
            m_surfaces.clear();
            release_context();
            return false;
        }
        if (m_va.create_context(m_display, m_config, width, height, va_progressive, m_surfaces.data(), surface_count, &m_context) != va_success) {
            m_context = va_invalid_id;
            release_context();
            return false;
        }
        m_surface_width = width;
        m_surface_height = height;
        m_sizes.assign(surface_count, { 0, 0 });
        return true;
    }

    // A surface no reference slot holds, other than the one just decoded
    // (which may still be read back).
    int free_surface() const
    {
        for (int i = 0; i < static_cast<int>(m_surfaces.size()); ++i) {
            if (i == m_last)
                continue;
            if (std::find(m_slots.begin(), m_slots.end(), i) == m_slots.end())
                return i;
        }
        return -1;
    }

    std::optional<Nv12Picture> read(int index)
    {
        VASurfaceID const surface = m_surfaces[static_cast<std::size_t>(index)];
        auto const [width, height] = m_sizes[static_cast<std::size_t>(index)];
        if (width <= 0 || height <= 0 || m_va.sync_surface(m_display, surface) != va_success)
            return std::nullopt;
        // The surface's own memory when the driver lets it be seen, a copy
        // into an image of our asking when it does not (a tiled surface).
        VAImage image {};
        bool derived = m_va.derive_image(m_display, surface, &image) == va_success;
        if (derived && image.format.fourcc != va_fourcc_nv12) {
            m_va.destroy_image(m_display, image.image_id);
            derived = false;
        }
        if (!derived) {
            VAImageFormat format {};
            format.fourcc = va_fourcc_nv12;
            format.byte_order = 1; // VA_LSB_FIRST
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
        picture.width = width;
        picture.height = height;
        auto const* base = static_cast<std::uint8_t const*>(mapped);
        picture.luma.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y)
            std::memcpy(picture.luma.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width),
                base + image.offsets[0] + static_cast<std::size_t>(y) * image.pitches[0], static_cast<std::size_t>(width));
        std::size_t const chroma_row = static_cast<std::size_t>(picture.chroma_width()) * 2;
        picture.chroma.resize(chroma_row * static_cast<std::size_t>(picture.chroma_height()));
        for (int y = 0; y < picture.chroma_height(); ++y)
            std::memcpy(picture.chroma.data() + static_cast<std::size_t>(y) * chroma_row,
                base + image.offsets[1] + static_cast<std::size_t>(y) * image.pitches[1], chroma_row);
        m_va.unmap_buffer(m_display, image.buf);
        m_va.destroy_image(m_display, image.image_id);
        return picture;
    }

    Va m_va;
    int m_fd;
    VADisplay m_display;
    std::string m_device;
    VAConfigID m_config = va_invalid_id;
    VAContextID m_context = va_invalid_id;
    std::vector<VASurfaceID> m_surfaces;
    std::vector<std::pair<int, int>> m_sizes;
    int m_surface_width = 0;
    int m_surface_height = 0;
    std::array<int, 8> m_slots {};
    int m_last = -1;
};

}

std::unique_ptr<Vp9Accelerator> open_vaapi_vp9(std::string& error)
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
        // VP9 profile 0 with a decoding entry point.
        std::vector<int> profiles(static_cast<std::size_t>(std::max(0, va->max_num_profiles(display))));
        int profile_count = 0;
        bool vp9 = !profiles.empty() && va->query_config_profiles(display, profiles.data(), &profile_count) == va_success
            && std::find(profiles.begin(), profiles.begin() + profile_count, va_profile_vp9_profile0) != profiles.begin() + profile_count;
        if (vp9) {
            std::vector<int> entrypoints(static_cast<std::size_t>(std::max(0, va->max_num_entrypoints(display))));
            int entrypoint_count = 0;
            vp9 = !entrypoints.empty()
                && va->query_config_entrypoints(display, va_profile_vp9_profile0, entrypoints.data(), &entrypoint_count) == va_success
                && std::find(entrypoints.begin(), entrypoints.begin() + entrypoint_count, va_entrypoint_vld) != entrypoints.begin() + entrypoint_count;
        }
        if (!vp9) {
            va->terminate(display);
            close(fd);
            tried += (tried.empty() ? "" : ", ") + path + " (no VP9)";
            continue;
        }
        char const* const vendor = va->query_vendor_string(display);
        std::string const description = path + " (" + (vendor ? vendor : "VA-API") + ")";
        return std::make_unique<VaapiVp9>(*va, fd, display, description);
    }
    error = tried.empty() ? "no GPU render node to decode on" : "no device decodes VP9: " + tried;
    dlclose(va->drm);
    dlclose(va->va);
    return nullptr;
}

}
