#include "paint/Effects.h"

#include "paint/Blur.h"
#include "svg/Svg.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sashfold::paint {

namespace {

float resolve(css::LengthPercent const& length, float base)
{
    switch (length.kind) {
    case css::LengthPercent::Kind::Px: return length.value;
    case css::LengthPercent::Kind::Percent: return base * length.value / 100.0f;
    case css::LengthPercent::Kind::Calc: return length.value + base * length.percent / 100.0f;
    default: return 0;
    }
}

std::size_t area_of(EffectLayer const& layer)
{
    return static_cast<std::size_t>(layer.width) * static_cast<std::size_t>(layer.height);
}

EffectLayer empty_layer(int width, int height)
{
    EffectLayer layer;
    layer.width = std::max(0, width);
    layer.height = std::max(0, height);
    std::size_t const area = area_of(layer);
    layer.red.assign(area, 0.0f);
    layer.green.assign(area, 0.0f);
    layer.blue.assign(area, 0.0f);
    layer.alpha.assign(area, 0.0f);
    return layer;
}

// A color matrix over the premultiplied colors: with no constant term it
// commutes with the premultiplication, and the result is held within the
// pixel's own alpha as a straight color is held within one.
void color_matrix(EffectLayer& layer, float const m[9])
{
    std::size_t const area = area_of(layer);
    for (std::size_t i = 0; i < area; ++i) {
        float const r = layer.red[i];
        float const g = layer.green[i];
        float const b = layer.blue[i];
        float const a = layer.alpha[i];
        layer.red[i] = std::clamp(m[0] * r + m[1] * g + m[2] * b, 0.0f, a);
        layer.green[i] = std::clamp(m[3] * r + m[4] * g + m[5] * b, 0.0f, a);
        layer.blue[i] = std::clamp(m[6] * r + m[7] * g + m[8] * b, 0.0f, a);
    }
}

// Each straight color c becomes c * slope + intercept: on premultiplied
// colors the intercept is scaled by the pixel's alpha.
void color_line(EffectLayer& layer, float slope, float intercept)
{
    std::size_t const area = area_of(layer);
    for (std::size_t i = 0; i < area; ++i) {
        float const a = layer.alpha[i];
        layer.red[i] = std::clamp(layer.red[i] * slope + intercept * a, 0.0f, a);
        layer.green[i] = std::clamp(layer.green[i] * slope + intercept * a, 0.0f, a);
        layer.blue[i] = std::clamp(layer.blue[i] * slope + intercept * a, 0.0f, a);
    }
}

void blur(EffectLayer& layer, float sigma)
{
    if (sigma <= 0 || layer.width <= 0 || layer.height <= 0)
        return;
    for (std::vector<float>* const plane : { &layer.red, &layer.green, &layer.blue, &layer.alpha })
        gaussian_plane(*plane, layer.width, layer.height, static_cast<double>(sigma));
}

// The layer's own shape, moved and blurred, in one color, laid under it.
void drop_shadow(EffectLayer& layer, css::FilterFunction const& shadow, Color color)
{
    int const dx = static_cast<int>(std::lround(static_cast<double>(shadow.x)));
    int const dy = static_cast<int>(std::lround(static_cast<double>(shadow.y)));
    std::vector<float> shape(area_of(layer), 0.0f);
    for (int y = 0; y < layer.height; ++y) {
        int const from_y = y - dy;
        if (from_y < 0 || from_y >= layer.height)
            continue;
        for (int x = 0; x < layer.width; ++x) {
            int const from_x = x - dx;
            if (from_x < 0 || from_x >= layer.width)
                continue;
            shape[static_cast<std::size_t>(y) * static_cast<std::size_t>(layer.width) + static_cast<std::size_t>(x)]
                = layer.alpha[static_cast<std::size_t>(from_y) * static_cast<std::size_t>(layer.width) + static_cast<std::size_t>(from_x)];
        }
    }
    if (shadow.amount > 0)
        gaussian_plane(shape, layer.width, layer.height, static_cast<double>(shadow.amount));
    float const strength = static_cast<float>(color.a) / 255.0f;
    float const r = static_cast<float>(color.r) / 255.0f;
    float const g = static_cast<float>(color.g) / 255.0f;
    float const b = static_cast<float>(color.b) / 255.0f;
    std::size_t const area = area_of(layer);
    for (std::size_t i = 0; i < area; ++i) {
        float const under = std::clamp(shape[i], 0.0f, 1.0f) * strength;
        float const through = 1.0f - layer.alpha[i];
        layer.red[i] += through * under * r;
        layer.green[i] += through * under * g;
        layer.blue[i] += through * under * b;
        layer.alpha[i] += through * under;
    }
}

int reach_of_blur(float sigma)
{
    return sigma <= 0 ? 0 : static_cast<int>(std::ceil(static_cast<double>(sigma) * 3.0));
}

// The reference box the clip path names is the caller's to give; what a
// radius of a circle or an ellipse comes to against it.
float resolve_radius(css::ClipPath::Radius const& radius, float center, float near_edge, float far_edge, float base)
{
    float const to_near = std::abs(center - near_edge);
    float const to_far = std::abs(far_edge - center);
    switch (radius.kind) {
    case css::ClipPath::Radius::Kind::ClosestSide: return std::min(to_near, to_far);
    case css::ClipPath::Radius::Kind::FarthestSide: return std::max(to_near, to_far);
    case css::ClipPath::Radius::Kind::Length: break;
    }
    return std::max(0.0f, resolve(radius.length, base));
}

void add_ellipse(svg::Path& path, float cx, float cy, float rx, float ry)
{
    if (rx <= 0 || ry <= 0)
        return;
    path.move_to({ cx + rx, cy });
    path.arc_to(rx, ry, 0, false, true, { cx - rx, cy });
    path.arc_to(rx, ry, 0, false, true, { cx + rx, cy });
    path.close();
}

// A rectangle with elliptical corners, the radii shrunk together where
// two of them overrun an edge (css-backgrounds-3 §5.5).
void add_rounded_rect(svg::Path& path, float x, float y, float width, float height, float radii[8])
{
    if (width <= 0 || height <= 0)
        return;
    float factor = 1;
    auto const fit = [&](float a, float b, float edge) {
        if (a + b > edge && a + b > 0)
            factor = std::min(factor, edge / (a + b));
    };
    fit(radii[0], radii[2], width);
    fit(radii[4], radii[6], width);
    fit(radii[1], radii[7], height);
    fit(radii[3], radii[5], height);
    float r[8];
    for (int i = 0; i < 8; ++i)
        r[i] = std::max(0.0f, radii[i] * factor);
    auto const corner = [&](float rx, float ry, svg::Point to) {
        if (rx > 0 && ry > 0)
            path.arc_to(rx, ry, 0, false, true, to);
        else
            path.line_to(to);
    };
    path.move_to({ x + r[0], y });
    path.line_to({ x + width - r[2], y });
    corner(r[2], r[3], { x + width, y + r[3] });
    path.line_to({ x + width, y + height - r[5] });
    corner(r[4], r[5], { x + width - r[4], y + height });
    path.line_to({ x + r[6], y + height });
    corner(r[6], r[7], { x, y + height - r[7] });
    path.line_to({ x, y + r[1] });
    corner(r[0], r[1], { x + r[0], y });
    path.close();
}

}

