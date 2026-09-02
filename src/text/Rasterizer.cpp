#include "text/Rasterizer.h"

#include <algorithm>
#include <cstdlib>
#include <optional>

namespace sashfold::text {

namespace {

struct Point {
    std::int64_t x; // 1/64 pixel, y down
    std::int64_t y;
};

struct Edge {
    std::int64_t x0, y0, x1, y1;
    int direction; // +1 when y grows along the edge
};

// Integer division with a stated rounding; every divisor here is positive.
std::int64_t div_floor(std::int64_t n, std::int64_t d)
{
    std::int64_t q = n / d;
    if (n % d != 0 && n < 0)
        --q;
    return q;
}

std::int64_t div_ceil(std::int64_t n, std::int64_t d)
{
    std::int64_t q = n / d;
    if (n % d != 0 && n > 0)
        ++q;
    return q;
}

std::int64_t div_round(std::int64_t n, std::int64_t d)
{
    return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d);
}

Point midpoint(Point a, Point b)
{
    return Point { div_round(a.x + b.x, 2), div_round(a.y + b.y, 2) };
}

class Flattener {
public:
    explicit Flattener(std::vector<Edge>& edges)
        : m_edges(edges)
    {
    }

    void line(Point a, Point b)
    {
        if (a.y != b.y)
            m_edges.push_back(Edge { a.x, a.y, b.x, b.y, b.y > a.y ? 1 : -1 });
    }

    // Uniform subdivision: a quadratic's chord error over n pieces is
    // |a - 2c + b| / (4 n^2), so n keeps every chord within 1/16 px.
    void quadratic(Point a, Point control, Point b)
    {
        std::int64_t const deviation = std::max(std::abs(a.x - 2 * control.x + b.x),
            std::abs(a.y - 2 * control.y + b.y));
        std::int64_t n = 1;
        while (n * n * 16 < deviation && n < 64)
            ++n;
        Point previous = a;
        for (std::int64_t i = 1; i <= n; ++i) {
            std::int64_t const w0 = (n - i) * (n - i);
            std::int64_t const w1 = 2 * i * (n - i);
            std::int64_t const w2 = i * i;
            Point const point { div_round(w0 * a.x + w1 * control.x + w2 * b.x, n * n),
                div_round(w0 * a.y + w1 * control.y + w2 * b.y, n * n) };
            line(previous, point);
            previous = point;
        }
    }

private:
    std::vector<Edge>& m_edges;
};

// Walks one TrueType contour: consecutive off-curve points imply an
// on-curve midpoint between them, and a contour may begin off-curve.
void flatten_contour(std::vector<Point> const& points, std::vector<bool> const& on_curve,
    Flattener& flattener)
{
    std::size_t const n = points.size();
    if (n < 2)
        return;
    std::size_t first_on = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (on_curve[i]) {
            first_on = i;
            break;
        }
    }
    Point start;
    std::size_t index;
    std::size_t count;
    if (first_on == n) {
        start = midpoint(points[n - 1], points[0]);
        index = 0;
        count = n;
    } else {
        start = points[first_on];
        index = (first_on + 1) % n;
        count = n - 1;
    }
    Point current = start;
    std::optional<Point> control;
    for (std::size_t step = 0; step < count; ++step) {
        std::size_t const i = (index + step) % n;
        Point const point = points[i];
        if (on_curve[i]) {
            if (control) {
                flattener.quadratic(current, *control, point);
                control.reset();
            } else {
                flattener.line(current, point);
            }
            current = point;
        } else {
            if (control) {
                Point const implied = midpoint(*control, point);
                flattener.quadratic(current, *control, implied);
                current = implied;
            }
            control = point;
        }
    }
    if (control)
        flattener.quadratic(current, *control, start);
    else
        flattener.line(current, start);
}

} // namespace

