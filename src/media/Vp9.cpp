#include "media/Vp9.h"

#include <algorithm>
#include <tuple>

namespace sashfold::media {

namespace {

// The quantizer steps at 8 bits (the specification's dc_qlookup and
// ac_qlookup), by index.
constexpr std::int16_t dc_qlookup[256] = {
    4, 8, 8, 9, 10, 11, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19,
    20, 21, 22, 23, 24, 25, 26, 26, 27, 28, 29, 30, 31, 32, 32, 33,
    34, 35, 36, 37, 38, 38, 39, 40, 41, 42, 43, 43, 44, 45, 46, 47,
    48, 48, 49, 50, 51, 52, 53, 53, 54, 55, 56, 57, 57, 58, 59, 60,
    61, 62, 62, 63, 64, 65, 66, 66, 67, 68, 69, 70, 70, 71, 72, 73,
    74, 74, 75, 76, 77, 78, 78, 79, 80, 81, 81, 82, 83, 84, 85, 85,
    87, 88, 90, 92, 93, 95, 96, 98, 99, 101, 102, 104, 105, 107, 108, 110,
    111, 113, 114, 116, 117, 118, 120, 121, 123, 125, 127, 129, 131, 134, 136, 138,
    140, 142, 144, 146, 148, 150, 152, 154, 156, 158, 161, 164, 166, 169, 172, 174,
    177, 180, 182, 185, 187, 190, 192, 195, 199, 202, 205, 208, 211, 214, 217, 220,
    223, 226, 230, 233, 237, 240, 243, 247, 250, 253, 257, 261, 265, 269, 272, 276,
    280, 284, 288, 292, 296, 300, 304, 309, 313, 317, 322, 326, 330, 335, 340, 344,
    349, 354, 359, 364, 369, 374, 379, 384, 389, 395, 400, 406, 411, 417, 423, 429,
    435, 441, 447, 454, 461, 467, 475, 482, 489, 497, 505, 513, 522, 530, 539, 549,
    559, 569, 579, 590, 602, 614, 626, 640, 654, 668, 684, 700, 717, 736, 755, 775,
    796, 819, 843, 869, 896, 925, 955, 988, 1022, 1058, 1098, 1139, 1184, 1232, 1282, 1336,
};
constexpr std::int16_t ac_qlookup[256] = {
    4, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
    23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
    39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,
    55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70,
    71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86,
    87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102,
    104, 106, 108, 110, 112, 114, 116, 118, 120, 122, 124, 126, 128, 130, 132, 134,
    136, 138, 140, 142, 144, 146, 148, 150, 152, 155, 158, 161, 164, 167, 170, 173,
    176, 179, 182, 185, 188, 191, 194, 197, 200, 203, 207, 211, 215, 219, 223, 227,
    231, 235, 239, 243, 247, 251, 255, 260, 265, 270, 275, 280, 285, 290, 295, 300,
    305, 311, 317, 323, 329, 335, 341, 347, 353, 359, 366, 373, 380, 387, 394, 401,
    408, 416, 424, 432, 440, 448, 456, 465, 474, 483, 492, 501, 510, 520, 530, 540,
    550, 560, 571, 582, 593, 604, 615, 627, 639, 651, 663, 676, 689, 702, 715, 729,
    743, 757, 771, 786, 801, 816, 832, 848, 864, 881, 898, 915, 933, 951, 969, 988,
    1007, 1026, 1046, 1066, 1087, 1108, 1129, 1151, 1173, 1196, 1219, 1243, 1267, 1292, 1317, 1343,
    1369, 1396, 1423, 1451, 1479, 1508, 1537, 1567, 1597, 1628, 1660, 1692, 1725, 1759, 1793, 1828,
};

// The header's bits, most significant first (§4.9 f(n)).
class BitReader {
public:
    explicit BitReader(std::span<std::uint8_t const> bytes)
        : m_bytes(bytes)
    {
    }

    bool overrun() const { return m_overrun; }
    std::size_t bit_position() const { return m_position; }

    int bit()
    {
        if (m_position >= m_bytes.size() * 8) {
            m_overrun = true;
            return 0;
        }
        int const value = (m_bytes[m_position >> 3] >> (7 - (m_position & 7))) & 1;
        m_position++;
        return value;
    }

