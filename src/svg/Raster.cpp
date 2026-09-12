#include "svg/Raster.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sashfold::svg {

namespace {

constexpr float pi = 3.14159265358979323846f;

float length_of(Point a, Point b)
{
    return std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
}

} // namespace

// --- Matrix ---------------------------------------------------------------------

Matrix Matrix::translate(float x, float y)
{
    return Matrix { 1, 0, 0, 1, x, y };
}

Matrix Matrix::scale(float x, float y)
{
    return Matrix { x, 0, 0, y, 0, 0 };
}

Matrix Matrix::rotate(float degrees)
{
    float const radians = degrees * pi / 180.0f;
    float const cs = std::cos(radians);
    float const sn = std::sin(radians);
    return Matrix { cs, sn, -sn, cs, 0, 0 };
}

Matrix Matrix::skew_x(float degrees)
{
    return Matrix { 1, 0, std::tan(degrees * pi / 180.0f), 1, 0, 0 };
}

Matrix Matrix::skew_y(float degrees)
{
    return Matrix { 1, std::tan(degrees * pi / 180.0f), 0, 1, 0, 0 };
}

Matrix Matrix::then(Matrix const& n) const
{
    // n * this, in column-vector convention.
    return Matrix {
        n.a * a + n.c * b,
        n.b * a + n.d * b,
        n.a * c + n.c * d,
        n.b * c + n.d * d,
        n.a * e + n.c * f + n.e,
        n.b * e + n.d * f + n.f,
    };
}

Point Matrix::apply(Point p) const
{
    return Point { a * p.x + c * p.y + e, b * p.x + d * p.y + f };
}

std::optional<Matrix> Matrix::inverse() const
{
    float const det = a * d - b * c;
    if (!std::isfinite(det) || std::abs(det) < 1e-12f)
        return std::nullopt;
    Matrix m;
    m.a = d / det;
    m.b = -b / det;
    m.c = -c / det;
    m.d = a / det;
    m.e = (c * f - d * e) / det;
    m.f = (b * e - a * f) / det;
    return m;
}

float Matrix::scale_factor() const
{
    float const det = std::abs(a * d - b * c);
    return std::sqrt(det);
}

// --- Path -----------------------------------------------------------------------

namespace {

// Where the subpath holding segment `index` began: the last move before it.
Point subpath_start_before(Path const& path, std::size_t index)
{
    for (std::size_t i = std::min(index, path.segments.size()); i > 0; --i) {
        if (path.segments[i - 1].verb == Path::Verb::Move)
            return path.segments[i - 1].p1;
    }
    return {};
}

} // namespace

Point Path::current() const
{
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        switch (it->verb) {
        case Verb::Move:
        case Verb::Line:
            return it->p1;
        case Verb::Cubic:
            return it->p3;
        case Verb::Close:
            // The pen goes back to where the subpath began.
            return subpath_start_before(*this, static_cast<std::size_t>(segments.rend() - it - 1));
        }
    }
    return {};
}

Point Path::subpath_start() const
{
    return subpath_start_before(*this, segments.size());
}

void Path::move_to(Point point)
{
    segments.push_back(Segment { Verb::Move, point, {}, {} });
}

void Path::line_to(Point point)
{
    if (segments.empty())
        move_to(point);
    segments.push_back(Segment { Verb::Line, point, {}, {} });
}

void Path::cubic_to(Point control1, Point control2, Point point)
{
    if (segments.empty())
        move_to(control1);
    segments.push_back(Segment { Verb::Cubic, control1, control2, point });
}

void Path::quadratic_to(Point control, Point point)
{
    Point const from = current();
    Point const c1 { from.x + 2.0f / 3.0f * (control.x - from.x), from.y + 2.0f / 3.0f * (control.y - from.y) };
    Point const c2 { point.x + 2.0f / 3.0f * (control.x - point.x), point.y + 2.0f / 3.0f * (control.y - point.y) };
    cubic_to(c1, c2, point);
}

