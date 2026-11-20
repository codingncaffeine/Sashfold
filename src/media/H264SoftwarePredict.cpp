// A macroblock's motion (8.4.1), its prediction — intra (8.3) or inter
// (8.4.2, weighted as 8.4.2.3 says) — and its residual added (8.5).
#include "media/H264SoftwareInternal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

namespace sashfold::media::h264sw {

namespace {

constexpr std::uint8_t blk_to_raster[16] = { 0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15 };

// 8.5.8 and Table 8-15: QPC of a chroma component (without QpBdOffsetC).
int chroma_qp(int qp_y, int offset, int qp_bd_offset_chroma)
{
    static constexpr std::uint8_t table[22] = { 29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39 };
    int const qpi = std::clamp(qp_y + offset, -qp_bd_offset_chroma, 51);
    return qpi < 30 ? qpi : table[qpi - 30];
}

struct Neighbor {
    bool available = false;
    int ref = -1;
    Mv mv;
    std::int32_t pic = -1;
};

// How a co-located motion vector's vertical component is scaled (8.4.1.2.1).
enum class VertMvScale : std::uint8_t {
    OneToOne,
    FrmToFld,
    FldToFrm,
};

class MotionContext {
public:
    MotionContext(Frame& frame, Slice const& slice, int addr)
        : m_frame(frame)
        , m_slice(slice)
        , m_pic(*frame.picture)
        , m_addr(addr)
        , m_w(frame.mb_width)
        , m_x(addr % frame.mb_width)
        , m_y(addr / frame.mb_width)
        , m_bw(frame.block_stride())
        , m_base(frame.mbaff ? frame.block_base(addr) : frame.block_base_at(m_x, m_y))
        , m_row_step(frame.block_row_step())
        , m_field_mb(frame.mbs[static_cast<std::size_t>(addr)].field)
        , m_lists(&slice.list)
        , m_poc(slice.current_poc)
    {
        // An MBAFF field macroblock predicts from fields (8.4.2.1), and its
        // order counts are its field's (8.4.1.2.3, 8.4.3).
        if (frame.mbaff && m_field_mb) {
            m_parity = addr & 1;
            m_lists = &slice.field_list[static_cast<std::size_t>(m_parity)];
            m_poc = m_parity ? slice.poc_bottom : slice.poc_top;
        }
    }

    int storage_mb() const { return m_frame.mbaff ? m_frame.storage_mb(m_addr) : m_frame.storage_mb_at(m_x, m_y); }

    // at() outside the macroblock in an MBAFF frame: 6.4.12.2 for the luma
    // location, then 8.4.1.3.2's scaling between a frame and a field
    // macroblock.
    [[gnu::noinline]] Neighbor at_mbaff(int list, int x_n, int y_n) const
    {
        Neighbor n;
        Frame::Location const loc = m_frame.neighbor(m_addr, x_n, y_n, 16, 16, m_slice.number);
        if (loc.addr < 0)
            return n;
        n.available = true;
        MbState const& s = m_frame.mbs[static_cast<std::size_t>(loc.addr)];
        if (s.intra)
            return n;
        int const i = m_frame.block_base(loc.addr) + (loc.y >> 2) * m_bw + (loc.x >> 2);
        n.ref = m_pic.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
        if (n.ref < 0)
            return n;
        n.mv = m_pic.mv[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
        if (m_field_mb && !s.field) {
            n.mv.y = static_cast<std::int16_t>(n.mv.y / 2); // (8-217), (8-218)
            n.ref *= 2;
        } else if (!m_field_mb && s.field) {
            n.mv.y = static_cast<std::int16_t>(n.mv.y * 2); // (8-219), (8-220)
            n.ref >>= 1;
        }
        return n;
    }

    bool mb_available(int mx, int my) const
    {
        if (mx < 0 || my < 0 || mx >= m_w)
            return false;
        int const a = my * m_w + mx;
        return a != m_addr && m_frame.mbs[static_cast<std::size_t>(a)].slice == m_slice.number;
    }

    // The motion of list `list` at luma location (x_n, y_n) relative to the
    // current macroblock (6.4.11.7 with 8.4.1.3.2's rules): A is (x - 1, y),
    // B (x, y - 1), C (x + width, y - 1), D (x - 1, y - 1) of a partition at
    // (x, y) — the exact row matters to an MBAFF field neighbor.
    Neighbor at(int list, int x_n, int y_n) const
    {
        Neighbor n;
        int const bx = x_n >> 2, by = y_n >> 2;
        int mx = m_x, my = m_y;
        if (by < 0) {
            my -= 1;
            if (bx < 0)
                mx -= 1;
            else if (bx >= 4)
                mx += 1;
        } else if (by >= 4) {
            return n;
        } else if (bx < 0) {
            mx -= 1;
        } else if (bx >= 4) {
            return n;
        } else {
            if (!((m_done >> (by * 4 + bx)) & 1))
                return n;
            n.available = true;
            int const i = m_base + by * m_bw + bx;
            n.ref = m_pic.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
            if (n.ref >= 0) {
                n.mv = m_pic.mv[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
                n.pic = m_pic.ref_pic[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
            }
            return n;
        }
        if (m_frame.mbaff)
            return at_mbaff(list, x_n, y_n);
        if (!mb_available(mx, my))
            return n;
        n.available = true;
        int const neighbor = my * m_w + mx;
        if (m_frame.mbs[static_cast<std::size_t>(neighbor)].intra)
            return n;
        int const i = m_base + (my - m_y) * m_row_step + (mx - m_x) * 4 + ((by + 4) & 3) * m_bw + ((bx + 4) & 3);
        n.ref = m_pic.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
        if (n.ref >= 0)
            n.mv = m_pic.mv[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
        else
            n.mv = {};
        return n;
    }

    // 8.4.1.3: the motion vector predictor. shape: 0 any, 1 16x8, 2 8x16.
    Mv predict(int list, int ref, int x4, int y4, int w4, int shape, int part) const
    {
        Neighbor a = at(list, x4 * 4 - 1, y4 * 4);
        Neighbor b = at(list, x4 * 4, y4 * 4 - 1);
        Neighbor c = at(list, (x4 + w4) * 4, y4 * 4 - 1);
        if (!c.available)
            c = at(list, x4 * 4 - 1, y4 * 4 - 1);
        if (shape == 1) {
            if (part == 0 && b.ref == ref)
                return b.mv;
            if (part == 1 && a.ref == ref)
                return a.mv;
        } else if (shape == 2) {
            if (part == 0 && a.ref == ref)
                return a.mv;
            if (part == 1 && c.ref == ref)
                return c.mv;
        }
        if (!b.available && !c.available && a.available) {
            b = a;
            c = a;
        }
        int const matches = (a.ref == ref) + (b.ref == ref) + (c.ref == ref);
        if (matches == 1) {
            if (a.ref == ref)
                return a.mv;
            if (b.ref == ref)
                return b.mv;
            return c.mv;
        }
        auto median = [](int p, int q, int r) { return std::max(std::min(p, q), std::min(std::max(p, q), r)); };
        return { static_cast<std::int16_t>(median(a.mv.x, b.mv.x, c.mv.x)), static_cast<std::int16_t>(median(a.mv.y, b.mv.y, c.mv.y)) };
    }

    void store(int x4, int y4, int w4, int h4, int list, int ref, Mv mv)
    {
        std::int32_t pic = -1;
        std::vector<RefEntry> const& l = (*m_lists)[static_cast<std::size_t>(list)];
        if (ref >= 0 && static_cast<std::size_t>(ref) < l.size())
            pic = l[static_cast<std::size_t>(ref)].id();
        for (int y = y4; y < y4 + h4; ++y)
            for (int x = x4; x < x4 + w4; ++x) {
                int const i = m_base + y * m_bw + x;
                m_pic.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)] = static_cast<std::int8_t>(ref);
                m_pic.mv[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)] = ref >= 0 ? mv : Mv {};
                m_pic.ref_pic[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)] = pic;
            }
    }
    void mark(int x4, int y4, int w4, int h4)
    {
        for (int y = y4; y < y4 + h4; ++y)
            for (int x = x4; x < x4 + w4; ++x)
                m_done |= 1u << (y * 4 + x);
    }

