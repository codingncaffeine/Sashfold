// H.264 decoded on the processor, for a machine whose graphics have no
// H.264 decoder (or none we can reach): the same accelerator interface
// the hardware paths implement, so the host's buffer bookkeeping
// (media/H264Decoder.h) drives it unchanged. Written from ITU-T H.264
// alone; held to ffmpeg's decoding of the same files bit for bit.
#include "media/H264Software.h"

#include "media/H264SoftwareInternal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::media {

namespace h264sw {

void Picture::allocate(int mb_w, int mb_h)
{
    mb_width = mb_w;
    mb_height = mb_h;
    width = mb_w * 16;
    height = mb_h * 16;
    std::size_t const luma = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    y.assign(luma, 128);
    cb.assign(luma / 4, 128);
    cr.assign(luma / 4, 128);
    std::size_t const blocks = static_cast<std::size_t>(mb_w) * static_cast<std::size_t>(mb_h) * 16;
    for (int l = 0; l < 2; ++l) {
        mv[static_cast<std::size_t>(l)].assign(blocks, Mv {});
        ref_idx[static_cast<std::size_t>(l)].assign(blocks, -1);
        ref_pic[static_cast<std::size_t>(l)].assign(blocks, -1);
    }
    mb_intra.assign(static_cast<std::size_t>(mb_w) * static_cast<std::size_t>(mb_h), 1);
}

}

namespace {

using namespace h264sw;

constexpr int slot_count = 17;

class SoftwareH264 final : public H264Accelerator {
public:
    explicit SoftwareH264(int max_macroblocks)
        : m_max_macroblocks(max_macroblocks)
    {
        m_pictures.resize(slot_count);
    }

    std::string const& device() const override { return m_device; }
    bool is_software() const override { return true; }
    int slots() const override { return slot_count; }

    bool set_parameter_sets(std::vector<H264Sps> const& sps, std::vector<H264Pps> const& pps) override
    {
        m_sps.clear();
        m_pps.clear();
        for (H264Sps const& s : sps)
            m_sps[s.id] = s;
        for (H264Pps const& p : pps)
            m_pps[p.id] = p;
        m_scales.clear();
        return true;
    }

    bool decode(H264PictureInfo const& info) override
    {
        if (!info.sps || !info.pps || info.slot < 0 || info.slot >= slot_count)
            return false;
        H264Sps const& sps = *info.sps;
        H264Pps const& pps = *info.pps;
        // What is written here: frames, 4:2:0, 8 bits, one slice group.
        if (!sps.frame_mbs_only || sps.chroma_format_idc != 1 || sps.bit_depth_luma != 8 || sps.bit_depth_chroma != 8
            || pps.num_slice_groups != 1 || sps.pic_width_in_mbs <= 0 || sps.pic_height_in_map_units <= 0
            || static_cast<long long>(sps.pic_width_in_mbs) * sps.frame_height_in_mbs() > m_max_macroblocks)
            return false;
        int const mb_w = sps.pic_width_in_mbs, mb_h = sps.frame_height_in_mbs();
        Picture& pic = m_pictures[static_cast<std::size_t>(info.slot)];
        if (pic.mb_width != mb_w || pic.mb_height != mb_h)
            pic.allocate(mb_w, mb_h);
        pic.serial = ++m_serial;
        pic.poc = std::min(info.poc_top, info.poc_bottom);
        pic.valid = true;
        // what read() shows: the sequence's cropping (7.4.2.1.1, frames, 4:2:0)
        pic.crop_x = sps.frame_cropping ? sps.crop_left * 2 : 0;
        pic.crop_y = sps.frame_cropping ? sps.crop_top * 2 : 0;
        pic.shown_width = sps.width();
        pic.shown_height = sps.height();
        std::fill(pic.mb_intra.begin(), pic.mb_intra.end(), std::uint8_t { 1 });

        Frame frame;
        frame.picture = &pic;
        frame.mbs.assign(static_cast<std::size_t>(mb_w) * static_cast<std::size_t>(mb_h), MbState {});
        frame.chroma_qp_offset[0] = pps.chroma_qp_index_offset;
        frame.chroma_qp_offset[1] = pps.second_chroma_qp_index_offset; // the first's value where the set gives none

        LevelScales const& scales = scales_for(pps);
        int const max_frame_num = 1 << sps.log2_max_frame_num;
        std::vector<H264ListPicture> const references = h264_list_pictures(info, max_frame_num);
        int const current_poc = std::min(info.poc_top, info.poc_bottom);
        bool any = false;
        for (std::size_t i = 0; i < info.slices.size() && i < info.headers.size(); ++i) {
            H264SliceHeader const& h = info.headers[i];
            Slice slice;
            slice.sps = &sps;
            slice.pps = &pps;
            slice.header = &h;
            slice.number = static_cast<int>(i);
            slice.type = h.slice_type == H264SliceType::SP ? H264SliceType::P : (h.slice_type == H264SliceType::SI ? H264SliceType::I : h.slice_type);
            if (h.slice_type == H264SliceType::SP || h.slice_type == H264SliceType::SI)
                continue; // the Extended profile's switching slices are not written
            slice.qp = std::clamp(pps.pic_init_qp + h.slice_qp_delta, 0, 51); // 7.4.3's range, held for a broken stream
            slice.current_poc = current_poc;
            slice.scales = &scales;
            bool const is_b = slice.type == H264SliceType::B;
            if (slice.type != H264SliceType::I) {
                for (std::size_t list = 0; list < (is_b ? 2u : 1u); ++list) {
                    for (H264ListPicture const& e : h264_reference_list(references, h, list, is_b, current_poc, max_frame_num, info.frame_num)) {
                        RefEntry r;
                        if (e.slot >= 0 && e.slot < slot_count && e.slot != info.slot && m_pictures[static_cast<std::size_t>(e.slot)].valid
                            && m_pictures[static_cast<std::size_t>(e.slot)].mb_width == mb_w && m_pictures[static_cast<std::size_t>(e.slot)].mb_height == mb_h)
                            r.picture = &m_pictures[static_cast<std::size_t>(e.slot)];
                        r.long_term = e.long_term;
                        r.poc = e.poc;
                        slice.list[list].push_back(r);
                    }
                }
            }
            if (slice.type == H264SliceType::P)
                slice.weighted_mode = pps.weighted_pred ? 1 : 0;
            else if (is_b)
                slice.weighted_mode = pps.weighted_bipred_idc;
            if (slice.weighted_mode == 2)
                implicit_weights(slice);
            if (frame.slice_alpha.size() <= i) {
                frame.slice_alpha.resize(i + 1);
                frame.slice_beta.resize(i + 1);
                frame.slice_disable.resize(i + 1);
            }
            frame.slice_alpha[i] = h.slice_alpha_c0_offset_div2 * 2;
            frame.slice_beta[i] = h.slice_beta_offset_div2 * 2;
            frame.slice_disable[i] = h.disable_deblocking_filter_idc;

            std::vector<std::uint8_t> const rbsp = h264_rbsp(info.slices[i]);
            if (h.header_bits < 8)
                continue;
            BitReader reader(rbsp.data(), rbsp.size(), static_cast<std::size_t>(h.header_bits - 8));
            if (decode_slice_data(frame, slice, reader))
                any = true;
        }
        deblock_picture(frame);
        return any;
    }