EffectLayer layer_from(Bitmap const& over_black, Bitmap const& over_white)
{
    EffectLayer layer = empty_layer(over_black.width(), over_black.height());
    std::vector<std::uint8_t> const& black = over_black.pixels();
    std::vector<std::uint8_t> const& white = over_white.pixels();
    std::size_t const area = area_of(layer);
    if (white.size() < area * 4 || black.size() < area * 4)
        return layer;
    for (std::size_t i = 0; i < area; ++i) {
        std::uint8_t const* const b = &black[i * 4];
        std::uint8_t const* const w = &white[i * 4];
        // A place opened to what is behind the page (a video shown apart)
        // cannot be carried through a filter: it stays out of the group.
        if (b[3] == 0 && w[3] == 0)
            continue;
        int const spread = (static_cast<int>(w[0]) - b[0]) + (static_cast<int>(w[1]) - b[1]) + (static_cast<int>(w[2]) - b[2]);
        float const alpha = std::clamp(1.0f - static_cast<float>(spread) / (3.0f * 255.0f), 0.0f, 1.0f);
        layer.alpha[i] = alpha;
        layer.red[i] = std::min(alpha, static_cast<float>(b[0]) / 255.0f);
        layer.green[i] = std::min(alpha, static_cast<float>(b[1]) / 255.0f);
        layer.blue[i] = std::min(alpha, static_cast<float>(b[2]) / 255.0f);
    }
    return layer;
}

