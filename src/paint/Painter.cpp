#include "paint/Painter.h"

#include "core/Unicode.h"
#include "dom/Dom.h"
#include "paint/Blur.h"
#include "paint/Effects.h"
#include "svg/Svg.h"
#include "text/Face.h"
#include "text/FontManager.h"

#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sashfold::paint {

namespace {

// Whether the fields' carets are in their shown phase (CaretsShown).
thread_local bool g_carets_shown = true;

using css::BorderStyle;
using css::ComputedStyle;
using layout::Fragment;
using layout::TextRun;

// text-overflow: ellipsis (css-overflow-3 §7.1), as one painting of the page
// shows it. Layout leaves every line whole; a block that ends its lines in
// an ellipsis has, for each line that runs out past its end edge where the
// content now stands, the characters and the atomic inlines that give way
// to the ellipsis, and the ellipsis itself. Nothing here outlives the
// painting: content that is scrolled is read again where it then stands.
struct Elisions {
    // The characters of a run that show, [from, to), and how far from the
    // run's left edge the first of them begins. A run not named shows whole.
    struct Kept {
        std::size_t from = 0;
        std::size_t to = 0;
        float shift = 0;
    };
    std::unordered_map<TextRun const*, Kept> runs;
    // The atomic inlines that do not show.
    std::unordered_set<Fragment const*> hidden;
    // The ellipses, by the fragment that holds their lines' words.
    std::unordered_map<Fragment const*, std::vector<TextRun>> marks;
};

struct Context {
    Bitmap& target;
    float dx;
    float dy;
    layout::BackgroundImages const* backgrounds;
    // The box whose background already went onto the canvas: it does not
    // paint that background again over its own box.
    Fragment const* canvas_owner = nullptr;
    // How far each box that scrolls has moved its content. Null leaves
    // every scrollport at its origin, which is what a single render of a
    // page shows.
    layout::ScrollOffsets const* scrolls = nullptr;
    // Device px per CSS px, for the shapes the painter draws on its own.
    float device_scale = 1;
    // The clip the page as a whole is painted through: all that clips a
    // fixed box, wherever in the tree it stands.
    std::optional<Rect> page_clip = std::nullopt;
    // Where the pictures went, for a host that asked (paint_page).
    std::vector<PaintedPicture>* pictures = nullptr;
    // The videos' pictures the host shows itself: their places are left open.
    std::vector<Bitmap const*> const* shown_apart = nullptr;
    // Where this target's corner lies in the one the host gave: a group
    // painted apart is a part of it, and the pictures are noted in the
    // host's own coordinates.
    int origin_x = 0;
    int origin_y = 0;
    // What text-overflow hides and adds in this painting; null hides nothing.
    Elisions* elisions = nullptr;
};

int round_px(float value)
{
    // A coordinate beyond the int range saturates: the cast is undefined
    // for it, and nothing that far from the canvas is drawn anyway.
    float const rounded = value >= 0 ? value + 0.5f : value - 0.5f;
    if (rounded != rounded)
        return 0;
    if (rounded >= 2147483648.0f)
        return 2147483647;
    if (rounded <= -2147483648.0f)
        return -2147483647 - 1;
    return static_cast<int>(rounded);
}

Rect snap(float x, float y, float width, float height)
{
    int const left = round_px(x);
    int const top = round_px(y);
    return Rect { left, top, round_px(x + width) - left, round_px(y + height) - top };
}

// --- Backgrounds ------------------------------------------------------------------

float resolve_length(css::LengthPercent const& length, float base)
{
    switch (length.kind) {
    case css::LengthPercent::Kind::Auto: return 0;
    case css::LengthPercent::Kind::Px: return length.value;
    case css::LengthPercent::Kind::Percent: return base * length.value / 100.0f;
    case css::LengthPercent::Kind::Calc: return length.value + base * length.percent / 100.0f;
    // Painting is given lengths, never a size property: a content keyword
    // can only arrive here through a value that never carries one.
    case css::LengthPercent::Kind::MinContent:
    case css::LengthPercent::Kind::MaxContent:
    case css::LengthPercent::Kind::FitContent: return 0;
    }
    return 0;
}

struct Area {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

// The box a background property names, in target coordinates: the border
// box, the padding box inside the borders, or the content box inside the
// padding too.
Area box_of(Context const& context, Fragment const& fragment, css::BackgroundBox box)
{
    ComputedStyle const& style = *fragment.style;
    Area area { fragment.x + context.dx, fragment.y + context.dy, fragment.width, fragment.height };
    if (box == css::BackgroundBox::BorderBox)
        return area;
    auto const inset = [&](float left, float top, float right, float bottom) {
        area.x += left;
        area.y += top;
        area.width = std::max(0.0f, area.width - left - right);
        area.height = std::max(0.0f, area.height - top - bottom);
    };
    inset(style.border_left.width, style.border_top.width, style.border_right.width, style.border_bottom.width);
    if (box == css::BackgroundBox::ContentBox) {
        float const base = fragment.width;
        inset(resolve_length(style.padding_left, base), resolve_length(style.padding_top, base),
            resolve_length(style.padding_right, base), resolve_length(style.padding_bottom, base));
    }
    return area;
}

Rect intersect(Rect const& a, Rect const& b);

// --- Rounded corners --------------------------------------------------------------

// A radius resolves against the border box: the horizontal one against
// its width, the vertical one against its height.
float resolve_radius(css::LengthPercent const& length, float base)
{
    return std::max(0.0f, resolve_length(length, base));
}

// The shape a box's border box makes, its corners curved by
// border-radius and settled so no edge's two radii overrun it. The
// straight edges land where `snap` puts them, so a square box paints
// exactly the pixels the plain rectangle would.
RoundedRect rounded_box(Context const& context, Fragment const& fragment)
{
    ComputedStyle const& style = *fragment.style;
    Rect const box = snap(fragment.x + context.dx, fragment.y + context.dy, fragment.width, fragment.height);
    RoundedRect shape = RoundedRect::of(box);
    float const across = static_cast<float>(box.width);
    float const down = static_cast<float>(box.height);
    auto const corner = [&](css::CornerRadius const& radius, float& x, float& y) {
        if (radius.is_zero())
            return;
        x = resolve_radius(radius.x, across);
        y = resolve_radius(radius.y, down);
    };
    corner(style.border_top_left_radius, shape.top_left_x, shape.top_left_y);
    corner(style.border_top_right_radius, shape.top_right_x, shape.top_right_y);
    corner(style.border_bottom_right_radius, shape.bottom_right_x, shape.bottom_right_y);
    corner(style.border_bottom_left_radius, shape.bottom_left_x, shape.bottom_left_y);
    shape.settle();
    return shape;
}

// The border box's shape narrowed to a background box: the padding box
// inside the borders, or the content box inside the padding too. The
// edges are snapped the same way `box_of` snaps them, so the curve and
// the straight parts agree.
RoundedRect rounded_area(Context const& context, Fragment const& fragment, css::BackgroundBox box)
{
    RoundedRect const outer = rounded_box(context, fragment);
    if (box == css::BackgroundBox::BorderBox)
        return outer;
    ComputedStyle const& style = *fragment.style;
    float left = style.border_left.width;
    float top = style.border_top.width;
    float right = style.border_right.width;
    float bottom = style.border_bottom.width;
    if (box == css::BackgroundBox::ContentBox) {
        float const base = fragment.width;
        left += resolve_length(style.padding_left, base);
        top += resolve_length(style.padding_top, base);
        right += resolve_length(style.padding_right, base);
        bottom += resolve_length(style.padding_bottom, base);
    }
    Area const inner = box_of(context, fragment, box);
    Rect const snapped = snap(inner.x, inner.y, inner.width, inner.height);
    RoundedRect shape = outer.inset(left, top, right, bottom);
    shape.x = static_cast<float>(snapped.x);
    shape.y = static_cast<float>(snapped.y);
    shape.width = static_cast<float>(snapped.width);
    shape.height = static_cast<float>(snapped.height);
    shape.settle();
    return shape;
}

// The color at `t` along a gradient whose stops are settled at positions
// in [0, 1] (repeating ones wrap over their span).
Color gradient_color(std::vector<std::pair<float, Color>> const& stops, float t, bool repeating)
{
    if (stops.empty())
        return Color::rgba(0, 0, 0, 0);
    if (repeating) {
        float const first = stops.front().first;
        float const last = stops.back().first;
        float const span = last - first;
        if (span > 0) {
            t = first + std::fmod(t - first, span);
            if (t < first)
                t += span;
        }
    }
    if (t <= stops.front().first)
        return stops.front().second;
    if (t >= stops.back().first)
        return stops.back().second;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t > stops[i].first)
            continue;
        auto const& [p0, c0] = stops[i - 1];
        auto const& [p1, c1] = stops[i];
        float const f = p1 > p0 ? (t - p0) / (p1 - p0) : 1.0f;
        auto const mix = [f](std::uint8_t a, std::uint8_t b) {
            return static_cast<std::uint8_t>(std::clamp(
                static_cast<int>(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * f + 0.5f), 0, 255));
        };
        return Color::rgba(mix(c0.r, c1.r), mix(c0.g, c1.g), mix(c0.b, c1.b), mix(c0.a, c1.a));
    }
    return stops.back().second;
}

// The stops' positions along a line of `length` px, as fractions: written
// ones resolved, the first at 0 and the last at 1 when unwritten, the rest
// spread evenly between their neighbors, and none before the one before it.
std::vector<std::pair<float, Color>> settle_stops(css::Gradient const& gradient, float length)
{
    std::vector<std::optional<float>> positions;
    for (css::GradientStop const& stop : gradient.stops) {
        if (!stop.position) {
            positions.emplace_back();
            continue;
        }
        float const px = resolve_length(*stop.position, length);
        positions.emplace_back(length > 0 ? px / length : 0.0f);
    }
    if (!positions.front())
        positions.front() = 0.0f;
    if (!positions.back())
        positions.back() = 1.0f;
    float floor = *positions.front();
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (!positions[i])
            continue;
        if (*positions[i] < floor)
            positions[i] = floor;
        floor = *positions[i];
    }
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (positions[i])
            continue;
        std::size_t next = i;
        while (!positions[next])
            ++next;
        float const from = *positions[i - 1];
        float const to = *positions[next];
        auto const count = static_cast<float>(next - i + 1);
        for (std::size_t k = i; k < next; ++k)
            positions[k] = from + (to - from) * static_cast<float>(k - i + 1) / count;
        i = next;
    }
    std::vector<std::pair<float, Color>> settled;
    for (std::size_t i = 0; i < positions.size(); ++i)
        settled.emplace_back(*positions[i], gradient.stops[i].color);
    return settled;
}

// The sine and cosine of an angle in degrees, the same to the bit on
// every system: the angle reduced to an octant, then a fixed series in
// double — only additions, multiplications and divisions, which IEEE
// rounds identically everywhere (contraction is off for the build).
std::pair<float, float> sin_cos_degrees(float degrees)
{
    double a = std::fmod(static_cast<double>(degrees), 360.0);
    if (a < 0)
        a += 360.0;
    int const quadrant = static_cast<int>(a / 90.0) % 4;
    double r = a - static_cast<double>(quadrant) * 90.0;
    bool const swap = r > 45.0;
    if (swap)
        r = 90.0 - r;
    double const x = r * (3.14159265358979323846 / 180.0);
    double const x2 = x * x;
    double s = x * (1.0 - x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0 * (1.0 - x2 / 110.0)))));
    double c = 1.0 - x2 / 2.0 * (1.0 - x2 / 12.0 * (1.0 - x2 / 30.0 * (1.0 - x2 / 56.0 * (1.0 - x2 / 90.0))));
    if (swap)
        std::swap(s, c);
    switch (quadrant) {
    case 1: {
        double const t = s;
        s = c;
        c = -t;
        break;
    }
    case 2:
        s = -s;
        c = -c;
        break;
    case 3: {
        double const t = s;
        s = -c;
        c = t;
        break;
    }
    default:
        break;
    }
    return { static_cast<float>(s), static_cast<float>(c) };
}

// Paints a gradient over one tile of the image, clipped to `clip`: a
// linear gradient along its line through the tile's center, a radial one
// out from its center to the extent named.
void paint_gradient(Bitmap& target, css::Gradient const& gradient, Area const& tile, Rect const& clip)
{
    Rect const bounds = intersect(snap(tile.x, tile.y, tile.width, tile.height), clip);
    if (bounds.width <= 0 || bounds.height <= 0 || tile.width <= 0 || tile.height <= 0)
        return;
    float const w = tile.width;
    float const h = tile.height;
    if (gradient.kind == css::Gradient::Kind::Linear) {
        // The gradient line's direction. Toward a corner it is perpendicular
        // to the diagonal between the two other corners (a square root,
        // which IEEE rounds the same everywhere); at an angle it comes from
        // a sine and cosine computed here — no libm, whose results differ
        // from one system to the next, and the reftests must not.
        float dir_x = 0;
        float dir_y = 0;
        if (gradient.corner != css::Gradient::Corner::None) {
            float const d = std::sqrt(w * w + h * h);
            float const ux = d > 0 ? h / d : 0.0f;
            float const uy = d > 0 ? w / d : 0.0f;
            switch (gradient.corner) {
            case css::Gradient::Corner::TopRight: dir_x = ux; dir_y = -uy; break;
            case css::Gradient::Corner::BottomRight: dir_x = ux; dir_y = uy; break;
            case css::Gradient::Corner::BottomLeft: dir_x = -ux; dir_y = uy; break;
            case css::Gradient::Corner::TopLeft: dir_x = -ux; dir_y = -uy; break;
            case css::Gradient::Corner::None: break;
            }
        } else {
            auto const [s, c] = sin_cos_degrees(gradient.angle);
            dir_x = s;
            dir_y = -c;
        }
        float const length = std::fabs(w * dir_x) + std::fabs(h * dir_y);
        std::vector<std::pair<float, Color>> const stops = settle_stops(gradient, length);
        float const cx = tile.x + w / 2;
        float const cy = tile.y + h / 2;
        for (int py = bounds.y; py < bounds.bottom(); ++py) {
            for (int px = bounds.x; px < bounds.right(); ++px) {
                float const rx = static_cast<float>(px) + 0.5f - cx;
                float const ry = static_cast<float>(py) + 0.5f - cy;
                float const t = length > 0 ? (rx * dir_x + ry * dir_y) / length + 0.5f : 0.0f;
                target.blend_pixel(px, py, gradient_color(stops, t, gradient.repeating));
            }
        }
        return;
    }
    // Radial.
    float const cx = tile.x + resolve_length(gradient.center_x, w);
    float const cy = tile.y + resolve_length(gradient.center_y, h);
    float const to_left = cx - tile.x;
    float const to_right = tile.x + w - cx;
    float const to_top = cy - tile.y;
    float const to_bottom = tile.y + h - cy;
    float rx = 0;
    float ry = 0;
    bool const closest = gradient.extent == css::Gradient::Extent::ClosestSide
        || gradient.extent == css::Gradient::Extent::ClosestCorner;
    float const side_x = closest ? std::min(to_left, to_right) : std::max(to_left, to_right);
    float const side_y = closest ? std::min(to_top, to_bottom) : std::max(to_top, to_bottom);
    bool const corner = gradient.extent == css::Gradient::Extent::ClosestCorner
        || gradient.extent == css::Gradient::Extent::FarthestCorner;
    if (gradient.shape == css::Gradient::Shape::Circle) {
        float r = closest ? std::min(side_x, side_y) : std::max(side_x, side_y);
        if (corner)
            r = std::sqrt(side_x * side_x + side_y * side_y);
        rx = r;
        ry = r;
    } else {
        rx = side_x;
        ry = side_y;
        if (corner && side_x > 0 && side_y > 0) {
            // The ellipse through the corner with the sides' aspect ratio.
            float const ratio = side_y / side_x;
            rx = std::sqrt(side_x * side_x + (side_y * side_y) / (ratio * ratio));
            ry = rx * ratio;
        }
    }
    if (rx <= 0 || ry <= 0)
        return;
    std::vector<std::pair<float, Color>> const stops = settle_stops(gradient, rx);
    for (int py = bounds.y; py < bounds.bottom(); ++py) {
        for (int px = bounds.x; px < bounds.right(); ++px) {
            float const dx = (static_cast<float>(px) + 0.5f - cx) / rx;
            float const dy = (static_cast<float>(py) + 0.5f - cy) / ry;
            float const t = std::sqrt(dx * dx + dy * dy);
            target.blend_pixel(px, py, gradient_color(stops, t, gradient.repeating));
        }
    }
}

