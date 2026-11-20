// 8.7: the deblocking filter, over a whole decoded picture, macroblock by
// macroblock in raster order (vertical edges, then horizontal), in place.
#include "media/H264SoftwareInternal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <tuple>
#include <utility>
#include <vector>

namespace sashfold::media::h264sw {

namespace {

// Table 8-16.
constexpr std::uint8_t alpha_table[52] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 5, 6, 7, 8, 9, 10, 12, 13,
    15, 17, 20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 71, 80, 90, 101, 113, 127, 144, 162, 182, 203, 226, 255, 255 };
constexpr std::uint8_t beta_table[52] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18 };
// Table 8-17, [bS - 1][indexA].
constexpr std::uint8_t tc0_table[3][52] = {
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11, 13 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 7, 8, 8, 10, 11, 12, 13, 15, 17 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 23, 25 },
};

// alpha and beta (8-456 to 8-459) for indexA and indexB at a component's
// bit depth.
struct Thresholds {
    int alpha = 0;
    int beta = 0;
    int index_a = 0;
};
inline Thresholds thresholds_at(int qp_av, int off_a, int off_b, int bd_shift)
{
    Thresholds t;
    t.index_a = std::clamp(qp_av + off_a, 0, 51);
    t.alpha = alpha_table[t.index_a] * (1 << bd_shift);
    t.beta = beta_table[std::clamp(qp_av + off_b, 0, 51)] * (1 << bd_shift);
    return t;
}

// 8.7.2.1: bS between two 4x4 luma blocks (indices in the picture's
// motion arrays). lp, lq: the blocks' raster indices inside their
// macroblocks. An intra macroblock edge is 4 between frame macroblocks and
// across a vertical edge, 3 across a horizontal one between field
// macroblocks; motion differs at a quarter frame sample apart, which
// vertically is half that for field macroblocks (`mvy_limit` 2).
[[gnu::always_inline]] inline int strength(Frame const& f, int mb_p, int mb_q, int bp, int bq, int lp, int lq, bool mb_edge, bool vertical_edge, int mvy_limit,
    bool mixed = false)
{
    MbState const& p = f.mbs[static_cast<std::size_t>(mb_p)];
    MbState const& q = f.mbs[static_cast<std::size_t>(mb_q)];
    if (p.intra || q.intra)
        return mb_edge && (vertical_edge || (!p.field && !q.field)) ? 4 : 3;
    Picture const& pic = *f.picture;
    if (((p.nonzero >> lp) & 1) || ((q.nonzero >> lq) & 1))
        return 2;
    // An MBAFF edge between a frame pair and a field pair (mixedModeEdgeFlag).
    if (mixed)
        return 1;
    auto pic_of = [&](int list, int b) { return pic.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(b)] >= 0 ? pic.ref_pic[static_cast<std::size_t>(list)][static_cast<std::size_t>(b)] : -2; };
    std::int32_t const p0 = pic_of(0, bp), p1 = pic_of(1, bp), q0 = pic_of(0, bq), q1 = pic_of(1, bq);
    int const np = (p0 != -2) + (p1 != -2), nq = (q0 != -2) + (q1 != -2);
    if (np != nq)
        return 1;
    auto far = [&](int lp_, int lq_) {
        Mv const a = pic.mv[static_cast<std::size_t>(lp_)][static_cast<std::size_t>(bp)];
        Mv const b = pic.mv[static_cast<std::size_t>(lq_)][static_cast<std::size_t>(bq)];
        return std::abs(a.x - b.x) >= 4 || std::abs(a.y - b.y) >= mvy_limit;
    };
    if (np == 1) {
        int const listp = p0 != -2 ? 0 : 1, listq = q0 != -2 ? 0 : 1;
        if ((listp == 0 ? p0 : p1) != (listq == 0 ? q0 : q1))
            return 1;
        return far(listp, listq) ? 1 : 0;
    }
    if (np == 0)
        return 0;
    if (!((p0 == q0 && p1 == q1) || (p0 == q1 && p1 == q0)))
        return 1;
    if (p0 != p1) {
        if (p0 == q0)
            return (far(0, 0) || far(1, 1)) ? 1 : 0;
        return (far(0, 1) || far(1, 0)) ? 1 : 0;
    }
    return ((far(0, 0) || far(1, 1)) && (far(0, 1) || far(1, 0))) ? 1 : 0;
}

