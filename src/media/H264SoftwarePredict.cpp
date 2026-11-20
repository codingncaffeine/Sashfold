// A macroblock's motion (8.4.1), its prediction — intra (8.3) or inter
// (8.4.2, weighted as 8.4.2.3 says) — and its residual added (8.5).
#include "media/H264SoftwareInternal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sashfold::media::h264sw {

namespace {

constexpr std::uint8_t blk_to_raster[16] = { 0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15 };

// Table 8-15.
int chroma_qp(int qp_y, int offset)
{
    static constexpr std::uint8_t table[22] = { 29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39 };
    int const qpi = std::clamp(qp_y + offset, 0, 51);
    return qpi < 30 ? qpi : table[qpi - 30];
}

std::uint8_t clip1(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }

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
        , m_base(frame.block_base(addr))
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
        if (m_frame.mbaff) {
            // 6.4.12.2 for the luma location, then 8.4.1.3.2's scaling
            // between a frame and a field macroblock.
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
        if (!mb_available(mx, my))
            return n;
        n.available = true;
        int const neighbor = my * m_w + mx;
        if (m_frame.mbs[static_cast<std::size_t>(neighbor)].intra)
            return n;
        int const i = m_frame.block_base(neighbor) + ((by + 4) & 3) * m_bw + ((bx + 4) & 3);
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
    Col colocated(int bx, int by) const
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
    frame.picture->mb_intra[static_cast<std::size_t>(frame.storage_mb(addr))] = 0;
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
struct EdgeWindow {
    static constexpr int pitch = 32;
    std::uint8_t samples[pitch * (16 + 6)];
};

// `src` for a block at (x, y) of w x h whose filter reads `before` samples
// above and left of it and `after` below and right: the plane itself (a
// frame's, or a field's every other row: `pitch` apart), or `window`
// filled from it.
std::uint8_t const* mc_source(std::uint8_t const* data, int pitch, int width, int height, int x, int y, int w, int h, int before, int after,
    EdgeWindow& window, int& stride)
{
    if (x - before >= 0 && y - before >= 0 && x + w + after <= width && y + h + after <= height) {
        stride = pitch;
        return data + static_cast<std::ptrdiff_t>(y) * pitch + x;
    }
    int const rows = h + before + after, cols = w + before + after;
    for (int r = 0; r < rows; ++r) {
        std::uint8_t const* row = data + static_cast<std::ptrdiff_t>(std::clamp(y - before + r, 0, height - 1)) * pitch;
        std::uint8_t* out = window.samples + r * EdgeWindow::pitch;
        for (int c = 0; c < cols; ++c)
            out[c] = row[std::clamp(x - before + c, 0, width - 1)];
    }
    stride = EdgeWindow::pitch;
    return window.samples + before * EdgeWindow::pitch + before;
}

inline int six_tap(int a, int b, int c, int d, int e, int f) { return a - 5 * b + 20 * c + 20 * d - 5 * e + f; }

// b (8-241): the horizontal half samples of the block's rows.
template<int W>
void half_h(std::uint8_t* dst, std::uint8_t const* src, int stride, int h)
{
    for (int r = 0; r < h; ++r, src += stride, dst += mc_stride)
        for (int c = 0; c < W; ++c)
            dst[c] = clip1((six_tap(src[c - 2], src[c - 1], src[c], src[c + 1], src[c + 2], src[c + 3]) + 16) >> 5);
}

// h (8-242): the vertical half samples.
template<int W>
void half_v(std::uint8_t* dst, std::uint8_t const* src, int stride, int h)
{
    for (int r = 0; r < h; ++r, src += stride, dst += mc_stride)
        for (int c = 0; c < W; ++c)
            dst[c] = clip1((six_tap(src[c - 2 * stride], src[c - stride], src[c], src[c + stride], src[c + 2 * stride], src[c + 3 * stride]) + 16) >> 5);
}

// j (8-243, 8-244): the centre half samples, from the unclipped horizontal
// intermediates of rows -2 .. h + 2.
template<int W>
void half_hv(std::uint8_t* dst, std::uint8_t const* src, int stride, int h)
{
    std::int16_t mid[(16 + 5) * 16];
    std::uint8_t const* s = src - 2 * stride;
    for (int r = 0; r < h + 5; ++r, s += stride)
        for (int c = 0; c < W; ++c)
            mid[r * 16 + c] = static_cast<std::int16_t>(six_tap(s[c - 2], s[c - 1], s[c], s[c + 1], s[c + 2], s[c + 3]));
    for (int r = 0; r < h; ++r, dst += mc_stride)
        for (int c = 0; c < W; ++c) {
            std::int16_t const* m = mid + r * 16 + c;
            dst[c] = clip1((six_tap(m[0], m[16], m[32], m[48], m[64], m[80]) + 512) >> 10);
        }
}

template<int W>
void average(std::uint8_t* dst, std::uint8_t const* a, int a_stride, std::uint8_t const* b, int b_stride, int h)
{
    for (int r = 0; r < h; ++r, dst += mc_stride, a += a_stride, b += b_stride)
        for (int c = 0; c < W; ++c)
            dst[c] = static_cast<std::uint8_t>((a[c] + b[c] + 1) >> 1);
}

// 8.4.2.2.1 for a w x h block at (x, y) with fraction (fx, fy), into `out`
// (rows of mc_stride).
template<int W>
void luma_partition(std::uint8_t const* data, int pitch, int width, int height, int x, int y, int fx, int fy, int h, std::uint8_t* out)
{
    EdgeWindow window;
    int stride = 0;
    if (fx == 0 && fy == 0) {
        std::uint8_t const* src = mc_source(data, pitch, width, height, x, y, W, h, 0, 0, window, stride);
        for (int r = 0; r < h; ++r)
            std::memcpy(out + r * mc_stride, src + r * stride, W);
        return;
    }
    std::uint8_t const* g = mc_source(data, pitch, width, height, x, y, W, h, 2, 3, window, stride);
    std::uint8_t b[mc_stride * 17], hv[mc_stride * 16], j[mc_stride * 16];
    switch (fy * 4 + fx) {
    case 1: // a
        half_h<W>(b, g, stride, h);
        average<W>(out, g, stride, b, mc_stride, h);
        break;
    case 2: // b
        half_h<W>(out, g, stride, h);
        break;
    case 3: // c
        half_h<W>(b, g, stride, h);
        average<W>(out, g + 1, stride, b, mc_stride, h);
        break;
    case 4: // d
        half_v<W>(hv, g, stride, h);
        average<W>(out, g, stride, hv, mc_stride, h);
        break;
    case 8: // h
        half_v<W>(out, g, stride, h);
        break;
    case 12: // n
        half_v<W>(hv, g, stride, h);
        average<W>(out, g + stride, stride, hv, mc_stride, h);
        break;
    case 5: // e
    case 7: // g
    case 13: // p
    case 15: // r
        // b of the block's rows (one row down for p and r), h of its columns (one right for g and r)
        half_h<W>(b, g + (fy == 3 ? stride : 0), stride, h);
        half_v<W>(hv, g + (fx == 3 ? 1 : 0), stride, h);
        average<W>(out, b, mc_stride, hv, mc_stride, h);
        break;
    case 6: // f
    case 14: // q
        half_hv<W>(j, g, stride, h);
        half_h<W>(b, g + (fy == 3 ? stride : 0), stride, h);
        average<W>(out, j, mc_stride, b, mc_stride, h);
        break;
    case 9: // i
    case 11: // k
        half_hv<W>(j, g, stride, h);
        half_v<W>(hv, g + (fx == 3 ? 1 : 0), stride, h);
        average<W>(out, j, mc_stride, hv, mc_stride, h);
        break;
    default: // 10, j
        half_hv<W>(out, g, stride, h);
        break;
    }
}

void luma_partition(std::uint8_t const* data, int pitch, int width, int height, int x, int y, int fx, int fy, int w, int h, std::uint8_t* out)
{
    if (w == 16)
        luma_partition<16>(data, pitch, width, height, x, y, fx, fy, h, out);
    else if (w == 8)
        luma_partition<8>(data, pitch, width, height, x, y, fx, fy, h, out);
    else
        luma_partition<4>(data, pitch, width, height, x, y, fx, fy, h, out);
}

// 8.4.2.2.2 for a w x h chroma block at (x, y) with fraction (fx, fy).
void chroma_partition(std::uint8_t const* data, int pitch, int width, int height, int x, int y, int fx, int fy, int w, int h, std::uint8_t* out)
{
    EdgeWindow window;
    int stride = 0;
    std::uint8_t const* s = mc_source(data, pitch, width, height, x, y, w, h, 0, 1, window, stride);
    int const a = (8 - fx) * (8 - fy), b = fx * (8 - fy), c = (8 - fx) * fy, d = fx * fy;
    for (int r = 0; r < h; ++r, s += stride, out += mc_stride)
        for (int i = 0; i < w; ++i)
            out[i] = static_cast<std::uint8_t>((a * s[i] + b * s[i + 1] + c * s[i + stride] + d * s[i + stride + 1] + 32) >> 6);
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
// (8-270 to 8-273: default, then explicit or implicit with two lists, then one).
void weigh_block(std::uint8_t* dst, int stride, std::uint8_t const* p0, std::uint8_t const* p1, bool l0, bool l1, Weights const& w, int bw, int bh)
{
    for (int r = 0; r < bh; ++r, dst += stride, p0 += mc_stride, p1 += mc_stride) {
        if (l0 && l1 && w.mode == 0) {
            for (int c = 0; c < bw; ++c)
                dst[c] = static_cast<std::uint8_t>((p0[c] + p1[c] + 1) >> 1);
        } else if (!(l0 && l1) && w.mode != 1) {
            std::memcpy(dst, l0 ? p0 : p1, static_cast<std::size_t>(bw));
        } else if (l0 && l1) {
            int const round = 1 << w.log_wd, shift = w.log_wd + 1, o = (w.o0 + w.o1 + 1) >> 1;
            for (int c = 0; c < bw; ++c)
                dst[c] = clip1(((p0[c] * w.w0 + p1[c] * w.w1 + round) >> shift) + o);
        } else {
            std::uint8_t const* p = l0 ? p0 : p1;
            int const wt = l0 ? w.w0 : w.w1, o = l0 ? w.o0 : w.o1;
            if (w.log_wd >= 1) {
                int const round = 1 << (w.log_wd - 1);
                for (int c = 0; c < bw; ++c)
                    dst[c] = clip1(((p[c] * wt + round) >> w.log_wd) + o);
            } else {
                for (int c = 0; c < bw; ++c)
                    dst[c] = clip1(p[c] * wt + o);
            }
        }
    }
}

// The macroblock being predicted: where its motion and its samples are.
struct InterTarget {
    InterTarget(Picture& p, Slice const& s)
        : pic(p)
        , slice(s)
        , lists(s.list.data())
    {
    }
    Picture& pic;
    Slice const& slice;
    std::vector<RefEntry> const* lists; // [2]: the slice's, or an MBAFF field macroblock's fields
    int base = 0; // its first motion block
    int bw = 0; // motion blocks per row
    Frame::Place place;
    int mbaff_parity = -1; // an MBAFF field macroblock's parity
    std::uint8_t* luma = nullptr; // the rows it is made of
    std::uint8_t* chroma[2] = { nullptr, nullptr };
    int luma_stride = 0;
    int chroma_stride = 0;
};

// Whether the 4x4 blocks of a w4 x h4 rectangle at (x4, y4) of the
// macroblock share their motion.
bool same_motion(InterTarget const& t, int x4, int y4, int w4, int h4)
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
void predict_rectangle(InterTarget const& t, int x4, int y4, int w4, int h4)
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
    alignas(16) std::uint8_t pl[2][mc_stride * 16];
    for (int list = 0; list < 2; ++list)
        if (use[list])
            luma_partition(ref_pics[list]->luma(), ref_pics[list]->luma_stride(), pic.width, ref_pics[list]->luma_height(), x0 + (mvs[list].x >> 2),
                y0 + (mvs[list].y >> 2), mvs[list].x & 3, mvs[list].y & 3, w, h, pl[list]);
    weigh_block(t.luma + static_cast<std::ptrdiff_t>(y0) * t.luma_stride + x0, t.luma_stride, pl[0], pl[1], use[0], use[1],
        weights_for(t.slice, refs[0], refs[1], use[0], use[1], 0, t.mbaff_parity), w, h);
    int const cx0 = x0 / 2, cy0 = y0 / 2;
    for (int c = 0; c < 2; ++c) {
        for (int list = 0; list < 2; ++list) {
            if (!use[list])
                continue;
            RefEntry const& r = *ref_pics[list];
            // Table 8-10: between fields of opposite parity, the chroma
            // vector moves a quarter of a chroma row.
            int mvc_y = mvs[list].y;
            if (t.place.structure == 2 && r.structure == 1)
                mvc_y += 2;
            else if (t.place.structure == 1 && r.structure == 2)
                mvc_y -= 2;
            chroma_partition(r.chroma(c), r.chroma_stride(), pic.width / 2, r.luma_height() / 2, cx0 + (mvs[list].x >> 3), cy0 + (mvc_y >> 3),
                mvs[list].x & 7, mvc_y & 7, w / 2, h / 2, pl[list]);
        }
        weigh_block(t.chroma[c] + static_cast<std::ptrdiff_t>(cy0) * t.chroma_stride + cx0, t.chroma_stride, pl[0], pl[1], use[0], use[1],
            weights_for(t.slice, refs[0], refs[1], use[0], use[1], 1 + c, t.mbaff_parity), w / 2, h / 2);
    }
}

// An 8x8 quadrant: whole, two halves either way, or four 4x4 blocks.
void predict_quadrant(InterTarget const& t, int x4, int y4)
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

void inter_predict(Frame& frame, Slice const& slice, int addr)
{
    InterTarget t(*frame.picture, slice);
    t.base = frame.block_base(addr);
    t.bw = frame.block_stride();
    t.place = frame.place(addr);
    if (frame.mbaff && t.place.structure != 0) {
        t.mbaff_parity = t.place.structure - 1;
        t.lists = slice.field_list[static_cast<std::size_t>(t.mbaff_parity)].data();
    }
    t.luma = frame.luma_rows(t.place.structure);
    t.luma_stride = frame.luma_rows_stride(t.place.structure);
    for (int c = 0; c < 2; ++c)
        t.chroma[c] = frame.chroma_rows(c, t.place.structure);
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
class IntraContext {
public:
    IntraContext(Frame& frame, Slice const& slice, int addr)
        : m_frame(frame)
        , m_slice(slice)
        , m_x(addr % frame.mb_width)
        , m_y(addr / frame.mb_width)
    {
        Frame::Place const p = frame.place(addr);
        m_stride = frame.luma_rows_stride(p.structure);
        m_cstride = frame.chroma_rows_stride(p.structure);
        m_luma = frame.luma_rows(p.structure) + static_cast<std::ptrdiff_t>(p.y) * m_stride + p.x;
        for (int c = 0; c < 2; ++c)
            m_chroma[c] = frame.chroma_rows(c, p.structure) + static_cast<std::ptrdiff_t>(p.y / 2) * m_cstride + p.x / 2;
        if (frame.mbaff && frame.mbs[static_cast<std::size_t>(addr)].intra)
            gather_mbaff_neighbors(addr);
    }
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
    bool chroma_left() const { return m_mbaff ? chroma_left_rows(0, 8) : mb_ok(m_x - 1, m_y); }
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
    std::uint8_t* luma_origin() const { return m_luma; }
    std::uint8_t* chroma_origin(int c) const { return m_chroma[c]; }
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
        auto luma_at = [&](Frame::Location const& n) {
            Frame::Place const p = m_frame.place(n.addr);
            return static_cast<int>(m_frame.luma_rows(p.structure)[static_cast<std::ptrdiff_t>(p.y + n.y) * m_frame.luma_rows_stride(p.structure) + p.x + n.x]);
        };
        auto chroma_at = [&](int c, Frame::Location const& n) {
            Frame::Place const p = m_frame.place(n.addr);
            return static_cast<int>(
                m_frame.chroma_rows(c, p.structure)[static_cast<std::ptrdiff_t>(p.y / 2 + n.y) * m_frame.chroma_rows_stride(p.structure) + p.x / 2 + n.x]);
        };
        auto luma_location = [&](int x, int y) { return m_frame.neighbor(addr, x, y, 16, 16, m_slice.number); };
        auto chroma_location = [&](int x, int y) { return m_frame.neighbor(addr, x, y, 8, 8, m_slice.number); };
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
        Frame::Location const chroma_corner = chroma_location(-1, -1);
        m_chroma_corner_ok = usable(chroma_corner);
        m_chroma_top_ok = true;
        for (int k = 0; k < 8; ++k) {
            Frame::Location const left = chroma_location(-1, k);
            Frame::Location const above = chroma_location(k, -1);
            m_chroma_left_ok[k] = usable(left);
            m_chroma_top_ok = m_chroma_top_ok && usable(above);
            for (int c = 0; c < 2; ++c) {
                m_chroma_left[c][k] = m_chroma_left_ok[k] ? chroma_at(c, left) : 0;
                m_chroma_top[c][k] = usable(above) ? chroma_at(c, above) : 0;
            }
        }
        for (int c = 0; c < 2; ++c)
            m_chroma_corner[c] = m_chroma_corner_ok ? chroma_at(c, chroma_corner) : 0;
    }

    Frame& m_frame;
    Slice const& m_slice;
    int m_x;
    int m_y;
    std::uint8_t* m_luma = nullptr;
    std::uint8_t* m_chroma[2] = { nullptr, nullptr };
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
    std::array<std::array<int, 8>, 2> m_chroma_left {};
    std::array<std::array<int, 8>, 2> m_chroma_top {};
    std::array<int, 2> m_chroma_corner {};
    std::array<bool, 8> m_chroma_left_ok {};
    bool m_chroma_top_ok = false;
    bool m_chroma_corner_ok = false;
};

// 8.3.1.2: a 4x4 block's prediction into `out` (raster 4x4).
void predict_4x4(IntraContext const& ic, int xo, int yo, int mode, int* out)
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
            int v = 128;
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
                    v = 128;
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
void predict_8x8(IntraContext const& ic, int xo, int yo, int mode, int* out)
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
            int v = 128;
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
void predict_16x16(IntraContext const& ic, int mode, int* out)
{
    bool const has_top = ic.luma_available(0, -1);
    bool const has_left = ic.left_available(0, 0, 16);
    // A mode whose samples are not there is no conforming stream's; such
    // a stream gets DC from what there is (as ffmpeg conceals it).
    if ((mode == 0 && !has_top) || (mode == 1 && !has_left) || (mode == 3 && !(has_top && has_left && ic.luma_available(-1, -1))))
        mode = 2;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            int v = 128;
            if (mode == 0) {
                v = ic.luma(x, -1);
            } else if (mode == 1) {
                v = ic.luma(-1, y);
            } else if (mode == 2) {
                int st = 0, sl = 0;
                for (int k = 0; k < 16; ++k) {
                    if (has_top)
                        st += ic.luma(k, -1);
                    if (has_left)
                        sl += ic.luma(-1, k);
                }
                if (has_top && has_left)
                    v = (st + sl + 16) >> 5;
                else if (has_left)
                    v = (sl + 8) >> 4;
                else if (has_top)
                    v = (st + 8) >> 4;
            } else {
                int h = 0, vv = 0;
                for (int k = 0; k < 8; ++k) {
                    h += (k + 1) * (ic.luma(8 + k, -1) - ic.luma(6 - k, -1));
                    vv += (k + 1) * (ic.luma(-1, 8 + k) - ic.luma(-1, 6 - k));
                }
                int const a = 16 * (ic.luma(-1, 15) + ic.luma(15, -1));
                int const b = (5 * h + 32) >> 6;
                int const c = (5 * vv + 32) >> 6;
                v = clip1((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
            }
            out[y * 16 + x] = v;
        }
}

// 8.3.4 for 4:2:0.
void predict_chroma(IntraContext const& ic, int comp, int mode, int* out)
{
    bool const has_top = ic.chroma_top();
    bool const left_all = ic.chroma_left();
    if ((mode == 1 && !left_all) || (mode == 2 && !has_top) || (mode == 3 && !(has_top && left_all && ic.chroma_corner())))
        mode = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            int v = 128;
            if (mode == 0) {
                int const xo = x & ~3, yo = y & ~3;
                // 8.3.4.1: each 4x4 block's own left samples, all there or not.
                bool const has_left = ic.chroma_left_rows(yo, 4);
                int st = 0, sl = 0;
                for (int k = 0; k < 4; ++k) {
                    if (has_top)
                        st += ic.chroma(comp, xo + k, -1);
                    if (has_left)
                        sl += ic.chroma(comp, -1, yo + k);
                }
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
            } else if (mode == 1) {
                v = ic.chroma(comp, -1, y);
            } else if (mode == 2) {
                v = ic.chroma(comp, x, -1);
            } else {
                int h = 0, vv = 0;
                for (int k = 0; k < 4; ++k) {
                    h += (k + 1) * (ic.chroma(comp, 4 + k, -1) - ic.chroma(comp, 2 - k, -1));
                    vv += (k + 1) * (ic.chroma(comp, -1, 4 + k) - ic.chroma(comp, -1, 2 - k));
                }
                int const a = 16 * (ic.chroma(comp, -1, 7) + ic.chroma(comp, 7, -1));
                int const b = (34 * h + 32) >> 6;
                int const c = (34 * vv + 32) >> 6;
                v = clip1((a + b * (x - 3) + c * (y - 3) + 16) >> 5);
            }
            out[y * 8 + x] = v;
        }
}

void write_block(std::uint8_t* dst, int stride, int const* pred, int size)
{
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            dst[y * stride + x] = static_cast<std::uint8_t>(pred[y * size + x]);
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
    pic.mb_intra[static_cast<std::size_t>(frame.storage_mb(addr))] = 1;
    int const base = frame.block_base(addr), bw = frame.block_stride();
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

void reconstruct_macroblock(Frame& frame, Slice const& slice, int addr, MbSyntax const& mb)
{
    MbState& s = frame.mbs[static_cast<std::size_t>(addr)];
    IntraContext ic(frame, slice, addr);
    int const stride = ic.luma_stride();
    int const cstride = ic.chroma_stride();
    std::uint8_t* const y0 = ic.luma_origin();
    s.qpc[0] = chroma_qp(s.qp, frame.chroma_qp_offset[0]);
    s.qpc[1] = chroma_qp(s.qp, frame.chroma_qp_offset[1]);
    if (mb.kind == MbKind::IPCM) {
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                y0[y * stride + x] = mb.pcm_luma[static_cast<std::size_t>(y * 16 + x)];
        for (int c = 0; c < 2; ++c) {
            std::uint8_t* const c0 = ic.chroma_origin(c);
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    c0[y * cstride + x] = mb.pcm_chroma[static_cast<std::size_t>(c * 64 + y * 8 + x)];
        }
        return;
    }
    LevelScales const& ls = *slice.scales;
    bool const inter = !s.intra;
    int const qp = s.qp;
    std::array<std::int32_t, 64> d {};
    std::array<int, 256> pred {};
    if (inter)
        inter_predict(frame, slice, addr);
    int const cbp_luma = mb.cbp & 15;
    // Luma.
    if (mb.kind == MbKind::I4x4) {
        for (int blk = 0; blk < 16; ++blk) {
            int const r = blk_to_raster[blk];
            int const bx = r & 3, by = r >> 2;
            predict_4x4(ic, bx * 4, by * 4, s.intra4x4_modes[static_cast<std::size_t>(r)], pred.data());
            std::uint8_t* const dst = y0 + by * 4 * stride + bx * 4;
            write_block(dst, stride, pred.data(), 4);
            std::int32_t const* c = mb.luma.data() + r * 16;
            if ((cbp_luma >> (blk >> 2)) & 1 && any_nonzero(c, 16)) {
                scale_4x4(d.data(), c, ls.scale4[0][static_cast<std::size_t>(qp % 6)].data(), qp, false);
                idct4x4_add(dst, stride, d.data());
            }
            ic.done(bx, by, 1, 1);
        }
    } else if (mb.kind == MbKind::I8x8) {
        for (int b8 = 0; b8 < 4; ++b8) {
            int const bx = (b8 & 1) * 2, by = (b8 >> 1) * 2;
            predict_8x8(ic, bx * 4, by * 4, s.intra4x4_modes[static_cast<std::size_t>(by * 4 + bx)], pred.data());
            std::uint8_t* const dst = y0 + by * 4 * stride + bx * 4;
            write_block(dst, stride, pred.data(), 8);
            std::int32_t const* c = mb.luma.data() + b8 * 64;
            if ((cbp_luma >> b8) & 1 && any_nonzero(c, 64)) {
                scale_8x8(d.data(), c, ls.scale8[0][static_cast<std::size_t>(qp % 6)].data(), qp);
                idct8x8_add(dst, stride, d.data());
            }
            ic.done(bx, by, 2, 2);
        }
    } else {
        if (mb.kind == MbKind::I16x16) {
            predict_16x16(ic, mb.i16_mode, pred.data());
            write_block(y0, stride, pred.data(), 16);
            std::array<std::int32_t, 16> dc {};
            luma_dc(dc.data(), mb.luma_dc.data(), ls.scale4[0][static_cast<std::size_t>(qp % 6)][0], qp);
            for (int r = 0; r < 16; ++r) {
                std::array<std::int32_t, 16> c {};
                std::copy_n(mb.luma.data() + r * 16, 16, c.data());
                c[0] = dc[static_cast<std::size_t>(r)];
                if (!any_nonzero(c.data(), 16))
                    continue;
                scale_4x4(d.data(), c.data(), ls.scale4[0][static_cast<std::size_t>(qp % 6)].data(), qp, true);
                idct4x4_add(y0 + (r >> 2) * 4 * stride + (r & 3) * 4, stride, d.data());
            }
        } else if (cbp_luma) {
            if (mb.transform_8x8) {
                for (int b8 = 0; b8 < 4; ++b8) {
                    std::int32_t const* c = mb.luma.data() + b8 * 64;
                    if (!((cbp_luma >> b8) & 1) || !any_nonzero(c, 64))
                        continue;
                    scale_8x8(d.data(), c, ls.scale8[1][static_cast<std::size_t>(qp % 6)].data(), qp);
                    idct8x8_add(y0 + (b8 >> 1) * 8 * stride + (b8 & 1) * 8, stride, d.data());
                }
            } else {
                for (int r = 0; r < 16; ++r) {
                    int const b8 = (r >> 3) * 2 + ((r & 3) >> 1);
                    std::int32_t const* c = mb.luma.data() + r * 16;
                    if (!((cbp_luma >> b8) & 1) || !any_nonzero(c, 16))
                        continue;
                    scale_4x4(d.data(), c, ls.scale4[3][static_cast<std::size_t>(qp % 6)].data(), qp, false);
                    idct4x4_add(y0 + (r >> 2) * 4 * stride + (r & 3) * 4, stride, d.data());
                }
            }
        }
    }
    // Chroma.
    int const cbp_chroma = mb.cbp >> 4;
    for (int comp = 0; comp < 2; ++comp) {
        std::uint8_t* const c0 = ic.chroma_origin(comp);
        if (!inter) {
            predict_chroma(ic, comp, mb.chroma_mode, pred.data());
            write_block(c0, cstride, pred.data(), 8);
        }
        if (cbp_chroma == 0)
            continue;
        int const qpc = s.qpc[static_cast<std::size_t>(comp)];
        int const list = (inter ? 3 : 0) + 1 + comp;
        int const* scale = ls.scale4[static_cast<std::size_t>(list)][static_cast<std::size_t>(qpc % 6)].data();
        std::array<std::int32_t, 4> dc {};
        chroma_dc(dc.data(), mb.chroma_dc[static_cast<std::size_t>(comp)].data(), scale[0], qpc);
        for (int b4 = 0; b4 < 4; ++b4) {
            std::array<std::int32_t, 16> c {};
            if (cbp_chroma == 2)
                std::copy_n(mb.chroma_ac[static_cast<std::size_t>(comp)].data() + b4 * 16, 16, c.data());
            c[0] = dc[static_cast<std::size_t>(b4)];
            if (!any_nonzero(c.data(), 16))
                continue;
            scale_4x4(d.data(), c.data(), scale, qpc, true);
            idct4x4_add(c0 + (b4 >> 1) * 4 * cstride + (b4 & 1) * 4, cstride, d.data());
        }
    }
}

}