// The background images of a box, the last layer first: each sized by
// background-size against its positioning area, placed by
// background-position, tiled as background-repeat says, and clipped to
// its background-clip box.
// `layers_from` names the style whose image layers to draw when it is not
// the fragment's own: a background the body handed to the canvas is laid
// out as if it had been written on the root element (css-backgrounds-3
// §2.11.2), so the layers come from the body and the box from the root.
void paint_background_layers(Context& context, Fragment const& fragment, Rect const* over = nullptr,
    ComputedStyle const* layers_from = nullptr)
{
    ComputedStyle const& style = layers_from ? *layers_from : *fragment.style;
    std::vector<css::BackgroundImage> const& images = *style.background_images;
    auto const pick = [](auto const* list, std::size_t i, auto fallback) {
        if (!list || list->empty())
            return fallback;
        return (*list)[i % list->size()];
    };
    std::optional<Rect> const saved = context.target.clip();
    for (std::size_t n = images.size(); n-- > 0;) {
        css::BackgroundImage const& image = images[n];
        if (image.none())
            continue;
        Bitmap const* bitmap = nullptr;
        if (!image.url.empty()) {
            if (!context.backgrounds)
                continue;
            auto const it = context.backgrounds->find(image.url);
            if (it == context.backgrounds->end() || !it->second)
                continue;
            bitmap = it->second.get();
        }
        css::BackgroundRepeatPair const repeat
            = pick(style.background_repeats.get(), n, css::BackgroundRepeatPair {});
        css::BackgroundPosition const position
            = pick(style.background_positions.get(), n, css::BackgroundPosition {});
        css::BackgroundSize const size = pick(style.background_sizes.get(), n, css::BackgroundSize {});
        css::BackgroundBox const origin = pick(style.background_origins.get(), n, css::BackgroundBox::PaddingBox);
        css::BackgroundBox const clip = pick(style.background_clips.get(), n, css::BackgroundBox::BorderBox);
        Area const area = box_of(context, fragment, origin);
        // The canvas takes the root's picture over the whole surface,
        // laid out from where the root's own box would have started it.
        Area const painting = over ? Area { static_cast<float>(over->x), static_cast<float>(over->y),
                                        static_cast<float>(over->width), static_cast<float>(over->height) }
                                   : box_of(context, fragment, clip);
        Rect clip_rect = snap(painting.x, painting.y, painting.width, painting.height);
        if (saved)
            clip_rect = intersect(clip_rect, *saved);
        if (clip_rect.width <= 0 || clip_rect.height <= 0)
            continue;
        // The image's size.
        float const natural_w = bitmap ? static_cast<float>(bitmap->width()) : area.width;
        float const natural_h = bitmap ? static_cast<float>(bitmap->height()) : area.height;
        float w = natural_w;
        float h = natural_h;
        switch (size.kind) {
        case css::BackgroundSize::Kind::Auto:
            break;
        case css::BackgroundSize::Kind::Cover:
        case css::BackgroundSize::Kind::Contain:
            if (natural_w > 0 && natural_h > 0) {
                float const sx = area.width / natural_w;
                float const sy = area.height / natural_h;
                float const s = size.kind == css::BackgroundSize::Kind::Cover ? std::max(sx, sy) : std::min(sx, sy);
                w = natural_w * s;
                h = natural_h * s;
            }
            break;
        case css::BackgroundSize::Kind::Lengths:
            if (!size.width.is_auto())
                w = resolve_length(size.width, area.width);
            if (!size.height.is_auto())
                h = resolve_length(size.height, area.height);
            // A picture keeps its ratio through an auto side; a gradient,
            // having none, takes the area's size there.
            if (bitmap && size.width.is_auto() && !size.height.is_auto() && natural_h > 0)
                w = h * natural_w / natural_h;
            if (bitmap && !size.width.is_auto() && size.height.is_auto() && natural_w > 0)
                h = w * natural_h / natural_w;
            break;
        }
        // `round` stretches the tile in its direction until a whole
        // number of them fills the area (css-backgrounds-3 §3.5); when
        // the other side was left auto and does not round itself, it
        // follows so a picture keeps its shape.
        bool const round_x = repeat.x == css::BackgroundRepeat::Round;
        bool const round_y = repeat.y == css::BackgroundRepeat::Round;
        if (round_x && w > 0 && area.width > 0)
            w = area.width / std::max(1.0f, std::floor(area.width / w + 0.5f));
        if (round_y && h > 0 && area.height > 0)
            h = area.height / std::max(1.0f, std::floor(area.height / h + 0.5f));
        if (bitmap && natural_w > 0 && natural_h > 0 && size.kind == css::BackgroundSize::Kind::Auto) {
            if (round_x && !round_y)
                h = w * natural_h / natural_w;
            else if (round_y && !round_x)
                w = h * natural_w / natural_h;
        }
        if (w < 0.5f || h < 0.5f)
            continue;
        // Its position: a percentage aligns the image's point with the area's.
        auto const offset = [](css::LengthPercent const& length, float room, float extent) {
            if (length.kind == css::LengthPercent::Kind::Percent)
                return (room - extent) * length.value / 100.0f;
            if (length.kind == css::LengthPercent::Kind::Calc)
                return length.value + (room - extent) * length.percent / 100.0f;
            return resolve_length(length, room);
        };
        float const x0 = area.x + offset(position.x, area.width, w);
        float const y0 = area.y + offset(position.y, area.height, h);
        // The tiles: where the first one goes, how far apart they stand,
        // and where to stop. `space` fits as many whole tiles as it can
        // and shares the room left over between them, so the first and
        // last touch the edges and background-position has no say;
        // `round` and `repeat` tile edge to edge from the position.
        auto const tiling = [](css::BackgroundRepeat how, float first, float extent, float from,
                                float room, float& step, float& start) {
            step = extent;
            start = first;
            if (how == css::BackgroundRepeat::NoRepeat)
                return first + extent;
            if (how == css::BackgroundRepeat::Space) {
                float const count = std::floor(room / extent);
                if (count < 2)
                    return first + extent; // no room for two: it stays where it was put
                step = extent + (room - count * extent) / (count - 1);
                start = from;
                return from + room;
            }
            start = first - std::ceil((first - from) / extent) * extent;
            return from + room;
        };
        float step_x = w;
        float step_y = h;
        float start_x = x0;
        float start_y = y0;
        float const end_x = tiling(repeat.x, x0, w, painting.x, painting.width, step_x, start_x);
        float const end_y = tiling(repeat.y, y0, h, painting.y, painting.height, step_y, start_y);
        context.target.set_clip(clip_rect);
        std::size_t const rounds = context.target.round_clip_depth();
        if (!over && style.rounded()) {
            RoundedRect const curve = rounded_area(context, fragment, clip);
            if (!curve.is_rectangular())
                context.target.push_round_clip(curve);
        }
        int tiles = 0;
        for (float ty = start_y; ty < end_y && tiles < 100000; ty += step_y) {
            for (float tx = start_x; tx < end_x && tiles < 100000; tx += step_x) {
                ++tiles;
                if (bitmap)
                    context.target.draw_scaled(*bitmap, snap(tx, ty, w, h));
                else
                    paint_gradient(context.target, *image.gradient, Area { tx, ty, w, h }, clip_rect);
            }
        }
        context.target.truncate_round_clips(rounds);
        context.target.set_clip(saved);
    }
}

// The four borders of a box whose corners are curved: the ring between
// the border box's shape and the padding box's, every pixel taking the
// color of the side whose edge is nearest measured in that side's own
// width — which puts the seam between two colors on the corner's
// diagonal, where CSS 2.1 §8.5.4 asks for it.
void paint_rounded_borders(Context& context, Fragment const& fragment, RoundedRect const& outer)
{
    ComputedStyle const& style = *fragment.style;
    auto const drawn = [](css::BorderSide const& side) {
        return side.style == BorderStyle::Solid && side.width > 0 ? side.width : 0.0f;
    };
    float const top = drawn(style.border_top);
    float const right = drawn(style.border_right);
    float const bottom = drawn(style.border_bottom);
    float const left = drawn(style.border_left);
    if (top <= 0 && right <= 0 && bottom <= 0 && left <= 0)
        return;
    RoundedRect const inner = outer.inset(left, top, right, bottom);
    float const outer_right = outer.x + outer.width;
    float const outer_bottom = outer.y + outer.height;
    auto const side_at = [&](float px, float py, Color& color) {
        float nearest = 0;
        bool found = false;
        auto const consider = [&](float width, Color const& candidate, float distance) {
            if (width <= 0)
                return;
            float const reach = distance / width;
            if (found && reach >= nearest)
                return;
            nearest = reach;
            color = candidate;
            found = true;
        };
        consider(top, style.border_top.color, py - outer.y);
        consider(right, style.border_right.color, outer_right - px);
        consider(bottom, style.border_bottom.color, outer_bottom - py);
        consider(left, style.border_left.color, px - outer.x);
        return found;
    };
    // One color all round has no seam to find.
    bool const one_color = [&] {
        Color const* first = nullptr;
        for (auto const& [width, color] : { std::pair { top, &style.border_top.color }, std::pair { right, &style.border_right.color },
                 std::pair { bottom, &style.border_bottom.color }, std::pair { left, &style.border_left.color } }) {
            if (width <= 0)
                continue;
            if (first && !(*first == *color))
                return false;
            first = color;
        }
        return true;
    }();
    if (one_color) {
        context.target.fill_ring(outer, inner, side_at);
        return;
    }
    // The seam between two colors runs along the corner's diagonal: a
    // pixel it crosses takes its share of each side's color.
    context.target.fill_ring(outer, inner, [&](float px, float py, Color& color) {
        float red = 0;
        float green = 0;
        float blue = 0;
        float alpha = 0;
        int found = 0;
        for (float const dy : { -0.25f, 0.25f }) {
            for (float const dx : { -0.25f, 0.25f }) {
                Color sample;
                if (!side_at(px + dx, py + dy, sample))
                    continue;
                ++found;
                float const share = static_cast<float>(sample.a) / 255.0f;
                red += static_cast<float>(sample.r) * share;
                green += static_cast<float>(sample.g) * share;
                blue += static_cast<float>(sample.b) * share;
                alpha += share;
            }
        }
        if (found == 0)
            return false;
        if (alpha <= 0) {
            color = Color { 0, 0, 0, 0 };
            return true;
        }
        color = Color::rgb(static_cast<std::uint8_t>(std::clamp(red / alpha + 0.5f, 0.0f, 255.0f)),
            static_cast<std::uint8_t>(std::clamp(green / alpha + 0.5f, 0.0f, 255.0f)),
            static_cast<std::uint8_t>(std::clamp(blue / alpha + 0.5f, 0.0f, 255.0f)));
        color.a = static_cast<std::uint8_t>(std::clamp(alpha / 4 * 255.0f + 0.5f, 0.0f, 255.0f));
        return true;
    });
}

// --- Shadows ----------------------------------------------------------------------

// A shadow larger than this many pixels is laid down unblurred: the blur
// costs time in proportion, and nothing that size is a soft edge anyone sees.
constexpr std::size_t max_blurred_shadow_pixels = std::size_t { 1 } << 24;

// css-backgrounds-3 §6.1.1 and §4.2's "outset-adjusted border radius": a
// corner's radius moved by a spread distance. Shrunk, it loses the spread
// and stops at zero. Grown — an outer shadow's shape, or an inset shadow's
// hole under a negative spread, as the engines all draw it — a small radius
// on a rectangular box grows by less than the spread, so that a nearly
// sharp corner does not turn round; `coverage` (twice the corner's smaller
// share of the box's sides) leaves a nearly elliptical box as round as it
// was.
float spread_radius(float radius, float spread, float coverage)
{
    if (spread <= 0)
        return std::max(0.0f, radius + spread);
    if (radius > spread || coverage > 1)
        return radius + spread;
    float const t = 1 - radius / spread;
    return radius + spread * (1 - t * t * t * (1 - coverage * coverage * coverage));
}

// A box's shape moved by an offset and grown (or, below zero, shrunk) by a
// spread distance on every side: an outer shadow's shape, or an inset
// shadow's hole.
RoundedRect spread_shape(RoundedRect const& box, float dx, float dy, float spread)
{
    RoundedRect shape = box;
    shape.x = box.x + dx - spread;
    shape.y = box.y + dy - spread;
    shape.width = std::max(0.0f, box.width + 2 * spread);
    shape.height = std::max(0.0f, box.height + 2 * spread);
    auto const corner = [&](float& x, float& y) {
        float const coverage = box.width > 0 && box.height > 0 ? 2 * std::min(x / box.width, y / box.height) : 0;
        x = spread_radius(x, spread, coverage);
        y = spread_radius(y, spread, coverage);
    };
    corner(shape.top_left_x, shape.top_left_y);
    corner(shape.top_right_x, shape.top_right_y);
    corner(shape.bottom_right_x, shape.bottom_right_y);
    corner(shape.bottom_left_x, shape.bottom_left_y);
    shape.settle();
    return shape;
}

Rect intersected(Rect const& a, Rect const& b)
{
    int const left = std::max(a.x, b.x);
    int const top = std::max(a.y, b.y);
    int const right = std::min(a.x + a.width, b.x + b.width);
    int const bottom = std::min(a.y + a.height, b.y + b.height);
    return Rect { left, top, std::max(0, right - left), std::max(0, bottom - top) };
}

// The pixels a shadow blurred `margin` beyond `bounds` can reach and still
// be seen: no further than that reach beyond the target's edges.
Rect shadow_work_area(Context const& context, Rect bounds, int margin)
{
    Rect const grown { bounds.x - margin, bounds.y - margin, bounds.width + 2 * margin, bounds.height + 2 * margin };
    Rect const reach { -margin, -margin, context.target.width() + 2 * margin, context.target.height() + 2 * margin };
    return intersected(grown, reach);
}

// How far a blur radius reaches: three deviations, the blur's deviation
// being half the radius (css-backgrounds-3 §7.1).
int blur_margin(float blur)
{
    return blur > 0 ? static_cast<int>(std::ceil(static_cast<double>(blur) / 2 * 3)) + 1 : 0;
}

// `color` laid over the target through `mask` (one value per pixel of
// `area`, 0 to 1).
void paint_through_mask(Context& context, std::vector<float> const& mask, Rect const& area, Color color)
{
    for (int y = 0; y < area.height; ++y) {
        for (int x = 0; x < area.width; ++x) {
            float const a = mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(area.width) + static_cast<std::size_t>(x)];
            if (a <= 0)
                continue;
            Color shade = color;
            shade.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(color.a) * std::min(a, 1.0f)));
            if (shade.a != 0)
                context.target.blend_pixel(area.x + x, area.y + y, shade);
        }
    }
}

// The coverage of `shape` over `area`, 0 to 1 per pixel.
std::vector<float> coverage_of(RoundedRect const& shape, Rect const& area)
{
    std::vector<float> plane(static_cast<std::size_t>(area.width) * static_cast<std::size_t>(area.height), 0);
    Rect const inside = intersected(area, shape.bounds());
    for (int y = inside.y; y < inside.y + inside.height; ++y) {
        for (int x = inside.x; x < inside.x + inside.width; ++x)
            plane[static_cast<std::size_t>(y - area.y) * static_cast<std::size_t>(area.width) + static_cast<std::size_t>(x - area.x)]
                = static_cast<float>(shape.coverage(x, y)) / 255.0f;
    }
    return plane;
}

// css-backgrounds-3 §7.1: the outer shadows, under the background, drawn
// only outside the border box; the first in the list on top.
void paint_outer_shadows(Context& context, Fragment const& fragment, RoundedRect const& border_box)
{
    ComputedStyle const& style = *fragment.style;
    for (auto it = style.box_shadow->rbegin(); it != style.box_shadow->rend(); ++it) {
        css::Shadow const& shadow = *it;
        Color const color = shadow.current_color ? style.color : shadow.color;
        if (shadow.inset || color.a == 0)
            continue;
        RoundedRect const shape = spread_shape(border_box, shadow.x, shadow.y, shadow.spread);
        if (shape.is_empty())
            continue;
        int const margin = blur_margin(shadow.blur);
        Rect const area = shadow_work_area(context, shape.bounds(), margin);
        if (area.width <= 0 || area.height <= 0)
            continue;
        std::vector<float> mask = coverage_of(shape, area);
        if (shadow.blur > 0 && mask.size() <= max_blurred_shadow_pixels)
            gaussian_plane(mask, area.width, area.height, static_cast<double>(shadow.blur) / 2);
        // The box itself casts it: none falls inside the border box.
        Rect const under = intersected(area, border_box.bounds());
        for (int y = under.y; y < under.y + under.height; ++y) {
            for (int x = under.x; x < under.x + under.width; ++x) {
                float& a = mask[static_cast<std::size_t>(y - area.y) * static_cast<std::size_t>(area.width) + static_cast<std::size_t>(x - area.x)];
                a *= 1.0f - static_cast<float>(border_box.coverage(x, y)) / 255.0f;
            }
        }
        paint_through_mask(context, mask, area, color);
    }
}

