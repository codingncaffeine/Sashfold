#include "media/Yuv.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sashfold::media {

namespace {

// Y'CbCr to R'G'B' (ITU-R BT.601/709/2020, SMPTE 240M): with Kr and Kb the
// red and blue weights of luma and Kg = 1 - Kr - Kb,
//   R = Y + 2(1 - Kr) Cr
//   G = Y - 2 Kb (1 - Kb) / Kg Cb - 2 Kr (1 - Kr) / Kg Cr
//   B = Y + 2(1 - Kb) Cb
// with Y, Cb and Cr scaled from their range: studio luma runs 16-235 and
// the differences 16-240 about 128. Everything is in 16-bit fixed point
// and rounded once, at the end.
struct Coefficients {
    int y_offset;
    int y_scale;
    int cr_r;
    int cb_g;
    int cr_g;
    int cb_b;
};

Coefficients coefficients(YuvColour colour)
{
    double kr = 0.2126;
    double kb = 0.0722;
    switch (colour.matrix) {
    case YuvMatrix::Bt601: kr = 0.299, kb = 0.114; break;
    case YuvMatrix::Bt709: break;
    case YuvMatrix::Smpte240: kr = 0.212, kb = 0.087; break;
    case YuvMatrix::Bt2020: kr = 0.2627, kb = 0.0593; break;
    }
    double const kg = 1.0 - kr - kb;
    double const luma = colour.full_range ? 1.0 : 255.0 / 219.0;
    double const chroma = colour.full_range ? 1.0 : 255.0 / 224.0;
    auto const fixed = [](double value) { return static_cast<int>(std::lround(value * 65536.0)); };
    return Coefficients {
        colour.full_range ? 0 : 16,
        fixed(luma),
        fixed(2 * (1 - kr) * chroma),
        fixed(2 * kb * (1 - kb) / kg * chroma),
        fixed(2 * kr * (1 - kr) / kg * chroma),
        fixed(2 * (1 - kb) * chroma),
    };
}

std::uint8_t clamp_to_byte(int value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 255)); }

}

YuvColour vp9_colour(int color_space, bool color_range, int height)
{
    YuvColour colour;
    colour.full_range = color_range;
    switch (color_space) {
    case 1: // BT.601
    case 3: // SMPTE 170, BT.601's matrix
        colour.matrix = YuvMatrix::Bt601;
        break;
    case 2:
        colour.matrix = YuvMatrix::Bt709;
        break;
    case 4:
        colour.matrix = YuvMatrix::Smpte240;
        break;
    case 5:
        colour.matrix = YuvMatrix::Bt2020;
        break;
    default: // unknown or reserved: by the picture's size
        colour.matrix = height > 576 ? YuvMatrix::Bt709 : YuvMatrix::Bt601;
        break;
    }
    return colour;
}

void nv12_to_rgba(Nv12Picture const& picture, YuvColour colour, std::span<std::uint8_t> rgba)
{
    Coefficients const k = coefficients(colour);
    auto const width = static_cast<std::size_t>(picture.width);
    auto const height = static_cast<std::size_t>(picture.height);
    std::size_t const chroma_row = static_cast<std::size_t>(picture.chroma_width()) * 2;
    if (rgba.size() < width * height * 4 || picture.luma.size() < width * height
        || picture.chroma.size() < chroma_row * static_cast<std::size_t>(picture.chroma_height()))
        return;
    for (std::size_t y = 0; y < height; ++y) {
        std::uint8_t const* const luma = picture.luma.data() + y * width;
        std::uint8_t const* const chroma = picture.chroma.data() + (y / 2) * chroma_row;
        std::uint8_t* out = rgba.data() + y * width * 4;
        for (std::size_t x = 0; x < width; ++x) {
            int const cb = chroma[(x / 2) * 2] - 128;
            int const cr = chroma[(x / 2) * 2 + 1] - 128;
            int const l = (luma[x] - k.y_offset) * k.y_scale + (1 << 15);
            out[0] = clamp_to_byte((l + k.cr_r * cr) >> 16);
            out[1] = clamp_to_byte((l - k.cb_g * cb - k.cr_g * cr) >> 16);
            out[2] = clamp_to_byte((l + k.cb_b * cb) >> 16);
            out[3] = 255;
            out += 4;
        }
    }
}

}
