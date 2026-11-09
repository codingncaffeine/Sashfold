// 8.7: the deblocking filter, over a whole decoded picture, macroblock by
// macroblock in raster order (vertical edges, then horizontal), in place.
#include "media/H264SoftwareInternal.h"

#include <algorithm>
#include <cstdlib>

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

std::uint8_t clip1(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }

// 8.7.2.1: bS between two 4x4 luma blocks (global block indices).
int strength(Frame const& f, int mb_p, int mb_q, int bp, int bq, bool mb_edge)
{
    MbState const& p = f.mbs[static_cast<std::size_t>(mb_p)];
    MbState const& q = f.mbs[static_cast<std::size_t>(mb_q)];
    if (p.intra || q.intra)
        return mb_edge ? 4 : 3;
    Picture const& pic = *f.picture;
    int const lp = (bp / (pic.mb_width * 4)) % 4 * 4 + bp % 4; // raster index inside its macroblock
    int const lq = (bq / (pic.mb_width * 4)) % 4 * 4 + bq % 4;
    if (((p.nonzero >> lp) & 1) || ((q.nonzero >> lq) & 1))
        return 2;
    auto pic_of = [&](int list, int b) { return pic.ref_idx[static_cast<std::size_t>(list)][static_cast<std::size_t>(b)] >= 0 ? pic.ref_pic[static_cast<std::size_t>(list)][static_cast<std::size_t>(b)] : -2; };
    std::int32_t const p0 = pic_of(0, bp), p1 = pic_of(1, bp), q0 = pic_of(0, bq), q1 = pic_of(1, bq);
    int const np = (p0 != -2) + (p1 != -2), nq = (q0 != -2) + (q1 != -2);
    if (np != nq)
        return 1;
    auto far = [&](int lp_, int lq_) {
        Mv const a = pic.mv[static_cast<std::size_t>(lp_)][static_cast<std::size_t>(bp)];
        Mv const b = pic.mv[static_cast<std::size_t>(lq_)][static_cast<std::size_t>(bq)];
        return std::abs(a.x - b.x) >= 4 || std::abs(a.y - b.y) >= 4;
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
// pos - (i + 1) * step, q[i] at pos + i * step.
void filter_line(std::uint8_t* pos, int step, int bs, int alpha, int beta, int index_a, bool chroma)
{
    int const p0 = pos[-step], p1 = pos[-2 * step], q0 = pos[0], q1 = pos[step];
    if (!(std::abs(p0 - q0) < alpha && std::abs(p1 - p0) < beta && std::abs(q1 - q0) < beta))
        return;
    if (bs < 4) {
        int const tc0 = tc0_table[bs - 1][index_a];
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
                pos[-2 * step] = static_cast<std::uint8_t>(p1 + std::clamp((p2 + ((p0 + q0 + 1) >> 1) - (p1 * 2)) >> 1, -tc0, tc0));
            if (aq < beta)
                pos[step] = static_cast<std::uint8_t>(q1 + std::clamp((q2 + ((p0 + q0 + 1) >> 1) - (q1 * 2)) >> 1, -tc0, tc0));
        }
        return;
    }
    if (chroma) {
        pos[-step] = static_cast<std::uint8_t>((2 * p1 + p0 + q1 + 2) >> 2);
        pos[0] = static_cast<std::uint8_t>((2 * q1 + q0 + p1 + 2) >> 2);
        return;
    }
    int const p2 = pos[-3 * step], q2 = pos[2 * step];
    int const p3 = pos[-4 * step], q3 = pos[3 * step];
    int const ap = std::abs(p2 - p0), aq = std::abs(q2 - q0);
    bool const strong = std::abs(p0 - q0) < ((alpha >> 2) + 2);
    if (ap < beta && strong) {
        pos[-step] = static_cast<std::uint8_t>((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
        pos[-2 * step] = static_cast<std::uint8_t>((p2 + p1 + p0 + q0 + 2) >> 2);
        pos[-3 * step] = static_cast<std::uint8_t>((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
    } else {
        pos[-step] = static_cast<std::uint8_t>((2 * p1 + p0 + q1 + 2) >> 2);
    }
    if (aq < beta && strong) {
        pos[0] = static_cast<std::uint8_t>((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
        pos[step] = static_cast<std::uint8_t>((p0 + q0 + q1 + q2 + 2) >> 2);
        pos[2 * step] = static_cast<std::uint8_t>((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
    } else {
        pos[0] = static_cast<std::uint8_t>((2 * q1 + q0 + p1 + 2) >> 2);
    }
}

} // namespace

void deblock_picture(Frame& frame)
{
    Picture& pic = *frame.picture;
    int const w = pic.mb_width, h = pic.mb_height;
    int const stride = pic.width, cstride = pic.width / 2;
    int const bw = w * 4;
    for (int my = 0; my < h; ++my)
        for (int mx = 0; mx < w; ++mx) {
            int const addr = my * w + mx;
            MbState const& q = frame.mbs[static_cast<std::size_t>(addr)];
            if (q.slice < 0)
                continue;
            int const idc = frame.slice_disable[static_cast<std::size_t>(q.slice)];
            if (idc == 1)
                continue;
            int const off_a = frame.slice_alpha[static_cast<std::size_t>(q.slice)];
            int const off_b = frame.slice_beta[static_cast<std::size_t>(q.slice)];
            bool const left = mx > 0 && frame.mbs[static_cast<std::size_t>(addr - 1)].slice >= 0
                && (idc != 2 || frame.mbs[static_cast<std::size_t>(addr - 1)].slice == q.slice);
            bool const top = my > 0 && frame.mbs[static_cast<std::size_t>(addr - w)].slice >= 0
                && (idc != 2 || frame.mbs[static_cast<std::size_t>(addr - w)].slice == q.slice);
            for (int dir = 0; dir < 2; ++dir) { // 0: vertical edges, 1: horizontal
                int bs[4][4]; // [edge][block along it]
                for (int e = 0; e < 4; ++e) {
                    for (int k = 0; k < 4; ++k)
                        bs[e][k] = 0;
                    bool const mb_edge = e == 0;
                    if (mb_edge && !(dir == 0 ? left : top))
                        continue;
                    if (!mb_edge && q.transform_8x8 && (e & 1))
                        continue;
                    int const mb_p = mb_edge ? (dir == 0 ? addr - 1 : addr - w) : addr;
                    for (int k = 0; k < 4; ++k) {
                        int const qx = mx * 4 + (dir == 0 ? e : k), qy = my * 4 + (dir == 0 ? k : e);
                        int const px = dir == 0 ? qx - 1 : qx, py = dir == 0 ? qy : qy - 1;
                        bs[e][k] = strength(frame, mb_p, addr, py * bw + px, qy * bw + qx, mb_edge);
                    }
                }
                // Luma.
                for (int e = 0; e < 4; ++e) {
                    bool const mb_edge = e == 0;
                    if (mb_edge && !(dir == 0 ? left : top))
                        continue;
                    MbState const& p = mb_edge ? frame.mbs[static_cast<std::size_t>(dir == 0 ? addr - 1 : addr - w)] : q;
                    int const qp_av = (p.qp + q.qp + 1) >> 1;
                    int const index_a = std::clamp(qp_av + off_a, 0, 51);
                    int const index_b = std::clamp(qp_av + off_b, 0, 51);
                    int const alpha = alpha_table[index_a], beta = beta_table[index_b];
                    for (int i = 0; i < 16; ++i) {
                        int const b = bs[e][i >> 2];
                        if (b == 0)
                            continue;
                        std::uint8_t* pos = dir == 0 ? &pic.y[static_cast<std::size_t>((my * 16 + i) * stride + mx * 16 + e * 4)]
                                                     : &pic.y[static_cast<std::size_t>((my * 16 + e * 4) * stride + mx * 16 + i)];
                        filter_line(pos, dir == 0 ? 1 : stride, b, alpha, beta, index_a, false);
                    }
                }
                // Chroma: edges 0 and 4 of each 8x8, their bS from luma edges 0 and 2.
                for (int c = 0; c < 2; ++c) {
                    std::vector<std::uint8_t>& plane = c == 0 ? pic.cb : pic.cr;
                    for (int ce = 0; ce < 2; ++ce) {
                        int const e = ce * 2;
                        bool const mb_edge = e == 0;
                        if (mb_edge && !(dir == 0 ? left : top))
                            continue;
                        MbState const& p = mb_edge ? frame.mbs[static_cast<std::size_t>(dir == 0 ? addr - 1 : addr - w)] : q;
                        int const qp_av = (p.qpc[static_cast<std::size_t>(c)] + q.qpc[static_cast<std::size_t>(c)] + 1) >> 1;
                        int const index_a = std::clamp(qp_av + off_a, 0, 51);
                        int const index_b = std::clamp(qp_av + off_b, 0, 51);
                        int const alpha = alpha_table[index_a], beta = beta_table[index_b];
                        for (int i = 0; i < 8; ++i) {
                            int const b = bs[e][i >> 1];
                            if (b == 0)
                                continue;
                            std::uint8_t* pos = dir == 0 ? &plane[static_cast<std::size_t>((my * 8 + i) * cstride + mx * 8 + ce * 4)]
                                                         : &plane[static_cast<std::size_t>((my * 8 + ce * 4) * cstride + mx * 8 + i)];
                            filter_line(pos, dir == 0 ? 1 : cstride, b, alpha, beta, index_a, true);
                        }
                    }
                }
            }
        }
}

}
