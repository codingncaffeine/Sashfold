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
#include <memory>
#include <span>
#include <thread>
#include <vector>

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
// H.264's VUI (E.2.1): matrix_coefficients as ITU-T H.273 numbers them
// (1 BT.709, 5 and 6 BT.601, 7 SMPTE 240, 9 BT.2020; 2 unspecified) and
// video_full_range_flag, for a picture this tall; `present` false when
// the stream says nothing.
YuvColour h264_colour(bool present, int matrix_coefficients, bool full_range, int height);

// Converts the picture into `rgba`, which holds width * height * 4 bytes,
// rows packed, alpha opaque.
void nv12_to_rgba(Nv12Picture const& picture, YuvColour colour, std::span<std::uint8_t> rgba);

// The same, a large picture's rows split among threads kept for it: a 4K
// picture is about 4 ms on one core, a quarter of a 60 Hz frame. The
// output is the same as nv12_to_rgba's.
class Nv12Converter {
public:
    // `threads`: how many convert a large picture (the caller's own
    // included); 0 for the machine's cores up to four.
    explicit Nv12Converter(int threads = 0);
    ~Nv12Converter();
    Nv12Converter(Nv12Converter const&) = delete;
    Nv12Converter& operator=(Nv12Converter const&) = delete;

    void convert(Nv12Picture const& picture, YuvColour colour, std::span<std::uint8_t> rgba);

private:
    struct Shared;
    std::unique_ptr<Shared> m_shared;
    std::vector<std::thread> m_helpers;
};

}
