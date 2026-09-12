#pragma once

// Vector geometry for the SVG subset: paths of lines and cubic curves
// under an affine transform, flattened to polygons and scan-converted into
// a coverage mask by the nonzero or the even-odd rule, and strokes turned
// into the polygons of their outline. The scan conversion samples each
// pixel at 4x4 points, as the glyph rasterizer does, so a shape has the
// same edge as a letter on every machine.

#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold::svg {

struct Point {
    float x = 0;
    float y = 0;
};

// An affine transform: x' = a x + c y + e, y' = b x + d y + f — the SVG
// matrix(a b c d e f).
struct Matrix {
    float a = 1;
    float b = 0;
    float c = 0;
    float d = 1;
    float e = 0;
    float f = 0;

    static Matrix translate(float x, float y);
    static Matrix scale(float x, float y);
    static Matrix rotate(float degrees);
    static Matrix skew_x(float degrees);
    static Matrix skew_y(float degrees);

    // This transform followed by `next`: a point goes through this one
    // first. A child's transform is `child.then(parent_ctm)`.
    Matrix then(Matrix const& next) const;
    Point apply(Point point) const;
    std::optional<Matrix> inverse() const;
    // How much the transform scales a length, taking the geometric mean
    // of its two axes: what a stroke width or a font size becomes.
    float scale_factor() const;
};

struct Path {
    enum class Verb : std::uint8_t { Move, Line, Cubic, Close };
    struct Segment {
        Verb verb;
        Point p1; // Line: the end; Cubic: the first control point
        Point p2; // Cubic: the second control point
        Point p3; // Cubic: the end
    };
    std::vector<Segment> segments;

    bool empty() const { return segments.empty(); }
    Point current() const; // where the pen is
    Point subpath_start() const;
    void move_to(Point point);
    void line_to(Point point);
    void cubic_to(Point control1, Point control2, Point point);
    void quadratic_to(Point control, Point point); // raised to a cubic
    // An elliptical arc as SVG writes it (endpoint parameterization),
    // made of cubic curves each no more than a quarter turn.
    void arc_to(float rx, float ry, float rotation_degrees, bool large_arc, bool sweep, Point point);
    void close();

    Path transformed(Matrix const& matrix) const;
};

using Polygon = std::vector<Point>;

// One subpath flattened: its points in order, and whether it was closed.
struct Polyline {
    Polygon points;
    bool closed = false;
};

// Chords within `tolerance` px of the curve.
std::vector<Polyline> flatten(Path const& path, float tolerance = 0.1f);

enum class FillRule : std::uint8_t { NonZero, EvenOdd };
enum class LineCap : std::uint8_t { Butt, Round, Square };
enum class LineJoin : std::uint8_t { Miter, Round, Bevel };

struct StrokeStyle {
    float width = 1;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    float miter_limit = 4;
    std::vector<float> dashes; // empty: solid
    float dash_offset = 0;
};

// The outline of a stroke, as polygons to fill together by the nonzero
// rule: every polygon is wound the same way, so where two overlap the
// winding adds rather than cancels.
std::vector<Polygon> stroke(std::vector<Polyline> const& lines, StrokeStyle const& style);

// A coverage mask over whole pixels: `alpha` is width * height bytes,
// row-major, from (left, top).
struct Mask {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha;

    bool empty() const { return alpha.empty(); }
    std::uint8_t at(int x, int y) const;
};

// Scan-converts polygons in device pixels, keeping to the pixels inside
// [clip_left, clip_right) x [clip_top, clip_bottom).
Mask rasterize(std::vector<Polygon> const& polygons, FillRule rule, int clip_left, int clip_top,
    int clip_right, int clip_bottom);

// The bounding box of a set of polygons; nullopt when there is nothing.
struct Box {
    float left = 0;
    float top = 0;
    float right = 0;
    float bottom = 0;
};
std::optional<Box> bounds_of(std::vector<Polygon> const& polygons);

}