// SVG implementation notes F.6.5: from the endpoints, the radii and the
// flags to the ellipse's centre and the sweep, then the sweep cut into
// pieces of at most a quarter turn, each drawn as one cubic.
void Path::arc_to(float rx, float ry, float rotation_degrees, bool large_arc, bool sweep, Point point)
{
    Point const from = current();
    if (from.x == point.x && from.y == point.y)
        return;
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx == 0 || ry == 0) {
        line_to(point);
        return;
    }
    float const phi = rotation_degrees * pi / 180.0f;
    float const cos_phi = std::cos(phi);
    float const sin_phi = std::sin(phi);
    float const dx2 = (from.x - point.x) / 2;
    float const dy2 = (from.y - point.y) / 2;
    float const x1p = cos_phi * dx2 + sin_phi * dy2;
    float const y1p = -sin_phi * dx2 + cos_phi * dy2;
    // Radii too small for the chord are scaled up until they fit (F.6.6).
    float const lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1) {
        float const s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
    }
    float const rx2 = rx * rx;
    float const ry2 = ry * ry;
    float numerator = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    float const denominator = rx2 * y1p * y1p + ry2 * x1p * x1p;
    if (numerator < 0)
        numerator = 0;
    float coefficient = denominator > 0 ? std::sqrt(numerator / denominator) : 0;
    if (large_arc == sweep)
        coefficient = -coefficient;
    float const cxp = coefficient * (rx * y1p / ry);
    float const cyp = coefficient * -(ry * x1p / rx);
    float const cx = cos_phi * cxp - sin_phi * cyp + (from.x + point.x) / 2;
    float const cy = sin_phi * cxp + cos_phi * cyp + (from.y + point.y) / 2;
    auto const angle = [](float ux, float uy, float vx, float vy) {
        float const dot = ux * vx + uy * vy;
        float const len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
        float const cosine = len > 0 ? std::clamp(dot / len, -1.0f, 1.0f) : 1.0f;
        float a = std::acos(cosine);
        if (ux * vy - uy * vx < 0)
            a = -a;
        return a;
    };
    float const theta1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    float delta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!sweep && delta > 0)
        delta -= 2 * pi;
    else if (sweep && delta < 0)
        delta += 2 * pi;
    int const pieces = std::max(1, static_cast<int>(std::ceil(std::abs(delta) / (pi / 2) - 1e-5f)));
    float const step = delta / static_cast<float>(pieces);
    float const k = 4.0f / 3.0f * std::tan(step / 4);
    float theta = theta1;
    for (int i = 0; i < pieces; ++i) {
        float const cos1 = std::cos(theta);
        float const sin1 = std::sin(theta);
        float const cos2 = std::cos(theta + step);
        float const sin2 = std::sin(theta + step);
        // In the ellipse's own frame, then rotated and moved to its centre.
        auto const to_page = [&](float ex, float ey) {
            return Point { cos_phi * ex - sin_phi * ey + cx, sin_phi * ex + cos_phi * ey + cy };
        };
        Point const c1 = to_page(rx * (cos1 - k * sin1), ry * (sin1 + k * cos1));
        Point const c2 = to_page(rx * (cos2 + k * sin2), ry * (sin2 - k * cos2));
        Point const end = i + 1 == pieces ? point : to_page(rx * cos2, ry * sin2);
        cubic_to(c1, c2, end);
        theta += step;
    }
}

void Path::close()
{
    if (!segments.empty() && segments.back().verb != Verb::Close)
        segments.push_back(Segment { Verb::Close, {}, {}, {} });
}

Path Path::transformed(Matrix const& m) const
{
    Path out;
    out.segments.reserve(segments.size());
    for (Segment const& s : segments)
        out.segments.push_back(Segment { s.verb, m.apply(s.p1), m.apply(s.p2), m.apply(s.p3) });
    return out;
}

// --- Flattening -----------------------------------------------------------------

namespace {

void flatten_cubic(Point p0, Point p1, Point p2, Point p3, float tolerance, Polygon& out)
{
    // The number of chords that keeps the error under the tolerance, from
    // the curve's second differences (Wang's formula). A curve whose
    // control points are not numbers — an overflowed transform — is one
    // chord: the rasterizer drops what is not a number.
    float const ddx = std::max(std::abs(p0.x - 2 * p1.x + p2.x), std::abs(p1.x - 2 * p2.x + p3.x));
    float const ddy = std::max(std::abs(p0.y - 2 * p1.y + p2.y), std::abs(p1.y - 2 * p2.y + p3.y));
    float const dd = std::sqrt(ddx * ddx + ddy * ddy);
    float const wanted = std::ceil(std::sqrt(0.75f * dd / std::max(tolerance, 0.001f)));
    int const n = !(wanted >= 1) ? 1 : wanted >= 256 ? 256 : static_cast<int>(wanted);
    for (int i = 1; i <= n; ++i) {
        float const t = static_cast<float>(i) / static_cast<float>(n);
        float const u = 1 - t;
        float const w0 = u * u * u;
        float const w1 = 3 * u * u * t;
        float const w2 = 3 * u * t * t;
        float const w3 = t * t * t;
        out.push_back(Point { w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
            w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y });
    }
}

} // namespace

