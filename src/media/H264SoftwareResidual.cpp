// 8.5: the transform coefficients scaled (with the scaling matrices in
// force) and inverse transformed, and the residual added to a block's
// prediction.
#include "media/H264SoftwareResidual.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace sashfold::media::h264sw {

namespace {

// 8-315 and 8-318.
constexpr int v4[6][3] = { { 10, 16, 13 }, { 11, 18, 14 }, { 13, 20, 16 }, { 14, 23, 18 }, { 16, 25, 20 }, { 18, 29, 23 } };
constexpr int v8[6][6] = {
    { 20, 18, 32, 19, 25, 24 },
    { 22, 19, 35, 21, 28, 26 },
    { 26, 23, 42, 24, 33, 31 },
    { 28, 25, 45, 26, 35, 33 },
    { 32, 28, 51, 30, 40, 38 },
    { 36, 32, 58, 34, 46, 43 },
};

constexpr std::uint8_t zigzag4x4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
constexpr std::uint8_t zigzag8x8[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45,
    38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

int norm4(int m, int i, int j)
{
    if ((i & 1) == 0 && (j & 1) == 0)
        return v4[m][0];
    if ((i & 1) == 1 && (j & 1) == 1)
        return v4[m][1];
    return v4[m][2];
}

int norm8(int m, int i, int j)
{
    if (i % 4 == 0 && j % 4 == 0)
        return v8[m][0];
    if (i % 2 == 1 && j % 2 == 1)
        return v8[m][1];
    if (i % 4 == 2 && j % 4 == 2)
        return v8[m][2];
    if ((i % 4 == 0 && j % 2 == 1) || (i % 2 == 1 && j % 4 == 0))
        return v8[m][3];
    if ((i % 4 == 0 && j % 4 == 2) || (i % 4 == 2 && j % 4 == 0))
        return v8[m][4];
    return v8[m][5];
}

std::uint8_t clip1(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }

}

void LevelScales::build(H264ScalingLists const& lists)
{
    for (int list = 0; list < 6; ++list) {
        std::array<int, 16> weight {};
        for (int k = 0; k < 16; ++k)
            weight[zigzag4x4[k]] = lists.present ? lists.list_4x4[static_cast<std::size_t>(list)][static_cast<std::size_t>(k)] : 16;
        for (int m = 0; m < 6; ++m)
            for (int p = 0; p < 16; ++p)
                scale4[static_cast<std::size_t>(list)][static_cast<std::size_t>(m)][static_cast<std::size_t>(p)] = weight[static_cast<std::size_t>(p)] * norm4(m, p >> 2, p & 3);
    }
    for (int list = 0; list < 2; ++list) {
        std::array<int, 64> weight {};
        for (int k = 0; k < 64; ++k)
            weight[zigzag8x8[k]] = lists.present ? lists.list_8x8[static_cast<std::size_t>(list)][static_cast<std::size_t>(k)] : 16;
        for (int m = 0; m < 6; ++m)
            for (int p = 0; p < 64; ++p)
                scale8[static_cast<std::size_t>(list)][static_cast<std::size_t>(m)][static_cast<std::size_t>(p)] = weight[static_cast<std::size_t>(p)] * norm8(m, p >> 3, p & 7);
    }
}

// 8.5.12.1: a 4x4 block's levels scaled; the DC left alone when it came
// from a DC transform (Intra_16x16, chroma).
void scale_4x4(std::int32_t* d, std::int32_t const* c, int const* scale, int qp, bool dc_separate)
{
    int const q6 = qp / 6;
    for (int p = 0; p < 16; ++p) {
        if (p == 0 && dc_separate) {
            d[0] = c[0];
            continue;
        }
        int const v = c[p] * scale[p];
        d[p] = qp >= 24 ? v * (1 << (q6 - 4)) : (v + (1 << (3 - q6))) >> (4 - q6);
    }
}

// 8.5.13.1.
void scale_8x8(std::int32_t* d, std::int32_t const* c, int const* scale, int qp)
{
    int const q6 = qp / 6;
    for (int p = 0; p < 64; ++p) {
        int const v = c[p] * scale[p];
        d[p] = qp >= 36 ? v * (1 << (q6 - 6)) : (v + (1 << (5 - q6))) >> (6 - q6);
    }
}

// 8.5.10: the luma DC of Intra_16x16, into each 4x4 block's position 0
// (dc[blk] in raster block order).
void luma_dc(std::int32_t* dc, std::int32_t const* c, int scale00, int qp)
{
    std::array<int, 16> t {};
    // f = A c A, A the 4x4 Hadamard matrix of 8-320.
    for (int i = 0; i < 4; ++i) {
        int const a = c[i * 4 + 0], b = c[i * 4 + 1], e = c[i * 4 + 2], g = c[i * 4 + 3];
        t[static_cast<std::size_t>(i * 4 + 0)] = a + b + e + g;
        t[static_cast<std::size_t>(i * 4 + 1)] = a + b - e - g;
        t[static_cast<std::size_t>(i * 4 + 2)] = a - b - e + g;
        t[static_cast<std::size_t>(i * 4 + 3)] = a - b + e - g;
    }
    int const q6 = qp / 6;
    for (int j = 0; j < 4; ++j) {
        int const a = t[static_cast<std::size_t>(0 * 4 + j)], b = t[static_cast<std::size_t>(1 * 4 + j)];
        int const e = t[static_cast<std::size_t>(2 * 4 + j)], g = t[static_cast<std::size_t>(3 * 4 + j)];
        int const f[4] = { a + b + e + g, a + b - e - g, a - b - e + g, a - b + e - g };
        for (int i = 0; i < 4; ++i) {
            int const v = f[i] * scale00;
            dc[i * 4 + j] = qp >= 36 ? v * (1 << (q6 - 6)) : (v + (1 << (5 - q6))) >> (6 - q6);
        }
    }
}

// 8.5.11.2 for 4:2:0: the chroma DC 2x2.
void chroma_dc(std::int32_t* dc, std::int32_t const* c, int scale00, int qp)
{
    int const f0 = c[0] + c[1] + c[2] + c[3];
    int const f1 = c[0] - c[1] + c[2] - c[3];
    int const f2 = c[0] + c[1] - c[2] - c[3];
    int const f3 = c[0] - c[1] - c[2] + c[3];
    int const q6 = qp / 6;
    dc[0] = ((f0 * scale00) * (1 << q6)) >> 5;
    dc[1] = ((f1 * scale00) * (1 << q6)) >> 5;
    dc[2] = ((f2 * scale00) * (1 << q6)) >> 5;
    dc[3] = ((f3 * scale00) * (1 << q6)) >> 5;
}

// 8.5.12.2 and the construction of 8.5.14: d inverse transformed and
// added to the prediction at `dst`.
void idct4x4_add(std::uint8_t* dst, int stride, std::int32_t const* d)
{
    std::array<int, 16> g {};
    for (int i = 0; i < 4; ++i) {
        int const e0 = d[i * 4 + 0] + d[i * 4 + 2];
        int const e1 = d[i * 4 + 0] - d[i * 4 + 2];
        int const e2 = (d[i * 4 + 1] >> 1) - d[i * 4 + 3];
        int const e3 = d[i * 4 + 1] + (d[i * 4 + 3] >> 1);
        g[static_cast<std::size_t>(i * 4 + 0)] = e0 + e3;
        g[static_cast<std::size_t>(i * 4 + 1)] = e1 + e2;
        g[static_cast<std::size_t>(i * 4 + 2)] = e1 - e2;
        g[static_cast<std::size_t>(i * 4 + 3)] = e0 - e3;
    }
    for (int j = 0; j < 4; ++j) {
        int const f0 = g[static_cast<std::size_t>(0 * 4 + j)] + g[static_cast<std::size_t>(2 * 4 + j)];
        int const f1 = g[static_cast<std::size_t>(0 * 4 + j)] - g[static_cast<std::size_t>(2 * 4 + j)];
        int const f2 = (g[static_cast<std::size_t>(1 * 4 + j)] >> 1) - g[static_cast<std::size_t>(3 * 4 + j)];
        int const f3 = g[static_cast<std::size_t>(1 * 4 + j)] + (g[static_cast<std::size_t>(3 * 4 + j)] >> 1);
        int const h[4] = { f0 + f3, f1 + f2, f1 - f2, f0 - f3 };
        for (int i = 0; i < 4; ++i) {
            std::uint8_t& p = dst[i * stride + j];
            p = clip1(p + ((h[i] + 32) >> 6));
        }
    }
}

namespace {

void idct8_1d(int const* d, int step, int* out, int out_step)
{
    int const d0 = d[0], d1 = d[step], d2 = d[2 * step], d3 = d[3 * step];
    int const d4 = d[4 * step], d5 = d[5 * step], d6 = d[6 * step], d7 = d[7 * step];
    int const e0 = d0 + d4;
    int const e1 = -d3 + d5 - d7 - (d7 >> 1);
    int const e2 = d0 - d4;
    int const e3 = d1 + d7 - d3 - (d3 >> 1);
    int const e4 = (d2 >> 1) - d6;
    int const e5 = -d1 + d7 + d5 + (d5 >> 1);
    int const e6 = d2 + (d6 >> 1);
    int const e7 = d3 + d5 + d1 + (d1 >> 1);
    int const f0 = e0 + e6;
    int const f1 = e1 + (e7 >> 2);
    int const f2 = e2 + e4;
    int const f3 = e3 + (e5 >> 2);
    int const f4 = e2 - e4;
    int const f5 = (e3 >> 2) - e5;
    int const f6 = e0 - e6;
    int const f7 = e7 - (e1 >> 2);
    out[0] = f0 + f7;
    out[out_step] = f2 + f5;
    out[2 * out_step] = f4 + f3;
    out[3 * out_step] = f6 + f1;
    out[4 * out_step] = f6 - f1;
    out[5 * out_step] = f4 - f3;
    out[6 * out_step] = f2 - f5;
    out[7 * out_step] = f0 - f7;
}

}

// 8.5.13.2.
void idct8x8_add(std::uint8_t* dst, int stride, std::int32_t const* d)
{
    std::array<int, 64> in {};
    for (int p = 0; p < 64; ++p)
        in[static_cast<std::size_t>(p)] = d[p];
    std::array<int, 64> g {};
    for (int i = 0; i < 8; ++i)
        idct8_1d(in.data() + i * 8, 1, g.data() + i * 8, 1);
    std::array<int, 64> h {};
    for (int j = 0; j < 8; ++j)
        idct8_1d(g.data() + j, 8, h.data() + j, 8);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) {
            std::uint8_t& p = dst[i * stride + j];
            p = clip1(p + ((h[static_cast<std::size_t>(i * 8 + j)] + 32) >> 6));
        }
}

}