// The inset shadows, over the background and under the border: the padding
// box shaded but for a hole the shadow's shape cuts, moved by the offset and
// shrunk by the spread, its edge blurred.
void paint_inset_shadows(Context& context, Fragment const& fragment)
{
    ComputedStyle const& style = *fragment.style;
    RoundedRect const padding = rounded_area(context, fragment, css::BackgroundBox::PaddingBox);
    if (padding.is_empty())
        return;
    for (auto it = style.box_shadow->rbegin(); it != style.box_shadow->rend(); ++it) {
        css::Shadow const& shadow = *it;
        Color const color = shadow.current_color ? style.color : shadow.color;
        if (!shadow.inset || color.a == 0)
            continue;
        RoundedRect const hole = spread_shape(padding, shadow.x, shadow.y, -shadow.spread);
        int const margin = blur_margin(shadow.blur);
        Rect const area = shadow_work_area(context, padding.bounds(), margin);
        if (area.width <= 0 || area.height <= 0)
            continue;
        std::vector<float> mask = hole.is_empty()
            ? std::vector<float>(static_cast<std::size_t>(area.width) * static_cast<std::size_t>(area.height), 0)
            : coverage_of(hole, area);
        if (shadow.blur > 0 && mask.size() <= max_blurred_shadow_pixels)
            gaussian_plane(mask, area.width, area.height, static_cast<double>(shadow.blur) / 2);
        for (int y = 0; y < area.height; ++y) {
            for (int x = 0; x < area.width; ++x) {
                float& a = mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(area.width) + static_cast<std::size_t>(x)];
                a = (1.0f - a) * static_cast<float>(padding.coverage(area.x + x, area.y + y)) / 255.0f;
            }
        }
        paint_through_mask(context, mask, area, color);
    }
}

bool casts_shadow(ComputedStyle const& style, bool inset)
{
    if (!style.box_shadow)
        return false;
    return std::any_of(style.box_shadow->begin(), style.box_shadow->end(), [inset](css::Shadow const& s) { return s.inset == inset; });
}

void paint_background_and_borders(Context& context, Fragment const& fragment,
    bool skip_it)
{
    ComputedStyle const& style = *fragment.style;
    bool const skip_background = skip_it || &fragment == context.canvas_owner;
    float const x = fragment.x + context.dx;
    float const y = fragment.y + context.dy;
    RoundedRect const shape = style.rounded() ? rounded_box(context, fragment) : RoundedRect {};
    bool const round = !shape.is_rectangular();
    if (casts_shadow(style, false))
        paint_outer_shadows(context, fragment, rounded_box(context, fragment));
    if (!skip_background && style.background_color.a != 0) {
        // The color fills the last layer's clip box.
        css::BackgroundBox clip = css::BackgroundBox::BorderBox;
        if (style.background_clips && !style.background_clips->empty())
            clip = style.background_clips->back();
        if (round) {
            context.target.fill_rounded(rounded_area(context, fragment, clip), style.background_color);
        } else {
            Area const area = box_of(context, fragment, clip);
            context.target.fill_rect(
                snap(area.x, area.y, area.width, area.height), style.background_color);
        }
    }
    if (!skip_background && style.background_images && !style.background_images->empty())
        paint_background_layers(context, fragment);
    if (casts_shadow(style, true))
        paint_inset_shadows(context, fragment);

    if (round) {
        paint_rounded_borders(context, fragment, shape);
        return;
    }
    // Sides that meet in different colors, or in one that lets the page
    // show through, meet along the corner's diagonal (CSS 2.1 §8.5.4):
    // each side is the trapezoid, or the triangle, the diagonals leave it,
    // and no corner is painted twice.
    {
        int drawn = 0;
        bool same = true;
        bool opaque = true;
        Color const* first = nullptr;
        for (css::BorderSide const* const side : { &style.border_top, &style.border_right, &style.border_bottom, &style.border_left }) {
            if (side->style != BorderStyle::Solid || side->width <= 0)
                continue;
            ++drawn;
            opaque = opaque && side->color.a == 255;
            if (first && !(*first == side->color))
                same = false;
            first = &side->color;
        }
        if (drawn > 1 && (!same || !opaque)) {
            paint_rounded_borders(context, fragment, rounded_box(context, fragment));
            return;
        }
    }

    if (style.border_top.style == BorderStyle::Solid && style.border_top.width > 0)
        context.target.fill_rect(snap(x, y, fragment.width, style.border_top.width),
            style.border_top.color);
    if (style.border_bottom.style == BorderStyle::Solid && style.border_bottom.width > 0)
        context.target.fill_rect(snap(x, y + fragment.height - style.border_bottom.width,
                                     fragment.width, style.border_bottom.width),
            style.border_bottom.color);
    if (style.border_left.style == BorderStyle::Solid && style.border_left.width > 0)
        context.target.fill_rect(snap(x, y, style.border_left.width, fragment.height),
            style.border_left.color);
    if (style.border_right.style == BorderStyle::Solid && style.border_right.width > 0)
        context.target.fill_rect(snap(x + fragment.width - style.border_right.width, y,
                                     style.border_right.width, fragment.height),
            style.border_right.color);
}

// A run on a vertical line: the text advances down the page — up, in
// `sideways-lr` — and every glyph is turned a quarter circle, clockwise
// unless the line reads upwards. `x` is where the run's text begins along
// the line and `baseline_y` is the page x its baseline stands at.
void paint_vertical_run(Context& context, TextRun const& run)
{
    ComputedStyle const& style = *run.style;
    float const baseline_x = run.baseline_y + context.dx;
    if (baseline_x + style.font_size < 0
        || baseline_x - style.font_size > static_cast<float>(context.target.width()))
        return;
    bool const clockwise = !css::inline_runs_up(run.mode);
    float const start = run.x + context.dy + (clockwise ? 0.0f : run.width);
    // The run drawn onto a target with its baseline at `line` and its pen
    // starting at `from`: the glyphs, then the decoration line.
    auto const draw = [&](Bitmap& onto, float line, float from, Color ink) {
        float y = from;
        for (char32_t const c : run.text) {
            text::FontStack::Glyph const glyph = run.fonts->glyph_for(c);
            float const advance = glyph.face->advance(glyph.glyph, style.font_size);
            float const pen = clockwise ? y : y - advance;
            bool const designed = glyph.face->designs_every_style();
            glyph.face->draw_glyph_turned(onto, glyph.glyph, line, pen, style.font_size,
                ink, style.drawn_bold(designed), style.drawn_slant(designed), clockwise);
            float step = advance + style.letter_spacing;
            if (c == U' ')
                step += style.word_spacing;
            y += clockwise ? step : -step;
        }
        if (style.text_decoration == css::TextDecorationLine::None || run.text.empty())
            return;
        float const thickness = std::max(1.0f, style.font_size / 14.0f);
        // The line sits on the side the ascenders do not point at.
        float const offset = style.text_decoration == css::TextDecorationLine::Underline
            ? -(style.font_size * 2.0f / 32.0f + 1.0f)
            : style.font_size * 8.0f / 32.0f;
        float const line_x = clockwise ? line + offset : line - offset;
        onto.fill_rect(snap(line_x, std::min(from, y), thickness, std::abs(y - from)), ink);
    };
    // The shadows, as a horizontal run's: the run drawn apart in each,
    // blurred, and laid down in its color, the first on top.
    if (style.text_shadow && !run.text.empty()) {
        float const pad = style.font_size / 3;
        Rect const ink = snap(baseline_x - style.font_size * 1.25f, run.x + context.dy - pad, style.font_size * 2.5f, run.width + 2 * pad);
        for (auto it = style.text_shadow->rbegin(); it != style.text_shadow->rend(); ++it) {
            css::Shadow const& shadow = *it;
            Color const shade = shadow.current_color ? style.color : shadow.color;
            if (shade.a == 0)
                continue;
            int const margin = blur_margin(shadow.blur);
            Rect const moved = snap(static_cast<float>(ink.x) + shadow.x, static_cast<float>(ink.y) + shadow.y,
                static_cast<float>(ink.width), static_cast<float>(ink.height));
            Rect const area = shadow_work_area(context, moved, margin);
            if (area.width <= 0 || area.height <= 0)
                continue;
            Bitmap apart(area.width, area.height, Color::rgba(0, 0, 0, 0));
            draw(apart, baseline_x + shadow.x - static_cast<float>(area.x), start + shadow.y - static_cast<float>(area.y), Color::rgb(0, 0, 0));
            std::vector<float> mask(static_cast<std::size_t>(area.width) * static_cast<std::size_t>(area.height));
            std::vector<std::uint8_t> const& pixels = apart.pixels();
            for (std::size_t i = 0; i < mask.size(); ++i)
                mask[i] = static_cast<float>(pixels[i * 4 + 3]) / 255.0f;
            if (shadow.blur > 0 && mask.size() <= max_blurred_shadow_pixels)
                gaussian_plane(mask, area.width, area.height, static_cast<double>(shadow.blur) / 2);
            paint_through_mask(context, mask, area, shade);
        }
    }
    draw(context.target, baseline_x, start, style.color);
}

void paint_whole_run(Context& context, TextRun const& run)
{
    if (!run.style || !run.fonts)
        return;
    if (css::is_vertical(run.mode)) {
        paint_vertical_run(context, run);
        return;
    }
    ComputedStyle const& style = *run.style;
    float const baseline = run.baseline_y + context.dy;
    // A run entirely above or below the target draws nothing: skip it before
    // rasterizing — a scrolled page has thousands of such runs. One em of
    // margin covers descenders, decorations, and synthesized styles.
    if (baseline + style.font_size < 0
        || baseline - style.font_size > static_cast<float>(context.target.height()))
        return;

    if (!run.fonts)
        return;
    bool const kern = style.font_kerning != css::FontKerning::None;
    float const start_x = run.x + context.dx;
    // A placeholder wears what ::placeholder gives it, its color at its
    // opacity; given no color of its own, it is the field's at a little over
    // half strength, so that it reads as a hint on a light field and on a
    // dark one.
    Color color = style.color;
    if (run.placeholder) {
        if (style.placeholder && style.placeholder->color != style.color)
            color = style.placeholder->color;
        else
            color.a = static_cast<std::uint8_t>(color.a * 0.54f);
        if (style.placeholder)
            color.a = static_cast<std::uint8_t>(std::lround(color.a * std::clamp(style.placeholder->opacity, 0.0f, 1.0f)));
    }
    // The run drawn onto a target with its pen at (`left`, `line`): the
    // glyphs, then the decoration line. Measures it when `onto` is null.
    // Returns the run's width.
    auto const draw = [&](Bitmap* onto, float left, float line, Color ink, Color decoration) {
        float x = left;
        text::FontStack::Glyph previous { nullptr, 0 };
        for (char32_t const c : run.text) {
            text::FontStack::Glyph const glyph = run.fonts->glyph_for(c);
            // The steps layout measured: a pair's kerning before the glyph,
            // the glyph's advance, a letter's worth of extra room after it,
            // and a word's worth after a word separator.
            if (kern && previous.face == glyph.face)
                x += glyph.face->kerning(previous.glyph, glyph.glyph, style.font_size);
            bool const designed = glyph.face->designs_every_style();
            if (onto)
                glyph.face->draw_glyph(*onto, glyph.glyph, x, line, style.font_size,
                    ink, style.drawn_bold(designed), style.drawn_slant(designed));
            x += glyph.face->advance(glyph.glyph, style.font_size) + style.letter_spacing;
            if (c == U' ')
                x += style.word_spacing;
            previous = glyph;
        }
        float const width = x - left;
        if (!onto || style.text_decoration == css::TextDecorationLine::None || run.text.empty())
            return width;
        float const thickness = std::max(1.0f, style.font_size / 14.0f);
        float line_y;
        if (style.text_decoration == css::TextDecorationLine::Underline)
            line_y = line + style.font_size * 2.0f / 32.0f + 1.0f;
        else
            line_y = line - style.font_size * 8.0f / 32.0f;
        onto->fill_rect(snap(left, line_y, width, thickness), decoration);
        return width;
    };
    // css-text-decor-4 §7: the shadows under the text, the first on top, each
    // the run and its decoration drawn apart, blurred, and laid down in the
    // shadow's color.
    if (style.text_shadow && !run.text.empty()) {
        float const width = draw(nullptr, start_x, baseline, color, style.color);
        // The run's ink: an em and a quarter above the baseline, half an em
        // below, a third of an em to either side for slants and overhangs.
        float const pad = style.font_size / 3;
        Rect const ink = snap(start_x - pad, baseline - style.font_size * 1.25f, width + 2 * pad, style.font_size * 1.75f);
        for (auto it = style.text_shadow->rbegin(); it != style.text_shadow->rend(); ++it) {
            css::Shadow const& shadow = *it;
            Color const shade = shadow.current_color ? style.color : shadow.color;
            if (shade.a == 0)
                continue;
            int const margin = blur_margin(shadow.blur);
            Rect const moved = snap(static_cast<float>(ink.x) + shadow.x, static_cast<float>(ink.y) + shadow.y,
                static_cast<float>(ink.width), static_cast<float>(ink.height));
            Rect const area = shadow_work_area(context, moved, margin);
            if (area.width <= 0 || area.height <= 0)
                continue;
            Bitmap apart(area.width, area.height, Color::rgba(0, 0, 0, 0));
            Color const opaque = Color::rgb(0, 0, 0);
            draw(&apart, start_x + shadow.x - static_cast<float>(area.x), baseline + shadow.y - static_cast<float>(area.y), opaque, opaque);
            std::vector<float> mask(static_cast<std::size_t>(area.width) * static_cast<std::size_t>(area.height));
            std::vector<std::uint8_t> const& pixels = apart.pixels();
            for (std::size_t i = 0; i < mask.size(); ++i)
                mask[i] = static_cast<float>(pixels[i * 4 + 3]) / 255.0f;
            if (shadow.blur > 0 && mask.size() <= max_blurred_shadow_pixels)
                gaussian_plane(mask, area.width, area.height, static_cast<double>(shadow.blur) / 2);
            paint_through_mask(context, mask, area, shade);
        }
    }
    draw(&context.target, start_x, baseline, color, style.color);
}

// A run as the page shows it: whole, or the part of it text-overflow left.
void paint_run(Context& context, TextRun const& run)
{
    if (context.elisions && !context.elisions->runs.empty()) {
        if (auto const it = context.elisions->runs.find(&run); it != context.elisions->runs.end()) {
            Elisions::Kept const& kept = it->second;
            if (kept.from >= kept.to)
                return;
            TextRun part = run;
            part.text = run.text.substr(kept.from, kept.to - kept.from);
            part.x += kept.shift;
            paint_whole_run(context, part);
            return;
        }
    }
    paint_whole_run(context, run);
}

// Whether text-overflow has taken a box off its line.
bool elided(Context const& context, Fragment const& fragment)
{
    return context.elisions && !context.elisions->hidden.empty() && context.elisions->hidden.contains(&fragment);
}

// Where each character of a run begins, from the run's left edge, and where
// the last one ends: the pen's steps as `paint_whole_run` takes them.
std::vector<float> stops_of(TextRun const& run)
{
    ComputedStyle const& style = *run.style;
    bool const kern = style.font_kerning != css::FontKerning::None;
    std::vector<float> stops;
    stops.reserve(run.text.size() + 1);
    float x = 0;
    text::FontStack::Glyph previous { nullptr, 0 };
    for (char32_t const c : run.text) {
        text::FontStack::Glyph const glyph = run.fonts->glyph_for(c);
        if (kern && previous.face == glyph.face)
            x += glyph.face->kerning(previous.glyph, glyph.glyph, style.font_size);
        stops.push_back(x);
        x += glyph.face->advance(glyph.glyph, style.font_size) + style.letter_spacing;
        if (c == U' ')
            x += style.word_spacing;
        previous = glyph;
    }
    stops.push_back(x);
    return stops;
}