std::vector<Polyline> flatten(Path const& path, float tolerance)
{
    std::vector<Polyline> lines;
    Polyline current;
    Point pen;
    Point start;
    auto const finish = [&](bool closed) {
        if (!current.points.empty()) {
            current.closed = closed;
            lines.push_back(std::move(current));
        }
        current = Polyline {};
    };
    for (Path::Segment const& s : path.segments) {
        switch (s.verb) {
        case Path::Verb::Move:
            finish(false);
            pen = start = s.p1;
            current.points.push_back(pen);
            break;
        case Path::Verb::Line:
            if (current.points.empty())
                current.points.push_back(pen);
            current.points.push_back(s.p1);
            pen = s.p1;
            break;
        case Path::Verb::Cubic:
            if (current.points.empty())
                current.points.push_back(pen);
            flatten_cubic(pen, s.p1, s.p2, s.p3, tolerance, current.points);
            pen = s.p3;
            break;
        case Path::Verb::Close:
            finish(true);
            pen = start;
            current.points.push_back(pen);
            break;
        }
    }
    finish(false);
    // A subpath of one point draws nothing when filled; a stroke with round
    // or square caps draws a dot there, which the stroker handles from the
    // single point.
    return lines;
}

// --- Stroking -------------------------------------------------------------------

namespace {

float signed_area(Polygon const& polygon)
{
    float area = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        Point const& a = polygon[i];
        Point const& b = polygon[(i + 1) % polygon.size()];
        area += a.x * b.y - b.x * a.y;
    }
    return area / 2;
}

// Every polygon wound the same way (positive area, y down), so the
// nonzero rule adds where they overlap.
void add_polygon(std::vector<Polygon>& out, Polygon polygon)
{
    if (polygon.size() < 3)
        return;
    if (signed_area(polygon) < 0)
        std::reverse(polygon.begin(), polygon.end());
    out.push_back(std::move(polygon));
}

void add_circle(std::vector<Polygon>& out, Point centre, float radius)
{
    if (!std::isfinite(radius) || !std::isfinite(centre.x) || !std::isfinite(centre.y))
        return;
    int const n = radius >= 28 ? 64 : 8 + static_cast<int>(radius * 2);
    Polygon circle;
    circle.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        float const t = 2 * pi * static_cast<float>(i) / static_cast<float>(n);
        circle.push_back(Point { centre.x + radius * std::cos(t), centre.y + radius * std::sin(t) });
    }
    add_polygon(out, std::move(circle));
}

// Cuts the polylines into dashes along their length; an offset moves the
// pattern's start. Dashes and gaps alternate; an odd list repeats doubled.
std::vector<Polyline> apply_dashes(std::vector<Polyline> const& lines, std::vector<float> const& dashes,
    float offset)
{
    std::vector<float> pattern;
    float total = 0;
    for (float const dash : dashes) {
        if (!(dash >= 0) || !std::isfinite(dash))
            return lines; // an invalid list is rendered solid
        pattern.push_back(dash);
        total += dash;
    }
    if (pattern.empty() || total <= 0)
        return lines;
    if (pattern.size() % 2 == 1) {
        std::size_t const n = pattern.size();
        for (std::size_t i = 0; i < n; ++i)
            pattern.push_back(pattern[i]);
        total *= 2;
    }
    std::vector<Polyline> out;
    for (Polyline const& line : lines) {
        std::vector<Point> points = line.points;
        if (line.closed && !points.empty())
            points.push_back(points.front());
        if (points.size() < 2)
            continue;
        // Where in the pattern the line starts.
        float phase = std::fmod(offset, total);
        if (phase < 0)
            phase += total;
        std::size_t index = 0;
        while (phase >= pattern[index]) {
            phase -= pattern[index];
            index = (index + 1) % pattern.size();
        }
        float remaining = pattern[index] - phase; // of the current dash or gap
        bool on = index % 2 == 0;
        Polyline piece;
        if (on)
            piece.points.push_back(points.front());
        std::size_t emitted = 0;
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            Point a = points[i];
            Point const b = points[i + 1];
            float segment = length_of(a, b);
            while (segment > 0) {
                if (remaining >= segment) {
                    remaining -= segment;
                    if (on)
                        piece.points.push_back(b);
                    segment = 0;
                    break;
                }
                float const t = remaining / segment;
                Point const cut { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
                if (on) {
                    piece.points.push_back(cut);
                    out.push_back(std::move(piece));
                    piece = Polyline {};
                    ++emitted;
                } else {
                    piece.points.push_back(cut);
                }
                segment -= remaining;
                a = cut;
                index = (index + 1) % pattern.size();
                remaining = pattern[index];
                on = !on;
                if (!on)
                    piece = Polyline {};
                if (emitted > 100000)
                    return out; // a pattern of hairs along a long path: enough
            }
        }
        if (on && piece.points.size() >= 2)
            out.push_back(std::move(piece));
    }
    return out;
}

} // namespace

