#include "paint/Painter.h"

#include "text/Face.h"
#include "text/FontManager.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace sashfold::paint {

namespace {

using css::BorderStyle;
using css::ComputedStyle;
using layout::Fragment;
using layout::TextRun;

struct Context {
    Bitmap& target;
    float dx;
    float dy;
    layout::BackgroundImages const* backgrounds;
};

int round_px(float value)
{
    return static_cast<int>(value >= 0 ? value + 0.5f : value - 0.5f);
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
void paint_background_layers(Context& context, Fragment const& fragment)
{
    ComputedStyle const& style = *fragment.style;
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
        Area const painting = box_of(context, fragment, clip);
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
        // The tiles.
        float start_x = x0;
        float end_x = x0 + w;
        float start_y = y0;
        float end_y = y0 + h;
        if (repeat.x == css::BackgroundRepeat::Repeat) {
            start_x = x0 - std::ceil((x0 - painting.x) / w) * w;
            end_x = painting.x + painting.width;
        }
        if (repeat.y == css::BackgroundRepeat::Repeat) {
            start_y = y0 - std::ceil((y0 - painting.y) / h) * h;
            end_y = painting.y + painting.height;
        }
        context.target.set_clip(clip_rect);
        std::size_t const rounds = context.target.round_clip_depth();
        if (style.rounded()) {
            RoundedRect const curve = rounded_area(context, fragment, clip);
            if (!curve.is_rectangular())
                context.target.push_round_clip(curve);
        }
        int tiles = 0;
        for (float ty = start_y; ty < end_y && tiles < 100000; ty += h) {
            for (float tx = start_x; tx < end_x && tiles < 100000; tx += w) {
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
    context.target.fill_ring(outer, inner, [&](float px, float py, Color& color) {
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
    });
}

void paint_background_and_borders(Context& context, Fragment const& fragment,
    bool skip_background)
{
    ComputedStyle const& style = *fragment.style;
    float const x = fragment.x + context.dx;
    float const y = fragment.y + context.dy;
    RoundedRect const shape = style.rounded() ? rounded_box(context, fragment) : RoundedRect {};
    bool const round = !shape.is_rectangular();
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

    if (round) {
        paint_rounded_borders(context, fragment, shape);
        return;
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

void paint_run(Context& context, TextRun const& run)
{
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
    bool const italic = style.font_style == css::FontStyle::Italic;
    float const start_x = run.x + context.dx;
    float x = start_x;
    for (char32_t const c : run.text) {
        text::FontStack::Glyph const glyph = run.fonts->glyph_for(c);
        glyph.face->draw_glyph(context.target, glyph.glyph, x, baseline, style.font_size,
            style.color, style.bold(), italic);
        x += glyph.face->advance(glyph.glyph, style.font_size);
    }

    if (style.text_decoration == css::TextDecorationLine::None || run.text.empty())
        return;
    float const width = x - start_x;
    float const thickness = std::max(1.0f, style.font_size / 14.0f);
    float line_y;
    if (style.text_decoration == css::TextDecorationLine::Underline)
        line_y = baseline + style.font_size * 2.0f / 32.0f + 1.0f;
    else
        line_y = baseline - style.font_size * 8.0f / 32.0f;
    context.target.fill_rect(snap(start_x, line_y, width, thickness), style.color);
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
    bool const author_look = style.background_color.a != 0 || solid(style.border_top)
        || solid(style.border_bottom) || solid(style.border_left) || solid(style.border_right);
    Color const border = control.disabled ? Color::rgb(0xc0, 0xc0, 0xc0) : Color::rgb(0x76, 0x76, 0x76);
    Color const white = Color::rgb(0xff, 0xff, 0xff);
    Color const gray = Color::rgb(0xef, 0xef, 0xef);
    Color const ink = control.disabled ? Color::rgb(0x8d, 0x8d, 0x8d) : Color::rgb(0x1a, 0x1a, 0x1a);
    auto const frame = [&](Rect const& r, Color color) {
        context.target.fill_rect(Rect { r.x, r.y, r.width, 1 }, color);
        context.target.fill_rect(Rect { r.x, r.y + r.height - 1, r.width, 1 }, color);
        context.target.fill_rect(Rect { r.x, r.y, 1, r.height }, color);
        context.target.fill_rect(Rect { r.x + r.width - 1, r.y, 1, r.height }, color);
    };
    auto const inset = [](Rect const& r, int by) {
        return Rect { r.x + by, r.y + by, r.width - 2 * by, r.height - 2 * by };
    };
    switch (control.kind) {
    case ControlKind::Checkbox:
        context.target.fill_round_rect(rect, 2, border);
        context.target.fill_round_rect(inset(rect, 1), 2, control.disabled ? gray : white);
        if (control.checked)
            context.target.fill_rect(inset(rect, 3), ink);
        break;
    case ControlKind::Radio: {
        int const radius = rect.width / 2;
        context.target.fill_round_rect(rect, radius, border);
        context.target.fill_round_rect(inset(rect, 1), radius - 1, control.disabled ? gray : white);
        if (control.checked)
            context.target.fill_round_rect(inset(rect, 4), radius - 4, ink);
        break;
    }
    default: {
        bool const button = control.kind == ControlKind::Submit || control.kind == ControlKind::Button
            || control.kind == ControlKind::File;
        if (!author_look) {
            context.target.fill_rect(rect, button || control.disabled ? gray : white);
            frame(rect, border);
        }
        if (control.kind == ControlKind::Select) {
            // A small triangle pointing down, near the right edge.
            int const tip_x = rect.x + rect.width - 12;
            int const top = rect.y + rect.height / 2 - 2;
            for (int row = 0; row < 4; ++row)
                context.target.fill_rect(Rect { tip_x - 3 + row, top + row, 7 - 2 * row, 1 }, ink);
        }
        break;
    }
    }
    if (control.focused) {
        Color const accent = Color::rgb(0x00, 0x60, 0xdf);
        frame(rect, accent);
        frame(inset(rect, 1), accent);
    }
    if (control.caret_x)
        context.target.fill_rect(snap(*control.caret_x + context.dx, y + 4, 1, control.height - 8), ink);
}

void paint_stacking_context(Context& context, Fragment const& root, bool is_canvas_background_owner);
void paint_flow(Context& context, Fragment const& fragment, bool is_canvas_background_owner);

// The box's own painting: background, borders, picture, control — nothing
// when its visibility is hidden (the box keeps its room).
void paint_box(Context& context, Fragment const& fragment, bool skip_background)
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
    if (fragment.control)
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
        context.target.draw_scaled(*box.bitmap,
            snap(box.x + context.dx, box.y + context.dy, box.width, box.height));
        context.target.truncate_round_clips(rounds);
    }
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

// What flows inside a box: its in-flow children, the floats over them (a
// float paints above the blocks whose lines flow around it), and its own
// lines, clipped to the box when its overflow is hidden. The positioned
// descendants are not here: they belong to the stacking context and paint
// at their level in it; a child that is a stacking context of its own
// (opacity below one) paints as one unit.
void paint_contents(Context& context, Fragment const& fragment)
{
    std::size_t const rounds = context.target.round_clip_depth();
    std::optional<Rect> const restore = clip_for(context, fragment);
    for (Fragment const& child : fragment.children) {
        if (child.floating || child.positioned)
            continue;
        if (child.stacking_context)
            paint_stacking_context(context, child, false);
        else
            paint_flow(context, child, false);
    }
    for (Fragment const& child : fragment.children) {
        if (child.floating && !child.positioned) {
            if (child.stacking_context)
                paint_stacking_context(context, child, false);
            else
                paint_flow(context, child, false);
        }
    }
    for (TextRun const& run : fragment.runs) {
        if (!run.style->hidden())
            paint_run(context, run);
    }
    context.target.truncate_round_clips(rounds);
    context.target.set_clip(restore);
}

// A box and what flows inside it.
void paint_flow(Context& context, Fragment const& fragment, bool is_canvas_background_owner)
{
    paint_box(context, fragment, is_canvas_background_owner);
    paint_contents(context, fragment);
}

// A positioned box a stacking context paints, with the clip the ancestors
// that contain it impose: every overflow-clipping ancestor for a box in
// flow (relative, sticky); only the positioned ones — its containing
// blocks — for an absolutely or fixed positioned box.
struct Layer {
    Fragment const* box = nullptr;
    std::optional<Rect> clip;
};

// The positioned boxes a stacking context paints: every positioned
// descendant reached without crossing another stacking context (a
// positioned box with z-index auto is walked through — its positioned
// descendants are the parent context's, per CSS 2.1 Appendix E).
void collect_positioned(Context const& context, Fragment const& fragment, std::optional<Rect> const& clip_in_flow,
    std::optional<Rect> const& clip_out_of_flow, std::vector<Layer>& out)
{
    for (Fragment const& child : fragment.children) {
        if (child.positioned)
            out.push_back(Layer { &child, child.out_of_flow ? clip_out_of_flow : clip_in_flow });
        if (child.stacking_context)
            continue;
        std::optional<Rect> const inner = clip_within(context, child, clip_in_flow);
        collect_positioned(context, child, inner,
            child.positioned ? clip_within(context, child, clip_out_of_flow) : clip_out_of_flow, out);
    }
}

// CSS 2.1 Appendix E, the reader-web subset: the context's own flow first,
// then its positioned descendants by z-index — negative ones under the
// flow — in tree order within a level; a descendant that is itself a
// stacking context paints as one unit, one with z-index auto as its flow
// alone (its positioned descendants are already in this list).
void paint_stacking_context(Context& context, Fragment const& root, bool is_canvas_background_owner)
{
    // A context at opacity zero paints nothing, positioned descendants included.
    if (root.style && root.style->opacity <= 0)
        return;
    // The root's overflow clips everything inside it that it contains: its
    // flow, and the positioned descendants whose containing block it is.
    std::optional<Rect> const outer = context.target.clip();
    std::optional<Rect> const inner = clip_within(context, root, outer);
    std::vector<Layer> positioned;
    collect_positioned(context, root, inner, root.positioned ? inner : outer, positioned);
    std::stable_sort(positioned.begin(), positioned.end(),
        [](Layer const& a, Layer const& b) { return a.box->z_index < b.box->z_index; });
    auto const paint_one = [&](Layer const& layer) {
        context.target.set_clip(layer.clip);
        if (layer.box->stacking_context)
            paint_stacking_context(context, *layer.box, false);
        else
            paint_flow(context, *layer.box, false);
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
    context.target.set_clip(outer);
}

} // namespace

void paint_page(Bitmap& target, layout::LayoutResult const& page, float offset_x, float offset_y,
    layout::BackgroundImages const* backgrounds)
{
    target.fill_rect(Rect { 0, 0, target.width(), target.height() }, page.canvas_background);
    if (!page.root.style)
        return;
    // The html fragment's own background became the canvas, so it skips
    // itself. A promoted opaque body background repaints the same color over
    // its own box, which is invisible; translucent body backgrounds are the
    // one known double-composite, noted for the reftest era.
    Context context { target, offset_x, offset_y, backgrounds };
    paint_stacking_context(context, page.root, true);
}

}