GlyphMask rasterize(GlyphOutline const& outline, int units_per_em, int size_q, RasterOptions options)
{
    GlyphMask mask;
    if (outline.points.empty() || outline.contour_ends.empty() || units_per_em <= 0 || size_q <= 0)
        return mask;

    // Font units to 1/64 pixel: size_q / 4 pixels per em.
    auto const to_device = [&](std::int16_t units) {
        return div_round(static_cast<std::int64_t>(units) * size_q * 16, units_per_em);
    };
    std::vector<Point> device(outline.points.size());
    for (std::size_t i = 0; i < outline.points.size(); ++i) {
        std::int64_t const y = -to_device(outline.points[i].y);
        std::int64_t x = to_device(outline.points[i].x);
        if (options.oblique)
            x += (-y) / 4;
        device[i] = Point { x, y };
    }

    std::vector<Edge> edges;
    Flattener flattener(edges);
    std::size_t begin = 0;
    for (std::uint16_t const end : outline.contour_ends) {
        if (end < begin || end >= device.size())
            break;
        std::vector<Point> const contour(device.begin() + static_cast<std::ptrdiff_t>(begin),
            device.begin() + static_cast<std::ptrdiff_t>(end) + 1);
        std::vector<bool> on_curve(contour.size());
        for (std::size_t k = 0; k < contour.size(); ++k)
            on_curve[k] = outline.points[begin + k].on_curve;
        flatten_contour(contour, on_curve, flattener);
        begin = end + 1u;
    }
    if (edges.empty())
        return mask;

    std::int64_t min_x = edges[0].x0;
    std::int64_t max_x = edges[0].x0;
    std::int64_t min_y = edges[0].y0;
    std::int64_t max_y = edges[0].y0;
    for (Edge const& edge : edges) {
        min_x = std::min({ min_x, edge.x0, edge.x1 });
        max_x = std::max({ max_x, edge.x0, edge.x1 });
        min_y = std::min({ min_y, edge.y0, edge.y1 });
        max_y = std::max({ max_y, edge.y0, edge.y1 });
    }
    std::int64_t const left = div_floor(min_x, 64);
    std::int64_t const right = div_ceil(max_x, 64); // exclusive
    std::int64_t const top = div_floor(min_y, 64);
    std::int64_t const bottom = div_ceil(max_y, 64);
    std::int64_t const width = right - left + (options.embolden ? 1 : 0);
    std::int64_t const height = bottom - top;
    if (width <= 0 || height <= 0 || width * height > 16'000'000)
        return mask; // a size no glyph on a page needs
    mask.left = static_cast<int>(left);
    mask.top = static_cast<int>(top);
    mask.width = static_cast<int>(width);
    mask.height = static_cast<int>(height);

    struct Crossing {
        std::int64_t x; // the first sample position at or right of the edge
        int direction;
    };
    std::vector<std::uint8_t> hits(static_cast<std::size_t>(width * height), 0);
    std::vector<Crossing> crossings;
    for (std::int64_t row = 0; row < height; ++row) {
        for (int sub = 0; sub < 4; ++sub) {
            std::int64_t const ys = (top + row) * 64 + 8 + 16 * sub;
            crossings.clear();
            for (Edge const& edge : edges) {
                if (ys < std::min(edge.y0, edge.y1) || ys >= std::max(edge.y0, edge.y1))
                    continue;
                // x on the edge at ys, exactly: its ceiling is the first
                // integer position not left of it, which is what the sample
                // test below needs.
                std::int64_t numerator = (edge.x1 - edge.x0) * (ys - edge.y0);
                std::int64_t denominator = edge.y1 - edge.y0;
                if (denominator < 0) {
                    numerator = -numerator;
                    denominator = -denominator;
                }
                crossings.push_back(Crossing { edge.x0 + div_ceil(numerator, denominator), edge.direction });
            }
            if (crossings.size() < 2)
                continue;
            std::sort(crossings.begin(), crossings.end(),
                [](Crossing const& a, Crossing const& b) { return a.x < b.x; });
            int winding = 0;
            for (std::size_t i = 0; i + 1 < crossings.size(); ++i) {
                winding += crossings[i].direction;
                if (winding == 0)
                    continue;
                std::int64_t const span_begin = crossings[i].x;
                std::int64_t const span_end = crossings[i + 1].x; // exclusive
                if (span_end <= span_begin)
                    continue;
                // Sample centers sit at 64 px + 8 + 16 k; count those inside.
                std::int64_t const first = std::max(left, div_floor(span_begin - 56, 64));
                std::int64_t const last = std::min(right - 1, div_floor(span_end - 9, 64));
                for (std::int64_t px = first; px <= last; ++px) {
                    std::uint8_t count = 0;
                    for (int k = 0; k < 4; ++k) {
                        std::int64_t const cx = px * 64 + 8 + 16 * k;
                        if (cx >= span_begin && cx < span_end)
                            ++count;
                    }
                    hits[static_cast<std::size_t>(row * width + (px - left))]
                        = static_cast<std::uint8_t>(hits[static_cast<std::size_t>(row * width + (px - left))] + count);
                }
            }
        }
    }

    mask.alpha.resize(hits.size());
    for (std::size_t i = 0; i < hits.size(); ++i)
        mask.alpha[i] = static_cast<std::uint8_t>(std::min<int>(hits[i], 16) * 255 / 16);
    if (options.embolden) {
        for (std::int64_t row = 0; row < height; ++row) {
            std::uint8_t* line = mask.alpha.data() + row * width;
            for (std::int64_t px = width - 1; px > 0; --px)
                line[px] = std::max(line[px], line[px - 1]);
        }
    }
    return mask;
}

}
