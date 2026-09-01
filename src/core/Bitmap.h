#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sashfold {

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    static constexpr Color rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
    {
        return Color { red, green, blue, 255 };
    }

    static constexpr Color rgba(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha)
    {
        return Color { red, green, blue, alpha };
    }

    friend constexpr bool operator==(Color const&, Color const&) = default;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    constexpr int right() const { return x + width; }
    constexpr int bottom() const { return y + height; }
    constexpr bool is_empty() const { return width <= 0 || height <= 0; }
    constexpr bool contains(int px, int py) const
    {
        return px >= x && py >= y && px < right() && py < bottom();
    }
};

// An RGBA8 image, row-major, no padding between rows.
class Bitmap {
public:
    Bitmap(int width, int height, Color fill = Color::rgb(255, 255, 255));

    int width() const { return m_width; }
    int height() const { return m_height; }
    std::vector<std::uint8_t> const& pixels() const { return m_pixels; }

    bool contains(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < m_width && y < m_height;
    }

    Color pixel(int x, int y) const;

    // Overwrites the destination outright.
    void set_pixel(int x, int y, Color color);

    // Source-over compositing against the existing destination pixel.
    void blend_pixel(int x, int y, Color color);

    void fill_rect(Rect rect, Color color);

    // A filled rectangle whose corners are quarter circles of the given
    // radius — integer geometry only, so it is byte-identical everywhere.
    void fill_round_rect(Rect rect, int radius, Color color);

    // Copies the source's pixels over this bitmap at (x, y), clipped; no
    // blending — the source replaces what was there.
    void blit(Bitmap const& source, int x, int y);

private:
    std::size_t offset_of(int x, int y) const
    {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)
                   + static_cast<std::size_t>(x))
            * 4u;
    }

    int m_width;
    int m_height;
    std::vector<std::uint8_t> m_pixels;
};

}
