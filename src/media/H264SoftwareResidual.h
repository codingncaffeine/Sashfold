#pragma once

// 8.5's scaling and inverse transforms (media/H264SoftwareResidual.cpp).

#include "media/H264.h"

#include <array>
#include <cstdint>

namespace sashfold::media::h264sw {

// LevelScale4x4 and LevelScale8x8 (8-313, 8-316) for every qP % 6, from
// the scaling lists in force: 4x4 lists 0 to 5 (Y, Cb, Cr intra, then
// inter), 8x8 lists 0 and 1 (Y intra, inter); positions in raster order.
struct LevelScales {
    std::array<std::array<std::array<int, 16>, 6>, 6> scale4 {};
    std::array<std::array<std::array<int, 64>, 6>, 2> scale8 {};
    void build(H264ScalingLists const& lists);
};

void scale_4x4(std::int32_t* d, std::int32_t const* c, int const* scale, int qp, bool dc_separate);
void scale_8x8(std::int32_t* d, std::int32_t const* c, int const* scale, int qp);
void luma_dc(std::int32_t* dc, std::int32_t const* c, int scale00, int qp);
void chroma_dc(std::int32_t* dc, std::int32_t const* c, int scale00, int qp);
void idct4x4_add(std::uint8_t* dst, int stride, std::int32_t const* d);
void idct8x8_add(std::uint8_t* dst, int stride, std::int32_t const* d);

}