    std::optional<Nv12Picture> read(int slot) override
    {
        if (slot < 0 || slot >= slot_count || !m_pictures[static_cast<std::size_t>(slot)].valid)
            return std::nullopt;
        Picture const& p = m_pictures[static_cast<std::size_t>(slot)];
        int const crop_x = p.crop_x, crop_y = p.crop_y;
        Nv12Picture out;
        out.width = std::min(p.shown_width, p.width - crop_x);
        out.height = std::min(p.shown_height, p.height - crop_y);
        if (out.width <= 0 || out.height <= 0)
            return std::nullopt;
        out.luma.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
        for (int y = 0; y < out.height; ++y)
            std::copy_n(&p.y[static_cast<std::size_t>((y + crop_y) * p.width + crop_x)], out.width, &out.luma[static_cast<std::size_t>(y * out.width)]);
        int const cw = out.chroma_width(), ch = out.chroma_height();
        out.chroma.resize(static_cast<std::size_t>(cw) * 2 * static_cast<std::size_t>(ch));
        int const pcw = p.width / 2;
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                std::size_t const src = static_cast<std::size_t>((y + crop_y / 2) * pcw + x + crop_x / 2);
                out.chroma[static_cast<std::size_t>((y * cw + x) * 2)] = p.cb[src];
                out.chroma[static_cast<std::size_t>((y * cw + x) * 2 + 1)] = p.cr[src];
            }
        return out;
    }

    void reset() override
    {
        for (Picture& p : m_pictures)
            p.valid = false;
    }

private:
    LevelScales const& scales_for(H264Pps const& pps)
    {
        auto found = m_scales.find(pps.id);
        if (found != m_scales.end())
            return found->second;
        LevelScales& s = m_scales[pps.id];
        s.build(pps.scaling);
        return s;
    }

    // 8.4.2.3.1: implicit weights for every pair of references.
    static void implicit_weights(Slice& slice)
    {
        slice.implicit.assign(32 * 32, { 32, 32 });
        for (std::size_t r0 = 0; r0 < slice.list[0].size() && r0 < 32; ++r0)
            for (std::size_t r1 = 0; r1 < slice.list[1].size() && r1 < 32; ++r1) {
                RefEntry const& p0 = slice.list[0][r0];
                RefEntry const& p1 = slice.list[1][r1];
                int const tb = std::clamp(slice.current_poc - p0.poc, -128, 127);
                int const td = std::clamp(p1.poc - p0.poc, -128, 127);
                std::array<int, 2> w { 32, 32 };
                if (td != 0 && !p0.long_term && !p1.long_term) {
                    int const tx = (16384 + std::abs(td / 2)) / td;
                    int const dsf = std::clamp((tb * tx + 32) >> 6, -1024, 1023);
                    if ((dsf >> 2) >= -64 && (dsf >> 2) <= 128)
                        w = { 64 - (dsf >> 2), dsf >> 2 };
                }
                slice.implicit[r0 * 32 + r1] = w;
            }
    }

    std::string m_device = "software (Sashfold's own H.264 decoder)";
    std::vector<Picture> m_pictures;
    std::map<int, H264Sps> m_sps;
    std::map<int, H264Pps> m_pps;
    std::map<int, LevelScales> m_scales;
    std::int32_t m_serial = 0;
    int m_max_macroblocks;
};

}

std::unique_ptr<H264Accelerator> open_software_h264(int max_macroblocks)
{
    return std::make_unique<SoftwareH264>(max_macroblocks);
}

}