EffectLayer layer_of(Bitmap const& picture)
{
    EffectLayer layer = empty_layer(picture.width(), picture.height());
    std::vector<std::uint8_t> const& pixels = picture.pixels();
    std::size_t const area = area_of(layer);
    if (pixels.size() < area * 4)
        return layer;
    for (std::size_t i = 0; i < area; ++i) {
        layer.red[i] = static_cast<float>(pixels[i * 4]) / 255.0f;
        layer.green[i] = static_cast<float>(pixels[i * 4 + 1]) / 255.0f;
        layer.blue[i] = static_cast<float>(pixels[i * 4 + 2]) / 255.0f;
        layer.alpha[i] = 1.0f;
    }
    return layer;
}

Bitmap bitmap_of(EffectLayer const& layer)
{
    std::size_t const area = area_of(layer);
    std::vector<std::uint8_t> pixels(area * 4, 0);
    auto const byte = [](float value) { return static_cast<std::uint8_t>(std::lround(static_cast<double>(std::clamp(value, 0.0f, 1.0f)) * 255.0)); };
    for (std::size_t i = 0; i < area; ++i) {
        float const alpha = std::clamp(layer.alpha[i], 0.0f, 1.0f);
        if (alpha <= 0)
            continue;
        pixels[i * 4] = byte(layer.red[i] / alpha);
        pixels[i * 4 + 1] = byte(layer.green[i] / alpha);
        pixels[i * 4 + 2] = byte(layer.blue[i] / alpha);
        pixels[i * 4 + 3] = byte(alpha);
    }
    return Bitmap(layer.width, layer.height, std::move(pixels));
}

Reach reach_of(css::FilterList const& filters)
{
    Reach reach;
    for (css::FilterFunction const& function : filters) {
        if (function.kind == css::FilterFunction::Kind::Blur) {
            int const by = reach_of_blur(function.amount);
            reach.left += by;
            reach.top += by;
            reach.right += by;
            reach.bottom += by;
        } else if (function.kind == css::FilterFunction::Kind::DropShadow) {
            int const by = reach_of_blur(function.amount);
            int const dx = static_cast<int>(std::lround(static_cast<double>(function.x)));
            int const dy = static_cast<int>(std::lround(static_cast<double>(function.y)));
            reach.left += std::max(0, by - dx);
            reach.right += std::max(0, by + dx);
            reach.top += std::max(0, by - dy);
            reach.bottom += std::max(0, by + dy);
        }
    }
    return reach;
}

