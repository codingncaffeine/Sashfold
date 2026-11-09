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

YuvColour h264_colour(bool present, int matrix_coefficients, bool full_range, int height)
{
    YuvColour colour;
    colour.full_range = present && full_range;
    switch (present ? matrix_coefficients : 2) {
    case 1:
        colour.matrix = YuvMatrix::Bt709;
        break;
    case 5:
    case 6:
        colour.matrix = YuvMatrix::Bt601;
        break;
    case 7:
        colour.matrix = YuvMatrix::Smpte240;
        break;
    case 9:
    case 10:
        colour.matrix = YuvMatrix::Bt2020;
        break;
    default: // unspecified or beyond: by the picture's size
        colour.matrix = height > 576 ? YuvMatrix::Bt709 : YuvMatrix::Bt601;
        break;
    }
    return colour;
}

namespace {

// One row: the pixels in pairs that share a chroma sample, in a loop the
// compiler vectorizes; an odd width's last pixel alone. The arithmetic is
// the same 16-bit fixed point whichever way it is compiled.
[[gnu::always_inline]] inline void convert_row(std::uint8_t const* luma, std::uint8_t const* chroma, std::uint8_t* out, std::size_t width,
    Coefficients const& k)
{
    auto const pixel = [&k](std::uint8_t* to, int y, int red, int green, int blue) {
        int const l = (y - k.y_offset) * k.y_scale + (1 << 15);
        to[0] = static_cast<std::uint8_t>(std::clamp((l + red) >> 16, 0, 255));
        to[1] = static_cast<std::uint8_t>(std::clamp((l + green) >> 16, 0, 255));
        to[2] = static_cast<std::uint8_t>(std::clamp((l + blue) >> 16, 0, 255));
        to[3] = 255;
    };
    std::size_t const pairs = width / 2;
    for (std::size_t i = 0; i < pairs; ++i) {
        int const cb = chroma[i * 2] - 128;
        int const cr = chroma[i * 2 + 1] - 128;
        int const red = k.cr_r * cr, green = -k.cb_g * cb - k.cr_g * cr, blue = k.cb_b * cb;
        pixel(out + i * 8, luma[i * 2], red, green, blue);
        pixel(out + i * 8 + 4, luma[i * 2 + 1], red, green, blue);
    }
    if (width & 1) {
        int const cb = chroma[pairs * 2] - 128;
        int const cr = chroma[pairs * 2 + 1] - 128;
        pixel(out + pairs * 8, luma[pairs * 2], k.cr_r * cr, -k.cb_g * cb - k.cr_g * cr, k.cb_b * cb);
    }
}

[[gnu::always_inline]] inline void convert(Nv12Picture const& picture, Coefficients const& k, std::uint8_t* rgba)
{
    auto const width = static_cast<std::size_t>(picture.width);
    std::size_t const chroma_row = static_cast<std::size_t>(picture.chroma_width()) * 2;
    for (std::size_t y = 0; y < static_cast<std::size_t>(picture.height); ++y)
        convert_row(picture.luma.data() + y * width, picture.chroma.data() + (y / 2) * chroma_row, rgba + y * width * 4, width, k);
}

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
// The same loop with AVX2's eight-wide 32-bit multiplies, where the
// processor has them.
__attribute__((target("avx2"))) void convert_avx2(Nv12Picture const& picture, Coefficients const& k, std::uint8_t* rgba)
{
    convert(picture, k, rgba);
}
bool has_avx2()
{
    // Asked once, after the processor has been looked at (a static
    // initializer may run before libgcc's own does).
    static bool const yes = [] {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") != 0;
    }();
    return yes;
}
#endif

void convert_baseline(Nv12Picture const& picture, Coefficients const& k, std::uint8_t* rgba) { convert(picture, k, rgba); }

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
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    if (has_avx2()) {
        convert_avx2(picture, k, rgba.data());
        return;
    }
#endif
    convert_baseline(picture, k, rgba.data());
}

}