std::vector<Polygon> stroke(std::vector<Polyline> const& input, StrokeStyle const& style)
{
    std::vector<Polygon> out;
    if (!(style.width > 0) || !std::isfinite(style.width))
        return out;
    float const half = style.width / 2;
    std::vector<Polyline> dashed;
    if (!style.dashes.empty())
        dashed = apply_dashes(input, style.dashes, style.dash_offset);
    std::vector<Polyline> const& lines = style.dashes.empty() ? input : dashed;
    for (Polyline const& line : lines) {
        // Drop repeated points: a zero-length segment has no direction.
        std::vector<Point> points;
        for (Point const& p : line.points) {
            if (points.empty() || length_of(points.back(), p) > 1e-6f)
                points.push_back(p);
        }
        bool closed = line.closed;
        if (closed && points.size() > 1 && length_of(points.front(), points.back()) <= 1e-6f)
            points.pop_back();
        if (points.size() == 1) {
            // A dot: round caps draw a circle, square caps a square, butt nothing.
            if (style.cap == LineCap::Round)
                add_circle(out, points[0], half);
            else if (style.cap == LineCap::Square)
                add_polygon(out,
                    Polygon { { points[0].x - half, points[0].y - half }, { points[0].x + half, points[0].y - half },
                        { points[0].x + half, points[0].y + half }, { points[0].x - half, points[0].y + half } });
            continue;
        }
        if (points.size() < 2)
            continue;
        if (closed && points.size() < 3)
            closed = false;
        std::size_t const n = points.size();
        std::size_t const segment_count = closed ? n : n - 1;
        // The unit direction of each segment and its normal.
        std::vector<Point> directions(segment_count);
        for (std::size_t i = 0; i < segment_count; ++i) {
            Point const a = points[i];
            Point const b = points[(i + 1) % n];
            float const len = length_of(a, b);
            directions[i] = Point { (b.x - a.x) / len, (b.y - a.y) / len };
        }
        auto const normal = [](Point d) { return Point { -d.y, d.x }; };
        // The body of each segment.
        for (std::size_t i = 0; i < segment_count; ++i) {
            Point const a = points[i];
            Point const b = points[(i + 1) % n];
            Point const nrm = normal(directions[i]);
            float ext_a = 0;
            float ext_b = 0;
            if (!closed && style.cap == LineCap::Square) {
                if (i == 0)
                    ext_a = half;
                if (i + 1 == segment_count)
                    ext_b = half;
            }
            Point const d = directions[i];
            Point const a2 { a.x - d.x * ext_a, a.y - d.y * ext_a };
            Point const b2 { b.x + d.x * ext_b, b.y + d.y * ext_b };
            add_polygon(out,
                Polygon { { a2.x + nrm.x * half, a2.y + nrm.y * half }, { b2.x + nrm.x * half, b2.y + nrm.y * half },
                    { b2.x - nrm.x * half, b2.y - nrm.y * half }, { a2.x - nrm.x * half, a2.y - nrm.y * half } });
        }
        // The joins: at every interior vertex, and at every vertex of a
        // closed line.
        std::size_t const first_join = closed ? 0 : 1;
        std::size_t const last_join = closed ? n : n - 1; // exclusive
        for (std::size_t v = first_join; v < last_join; ++v) {
            Point const p = points[v];
            Point const din = directions[(v + segment_count - 1) % segment_count];
            Point const dout = directions[v % segment_count];
            float const cross = din.x * dout.y - din.y * dout.x;
            float const dot = din.x * dout.x + din.y * dout.y;
            if (std::abs(cross) < 1e-6f && dot > 0)
                continue; // straight on: nothing to fill
            if (style.join == LineJoin::Round) {
                add_circle(out, p, half);
                continue;
            }
            // The outer side is the one the turn leaves open: turning left
            // (cross < 0 with y down) opens the right side.
            float const side = cross > 0 ? -1.0f : 1.0f;
            Point const nin = normal(din);
            Point const nout = normal(dout);
            Point const outer_in { p.x + nin.x * half * side, p.y + nin.y * half * side };
            Point const outer_out { p.x + nout.x * half * side, p.y + nout.y * half * side };
            // The bevel triangle is always there; a miter adds its tip.
            add_polygon(out, Polygon { p, outer_in, outer_out });
            if (style.join == LineJoin::Miter) {
                // The angle between the segments decides the miter's length:
                // 1 / sin(theta / 2) half-widths, held to the limit.
                float const cos_theta = std::clamp(-dot, -1.0f, 1.0f);
                float const theta = std::acos(cos_theta); // the interior angle
                float const half_theta = theta / 2;
                float const sin_half = std::sin(half_theta);
                if (sin_half > 1e-6f && 1.0f / sin_half <= style.miter_limit) {
                    // The tip lies along the bisector of the two normals.
                    Point bis { nin.x * side + nout.x * side, nin.y * side + nout.y * side };
                    float const bl = std::sqrt(bis.x * bis.x + bis.y * bis.y);
                    if (bl > 1e-6f) {
                        float const tip_len = half / sin_half;
                        Point const tip { p.x + bis.x / bl * tip_len, p.y + bis.y / bl * tip_len };
                        add_polygon(out, Polygon { outer_in, tip, outer_out });
                    }
                }
            }
        }
        // Round caps on an open line.
        if (!closed && style.cap == LineCap::Round) {
            add_circle(out, points.front(), half);
            add_circle(out, points.back(), half);
        }
    }
    return out;
}

