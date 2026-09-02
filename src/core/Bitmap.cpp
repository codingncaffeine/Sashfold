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
    if (!writable(x, y))
        return;
    std::size_t const at = offset_of(x, y);
    m_pixels[at + 0u] = color.r;
    m_pixels[at + 1u] = color.g;
    m_pixels[at + 2u] = color.b;
    m_pixels[at + 3u] = color.a;
}

void Bitmap::blend_pixel(int x, int y, Color color)
{
    if (!writable(x, y))
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

    int x0 = std::max(rect.x, 0);
    int y0 = std::max(rect.y, 0);
    int x1 = std::min(rect.right(), m_width);
    int y1 = std::min(rect.bottom(), m_height);
    if (m_clip) {
        x0 = std::max(x0, m_clip->x);
        y0 = std::max(y0, m_clip->y);
        x1 = std::min(x1, m_clip->right());
        y1 = std::min(y1, m_clip->bottom());
    }

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

void Bitmap::fill_round_rect(Rect rect, int radius, Color color)
{
    if (rect.is_empty() || color.a == 0)
        return;
    int const r = std::max(0, std::min({ radius, rect.width / 2, rect.height / 2 }));
    if (r == 0) {
        fill_rect(rect, color);
        return;
    }
    // The band between the corner rows is a plain rectangle.
    fill_rect(Rect { rect.x, rect.y + r, rect.width, rect.height - 2 * r }, color);
    // Each corner row is inset to where its pixel centers fall inside the
    // corner circle; distances are kept doubled so the half-pixel centers
    // stay integral.
    int const rr4 = 4 * r * r;
    for (int row = 0; row < r; ++row) {
        int const dy2 = 2 * (r - row) - 1;
        int inset = r;
        for (int col = 0; col < r; ++col) {
            int const dx2 = 2 * (r - col) - 1;
            if (dx2 * dx2 + dy2 * dy2 <= rr4) {
                inset = col;
                break;
            }
        }
        fill_rect(Rect { rect.x + inset, rect.y + row, rect.width - 2 * inset, 1 }, color);
        fill_rect(Rect { rect.x + inset, rect.bottom() - 1 - row, rect.width - 2 * inset, 1 },
            color);
    }
}

void Bitmap::blit(Bitmap const& source, int x, int y)
{
    int x0 = std::max(x, 0);
    int y0 = std::max(y, 0);
    int x1 = std::min(x + source.width(), m_width);
    int y1 = std::min(y + source.height(), m_height);
    if (m_clip) {
        x0 = std::max(x0, m_clip->x);
        y0 = std::max(y0, m_clip->y);
        x1 = std::min(x1, m_clip->right());
        y1 = std::min(y1, m_clip->bottom());
    }
    if (x1 <= x0 || y1 <= y0)
        return;
    std::size_t const row_bytes = static_cast<std::size_t>(x1 - x0) * 4u;
    for (int row = y0; row < y1; ++row) {
        std::size_t const from = source.offset_of(x0 - x, row - y);
        std::size_t const to = offset_of(x0, row);
        std::copy_n(source.m_pixels.begin() + static_cast<std::ptrdiff_t>(from), row_bytes,
            m_pixels.begin() + static_cast<std::ptrdiff_t>(to));
    }
}

void Bitmap::draw_scaled(Bitmap const& source, Rect dest)
{
    if (dest.is_empty() || source.width() <= 0 || source.height() <= 0)
        return;
    int const source_width = source.width();
    int const source_height = source.height();
    int const x0 = std::max(dest.x, 0);
    int const y0 = std::max(dest.y, 0);
    int const x1 = std::min(dest.right(), m_width);
    int const y1 = std::min(dest.bottom(), m_height);
    // The source span each destination row or column covers.
    auto const span = [](int index, int source_size, int dest_size, int& from, int& to) {
        from = static_cast<int>(static_cast<std::int64_t>(index) * source_size / dest_size);
        to = static_cast<int>(static_cast<std::int64_t>(index + 1) * source_size / dest_size);
        if (to <= from)
            to = from + 1;
        to = std::min(to, source_size);
    };
    for (int y = y0; y < y1; ++y) {
        int sy0 = 0;
        int sy1 = 0;
        span(y - dest.y, source_height, dest.height, sy0, sy1);
        for (int x = x0; x < x1; ++x) {
            int sx0 = 0;
            int sx1 = 0;
            span(x - dest.x, source_width, dest.width, sx0, sx1);
            std::uint32_t sum_r = 0;
            std::uint32_t sum_g = 0;
            std::uint32_t sum_b = 0;
            std::uint32_t sum_a = 0;
            std::uint32_t count = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                for (int sx = sx0; sx < sx1; ++sx) {
                    Color const c = source.pixel(sx, sy);
                    sum_r += static_cast<std::uint32_t>(c.r) * c.a;
                    sum_g += static_cast<std::uint32_t>(c.g) * c.a;
                    sum_b += static_cast<std::uint32_t>(c.b) * c.a;
                    sum_a += c.a;
                    ++count;
                }
            }
            if (sum_a == 0)
                continue;
            blend_pixel(x, y,
                Color::rgba(static_cast<std::uint8_t>(sum_r / sum_a), static_cast<std::uint8_t>(sum_g / sum_a),
                    static_cast<std::uint8_t>(sum_b / sum_a), static_cast<std::uint8_t>(sum_a / count)));
        }
    }
}

}
