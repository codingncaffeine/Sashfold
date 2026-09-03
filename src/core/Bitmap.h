#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
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

// A rectangle whose four corners are quarter ellipses — the shape
// border-radius describes. Every corner keeps two radii: one measured
// along the horizontal edge it touches, one along the vertical. All
// eight zero and the shape is its rectangle.
//
// The curves come out of a square root of an exactly-rounded quotient,
// so the same pixels are drawn on every machine.
struct RoundedRect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    float top_left_x = 0;
    float top_left_y = 0;
    float top_right_x = 0;
    float top_right_y = 0;
    float bottom_right_x = 0;
    float bottom_right_y = 0;
    float bottom_left_x = 0;
    float bottom_left_y = 0;

    static RoundedRect of(Rect const& rect);

    // The whole pixels the shape can touch.
    Rect bounds() const;
    bool is_empty() const { return width <= 0 || height <= 0; }
    bool is_rectangular() const;

    // CSS Backgrounds §5.2: when the two radii along one edge together
    // overrun it, every radius shrinks by the same factor.
    void settle();

    // Where the shape's left and right edges lie at a height; false when
    // that height is outside it, or the two edges have met.
    bool span_at(float sample_y, float& left, float& right) const;

    // Whether a point lies inside the shape.
    bool contains_point(float px, float py) const;

    // How much of the pixel whose top-left corner is (px, py) the shape
    // covers, from 0 (none) through 255 (all of it).
    std::uint8_t coverage(int px, int py) const;

    // The shape a border of these widths leaves inside itself: each
    // radius loses the border behind it and stops at zero (§5.2).
    RoundedRect inset(float left, float top, float right, float bottom) const;
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

    // A filled shape with rounded corners, the curves antialiased; the
    // straight parts are filled as `fill_rect` would fill them.
    void fill_rounded(RoundedRect const& shape, Color color);

    // Fills what lies inside `outer` but outside `inner`, asking `color_at`
    // for the color of each pixel's center — the ring a border draws.
    void fill_ring(RoundedRect const& outer, RoundedRect const& inner,
        std::function<bool(float, float, Color&)> const& color_at);

    // Copies the source's pixels over this bitmap at (x, y), clipped; no
    // blending — the source replaces what was there.
    void blit(Bitmap const& source, int x, int y);

    // Draws the source scaled into `dest` with source-over compositing:
    // box-filtered when shrinking, nearest when growing, in integer
    // arithmetic, averaging premultiplied so transparent edges do not fringe.
    void draw_scaled(Bitmap const& source, Rect dest);

    // A clip rectangle every write honors (pixels outside it stay as they
    // are); none by default. The painter narrows it for a box that clips
    // its overflow and restores it after.
    void set_clip(std::optional<Rect> clip) { m_clip = clip; }
    std::optional<Rect> const& clip() const { return m_clip; }

    // Rounded clips, innermost last: a write is faded by how much of its
    // pixel every shape on the stack covers, so a curve clips a curve.
    // The painter pushes one for a box that rounds its corners and clips
    // what it holds, and truncates back to the depth it saved.
    std::size_t round_clip_depth() const { return m_round_clips.size(); }
    void push_round_clip(RoundedRect const& shape) { m_round_clips.push_back(shape); }
    void truncate_round_clips(std::size_t depth)
    {
        if (depth < m_round_clips.size())
            m_round_clips.resize(depth);
    }

private:
    bool writable(int x, int y) const
    {
        return contains(x, y) && (!m_clip || m_clip->contains(x, y));
    }

    // How much of the pixel the rounded clips leave writable, 0 to 255.
    std::uint8_t round_clip_coverage(int x, int y) const;

    // The store and the source-over blend, both past every clip test.
    void write_raw(int x, int y, Color color);
    void blend_raw(int x, int y, Color color);

    std::optional<Rect> m_clip;
    std::vector<RoundedRect> m_round_clips;
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