// --- Scan conversion ------------------------------------------------------------

std::uint8_t Mask::at(int x, int y) const
{
    if (x < left || y < top || x >= left + width || y >= top + height)
        return 0;
    return alpha[static_cast<std::size_t>(y - top) * static_cast<std::size_t>(width)
        + static_cast<std::size_t>(x - left)];
}

std::optional<Box> bounds_of(std::vector<Polygon> const& polygons)
{
    std::optional<Box> box;
    for (Polygon const& polygon : polygons) {
        for (Point const& p : polygon) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y))
                continue;
            if (!box) {
                box = Box { p.x, p.y, p.x, p.y };
                continue;
            }
            box->left = std::min(box->left, p.x);
            box->top = std::min(box->top, p.y);
            box->right = std::max(box->right, p.x);
            box->bottom = std::max(box->bottom, p.y);
        }
    }
    return box;
}

namespace {

struct Edge {
    std::int64_t x0, y0, x1, y1; // 1/64 px
    int direction;
};

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

std::int64_t to_fixed(float v)
{
    // Coordinates far outside any bitmap are held to a range the
    // arithmetic below cannot overflow in; one that is not a number was
    // refused before this.
    if (!std::isfinite(v))
        return 0;
    float const held = std::clamp(v, -1.0e6f, 1.0e6f);
    return static_cast<std::int64_t>(std::lround(held * 64.0f));
}

} // namespace