    int bits(int count)
    {
        int value = 0;
        for (int i = 0; i < count; ++i)
            value = (value << 1) | bit();
        return value;
    }

    // su(n): a magnitude, then its sign.
    int signed_bits(int count)
    {
        int const value = bits(count);
        return bit() ? -value : value;
    }

private:
    std::span<std::uint8_t const> m_bytes;
    std::size_t m_position = 0;
    bool m_overrun = false;
};

constexpr int feature_bits[4] = { 8, 6, 2, 0 };
constexpr bool feature_signed[4] = { true, true, false, false };
constexpr int feature_max[4] = { 255, 63, 3, 0 };

}

std::vector<std::span<std::uint8_t const>> split_vp9_superframe(std::span<std::uint8_t const> block)
{
    std::vector<std::span<std::uint8_t const>> frames;
    if (block.empty())
        return frames;
    std::uint8_t const marker = block.back();
    if ((marker & 0xe0) == 0xc0) {
        std::size_t const count = (marker & 0x7) + 1u;
        std::size_t const size_bytes = ((marker >> 3) & 0x3) + 1u;
        std::size_t const index_size = 2 + size_bytes * count;
        if (block.size() >= index_size && block[block.size() - index_size] == marker) {
            std::size_t at = block.size() - index_size + 1;
            std::size_t offset = 0;
            for (std::size_t i = 0; i < count; ++i) {
                std::size_t size = 0;
                for (std::size_t b = 0; b < size_bytes; ++b)
                    size |= static_cast<std::size_t>(block[at++]) << (8 * b);
                if (offset + size > block.size() - index_size)
                    return {};
                frames.push_back(block.subspan(offset, size));
                offset += size;
            }
            return frames;
        }
    }
    frames.push_back(block);
    return frames;
}

void Vp9HeaderReader::reset()
{
    *this = Vp9HeaderReader {};
}

std::optional<Vp9FrameHeader> Vp9HeaderReader::read(std::span<std::uint8_t const> frame)
{
    BitReader in(frame);
    Vp9FrameHeader header;
    if (in.bits(2) != 2) // frame_marker
        return std::nullopt;
    int const profile_low = in.bit();
    int const profile_high = in.bit();
    header.profile = (profile_high << 1) + profile_low;
    if (header.profile == 3 && in.bit() != 0)
        return std::nullopt;
    header.show_existing_frame = in.bit();
    if (header.show_existing_frame) {
        header.frame_to_show = in.bits(3);
        header.show_frame = true;
        header.uncompressed_header_size = static_cast<int>((in.bit_position() + 7) / 8);
        if (in.overrun() || !m_seen_key_frame)
            return std::nullopt;
        std::tie(header.width, header.height) = m_slot_sizes[static_cast<std::size_t>(header.frame_to_show)];
        return header;
    }
    header.key_frame = in.bit() == 0;
    header.show_frame = in.bit();
    header.error_resilient = in.bit();

    auto const sync_code = [&in] { return in.bits(8) == 0x49 && in.bits(8) == 0x83 && in.bits(8) == 0x42; };
    auto const color_config = [&in, &header] {
        header.bit_depth = header.profile >= 2 ? (in.bit() ? 12 : 10) : 8;
        header.color_space = in.bits(3);
        if (header.color_space != 7) { // not RGB
            header.color_range = in.bit();
            if (header.profile == 1 || header.profile == 3) {
                header.subsampling_x = in.bit();
                header.subsampling_y = in.bit();
                if (in.bit() != 0)
                    return false;
            } else {
                header.subsampling_x = header.subsampling_y = true;
            }
        } else {
            header.color_range = true;
            if (header.profile == 1 || header.profile == 3) {
                header.subsampling_x = header.subsampling_y = false;
                if (in.bit() != 0)
                    return false;
            }
        }
        return true;
    };
    auto const frame_size = [&in, &header] {
        header.width = in.bits(16) + 1;
        header.height = in.bits(16) + 1;
    };
    auto const render_size = [&in, &header] {
        if (in.bit()) {
            header.render_width = in.bits(16) + 1;
            header.render_height = in.bits(16) + 1;
        } else {
            header.render_width = header.width;
            header.render_height = header.height;
        }
    };

    if (header.key_frame) {
        if (!sync_code() || !color_config())
            return std::nullopt;
        frame_size();
        render_size();
        header.refresh_frame_flags = 0xFF;
        m_seen_key_frame = true;
    } else {
        if (!m_seen_key_frame)
            return std::nullopt;
        header.intra_only = header.show_frame ? false : in.bit() != 0;
        header.reset_frame_context = header.error_resilient ? 0 : in.bits(2);
        if (header.intra_only) {
            if (!sync_code())
                return std::nullopt;
            if (header.profile > 0) {
                if (!color_config())
                    return std::nullopt;
            } else {
                header.color_space = 1; // BT.601
                header.subsampling_x = header.subsampling_y = true;
                header.bit_depth = 8;
            }
            header.refresh_frame_flags = static_cast<std::uint8_t>(in.bits(8));
            frame_size();
            render_size();
        } else {
            header.refresh_frame_flags = static_cast<std::uint8_t>(in.bits(8));
            for (std::size_t i = 0; i < 3; ++i) {
                header.ref_frame_idx[i] = in.bits(3);
                header.ref_sign_bias[i + 1] = in.bit();
            }
            // The size of a reference, or one of its own.
            bool found = false;
            for (int const slot : header.ref_frame_idx) {
                if (in.bit()) {
                    std::tie(header.width, header.height) = m_slot_sizes[static_cast<std::size_t>(slot)];
                    found = true;
                    break;
                }
            }
            if (!found)
                frame_size();
            render_size();
            header.allow_high_precision_mv = in.bit();
            if (in.bit()) {
                header.interp_filter = 4;
            } else {
                static constexpr int literal_to_type[4] = { 1, 0, 2, 3 };
                header.interp_filter = literal_to_type[in.bits(2)];
            }
        }
    }
    if (header.width <= 0 || header.height <= 0)
        return std::nullopt;

    if (!header.error_resilient) {
        header.refresh_frame_context = in.bit();
        header.frame_parallel_decoding = in.bit();
    } else {
        header.frame_parallel_decoding = true;
    }
    header.frame_context_idx = in.bits(2);

    // A frame that cannot lean on what came before starts the carried
    // state afresh (setup_past_independence).
    if (header.intra() || header.error_resilient) {
        for (auto& features : m_segmentation.feature_enabled)
            features.fill(false);
        for (auto& data : m_segmentation.feature_data)
            data.fill(0);
        m_segmentation.abs_or_delta_update = false;
        m_loop_filter.delta_enabled = true;
        m_loop_filter.ref_deltas = { 1, 0, -1, -1 };
        m_loop_filter.mode_deltas = { 0, 0 };
    }

    // The loop filter.
    m_loop_filter.level = in.bits(6);
    m_loop_filter.sharpness = in.bits(3);
    m_loop_filter.delta_update = false;
    m_loop_filter.delta_enabled = in.bit();
    if (m_loop_filter.delta_enabled) {
        m_loop_filter.delta_update = in.bit();
        if (m_loop_filter.delta_update) {
            for (int& delta : m_loop_filter.ref_deltas) {
                if (in.bit())
                    delta = in.signed_bits(6);
            }
            for (int& delta : m_loop_filter.mode_deltas) {
                if (in.bit())
                    delta = in.signed_bits(6);
            }
        }
    }
    header.loop_filter = m_loop_filter;

    // The quantizer.
    header.base_q_idx = in.bits(8);
    auto const delta_q = [&in] { return in.bit() ? in.signed_bits(4) : 0; };
    header.delta_q_y_dc = delta_q();
    header.delta_q_uv_dc = delta_q();
    header.delta_q_uv_ac = delta_q();
    header.lossless = header.base_q_idx == 0 && header.delta_q_y_dc == 0 && header.delta_q_uv_dc == 0 && header.delta_q_uv_ac == 0;

    // Segmentation.
    m_segmentation.update_map = false;
    m_segmentation.update_data = false;
    m_segmentation.enabled = in.bit();
    if (m_segmentation.enabled) {
        m_segmentation.update_map = in.bit();
        if (m_segmentation.update_map) {
            auto const prob = [&in] { return static_cast<std::uint8_t>(in.bit() ? in.bits(8) : 255); };
            for (std::uint8_t& p : m_segmentation.tree_probs)
                p = prob();
            m_segmentation.temporal_update = in.bit();
            for (std::uint8_t& p : m_segmentation.pred_probs)
                p = m_segmentation.temporal_update ? prob() : 255;
        }
        m_segmentation.update_data = in.bit();
        if (m_segmentation.update_data) {
            m_segmentation.abs_or_delta_update = in.bit();
            for (std::size_t i = 0; i < 8; ++i) {
                for (std::size_t j = 0; j < 4; ++j) {
                    int value = 0;
                    bool const enabled = in.bit();
                    m_segmentation.feature_enabled[i][j] = enabled;
                    if (enabled) {
                        value = std::min(in.bits(feature_bits[j]), feature_max[j]);
                        if (feature_signed[j] && in.bit())
                            value = -value;
                    }
                    m_segmentation.feature_data[i][j] = value;
                }
            }
        }
    }
    header.segmentation = m_segmentation;

    // Tiles: at most 4096 pixels (64 superblocks) a column, at least 256
    // (4 superblocks).
    int const mi_cols = (header.width + 7) >> 3;
    int const sb64_cols = (mi_cols + 7) >> 3;
    int min_log2 = 0;
    while ((64 << min_log2) < sb64_cols)
        min_log2++;
    int max_log2 = 1;
    while ((sb64_cols >> max_log2) >= 4)
        max_log2++;
    max_log2--;
    header.tile_cols_log2 = min_log2;
    while (header.tile_cols_log2 < max_log2 && in.bit())
        header.tile_cols_log2++;
    header.tile_rows_log2 = in.bit();
    if (header.tile_rows_log2)
        header.tile_rows_log2 += in.bit();

    header.compressed_header_size = in.bits(16);
    header.uncompressed_header_size = static_cast<int>((in.bit_position() + 7) / 8);
    if (in.overrun() || header.compressed_header_size == 0
        || static_cast<std::size_t>(header.uncompressed_header_size + header.compressed_header_size) > frame.size())
        return std::nullopt;

    // The slots this frame fills now hold its size.
    for (std::size_t slot = 0; slot < 8; ++slot) {
        if (header.refresh_frame_flags & (1u << slot))
            m_slot_sizes[slot] = { header.width, header.height };
    }
    return header;
}

int vp9_dc_quant(int qindex)
{
    return dc_qlookup[std::clamp(qindex, 0, 255)];
}

int vp9_ac_quant(int qindex)
{
    return ac_qlookup[std::clamp(qindex, 0, 255)];
}

int vp9_segment_qindex(Vp9FrameHeader const& header, int segment)
{
    Vp9Segmentation const& seg = header.segmentation;
    auto const s = static_cast<std::size_t>(segment);
    if (seg.enabled && seg.feature_enabled[s][0]) {
        int const data = seg.feature_data[s][0];
        return std::clamp(seg.abs_or_delta_update ? data : header.base_q_idx + data, 0, 255);
    }
    return header.base_q_idx;
}

std::array<std::array<std::uint8_t, 2>, 4> vp9_segment_filter_levels(Vp9FrameHeader const& header, int segment)
{
    Vp9Segmentation const& seg = header.segmentation;
    Vp9LoopFilter const& lf = header.loop_filter;
    auto const s = static_cast<std::size_t>(segment);
    int level = lf.level;
    if (seg.enabled && seg.feature_enabled[s][1]) {
        int const data = seg.feature_data[s][1];
        level = std::clamp(seg.abs_or_delta_update ? data : level + data, 0, 63);
    }
    std::array<std::array<std::uint8_t, 2>, 4> levels {};
    if (!lf.delta_enabled) {
        for (auto& by_mode : levels)
            by_mode.fill(static_cast<std::uint8_t>(level));
        return levels;
    }
    // The deltas scale with the frame's own level, as the reference decoder
    // does it.
    int const scale = 1 << (lf.level >> 5);
    auto const clamp63 = [](int value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 63)); };
    levels[0][0] = levels[0][1] = clamp63(level + lf.ref_deltas[0] * scale);
    for (std::size_t ref = 1; ref < 4; ++ref) {
        for (std::size_t mode = 0; mode < 2; ++mode)
            levels[ref][mode] = clamp63(level + lf.ref_deltas[ref] * scale + lf.mode_deltas[mode] * scale);
    }
    return levels;
}

}