    // 8.4.1.2.1: the co-located block's motion — in the frame or field
    // Table 8-6 picks, at the macroblock and row Table 8-8 gives.
    struct Col {
        Mv mv;
        int ref = -1;
        std::int32_t pic = -1; // the picture its reference index named (RefEntry::id)
        VertMvScale scale = VertMvScale::OneToOne;
    };
    [[gnu::always_inline]] inline Col colocated(int bx, int by) const
    {
        Col c;
        if (m_slice.list[1].empty() || !m_slice.list[1][0].picture)
            return c;
        RefEntry const& first = m_slice.list[1][0];
        Picture const& col = *first.picture;
        if (col.mb_width != m_pic.mb_width || col.mb_height != m_pic.mb_height)
            return c;
        if (m_slice.sps->direct_8x8_inference) {
            bx = (bx >> 1) * 3;
            by = (by >> 1) * 3;
        }
        if (m_slice.structure == 0 && !m_slice.mbaff && col.coding == Coding::Frame) {
            // A frame from a frame (Table 8-8's FRM, FRM): the same
            // macroblock and block, kept where this picture keeps its own.
            int const at = m_y * m_w + m_x;
            if (col.mb_intra[static_cast<std::size_t>(at)])
                return c;
            int const i = m_base + by * m_bw + bx;
            int const list = col.ref_idx[0][static_cast<std::size_t>(i)] < 0 ? 1 : 0;
            c.ref = col.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
            c.mv = col.mv[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
            c.pic = col.ref_pic[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
            return c;
        }
        return colocated_interlaced(first, bx, by);
    }

    // colocated() where fields are involved: the current picture is a
    // field or an MBAFF frame, or the co-located one is.
    [[gnu::noinline]] Col colocated_interlaced(RefEntry const& first, int bx, int by) const
    {
        Col c;
        Picture const& col = *first.picture;
        int const x_col = bx * 4, y_col = by * 4;
        int const curr = m_addr, w = m_w;
        bool const field_mb = m_frame.mbs[static_cast<std::size_t>(curr)].field;
        // Table 8-6: a field of a decoded frame is taken with its frame; a
        // frame's field pair gives up the field nearer in order (frame
        // macroblocks) or of the macroblock's parity (field ones).
        bool col_is_fields = col.coding == Coding::Fields;
        int col_parity = 0; // of a field colPic: 0 top, 1 bottom
        if (m_slice.structure != 0) {
            col_parity = first.structure == 2 ? 1 : 0;
        } else if (col_is_fields) {
            int const top_diff = std::abs(first.poc_top - m_slice.current_poc);
            int const bottom_diff = std::abs(first.poc_bottom - m_slice.current_poc);
            col_parity = field_mb ? (curr & 1) : (top_diff < bottom_diff ? 0 : 1);
        }
        // The co-located macroblock's motion row and column (Picture), and yM.
        int row = 0, column = 0, y_m = y_col;
        auto frame_mb = [&](int a) { row = a / w; column = a % w; };
        auto field_mb_of = [&](int a, int parity) { row = (a / w) * 2 + parity; column = a % w; };
        auto mbaff_mb = [&](int a) { row = ((a >> 1) / w) * 2 + (a & 1); column = (a >> 1) % w; };
        auto is_field_at = [&](int a) { return col.mb_field[static_cast<std::size_t>((((a >> 1) / w) * 2 + (a & 1)) * w + (a >> 1) % w)] != 0; };
        if (m_slice.structure != 0) { // FLD
            if (col_is_fields) {
                field_mb_of(curr, col_parity);
            } else if (col.coding == Coding::Frame) {
                frame_mb(2 * w * (curr / w) + (curr % w) + w * (y_col / 8)); // (8-177)
                y_m = (2 * y_col) % 16;
                c.scale = VertMvScale::FrmToFld;
            } else if (!is_field_at(2 * curr)) {
                mbaff_mb(2 * curr + y_col / 8); // (8-178)
                y_m = (2 * y_col) % 16;
                c.scale = VertMvScale::FrmToFld;
            } else {
                mbaff_mb(2 * curr + (m_slice.structure == 2 ? 1 : 0)); // (8-179)
            }
        } else if (!m_slice.mbaff) { // FRM
            if (col_is_fields) {
                field_mb_of(w * (curr / (2 * w)) + (curr % w), col_parity); // (8-180)
                y_m = 8 * ((curr / w) % 2) + 4 * (y_col / 8);
                c.scale = VertMvScale::FldToFrm;
            } else {
                frame_mb(curr);
            }
        } else if (col_is_fields) { // AFRM, the co-located a field
            field_mb_of(curr / 2, col_parity); // (8-181)
            if (!field_mb) {
                y_m = 8 * (curr % 2) + 4 * (y_col / 8);
                c.scale = VertMvScale::FldToFrm;
            }
        } else { // AFRM, the co-located an MBAFF frame
            bool const x_field = is_field_at(curr);
            if (!field_mb && !x_field) {
                mbaff_mb(curr);
            } else if (!field_mb) {
                int const top_diff = std::abs(first.poc_top - m_slice.current_poc);
                int const bottom_diff = std::abs(first.poc_bottom - m_slice.current_poc);
                mbaff_mb(2 * (curr / 2) + (top_diff < bottom_diff ? 0 : 1)); // (8-182)
                y_m = 8 * (curr % 2) + 4 * (y_col / 8);
                c.scale = VertMvScale::FldToFrm;
            } else if (!x_field) {
                mbaff_mb(2 * (curr / 2) + y_col / 8); // (8-183)
                y_m = (2 * y_col) % 16;
                c.scale = VertMvScale::FrmToFld;
            } else {
                mbaff_mb(curr);
            }
        }
        if (row < 0 || row >= col.mb_height || column < 0 || column >= col.mb_width)
            return c;
        if (col.mb_intra[static_cast<std::size_t>(row * w + column)])
            return c;
        int const i = (row * 4 + y_m / 4) * m_bw + column * 4 + x_col / 4;
        int const list = col.ref_idx[0][static_cast<std::size_t>(i)] < 0 ? 1 : 0;
        c.ref = col.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
        c.mv = col.mv[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
        c.pic = col.ref_pic[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
        return c;
    }

    // 8.4.1.2.3: MapColToList0, the lowest index of list 0 naming the
    // picture the co-located block referred to (or its frame, or its field
    // of the current parity, as vertMvScale has it).
    int map_col_to_list0(Col const& c) const
    {
        auto const& l0 = m_slice.list[0];
        std::int32_t const serial = c.pic >> 2;
        if (m_frame.mbaff && m_field_mb) {
            // A field macroblock of a frame: the frame (or pair) of list 0
            // holding the picture, then its field of the parity asked for.
            for (std::size_t k = 0; k < l0.size(); ++k) {
                if (!l0[k].picture || l0[k].picture->serial != serial)
                    continue;
                int const frame_index = static_cast<int>(k) << 1;
                if (c.scale == VertMvScale::FrmToFld)
                    return frame_index;
                return frame_index + ((c.pic & 3) - 1 == m_parity ? 0 : 1);
            }
            return 0;
        }
        for (std::size_t k = 0; k < l0.size(); ++k) {
            if (!l0[k].picture)
                continue;
            bool match;
            if (c.scale == VertMvScale::OneToOne)
                match = l0[k].id() == c.pic;
            else if (c.scale == VertMvScale::FrmToFld)
                match = l0[k].picture->serial == serial && l0[k].structure == m_slice.structure;
            else
                match = l0[k].picture->serial == serial && l0[k].structure == 0;
            if (match)
                return static_cast<int>(k);
        }
        return 0;
    }

    // 8.4.1.2.2 (the macroblock-level part) and 8.4.1.2.3, for the 4x4
    // blocks of a rectangle.
    void direct(int x4, int y4, int w4, int h4)
    {
        if (m_slice.header->direct_spatial_mv_pred) {
            if (!m_spatial_ready) {
                auto min_positive = [](int p, int q) { return (p >= 0 && q >= 0) ? std::min(p, q) : std::max(p, q); };
                for (int list = 0; list < 2; ++list) {
                    // the macroblock's own neighbors, as for a 16x16 partition
                    std::uint32_t const saved = m_done;
                    m_done = 0;
                    Neighbor const a = at(list, -1, 0);
                    Neighbor const b = at(list, 0, -1);
                    Neighbor c = at(list, 16, -1);
                    if (!c.available)
                        c = at(list, -1, -1);
                    m_spatial_ref[list] = min_positive(a.ref, min_positive(b.ref, c.ref));
                    m_done = saved;
                }
                m_direct_zero = m_spatial_ref[0] < 0 && m_spatial_ref[1] < 0;
                if (m_direct_zero) {
                    m_spatial_ref[0] = m_spatial_ref[1] = 0;
                } else {
                    for (int list = 0; list < 2; ++list) {
                        if (m_spatial_ref[list] >= 0) {
                            std::uint32_t const saved = m_done;
                            m_done = 0;
                            m_spatial_mv[list] = predict(list, m_spatial_ref[list], 0, 0, 4, 0, 0);
                            m_done = saved;
                        }
                    }
                }
                m_spatial_ready = true;
            }
            bool const l1_short = !m_slice.list[1].empty() && !m_slice.list[1][0].long_term;
            for (int y = y4; y < y4 + h4; ++y)
                for (int x = x4; x < x4 + w4; ++x) {
                    Mv mv0 = m_direct_zero ? Mv {} : m_spatial_mv[0];
                    Mv mv1 = m_direct_zero ? Mv {} : m_spatial_mv[1];
                    if (!m_direct_zero) {
                        Col const c = colocated(x, y);
                        bool const col_zero = l1_short && c.ref == 0 && c.mv.x >= -1 && c.mv.x <= 1 && c.mv.y >= -1 && c.mv.y <= 1;
                        if (col_zero && m_spatial_ref[0] == 0)
                            mv0 = {};
                        if (col_zero && m_spatial_ref[1] == 0)
                            mv1 = {};
                    }
                    store(x, y, 1, 1, 0, m_spatial_ref[0], mv0);
                    store(x, y, 1, 1, 1, m_spatial_ref[1], mv1);
                }
            mark(x4, y4, w4, h4);
            return;
        }
        // Temporal.
        for (int y = y4; y < y4 + h4; ++y)
            for (int x = x4; x < x4 + w4; ++x) {
                Col c = colocated(x, y);
                int const ref0 = c.ref >= 0 ? map_col_to_list0(c) : 0;
                // (8-193), (8-194): the co-located vector in the current macroblock's units.
                if (c.scale == VertMvScale::FrmToFld)
                    c.mv.y = static_cast<std::int16_t>(c.mv.y / 2);
                else if (c.scale == VertMvScale::FldToFrm)
                    c.mv.y = static_cast<std::int16_t>(c.mv.y * 2);
                Mv mv0, mv1;
                std::vector<RefEntry> const& l0 = (*m_lists)[0];
                std::vector<RefEntry> const& l1 = (*m_lists)[1];
                if (static_cast<std::size_t>(ref0) < l0.size() && !l1.empty()) {
                    RefEntry const& p0 = l0[static_cast<std::size_t>(ref0)];
                    RefEntry const& p1 = l1[0];
                    int const tb = std::clamp(m_poc - p0.poc, -128, 127);
                    int const td = std::clamp(p1.poc - p0.poc, -128, 127);
                    if (p0.long_term || td == 0) {
                        mv0 = c.mv;
                        mv1 = {};
                    } else {
                        int const tx = (16384 + std::abs(td / 2)) / td;
                        int const dsf = std::clamp((tb * tx + 32) >> 6, -1024, 1023);
                        mv0.x = static_cast<std::int16_t>((dsf * c.mv.x + 128) >> 8);
                        mv0.y = static_cast<std::int16_t>((dsf * c.mv.y + 128) >> 8);
                        mv1.x = static_cast<std::int16_t>(mv0.x - c.mv.x);
                        mv1.y = static_cast<std::int16_t>(mv0.y - c.mv.y);
                    }
                }
                store(x, y, 1, 1, 0, ref0, mv0);
                store(x, y, 1, 1, 1, 0, mv1);
            }
        mark(x4, y4, w4, h4);
    }

private:
    Frame& m_frame;
    Slice const& m_slice;
    Picture& m_pic;
    int m_addr;
    int m_w;
    int m_x;
    int m_y;
    int m_bw; // motion blocks per row
    int m_base; // the macroblock's first motion block
    int m_row_step; // motion blocks from one row of macroblocks to the next (not MBAFF)
    bool m_field_mb;
    int m_parity = 0; // an MBAFF field macroblock's: 0 top, 1 bottom
    std::array<std::vector<RefEntry>, 2> const* m_lists; // the lists its reference indices index
    int m_poc; // PicOrderCnt of the current picture, or of the macroblock's field
    std::uint32_t m_done = 0;
    bool m_spatial_ready = false;
    bool m_direct_zero = false;
    int m_spatial_ref[2] = { -1, -1 };
    Mv m_spatial_mv[2];
};

bool uses_list(Pred p, int list) { return p != PredNone && p != PredDirect && (p & (1 << list)); }

} // namespace

void derive_motion(Frame& frame, Slice const& slice, int addr, MbSyntax const& mb)
{
    MotionContext ctx(frame, slice, addr);
    frame.picture->mb_intra[static_cast<std::size_t>(ctx.storage_mb())] = 0;
    auto add = [](Mv a, Mv d) { return Mv { static_cast<std::int16_t>(a.x + d.x), static_cast<std::int16_t>(a.y + d.y) }; };
    switch (mb.kind) {
    case MbKind::PSkip: {
        Neighbor const a = ctx.at(0, -1, 0);
        Neighbor const b = ctx.at(0, 0, -1);
        Mv mv;
        if (!(!a.available || !b.available || (a.ref == 0 && a.mv == Mv {}) || (b.ref == 0 && b.mv == Mv {})))
            mv = ctx.predict(0, 0, 0, 0, 4, 0, 0);
        ctx.store(0, 0, 4, 4, 0, 0, mv);
        ctx.store(0, 0, 4, 4, 1, -1, {});
        return;
    }
    case MbKind::BSkip:
    case MbKind::BDirect16x16:
        ctx.direct(0, 0, 4, 4);
        return;
    case MbKind::P16x16:
    case MbKind::B16x16:
    case MbKind::P16x8:
    case MbKind::B16x8:
    case MbKind::P8x16:
    case MbKind::B8x16: {
        int const shape = (mb.kind == MbKind::P16x8 || mb.kind == MbKind::B16x8) ? 1 : ((mb.kind == MbKind::P8x16 || mb.kind == MbKind::B8x16) ? 2 : 0);
        for (int p = 0; p < mb.partitions; ++p) {
            int const x4 = shape == 2 ? p * 2 : 0;
            int const y4 = shape == 1 ? p * 2 : 0;
            int const w4 = shape == 2 ? 2 : 4;
            int const h4 = shape == 1 ? 2 : 4;
            for (int list = 0; list < 2; ++list) {
                if (!uses_list(mb.part_pred[static_cast<std::size_t>(p)], list)) {
                    ctx.store(x4, y4, w4, h4, list, -1, {});
                    continue;
                }
                int const ref = mb.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(p)];
                Mv const mvp = ctx.predict(list, ref, x4, y4, w4, shape, p);
                ctx.store(x4, y4, w4, h4, list, ref, add(mvp, mb.mvd[static_cast<std::size_t>(list)][static_cast<std::size_t>(p)]));
            }
            ctx.mark(x4, y4, w4, h4);
        }
        return;
    }
    case MbKind::P8x8:
    case MbKind::B8x8:
        for (int i = 0; i < 4; ++i) {
            int const bx = (i & 1) * 2, by = (i >> 1) * 2;
            Pred const pred = mb.part_pred[static_cast<std::size_t>(i)];
            if (pred == PredDirect) {
                ctx.direct(bx, by, 2, 2);
                continue;
            }
            SubMbInfo const info = sub_mb_info(slice.type, mb.sub_type[static_cast<std::size_t>(i)]);
            int const w4 = info.width / 4, h4 = info.height / 4;
            for (int j = 0; j < info.parts; ++j) {
                int const x4 = bx + (info.width == 4 ? (j & 1) : 0);
                int const y4 = by + (info.height == 4 ? (info.width == 4 ? (j >> 1) : j) : 0);
                for (int list = 0; list < 2; ++list) {
                    if (!uses_list(pred, list)) {
                        ctx.store(x4, y4, w4, h4, list, -1, {});
                        continue;
                    }
                    int const ref = mb.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
                    Mv const mvp = ctx.predict(list, ref, x4, y4, w4, 0, 0);
                    ctx.store(x4, y4, w4, h4, list, ref, add(mvp, mb.mvd[static_cast<std::size_t>(list)][static_cast<std::size_t>(i * 4 + j)]));
                }
                ctx.mark(x4, y4, w4, h4);
            }
        }
        return;
    default:
        return;
    }
}

namespace {

// Inter prediction (8.4.2.2) a partition at a time: the blocks of a
// macroblock that share their motion are predicted as one rectangle, read
// straight from the reference plane where the filter's window lies inside
// it and from a copy with the edge samples repeated (8-228, 8-229) where it
// does not; each quarter position computes only the half-sample arrays
// Table 8-12 averages it from.

// The largest block: a partition of 16x16 luma samples.
constexpr int mc_stride = 16;

// A reference window with its edges repeated: rows y - top .. y + h + bottom
// and columns x - left .. x + w + right of `data`, at `pitch`.
template<typename P>
struct EdgeWindow {
    static constexpr int pitch = 32;
    P samples[pitch * (16 + 6)];
};

// `src` for a block at (x, y) of w x h whose filter reads `before` samples
// above and left of it and `after` below and right: the plane itself (a
// frame's, or a field's every other row: `pitch` apart), or `window`
// filled from it.
template<typename P>
P const* mc_source(P const* data, int pitch, int width, int height, int x, int y, int w, int h, int before, int after, EdgeWindow<P>& window, int& stride)
{
    if (x - before >= 0 && y - before >= 0 && x + w + after <= width && y + h + after <= height) {
        stride = pitch;
        return data + static_cast<std::ptrdiff_t>(y) * pitch + x;
    }
    int const rows = h + before + after, cols = w + before + after;
    for (int r = 0; r < rows; ++r) {
        P const* row = data + static_cast<std::ptrdiff_t>(std::clamp(y - before + r, 0, height - 1)) * pitch;
        P* out = window.samples + r * EdgeWindow<P>::pitch;
        for (int c = 0; c < cols; ++c)
            out[c] = row[std::clamp(x - before + c, 0, width - 1)];
    }
    stride = EdgeWindow<P>::pitch;
    return window.samples + before * EdgeWindow<P>::pitch + before;
}

inline int six_tap(int a, int b, int c, int d, int e, int f) { return a - 5 * b + 20 * c + 20 * d - 5 * e + f; }

// b (8-241): the horizontal half samples of the block's rows.
template<int W, typename P>
void half_h(P* dst, P const* src, int stride, int h, int max)
{
    for (int r = 0; r < h; ++r, src += stride, dst += mc_stride)
        for (int c = 0; c < W; ++c)
            dst[c] = static_cast<P>(clip_sample<P>((six_tap(src[c - 2], src[c - 1], src[c], src[c + 1], src[c + 2], src[c + 3]) + 16) >> 5, max));
}

// h (8-242): the vertical half samples.
template<int W, typename P>
void half_v(P* dst, P const* src, int stride, int h, int max)
{
    for (int r = 0; r < h; ++r, src += stride, dst += mc_stride)
        for (int c = 0; c < W; ++c)
            dst[c] = static_cast<P>(clip_sample<P>(
                (six_tap(src[c - 2 * stride], src[c - stride], src[c], src[c + stride], src[c + 2 * stride], src[c + 3 * stride]) + 16) >> 5, max));
}

// j (8-243, 8-244): the centre half samples, from the unclipped horizontal
// intermediates of rows -2 .. h + 2 (16 bits hold them at 8 bits a sample;
// 32 deeper).
template<int W, typename P>
void half_hv(P* dst, P const* src, int stride, int h, int max)
{
    using Mid = std::conditional_t<sizeof(P) == 1, std::int16_t, std::int32_t>;
    Mid mid[(16 + 5) * 16];
    P const* s = src - 2 * stride;
    for (int r = 0; r < h + 5; ++r, s += stride)
        for (int c = 0; c < W; ++c)
            mid[r * 16 + c] = static_cast<Mid>(six_tap(s[c - 2], s[c - 1], s[c], s[c + 1], s[c + 2], s[c + 3]));
    for (int r = 0; r < h; ++r, dst += mc_stride)
        for (int c = 0; c < W; ++c) {
            Mid const* m = mid + r * 16 + c;
            dst[c] = static_cast<P>(clip_sample<P>((six_tap(m[0], m[16], m[32], m[48], m[64], m[80]) + 512) >> 10, max));
        }
}

template<int W, typename P>
void average(P* dst, P const* a, int a_stride, P const* b, int b_stride, int h)
{
    for (int r = 0; r < h; ++r, dst += mc_stride, a += a_stride, b += b_stride)
        for (int c = 0; c < W; ++c)
            dst[c] = static_cast<P>((a[c] + b[c] + 1) >> 1);
}

// 8.4.2.2.1 for a w x h block at (x, y) with fraction (fx, fy), into `out`
// (rows of mc_stride).
template<int W, typename P>
void luma_partition(P const* data, int pitch, int width, int height, int x, int y, int fx, int fy, int h, P* out, int max)
{
    EdgeWindow<P> window;
    int stride = 0;
    if (fx == 0 && fy == 0) {
        P const* src = mc_source(data, pitch, width, height, x, y, W, h, 0, 0, window, stride);
        for (int r = 0; r < h; ++r)
            std::memcpy(out + r * mc_stride, src + r * stride, W * sizeof(P));
        return;
    }
    P const* g = mc_source(data, pitch, width, height, x, y, W, h, 2, 3, window, stride);
    P b[mc_stride * 17], hv[mc_stride * 16], j[mc_stride * 16];
    switch (fy * 4 + fx) {
    case 1: // a
        half_h<W>(b, g, stride, h, max);
        average<W>(out, g, stride, b, mc_stride, h);
        break;
    case 2: // b
        half_h<W>(out, g, stride, h, max);
        break;
    case 3: // c
        half_h<W>(b, g, stride, h, max);
        average<W>(out, g + 1, stride, b, mc_stride, h);
        break;
    case 4: // d
        half_v<W>(hv, g, stride, h, max);
        average<W>(out, g, stride, hv, mc_stride, h);
        break;
    case 8: // h
        half_v<W>(out, g, stride, h, max);
        break;
    case 12: // n
        half_v<W>(hv, g, stride, h, max);
        average<W>(out, g + stride, stride, hv, mc_stride, h);
        break;
    case 5: // e
    case 7: // g
    case 13: // p
    case 15: // r
        // b of the block's rows (one row down for p and r), h of its columns (one right for g and r)
        half_h<W>(b, g + (fy == 3 ? stride : 0), stride, h, max);
        half_v<W>(hv, g + (fx == 3 ? 1 : 0), stride, h, max);
        average<W>(out, b, mc_stride, hv, mc_stride, h);
        break;
    case 6: // f
    case 14: // q
        half_hv<W>(j, g, stride, h, max);
        half_h<W>(b, g + (fy == 3 ? stride : 0), stride, h, max);
        average<W>(out, j, mc_stride, b, mc_stride, h);
        break;
    case 9: // i
    case 11: // k
        half_hv<W>(j, g, stride, h, max);
        half_v<W>(hv, g + (fx == 3 ? 1 : 0), stride, h, max);
        average<W>(out, j, mc_stride, hv, mc_stride, h);
        break;
    default: // 10, j
        half_hv<W>(out, g, stride, h, max);
        break;
    }
}

template<typename P>
void luma_partition(P const* data, int pitch, int width, int height, int x, int y, int fx, int fy, int w, int h, P* out, int max)
{
    if (w == 16)
        luma_partition<16>(data, pitch, width, height, x, y, fx, fy, h, out, max);
    else if (w == 8)
        luma_partition<8>(data, pitch, width, height, x, y, fx, fy, h, out, max);
    else
        luma_partition<4>(data, pitch, width, height, x, y, fx, fy, h, out, max);
}

// 8.4.2.2.2 for a w x h chroma block at (x, y) with fraction (fx, fy)
// in eighths of a chroma sample.
template<typename P>
void chroma_partition(P const* data, int pitch, int width, int height, int x, int y, int fx, int fy, int w, int h, P* out)
{
    EdgeWindow<P> window;
    int stride = 0;
    P const* s = mc_source(data, pitch, width, height, x, y, w, h, 0, 1, window, stride);
    int const a = (8 - fx) * (8 - fy), b = fx * (8 - fy), c = (8 - fx) * fy, d = fx * fy;
    for (int r = 0; r < h; ++r, s += stride, out += mc_stride)
        for (int i = 0; i < w; ++i)
            out[i] = static_cast<P>((a * s[i] + b * s[i + 1] + c * s[i + stride] + d * s[i + stride + 1] + 32) >> 6);
}

struct Weights {
    int mode = 0; // 0 default, 1 explicit, 2 implicit
    int log_wd = 0;
    int w0 = 1, w1 = 1, o0 = 0, o1 = 0;
};

// 8.4.3. `mbaff_parity`: an MBAFF field macroblock's (0 top, 1 bottom),
// whose reference indices count fields; -1 for any other.
Weights weights_for(Slice const& slice, int ref0, int ref1, bool l0, bool l1, int component, int mbaff_parity)
{
    Weights w;
    w.mode = slice.weighted_mode;
    if (w.mode == 2) {
        if (!(l0 && l1)) {
            w.mode = 0;
            return w;
        }
        std::vector<std::array<int, 2>> const& table = mbaff_parity >= 0 ? slice.field_implicit[static_cast<std::size_t>(mbaff_parity)] : slice.implicit;
        std::size_t const at = static_cast<std::size_t>(ref0 * 64 + ref1);
        std::array<int, 2> const iw = mbaff_parity >= 0 ? (at < table.size() ? table[at] : std::array<int, 2> { 32, 32 })
                                                        : slice.implicit[static_cast<std::size_t>(ref0 * 32 + ref1)];
        w.log_wd = 5;
        w.w0 = iw[0];
        w.w1 = iw[1];
        return w;
    }
    if (w.mode == 1) {
        H264PredWeights const& pw = slice.header->weights;
        w.log_wd = component == 0 ? pw.luma_log2_denom : pw.chroma_log2_denom;
        if (mbaff_parity >= 0) { // (8-284), (8-285)
            ref0 >>= 1;
            ref1 >>= 1;
        }
        auto entry = [&](int list, int ref, int& wt, int& o) {
            H264PredWeights::Entry const& e = pw.list[static_cast<std::size_t>(list)][static_cast<std::size_t>(std::clamp(ref, 0, 31))];
            if (component == 0) {
                wt = e.luma_weight;
                o = e.luma_offset;
            } else {
                wt = e.chroma_weight[static_cast<std::size_t>(component - 1)];
                o = e.chroma_offset[static_cast<std::size_t>(component - 1)];
            }
        };
        if (l0)
            entry(0, ref0, w.w0, w.o0);
        if (l1)
            entry(1, ref1, w.w1, w.o1);
    }
    return w;
}

// 8.4.2.3 over a w x h block: the list predictions into the picture
// (8-270 to 8-273: default, then explicit or implicit with two lists, then
// one). The explicit offsets are in the samples' own scale already
// (8-294 to 8-296: times 2^(bitDepth - 8)).
template<typename P>
void weigh_block(P* dst, int stride, P const* p0, P const* p1, bool l0, bool l1, Weights const& w, int bw, int bh, int max)
{
    for (int r = 0; r < bh; ++r, dst += stride, p0 += mc_stride, p1 += mc_stride) {
        if (l0 && l1 && w.mode == 0) {
            for (int c = 0; c < bw; ++c)
                dst[c] = static_cast<P>((p0[c] + p1[c] + 1) >> 1);
        } else if (!(l0 && l1) && w.mode != 1) {
            std::memcpy(dst, l0 ? p0 : p1, static_cast<std::size_t>(bw) * sizeof(P));
        } else if (l0 && l1) {
            int const round = 1 << w.log_wd, shift = w.log_wd + 1, o = (w.o0 + w.o1 + 1) >> 1;
            for (int c = 0; c < bw; ++c)
                dst[c] = static_cast<P>(clip_sample<P>(((p0[c] * w.w0 + p1[c] * w.w1 + round) >> shift) + o, max));
        } else {
            P const* p = l0 ? p0 : p1;
            int const wt = l0 ? w.w0 : w.w1, o = l0 ? w.o0 : w.o1;
            if (w.log_wd >= 1) {
                int const round = 1 << (w.log_wd - 1);
                for (int c = 0; c < bw; ++c)
                    dst[c] = static_cast<P>(clip_sample<P>(((p[c] * wt + round) >> w.log_wd) + o, max));
            } else {
                for (int c = 0; c < bw; ++c)
                    dst[c] = static_cast<P>(clip_sample<P>(p[c] * wt + o, max));
            }
        }
    }
}

// The macroblock being predicted: where its motion and its samples are.
template<typename P>
struct InterTarget {
    InterTarget(Frame const& f, Slice const& s)
        : frame(f)
        , pic(*f.picture)
        , slice(s)
        , lists(s.list.data())
    {
    }
    Frame const& frame;
    Picture& pic;
    Slice const& slice;
    std::vector<RefEntry> const* lists; // [2]: the slice's, or an MBAFF field macroblock's fields
    int base = 0; // its first motion block
    int bw = 0; // motion blocks per row
    Frame::Place place;
    int mbaff_parity = -1; // an MBAFF field macroblock's parity
    P* luma = nullptr; // the rows it is made of
    P* chroma[2] = { nullptr, nullptr };
    int luma_stride = 0;
    int chroma_stride = 0;
};

// Whether the 4x4 blocks of a w4 x h4 rectangle at (x4, y4) of the
// macroblock share their motion.
template<typename P>
bool same_motion(InterTarget<P> const& t, int x4, int y4, int w4, int h4)
{
    int const first = t.base + y4 * t.bw + x4;
    for (int y = y4; y < y4 + h4; ++y)
        for (int x = x4; x < x4 + w4; ++x) {
            int const i = t.base + y * t.bw + x;
            for (std::size_t list = 0; list < 2; ++list)
                if (t.pic.ref_idx[list][static_cast<std::size_t>(i)] != t.pic.ref_idx[list][static_cast<std::size_t>(first)]
                    || !(t.pic.mv[list][static_cast<std::size_t>(i)] == t.pic.mv[list][static_cast<std::size_t>(first)]))
                    return false;
        }
    return true;
}

// One rectangle of the macroblock with one motion: (x4, y4) and its size in 4x4 blocks.
template<typename P>
void predict_rectangle(InterTarget<P> const& t, int x4, int y4, int w4, int h4)
{
    Picture const& pic = t.pic;
    int const i = t.base + y4 * t.bw + x4;
    int refs[2];
    Mv mvs[2];
    RefEntry const* ref_pics[2] = { nullptr, nullptr };
    bool use[2];
    for (int list = 0; list < 2; ++list) {
        std::vector<RefEntry> const& l = t.lists[list];
        refs[list] = pic.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
        mvs[list] = pic.mv[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
        use[list] = refs[list] >= 0 && static_cast<std::size_t>(refs[list]) < l.size() && l[static_cast<std::size_t>(refs[list])].picture;
        if (use[list])
            ref_pics[list] = &l[static_cast<std::size_t>(refs[list])];
    }
    if (!use[0] && !use[1]) {
        // A reference the buffer does not hold: the block stays as it was (grey on a fresh picture).
        return;
    }
    int const w = w4 * 4, h = h4 * 4;
    int const x0 = t.place.x + x4 * 4, y0 = t.place.y + y4 * 4;
    Frame const& f = t.frame;
    // The explicit weights' offsets in the samples' scale (8-294 to 8-296).
    auto weights = [&](int component) {
        Weights wt = weights_for(t.slice, refs[0], refs[1], use[0], use[1], component, t.mbaff_parity);
        if constexpr (sizeof(P) == 2) {
            int const shift = (component == 0 ? f.bit_depth_luma : f.bit_depth_chroma) - 8;
            wt.o0 *= 1 << shift;
            wt.o1 *= 1 << shift;
        }
        return wt;
    };
    alignas(16) P pl[2][mc_stride * 16];
    for (int list = 0; list < 2; ++list)
        if (use[list])
            luma_partition(ref_pics[list]->template luma<P>(), ref_pics[list]->luma_stride(), pic.width, ref_pics[list]->luma_height(), x0 + (mvs[list].x >> 2),
                y0 + (mvs[list].y >> 2), mvs[list].x & 3, mvs[list].y & 3, w, h, pl[list], f.luma_max);
    weigh_block(t.luma + static_cast<std::ptrdiff_t>(y0) * t.luma_stride + x0, t.luma_stride, pl[0], pl[1], use[0], use[1], weights(0), w, h, f.luma_max);
    if (f.chroma_format == 0)
        return;
    // 8.4.2.2.2: 4:2:0 and 4:2:2 chroma in eighths of a sample across,
    // eighths (4:2:0) or quarters (4:2:2) down; 4:4:4's as luma's (8-235, 8-236).
    int const cx0 = x0 >> f.chroma_shift_x, cy0 = y0 >> f.chroma_shift_y;
    int const cw = w >> f.chroma_shift_x, ch = h >> f.chroma_shift_y;
    for (int c = 0; c < 2; ++c) {
        for (int list = 0; list < 2; ++list) {
            if (!use[list])
                continue;
            RefEntry const& r = *ref_pics[list];
            if (f.chroma_format == 3) {
                luma_partition(r.template chroma<P>(c), r.chroma_stride(), pic.chroma_width, r.chroma_height(), x0 + (mvs[list].x >> 2), y0 + (mvs[list].y >> 2),
                    mvs[list].x & 3, mvs[list].y & 3, w, h, pl[list], f.chroma_max);
                continue;
            }
            int mvc_y = mvs[list].y;
            int y_int, y_frac;
            if (f.chroma_format == 1) {
                // Table 8-10: between fields of opposite parity, the
                // chroma vector moves a quarter of a chroma row.
                if (t.place.structure == 2 && r.structure == 1)
                    mvc_y += 2;
                else if (t.place.structure == 1 && r.structure == 2)
                    mvc_y -= 2;
                y_int = cy0 + (mvc_y >> 3);
                y_frac = mvc_y & 7;
            } else {
                y_int = cy0 + (mvc_y >> 2);
                y_frac = (mvc_y & 3) << 1;
            }
            chroma_partition(r.template chroma<P>(c), r.chroma_stride(), pic.chroma_width, r.chroma_height(), cx0 + (mvs[list].x >> 3), y_int, mvs[list].x & 7,
                y_frac, cw, ch, pl[list]);
        }
        weigh_block(t.chroma[c] + static_cast<std::ptrdiff_t>(cy0) * t.chroma_stride + cx0, t.chroma_stride, pl[0], pl[1], use[0], use[1], weights(1 + c), cw, ch,
            f.chroma_max);
    }
}

// An 8x8 quadrant: whole, two halves either way, or four 4x4 blocks.
template<typename P>
void predict_quadrant(InterTarget<P> const& t, int x4, int y4)
{
    if (same_motion(t, x4, y4, 2, 2)) {
        predict_rectangle(t, x4, y4, 2, 2);
    } else if (same_motion(t, x4, y4, 2, 1) && same_motion(t, x4, y4 + 1, 2, 1)) {
        predict_rectangle(t, x4, y4, 2, 1);
        predict_rectangle(t, x4, y4 + 1, 2, 1);
    } else if (same_motion(t, x4, y4, 1, 2) && same_motion(t, x4 + 1, y4, 1, 2)) {
        predict_rectangle(t, x4, y4, 1, 2);
        predict_rectangle(t, x4 + 1, y4, 1, 2);
    } else {
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                predict_rectangle(t, x4 + x, y4 + y, 1, 1);
    }
}

template<typename P>
void inter_predict(Frame& frame, Slice const& slice, int addr)
{
    InterTarget<P> t(frame, slice);
    if (frame.mbaff) {
        t.base = frame.block_base(addr);
        t.place = frame.place(addr);
    } else {
        int const mx = addr % frame.mb_width, my = addr / frame.mb_width;
        t.base = frame.block_base_at(mx, my);
        t.place = frame.place_at(mx, my);
    }
    t.bw = frame.block_stride();
    if (frame.mbaff && t.place.structure != 0) {
        t.mbaff_parity = t.place.structure - 1;
        t.lists = slice.field_list[static_cast<std::size_t>(t.mbaff_parity)].data();
    }
    t.luma = frame.luma_rows<P>(t.place.structure);
    t.luma_stride = frame.luma_rows_stride(t.place.structure);
    for (int c = 0; c < 2; ++c)
        t.chroma[c] = frame.chroma_rows<P>(c, t.place.structure);
    t.chroma_stride = frame.chroma_rows_stride(t.place.structure);
    if (same_motion(t, 0, 0, 4, 4)) {
        predict_rectangle(t, 0, 0, 4, 4);
    } else if (same_motion(t, 0, 0, 4, 2) && same_motion(t, 0, 2, 4, 2)) {
        predict_rectangle(t, 0, 0, 4, 2);
        predict_rectangle(t, 0, 2, 4, 2);
    } else if (same_motion(t, 0, 0, 2, 4) && same_motion(t, 2, 0, 2, 4)) {
        predict_rectangle(t, 0, 0, 2, 4);
        predict_rectangle(t, 2, 0, 2, 4);
    } else {
        for (int q = 0; q < 4; ++q)
            predict_quadrant(t, (q & 1) * 2, (q >> 1) * 2);
    }
}

// Intra prediction's neighboring samples (8.3.1.2, 8.3.2.2, 8.3.3,
// 8.3.4): available when their macroblock is (in the slice, and not
// inter under constrained_intra_pred) and, inside the current macroblock,
// already constructed.
//
// `plane` is the component predicted as luma is: 0 for Y, and 1 (Cb) or 2
// (Cr) in 4:4:4, whose chroma takes the luma's tools (8.3.4.5).
template<typename P>
class IntraContext {
public:
    IntraContext(Frame& frame, Slice const& slice, int addr, int plane = 0)
        : m_frame(frame)
        , m_slice(slice)
        , m_x(addr % frame.mb_width)
        , m_y(addr / frame.mb_width)
        , m_plane(plane)
        , m_chroma_rows(frame.mb_chroma_height)
    {
        Frame::Place const p = frame.mbaff ? frame.place(addr) : frame.place_at(m_x, m_y);
        m_stride = frame.luma_rows_stride(p.structure);
        m_cstride = frame.chroma_rows_stride(p.structure);
        P* const rows = plane == 0 ? frame.luma_rows<P>(p.structure) : frame.chroma_rows<P>(plane - 1, p.structure);
        m_luma = rows + static_cast<std::ptrdiff_t>(p.y) * m_stride + p.x;
        if (frame.chroma_format != 0) {
            for (int c = 0; c < 2; ++c)
                m_chroma[c] = frame.chroma_rows<P>(c, p.structure) + static_cast<std::ptrdiff_t>(p.y >> frame.chroma_shift_y) * m_cstride
                    + (p.x >> frame.chroma_shift_x);
        }
        if (frame.mbaff && frame.mbs[static_cast<std::size_t>(addr)].intra)
            gather_mbaff_neighbors(addr);
    }
    int chroma_rows() const { return m_chroma_rows; }
    bool mb_ok(int mx, int my) const
    {
        if (mx < 0 || my < 0 || mx >= m_frame.mb_width || my >= m_frame.mb_height)
            return false;
        MbState const& s = m_frame.mbs[static_cast<std::size_t>(my * m_frame.mb_width + mx)];
        if (s.slice != m_slice.number)
            return false;
        if (!s.intra && m_slice.pps->constrained_intra_pred)
            return false;
        return true;
    }
    // A luma sample at (x, y) relative to the macroblock.
    bool luma_available(int x, int y) const
    {
        if (y >= 16 || (x >= 16 && y >= 0))
            return false;
        if (x >= 0 && y >= 0)
            return (m_done >> ((y >> 2) * 4 + (x >> 2))) & 1;
        if (m_mbaff)
            return x < 0 ? (y < 0 ? m_corner_ok : m_left_ok[y]) : m_top_ok[x];
        int const mx = m_x + (x < 0 ? -1 : (x >= 16 ? 1 : 0));
        int const my = m_y + (y < 0 ? -1 : 0);
        return mb_ok(mx, my);
    }
    // The n samples left of a block at (xo, yo), all of them: in an MBAFF
    // frame they may come from both macroblocks of the pair beside.
    bool left_available(int xo, int yo, int n) const
    {
        if (!m_mbaff || xo > 0)
            return luma_available(xo - 1, yo);
        for (int k = 0; k < n; ++k) {
            if (!m_left_ok[yo + k])
                return false;
        }
        return true;
    }
    int luma(int x, int y) const
    {
        if (m_mbaff && (x < 0 || y < 0))
            return x < 0 ? (y < 0 ? m_corner : m_left[y]) : m_top[x];
        return m_luma[static_cast<std::ptrdiff_t>(y) * m_stride + x];
    }
    bool chroma_left() const { return m_mbaff ? chroma_left_rows(0, m_chroma_rows) : mb_ok(m_x - 1, m_y); }
    bool chroma_left_rows(int yo, int n) const
    {
        if (!m_mbaff)
            return mb_ok(m_x - 1, m_y);
        for (int k = 0; k < n; ++k) {
            if (!m_chroma_left_ok[yo + k])
                return false;
        }
        return true;
    }
    bool chroma_top() const { return m_mbaff ? m_chroma_top_ok : mb_ok(m_x, m_y - 1); }
    bool chroma_corner() const { return m_mbaff ? m_chroma_corner_ok : mb_ok(m_x - 1, m_y - 1); }
    int chroma(int c, int x, int y) const
    {
        if (m_mbaff && (x < 0 || y < 0))
            return x < 0 ? (y < 0 ? m_chroma_corner[c] : m_chroma_left[c][y]) : m_chroma_top[c][x];
        return m_chroma[c][static_cast<std::ptrdiff_t>(y) * m_cstride + x];
    }
    void done(int x4, int y4, int w4, int h4)
    {
        for (int y = y4; y < y4 + h4; ++y)
            for (int x = x4; x < x4 + w4; ++x)
                m_done |= 1u << (y * 4 + x);
    }
    // The macroblock's own samples, and the distance between its rows.
    P* luma_origin() const { return m_luma; }
    P* chroma_origin(int c) const { return m_chroma[c]; }
    int luma_stride() const { return m_stride; }
    int chroma_stride() const { return m_cstride; }

private:
    // 8.3.1.2 and its kin in an MBAFF frame: each neighboring sample where
    // 6.4.12.2 puts it — the left column may alternate between the two
    // macroblocks of the pair beside, the row above may be a field's —
    // each available as its own macroblock is.
    void gather_mbaff_neighbors(int addr)
    {
        m_mbaff = true;
        auto usable = [&](Frame::Location const& n) {
            if (n.addr < 0)
                return false;
            return m_frame.mbs[static_cast<std::size_t>(n.addr)].intra || !m_slice.pps->constrained_intra_pred;
        };
        int const sx = m_frame.chroma_shift_x, sy = m_frame.chroma_shift_y;
        auto luma_at = [&](Frame::Location const& n) {
            Frame::Place const p = m_frame.place(n.addr);
            P const* const rows = m_plane == 0 ? m_frame.luma_rows<P>(p.structure) : m_frame.chroma_rows<P>(m_plane - 1, p.structure);
            return static_cast<int>(rows[static_cast<std::ptrdiff_t>(p.y + n.y) * m_frame.luma_rows_stride(p.structure) + p.x + n.x]);
        };
        auto chroma_at = [&](int c, Frame::Location const& n) {
            Frame::Place const p = m_frame.place(n.addr);
            return static_cast<int>(m_frame.chroma_rows<P>(c, p.structure)[static_cast<std::ptrdiff_t>((p.y >> sy) + n.y) * m_frame.chroma_rows_stride(p.structure)
                + (p.x >> sx) + n.x]);
        };
        auto luma_location = [&](int x, int y) { return m_frame.neighbor(addr, x, y, 16, 16, m_slice.number); };
        int const cw = m_frame.mb_chroma_width, chh = m_frame.mb_chroma_height;
        auto chroma_location = [&](int x, int y) { return m_frame.neighbor(addr, x, y, cw, chh, m_slice.number); };
        for (int y = 0; y < 16; ++y) {
            Frame::Location const n = luma_location(-1, y);
            m_left_ok[y] = usable(n);
            m_left[y] = m_left_ok[y] ? luma_at(n) : 0;
        }
        for (int x = 0; x < 24; ++x) {
            Frame::Location const n = luma_location(x, -1);
            m_top_ok[x] = usable(n);
            m_top[x] = m_top_ok[x] ? luma_at(n) : 0;
        }
        Frame::Location const corner = luma_location(-1, -1);
        m_corner_ok = usable(corner);
        m_corner = m_corner_ok ? luma_at(corner) : 0;
        // The chroma's own neighbors where it has its own prediction (8.3.4).
        if (m_plane != 0 || m_frame.chroma_format == 0 || m_frame.chroma_format == 3)
            return;
        Frame::Location const chroma_corner = chroma_location(-1, -1);
        m_chroma_corner_ok = usable(chroma_corner);
        m_chroma_top_ok = true;
        for (int k = 0; k < cw; ++k) {
            Frame::Location const above = chroma_location(k, -1);
            m_chroma_top_ok = m_chroma_top_ok && usable(above);
            for (int c = 0; c < 2; ++c)
                m_chroma_top[c][k] = usable(above) ? chroma_at(c, above) : 0;
        }
        for (int k = 0; k < chh; ++k) {
            Frame::Location const left = chroma_location(-1, k);
            m_chroma_left_ok[k] = usable(left);
            for (int c = 0; c < 2; ++c)
                m_chroma_left[c][k] = m_chroma_left_ok[k] ? chroma_at(c, left) : 0;
        }
        for (int c = 0; c < 2; ++c)
            m_chroma_corner[c] = m_chroma_corner_ok ? chroma_at(c, chroma_corner) : 0;
    }

    Frame& m_frame;
    Slice const& m_slice;
    int m_x;
    int m_y;
    int m_plane;
    int m_chroma_rows; // MbHeightC
    P* m_luma = nullptr;
    P* m_chroma[2] = { nullptr, nullptr };
    int m_stride = 0;
    int m_cstride = 0;
    std::uint32_t m_done = 0;
    // MBAFF: the neighboring samples, gathered (gather_mbaff_neighbors).
    bool m_mbaff = false;
    std::array<int, 16> m_left {};
    std::array<int, 24> m_top {};
    int m_corner = 0;
    std::array<bool, 16> m_left_ok {};
    std::array<bool, 24> m_top_ok {};
    bool m_corner_ok = false;
    std::array<std::array<int, 16>, 2> m_chroma_left {};
    std::array<std::array<int, 8>, 2> m_chroma_top {};
    std::array<int, 2> m_chroma_corner {};
    std::array<bool, 16> m_chroma_left_ok {};
    bool m_chroma_top_ok = false;
    bool m_chroma_corner_ok = false;
};

// 8.3.1.2: a 4x4 block's prediction into `out` (raster 4x4). `mid`:
// 1 << (BitDepth - 1), DC's value with no neighbors.
template<typename IC>
void predict_4x4(IC const& ic, int xo, int yo, int mode, int* out, int mid)
{
    int top[8] = {}, left[4] = {}, corner = 0;
    bool const has_top = ic.luma_available(xo, yo - 1);
    bool const has_left = ic.left_available(xo, yo, 4);
    bool const has_corner = ic.luma_available(xo - 1, yo - 1);
    bool const has_tr = ic.luma_available(xo + 4, yo - 1);
    if (has_top)
        for (int x = 0; x < 4; ++x)
            top[x] = ic.luma(xo + x, yo - 1);
    if (has_top) {
        for (int x = 4; x < 8; ++x)
            top[x] = has_tr ? ic.luma(xo + x, yo - 1) : top[3];
    }
    if (has_left)
        for (int y = 0; y < 4; ++y)
            left[y] = ic.luma(xo - 1, yo + y);
    if (has_corner)
        corner = ic.luma(xo - 1, yo - 1);
    auto P = [&](int x, int y) -> int {
        // p[x, y] in the spec's indexing: y = -1 is the top row, x = -1 the left column
        if (y < 0)
            return x < 0 ? corner : top[std::min(x, 7)];
        return left[std::min(y, 3)];
    };
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int v = mid;
            switch (mode) {
            case 0:
                v = top[x];
                break;
            case 1:
                v = left[y];
                break;
            case 2:
                if (has_top && has_left)
                    v = (top[0] + top[1] + top[2] + top[3] + left[0] + left[1] + left[2] + left[3] + 4) >> 3;
                else if (has_left)
                    v = (left[0] + left[1] + left[2] + left[3] + 2) >> 2;
                else if (has_top)
                    v = (top[0] + top[1] + top[2] + top[3] + 2) >> 2;
                else
                    v = mid;
                break;
            case 3: // diagonal down left
                if (x == 3 && y == 3)
                    v = (top[6] + 3 * top[7] + 2) >> 2;
                else
                    v = (top[x + y] + 2 * top[x + y + 1] + top[x + y + 2] + 2) >> 2;
                break;
            case 4: // diagonal down right
                if (x > y)
                    v = (P(x - y - 2, -1) + 2 * P(x - y - 1, -1) + P(x - y, -1) + 2) >> 2;
                else if (x < y)
                    v = (P(-1, y - x - 2) + 2 * P(-1, y - x - 1) + P(-1, y - x) + 2) >> 2;
                else
                    v = (P(0, -1) + 2 * P(-1, -1) + P(-1, 0) + 2) >> 2;
                break;
            case 5: { // vertical right
                int const z = 2 * x - y;
                if (z >= 0 && (z & 1) == 0)
                    v = (P(x - (y >> 1) - 1, -1) + P(x - (y >> 1), -1) + 1) >> 1;
                else if (z >= 0)
                    v = (P(x - (y >> 1) - 2, -1) + 2 * P(x - (y >> 1) - 1, -1) + P(x - (y >> 1), -1) + 2) >> 2;
                else if (z == -1)
                    v = (P(-1, 0) + 2 * P(-1, -1) + P(0, -1) + 2) >> 2;
                else
                    v = (P(-1, y - 1) + 2 * P(-1, y - 2) + P(-1, y - 3) + 2) >> 2;
                break;
            }
            case 6: { // horizontal down
                int const z = 2 * y - x;
                if (z >= 0 && (z & 1) == 0)
                    v = (P(-1, y - (x >> 1) - 1) + P(-1, y - (x >> 1)) + 1) >> 1;
                else if (z >= 0)
                    v = (P(-1, y - (x >> 1) - 2) + 2 * P(-1, y - (x >> 1) - 1) + P(-1, y - (x >> 1)) + 2) >> 2;
                else if (z == -1)
                    v = (P(-1, 0) + 2 * P(-1, -1) + P(0, -1) + 2) >> 2;
                else
                    v = (P(x - 1, -1) + 2 * P(x - 2, -1) + P(x - 3, -1) + 2) >> 2;
                break;
            }
            case 7: // vertical left
                if ((y & 1) == 0)
                    v = (top[x + (y >> 1)] + top[x + (y >> 1) + 1] + 1) >> 1;
                else
                    v = (top[x + (y >> 1)] + 2 * top[x + (y >> 1) + 1] + top[x + (y >> 1) + 2] + 2) >> 2;
                break;
            default: { // horizontal up
                int const z = x + 2 * y;
                if (z < 5 && (z & 1) == 0)
                    v = (left[y + (x >> 1)] + left[y + (x >> 1) + 1] + 1) >> 1;
                else if (z < 5)
                    v = (left[y + (x >> 1)] + 2 * left[y + (x >> 1) + 1] + left[y + (x >> 1) + 2] + 2) >> 2;
                else if (z == 5)
                    v = (left[2] + 3 * left[3] + 2) >> 2;
                else
                    v = left[3];
                break;
            }
            }
            out[y * 4 + x] = v;
        }
}

// 8.3.2.2: an 8x8 block's prediction, its references filtered first.
template<typename IC>
void predict_8x8(IC const& ic, int xo, int yo, int mode, int* out, int mid)
{
    bool const has_top = ic.luma_available(xo, yo - 1);
    bool const has_left = ic.left_available(xo, yo, 8);
    bool const has_corner = ic.luma_available(xo - 1, yo - 1);
    bool const has_tr = ic.luma_available(xo + 8, yo - 1);
    int p_top[16] = {}, p_left[8] = {}, p_corner = 0;
    if (has_top) {
        for (int x = 0; x < 8; ++x)
            p_top[x] = ic.luma(xo + x, yo - 1);
        for (int x = 8; x < 16; ++x)
            p_top[x] = has_tr ? ic.luma(xo + x, yo - 1) : p_top[7];
    }
    if (has_left)
        for (int y = 0; y < 8; ++y)
            p_left[y] = ic.luma(xo - 1, yo + y);
    if (has_corner)
        p_corner = ic.luma(xo - 1, yo - 1);
    // 8.3.2.2.1.
    int t[16] = {}, l[8] = {}, c = 0;
    if (has_top) {
        t[0] = has_corner ? (p_corner + 2 * p_top[0] + p_top[1] + 2) >> 2 : (3 * p_top[0] + p_top[1] + 2) >> 2;
        for (int x = 1; x < 15; ++x)
            t[x] = (p_top[x - 1] + 2 * p_top[x] + p_top[x + 1] + 2) >> 2;
        t[15] = (p_top[14] + 3 * p_top[15] + 2) >> 2;
    }
    if (has_corner) {
        if (!has_top || !has_left) {
            if (has_top)
                c = (3 * p_corner + p_top[0] + 2) >> 2;
            else if (has_left)
                c = (3 * p_corner + p_left[0] + 2) >> 2;
            else
                c = p_corner;
        } else {
            c = (p_top[0] + 2 * p_corner + p_left[0] + 2) >> 2;
        }
    }
    if (has_left) {
        l[0] = has_corner ? (p_corner + 2 * p_left[0] + p_left[1] + 2) >> 2 : (3 * p_left[0] + p_left[1] + 2) >> 2;
        for (int y = 1; y < 7; ++y)
            l[y] = (p_left[y - 1] + 2 * p_left[y] + p_left[y + 1] + 2) >> 2;
        l[7] = (p_left[6] + 3 * p_left[7] + 2) >> 2;
    }
    auto P = [&](int x, int y) -> int {
        if (y < 0)
            return x < 0 ? c : t[std::min(x, 15)];
        return l[std::min(y, 7)];
    };
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            int v = mid;
            switch (mode) {
            case 0:
                v = t[x];
                break;
            case 1:
                v = l[y];
                break;
            case 2:
                if (has_top && has_left) {
                    int s = 8;
                    for (int k = 0; k < 8; ++k)
                        s += t[k] + l[k];
                    v = s >> 4;
                } else if (has_left) {
                    int s = 4;
                    for (int k = 0; k < 8; ++k)
                        s += l[k];
                    v = s >> 3;
                } else if (has_top) {
                    int s = 4;
                    for (int k = 0; k < 8; ++k)
                        s += t[k];
                    v = s >> 3;
                }
                break;
            case 3:
                if (x == 7 && y == 7)
                    v = (t[14] + 3 * t[15] + 2) >> 2;
                else
                    v = (t[x + y] + 2 * t[x + y + 1] + t[x + y + 2] + 2) >> 2;
                break;
            case 4:
                if (x > y)
                    v = (P(x - y - 2, -1) + 2 * P(x - y - 1, -1) + P(x - y, -1) + 2) >> 2;
                else if (x < y)
                    v = (P(-1, y - x - 2) + 2 * P(-1, y - x - 1) + P(-1, y - x) + 2) >> 2;
                else
                    v = (P(0, -1) + 2 * P(-1, -1) + P(-1, 0) + 2) >> 2;
                break;
            case 5: {
                int const z = 2 * x - y;
                if (z >= 0 && (z & 1) == 0)
                    v = (P(x - (y >> 1) - 1, -1) + P(x - (y >> 1), -1) + 1) >> 1;
                else if (z >= 0)
                    v = (P(x - (y >> 1) - 2, -1) + 2 * P(x - (y >> 1) - 1, -1) + P(x - (y >> 1), -1) + 2) >> 2;
                else if (z == -1)
                    v = (P(-1, 0) + 2 * P(-1, -1) + P(0, -1) + 2) >> 2;
                else
                    v = (P(-1, y - 2 * x - 1) + 2 * P(-1, y - 2 * x - 2) + P(-1, y - 2 * x - 3) + 2) >> 2;
                break;
            }
            case 6: {
                int const z = 2 * y - x;
                if (z >= 0 && (z & 1) == 0)
                    v = (P(-1, y - (x >> 1) - 1) + P(-1, y - (x >> 1)) + 1) >> 1;
                else if (z >= 0)
                    v = (P(-1, y - (x >> 1) - 2) + 2 * P(-1, y - (x >> 1) - 1) + P(-1, y - (x >> 1)) + 2) >> 2;
                else if (z == -1)
                    v = (P(-1, 0) + 2 * P(-1, -1) + P(0, -1) + 2) >> 2;
                else
                    v = (P(x - 2 * y - 1, -1) + 2 * P(x - 2 * y - 2, -1) + P(x - 2 * y - 3, -1) + 2) >> 2;
                break;
            }
            case 7:
                if ((y & 1) == 0)
                    v = (t[x + (y >> 1)] + t[x + (y >> 1) + 1] + 1) >> 1;
                else
                    v = (t[x + (y >> 1)] + 2 * t[x + (y >> 1) + 1] + t[x + (y >> 1) + 2] + 2) >> 2;
                break;
            default: {
                int const z = x + 2 * y;
                if (z < 13 && (z & 1) == 0)
                    v = (l[y + (x >> 1)] + l[y + (x >> 1) + 1] + 1) >> 1;
                else if (z < 13)
                    v = (l[y + (x >> 1)] + 2 * l[y + (x >> 1) + 1] + l[y + (x >> 1) + 2] + 2) >> 2;
                else if (z == 13)
                    v = (l[6] + 3 * l[7] + 2) >> 2;
                else
                    v = l[7];
                break;
            }
            }
            out[y * 8 + x] = v;
        }
}

// 8.3.3.
template<typename P>
void predict_16x16(IntraContext<P> const& ic, int mode, int* out, int mid, int max)
{
    bool const has_top = ic.luma_available(0, -1);
    bool const has_left = ic.left_available(0, 0, 16);
    // A mode whose samples are not there is no conforming stream's; such
    // a stream gets DC from what there is (as ffmpeg conceals it).
    if ((mode == 0 && !has_top) || (mode == 1 && !has_left) || (mode == 3 && !(has_top && has_left && ic.luma_available(-1, -1))))
        mode = 2;
    // The neighbors read once; each mode's terms computed once.
    int top[16] = {}, left[16] = {};
    for (int k = 0; k < 16; ++k) {
        if (has_top)
            top[k] = ic.luma(k, -1);
        if (has_left)
            left[k] = ic.luma(-1, k);
    }
    if (mode == 0 || mode == 1) {
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                out[y * 16 + x] = mode == 0 ? top[x] : left[y];
        return;
    }
    if (mode == 2) {
        int st = 0, sl = 0;
        for (int k = 0; k < 16; ++k) {
            st += top[k];
            sl += left[k];
        }
        int v = mid;
        if (has_top && has_left)
            v = (st + sl + 16) >> 5;
        else if (has_left)
            v = (sl + 8) >> 4;
        else if (has_top)
            v = (st + 8) >> 4;
        std::fill_n(out, 256, v);
        return;
    }
    int const corner = ic.luma(-1, -1);
    auto top_at = [&](int x) { return x < 0 ? corner : top[x]; };
    auto left_at = [&](int y) { return y < 0 ? corner : left[y]; };
    int h = 0, vv = 0;
    for (int k = 0; k < 8; ++k) {
        h += (k + 1) * (top_at(8 + k) - top_at(6 - k));
        vv += (k + 1) * (left_at(8 + k) - left_at(6 - k));
    }
    int const a = 16 * (left[15] + top[15]);
    int const b = (5 * h + 32) >> 6;
    int const c = (5 * vv + 32) >> 6;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            out[y * 16 + x] = clip_sample<P>((a + b * (x - 7) + c * (y - 7) + 16) >> 5, max);
}

// 8.3.4 for 4:2:0 and 4:2:2 (8 samples wide, 8 or 16 high: `Rows`).
template<int Rows, typename P>
void predict_chroma(IntraContext<P> const& ic, int comp, int mode, int* out, int mid, int max)
{
    constexpr int rows = Rows;
    bool const has_top = ic.chroma_top();
    bool const left_all = ic.chroma_left();
    if ((mode == 1 && !left_all) || (mode == 2 && !has_top) || (mode == 3 && !(has_top && left_all && ic.chroma_corner())))
        mode = 0;
    // The neighbors read once; each mode's terms computed once.
    int top[8] = {}, left[16] = {};
    for (int k = 0; k < 8; ++k) {
        if (has_top)
            top[k] = ic.chroma(comp, k, -1);
    }
    for (int k = 0; k < rows; ++k) {
        if (ic.chroma_left_rows(k & ~3, 4))
            left[k] = ic.chroma(comp, -1, k);
    }
    if (mode == 0) {
        // 8.3.4.1 to 8.3.4.3: a DC per 4x4 block, from its own top and
        // left samples (each side all there or not).
        for (int yo = 0; yo < rows; yo += 4)
            for (int xo = 0; xo < 8; xo += 4) {
                bool const has_left = ic.chroma_left_rows(yo, 4);
                int st = 0, sl = 0;
                for (int k = 0; k < 4; ++k) {
                    st += top[xo + k];
                    sl += left[yo + k];
                }
                int v = mid;
                if ((xo == 0 && yo == 0) || (xo > 0 && yo > 0)) {
                    if (has_top && has_left)
                        v = (st + sl + 4) >> 3;
                    else if (has_left)
                        v = (sl + 2) >> 2;
                    else if (has_top)
                        v = (st + 2) >> 2;
                } else if (xo > 0 && yo == 0) {
                    if (has_top)
                        v = (st + 2) >> 2;
                    else if (has_left)
                        v = (sl + 2) >> 2;
                } else {
                    if (has_left)
                        v = (sl + 2) >> 2;
                    else if (has_top)
                        v = (st + 2) >> 2;
                }
                for (int y = yo; y < yo + 4; ++y)
                    for (int x = xo; x < xo + 4; ++x)
                        out[y * 8 + x] = v;
            }
        return;
    }
    if (mode == 1 || mode == 2) {
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < 8; ++x)
                out[y * 8 + x] = mode == 1 ? left[y] : top[x];
        return;
    }
    // 8.3.4.4: yCF 4 for 4:2:2, whose c weighs V by 5 (8-142 to 8-145).
    constexpr int ycf = rows == 16 ? 4 : 0;
    int const corner = ic.chroma(comp, -1, -1);
    auto top_at = [&](int x) { return x < 0 ? corner : top[x]; };
    auto left_at = [&](int y) { return y < 0 ? corner : left[y]; };
    int h = 0, vv = 0;
    for (int k = 0; k < 4; ++k)
        h += (k + 1) * (top_at(4 + k) - top_at(2 - k));
    for (int k = 0; k < 4 + ycf; ++k)
        vv += (k + 1) * (left_at(4 + ycf + k) - left_at(2 + ycf - k));
    int const a = 16 * (left[rows - 1] + top[7]);
    int const b = (34 * h + 32) >> 6;
    int const c = ((ycf ? 5 : 34) * vv + 32) >> 6;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < 8; ++x)
            out[y * 8 + x] = clip_sample<P>((a + b * (x - 3) + c * (y - 3 - ycf) + 16) >> 5, max);
}

template<typename P>
void write_block(P* dst, int stride, int const* pred, int w, int h)
{
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            dst[y * stride + x] = static_cast<P>(pred[y * w + x]);
}

template<typename P>
void write_block(P* dst, int stride, int const* pred, int size)
{
    write_block(dst, stride, pred, size, size);
}

bool any_nonzero(std::int32_t const* c, int n)
{
    for (int i = 0; i < n; ++i)
        if (c[i])
            return true;
    return false;
}

} // namespace

void clear_intra_motion(Frame& frame, int addr)
{
    Picture& pic = *frame.picture;
    int const mx = addr % frame.mb_width, my = addr / frame.mb_width;
    pic.mb_intra[static_cast<std::size_t>(frame.mbaff ? frame.storage_mb(addr) : frame.storage_mb_at(mx, my))] = 1;
    int const base = frame.mbaff ? frame.block_base(addr) : frame.block_base_at(mx, my), bw = frame.block_stride();
    for (int by = 0; by < 4; ++by)
        for (int bx = 0; bx < 4; ++bx) {
            int const i = base + by * bw + bx;
            for (int list = 0; list < 2; ++list) {
                pic.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)] = -1;
                pic.mv[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)] = {};
                pic.ref_pic[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)] = -1;
            }
        }
}