Mask rasterize(std::vector<Polygon> const& polygons, FillRule rule, int clip_left, int clip_top,
    int clip_right, int clip_bottom)
{
    Mask mask;
    std::vector<Edge> edges;
    for (Polygon const& polygon : polygons) {
        if (polygon.size() < 3)
            continue;
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            Point const a = polygon[i];
            Point const b = polygon[(i + 1) % polygon.size()];
            if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y))
                return mask;
            std::int64_t const ax = to_fixed(a.x);
            std::int64_t const ay = to_fixed(a.y);
            std::int64_t const bx = to_fixed(b.x);
            std::int64_t const by = to_fixed(b.y);
            if (ay == by)
                continue;
            edges.push_back(Edge { ax, ay, bx, by, by > ay ? 1 : -1 });
        }
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
    std::int64_t const left = std::max<std::int64_t>(div_floor(min_x, 64), clip_left);
    std::int64_t const right = std::min<std::int64_t>(div_ceil(max_x, 64), clip_right);
    std::int64_t const top = std::max<std::int64_t>(div_floor(min_y, 64), clip_top);
    std::int64_t const bottom = std::min<std::int64_t>(div_ceil(max_y, 64), clip_bottom);
    std::int64_t const width = right - left;
    std::int64_t const height = bottom - top;
    if (width <= 0 || height <= 0 || width * height > 64'000'000)
        return mask;
    mask.left = static_cast<int>(left);
    mask.top = static_cast<int>(top);
    mask.width = static_cast<int>(width);
    mask.height = static_cast<int>(height);
    std::vector<std::uint8_t> hits(static_cast<std::size_t>(width * height), 0);

    struct Crossing {
        std::int64_t x;
        int direction;
    };
    std::vector<Crossing> crossings;
    // Only the edges that reach a row are looked at on it: sorted by
    // their top, with a moving window.
    std::sort(edges.begin(), edges.end(), [](Edge const& a, Edge const& b) {
        return std::min(a.y0, a.y1) < std::min(b.y0, b.y1);
    });
    std::size_t next_edge = 0;
    std::vector<Edge const*> active;
    for (std::int64_t row = 0; row < height; ++row) {
        for (int sub = 0; sub < 4; ++sub) {
            std::int64_t const ys = (top + row) * 64 + 8 + 16 * sub;
            while (next_edge < edges.size() && std::min(edges[next_edge].y0, edges[next_edge].y1) <= ys) {
                active.push_back(&edges[next_edge]);
                ++next_edge;
            }
            crossings.clear();
            for (std::size_t i = 0; i < active.size();) {
                Edge const& edge = *active[i];
                if (ys >= std::max(edge.y0, edge.y1)) {
                    active[i] = active.back();
                    active.pop_back();
                    continue;
                }
                if (ys >= std::min(edge.y0, edge.y1)) {
                    std::int64_t numerator = (edge.x1 - edge.x0) * (ys - edge.y0);
                    std::int64_t denominator = edge.y1 - edge.y0;
                    if (denominator < 0) {
                        numerator = -numerator;
                        denominator = -denominator;
                    }
                    crossings.push_back(Crossing { edge.x0 + div_ceil(numerator, denominator), edge.direction });
                }
                ++i;
            }
            if (crossings.size() < 2)
                continue;
            std::sort(crossings.begin(), crossings.end(),
                [](Crossing const& a, Crossing const& b) { return a.x < b.x; });
            int winding = 0;
            for (std::size_t i = 0; i + 1 < crossings.size(); ++i) {
                winding += rule == FillRule::NonZero ? crossings[i].direction : 1;
                bool const inside = rule == FillRule::NonZero ? winding != 0 : (winding % 2) != 0;
                if (!inside)
                    continue;
                std::int64_t const span_begin = crossings[i].x;
                std::int64_t const span_end = crossings[i + 1].x;
                if (span_end <= span_begin)
                    continue;
                std::int64_t const first = std::max(left, div_floor(span_begin - 56, 64));
                std::int64_t const last = std::min(right - 1, div_floor(span_end - 9, 64));
                for (std::int64_t px = first; px <= last; ++px) {
                    std::uint8_t count = 0;
                    for (int k = 0; k < 4; ++k) {
                        std::int64_t const cx = px * 64 + 8 + 16 * k;
                        if (cx >= span_begin && cx < span_end)
                            ++count;
                    }
                    std::uint8_t& cell = hits[static_cast<std::size_t>(row * width + (px - left))];
                    cell = static_cast<std::uint8_t>(std::min(16, cell + count));
                }
            }
        }
    }
    mask.alpha.resize(hits.size());
    for (std::size_t i = 0; i < hits.size(); ++i)
        mask.alpha[i] = static_cast<std::uint8_t>(hits[i] * 255 / 16);
    return mask;
}

}
