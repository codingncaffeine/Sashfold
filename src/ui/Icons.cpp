#include "ui/Icons.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace sashfold::ui {

namespace {

// An icon's geometry is written in 32nds of a grid unit — the grid is 16
// units, so 512 across — and scaled to the size asked for in 128ths of a
// pixel. At sizes up to 256 px every product below fits sixty-four bits.
using Unit = std::int64_t;

constexpr int samples = 8; // a side: sixty-four to a pixel
constexpr Unit per_pixel = 128;
constexpr Unit half_stroke = 24; // three quarters of a grid unit
constexpr Unit thinnest = per_pixel * 9 / 16; // a stroke never under 1 1/8 px

constexpr Unit u(double grid) { return static_cast<Unit>(grid * 32.0 + (grid < 0 ? -0.5 : 0.5)); }

// A round-ended segment, or — with `radius` — the ring about `a` of that
// radius, less the wedge that opens clockwise from the direction `gap_from`
// to the direction `gap_to` (under half a turn): an arc. An arc's ends are
// cut square along its radii; a segment of no length laid at each rounds them.
struct Stroke {
    Unit ax, ay, bx, by;
    Unit radius = 0;
    Unit gap_from_x = 0, gap_from_y = 0, gap_to_x = 0, gap_to_y = 0;
};

constexpr Stroke line(double ax, double ay, double bx, double by) { return { u(ax), u(ay), u(bx), u(by) }; }
constexpr Stroke dot(double x, double y) { return { u(x), u(y), u(x), u(y) }; }

std::vector<Stroke> const& strokes_of(Icon icon)
{
    static std::vector<Stroke> const back = { line(3.5, 8, 12.5, 8), line(7.5, 4, 3.5, 8), line(3.5, 8, 7.5, 12) };
    static std::vector<Stroke> const forward = { line(3.5, 8, 12.5, 8), line(8.5, 4, 12.5, 8), line(12.5, 8, 8.5, 12) };
    // Round the clock from a little past three to half past one, where it
    // runs on along its tangent into the corner of an arrowhead's two arms.
    static std::vector<Stroke> const reload = {
        Stroke { u(8), u(8), 0, 0, u(5), 1, -1, 17, 6 },
        dot(11.536, 4.464), dot(12.715, 9.664),
        line(11.536, 4.464, 13.1, 6.0), line(13.1, 2.6, 13.1, 6.0), line(13.1, 6.0, 9.7, 6.0),
    };
    static std::vector<Stroke> const reader = {
        line(4.5, 2.5, 11.5, 2.5), line(11.5, 2.5, 11.5, 13.5), line(11.5, 13.5, 4.5, 13.5), line(4.5, 13.5, 4.5, 2.5),
        line(6.75, 5.5, 9.25, 5.5), line(6.75, 8, 9.25, 8), line(6.75, 10.5, 9.25, 10.5),
    };
    static std::vector<Stroke> const menu = { line(3, 4.5, 13, 4.5), line(3, 8, 13, 8), line(3, 11.5, 13, 11.5) };
    static std::vector<Stroke> const plus = { line(8, 3.5, 8, 12.5), line(3.5, 8, 12.5, 8) };
    static std::vector<Stroke> const close = { line(4.5, 4.5, 11.5, 11.5), line(11.5, 4.5, 4.5, 11.5) };
    static std::vector<Stroke> const minimize = { line(4, 8, 12, 8) };
    static std::vector<Stroke> const maximize
        = { line(4.5, 4.5, 11.5, 4.5), line(11.5, 4.5, 11.5, 11.5), line(11.5, 11.5, 4.5, 11.5), line(4.5, 11.5, 4.5, 4.5) };
    // A sheet with its top right corner folded over: the reader's outline,
    // the corner cut along a diagonal and the fold's two edges inside it.
    static std::vector<Stroke> const page = {
        line(4.5, 2.5, 9, 2.5), line(9, 2.5, 11.5, 5), line(11.5, 5, 11.5, 13.5), line(11.5, 13.5, 4.5, 13.5),
        line(4.5, 13.5, 4.5, 2.5), line(9, 2.5, 9, 5), line(9, 5, 11.5, 5),
    };
    switch (icon) {
    case Icon::Back: return back;
    case Icon::Forward: return forward;
    case Icon::Reload: return reload;
    case Icon::Reader: return reader;
    case Icon::Menu: return menu;
    case Icon::Plus: return plus;
    case Icon::Close: return close;
    case Icon::Minimize: return minimize;
    case Icon::Maximize: return maximize;
    case Icon::Page: return page;
    }
    return back;
}

// A grid coordinate at `size` px to the 16 units, in 128ths of a pixel.
Unit scaled(Unit grid32, int size) { return (grid32 * size + 2) / 4; }

// Whether the point lies within `reach` of the stroke's spine.
bool within(Stroke const& stroke, int size, Unit reach, Unit px, Unit py)
{
    Unit const ax = scaled(stroke.ax, size);
    Unit const ay = scaled(stroke.ay, size);
    Unit const vx = px - ax;
    Unit const vy = py - ay;
    Unit const from_a = vx * vx + vy * vy;
    if (stroke.radius != 0) {
        Unit const radius = scaled(stroke.radius, size);
        Unit const inner = std::max<Unit>(0, radius - reach);
        Unit const outer = radius + reach;
        if (from_a < inner * inner || from_a > outer * outer)
            return false;
        // In the wedge the arc leaves open: clockwise of where it opens and
        // counter-clockwise of where it closes, y growing downwards.
        bool const past_opening = stroke.gap_from_x * vy - stroke.gap_from_y * vx >= 0;
        bool const before_closing = vx * stroke.gap_to_y - vy * stroke.gap_to_x >= 0;
        return !(past_opening && before_closing);
    }
    Unit const dx = scaled(stroke.bx, size) - ax;
    Unit const dy = scaled(stroke.by, size) - ay;
    Unit const length = dx * dx + dy * dy;
    Unit const along = vx * dx + vy * dy;
    if (length == 0 || along <= 0)
        return from_a <= reach * reach;
    if (along >= length) {
        Unit const wx = vx - dx;
        Unit const wy = vy - dy;
        return wx * wx + wy * wy <= reach * reach;
    }
    // The distance from the line squared is |v|² − along² / |d|²; compared
    // with reach² across the division, so that nothing is rounded.
    return from_a * length - along * along <= reach * reach * length;
}

std::vector<std::uint8_t> const& mask_of(Icon icon, int size)
{
    static std::map<std::pair<int, int>, std::vector<std::uint8_t>> cache;
    auto const key = std::make_pair(static_cast<int>(icon), size);
    if (auto const found = cache.find(key); found != cache.end())
        return found->second;
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0);
    std::vector<Stroke> const& strokes = strokes_of(icon);
    Unit const reach = std::max(scaled(half_stroke, size), thinnest);
    Unit const step = per_pixel / samples;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int inside = 0;
            for (int sy = 0; sy < samples; ++sy) {
                for (int sx = 0; sx < samples; ++sx) {
                    Unit const px = x * per_pixel + sx * step + step / 2;
                    Unit const py = y * per_pixel + sy * step + step / 2;
                    for (Stroke const& stroke : strokes) {
                        if (within(stroke, size, reach, px, py)) {
                            ++inside;
                            break;
                        }
                    }
                }
            }
            mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)]
                = static_cast<std::uint8_t>(inside * 255 / (samples * samples));
        }
    }
    return cache.emplace(key, std::move(mask)).first->second;
}

}

std::uint8_t icon_coverage(Icon icon, int size, int x, int y)
{
    if (size <= 0 || size > 256 || x < 0 || y < 0 || x >= size || y >= size)
        return 0;
    return mask_of(icon, size)[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)];
}

void draw_icon(Bitmap& target, Icon icon, Rect const& box, int size, Color color)
{
    if (size <= 0 || size > 256 || color.a == 0)
        return;
    std::vector<std::uint8_t> const& mask = mask_of(icon, size);
    int const left = box.x + (box.width - size) / 2;
    int const top = box.y + (box.height - size) / 2;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            std::uint8_t const coverage = mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)];
            if (coverage == 0)
                continue;
            Color shaded = color;
            shaded.a = static_cast<std::uint8_t>(static_cast<int>(color.a) * coverage / 255);
            target.blend_pixel(left + x, top + y, shaded);
        }
    }
}

}
