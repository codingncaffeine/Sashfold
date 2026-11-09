// A macroblock's motion (8.4.1), its prediction — intra (8.3) or inter
// (8.4.2, weighted as 8.4.2.3 says) — and its residual added (8.5).
#include "media/H264SoftwareInternal.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

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

class MotionContext {
public:
    MotionContext(Frame& frame, Slice const& slice, int addr)
        : m_frame(frame)
        , m_slice(slice)
        , m_pic(*frame.picture)
        , m_addr(addr)
        , m_w(frame.picture->mb_width)
        , m_x(addr % frame.picture->mb_width)
        , m_y(addr / frame.picture->mb_width)
    {
    }

    bool mb_available(int mx, int my) const
    {
        if (mx < 0 || my < 0 || mx >= m_w)
            return false;
        int const a = my * m_w + mx;
        return a != m_addr && m_frame.mbs[static_cast<std::size_t>(a)].slice == m_slice.number;
    }

    // The motion of list `list` at 4x4 block (bx, by) relative to the
    // current macroblock (6.4.11.7 with 8.4.1.3.2's rules).
    Neighbor at(int list, int bx, int by) const
    {
        Neighbor n;
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
            int const i = m_pic.block_index(m_x * 4 + bx, m_y * 4 + by);
            n.ref = m_pic.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
            if (n.ref >= 0) {
                n.mv = m_pic.mv[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
                n.pic = m_pic.ref_pic[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
            }
            return n;
        }
        if (!mb_available(mx, my))
            return n;
        n.available = true;
        if (m_frame.mbs[static_cast<std::size_t>(my * m_w + mx)].intra)
            return n;
        int const gx = mx * 4 + ((bx + 4) & 3);
        int const gy = my * 4 + ((by + 4) & 3);
        int const i = m_pic.block_index(gx, gy);
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
        Neighbor a = at(list, x4 - 1, y4);
        Neighbor b = at(list, x4, y4 - 1);
        Neighbor c = at(list, x4 + w4, y4 - 1);
        if (!c.available)
            c = at(list, x4 - 1, y4 - 1);
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
        if (ref >= 0 && static_cast<std::size_t>(ref) < m_slice.list[static_cast<std::size_t>(list)].size()) {
            RefEntry const& e = m_slice.list[static_cast<std::size_t>(list)][static_cast<std::size_t>(ref)];
            pic = e.picture ? e.picture->serial : -1;
        }
        for (int y = y4; y < y4 + h4; ++y)
            for (int x = x4; x < x4 + w4; ++x) {
                int const i = m_pic.block_index(m_x * 4 + x, m_y * 4 + y);
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

    // 8.4.1.2.1: the colocated block's motion.
    struct Col {
        Mv mv;
        int ref = -1;
        std::int32_t pic = -1;
    };
    Col colocated(int bx, int by) const
    {
        Col c;
        if (m_slice.list[1].empty() || !m_slice.list[1][0].picture)
            return c;
        Picture const& col = *m_slice.list[1][0].picture;
        if (col.mb_width != m_pic.mb_width || col.mb_height != m_pic.mb_height)
            return c;
        if (m_slice.sps->direct_8x8_inference) {
            bx = (bx >> 1) * 3;
            by = (by >> 1) * 3;
        }
        if (col.mb_intra[static_cast<std::size_t>(m_addr)])
            return c;
        int const i = col.block_index(m_x * 4 + bx, m_y * 4 + by);
        int const list = col.ref_idx[0][static_cast<std::size_t>(i)] < 0 ? 1 : 0;
        c.ref = col.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
        c.mv = col.mv[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
        c.pic = col.ref_pic[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
        return c;
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
                    Neighbor c = at(list, 4, -1);
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
                Col const c = colocated(x, y);
                int ref0 = 0;
                if (c.ref >= 0) {
                    auto const& l0 = m_slice.list[0];
                    for (std::size_t k = 0; k < l0.size(); ++k) {
                        if (l0[k].picture && l0[k].picture->serial == c.pic) {
                            ref0 = static_cast<int>(k);
                            break;
                        }
                    }
                }
                Mv mv0, mv1;
                if (static_cast<std::size_t>(ref0) < m_slice.list[0].size() && !m_slice.list[1].empty()) {
                    RefEntry const& p0 = m_slice.list[0][static_cast<std::size_t>(ref0)];
                    RefEntry const& p1 = m_slice.list[1][0];
                    int const tb = std::clamp(m_slice.current_poc - p0.poc, -128, 127);
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
    frame.picture->mb_intra[static_cast<std::size_t>(addr)] = 0;
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

// 8.4.2.2.1 for one 4x4 block: the reference window it reads fetched
// once, its edge samples repeated past the picture (8-228, 8-229), and
// each half-sample array the block's fraction needs computed once; the
// quarter positions are the averages Table 8-12 names.
void luma_block(std::uint8_t const* data, int width, int height, int x, int y, int fx, int fy, int* out)
{
    // Rows y - 2 .. y + 6 and columns x - 2 .. x + 6.
    int win[9][9];
    for (int r = 0; r < 9; ++r) {
        std::uint8_t const* row = data + static_cast<std::ptrdiff_t>(std::clamp(y - 2 + r, 0, height - 1)) * width;
        for (int c = 0; c < 9; ++c)
            win[r][c] = row[std::clamp(x - 2 + c, 0, width - 1)];
    }
    auto G = [&](int r, int c) { return win[r + 2][c + 2]; }; // r, c from the block's corner, -2 .. 6
    if (fx == 0 && fy == 0) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out[r * 4 + c] = G(r, c);
        return;
    }
    auto tap_h = [&](int r, int c) { return G(r, c - 2) - 5 * G(r, c - 1) + 20 * G(r, c) + 20 * G(r, c + 1) - 5 * G(r, c + 2) + G(r, c + 3); };
    auto tap_v = [&](int r, int c) { return G(r - 2, c) - 5 * G(r - 1, c) + 20 * G(r, c) + 20 * G(r + 1, c) - 5 * G(r + 2, c) + G(r + 3, c); };
    // b at rows 0 .. 4 (row 4 is s), h at columns 0 .. 4 (column 4 is m), j.
    int b[5][4], h[4][5], j[4][4];
    // j enters f, i, j, k and q: a half step one way and any step the other.
    bool const need_j = (fx == 2 && fy != 0) || (fy == 2 && fx != 0);
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 4; ++c)
            b[r][c] = clip1((tap_h(r, c) + 16) >> 5);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 5; ++c)
            h[r][c] = clip1((tap_v(r, c) + 16) >> 5);
    if (need_j) {
        int b1[9][4]; // the unclipped horizontal half samples, rows -2 .. 6
        for (int r = 0; r < 9; ++r)
            for (int c = 0; c < 4; ++c)
                b1[r][c] = tap_h(r - 2, c);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                int const j1 = b1[r][c] - 5 * b1[r + 1][c] + 20 * b1[r + 2][c] + 20 * b1[r + 3][c] - 5 * b1[r + 4][c] + b1[r + 5][c];
                j[r][c] = clip1((j1 + 512) >> 10);
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            int v;
            switch (fy * 4 + fx) {
            case 1: v = (G(r, c) + b[r][c] + 1) >> 1; break; // a
            case 2: v = b[r][c]; break; // b
            case 3: v = (b[r][c] + G(r, c + 1) + 1) >> 1; break; // c
            case 4: v = (G(r, c) + h[r][c] + 1) >> 1; break; // d
            case 5: v = (b[r][c] + h[r][c] + 1) >> 1; break; // e
            case 6: v = (b[r][c] + j[r][c] + 1) >> 1; break; // f
            case 7: v = (b[r][c] + h[r][c + 1] + 1) >> 1; break; // g
            case 8: v = h[r][c]; break; // h
            case 9: v = (h[r][c] + j[r][c] + 1) >> 1; break; // i
            case 10: v = j[r][c]; break; // j
            case 11: v = (j[r][c] + h[r][c + 1] + 1) >> 1; break; // k
            case 12: v = (h[r][c] + G(r + 1, c) + 1) >> 1; break; // n
            case 13: v = (h[r][c] + b[r + 1][c] + 1) >> 1; break; // p
            case 14: v = (j[r][c] + b[r + 1][c] + 1) >> 1; break; // q
            default: v = (h[r][c + 1] + b[r + 1][c] + 1) >> 1; break; // r
            }
            out[r * 4 + c] = v;
        }
}

// 8.4.2.2.2.
int chroma_sample(std::uint8_t const* data, int width, int height, int x, int y, int fx, int fy)
{
    auto at = [&](int xx, int yy) { return data[std::clamp(yy, 0, height - 1) * width + std::clamp(xx, 0, width - 1)]; };
    return ((8 - fx) * (8 - fy) * at(x, y) + fx * (8 - fy) * at(x + 1, y) + (8 - fx) * fy * at(x, y + 1) + fx * fy * at(x + 1, y + 1) + 32) >> 6;
}

struct Weights {
    int mode = 0; // 0 default, 1 explicit, 2 implicit
    int log_wd = 0;
    int w0 = 1, w1 = 1, o0 = 0, o1 = 0;
};

// 8.4.2.3: one sample from the list predictions.
std::uint8_t weigh(int p0, int p1, bool l0, bool l1, Weights const& w)
{
    if (l0 && l1) {
        if (w.mode == 0)
            return static_cast<std::uint8_t>((p0 + p1 + 1) >> 1);
        return clip1(((p0 * w.w0 + p1 * w.w1 + (1 << w.log_wd)) >> (w.log_wd + 1)) + ((w.o0 + w.o1 + 1) >> 1));
    }
    int const p = l0 ? p0 : p1;
    if (w.mode != 1)
        return static_cast<std::uint8_t>(p);
    int const wt = l0 ? w.w0 : w.w1;
    int const o = l0 ? w.o0 : w.o1;
    if (w.log_wd >= 1)
        return clip1(((p * wt + (1 << (w.log_wd - 1))) >> w.log_wd) + o);
    return clip1(p * wt + o);
}

Weights weights_for(Slice const& slice, int ref0, int ref1, bool l0, bool l1, int component)
{
    Weights w;
    w.mode = slice.weighted_mode;
    if (w.mode == 2) {
        if (!(l0 && l1)) {
            w.mode = 0;
            return w;
        }
        std::array<int, 2> const iw = slice.implicit[static_cast<std::size_t>(ref0 * 32 + ref1)];
        w.log_wd = 5;
        w.w0 = iw[0];
        w.w1 = iw[1];
        return w;
    }
    if (w.mode == 1) {
        H264PredWeights const& pw = slice.header->weights;
        w.log_wd = component == 0 ? pw.luma_log2_denom : pw.chroma_log2_denom;
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

void inter_predict(Frame& frame, Slice const& slice, int addr)
{
    Picture& pic = *frame.picture;
    int const mx = addr % pic.mb_width, my = addr / pic.mb_width;
    for (int by = 0; by < 4; ++by)
        for (int bx = 0; bx < 4; ++bx) {
            int const i = pic.block_index(mx * 4 + bx, my * 4 + by);
            int refs[2];
            Mv mvs[2];
            Picture const* ref_pics[2] = { nullptr, nullptr };
            bool use[2];
            for (int list = 0; list < 2; ++list) {
                refs[list] = pic.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
                mvs[list] = pic.mv[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)];
                use[list] = refs[list] >= 0 && static_cast<std::size_t>(refs[list]) < slice.list[static_cast<std::size_t>(list)].size()
                    && slice.list[static_cast<std::size_t>(list)][static_cast<std::size_t>(refs[list])].picture;
                if (use[list])
                    ref_pics[list] = slice.list[static_cast<std::size_t>(list)][static_cast<std::size_t>(refs[list])].picture;
            }
            if (!use[0] && !use[1]) {
                // A reference the buffer does not hold: the block stays as it was (grey on a fresh picture).
                continue;
            }
            int const x0 = mx * 16 + bx * 4, y0 = my * 16 + by * 4;
            Weights const wl = weights_for(slice, refs[0], refs[1], use[0], use[1], 0);
            int pl[2][16] = {};
            for (int list = 0; list < 2; ++list) {
                if (use[list])
                    luma_block(ref_pics[list]->y.data(), ref_pics[list]->width, ref_pics[list]->height, x0 + (mvs[list].x >> 2), y0 + (mvs[list].y >> 2),
                        mvs[list].x & 3, mvs[list].y & 3, pl[list]);
            }
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    pic.y[static_cast<std::size_t>((y0 + y) * pic.width + x0 + x)] = weigh(pl[0][y * 4 + x], pl[1][y * 4 + x], use[0], use[1], wl);
            int const cw = pic.width / 2, ch = pic.height / 2;
            int const cx0 = x0 / 2, cy0 = y0 / 2;
            for (int c = 0; c < 2; ++c) {
                Weights const wc = weights_for(slice, refs[0], refs[1], use[0], use[1], 1 + c);
                std::vector<std::uint8_t>& plane = c == 0 ? pic.cb : pic.cr;
                for (int y = 0; y < 2; ++y)
                    for (int x = 0; x < 2; ++x) {
                        int p[2] = { 0, 0 };
                        for (int list = 0; list < 2; ++list) {
                            if (!use[list])
                                continue;
                            std::vector<std::uint8_t> const& rp = c == 0 ? ref_pics[list]->cb : ref_pics[list]->cr;
                            int const xi = cx0 + x + (mvs[list].x >> 3), yi = cy0 + y + (mvs[list].y >> 3);
                            p[list] = chroma_sample(rp.data(), cw, ch, xi, yi, mvs[list].x & 7, mvs[list].y & 7);
                        }
                        plane[static_cast<std::size_t>((cy0 + y) * cw + cx0 + x)] = weigh(p[0], p[1], use[0], use[1], wc);
                    }
            }
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
        , m_pic(*frame.picture)
        , m_x(addr % frame.picture->mb_width)
        , m_y(addr / frame.picture->mb_width)
    {
    }
    bool mb_ok(int mx, int my) const
    {
        if (mx < 0 || my < 0 || mx >= m_pic.mb_width || my >= m_pic.mb_height)
            return false;
        MbState const& s = m_frame.mbs[static_cast<std::size_t>(my * m_pic.mb_width + mx)];
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
        int const mx = m_x + (x < 0 ? -1 : (x >= 16 ? 1 : 0));
        int const my = m_y + (y < 0 ? -1 : 0);
        return mb_ok(mx, my);
    }
    int luma(int x, int y) const { return m_pic.y[static_cast<std::size_t>((m_y * 16 + y) * m_pic.width + m_x * 16 + x)]; }
    bool chroma_left() const { return mb_ok(m_x - 1, m_y); }
    bool chroma_top() const { return mb_ok(m_x, m_y - 1); }
    bool chroma_corner() const { return mb_ok(m_x - 1, m_y - 1); }
    int chroma(int c, int x, int y) const
    {
        std::vector<std::uint8_t> const& p = c == 0 ? m_pic.cb : m_pic.cr;
        return p[static_cast<std::size_t>((m_y * 8 + y) * (m_pic.width / 2) + m_x * 8 + x)];
    }
    void done(int x4, int y4, int w4, int h4)
    {
        for (int y = y4; y < y4 + h4; ++y)
            for (int x = x4; x < x4 + w4; ++x)
                m_done |= 1u << (y * 4 + x);
    }
    std::uint8_t* luma_ptr(int x, int y) { return &m_pic.y[static_cast<std::size_t>((m_y * 16 + y) * m_pic.width + m_x * 16 + x)]; }
    std::uint8_t* chroma_ptr(int c, int x, int y)
    {
        std::vector<std::uint8_t>& p = c == 0 ? m_pic.cb : m_pic.cr;
        return &p[static_cast<std::size_t>((m_y * 8 + y) * (m_pic.width / 2) + m_x * 8 + x)];
    }

private:
    Frame& m_frame;
    Slice const& m_slice;
    Picture& m_pic;
    int m_x;
    int m_y;
    std::uint32_t m_done = 0;
};

// 8.3.1.2: a 4x4 block's prediction into `out` (raster 4x4).
void predict_4x4(IntraContext const& ic, int xo, int yo, int mode, int* out)
{
    int top[8] = {}, left[4] = {}, corner = 0;
    bool const has_top = ic.luma_available(xo, yo - 1);
    bool const has_left = ic.luma_available(xo - 1, yo);
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
    bool const has_left = ic.luma_available(xo - 1, yo);
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
    bool const has_left = ic.luma_available(-1, 0);
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
    bool const has_left = ic.chroma_left();
    if ((mode == 1 && !has_left) || (mode == 2 && !has_top) || (mode == 3 && !(has_top && has_left && ic.chroma_corner())))
        mode = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            int v = 128;
            if (mode == 0) {
                int const xo = x & ~3, yo = y & ~3;
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

void reconstruct_macroblock(Frame& frame, Slice const& slice, int addr, MbSyntax const& mb)
{
    Picture& pic = *frame.picture;
    MbState& s = frame.mbs[static_cast<std::size_t>(addr)];
    int const mx = addr % pic.mb_width, my = addr / pic.mb_width;
    int const stride = pic.width;
    int const cstride = pic.width / 2;
    std::uint8_t* const y0 = &pic.y[static_cast<std::size_t>(my * 16 * stride + mx * 16)];
    s.qpc[0] = chroma_qp(s.qp, frame.chroma_qp_offset[0]);
    s.qpc[1] = chroma_qp(s.qp, frame.chroma_qp_offset[1]);
    if (s.intra) {
        pic.mb_intra[static_cast<std::size_t>(addr)] = 1;
        for (int by = 0; by < 4; ++by)
            for (int bx = 0; bx < 4; ++bx) {
                int const i = pic.block_index(mx * 4 + bx, my * 4 + by);
                for (int list = 0; list < 2; ++list) {
                    pic.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)] = -1;
                    pic.mv[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)] = {};
                    pic.ref_pic[static_cast<std::size_t>(list)][static_cast<std::size_t>(i)] = -1;
                }
            }
    }
    if (mb.kind == MbKind::IPCM) {
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                y0[y * stride + x] = mb.pcm_luma[static_cast<std::size_t>(y * 16 + x)];
        for (int c = 0; c < 2; ++c) {
            std::vector<std::uint8_t>& plane = c == 0 ? pic.cb : pic.cr;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    plane[static_cast<std::size_t>((my * 8 + y) * cstride + mx * 8 + x)] = mb.pcm_chroma[static_cast<std::size_t>(c * 64 + y * 8 + x)];
        }
        return;
    }
    LevelScales const& ls = *slice.scales;
    bool const inter = !s.intra;
    int const qp = s.qp;
    std::array<std::int32_t, 64> d {};
    IntraContext ic(frame, slice, addr);
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
        std::uint8_t* const c0 = &(comp == 0 ? pic.cb : pic.cr)[static_cast<std::size_t>(my * 8 * cstride + mx * 8)];
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