// Whether a run may be cut in front of its character `at`: not inside what
// a reader takes for one character — a letter and the marks on it, an emoji
// and what is joined to it. `reversed` runs hold their text last character
// first, so the mark comes before its letter there.
bool cut_allowed(TextRun const& run, std::size_t at)
{
    if (at == 0 || at >= run.text.size())
        return true;
    char32_t const earlier = run.source_reversed ? run.text[at] : run.text[at - 1];
    char32_t const later = run.source_reversed ? run.text[at - 1] : run.text[at];
    auto const joins = [](char32_t c) {
        return is_combining_mark(c) || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x1F3FB && c <= 0x1F3FF);
    };
    return !joins(later) && earlier != 0x200D;
}

// One thing on a line that text-overflow hides or keeps: a run of text, or
// an atomic inline.
struct LineItem {
    TextRun const* run = nullptr;
    Fragment const* box = nullptr;
    float left = 0;
    float right = 0;
};

// What a fragment holds of each line, by the line's number: its own runs,
// the atomic inlines placed on the line, and what the inline boxes that
// took their words with them hold.
void gather_line_items(Fragment const& holder, std::unordered_map<std::uint32_t, std::vector<LineItem>>& lines)
{
    for (TextRun const& run : holder.runs) {
        if (run.line == 0 || run.swapped || run.mode != css::WritingMode::HorizontalTb || !run.style || !run.fonts
            || run.text.empty())
            continue;
        lines[run.line].push_back(LineItem { &run, nullptr, run.x, run.x + run.width });
    }
    for (Fragment const& child : holder.children) {
        if (child.line == 0 || child.floating || child.out_of_flow || !child.style)
            continue;
        if (child.style->display == css::Display::Inline && !child.image && !child.control)
            gather_line_items(child, lines);
        else
            lines[child.line].push_back(LineItem { nullptr, &child, child.x, child.x + child.width });
    }
}

// Ends the lines one fragment holds for a block that ends its lines in an
// ellipsis: `holder` is the block itself or an anonymous block inside it.
void plan_elision_of(Elisions& plan, Fragment const& block, Fragment const& holder)
{
    plan.marks.erase(&holder);
    if (holder.lines.empty())
        return;
    Fragment::Elision const& edges = *block.elision;
    ComputedStyle const& style = *block.style;
    bool const rtl = style.direction == css::Direction::Rtl;
    std::u32string const ellipsis = U"\u2026";
    float const room = edges.fonts->measure(ellipsis, style.font_size, style.font_kerning != css::FontKerning::None);
    float const slack = 0.01f;
    std::unordered_map<std::uint32_t, std::vector<LineItem>> lines;
    gather_line_items(holder, lines);
    for (Fragment::LineBox const& line : holder.lines) {
        auto const found = lines.find(line.number);
        if (found == lines.end())
            continue;
        std::vector<LineItem> const& items = found->second;
        float const left = edges.left + line.left_inset;
        float const right = edges.right - line.right_inset;
        bool over = false;
        // The line's first: the one that starts nearest its start edge.
        LineItem const* first = nullptr;
        for (LineItem const& item : items) {
            if (item.right <= item.left)
                continue;
            over = over || (rtl ? item.left < left - slack : item.right > right + slack);
            if (!first || (rtl ? item.right > first->right : item.left < first->left))
                first = &item;
        }
        if (!over || !first)
            continue;
        // How far the line's content may reach, the ellipsis beyond it; and
        // where what is left of it ends, which is where the ellipsis goes.
        float const limit = rtl ? left + room : right - room;
        float end = rtl ? first->right : first->left;
        auto const reaches = [&](float edge) { end = rtl ? std::min(end, edge) : std::max(end, edge); };
        for (LineItem const& item : items) {
            bool const fits = rtl ? item.left >= limit - slack : item.right <= limit + slack;
            if (fits) {
                reaches(rtl ? item.left : item.right);
                continue;
            }
            // The first character or atomic inline of a line is clipped
            // where it runs out, never taken away.
            bool const is_first = &item == first;
            if (item.box) {
                if (is_first)
                    reaches(rtl ? item.left : item.right);
                else
                    plan.hidden.insert(item.box);
                continue;
            }
            TextRun const& run = *item.run;
            std::size_t const count = run.text.size();
            std::vector<float> const stops = stops_of(run);
            if (rtl) {
                // The characters from `from` on show: the first of them
                // begins at or right of the limit.
                std::size_t from = count;
                for (std::size_t i = 0; i < count; ++i) {
                    if (run.x + stops[i] >= limit - slack && cut_allowed(run, i)) {
                        from = i;
                        break;
                    }
                }
                if (is_first && from >= count) {
                    from = count - 1;
                    while (from > 0 && !cut_allowed(run, from))
                        --from;
                }
                plan.runs.insert_or_assign(&run, Elisions::Kept { from, count, from < count ? stops[from] : 0.0f });
                if (from < count)
                    reaches(run.x + stops[from]);
            } else {
                // The characters before `to` show: the last of them ends
                // at or left of the limit.
                std::size_t to = 0;
                for (std::size_t i = count; i > 0; --i) {
                    if (run.x + stops[i] <= limit + slack && cut_allowed(run, i)) {
                        to = i;
                        break;
                    }
                }
                if (is_first && to == 0) {
                    to = 1;
                    while (to < count && !cut_allowed(run, to))
                        ++to;
                }
                plan.runs.insert_or_assign(&run, Elisions::Kept { 0, to, 0.0f });
                if (to > 0)
                    reaches(run.x + (to == count ? run.width : stops[to]));
            }
        }
        // The ellipsis, in the block's own font and on the block's own
        // baseline, whatever it stands in for.
        TextRun mark;
        mark.x = rtl ? end - room : end;
        mark.baseline_y = line.baseline;
        mark.text = ellipsis;
        mark.style = block.style;
        mark.element = block.element;
        mark.fonts = edges.fonts;
        mark.width = room;
        mark.line = line.number;
        plan.marks[&holder].push_back(std::move(mark));
    }
}

// Ends the lines of a block that ends its lines in an ellipsis: its own,
// and those of the anonymous blocks its inline content stands in between
// its block children — whose own lines are theirs to end.
void plan_elision(Context const& context, Fragment const& block)
{
    if (!context.elisions || !block.elision || !block.style || !block.elision->fonts)
        return;
    plan_elision_of(*context.elisions, block, block);
    for (Fragment const& child : block.children) {
        if (!child.style && !child.element)
            plan_elision_of(*context.elisions, block, child);
    }
}

// A form control's look: the page's own background and borders when the
// author gave it any, else the built-in look — a white or gray fill with a
// gray border — and then the check mark, the radio dot, the select's arrow,
// the focus ring and the caret. The text inside is the fragment's runs.
void paint_control(Context& context, Fragment const& fragment)
{
    using layout::ControlKind;
    Fragment::ControlBox const& control = *fragment.control;
    ComputedStyle const& style = *fragment.style;
    float const x = control.x + context.dx;
    float const y = control.y + context.dy;
    Rect const rect = snap(x, y, control.width, control.height);
    if (rect.width <= 0 || rect.height <= 0)
        return;
    auto const solid = [](css::BorderSide const& side) {
        return side.style == BorderStyle::Solid && side.width > 0;
    };
    // A background picture is the author's look as much as a colour is: a
    // button that is an icon — `border: 0; background: url(…)` — has no face
    // of ours behind it.
    bool const pictured = style.background_images && !style.background_images->empty();
    // A <button> says which it is by its declarations instead: its own
    // sheet gives it a background and a border, which the face stands for.
    bool const author_look = control.contents
        ? !control.themed
        : style.author_decorated
            && (style.background_color.a != 0 || pictured || solid(style.border_top) || solid(style.border_bottom)
                || solid(style.border_left) || solid(style.border_right));
    Color const border = control.disabled ? Color::rgb(0xc0, 0xc0, 0xc0) : Color::rgb(0x76, 0x76, 0x76);
    Color const white = Color::rgb(0xff, 0xff, 0xff);
    Color const gray = Color::rgb(0xef, 0xef, 0xef);
    Color const ink = control.disabled ? Color::rgb(0x8d, 0x8d, 0x8d) : Color::rgb(0x1a, 0x1a, 0x1a);
    // The built-in shapes are drawn in CSS px, scaled to the display: a
    // one-px line is `px` device px wide, an inset of n is n of them.
    float const scale = context.device_scale > 0 ? context.device_scale : 1.0f;
    auto const px = [scale](int css) { return std::max(1, static_cast<int>(std::lround(static_cast<float>(css) * scale))); };
    int const line = px(1);
    auto const frame = [&](Rect const& r, Color color) {
        context.target.fill_rect(Rect { r.x, r.y, r.width, line }, color);
        context.target.fill_rect(Rect { r.x, r.y + r.height - line, r.width, line }, color);
        context.target.fill_rect(Rect { r.x, r.y, line, r.height }, color);
        context.target.fill_rect(Rect { r.x + r.width - line, r.y, line, r.height }, color);
    };
    auto const inset = [](Rect const& r, int by) {
        return Rect { r.x + by, r.y + by, r.width - 2 * by, r.height - 2 * by };
    };
    switch (control.kind) {
    case ControlKind::Checkbox:
        context.target.fill_round_rect(rect, px(2), border);
        context.target.fill_round_rect(inset(rect, px(1)), px(2), control.disabled ? gray : white);
        if (control.checked)
            context.target.fill_rect(inset(rect, px(3)), ink);
        break;
    case ControlKind::Radio: {
        int const radius = rect.width / 2;
        context.target.fill_round_rect(rect, radius, border);
        context.target.fill_round_rect(inset(rect, px(1)), radius - px(1), control.disabled ? gray : white);
        if (control.checked)
            context.target.fill_round_rect(inset(rect, px(4)), radius - px(4), ink);
        break;
    }
    default: {
        bool const button = control.kind == ControlKind::Submit || control.kind == ControlKind::Button
            || control.kind == ControlKind::File;
        // A field keeps whichever half of its face the page left it: any
        // border it was given — `border: none` too — takes the frame away,
        // any background the fill, as in every engine (Blink's
        // hasAuthorBorder and hasAuthorBackground). Once its border is the
        // page's, the white is a CSS background (the cascade gives it one),
        // drawn under that border and within its corners.
        bool const field = !control.contents
            && (control.kind == ControlKind::Text || control.kind == ControlKind::Password
                || control.kind == ControlKind::TextArea);
        // A file input's face is its button alone; what was chosen beside it
        // is text on the page.
        Rect const face = control.kind == ControlKind::File && control.button_width > 0
            ? Rect { rect.x, rect.y, std::max(1, static_cast<int>(std::lround(control.button_width))), rect.height }
            : rect;
        if (field ? !style.author_background && !style.author_border : !author_look) {
            if (control.kind == ControlKind::File)
                context.target.fill_rect(face, control.disabled ? gray : Color::rgb(0xe9, 0xe9, 0xed));
            else
                context.target.fill_rect(face, button || control.disabled ? gray : white);
        }
        if (field ? !style.author_border : !author_look)
            frame(face, border);
        if (control.kind == ControlKind::Select) {
            // A small triangle pointing down, near the right edge: four
            // rows of 7, 5, 3 and 1 CSS px, each row `line` device px tall.
            int const tip_x = rect.x + rect.width - px(12);
            int const top = rect.y + rect.height / 2 - px(2);
            for (int row = 0; row < 4; ++row) {
                int const half = static_cast<int>(std::lround(static_cast<float>(3 - row) * scale));
                context.target.fill_rect(Rect { tip_x - half, top + row * line, 2 * half + line, line }, ink);
            }
        }
        break;
    }
    }
    // A selected value: the selection's color behind the text, which is
    // drawn after this.
    for (Fragment::ControlBox::Span const& span : control.selected)
        context.target.fill_rect(snap(span.x0 + context.dx, span.top + context.dy, span.x1 - span.x0, span.bottom - span.top),
            Color::rgba(0x5b, 0x9c, 0xf6, 0x66));
    // Misspelled words: a red wave under each, as every engine marks them
    // (a period of four device px, two tall).
    for (Fragment::ControlBox::Span const& span : control.misspelled) {
        int const x0 = static_cast<int>(std::lround(span.x0 + context.dx));
        int const x1 = static_cast<int>(std::lround(span.x1 + context.dx));
        int const top = static_cast<int>(std::lround(span.top + context.dy));
        int const amplitude = std::max(1, px(1));
        int const period = 2 * std::max(2, px(2));
        Color const red = Color::rgb(0xe0, 0x1b, 0x24);
        for (int wave_x = x0; wave_x < x1; ++wave_x) {
            int const phase = (wave_x - x0) % period;
            int const rise = phase < period / 2 ? phase : period - phase;
            int const wave_y = top + amplitude * 2 - rise * amplitude * 2 / std::max(1, period / 2);
            context.target.fill_rect(Rect { wave_x, wave_y, 1, line }, red);
        }
    }
    // The focus ring is the control's outline now (`:focus-visible
    // { outline: auto }` in the built-in sheet), drawn with every outline.
    if (control.caret_x && g_carets_shown) {
        // A textarea's caret is as tall as its line; a field's stands clear
        // of its edges.
        if (control.kind == ControlKind::TextArea && control.caret_bottom > control.caret_top)
            context.target.fill_rect(snap(*control.caret_x + context.dx, control.caret_top + context.dy, line,
                                         control.caret_bottom - control.caret_top),
                ink);
        else
            context.target.fill_rect(snap(*control.caret_x + context.dx, y + control.pad_top + px(4), line,
                                         control.height - control.pad_top - control.pad_bottom - 2 * px(4)),
                ink);
    }
    if (control.preedit_span && control.preedit_span->second > control.preedit_span->first) {
        // The composing text is underlined, as every editor shows it.
        context.target.fill_rect(snap(control.preedit_span->first + context.dx, y + control.height - px(5),
                                     control.preedit_span->second - control.preedit_span->first, line),
            ink);
    }
}

// `in_own_plane`: the root is being painted flat, on the way through its
// own transform, which is then not applied a second time.
void paint_stacking_context(Context& context, Fragment const& root, bool is_canvas_background_owner, bool in_own_plane = false);
void paint_flow(Context& context, Fragment const& fragment, bool is_canvas_background_owner);

// css-ui-4 §3: a box's outline, around its border box and `outline-offset`
// outside it, taking no room. `auto` is the platform's focus ring: two
// device pixels of the accent the chrome uses. Every style is drawn solid,
// as the borders are.
void paint_outline(Context& context, Fragment const& fragment)
{
    ComputedStyle const* style = fragment.style;
    if (!style || style->visibility != css::Visibility::Visible)
        return;
    css::ComputedStyle::Outline const& outline = style->outline;
    // No style, no outline: an anonymous box's style keeps the initial
    // medium width beside it.
    if (outline.style == css::BorderStyle::None && !outline.automatic)
        return;
    if (outline.width <= 0 && !outline.automatic)
        return;
    float const width = outline.automatic ? 2.0f / context.device_scale : outline.width;
    Color const color = outline.automatic ? Color::rgb(0x00, 0x60, 0xdf) : outline.color;
    if (color.a == 0)
        return;
    float const left = fragment.x + context.dx - outline.offset - width;
    float const top = fragment.y + context.dy - outline.offset - width;
    float const outer_width = fragment.width + 2 * (outline.offset + width);
    float const outer_height = fragment.height + 2 * (outline.offset + width);
    if (outer_width <= 0 || outer_height <= 0)
        return;
    auto const ring = [&](float x, float y, float w, float h, float thickness) {
        context.target.fill_rect(snap(x, y, w, thickness), color);
        context.target.fill_rect(snap(x, y + h - thickness, w, thickness), color);
        context.target.fill_rect(snap(x, y + thickness, thickness, h - 2 * thickness), color);
        context.target.fill_rect(snap(x + w - thickness, y + thickness, thickness, h - 2 * thickness), color);
    };
    ring(left, top, outer_width, outer_height, width);
}

std::optional<Rect> clip_within(Context const& context, Fragment const& fragment, std::optional<Rect> const& current);

// Every outline in a stacking context, after all else in it (CSS 2.1
// Appendix E, step 10), through the clips its boxes sit in; a stacking
// context inside paints its own.
void paint_outlines(Context& context, Fragment const& box)
{
    for (Fragment const& child : box.children) {
        if (child.stacking_context)
            continue;
        paint_outline(context, child);
        if (!child.children.empty()) {
            std::size_t const rounds = context.target.round_clip_depth();
            std::optional<Rect> const restore = context.target.clip();
            context.target.set_clip(clip_within(context, child, restore));
            paint_outlines(context, child);
            context.target.truncate_round_clips(rounds);
            context.target.set_clip(restore);
        }
    }
}