// 8.7.2.3 and 8.7.2.4 on one line of samples across an edge: p[i] at
// pos - (i + 1) * step, q[i] at pos + i * step. `chroma`:
// chromaStyleFilteringFlag (8-450); `bd_shift` and `max`: the component's
// bit depth less 8, and its largest sample (both fixed for bytes).
template<typename P>
void filter_line(P* pos, int step, int bs, int alpha, int beta, int index_a, bool chroma, int bd_shift, int max)
{
    auto clip1 = [max](int v) { return static_cast<P>(clip_sample<P>(v, max)); };
    int const p0 = pos[-step], p1 = pos[-2 * step], q0 = pos[0], q1 = pos[step];
    if (!(std::abs(p0 - q0) < alpha && std::abs(p1 - p0) < beta && std::abs(q1 - q0) < beta))
        return;
    if (bs < 4) {
        // 8-461, 8-462.
        int const tc0 = sizeof(P) == 1 ? tc0_table[bs - 1][index_a] : tc0_table[bs - 1][index_a] * (1 << bd_shift);
        int tc;
        int ap = 0, aq = 0;
        int p2 = 0, q2 = 0;
        if (chroma) {
            tc = tc0 + 1;
        } else {
            p2 = pos[-3 * step];
            q2 = pos[2 * step];
            ap = std::abs(p2 - p0);
            aq = std::abs(q2 - q0);
            tc = tc0 + (ap < beta ? 1 : 0) + (aq < beta ? 1 : 0);
        }
        int const delta = std::clamp((((q0 - p0) * 4) + (p1 - q1) + 4) >> 3, -tc, tc);
        pos[-step] = clip1(p0 + delta);
        pos[0] = clip1(q0 - delta);
        if (!chroma) {
            if (ap < beta)
                pos[-2 * step] = static_cast<P>(p1 + std::clamp((p2 + ((p0 + q0 + 1) >> 1) - (p1 * 2)) >> 1, -tc0, tc0));
            if (aq < beta)
                pos[step] = static_cast<P>(q1 + std::clamp((q2 + ((p0 + q0 + 1) >> 1) - (q1 * 2)) >> 1, -tc0, tc0));
        }
        return;
    }
    if (chroma) {
        pos[-step] = static_cast<P>((2 * p1 + p0 + q1 + 2) >> 2);
        pos[0] = static_cast<P>((2 * q1 + q0 + p1 + 2) >> 2);
        return;
    }
    int const p2 = pos[-3 * step], q2 = pos[2 * step];
    int const p3 = pos[-4 * step], q3 = pos[3 * step];
    int const ap = std::abs(p2 - p0), aq = std::abs(q2 - q0);
    bool const strong = std::abs(p0 - q0) < ((alpha >> 2) + 2);
    if (ap < beta && strong) {
        pos[-step] = static_cast<P>((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
        pos[-2 * step] = static_cast<P>((p2 + p1 + p0 + q0 + 2) >> 2);
        pos[-3 * step] = static_cast<P>((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
    } else {
        pos[-step] = static_cast<P>((2 * p1 + p0 + q1 + 2) >> 2);
    }
    if (aq < beta && strong) {
        pos[0] = static_cast<P>((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
        pos[step] = static_cast<P>((p0 + q0 + q1 + q2 + 2) >> 2);
        pos[2 * step] = static_cast<P>((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
    } else {
        pos[0] = static_cast<P>((2 * q1 + q0 + p1 + 2) >> 2);
    }
}

template<typename P, int Format>
void deblock_mb(Frame& frame, int mx, int my)
{
    int const w = frame.mb_width;
    int const stride = frame.luma_stride, cstride = frame.chroma_stride;
    int const shift_y = frame.bit_depth_luma - 8, shift_c = frame.bit_depth_chroma - 8;
    // The chroma format's geometry, fixed per instantiation (Table 6-1).
    constexpr int format = Format;
    constexpr int chroma_shift_x = Format == 3 ? 0 : 1, chroma_shift_y = Format == 1 ? 1 : 0;
    constexpr int cw = Format == 0 ? 0 : 16 >> chroma_shift_x, ch = Format == 0 ? 0 : 16 >> chroma_shift_y;
    P* const luma = frame.luma_at<P>();
    int const bw = frame.block_stride();
    int const addr = my * w + mx;
    int const mvy_limit = frame.structure != 0 ? 2 : 4;
    MbState const& q = frame.mbs[static_cast<std::size_t>(addr)];
    if (q.slice < 0)
        return;
    int const idc = frame.slice_disable[static_cast<std::size_t>(q.slice)];
    if (idc == 1)
        return;
    int const off_a = frame.slice_alpha[static_cast<std::size_t>(q.slice)];
    int const off_b = frame.slice_beta[static_cast<std::size_t>(q.slice)];
    bool const left = mx > 0 && frame.mbs[static_cast<std::size_t>(addr - 1)].slice >= 0
        && (idc != 2 || frame.mbs[static_cast<std::size_t>(addr - 1)].slice == q.slice);
    bool const top = my > 0 && frame.mbs[static_cast<std::size_t>(addr - w)].slice >= 0
        && (idc != 2 || frame.mbs[static_cast<std::size_t>(addr - w)].slice == q.slice);
    int const base_q = frame.block_base_at(mx, my);
    for (int dir = 0; dir < 2; ++dir) { // 0: vertical edges, 1: horizontal
        int bs[4][4]; // [edge][block along it]
        // 4:2:2's horizontal chroma edges 4 and 12 take the strength of luma
        // rows 4 and 12, filtered or not (8.7.2).
        bool const all_edges = format == 2 && dir == 1;
        for (int e = 0; e < 4; ++e) {
            for (int k = 0; k < 4; ++k)
                bs[e][k] = 0;
            bool const mb_edge = e == 0;
            if (mb_edge && !(dir == 0 ? left : top))
                continue;
            if (!mb_edge && q.transform_8x8 && (e & 1) && !all_edges)
                continue;
            int const mb_p = mb_edge ? (dir == 0 ? addr - 1 : addr - w) : addr;
            int const base_p = mb_edge ? (dir == 0 ? base_q - 4 : base_q - frame.block_row_step()) : base_q;
            for (int k = 0; k < 4; ++k) {
                // Blocks within their macroblocks: q's, and p's across the edge.
                int const qx = dir == 0 ? e : k, qy = dir == 0 ? k : e;
                int const px = dir == 0 ? (qx + 3) & 3 : qx, py = dir == 0 ? qy : (qy + 3) & 3;
                bs[e][k] = strength(frame, mb_p, addr, base_p + py * bw + px, base_q + qy * bw + qx, py * 4 + px, qy * 4 + qx, mb_edge, dir == 0, mvy_limit);
            }
        }
        // Luma.
        for (int e = 0; e < 4; ++e) {
            bool const mb_edge = e == 0;
            if (mb_edge && !(dir == 0 ? left : top))
                continue;
            if (!mb_edge && q.transform_8x8 && (e & 1))
                continue;
            MbState const& p = mb_edge ? frame.mbs[static_cast<std::size_t>(dir == 0 ? addr - 1 : addr - w)] : q;
            Thresholds const t = thresholds_at((p.qp + q.qp + 1) >> 1, off_a, off_b, shift_y);
            // 8-460's test fails for every sample where alpha or beta is 0.
            if (t.alpha == 0 || t.beta == 0 || (bs[e][0] | bs[e][1] | bs[e][2] | bs[e][3]) == 0)
                continue;
            for (int i = 0; i < 16; ++i) {
                int const b = bs[e][i >> 2];
                if (b == 0)
                    continue;
                P* pos = dir == 0 ? luma + static_cast<std::ptrdiff_t>(my * 16 + i) * stride + mx * 16 + e * 4
                                  : luma + static_cast<std::ptrdiff_t>(my * 16 + e * 4) * stride + mx * 16 + i;
                filter_line(pos, dir == 0 ? 1 : stride, b, t.alpha, t.beta, t.index_a, false, shift_y, frame.luma_max);
            }
        }
        if constexpr (format == 0)
            continue;
        // Chroma (8.7, step 3e): the edges of its 4x4 blocks (4:4:4: those
        // of its transform, as luma's, filtered as luma is), each with the
        // strength of the luma edge through its luma sample (SubWidthC * x,
        // SubHeightC * y): 4:2:0 edges 0 and 4 from luma's 0 and 2, 4:2:2's
        // vertical alike and its horizontal 0, 4, 8, 12 from luma's 0 to 3.
        int const along = dir == 0 ? ch : cw; // samples along an edge
        int const edges = (dir == 0 ? cw : ch) / 4;
        for (int c = 0; c < 2; ++c) {
            P* const plane = frame.chroma_at<P>(c);
            for (int ce = 0; ce < edges; ++ce) {
                int const e = ce << (dir == 0 ? chroma_shift_x : chroma_shift_y);
                bool const mb_edge = ce == 0;
                if (mb_edge && !(dir == 0 ? left : top))
                    continue;
                if (format == 3 && !mb_edge && q.transform_8x8 && (ce & 1))
                    continue;
                MbState const& p = mb_edge ? frame.mbs[static_cast<std::size_t>(dir == 0 ? addr - 1 : addr - w)] : q;
                Thresholds const t = thresholds_at((p.qpc[static_cast<std::size_t>(c)] + q.qpc[static_cast<std::size_t>(c)] + 1) >> 1, off_a, off_b, shift_c);
                if (t.alpha == 0 || t.beta == 0 || (bs[e][0] | bs[e][1] | bs[e][2] | bs[e][3]) == 0)
                    continue;
                // The luma block along the edge holding chroma sample i.
                int const sub = dir == 0 ? chroma_shift_y : chroma_shift_x;
                for (int i = 0; i < along; ++i) {
                    int const b = bs[e][(i << sub) >> 2];
                    if (b == 0)
                        continue;
                    P* pos = dir == 0 ? plane + static_cast<std::ptrdiff_t>(my * ch + i) * cstride + mx * cw + ce * 4
                                      : plane + static_cast<std::ptrdiff_t>(my * ch + ce * 4) * cstride + mx * cw + i;
                    filter_line(pos, dir == 0 ? 1 : cstride, b, t.alpha, t.beta, t.index_a, format != 3, shift_c, frame.chroma_max);
                }
            }
        }
    }
}

template<typename P>
void deblock_mb_of(Frame& frame, int mx, int my)
{
    switch (frame.chroma_format) {
    case 0:
        deblock_mb<P, 0>(frame, mx, my);
        break;
    case 2:
        deblock_mb<P, 2>(frame, mx, my);
        break;
    case 3:
        deblock_mb<P, 3>(frame, mx, my);
        break;
    default:
        deblock_mb<P, 1>(frame, mx, my);
        break;
    }
}

} // namespace

void deblock_macroblock(Frame& frame, int mx, int my)
{
    if (frame.wide)
        deblock_mb_of<std::uint16_t>(frame, mx, my);
    else
        deblock_mb_of<std::uint8_t>(frame, mx, my);
}

namespace {

// The macroblock of MBAFF pair column `px` holding frame row `row` (luma,
// 32 rows to a pair, or chroma, `rows` to a pair), and the row inside it.
std::pair<int, int> mbaff_mb_at(Frame const& f, int px, int row, int rows)
{
    int const pair = (row / rows) * f.mb_width + px, r = row % rows;
    int const top = 2 * pair;
    if (f.mbs[static_cast<std::size_t>(top)].field)
        return { top + (r & 1), r >> 1 };
    return { top + r / (rows / 2), r % (rows / 2) };
}

// 8.7 for one macroblock of an MBAFF frame, in the frame's own rows: a
// field macroblock's edges run along its field's rows (8.7.1's dy of 2),
// a vertical edge meets whichever macroblock of the pair beside holds the
// row, so its strength is taken line by line (8.7.2.1, mixedModeEdgeFlag),
// and a frame macroblock under a field pair filters its top edge twice,
// once against each field (8.7, step 3c).
template<typename P>
void deblock_mbaff(Frame& frame, int addr)
{
    Picture& pic = *frame.picture;
    P* const luma = pic.plane<P>(0);
    int const shift_y = frame.bit_depth_luma - 8, shift_c = frame.bit_depth_chroma - 8;
    MbState const& q = frame.mbs[static_cast<std::size_t>(addr)];
    if (q.slice < 0)
        return;
    int const idc = frame.slice_disable[static_cast<std::size_t>(q.slice)];
    if (idc == 1)
        return;
    int const off_a = frame.slice_alpha[static_cast<std::size_t>(q.slice)];
    int const off_b = frame.slice_beta[static_cast<std::size_t>(q.slice)];
    int const w = frame.mb_width, bw = frame.block_stride();
    int const stride = pic.width, cstride = pic.chroma_width;
    int const pair = addr >> 1, px = pair % w, py = pair / w, bottom = addr & 1;
    bool const field = q.field;
    int const dy = field ? 2 : 1;
    int const x_i = 16 * px, y_i = field ? 32 * py + bottom : 32 * py + 16 * bottom;
    // The chroma: 4:2:0's rows are half the luma's, 4:2:2's and 4:4:4's as
    // many; 4:4:4's chroma is filtered as luma is (8-450).
    int const format = frame.chroma_format;
    int const y_p = format == 1 ? (y_i + 1) / 2 : y_i;
    int const chroma_rows = frame.mb_chroma_height, chroma_pair_rows = 2 * chroma_rows;
    int const mvy_limit = field ? 2 : 4;
    // 8.7, step 2: which macroblock edges are filtered.
    bool const left = px > 0 && (idc != 2 || frame.neighbor(addr, -1, 0, 16, 16, q.slice).addr >= 0);
    bool const top = !(py == 0 && (field || bottom == 0)) && (idc != 2 || frame.neighbor(addr, 0, -1, 16, 16, q.slice).addr >= 0);
    int const base_q = frame.block_base(addr);
    auto block_of = [&](int mb, int x4, int y4) { return frame.block_base(mb) + y4 * bw + x4; };
    auto mixed = [&](int mb_p) { return (mb_p >> 1) != pair && frame.mbs[static_cast<std::size_t>(mb_p)].field != field; };
    auto thresholds = [&](int qp_av, int& alpha, int& beta, int& index_a, bool chroma) {
        Thresholds const t = thresholds_at(qp_av, off_a, off_b, chroma ? shift_c : shift_y);
        index_a = t.index_a;
        alpha = t.alpha;
        beta = t.beta;
    };

    // Vertical edges: luma lines k = 0..15 each with its own p macroblock
    // and strength; chroma lines take the strength of the luma line of
    // their field (8.7.2).
    for (int e = 0; e < 4; ++e) {
        if (e == 0 ? !left : (q.transform_8x8 && (e & 1)))
            continue;
        std::array<int, 16> bs {};
        std::array<int, 16> mb_p {};
        for (int k = 0; k < 16; ++k) {
            int row_p = k;
            mb_p[static_cast<std::size_t>(k)] = addr;
            if (e == 0)
                std::tie(mb_p[static_cast<std::size_t>(k)], row_p) = mbaff_mb_at(frame, px - 1, y_i + dy * k, 32);
            int const p_mb = mb_p[static_cast<std::size_t>(k)];
            int const px4 = e == 0 ? 3 : e - 1;
            bs[static_cast<std::size_t>(k)] = strength(frame, p_mb, addr, block_of(p_mb, px4, row_p >> 2), base_q + (k >> 2) * bw + e, (row_p >> 2) * 4 + px4,
                (k >> 2) * 4 + e, e == 0, true, mvy_limit, e == 0 && mixed(p_mb));
        }
        for (int k = 0; k < 16; ++k) {
            int const b = bs[static_cast<std::size_t>(k)];
            if (b == 0)
                continue;
            MbState const& p = frame.mbs[static_cast<std::size_t>(mb_p[static_cast<std::size_t>(k)])];
            int alpha, beta, index_a;
            thresholds((p.qp + q.qp + 1) >> 1, alpha, beta, index_a, false);
            if (alpha != 0 && beta != 0)
                filter_line(&luma[static_cast<std::size_t>((y_i + dy * k) * stride + x_i + 4 * e)], 1, b, alpha, beta, index_a, false, shift_y, frame.luma_max);
        }
        // Chroma: 4:2:0's and 4:2:2's vertical edges at chroma columns 0
        // and 4 (luma's 0 and 2), 4:4:4's at luma's.
        if (format == 0 || (format != 3 && (e & 1)))
            continue;
        for (int c = 0; c < 2; ++c) {
            P* const plane = pic.plane<P>(1 + c);
            for (int k = 0; k < chroma_rows; ++k) {
                // The luma line of the chroma line's own field (8.7.2):
                // 4:2:0 halves the rows of a field.
                int const luma_line = format != 1 ? k : (field ? 2 * k : 4 * (k >> 1) + (k & 1));
                int const b = bs[static_cast<std::size_t>(luma_line)];
                if (b == 0)
                    continue;
                int const row = y_p + dy * k;
                int const p_mb = e == 0 ? mbaff_mb_at(frame, px - 1, row, chroma_pair_rows).first : addr;
                MbState const& p = frame.mbs[static_cast<std::size_t>(p_mb)];
                int alpha, beta, index_a;
                thresholds((p.qpc[static_cast<std::size_t>(c)] + q.qpc[static_cast<std::size_t>(c)] + 1) >> 1, alpha, beta, index_a, true);
                int const x = format == 3 ? x_i + 4 * e : x_i / 2 + 2 * e;
                if (alpha != 0 && beta != 0)
                    filter_line(&plane[static_cast<std::size_t>(row * cstride + x)], 1, b, alpha, beta, index_a, format != 3, shift_c, frame.chroma_max);
            }
        }
    }

    // Horizontal edges: each a row of luma (and chroma) samples `step`
    // apart vertically from q0 at row `q_row`, against macroblock `mb_p`.
    // `q_crow` -1: no chroma edge here; `luma_edge` false: a chroma edge
    // alone (4:2:2's rows 4 and 12 under the 8x8 transform).
    auto horizontal = [&](int e, int q_row, int q_crow, int step, int mb_p, int p_row, bool mb_edge, bool mixed_edge, bool luma_edge = true) {
        std::array<int, 4> bs {};
        for (int k = 0; k < 4; ++k)
            bs[static_cast<std::size_t>(k)] = strength(frame, mb_p, addr, block_of(mb_p, k, p_row >> 2), base_q + e * bw + k, (p_row >> 2) * 4 + k, e * 4 + k,
                mb_edge, false, mvy_limit, mixed_edge);
        if ((bs[0] | bs[1] | bs[2] | bs[3]) == 0)
            return;
        MbState const& p = frame.mbs[static_cast<std::size_t>(mb_p)];
        int alpha, beta, index_a;
        thresholds((p.qp + q.qp + 1) >> 1, alpha, beta, index_a, false);
        if (luma_edge && alpha != 0 && beta != 0) {
            for (int i = 0; i < 16; ++i) {
                int const b = bs[static_cast<std::size_t>(i >> 2)];
                if (b != 0)
                    filter_line(&luma[static_cast<std::size_t>(q_row * stride + x_i + i)], step * stride, b, alpha, beta, index_a, false, shift_y, frame.luma_max);
            }
        }
        if (q_crow < 0)
            return;
        for (int c = 0; c < 2; ++c) {
            P* const plane = pic.plane<P>(1 + c);
            thresholds((p.qpc[static_cast<std::size_t>(c)] + q.qpc[static_cast<std::size_t>(c)] + 1) >> 1, alpha, beta, index_a, true);
            if (alpha == 0 || beta == 0)
                continue;
            // The chroma samples along the edge: 4:4:4's as luma's, the others' half as many.
            int const across = format == 3 ? 16 : 8, half = format == 3 ? 2 : 1;
            int const x = format == 3 ? x_i : x_i / 2;
            for (int i = 0; i < across; ++i) {
                int const b = bs[static_cast<std::size_t>(i >> half)];
                if (b != 0)
                    filter_line(&plane[static_cast<std::size_t>(q_crow * cstride + x + i)], step * cstride, b, alpha, beta, index_a, format != 3, shift_c,
                        frame.chroma_max);
            }
        }
    };
    int const top_crow = format == 0 ? -1 : y_p;
    if (top) {
        int const above = 2 * (pair - w);
        if (!field && bottom == 0 && frame.mbs[static_cast<std::size_t>(above)].field) {
            // A frame macroblock under a field pair: its rows of each parity against that field's macroblock.
            for (int parity = 0; parity < 2; ++parity)
                horizontal(0, y_i + parity, top_crow < 0 ? -1 : top_crow + parity, 2, above + parity, 15, true, true);
        } else {
            auto const [p_mb, p_row] = mbaff_mb_at(frame, px, y_i - dy, 32);
            horizontal(0, y_i, top_crow, dy, p_mb, p_row, true, mixed(p_mb));
        }
    }
    for (int e = 1; e < 4; ++e) {
        bool const luma_edge = !(q.transform_8x8 && (e & 1));
        // The chroma edge of luma edge e: 4:2:0's at chroma row 4 (e 2),
        // 4:2:2's at rows 4, 8 and 12 (each e, the 8x8 transform or not),
        // 4:4:4's as luma's.
        int crow = -1;
        if (format == 1 && e == 2)
            crow = y_p + dy * 4;
        else if (format == 2 || (format == 3 && luma_edge))
            crow = y_p + dy * 4 * e;
        if (!luma_edge) {
            if (crow >= 0)
                horizontal(e, y_i + dy * 4 * e, crow, dy, addr, 4 * e - 1, false, false, false);
            continue;
        }
        horizontal(e, y_i + dy * 4 * e, crow, dy, addr, 4 * e - 1, false, false);
    }
}

}

void deblock_pair(Frame& frame, int pair)
{
    if (frame.wide) {
        deblock_mbaff<std::uint16_t>(frame, 2 * pair);
        deblock_mbaff<std::uint16_t>(frame, 2 * pair + 1);
    } else {
        deblock_mbaff<std::uint8_t>(frame, 2 * pair);
        deblock_mbaff<std::uint8_t>(frame, 2 * pair + 1);
    }
}

void deblock_picture(Frame& frame)
{
    if (frame.mbaff) {
        for (int pair = 0; pair < static_cast<int>(frame.mbs.size()) / 2; ++pair)
            deblock_pair(frame, pair);
        return;
    }
    for (int my = 0; my < frame.mb_height; ++my)
        for (int mx = 0; mx < frame.mb_width; ++mx)
            deblock_macroblock(frame, mx, my);
}
}