namespace {

// 8.5.1 to 8.5.3, and 8.5.5's Cb and Cr of 4:4:4 the same way: component
// k (0 Y, 1 Cb, 2 Cr) coded as luma is — predicted when intra (from the
// luma's modes), and its residual added. `qp`: the component's QP'.
template<typename P>
void luma_component(IntraContext<P>& ic, MbState const& s, MbSyntax const& mb, int k, std::int32_t const* coeffs, std::int32_t const* dc_levels, int qp,
    LevelScales const& ls, int max, int mid, bool bypass)
{
    constexpr bool wide = sizeof(P) == 2;
    int const stride = ic.luma_stride();
    P* const y0 = ic.luma_origin();
    std::array<std::int32_t, 64> d {};
    std::array<int, 256> pred {};
    int const cbp_luma = mb.cbp & 15;
    std::size_t const q = static_cast<std::size_t>(qp % 6);
    std::size_t const intra4 = static_cast<std::size_t>(k), inter4 = static_cast<std::size_t>(3 + k);
    std::size_t const intra8 = static_cast<std::size_t>(2 * k), inter8 = static_cast<std::size_t>(2 * k + 1);
    // 8.5.15: a vertically or horizontally predicted intra block's residual, summed.
    auto hor_pred = [](int mode) { return mode < 2 ? mode : -1; };
    if (mb.kind == MbKind::I4x4) {
        for (int blk = 0; blk < 16; ++blk) {
            int const r = blk_to_raster[blk];
            int const bx = r & 3, by = r >> 2;
            int const mode = s.intra4x4_modes[static_cast<std::size_t>(r)];
            predict_4x4(ic, bx * 4, by * 4, mode, pred.data(), mid);
            P* const dst = y0 + by * 4 * stride + bx * 4;
            write_block(dst, stride, pred.data(), 4);
            std::int32_t const* c = coeffs + r * 16;
            if ((cbp_luma >> (blk >> 2)) & 1 && any_nonzero(c, 16)) {
                if (bypass) {
                    bypass_add(dst, stride, c, 4, 4, hor_pred(mode), max);
                } else {
                    scale_4x4<wide>(d.data(), c, ls.scale4[intra4][q].data(), qp, false);
                    idct4x4_add(dst, stride, d.data(), max);
                }
            }
            ic.done(bx, by, 1, 1);
        }
    } else if (mb.kind == MbKind::I8x8) {
        for (int b8 = 0; b8 < 4; ++b8) {
            int const bx = (b8 & 1) * 2, by = (b8 >> 1) * 2;
            int const mode = s.intra4x4_modes[static_cast<std::size_t>(by * 4 + bx)];
            predict_8x8(ic, bx * 4, by * 4, mode, pred.data(), mid);
            P* const dst = y0 + by * 4 * stride + bx * 4;
            write_block(dst, stride, pred.data(), 8);
            std::int32_t const* c = coeffs + b8 * 64;
            if ((cbp_luma >> b8) & 1 && any_nonzero(c, 64)) {
                if (bypass) {
                    bypass_add(dst, stride, c, 8, 8, hor_pred(mode), max);
                } else {
                    scale_8x8<wide>(d.data(), c, ls.scale8[intra8][q].data(), qp);
                    idct8x8_add(dst, stride, d.data(), max);
                }
            }
            ic.done(bx, by, 2, 2);
        }
    } else if (mb.kind == MbKind::I16x16) {
        predict_16x16(ic, mb.i16_mode, pred.data(), mid, max);
        write_block(y0, stride, pred.data(), 16);
        if (bypass) {
            // 8.5.10's dcY = c; the macroblock's residual as one 16x16 array (8.5.15).
            std::array<std::int32_t, 256> whole {};
            for (int r = 0; r < 16; ++r)
                for (int i = 0; i < 16; ++i)
                    whole[static_cast<std::size_t>(((r >> 2) * 4 + (i >> 2)) * 16 + (r & 3) * 4 + (i & 3))] = i == 0 ? dc_levels[r] : coeffs[r * 16 + i];
            bypass_add(y0, stride, whole.data(), 16, 16, hor_pred(mb.i16_mode), max);
            return;
        }
        std::array<std::int32_t, 16> dc {};
        luma_dc<wide>(dc.data(), dc_levels, ls.scale4[intra4][q][0], qp);
        for (int r = 0; r < 16; ++r) {
            std::array<std::int32_t, 16> c {};
            std::copy_n(coeffs + r * 16, 16, c.data());
            c[0] = dc[static_cast<std::size_t>(r)];
            if (!any_nonzero(c.data(), 16))
                continue;
            scale_4x4<wide>(d.data(), c.data(), ls.scale4[intra4][q].data(), qp, true);
            idct4x4_add(y0 + (r >> 2) * 4 * stride + (r & 3) * 4, stride, d.data(), max);
        }
    } else if (cbp_luma) {
        if (mb.transform_8x8) {
            for (int b8 = 0; b8 < 4; ++b8) {
                std::int32_t const* c = coeffs + b8 * 64;
                if (!((cbp_luma >> b8) & 1) || !any_nonzero(c, 64))
                    continue;
                P* const dst = y0 + (b8 >> 1) * 8 * stride + (b8 & 1) * 8;
                if (bypass) {
                    bypass_add(dst, stride, c, 8, 8, -1, max);
                } else {
                    scale_8x8<wide>(d.data(), c, ls.scale8[inter8][q].data(), qp);
                    idct8x8_add(dst, stride, d.data(), max);
                }
            }
        } else {
            for (int r = 0; r < 16; ++r) {
                int const b8 = (r >> 3) * 2 + ((r & 3) >> 1);
                std::int32_t const* c = coeffs + r * 16;
                if (!((cbp_luma >> b8) & 1) || !any_nonzero(c, 16))
                    continue;
                P* const dst = y0 + (r >> 2) * 4 * stride + (r & 3) * 4;
                if (bypass) {
                    bypass_add(dst, stride, c, 4, 4, -1, max);
                } else {
                    scale_4x4<wide>(d.data(), c, ls.scale4[inter4][q].data(), qp, false);
                    idct4x4_add(dst, stride, d.data(), max);
                }
            }
        }
    }
}

// 8.5.4 for 4:2:0 and 4:2:2: a chroma component predicted (intra, 8.3.4),
// its DC transformed (2x2 or 2x4) and its 4x4 blocks added.
template<typename P>
void chroma_component(Frame const& frame, IntraContext<P>& ic, MbState const& s, MbSyntax const& mb, int comp, LevelScales const& ls, bool bypass)
{
    constexpr bool wide = sizeof(P) == 2;
    bool const inter = !s.intra;
    int const rows = frame.mb_chroma_height, cstride = ic.chroma_stride();
    int const max = frame.chroma_max, mid = 1 << (frame.bit_depth_chroma - 1);
    bool const f422 = frame.chroma_format == 2;
    int const blocks = f422 ? 8 : 4;
    P* const c0 = ic.chroma_origin(comp);
    std::array<int, 128> pred; // written whole by the prediction before it is read
    if (!inter) {
        if (f422)
            predict_chroma<16>(ic, comp, mb.chroma_mode, pred.data(), mid, max);
        else
            predict_chroma<8>(ic, comp, mb.chroma_mode, pred.data(), mid, max);
        write_block(c0, cstride, pred.data(), 8, rows);
    }
    int const cbp_chroma = mb.cbp >> 4;
    if (cbp_chroma == 0)
        return;
    std::size_t const sc = static_cast<std::size_t>(comp);
    int const qpc = s.qpc[sc] + frame.qp_offset_chroma; // QP'C
    std::size_t const list = static_cast<std::size_t>((inter ? 3 : 0) + 1 + comp);
    int const* scale = ls.scale4[list][static_cast<std::size_t>(qpc % 6)].data();
    std::int32_t const* dc_levels = f422 ? mb.chroma_dc_wide[sc].data() : mb.chroma_dc[sc].data();
    std::int32_t const* ac = f422 ? mb.chroma_wide[sc].data() : mb.chroma_ac[sc].data();
    std::array<std::int32_t, 8> dc {};
    if (bypass) {
        // 8-323: dcC = c, laid out as 8-304 and 8-305 place the levels.
        static constexpr std::uint8_t order_422[8] = { 0, 2, 1, 5, 3, 6, 4, 7 };
        for (int b = 0; b < blocks; ++b)
            dc[static_cast<std::size_t>(b)] = dc_levels[f422 ? order_422[b] : b];
    } else if (f422) {
        int const qpdc = qpc + 3; // 8-327
        chroma_dc_422(dc.data(), dc_levels, ls.scale4[list][static_cast<std::size_t>(qpdc % 6)][0], qpdc);
    } else {
        chroma_dc<wide>(dc.data(), dc_levels, scale[0], qpc);
    }
    if (bypass) {
        // The residual as one 8 x rows array, summed by 8.5.15 (horPredFlag
        // 2 - intra_chroma_pred_mode) for modes 1 and 2.
        std::array<std::int32_t, 16 * 8> whole {};
        for (int b = 0; b < blocks; ++b)
            for (int i = 0; i < 16; ++i)
                whole[static_cast<std::size_t>(((b >> 1) * 4 + (i >> 2)) * 8 + (b & 1) * 4 + (i & 3))]
                    = i == 0 ? dc[static_cast<std::size_t>(b)] : (cbp_chroma == 2 ? ac[b * 16 + i] : 0);
        int const hor = !inter && (mb.chroma_mode == 1 || mb.chroma_mode == 2) ? 2 - mb.chroma_mode : -1;
        bypass_add(c0, cstride, whole.data(), 8, rows, hor, max);
        return;
    }
    std::array<std::int32_t, 64> d; // written by the scaling before the transform reads it
    for (int b = 0; b < blocks; ++b) {
        std::array<std::int32_t, 16> c {};
        if (cbp_chroma == 2)
            std::copy_n(ac + b * 16, 16, c.data());
        c[0] = dc[static_cast<std::size_t>(b)];
        if (!any_nonzero(c.data(), 16))
            continue;
        scale_4x4<wide>(d.data(), c.data(), scale, qpc, true);
        idct4x4_add(c0 + (b >> 1) * 4 * cstride + (b & 1) * 4, cstride, d.data(), max);
    }
}

template<typename P>
void reconstruct(Frame& frame, Slice const& slice, int addr, MbSyntax const& mb)
{
    MbState& s = frame.mbs[static_cast<std::size_t>(addr)];
    IntraContext<P> ic(frame, slice, addr);
    int const stride = ic.luma_stride();
    int const cstride = ic.chroma_stride();
    P* const y0 = ic.luma_origin();
    s.qpc[0] = chroma_qp(s.qp, frame.chroma_qp_offset[0], frame.qp_offset_chroma);
    s.qpc[1] = chroma_qp(s.qp, frame.chroma_qp_offset[1], frame.qp_offset_chroma);
    int const cw = frame.mb_chroma_width, ch = frame.mb_chroma_height;
    if (mb.kind == MbKind::IPCM) {
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                y0[y * stride + x] = static_cast<P>(mb.pcm[static_cast<std::size_t>(y * 16 + x)]);
        for (int c = 0; c < 2 && cw > 0; ++c) {
            P* const c0 = ic.chroma_origin(c);
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x)
                    c0[y * cstride + x] = static_cast<P>(mb.pcm[static_cast<std::size_t>(256 + c * cw * ch + y * cw + x)]);
        }
        return;
    }
    LevelScales const& ls = *slice.scales;
    int const qp = s.qp + frame.qp_offset_luma; // QP'Y
    // TransformBypassModeFlag (7.4.2.1.1): lossless macroblocks.
    bool const bypass = frame.transform_bypass && qp == 0;
    if (!s.intra)
        inter_predict<P>(frame, slice, addr);
    luma_component(ic, s, mb, frame.list_component, mb.luma.data(), mb.luma_dc.data(), qp, ls, frame.luma_max, 1 << (frame.bit_depth_luma - 1), bypass);
    if (frame.chroma_format == 3) {
        for (int c = 0; c < 2; ++c) {
            IntraContext<P> icc(frame, slice, addr, 1 + c);
            std::size_t const sc = static_cast<std::size_t>(c);
            luma_component(icc, s, mb, 1 + c, mb.chroma_wide[sc].data(), mb.chroma_dc_wide[sc].data(), s.qpc[sc] + frame.qp_offset_chroma, ls,
                frame.chroma_max, 1 << (frame.bit_depth_chroma - 1), bypass);
        }
    } else if (frame.chroma_format != 0) {
        for (int c = 0; c < 2; ++c)
            chroma_component(frame, ic, s, mb, c, ls, bypass);
    }
}

}

void reconstruct_macroblock(Frame& frame, Slice const& slice, int addr, MbSyntax const& mb)
{
    if (frame.wide)
        reconstruct<std::uint16_t>(frame, slice, addr, mb);
    else
        reconstruct<std::uint8_t>(frame, slice, addr, mb);
}

}