// The box's own background and borders — nothing when its visibility is
// hidden (the box keeps its room).
void paint_box_background(Context& context, Fragment const& fragment, bool skip_background)
{
    // A table's row, row group or column: a background under the cells.
    if (fragment.background && !skip_background && fragment.background->a != 0
        && (!fragment.style || !fragment.style->hidden()))
        context.target.fill_rect(
            snap(fragment.x + context.dx, fragment.y + context.dy, fragment.width, fragment.height),
            *fragment.background);
    if (!fragment.style || fragment.style->hidden())
        return;
    paint_background_and_borders(context, fragment, skip_background);
    // A <button>'s face is its background: under everything inside it,
    // the backgrounds of the blocks it holds among them.
    if (fragment.control && fragment.control->contents)
        paint_control(context, fragment);
}

// Where a picture went, for a host that asked: its rectangle as far as the
// clip and the target let it show.
void note_picture(Context const& context, Bitmap const& picture, Rect drawn)
{
    if (context.pictures == nullptr)
        return;
    int left = std::max(drawn.x, 0);
    int top = std::max(drawn.y, 0);
    int right = std::min(drawn.right(), context.target.width());
    int bottom = std::min(drawn.bottom(), context.target.height());
    if (std::optional<Rect> const& clip = context.target.clip()) {
        left = std::max(left, clip->x);
        top = std::max(top, clip->y);
        right = std::min(right, clip->right());
        bottom = std::min(bottom, clip->bottom());
    }
    if (right > left && bottom > top) {
        context.pictures->push_back({ &picture, Rect { left + context.origin_x, top + context.origin_y, right - left, bottom - top },
            Rect { drawn.x + context.origin_x, drawn.y + context.origin_y, drawn.width, drawn.height } });
    }
}

// A picture into its rectangle: a video's frame smoothly, as browsers draw
// video, anything else with the painter's own filtering. A video's frame
// that the host shows itself is not drawn: its place is left open.
void draw_picture(Context& context, Fragment::ImageBox const& box, Rect drawn)
{
    bool const apart = box.video && context.shown_apart != nullptr
        && std::find(context.shown_apart->begin(), context.shown_apart->end(), box.bitmap.get()) != context.shown_apart->end();
    if (apart)
        context.target.punch(drawn);
    else if (box.video)
        context.target.draw_scaled_opaque(*box.bitmap, drawn);
    else
        context.target.draw_scaled(*box.bitmap, drawn);
    note_picture(context, *box.bitmap, drawn);
    // SASHFOLD_PAINT_TRACE=1: every picture drawn — its size, where, and
    // how many of its pixels are not transparent — the instrument for an
    // image laid out and not seen: rasterized empty, or drawn and covered.
    static bool const traced = [] { char const* const set = std::getenv("SASHFOLD_PAINT_TRACE"); return set != nullptr && set[0] == '1'; }();
    if (traced) {
        unsigned opaque = 0;
        for (int y = 0; y < box.bitmap->height(); ++y) {
            for (int x = 0; x < box.bitmap->width(); ++x)
                opaque += box.bitmap->pixel(x, y).a != 0 ? 1u : 0u;
        }
        std::cerr << "paint: " << (box.video ? "video " : "image ") << box.bitmap->width() << "x" << box.bitmap->height() << " at " << drawn.x << "," << drawn.y
                  << " " << drawn.width << "x" << drawn.height << " opaque " << opaque << (apart ? " (shown apart)" : "") << "\n";
    }
}

// The box's replaced content: its picture or its control. Appendix E
// paints a block-level replaced element's content after the floats, with
// the inline content, and its background before them with the other block
// backgrounds, so the two halves are separate.
void paint_box_replaced(Context& context, Fragment const& fragment)
{
    if (!fragment.style || fragment.style->hidden())
        return;
    if (fragment.control && !fragment.control->contents)
        paint_control(context, fragment);
    if (fragment.image && fragment.image->bitmap) {
        Fragment::ImageBox const& box = *fragment.image;
        // A replaced element's picture is trimmed to the content edge's
        // curve, whatever its overflow says (CSS Backgrounds §5.3).
        std::size_t const rounds = context.target.round_clip_depth();
        if (fragment.style->rounded()) {
            RoundedRect const curve
                = rounded_area(context, fragment, css::BackgroundBox::ContentBox);
            if (!curve.is_rectangular())
                context.target.push_round_clip(curve);
        }
        if (box.drawn) {
            // object-fit: the picture at the shape it keeps, shown through
            // its content box.
            std::optional<Rect> const before = context.target.clip();
            Rect through = snap(box.x + context.dx, box.y + context.dy, box.width, box.height);
            if (before) {
                int const left = std::max(through.x, before->x);
                int const top = std::max(through.y, before->y);
                int const right = std::min(through.right(), before->right());
                int const bottom = std::min(through.bottom(), before->bottom());
                through = Rect { left, top, std::max(0, right - left), std::max(0, bottom - top) };
            }
            context.target.set_clip(through);
            Rect const drawn = snap(box.drawn->x + context.dx, box.drawn->y + context.dy, box.drawn->width, box.drawn->height);
            draw_picture(context, box, drawn);
            context.target.set_clip(before);
        } else {
            draw_picture(context, box, snap(box.x + context.dx, box.y + context.dy, box.width, box.height));
        }
        context.target.truncate_round_clips(rounds);
    }
}

// The box's own painting, whole: background, borders, picture, control.
void paint_box(Context& context, Fragment const& fragment, bool skip_background)
{
    paint_box_background(context, fragment, skip_background);
    paint_box_replaced(context, fragment);
}

Rect intersect(Rect const& a, Rect const& b)
{
    int const left = std::max(a.x, b.x);
    int const top = std::max(a.y, b.y);
    int const right = std::min(a.right(), b.right());
    int const bottom = std::min(a.bottom(), b.bottom());
    return Rect { left, top, std::max(0, right - left), std::max(0, bottom - top) };
}

// The clip a box imposes on what it contains: its padding box, when its
// overflow is not visible and overflow applies to it; else the clip as it was.
std::optional<Rect> clip_within(Context const& context, Fragment const& fragment,
    std::optional<Rect> const& current)
{
    if (!fragment.style || fragment.style->overflow == css::Overflow::Visible
        || !fragment.style->overflow_applies)
        return current;
    ComputedStyle const& style = *fragment.style;
    Rect box = snap(fragment.x + context.dx + style.border_left.width,
        fragment.y + context.dy + style.border_top.width,
        fragment.width - style.border_left.width - style.border_right.width,
        fragment.height - style.border_top.width - style.border_bottom.width);
    // An axis left visible is not clipped at all: `overflow: clip visible`
    // holds the sides in and lets the box run off the top and bottom.
    if (style.overflow_x == css::Overflow::Visible) {
        box.x = 0;
        box.width = context.target.width();
    }
    if (style.overflow_y == css::Overflow::Visible) {
        box.y = 0;
        box.height = context.target.height();
    }
    return current ? intersect(box, *current) : box;
}

// Whether a box's corners cut what it holds. Only a box that clips both
// ways does: with one axis left visible there is nothing to round
// against (CSS Overflow §corner clipping).
bool clips_corners(Fragment const& fragment)
{
    return fragment.style && fragment.style->overflow_applies
        && fragment.style->overflow_x != css::Overflow::Visible
        && fragment.style->overflow_y != css::Overflow::Visible;
}

// Narrows the clip to a box's padding box while its contents paint, when
// its overflow is not visible; returns what to restore. A box with
// curved corners clips along the curve as well.
std::optional<Rect> clip_for(Context& context, Fragment const& fragment)
{
    std::optional<Rect> const previous = context.target.clip();
    context.target.set_clip(clip_within(context, fragment, previous));
    if (clips_corners(fragment) && fragment.style->rounded()) {
        RoundedRect const curve = rounded_area(context, fragment, css::BackgroundBox::PaddingBox);
        if (!curve.is_rectangular())
            context.target.push_round_clip(curve);
    }
    return previous;
}

// A child painted at its own level of the stacking context rather than in
// the flow: a positioned box, at its z-index, or a box that is a stacking
// context for another reason — an opacity below one, a transform — which
// CSS Color §3.2 paints as if it were positioned at level zero.
bool paints_as_layer(Fragment const& child)
{
    return child.positioned || child.stacking_context;
}

// An inline-level box: an inline box's piece on a line, an inline-block,
// an inline flex or grid container, an inline table, an inline replaced
// element — painted with the line it is on. Everything else is
// block-level, the boxes of the flow's own making (anonymous, without a
// style) and the table parts included.
bool inline_level(Fragment const& fragment)
{
    if (!fragment.style)
        return false;
    switch (fragment.style->display) {
    case css::Display::Inline:
    case css::Display::InlineBlock:
    case css::Display::InlineFlex:
    case css::Display::InlineGrid:
    case css::Display::InlineTable:
        return true;
    default:
        return false;
    }
}

bool block_level(Fragment const& fragment)
{
    return !inline_level(fragment);
}

// A box that paints as one unit at the inline-content step, as if it made
// a stacking context of its own with its block descendants and floats
// inside it: an atomic inline — Appendix E says so of an inline-block and
// an inline table — and a flex or grid item, which css-flexbox §4 and
// css-grid §6 paint exactly as inline-blocks. An inline box is not: it is
// open to the flow around it, and a block it was split around is the
// containing block's block-level descendant, painted with the others.
bool paints_whole(Fragment const& parent, Fragment const& child)
{
    if (parent.style) {
        switch (parent.style->display) {
        case css::Display::Flex:
        case css::Display::InlineFlex:
        case css::Display::Grid:
        case css::Display::InlineGrid:
            return true;
        default:
            break;
        }
    }
    return inline_level(child) && child.style->display != css::Display::Inline;
}

// Walks into a box for one of the steps below with the clip the box puts
// on what it holds, and takes the clip off again after.
template <typename Walk>
void descend(Context& context, Fragment const& box, Walk const& walk)
{
    std::size_t const rounds = context.target.round_clip_depth();
    std::optional<Rect> const restore = clip_for(context, box);
    walk(context, box);
    context.target.truncate_round_clips(rounds);
    context.target.set_clip(restore);
}

// CSS 2.1 Appendix E, step 3: the backgrounds and borders of every in-flow
// block-level descendant, in tree order — through an inline box, whose
// own background is inline content, to a block it was split around. The
// floats, the positioned boxes and the stacking contexts are painted at
// their own steps and levels, and an atomic inline paints whole at step 6.
void paint_block_backgrounds(Context& context, Fragment const& box)
{
    for (Fragment const& child : box.children) {
        if (child.floating || paints_as_layer(child) || paints_whole(box, child))
            continue;
        if (block_level(child))
            paint_box_background(context, child, false);
        descend(context, child, paint_block_backgrounds);
    }
}

// Step 4: every non-positioned float in tree order, each as one unit — its
// own box and then its own steps 3 to 6 — except that its positioned
// descendants and the stacking contexts inside it belong to the context
// around it, which is what the layer walk gathers.
void paint_floats(Context& context, Fragment const& box)
{
    for (Fragment const& child : box.children) {
        if (paints_as_layer(child))
            continue;
        if (child.floating) {
            paint_flow(context, child, false);
            continue;
        }
        if (paints_whole(box, child))
            continue;
        descend(context, child, paint_floats);
    }
}

// Step 6: the inline content of the box and of each in-flow block-level
// descendant, in tree order — a block-level replaced element's picture,
// then the pieces of the inline boxes on the lines, each with its own
// background, borders and whatever it holds, the atomic inlines as units,
// and the lines' text. A box's boxes come before its own runs, so a line's
// words are over the inline box drawn under them.
void paint_inline_content(Context& context, Fragment const& box)
{
    for (Fragment const& child : box.children) {
        if (child.floating || paints_as_layer(child) || elided(context, child))
            continue;
        if (paints_whole(box, child)) {
            paint_flow(context, child, false);
        } else if (block_level(child)) {
            paint_box_replaced(context, child);
            descend(context, child, paint_inline_content);
        } else {
            paint_flow(context, child, false);
        }
    }
    for (TextRun const& run : box.runs) {
        if (!run.style->hidden())
            paint_run(context, run);
    }
    // The ellipses its lines end in.
    if (context.elisions && !context.elisions->marks.empty()) {
        if (auto const it = context.elisions->marks.find(&box); it != context.elisions->marks.end()) {
            for (TextRun const& mark : it->second) {
                if (!mark.style->hidden())
                    paint_whole_run(context, mark);
            }
        }
    }
}

// What flows inside a box, in the order of CSS 2.1 Appendix E: the
// backgrounds of all the block-level boxes in it first, then the floats
// over them, then the inline content — the lines and their inline boxes —
// over the floats, clipped to the box when its overflow is hidden. So a
// float overlapping a later block sits over that block's background and
// under its words, as it does in every engine. The positioned descendants
// and the stacking contexts are not here: they belong to the stacking
// context and paint at their level in it.
void paint_contents(Context& context, Fragment const& fragment)
{
    std::size_t const rounds = context.target.round_clip_depth();
    std::optional<Rect> const restore = clip_for(context, fragment);
    paint_block_backgrounds(context, fragment);
    paint_floats(context, fragment);
    paint_inline_content(context, fragment);
    context.target.truncate_round_clips(rounds);
    context.target.set_clip(restore);
}

// A box and what flows inside it.
void paint_flow(Context& context, Fragment const& fragment, bool is_canvas_background_owner)
{
    paint_box(context, fragment, is_canvas_background_owner);
    paint_contents(context, fragment);
}

// --- Scrollbars -------------------------------------------------------------------

// The scrollport: the padding box of a box that scrolls, where its content
// is shown and where its bars are drawn.
Rect scrollport(Fragment const& box)
{
    ComputedStyle const& s = *box.style;
    return snap(box.x + s.border_left.width, box.y + s.border_top.width,
        box.width - s.border_left.width - s.border_right.width,
        box.height - s.border_top.width - s.border_bottom.width);
}

// A bar along one axis: the track fills the scrollport's edge, and the
// thumb is as long a part of it as the scrollport is of the whole content,
// placed as far along as the content has been scrolled. It never falls
// below a size a reader can catch hold of.
ScrollbarGeometry bar_along(Rect track, float visible, float range, float at, bool vertical)
{
    int const length = vertical ? track.height : track.width;
    float const whole = visible + range;
    int const minimum = std::min(length, 20);
    int thumb = whole > 0 ? round_px(static_cast<float>(length) * visible / whole) : length;
    thumb = std::clamp(thumb, minimum, length);
    float const fraction = range > 0 ? std::clamp(at / range, 0.0f, 1.0f) : 0.0f;
    int const travel = length - thumb;
    int const start = round_px(fraction * static_cast<float>(travel));
    Rect thumb_rect = track;
    if (vertical) {
        thumb_rect.y += start;
        thumb_rect.height = thumb;
    } else {
        thumb_rect.x += start;
        thumb_rect.width = thumb;
    }
    return ScrollbarGeometry { track, thumb_rect };
}

// A box shows a bar on an axis when the style asks for one always, or asks
// for one when there is something out of sight and there is. The bars sit
// inside the scrollport over the content rather than taking room from it:
// reserving room would have to happen while the box is being laid out, and
// what the box holds is what decides whether a bar is needed at all.
std::optional<ScrollbarGeometry> vertical_bar(Fragment const& box, layout::ScrollOffset offset)
{
    if (!box.style || !box.style->overflow_applies)
        return std::nullopt;
    if (!css::shows_scrollbar(box.style->overflow_y, box.scroll_range_y > 0))
        return std::nullopt;
    Rect const port = scrollport(box);
    int const width = round_px(scrollbar_width);
    if (port.width <= width || port.height <= 0)
        return std::nullopt;
    bool const across = css::shows_scrollbar(box.style->overflow_x, box.scroll_range_x > 0);
    Rect track { port.x + port.width - width, port.y, width,
        port.height - (across ? width : 0) };
    if (track.height <= 0)
        return std::nullopt;
    return bar_along(track, static_cast<float>(track.height), box.scroll_range_y, offset.y, true);
}

std::optional<ScrollbarGeometry> horizontal_bar(Fragment const& box, layout::ScrollOffset offset)
{
    if (!box.style || !box.style->overflow_applies)
        return std::nullopt;
    if (!css::shows_scrollbar(box.style->overflow_x, box.scroll_range_x > 0))
        return std::nullopt;
    Rect const port = scrollport(box);
    int const width = round_px(scrollbar_width);
    if (port.height <= width || port.width <= 0)
        return std::nullopt;
    bool const down = css::shows_scrollbar(box.style->overflow_y, box.scroll_range_y > 0);
    Rect track { port.x, port.y + port.height - width, port.width - (down ? width : 0), width };
    if (track.width <= 0)
        return std::nullopt;
    return bar_along(track, static_cast<float>(track.width), box.scroll_range_x, offset.x, false);
}

