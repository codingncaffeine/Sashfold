#include "core/Bitmap.h"

#include <algorithm>
#include <cmath>

namespace sashfold {

namespace {

// How many rows of a pixel the coverage looks at. The horizontal extent
// of each row is exact, so this is the only place the answer is
// approximate — sixteen rows keep a corner smooth without costing more
// than the handful of pixels a curve touches.
constexpr int rows_per_pixel = 16;

float floor_to(float value)
{
    return static_cast<float>(static_cast<int>(std::floor(value)));
}

// How far in from a straight edge the corner's curve lies at a height
// `from_edge` below (or above) the shape's edge: `along` is the radius
// measured on the straight edge the curve leaves, `across` the one on
// the edge it meets.
float curve_inset(float from_edge, float along, float across)
{
    if (along <= 0 || across <= 0 || from_edge >= across || from_edge < 0)
        return 0;
    float const remaining = (across - from_edge) / across;
    float const squared = 1.0f - remaining * remaining;
    if (squared <= 0)
        return 0;
    return along - along * std::sqrt(squared);
}

}

RoundedRect RoundedRect::of(Rect const& rect)
{
    RoundedRect shape;
    shape.x = static_cast<float>(rect.x);
    shape.y = static_cast<float>(rect.y);
    shape.width = static_cast<float>(rect.width);
    shape.height = static_cast<float>(rect.height);
    return shape;
}

Rect RoundedRect::bounds() const
{
    int const left = static_cast<int>(floor_to(x));
    int const top = static_cast<int>(floor_to(y));
    int const right = static_cast<int>(std::ceil(x + width));
    int const bottom = static_cast<int>(std::ceil(y + height));
    return Rect { left, top, std::max(0, right - left), std::max(0, bottom - top) };
}

bool RoundedRect::is_rectangular() const
{
    return top_left_x <= 0 && top_left_y <= 0 && top_right_x <= 0 && top_right_y <= 0
        && bottom_right_x <= 0 && bottom_right_y <= 0 && bottom_left_x <= 0 && bottom_left_y <= 0;
}

void RoundedRect::settle()
{
    float* const radii[8] = { &top_left_x, &top_left_y, &top_right_x, &top_right_y,
        &bottom_right_x, &bottom_right_y, &bottom_left_x, &bottom_left_y };
    for (float* radius : radii)
        *radius = std::max(0.0f, *radius);
    // A corner is a quarter ellipse only when both its radii are real;
    // one of them at zero squares the corner off.
    auto const pair = [](float& along, float& across) {
        if (along <= 0 || across <= 0)
            along = across = 0;
    };
    pair(top_left_x, top_left_y);
    pair(top_right_x, top_right_y);
    pair(bottom_right_x, bottom_right_y);
    pair(bottom_left_x, bottom_left_y);

    float factor = 1;
    auto const fits = [&](float sum, float extent) {
        if (sum > 0 && sum > extent)
            factor = std::min(factor, std::max(0.0f, extent) / sum);
    };
    fits(top_left_x + top_right_x, width);
    fits(bottom_left_x + bottom_right_x, width);
    fits(top_left_y + bottom_left_y, height);
    fits(top_right_y + bottom_right_y, height);
    if (factor >= 1)
        return;
    for (float* radius : radii)
        *radius *= factor;
}

bool RoundedRect::span_at(float sample_y, float& left, float& right) const
{
    if (is_empty() || sample_y < y || sample_y >= y + height)
        return false;
    float const from_top = sample_y - y;
    float const from_bottom = y + height - sample_y;
    float const in_left = std::max(curve_inset(from_top, top_left_x, top_left_y),
        curve_inset(from_bottom, bottom_left_x, bottom_left_y));
    float const in_right = std::max(curve_inset(from_top, top_right_x, top_right_y),
        curve_inset(from_bottom, bottom_right_x, bottom_right_y));
    left = x + in_left;
    right = x + width - in_right;
    return right > left;
}

bool RoundedRect::contains_point(float px, float py) const
{
    float left = 0;
    float right = 0;
    if (!span_at(py, left, right))
        return false;
    return px >= left && px <= right;
}

std::uint8_t RoundedRect::coverage(int px, int py) const
{
    float const left_edge = static_cast<float>(px);
    float const top_edge = static_cast<float>(py);
    float const right_edge = left_edge + 1.0f;
    float const bottom_edge = top_edge + 1.0f;
    if (is_empty() || right_edge <= x || left_edge >= x + width || bottom_edge <= y
        || top_edge >= y + height)
        return 0;
    if (is_rectangular()) {
        // The straight case, kept exact: the overlap of two rectangles.
        float const across = std::min(right_edge, x + width) - std::max(left_edge, x);
        float const down = std::min(bottom_edge, y + height) - std::max(top_edge, y);
        if (across <= 0 || down <= 0)
            return 0;
        return static_cast<std::uint8_t>(std::min(across, 1.0f) * std::min(down, 1.0f) * 255.0f + 0.5f);
    }
    // Away from the corners the shape is its rectangle: a pixel in the
    // band between the left and right radii, or between the top and
    // bottom ones, is covered outright.
    float const left_radius = std::max(top_left_x, bottom_left_x);
    float const right_radius = std::max(top_right_x, bottom_right_x);
    float const top_radius = std::max(top_left_y, top_right_y);
    float const bottom_radius = std::max(bottom_left_y, bottom_right_y);
    bool const within = left_edge >= x && right_edge <= x + width && top_edge >= y
        && bottom_edge <= y + height;
    if (within
        && ((left_edge >= x + left_radius && right_edge <= x + width - right_radius)
            || (top_edge >= y + top_radius && bottom_edge <= y + height - bottom_radius)))
        return 255;
    // The shape is convex, so a pixel whose four corners are all inside
    // is inside; that spares the sampling every pixel but the curve's own.
    if (contains_point(left_edge, top_edge) && contains_point(right_edge, top_edge)
        && contains_point(left_edge, bottom_edge) && contains_point(right_edge, bottom_edge))
        return 255;

    unsigned total = 0;
    for (int row = 0; row < rows_per_pixel; ++row) {
        float const sample_y
            = top_edge + (2.0f * static_cast<float>(row) + 1.0f) / (2.0f * rows_per_pixel);
        float left = 0;
        float right = 0;
        if (!span_at(sample_y, left, right))
            continue;
        float const covered = std::min(right, right_edge) - std::max(left, left_edge);
        if (covered <= 0)
            continue;
        total += static_cast<unsigned>(std::min(covered, 1.0f) * 255.0f + 0.5f);
    }
    return static_cast<std::uint8_t>((total + rows_per_pixel / 2) / rows_per_pixel);
}

RoundedRect RoundedRect::inset(float left, float top, float right, float bottom) const
{
    RoundedRect inner;
    inner.x = x + left;
    inner.y = y + top;
    inner.width = std::max(0.0f, width - left - right);
    inner.height = std::max(0.0f, height - top - bottom);
    auto const shrink = [](float radius, float edge) { return std::max(0.0f, radius - edge); };
    inner.top_left_x = shrink(top_left_x, left);
    inner.top_left_y = shrink(top_left_y, top);
    inner.top_right_x = shrink(top_right_x, right);
    inner.top_right_y = shrink(top_right_y, top);
    inner.bottom_right_x = shrink(bottom_right_x, right);
    inner.bottom_right_y = shrink(bottom_right_y, bottom);
    inner.bottom_left_x = shrink(bottom_left_x, left);
    inner.bottom_left_y = shrink(bottom_left_y, bottom);
    inner.settle();
    return inner;
}

}