void apply_filters(EffectLayer& layer, css::FilterList const& filters, Color current)
{
    using Kind = css::FilterFunction::Kind;
    for (css::FilterFunction const& function : filters) {
        float const amount = function.amount;
        switch (function.kind) {
        case Kind::Blur: blur(layer, amount); break;
        case Kind::Brightness: color_line(layer, amount, 0); break;
        case Kind::Contrast: color_line(layer, amount, 0.5f - 0.5f * amount); break;
        case Kind::Invert: color_line(layer, 1 - 2 * amount, amount); break;
        case Kind::Opacity: {
            float const keep = std::clamp(amount, 0.0f, 1.0f);
            for (std::vector<float>* const plane : { &layer.red, &layer.green, &layer.blue, &layer.alpha }) {
                for (float& value : *plane)
                    value *= keep;
            }
            break;
        }
        case Kind::Grayscale: {
            float const s = 1 - std::clamp(amount, 0.0f, 1.0f);
            float const m[9] = { 0.2126f + 0.7874f * s, 0.7152f - 0.7152f * s, 0.0722f - 0.0722f * s,
                0.2126f - 0.2126f * s, 0.7152f + 0.2848f * s, 0.0722f - 0.0722f * s,
                0.2126f - 0.2126f * s, 0.7152f - 0.7152f * s, 0.0722f + 0.9278f * s };
            color_matrix(layer, m);
            break;
        }
        case Kind::Sepia: {
            float const s = 1 - std::clamp(amount, 0.0f, 1.0f);
            float const m[9] = { 0.393f + 0.607f * s, 0.769f - 0.769f * s, 0.189f - 0.189f * s,
                0.349f - 0.349f * s, 0.686f + 0.314f * s, 0.168f - 0.168f * s,
                0.272f - 0.272f * s, 0.534f - 0.534f * s, 0.131f + 0.869f * s };
            color_matrix(layer, m);
            break;
        }
        case Kind::Saturate: {
            float const s = amount;
            float const m[9] = { 0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s,
                0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s,
                0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s };
            color_matrix(layer, m);
            break;
        }
        case Kind::HueRotate: {
            double const radians = static_cast<double>(amount) * 3.14159265358979323846 / 180.0;
            auto const c = static_cast<float>(std::cos(radians));
            auto const s = static_cast<float>(std::sin(radians));
            float const m[9] = { 0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f,
                0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f,
                0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f };
            color_matrix(layer, m);
            break;
        }
        case Kind::DropShadow: drop_shadow(layer, function, function.current_color ? current : function.color); break;
        }
    }
}