// Drawn in the greys a bar keeps when the page asks for no colour of its
// own: a track that barely marks the edge and a thumb dark enough to find
// on any background, both translucent so the content stays legible under
// them.
void paint_bar(Context& context, ScrollbarGeometry const& bar)
{
    Rect track = bar.track;
    Rect thumb = bar.thumb;
    int const dx = round_px(context.dx);
    int const dy = round_px(context.dy);
    track.x += dx;
    track.y += dy;
    thumb.x += dx;
    thumb.y += dy;
    context.target.fill_rect(track, Color::rgba(0, 0, 0, 20));
    int const radius = std::min(4, std::min(thumb.width, thumb.height) / 2);
    context.target.fill_round_rect(thumb, radius, Color::rgba(0, 0, 0, 120));
}

void paint_scrollbars(Context& context, Fragment const& box)
{
    // No offsets means no shell behind this render: nothing has been
    // scrolled and nothing can be, so there is no bar to draw. A single
    // picture of a page shows the page, not the chrome around it.
    if (!context.scrolls)
        return;
    layout::ScrollOffset const offset = layout::scroll_of(box, context.scrolls);
    if (std::optional<ScrollbarGeometry> const bar = vertical_bar(box, offset))
        paint_bar(context, *bar);
    if (std::optional<ScrollbarGeometry> const bar = horizontal_bar(box, offset))
        paint_bar(context, *bar);
}

// A positioned box a stacking context paints, with the clip the ancestors
// that contain it impose: every overflow-clipping ancestor for a box in
// flow (relative, sticky); only the positioned ones — its containing
// blocks — for an absolutely or fixed positioned box.
struct Layer {
    Fragment const* box = nullptr;
    std::optional<Rect> clip;
    // The curves of the clipping ancestors that round their corners,
    // outermost first: a layer is faded along them as the flow is.
    std::vector<RoundedRect> rounds;
};

// The rounded clips a box adds for what it holds: the ancestors' and, when
// the box clips its overflow along curved corners, its own padding box.
std::vector<RoundedRect> rounds_within(Context const& context, Fragment const& fragment,
    std::vector<RoundedRect> const& current)
{
    if (!clips_corners(fragment) || !fragment.style->rounded())
        return current;
    RoundedRect const curve = rounded_area(context, fragment, css::BackgroundBox::PaddingBox);
    if (curve.is_rectangular())
        return current;
    std::vector<RoundedRect> rounds = current;
    rounds.push_back(curve);
    return rounds;
}

// --- Tree order among a context's layers ---------------------------------------------
//
// CSS 2.1 Appendix E paints the positioned descendants of a stacking
// context in tree order within a z-index level. The fragments mostly are in
// that order, but a box out of the flow is kept under its containing block
// after everything that block holds in the flow, wherever in the tree its
// element stands: a backdrop written first and laid over its parent would
// be painted over the positioned content written after it.

// An element's ancestors in the flat tree, the root first, itself last.
std::vector<dom::Node const*> tree_path(dom::Element const& element)
{
    std::vector<dom::Node const*> path;
    for (dom::Node const* node = &element; node; node = dom::flat_parent(*node))
        path.push_back(node);
    std::reverse(path.begin(), path.end());
    return path;
}

// Whether the element `a` leads to comes after the one `b` leads to in tree
// order: inside it, or in a later branch of the ancestor they share.
bool follows_in_tree(std::vector<dom::Node const*> const& a, std::vector<dom::Node const*> const& b)
{
    std::size_t shared = 0;
    while (shared < a.size() && shared < b.size() && a[shared] == b[shared])
        ++shared;
    if (shared == a.size() || shared == 0)
        return false; // the same element, an ancestor of the other, or another tree
    if (shared == b.size())
        return true;
    for (dom::Node const* child : dom::flat_children(*a[shared - 1])) {
        if (child == b[shared])
            return true;
        if (child == a[shared])
            return false;
    }
    return false;
}

// Puts each layer that is out of the flow back where its element stands in
// the tree: before the first layer in the flow whose element follows its
// own. The layers in the flow are in the order layout gave them, which is
// tree order as `order` changes it among flex and grid items, and keep it;
// so do the layers out of the flow among themselves, placed in that order
// too. Both the painter and the hit test read the layers through this, so
// what is on top is what answers the pointer.
template <typename Layers>
void put_in_tree_order(Layers& layers)
{
    using Entry = typename Layers::value_type;
    auto const hoisted = [](Entry const& layer) { return layer.box->out_of_flow && layer.box->element != nullptr; };
    if (layers.size() < 2 || std::none_of(layers.begin(), layers.end(), hoisted))
        return;
    struct Placed {
        Entry layer;
        std::vector<dom::Node const*> path;
        bool hoisted = false;
    };
    std::vector<Placed> kept;
    std::vector<Placed> moved;
    for (Entry& layer : layers) {
        bool const out = hoisted(layer);
        std::vector<dom::Node const*> path = layer.box->element ? tree_path(*layer.box->element)
                                                               : std::vector<dom::Node const*> {};
        (out ? moved : kept).push_back(Placed { std::move(layer), std::move(path), out });
    }
    for (Placed& placed : moved) {
        auto at = kept.begin();
        while (at != kept.end() && (at->hoisted || at->path.empty() || !follows_in_tree(at->path, placed.path)))
            ++at;
        kept.insert(at, std::move(placed));
    }
    layers.clear();
    for (Placed& placed : kept)
        layers.push_back(std::move(placed.layer));
}

// The layers a stacking context paints: every positioned descendant, and
// every descendant that is a stacking context for another reason, reached
// without crossing another stacking context (a positioned box with
// z-index auto is walked through — its positioned descendants are the
// parent context's, per CSS 2.1 Appendix E).
struct Clips {
    std::optional<Rect> rect;
    std::vector<RoundedRect> rounds;
};

void collect_positioned(Context const& context, Fragment const& fragment, Clips const& in_flow,
    Clips const& out_of_flow, std::vector<Layer>& out, std::vector<Layer>& bars)
{
    for (Fragment const& child : fragment.children) {
        // A block that ends its lines in an ellipsis is read here, before
        // anything of the context is painted: what it hides may paint at
        // any of the context's steps. A stacking context reads its own.
        if (child.elision && !child.stacking_context)
            plan_elision(context, child);
        if (paints_as_layer(child)) {
            if (child.fixed) {
                out.push_back(Layer { &child, context.page_clip, {} });
            } else {
                Clips const& clips = child.out_of_flow ? out_of_flow : in_flow;
                out.push_back(Layer { &child, clips.rect, clips.rounds });
            }
        }
        if (child.stacking_context)
            continue;
        Clips const inner { clip_within(context, child, in_flow.rect),
            rounds_within(context, child, in_flow.rounds) };
        // A scroll container's bars are drawn over everything it holds,
        // and a positioned descendant of it paints at this context's level
        // rather than inside it, so the bars are gathered here and drawn
        // after the whole context.
        if (context.scrolls && child.style && child.style->overflow_applies
            && css::scrolls(child.style->overflow_x))
            bars.push_back(Layer { &child, in_flow.rect, in_flow.rounds });
        Clips const inner_out_of_flow = child.positioned
            ? Clips { clip_within(context, child, out_of_flow.rect),
                  rounds_within(context, child, out_of_flow.rounds) }
            : out_of_flow;
        collect_positioned(context, child, inner, inner_out_of_flow, out, bars);
    }
}

// CSS 2.1 Appendix E, the reader-web subset: the context's own flow first,
// then its positioned descendants by z-index — negative ones under the
// flow — in tree order within a level; a descendant that is itself a
// stacking context paints as one unit, one with z-index auto as its flow
// alone (its positioned descendants are already in this list).
void paint_opaque_context(Context& context, Fragment const& root, bool is_canvas_background_owner)
{
    // The root's overflow clips everything inside it that it contains: its
    // flow, and the positioned descendants whose containing block it is.
    std::optional<Rect> const outer = context.target.clip();
    std::size_t const outer_rounds = context.target.round_clip_depth();
    Clips const outside { outer, {} };
    Clips const inside { clip_within(context, root, outer), rounds_within(context, root, {}) };
    std::vector<Layer> positioned;
    std::vector<Layer> bars;
    if (context.scrolls && root.style && root.style->overflow_applies
        && css::scrolls(root.style->overflow_x))
        bars.push_back(Layer { &root, inside.rect, inside.rounds });
    plan_elision(context, root);
    collect_positioned(context, root, inside, root.positioned ? inside : outside, positioned, bars);
    put_in_tree_order(positioned);
    std::stable_sort(positioned.begin(), positioned.end(),
        [](Layer const& a, Layer const& b) { return a.box->z_index < b.box->z_index; });
    auto const paint_one = [&](Layer const& layer) {
        if (elided(context, *layer.box))
            return;
        context.target.set_clip(layer.clip);
        for (RoundedRect const& round : layer.rounds)
            context.target.push_round_clip(round);
        if (layer.box->stacking_context)
            paint_stacking_context(context, *layer.box, false);
        else
            paint_flow(context, *layer.box, false);
        context.target.truncate_round_clips(outer_rounds);
        context.target.set_clip(outer);
    };
    // The root's own box comes before its negative descendants, which come
    // before the rest of its flow; the positioned ones over it all.
    paint_box(context, root, is_canvas_background_owner);
    for (Layer const& layer : positioned) {
        if (layer.box->z_index < 0)
            paint_one(layer);
    }
    paint_contents(context, root);
    for (Layer const& layer : positioned) {
        if (layer.box->z_index >= 0)
            paint_one(layer);
    }
    // The outlines, last of all (CSS 2.1 Appendix E, step 10).
    context.target.set_clip(inside.rect);
    paint_outline(context, root);
    paint_outlines(context, root);
    context.target.set_clip(outer);
    // A scroll container's bars are drawn over everything it holds, and a
    // positioned descendant of it paints at this level, so they come after
    // the whole context (css-overflow-3 §3.2).
    for (Layer const& bar : bars) {
        context.target.set_clip(bar.clip);
        paint_scrollbars(context, *bar.box);
    }
    context.target.set_clip(outer);
}

// Where a subtree could put ink, in page coordinates: the union of every
// box in it with every run's line, generously. It is only used to size the
// buffer a group at less than full opacity is painted into, so being too
// large costs memory and being too small would lose ink — the run's box is
// therefore taken as a whole em above and below its baseline, which covers
// its decorations and the way a synthesized italic leans.
//
// A box drawn through a transform puts its ink where the transform takes
// it; `through_own` false asks for the box's ink in its own plane, as it
// was laid out, which is what the picture drawn through the transform is
// sized by.
void ink_bounds(Fragment const& box, float& left, float& top, float& right, float& bottom, bool through_own = true)
{
    if (box.transform && through_own) {
        float own_left = std::numeric_limits<float>::max();
        float own_top = own_left;
        float own_right = std::numeric_limits<float>::lowest();
        float own_bottom = own_right;
        ink_bounds(box, own_left, own_top, own_right, own_bottom, false);
        double to_left = 0;
        double to_top = 0;
        double to_right = 0;
        double to_bottom = 0;
        if (box.transform->about(static_cast<double>(box.x), static_cast<double>(box.y))
                .map_rect(static_cast<double>(own_left), static_cast<double>(own_top), static_cast<double>(own_right - own_left),
                    static_cast<double>(own_bottom - own_top), to_left, to_top, to_right, to_bottom)) {
            left = std::min(left, static_cast<float>(to_left) - 1);
            top = std::min(top, static_cast<float>(to_top) - 1);
            right = std::max(right, static_cast<float>(to_right) + 1);
            bottom = std::max(bottom, static_cast<float>(to_bottom) + 1);
        }
        return;
    }
    // What the box draws beyond its border box: its outline, and the
    // shadows it casts outwards.
    float out_left = 0;
    float out_top = 0;
    float out_right = 0;
    float out_bottom = 0;
    if (box.style) {
        ComputedStyle const& style = *box.style;
        if (style.outline.style != css::BorderStyle::None) {
            float const reach = std::max(0.0f, style.outline.width + std::max(0.0f, style.outline.offset)) + 1;
            out_left = out_top = out_right = out_bottom = reach;
        }
        if (style.box_shadow) {
            for (css::Shadow const& shadow : *style.box_shadow) {
                if (shadow.inset)
                    continue;
                float const spread = std::max(0.0f, shadow.spread) + std::max(0.0f, shadow.blur) * 1.5f + 1;
                out_left = std::max(out_left, spread - shadow.x);
                out_right = std::max(out_right, spread + shadow.x);
                out_top = std::max(out_top, spread - shadow.y);
                out_bottom = std::max(out_bottom, spread + shadow.y);
            }
        }
    }
    left = std::min(left, box.x - out_left);
    top = std::min(top, box.y - out_top);
    right = std::max(right, box.x + box.width + out_right);
    bottom = std::max(bottom, box.y + box.height + out_bottom);
    for (TextRun const& run : box.runs) {
        float const em = run.style ? run.style->font_size : 0.0f;
        float x0 = css::is_vertical(run.mode) ? run.baseline_y - em : run.x;
        float y0 = css::is_vertical(run.mode) ? run.x : run.baseline_y - em;
        float x1 = css::is_vertical(run.mode) ? run.baseline_y + em : run.x + run.width;
        float y1 = css::is_vertical(run.mode) ? run.x + run.width : run.baseline_y + em;
        if (run.style && run.style->text_shadow) {
            float cast_left = 0;
            float cast_top = 0;
            float cast_right = 0;
            float cast_bottom = 0;
            for (css::Shadow const& shadow : *run.style->text_shadow) {
                float const blur = std::max(0.0f, shadow.blur) * 1.5f + 1;
                cast_left = std::max(cast_left, blur - shadow.x);
                cast_right = std::max(cast_right, blur + shadow.x);
                cast_top = std::max(cast_top, blur - shadow.y);
                cast_bottom = std::max(cast_bottom, blur + shadow.y);
            }
            x0 -= cast_left;
            y0 -= cast_top;
            x1 += cast_right;
            y1 += cast_bottom;
        }
        left = std::min(left, x0);
        top = std::min(top, y0);
        right = std::max(right, x1);
        bottom = std::max(bottom, y1);
    }
    for (Fragment const& child : box.children)
        ink_bounds(child, left, top, right, bottom);
}

// css-color-4 §12: a box whose opacity is below one is composited as a
// group — everything inside it paints at full strength against each other
// first, and the whole is then laid over the page at that alpha. Painting
// it straight onto the page would fade each piece separately, so where two
// of them overlap the one underneath would show through the one above.
//
// The group is painted onto a copy of the backdrop it stands on and moved
// back over it by its alpha, which for content that composites only
// source-over is exactly the group's colour there — the arithmetic is in
// `Bitmap::blend_over`. It costs one buffer the size of the group's ink,
// held to whatever clip is already in force.
// The box a clip path is sized against (css-masking-1 §5.1): the border
// box unless it names another, moved by (dx, dy).
ShapeBox clip_box_of(Fragment const& fragment, float dx, float dy)
{
    ComputedStyle const& style = *fragment.style;
    ShapeBox box { fragment.x + dx, fragment.y + dy, fragment.width, fragment.height };
    auto const inset = [&](float left, float top, float right, float bottom) {
        box.x += left;
        box.y += top;
        box.width = std::max(0.0f, box.width - left - right);
        box.height = std::max(0.0f, box.height - top - bottom);
    };
    float const base = fragment.width;
    using Box = css::ClipPath::Box;
    Box const named = style.clip_path ? style.clip_path->box : Box::Border;
    if (named == Box::Padding || named == Box::Content)
        inset(style.border_left.width, style.border_top.width, style.border_right.width, style.border_bottom.width);
    if (named == Box::Content)
        inset(resolve_length(style.padding_left, base), resolve_length(style.padding_top, base),
            resolve_length(style.padding_right, base), resolve_length(style.padding_bottom, base));
    if (named == Box::Margin) {
        float const left = resolve_length(style.margin_left, base);
        float const top = resolve_length(style.margin_top, base);
        box.x -= left;
        box.y -= top;
        box.width += left + resolve_length(style.margin_right, base);
        box.height += top + resolve_length(style.margin_bottom, base);
    }
    return box;
}

