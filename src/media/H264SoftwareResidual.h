#pragma once

// 8.5's scaling and inverse transforms (media/H264SoftwareResidual.cpp).

#include "media/H264.h"

#include <array>
#include <cstdint>

namespace sashfold::media::h264sw {

// LevelScale4x4 and LevelScale8x8 (8-313, 8-316) for every qP % 6, from
// the scaling lists in force: 4x4 lists 0 to 5 (Y, Cb, Cr intra, then
// inter), 8x8 lists 0 to 5 (Y intra, Y inter, then Cb and Cr likewise,
// 4:4:4's); positions in raster order.
struct LevelScales {
    std::array<std::array<std::array<int, 16>, 6>, 6> scale4 {};
    std::array<std::array<std::array<int, 64>, 6>, 6> scale8 {};
    void build(H264ScalingLists const& lists);
};

// The scaling of 8.5.12.1, 8.5.13.1, 8.5.10 and 8.5.11.2 with qP = QP' (the
// bit depth's QpBdOffset included). `Wide`: a stream deeper than 8 bits,
// whose levels may be larger than 16 bits; its products are taken in 64
// bits and the results held to what a conforming stream can reach
// (2^(7 + bitDepth), 7.4.5.3.3), so a broken one cannot overflow the
// transforms after them.
template<bool Wide>
void scale_4x4(std::int32_t* d, std::int32_t const* c, int const* scale, int qp, bool dc_separate);
template<bool Wide>
void scale_8x8(std::int32_t* d, std::int32_t const* c, int const* scale, int qp);
template<bool Wide>
void luma_dc(std::int32_t* dc, std::int32_t const* c, int scale00, int qp);
template<bool Wide>
void chroma_dc(std::int32_t* dc, std::int32_t const* c, int scale00, int qp);
// 4:2:2's 2x4 chroma DC (8-325, 8-327 to 8-329): `c` in ChromaDCLevel's
// order, `dc` per chroma4x4BlkIdx (raster order of the 2x4 blocks).
// `scale00_dc`: LevelScale4x4( qPDC % 6, 0, 0 ), qPDC = qP + 3.
void chroma_dc_422(std::int32_t* dc, std::int32_t const* c, int scale00_dc, int qp_dc);

// 8.5.12.2, 8.5.13.2 and 8.5.14: d inverse transformed and added to the
// prediction at `dst`, clipped to `max` (255 where P is a byte).
template<typename P>
void idct4x4_add(P* dst, int stride, std::int32_t const* d, int max);
template<typename P>
void idct8x8_add(P* dst, int stride, std::int32_t const* d, int max);

// 8.5.12 to 8.5.15 where TransformBypassModeFlag is 1: the levels (raster
// order, n x n) are the residual, added as they are — or, for an intra
// block predicted vertically (`hor_pred` 0) or horizontally (1), summed
// down its columns or along its rows first (8.5.15); `hor_pred` -1 for
// any other block. `levels` is the residual of a whole w x h area.
template<typename P>
void bypass_add(P* dst, int stride, std::int32_t const* levels, int w, int h, int hor_pred, int max);

}