std::vector<svg::Polygon> clip_outline(css::ClipPath const& clip, ShapeBox const& box, bool& even_odd)
{
    using Shape = css::ClipPath::Shape;
    even_odd = clip.even_odd;
    svg::Path path;
    switch (clip.shape) {
    case Shape::Box:
        path.move_to({ box.x, box.y });
        path.line_to({ box.x + box.width, box.y });
        path.line_to({ box.x + box.width, box.y + box.height });
        path.line_to({ box.x, box.y + box.height });
        path.close();
        break;
    case Shape::Inset: {
        float const top = resolve(clip.inset[0], box.height);
        float const right = resolve(clip.inset[1], box.width);
        float const bottom = resolve(clip.inset[2], box.height);
        float const left = resolve(clip.inset[3], box.width);
        // Offsets that pass each other leave nothing, shrunk in proportion
        // (css-shapes-1 §3.1).
        float width = box.width - left - right;
        float height = box.height - top - bottom;
        float x = box.x + left;
        float y = box.y + top;
        if (width < 0) {
            x = box.x + (left + right > 0 ? box.width * left / (left + right) : 0);
            width = 0;
        }
        if (height < 0) {
            y = box.y + (top + bottom > 0 ? box.height * top / (top + bottom) : 0);
            height = 0;
        }
        float radii[8];
        for (int i = 0; i < 8; ++i)
            radii[i] = std::max(0.0f, resolve(clip.radii[i], i % 2 == 0 ? box.width : box.height));
        add_rounded_rect(path, x, y, width, height, radii);
        break;
    }
    case Shape::Circle: {
        float const cx = box.x + resolve(clip.center_x, box.width);
        float const cy = box.y + resolve(clip.center_y, box.height);
        float radius = 0;
        if (clip.radius_x.kind == css::ClipPath::Radius::Kind::Length) {
            // A percentage is of the box's diagonal over the square root of two.
            double const across = static_cast<double>(box.width);
            double const down = static_cast<double>(box.height);
            auto const base = static_cast<float>(std::sqrt((across * across + down * down) / 2.0));
            radius = std::max(0.0f, resolve(clip.radius_x.length, base));
        } else {
            float const across = resolve_radius(clip.radius_x, cx, box.x, box.x + box.width, box.width);
            float const down = resolve_radius(clip.radius_x, cy, box.y, box.y + box.height, box.height);
            radius = clip.radius_x.kind == css::ClipPath::Radius::Kind::ClosestSide ? std::min(across, down) : std::max(across, down);
        }
        add_ellipse(path, cx, cy, radius, radius);
        break;
    }
    case Shape::Ellipse: {
        float const cx = box.x + resolve(clip.center_x, box.width);
        float const cy = box.y + resolve(clip.center_y, box.height);
        add_ellipse(path, cx, cy, resolve_radius(clip.radius_x, cx, box.x, box.x + box.width, box.width),
            resolve_radius(clip.radius_y, cy, box.y, box.y + box.height, box.height));
        break;
    }
    case Shape::Polygon:
        for (std::size_t i = 0; i < clip.points.size(); ++i) {
            svg::Point const point { box.x + resolve(clip.points[i].first, box.width), box.y + resolve(clip.points[i].second, box.height) };
            if (i == 0)
                path.move_to(point);
            else
                path.line_to(point);
        }
        path.close();
        break;
    case Shape::Path:
        path = svg::parse_path_data(clip.path).transformed(svg::Matrix::scale(clip.path_scale, clip.path_scale).then(svg::Matrix::translate(box.x, box.y)));
        break;
    }
    std::vector<svg::Polygon> polygons;
    for (svg::Polyline& line : svg::flatten(path)) {
        if (line.points.size() >= 3)
            polygons.push_back(std::move(line.points));
    }
    return polygons;
}

bool clip_holds(css::ClipPath const& clip, ShapeBox const& box, float px, float py)
{
    bool even_odd = false;
    std::vector<svg::Polygon> const polygons = clip_outline(clip, box, even_odd);
    // The winding number of the outline around the point, and how many
    // times a ray to the right crosses it.
    int winding = 0;
    int crossings = 0;
    for (svg::Polygon const& polygon : polygons) {
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            svg::Point const a = polygon[i];
            svg::Point const b = polygon[(i + 1) % polygon.size()];
            if ((a.y <= py) == (b.y <= py))
                continue;
            float const at = a.x + (py - a.y) * (b.x - a.x) / (b.y - a.y);
            if (at <= px)
                continue;
            ++crossings;
            winding += b.y > a.y ? 1 : -1;
        }
    }
    return even_odd ? crossings % 2 == 1 : winding != 0;
}

void cut_to(EffectLayer& layer, std::vector<svg::Polygon> const& outline, bool even_odd, int left, int top)
{
    svg::Mask const mask = svg::rasterize(outline, even_odd ? svg::FillRule::EvenOdd : svg::FillRule::NonZero, left, top,
        left + layer.width, top + layer.height);
    for (int y = 0; y < layer.height; ++y) {
        for (int x = 0; x < layer.width; ++x) {
            std::uint8_t const covered = mask.empty() ? std::uint8_t { 0 } : mask.at(left + x, top + y);
            if (covered == 255)
                continue;
            std::size_t const i = static_cast<std::size_t>(y) * static_cast<std::size_t>(layer.width) + static_cast<std::size_t>(x);
            float const keep = static_cast<float>(covered) / 255.0f;
            layer.red[i] *= keep;
            layer.green[i] *= keep;
            layer.blue[i] *= keep;
            layer.alpha[i] *= keep;
        }
    }
}

}
