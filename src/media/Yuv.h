#pragma once

// A decoded video picture's samples as the painter's colours: luma and the
// two colour differences, as a stream codes them, to 8-bit RGBA. The matrix
// and the range come from the stream (VP9's color_space and color_range);
// a stream that does not say is taken as BT.709 when it is high definition
// and BT.601 when not, as players do. The colour differences, coded at half
// the size each way, are taken for the four pixels they cover.
//
// This is the path until the compositor (plan 7.5.4) does the same in a
// shader on the GPU, with the picture where the decoder left it.

#include "media/Vp9Accelerator.h"

#include <cstdint>
#include <span>

namespace sashfold::media {

enum class YuvMatrix : std::uint8_t {
    Bt601,
    Bt709,
    Smpte240,
    Bt2020,
};

struct YuvColour {
    YuvMatrix matrix = YuvMatrix::Bt709;
    bool full_range = false; // studio range (16-235, 16-240) when not
};

// VP9's color_space (§7.2: 0 unknown, 1 BT.601, 2 BT.709, 3 SMPTE 170,
// 4 SMPTE 240, 5 BT.2020, 7 sRGB) and color_range, for a picture this tall.
YuvColour vp9_colour(int color_space, bool color_range, int height);

// Converts the picture into `rgba`, which holds width * height * 4 bytes,
// rows packed, alpha opaque.
void nv12_to_rgba(Nv12Picture const& picture, YuvColour colour, std::span<std::uint8_t> rgba);

}