Rect grown(Rect const& rect, Reach const& by)
{
    return Rect { rect.x - by.left, rect.y - by.top, rect.width + by.left + by.right, rect.height + by.top + by.bottom };
}

// backdrop-filter (filter-effects-2 §2): what is already painted behind
// the box, within its border box and its corners, passed through the list
// before anything of the box itself is painted over it. A blur reads from
// beyond the box, as far as the picture reaches.
void filter_backdrop(Context& context, Fragment const& root)
{
    css::FilterList const& filters = *root.style->backdrop_filter;
    RoundedRect const shape = rounded_box(context, root);
    Rect const whole { 0, 0, context.target.width(), context.target.height() };
    Rect area = intersect(shape.bounds(), whole);
    if (std::optional<Rect> const held = context.target.clip())
        area = intersect(area, *held);
    if (area.is_empty())
        return;
    Rect const read = intersect(grown(area, reach_of(filters)), whole);
    Bitmap behind(read.width, read.height);
    behind.blit(context.target, -read.x, -read.y);
    EffectLayer layer = layer_of(behind);
    apply_filters(layer, filters, root.style->color);
    Bitmap const filtered = bitmap_of(layer);
    for (int y = area.y; y < area.bottom(); ++y) {
        for (int x = area.x; x < area.right(); ++x) {
            std::uint8_t const covered = shape.coverage(x, y);
            if (covered == 0)
                continue;
            Color color = filtered.pixel(x - read.x, y - read.y);
            color.a = static_cast<std::uint8_t>(static_cast<int>(color.a) * covered / 255);
            context.target.blend_pixel(x, y, color);
        }
    }
}

// A box with a filter or a clip path (filter-effects-1 §4, css-masking-1
// §5): everything in it is painted as one picture with transparency of its
// own, which then goes through the filters, is cut to the shape, takes the
// box's opacity and is laid over the page. The picture is read back from
// two paintings of the group, over black and over white (`layer_from`).
void paint_through_effects(Context& context, Fragment const& root, bool is_canvas_background_owner, float alpha)
{
    ComputedStyle const& style = *root.style;
    if (style.backdrop_filter)
        filter_backdrop(context, root);
    if (!style.filter && !style.clip_path) {
        // Only the backdrop was filtered: the box itself paints as ever.
        if (alpha >= 1) {
            paint_opaque_context(context, root, is_canvas_background_owner);
            return;
        }
    }
    float left = std::numeric_limits<float>::max();
    float top = left;
    float right = std::numeric_limits<float>::lowest();
    float bottom = right;
    ink_bounds(root, left, top, right, bottom, false);
    Reach const reach = style.filter ? reach_of(*style.filter) : Reach {};
    Rect const ink = snap(left + context.dx, top + context.dy, right - left, bottom - top);
    Rect const whole { 0, 0, context.target.width(), context.target.height() };
    // What of the result can show, and what of the group has to be painted
    // to make it: a filter that reaches out draws from as far beyond.
    Rect shown = intersect(grown(ink, reach), whole);
    if (std::optional<Rect> const held = context.target.clip())
        shown = intersect(shown, *held);
    if (shown.is_empty())
        return;
    Rect const area = intersect(grown(shown, reach), grown(ink, reach));
    if (area.is_empty() || static_cast<long long>(area.width) * area.height > 64LL * 1024 * 1024)
        return;
    auto const paint_over = [&](Color backdrop, bool first) {
        Bitmap group(area.width, area.height, backdrop);
        Context inside { group, context.dx - static_cast<float>(area.x), context.dy - static_cast<float>(area.y),
            context.backgrounds, context.canvas_owner, context.scrolls };
        inside.device_scale = context.device_scale;
        // The pictures are noted once, by the first of the two paintings.
        inside.pictures = first ? context.pictures : nullptr;
        inside.shown_apart = context.shown_apart;
        inside.origin_x = context.origin_x + area.x;
        inside.origin_y = context.origin_y + area.y;
        inside.elisions = context.elisions;
        paint_opaque_context(inside, root, is_canvas_background_owner);
        return group;
    };
    EffectLayer layer;
    {
        Bitmap const over_black = paint_over(Color::rgb(0, 0, 0), true);
        Bitmap const over_white = paint_over(Color::rgb(255, 255, 255), false);
        layer = layer_from(over_black, over_white);
    }
    if (style.filter)
        apply_filters(layer, *style.filter, style.color);
    if (style.clip_path) {
        bool even_odd = false;
        std::vector<svg::Polygon> const outline = clip_outline(*style.clip_path, clip_box_of(root, context.dx, context.dy), even_odd);
        cut_to(layer, outline, even_odd, area.x, area.y);
    }
    if (alpha < 1) {
        for (std::vector<float>* const plane : { &layer.red, &layer.green, &layer.blue, &layer.alpha }) {
            for (float& value : *plane)
                value *= alpha;
        }
    }
    context.target.draw(bitmap_of(layer), area.x, area.y);
}

// A box with a transform that does more than move it (css-transforms-1
// §3): it and everything in it are painted flat, as one picture with
// transparency of its own, in the plane the box was laid out in, and the
// picture goes onto the page through the transform. Each pixel of the page
// it reaches takes the picture's color at the place the transform's
// inverse says the pixel came from, weighed between the four pixels of the
// picture around that place — which is what softens a turned box's edges
// too. A picture drawn at less than half its size along an axis is halved
// along it first, as often as that takes, so that every pixel of it has
// its say.
void paint_transformed(Context& context, Fragment const& root, bool is_canvas_background_owner)
{
    float left = std::numeric_limits<float>::max();
    float top = left;
    float right = std::numeric_limits<float>::lowest();
    float bottom = right;
    ink_bounds(root, left, top, right, bottom, false);
    if (root.style && root.style->filter) {
        Reach const reach = reach_of(*root.style->filter);
        left -= static_cast<float>(reach.left);
        top -= static_cast<float>(reach.top);
        right += static_cast<float>(reach.right);
        bottom += static_cast<float>(reach.bottom);
    }
    if (!(right > left) || !(bottom > top))
        return;
    css::PlaneTransform const to_target = css::PlaneTransform::translation(static_cast<double>(context.dx), static_cast<double>(context.dy))
        * root.transform->about(static_cast<double>(root.x), static_cast<double>(root.y));
    std::optional<css::PlaneTransform> const from_target = to_target.inverse();
    if (!from_target)
        return; // flattened to a line or a point: nothing of it shows
    Rect reach { 0, 0, context.target.width(), context.target.height() };
    if (std::optional<Rect> const held = context.target.clip())
        reach = intersect(reach, *held);
    auto const down = [](double value) { return static_cast<int>(std::floor(std::clamp(value, -1.0e9, 1.0e9))); };
    auto const up = [](double value) { return static_cast<int>(std::ceil(std::clamp(value, -1.0e9, 1.0e9))); };
    double to_left = 0;
    double to_top = 0;
    double to_right = 0;
    double to_bottom = 0;
    if (!to_target.map_rect(static_cast<double>(left), static_cast<double>(top), static_cast<double>(right - left),
            static_cast<double>(bottom - top), to_left, to_top, to_right, to_bottom))
        return;
    Rect shown { down(to_left) - 1, down(to_top) - 1, 0, 0 };
    shown.width = up(to_right) + 1 - shown.x;
    shown.height = up(to_bottom) + 1 - shown.y;
    shown = intersect(shown, reach);
    if (shown.is_empty())
        return;
    // What of the plane lands there: no more of the group is painted.
    double from_left = static_cast<double>(left);
    double from_top = static_cast<double>(top);
    double from_right = static_cast<double>(right);
    double from_bottom = static_cast<double>(bottom);
    {
        double l = 0;
        double t = 0;
        double r = 0;
        double b = 0;
        if (from_target->map_rect(shown.x, shown.y, shown.width, shown.height, l, t, r, b)) {
            from_left = std::max(from_left, l - 2);
            from_top = std::max(from_top, t - 2);
            from_right = std::min(from_right, r + 2);
            from_bottom = std::min(from_bottom, b + 2);
        }
    }
    if (!(from_right > from_left) || !(from_bottom > from_top))
        return;
    Rect const area { down(from_left), down(from_top), up(from_right) - down(from_left), up(from_bottom) - down(from_top) };
    if (area.is_empty() || static_cast<long long>(area.width) * area.height > 64LL * 1024 * 1024)
        return;
    auto const paint_over = [&](Color backdrop) {
        Bitmap group(area.width, area.height, backdrop);
        Context inside { group, -static_cast<float>(area.x), -static_cast<float>(area.y), context.backgrounds, context.canvas_owner,
            context.scrolls };
        inside.device_scale = context.device_scale;
        inside.elisions = context.elisions;
        paint_stacking_context(inside, root, is_canvas_background_owner, true);
        return group;
    };
    EffectLayer layer;
    {
        Bitmap const over_black = paint_over(Color::rgb(0, 0, 0));
        Bitmap const over_white = paint_over(Color::rgb(255, 255, 255));
        layer = layer_from(over_black, over_white);
    }
    auto const sample = [&](std::vector<float> const& plane, int x, int y) -> float {
        if (x < 0 || y < 0 || x >= layer.width || y >= layer.height)
            return 0.0f;
        return plane[static_cast<std::size_t>(y) * static_cast<std::size_t>(layer.width) + static_cast<std::size_t>(x)];
    };
    auto const put = [&](int px, int py, float r, float g, float b, float alpha) {
        if (alpha <= 0.0f)
            return;
        auto const channel = [alpha](float value) {
            return static_cast<std::uint8_t>(std::clamp(value / alpha * 255.0f + 0.5f, 0.0f, 255.0f));
        };
        Color color = Color::rgb(channel(r), channel(g), channel(b));
        color.a = static_cast<std::uint8_t>(std::clamp(alpha * 255.0f + 0.5f, 0.0f, 255.0f));
        context.target.blend_pixel(px, py, color);
    };
    // Stretched or squeezed along the page's own axes, and no more: each
    // pixel of the page covers a rectangle of the picture, and takes the
    // average of what lies under it — every pixel of a picture drawn
    // smaller has its say, and one drawn larger keeps its edges where
    // they were. Inside a picture drawn larger the colors run smoothly
    // from one of its pixels to the next, as a scaled photograph's must,
    // while a hard edge of the picture itself — all there on one side,
    // nothing on the other — stays as sharp as the box that made it.
    if (to_target.is_affine() && to_target.h[1] == 0 && to_target.h[3] == 0) {
        double const scale_x = to_target.h[0];
        double const scale_y = to_target.h[4];
        auto const span = [](double scale, double shift, int pixel, int origin, double& from, double& to) {
            double const a = (pixel - shift) / scale - origin;
            double const b = (pixel + 1 - shift) / scale - origin;
            from = std::min(a, b);
            to = std::max(a, b);
        };
        // The average of a plane under [u0, u1) x [v0, v1), in the layer's
        // own pixels.
        auto const under = [&](std::vector<float> const& plane, double u0, double u1, double v0, double v1) -> float {
            int const x0 = std::max(0, static_cast<int>(std::floor(u0)));
            int const x1 = std::min(layer.width, static_cast<int>(std::ceil(u1)));
            int const y0 = std::max(0, static_cast<int>(std::floor(v0)));
            int const y1 = std::min(layer.height, static_cast<int>(std::ceil(v1)));
            double sum = 0;
            for (int y = y0; y < y1; ++y) {
                double const tall = std::min(v1, y + 1.0) - std::max(v0, static_cast<double>(y));
                if (tall <= 0)
                    continue;
                for (int x = x0; x < x1; ++x) {
                    double const wide = std::min(u1, x + 1.0) - std::max(u0, static_cast<double>(x));
                    if (wide > 0)
                        sum += static_cast<double>(plane[static_cast<std::size_t>(y) * static_cast<std::size_t>(layer.width) + static_cast<std::size_t>(x)]) * wide * tall;
                }
            }
            return static_cast<float>(sum / ((u1 - u0) * (v1 - v0)));
        };
        bool const grows = std::fabs(scale_x) > 1 || std::fabs(scale_y) > 1;
        for (int py = shown.y; py < shown.bottom(); ++py) {
            double v0 = 0;
            double v1 = 0;
            span(scale_y, to_target.h[5], py, area.y, v0, v1);
            if (v1 <= 0 || v0 >= layer.height)
                continue;
            for (int px = shown.x; px < shown.right(); ++px) {
                double u0 = 0;
                double u1 = 0;
                span(scale_x, to_target.h[2], px, area.x, u0, u1);
                if (u1 <= 0 || u0 >= layer.width)
                    continue;
                float const covered = under(layer.alpha, u0, u1, v0, v1);
                if (!grows) {
                    put(px, py, under(layer.red, u0, u1, v0, v1), under(layer.green, u0, u1, v0, v1),
                        under(layer.blue, u0, u1, v0, v1), covered);
                    continue;
                }
                // Between the four pixels around the middle of what this
                // pixel covers.
                double const u = (u0 + u1) / 2 - 0.5;
                double const v = (v0 + v1) / 2 - 0.5;
                int const x0 = static_cast<int>(std::floor(u));
                int const y0 = static_cast<int>(std::floor(v));
                auto const fx = static_cast<float>(u - x0);
                auto const fy = static_cast<float>(v - y0);
                float const taps[4] = { sample(layer.alpha, x0, y0), sample(layer.alpha, x0 + 1, y0), sample(layer.alpha, x0, y0 + 1),
                    sample(layer.alpha, x0 + 1, y0 + 1) };
                float const weights[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
                auto const mixed = [&](std::vector<float> const& plane) {
                    return sample(plane, x0, y0) * weights[0] + sample(plane, x0 + 1, y0) * weights[1]
                        + sample(plane, x0, y0 + 1) * weights[2] + sample(plane, x0 + 1, y0 + 1) * weights[3];
                };
                float const smooth = taps[0] * weights[0] + taps[1] * weights[1] + taps[2] * weights[2] + taps[3] * weights[3];
                if (smooth <= 0.0f)
                    continue;
                bool const hard = std::all_of(std::begin(taps), std::end(taps), [](float tap) { return tap <= 0.001f || tap >= 0.999f; });
                float const alpha = hard ? covered : smooth;
                float const keep = alpha / smooth;
                put(px, py, mixed(layer.red) * keep, mixed(layer.green) * keep, mixed(layer.blue) * keep, alpha);
            }
        }
        return;
    }
    // Halved along an axis it is drawn at under half its size on.
    double shrink_x = 1;
    double shrink_y = 1;
    {
        double const along_x = std::hypot(to_target.h[0], to_target.h[3]) / std::max(1e-9, std::fabs(to_target.h[8]));
        double const along_y = std::hypot(to_target.h[1], to_target.h[4]) / std::max(1e-9, std::fabs(to_target.h[8]));
        auto const halve = [&](bool horizontal) {
            int const width = horizontal ? (layer.width + 1) / 2 : layer.width;
            int const height = horizontal ? layer.height : (layer.height + 1) / 2;
            EffectLayer half;
            half.width = width;
            half.height = height;
            for (std::vector<float> EffectLayer::* const plane : { &EffectLayer::red, &EffectLayer::green, &EffectLayer::blue, &EffectLayer::alpha }) {
                std::vector<float> const& from = layer.*plane;
                std::vector<float>& to = half.*plane;
                to.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0.0f);
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        int const x0 = horizontal ? x * 2 : x;
                        int const y0 = horizontal ? y : y * 2;
                        int const x1 = horizontal ? std::min(x0 + 1, layer.width - 1) : x0;
                        int const y1 = horizontal ? y0 : std::min(y0 + 1, layer.height - 1);
                        // A last odd column or row is averaged with nothing.
                        bool const pair = horizontal ? x0 + 1 < layer.width : y0 + 1 < layer.height;
                        float const a = from[static_cast<std::size_t>(y0) * static_cast<std::size_t>(layer.width) + static_cast<std::size_t>(x0)];
                        float const b = pair ? from[static_cast<std::size_t>(y1) * static_cast<std::size_t>(layer.width) + static_cast<std::size_t>(x1)] : 0.0f;
                        to[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = (a + b) / 2;
                    }
                }
            }
            layer = std::move(half);
        };
        while (along_x * shrink_x < 0.5 && layer.width > 1 && shrink_x < 4096) {
            halve(true);
            shrink_x *= 2;
        }
        while (along_y * shrink_y < 0.5 && layer.height > 1 && shrink_y < 4096) {
            halve(false);
            shrink_y *= 2;
        }
    }
    for (int py = shown.y; py < shown.bottom(); ++py) {
        for (int px = shown.x; px < shown.right(); ++px) {
            double x = 0;
            double y = 0;
            if (!from_target->map(px + 0.5, py + 0.5, x, y))
                continue;
            double const u = (x - area.x) / shrink_x - 0.5;
            double const v = (y - area.y) / shrink_y - 0.5;
            if (u <= -1 || v <= -1 || u >= layer.width || v >= layer.height)
                continue;
            int const x0 = static_cast<int>(std::floor(u));
            int const y0 = static_cast<int>(std::floor(v));
            auto const fx = static_cast<float>(u - x0);
            auto const fy = static_cast<float>(v - y0);
            float const w00 = (1 - fx) * (1 - fy);
            float const w10 = fx * (1 - fy);
            float const w01 = (1 - fx) * fy;
            float const w11 = fx * fy;
            auto const mixed = [&](std::vector<float> const& plane) {
                return sample(plane, x0, y0) * w00 + sample(plane, x0 + 1, y0) * w10 + sample(plane, x0, y0 + 1) * w01
                    + sample(plane, x0 + 1, y0 + 1) * w11;
            };
            put(px, py, mixed(layer.red), mixed(layer.green), mixed(layer.blue), mixed(layer.alpha));
        }
    }
}

