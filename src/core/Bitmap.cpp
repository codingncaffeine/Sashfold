#include "core/Bitmap.h"

#include <algorithm>

namespace sashfold {

Bitmap::Bitmap(int width, int height, Color fill)
    : m_width(std::max(width, 0))
    , m_height(std::max(height, 0))
{
    std::size_t const count = static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height);
    m_pixels.resize(count * 4u);
    for (std::size_t i = 0; i < count; ++i) {
        m_pixels[i * 4u + 0u] = fill.r;
        m_pixels[i * 4u + 1u] = fill.g;
        m_pixels[i * 4u + 2u] = fill.b;
        m_pixels[i * 4u + 3u] = fill.a;
    }
}

Color Bitmap::pixel(int x, int y) const
{
    if (!contains(x, y))
        return Color::rgba(0, 0, 0, 0);
    std::size_t const at = offset_of(x, y);
    return Color { m_pixels[at + 0u], m_pixels[at + 1u], m_pixels[at + 2u], m_pixels[at + 3u] };
}

void Bitmap::set_pixel(int x, int y, Color color)
{
    if (!contains(x, y))
        return;
    std::size_t const at = offset_of(x, y);
    m_pixels[at + 0u] = color.r;
    m_pixels[at + 1u] = color.g;
    m_pixels[at + 2u] = color.b;
    m_pixels[at + 3u] = color.a;
}

void Bitmap::blend_pixel(int x, int y, Color color)
{
    if (!contains(x, y))
        return;
    if (color.a == 0)
        return;
    if (color.a == 255) {
        set_pixel(x, y, color);
        return;
    }

    Color const dst = pixel(x, y);

    unsigned const src_alpha = color.a;
    unsigned const inverse = 255u - src_alpha;

    // Destination's surviving contribution, already scaled by (1 - src_alpha).
    unsigned const dst_contrib = (static_cast<unsigned>(dst.a) * inverse + 127u) / 255u;
    unsigned const out_alpha = src_alpha + dst_contrib;
    if (out_alpha == 0) {
        set_pixel(x, y, Color::rgba(0, 0, 0, 0));
        return;
    }

    auto const channel = [&](std::uint8_t src_c, std::uint8_t dst_c) {
        unsigned const value = (static_cast<unsigned>(src_c) * src_alpha
                                   + static_cast<unsigned>(dst_c) * dst_contrib
                                   + out_alpha / 2u)
            / out_alpha;
        return static_cast<std::uint8_t>(std::min(value, 255u));
    };

    set_pixel(x, y,
        Color { channel(color.r, dst.r),
            channel(color.g, dst.g),
            channel(color.b, dst.b),
            static_cast<std::uint8_t>(std::min(out_alpha, 255u)) });
}

void Bitmap::fill_rect(Rect rect, Color color)
{
    if (rect.is_empty() || color.a == 0)
        return;

    int const x0 = std::max(rect.x, 0);
    int const y0 = std::max(rect.y, 0);
    int const x1 = std::min(rect.right(), m_width);
    int const y1 = std::min(rect.bottom(), m_height);

    bool const opaque = color.a == 255;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (opaque)
                set_pixel(x, y, color);
            else
                blend_pixel(x, y, color);
        }
    }
}

}