void paint_stacking_context(Context& context, Fragment const& root, bool is_canvas_background_owner, bool in_own_plane)
{
    float const alpha = root.style ? root.style->opacity : 1.0f;
    if (alpha <= 0)
        return; // nothing of it paints, its positioned descendants included
    if (root.back_hidden)
        return; // turned to show a back it does not show
    if (root.transform && !in_own_plane) {
        paint_transformed(context, root, is_canvas_background_owner);
        return;
    }
    if (root.style && root.style->paints_through_effects()) {
        paint_through_effects(context, root, is_canvas_background_owner, alpha);
        return;
    }
    if (alpha >= 1) {
        paint_opaque_context(context, root, is_canvas_background_owner);
        return;
    }
    float left = std::numeric_limits<float>::max();
    float top = left;
    float right = std::numeric_limits<float>::lowest();
    float bottom = right;
    ink_bounds(root, left, top, right, bottom, false);
    Rect const whole { 0, 0, context.target.width(), context.target.height() };
    Rect area = intersect(
        snap(left + context.dx, top + context.dy, right - left, bottom - top), whole);
    if (std::optional<Rect> const held = context.target.clip())
        area = intersect(area, *held);
    if (area.is_empty())
        return;
    Bitmap group(area.width, area.height);
    group.blit(context.target, -area.x, -area.y);
    Context inside { group, context.dx - static_cast<float>(area.x),
        context.dy - static_cast<float>(area.y), context.backgrounds, context.canvas_owner,
        context.scrolls };
    inside.pictures = context.pictures;
    inside.shown_apart = context.shown_apart;
    inside.origin_x = context.origin_x + area.x;
    inside.origin_y = context.origin_y + area.y;
    inside.elisions = context.elisions;
    if (std::optional<Rect> const held = context.target.clip()) {
        group.set_clip(Rect { held->x - area.x, held->y - area.y, held->width, held->height });
    }
    paint_opaque_context(inside, root, is_canvas_background_owner);
    context.target.blend_over(group, area.x, area.y, alpha);
}

// --- What is under a point --------------------------------------------------------
//
// The painter's order, taken backwards. What was painted last at a point is
// what a reader sees there, and so what they are pointing at. Each walk
// below undoes one of the walks above — the last step first, the last
// sibling first — and asks the same questions of a box to decide which step
// it belongs to, so the two cannot disagree about which of two boxes that
// overlap is the one on top: an empty positioned link laid over a whole
// card answers for the card, a float answers over the block behind it, and
// a line's words answer over both.

struct Probe {
    float x = 0;
    float y = 0;
};

bool holds(float left, float top, float width, float height, Probe at)
{
    return at.x >= left && at.x < left + width && at.y >= top && at.y < top + height;
}

// Whether the pointer is heard by what a style dresses: not while it is
// hidden, which paints nothing, not with `pointer-events: none`, and not
// when it is inert (what is behind it answers, as Chrome does). A box
// at no opacity at all is still heard, as it is in every engine — a page
// lays a transparent control over the picture of one it drew itself.
bool hears_pointer(ComputedStyle const* style)
{
    return style && !style->hidden() && style->pointer_events && !style->inert;
}

// What a box that clips holds is out of reach outside its padding box, on
// the axes it clips: `clip_within`, asked of one point.
bool clip_lets_through(Fragment const& box, Probe at)
{
    if (!box.style || !box.style->overflow_applies)
        return true;
    ComputedStyle const& s = *box.style;
    if (s.overflow_x != css::Overflow::Visible
        && (at.x < box.x + s.border_left.width || at.x >= box.x + box.width - s.border_right.width))
        return false;
    if (s.overflow_y != css::Overflow::Visible
        && (at.y < box.y + s.border_top.width || at.y >= box.y + box.height - s.border_bottom.width))
        return false;
    return true;
}

// The words of the lines a box holds, the last painted first.
std::optional<PointHit> hit_words(Fragment const& box, Probe at)
{
    for (auto run = box.runs.rbegin(); run != box.runs.rend(); ++run) {
        if (!run->element || !hears_pointer(run->style))
            continue;
        float const size = run->style->font_size;
        text::FaceMetrics const face = run->fonts
            ? run->fonts->primary().metrics(size)
            : text::FaceMetrics { size * 25.0f / 32.0f, size * 7.0f / 32.0f, 0 };
        // A run written down the page carries the page's y in `x` and the
        // page's x in `baseline_y`; its glyphs reach to either side of that
        // line by as much as the face's taller half.
        bool const held = css::is_vertical(run->mode)
            ? holds(run->baseline_y - std::max(face.ascent, face.descent), run->x,
                  2 * std::max(face.ascent, face.descent), run->width, at)
            : holds(run->x, run->baseline_y - face.ascent, run->width, face.ascent + face.descent, at);
        if (held)
            return PointHit { run->element, &box, PointHit::Part::Words };
    }
    return std::nullopt;
}

// What `paint_box_replaced` draws: a control's face, a picture.
std::optional<PointHit> hit_replaced(Fragment const& box, Probe at)
{
    if (!box.element || !hears_pointer(box.style))
        return std::nullopt;
    if (box.control
        && holds(box.control->x, box.control->y, box.control->width, box.control->height, at))
        return PointHit { box.element, &box, PointHit::Part::Control };
    if (box.image && holds(box.x, box.y, box.width, box.height, at))
        return PointHit { box.element, &box, PointHit::Part::Picture };
    return std::nullopt;
}

// The box's own painting, whole: what it shows in place of content, then
// its area — the background and the borders, drawn or not.
std::optional<PointHit> hit_box(Fragment const& box, Probe at)
{
    if (std::optional<PointHit> const hit = hit_replaced(box, at))
        return hit;
    if (!box.element || !hears_pointer(box.style) || !holds(box.x, box.y, box.width, box.height, at))
        return std::nullopt;
    return PointHit { box.element, &box, PointHit::Part::Box };
}

std::optional<PointHit> hit_flow(Fragment const& box, Probe at);

// `paint_inline_content` backwards: a box's own words were painted after
// its boxes, and its boxes in tree order.
std::optional<PointHit> hit_inline_content(Fragment const& box, Probe at)
{
    if (std::optional<PointHit> const hit = hit_words(box, at))
        return hit;
    for (auto child = box.children.rbegin(); child != box.children.rend(); ++child) {
        if (child->floating || paints_as_layer(*child))
            continue;
        std::optional<PointHit> hit;
        if (paints_whole(box, *child) || !block_level(*child)) {
            hit = hit_flow(*child, at);
        } else {
            if (clip_lets_through(*child, at))
                hit = hit_inline_content(*child, at);
            if (!hit)
                hit = hit_replaced(*child, at);
        }
        if (hit)
            return hit;
    }
    return std::nullopt;
}

// `paint_floats` backwards: each float one unit, the last over the rest.
std::optional<PointHit> hit_floats(Fragment const& box, Probe at)
{
    for (auto child = box.children.rbegin(); child != box.children.rend(); ++child) {
        if (paints_as_layer(*child))
            continue;
        std::optional<PointHit> hit;
        if (child->floating)
            hit = hit_flow(*child, at);
        else if (!paints_whole(box, *child) && clip_lets_through(*child, at))
            hit = hit_floats(*child, at);
        if (hit)
            return hit;
    }
    return std::nullopt;
}

// `paint_block_backgrounds` backwards: a block's background went down
// before those of the blocks inside it, so they answer before it does.
std::optional<PointHit> hit_blocks(Fragment const& box, Probe at)
{
    for (auto child = box.children.rbegin(); child != box.children.rend(); ++child) {
        if (child->floating || paints_as_layer(*child) || paints_whole(box, *child))
            continue;
        std::optional<PointHit> hit;
        if (clip_lets_through(*child, at))
            hit = hit_blocks(*child, at);
        if (!hit && block_level(*child) && child->element && hears_pointer(child->style)
            && holds(child->x, child->y, child->width, child->height, at))
            hit = PointHit { child->element, &*child, PointHit::Part::Box };
        if (hit)
            return hit;
    }
    return std::nullopt;
}

// `paint_contents` backwards: the inline content, the floats under it, the
// blocks' backgrounds under those — all of it inside the box's clip.
std::optional<PointHit> hit_contents(Fragment const& box, Probe at)
{
    if (!clip_lets_through(box, at))
        return std::nullopt;
    if (std::optional<PointHit> const hit = hit_inline_content(box, at))
        return hit;
    if (std::optional<PointHit> const hit = hit_floats(box, at))
        return hit;
    return hit_blocks(box, at);
}

// `paint_flow` backwards: what flows inside the box, then the box.
std::optional<PointHit> hit_flow(Fragment const& box, Probe at)
{
    if (std::optional<PointHit> const hit = hit_contents(box, at))
        return hit;
    return hit_box(box, at);
}

// A layer of a stacking context and whether the point is inside the clip
// its ancestors put on it — `Layer`, asked of one point.
struct HitLayer {
    Fragment const* box = nullptr;
    bool in_reach = true;
};

// `collect_positioned`, with the two clips as the two answers they give
// for this point.
void collect_hit_layers(Fragment const& fragment, Probe at, bool in_flow, bool out_of_flow,
    std::vector<HitLayer>& out)
{
    for (Fragment const& child : fragment.children) {
        if (paints_as_layer(child))
            out.push_back(HitLayer { &child, child.fixed ? true : child.out_of_flow ? out_of_flow : in_flow });
        if (child.stacking_context)
            continue;
        bool const through = clip_lets_through(child, at);
        collect_hit_layers(child, at, in_flow && through,
            child.positioned ? out_of_flow && through : out_of_flow, out);
    }
}

// `paint_opaque_context` backwards: the layers at zero and above from the
// top down, the context's own flow, the layers below zero, its own box.
std::optional<PointHit> hit_context(Fragment const& root, Probe at, bool in_own_plane = false)
{
    if (root.back_hidden)
        return std::nullopt;
    // A box drawn through a transform is pointed at through it: the point
    // is taken back to where it lies in the box's own plane.
    if (root.transform && !in_own_plane) {
        std::optional<css::PlaneTransform> const back
            = root.transform->about(static_cast<double>(root.x), static_cast<double>(root.y)).inverse();
        double x = 0;
        double y = 0;
        if (!back || !back->map(static_cast<double>(at.x), static_cast<double>(at.y), x, y))
            return std::nullopt;
        return hit_context(root, Probe { static_cast<float>(x), static_cast<float>(y) }, true);
    }
    // Outside its clip path nothing of the box is there to point at
    // (css-masking-1 §5.1: pointer events are not dispatched on the
    // clipped-away region).
    if (root.style && root.style->clip_path && !clip_holds(*root.style->clip_path, clip_box_of(root, 0, 0), at.x, at.y))
        return std::nullopt;
    bool const inside = clip_lets_through(root, at);
    std::vector<HitLayer> layers;
    collect_hit_layers(root, at, inside, root.positioned ? inside : true, layers);
    put_in_tree_order(layers);
    std::stable_sort(layers.begin(), layers.end(),
        [](HitLayer const& a, HitLayer const& b) { return a.box->z_index < b.box->z_index; });
    auto const hit_one = [&](HitLayer const& layer) -> std::optional<PointHit> {
        if (!layer.in_reach)
            return std::nullopt;
        return layer.box->stacking_context ? hit_context(*layer.box, at) : hit_flow(*layer.box, at);
    };
    for (auto layer = layers.rbegin(); layer != layers.rend(); ++layer) {
        if (layer->box->z_index < 0)
            break;
        if (std::optional<PointHit> const hit = hit_one(*layer))
            return hit;
    }
    if (std::optional<PointHit> const hit = hit_contents(root, at))
        return hit;
    for (auto layer = layers.rbegin(); layer != layers.rend(); ++layer) {
        if (layer->box->z_index >= 0)
            continue;
        if (std::optional<PointHit> const hit = hit_one(*layer))
            return hit;
    }
    return hit_box(root, at);
}

} // namespace

bool within_clip(layout::Fragment const& box, float px, float py)
{
    return clip_lets_through(box, Probe { px, py });
}

std::optional<PointHit> hit_test(layout::Fragment const& root, float px, float py)
{
    Probe const at { px, py };
    if (std::optional<PointHit> const hit = hit_context(root, at))
        return hit;
    // The canvas is the root element's: a point on the bare page under a
    // short document is a point on its root, as it is in every engine.
    if (root.element && hears_pointer(root.style))
        return PointHit { root.element, &root, PointHit::Part::Box };
    return std::nullopt;
}

std::optional<PointHit> hit_test(layout::Fragment const& root, float px, float py,
    std::function<css::ComputedStyle const*(dom::Element const&)> const& styles)
{
    std::optional<PointHit> hit = hit_test(root, px, py);
    if (!hit || !hit->element || !hit->box || !hit->element->is_svg("svg") || !hit->box->image || !styles)
        return hit;
    layout::Fragment::ImageBox const& picture = *hit->box->image;
    if (dom::Element const* const inside = svg::element_at(*hit->element, picture.width, picture.height,
            svg::Point { px - picture.x, py - picture.y }, styles))
        hit->element = inside;
    return hit;
}

// The box whose background became the canvas's: the root, or the body it
// handed the job to when it had nothing of its own.
Fragment const* canvas_background_owner(layout::LayoutResult const& page)
{
    if (!page.canvas_background_from_body)
        return &page.root;
    for (Fragment const& child : page.root.children) {
        if (child.element && child.element->is_html("body"))
            return &child;
    }
    return nullptr;
}

std::optional<ScrollbarGeometry> vertical_scrollbar(
    layout::Fragment const& box, layout::ScrollOffset offset)
{
    return vertical_bar(box, offset);
}

std::optional<ScrollbarGeometry> horizontal_scrollbar(
    layout::Fragment const& box, layout::ScrollOffset offset)
{
    return horizontal_bar(box, offset);
}

void paint_page(Bitmap& target, layout::LayoutResult const& page, float offset_x, float offset_y,
    layout::BackgroundImages const* backgrounds, layout::ScrollOffsets const* scrolls, std::vector<PaintedPicture>* pictures,
    std::vector<Bitmap const*> const* shown_apart)
{
    target.fill_rect(Rect { 0, 0, target.width(), target.height() }, page.canvas_background);
    if (!page.root.style)
        return;
    // The html fragment's own background became the canvas, so it skips
    // itself. A promoted opaque body background repaints the same color over
    // its own box, which is invisible; translucent body backgrounds are the
    // one known double-composite, noted for the reftest era.
    Fragment const* const owner = canvas_background_owner(page);
    Context context { target, offset_x, offset_y, backgrounds, owner, scrolls, page.device_scale, target.clip(), pictures, shown_apart };
    Elisions elisions;
    context.elisions = &elisions;
    // The canvas takes the whole background of the box that owns it, its
    // pictures included: they cover the surface, sized and placed against
    // the root element's box whichever box they came from.
    if (owner && owner->style && owner->style->background_images
        && !owner->style->background_images->empty()) {
        Rect const whole { 0, 0, target.width(), target.height() };
        paint_background_layers(context, page.root, &whole, owner->style);
    }
    paint_stacking_context(context, page.root, true);
}

CaretsShown::CaretsShown(bool shown)
    : m_was(g_carets_shown)
{
    g_carets_shown = shown;
}

CaretsShown::~CaretsShown()
{
    g_carets_shown = m_was;
}

}
