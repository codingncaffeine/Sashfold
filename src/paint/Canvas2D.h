#pragma once

// The drawing model of the canvas 2D context (HTML section4.12.5.1), apart from
// any script: a premultiplied bitmap, and the operations that paint into it
// through a coverage mask, a fill or stroke style, global alpha, a shadow, a
// composition operator and a clip. Paths are the SVG renderer's (svg::Path),
// flattened, stroked and scan-converted by the same code, so a canvas shape
// has the same edge as an SVG one on every machine.
//
// Coordinates: a path handed to these functions is already in the bitmap's
// pixels (the context transforms each point as it is added, as the
// specification has it); the transform in a DrawState is the one current at
// the time of drawing, which a gradient, a pattern and a stroke's pen are
// measured in.

#include "core/Bitmap.h"
#include "svg/Raster.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::text {
class FontStack;
}

namespace sashfold::canvas {

// The sine and cosine of an angle in radians, the same to the bit on every
// system (no C library, whose results differ between them): what rotate(),
// arc(), ellipse() and DOMMatrix's turns and skews take, so a canvas golden
// is one picture everywhere. Within an ulp of the true values for every
// finite angle.
struct SineCosine {
    double sine = 0;
    double cosine = 1;
};
SineCosine sine_cosine(double radians);

// An affine transform in doubles, as the context keeps its current one:
// x' = a x + c y + e, y' = b x + d y + f.
struct Transform {
    double a = 1;
    double b = 0;
    double c = 0;
    double d = 1;
    double e = 0;
    double f = 0;

    // `inner` applied first, then this one: what transform(...) does to the
    // current transform.
    Transform multiply(Transform const& inner) const;
    svg::Point apply(double x, double y) const;
    std::optional<Transform> inverse() const;
    bool is_finite() const;
    svg::Matrix to_matrix() const;
    // How much the transform scales a length: the geometric mean of its axes.
    double scale_factor() const;
};

// A bitmap with its colors premultiplied by their alpha, eight bits each:
// the canvas's output bitmap, and every picture drawn into one.
struct Surface {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // RGBA, premultiplied, row-major

    Surface() = default;
    Surface(int w, int h); // transparent black
    static Surface from_bitmap(Bitmap const& bitmap);
    // Straight alpha again, for the page's painter and the encoders.
    Bitmap to_bitmap() const;
    std::size_t offset(int x, int y) const
    {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4u;
    }
    // A copy of a rectangle of this one, transparent black where it reaches
    // past the edges.
    Surface crop(int x, int y, int w, int h) const;
};

// The composition operators (Compositing and Blending section9 and section10), by the
// names globalCompositeOperation takes.
enum class Composite : std::uint8_t {
    SourceOver,
    SourceIn,
    SourceOut,
    SourceAtop,
    DestinationOver,
    DestinationIn,
    DestinationOut,
    DestinationAtop,
    Lighter,
    Copy,
    Xor,
    Clear,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    ColorDodge,
    ColorBurn,
    HardLight,
    SoftLight,
    Difference,
    Exclusion,
    Hue,
    Saturation,
    Color,
    Luminosity,
};
std::optional<Composite> composite_by_name(std::string_view name);
std::string_view composite_name(Composite);

struct ColorStop {
    double offset = 0;
    Color color;
};

// A CanvasGradient: linear from (x0, y0) to (x1, y1), radial between two
// circles, or conic about (x0, y0) from `angle`.
struct Gradient {
    enum class Kind : std::uint8_t { Linear, Radial, Conic };
    Kind kind = Kind::Linear;
    double x0 = 0;
    double y0 = 0;
    double r0 = 0;
    double x1 = 0;
    double y1 = 0;
    double r1 = 0;
    double angle = 0;
    std::vector<ColorStop> stops; // in offset order, those of one offset in the order added
    void add_stop(double offset, Color color);
};

// A CanvasPattern: a picture repeated along either axis, placed by its own
// transform inside the space of the transform current when it is drawn.
struct Pattern {
    std::shared_ptr<Surface const> image;
    bool repeat_x = true;
    bool repeat_y = true;
    Transform transform;
};

// A fill or stroke style: a color, a gradient or a pattern.
struct Style {
    Color color { 0, 0, 0, 255 };
    std::shared_ptr<Gradient const> gradient;
    std::shared_ptr<Pattern const> pattern;
};

// A clip region: how much of each pixel of the bitmap is inside it.
using ClipMask = std::vector<std::uint8_t>;

// What every drawing operation reads of the context's state.
struct DrawState {
    Transform transform;
    double global_alpha = 1;
    Composite composite = Composite::SourceOver;
    bool smoothing = true;
    double shadow_offset_x = 0;
    double shadow_offset_y = 0;
    double shadow_blur = 0;
    Color shadow_color { 0, 0, 0, 0 };
    std::shared_ptr<ClipMask const> clip; // null: nothing clipped
};

// Paints a coverage mask (device pixels) with a style: the shadow first,
// then the shape, each composited over the whole clip region.
void paint_mask(Surface& surface, svg::Mask const& coverage, Style const& style, DrawState const& state);

// The coverage of a filled path, and of a stroked one: the pen measured in
// the drawing transform's space. Given the state it is drawn with, the
// coverage reaches past the bitmap as far as a shadow cast from there could
// fall back onto it.
svg::Mask fill_coverage(Surface const& surface, svg::Path const& device_path, svg::FillRule rule,
    DrawState const* state = nullptr);
svg::Mask stroke_coverage(Surface const& surface, svg::Path const& device_path, svg::StrokeStyle const& pen,
    DrawState const& state);

// A picture drawn into the rectangle (dx, dy, dw, dh) of the current space
// from the rectangle (sx, sy, sw, sh) of the source, which the caller has
// already clipped to the source's bounds.
void draw_image(Surface& surface, Surface const& source, double sx, double sy, double sw, double sh, double dx,
    double dy, double dw, double dh, DrawState const& state);

// clearRect: transparent black over the rectangle, as far as the clip goes.
void clear_rect(Surface& surface, double x, double y, double w, double h, DrawState const& state);

// The clip region narrowed to a path.
std::shared_ptr<ClipMask const> intersect_clip(Surface const& surface, std::shared_ptr<ClipMask const> const& clip,
    svg::Path const& device_path, svg::FillRule rule);

// isPointInPath and isPointInStroke, a point in device pixels.
bool point_in_path(svg::Path const& device_path, svg::FillRule rule, double x, double y);
bool point_in_stroke(svg::Path const& device_path, svg::StrokeStyle const& pen, Transform const& transform, double x,
    double y);

// Building a path as CanvasPath's methods do: each point through a
// transform as it is added (the identity for a Path2D).
struct PathBuilder {
    svg::Path path;
    // Whether the path has a subpath the next segment continues.
    bool has_subpath() const { return !path.segments.empty(); }
    void move_to(Transform const& t, double x, double y);
    void line_to(Transform const& t, double x, double y);
    void quadratic_to(Transform const& t, double cx, double cy, double x, double y);
    void bezier_to(Transform const& t, double c1x, double c1y, double c2x, double c2y, double x, double y);
    // arcTo; the radius is not negative (the caller throws for that).
    void arc_to(Transform const& t, double x1, double y1, double x2, double y2, double radius);
    // arc and ellipse, the sweep as the specification works it out from
    // the two angles and the direction.
    void ellipse(Transform const& t, double x, double y, double rx, double ry, double rotation, double start,
        double end, bool anticlockwise);
    void rect(Transform const& t, double x, double y, double w, double h);
    // roundRect with its four corners' radii already worked out, as
    // (horizontal, vertical) pairs: upper left, upper right, lower right,
    // lower left.
    void round_rect(Transform const& t, double x, double y, double w, double h, std::array<svg::Point, 4> radii);
    void close();
    // Another path's segments through a transform (Path2D.addPath).
    void add_path(svg::Path const& other, Transform const& t);
};

// The font the context draws text in, as its font attribute parsed.
struct Font {
    float size = 10;
    int weight = 400;
    bool italic = false;
    int stretch = 100;
    bool small_caps = false;
    std::vector<std::string> families { "sans-serif" };
};

// The text drawing styles that place a run.
struct TextLayout {
    enum class Align : std::uint8_t { Start, End, Left, Right, Center };
    enum class Baseline : std::uint8_t { Top, Hanging, Middle, Alphabetic, Ideographic, Bottom };
    Align align = Align::Start;
    Baseline baseline = Baseline::Alphabetic;
    bool rtl = false;
    float letter_spacing = 0;
    float word_spacing = 0;
    bool kerning = true;
};

// A run of text measured: the advance and the TextMetrics members, relative
// to the point the run is drawn from as the alignment and baseline place it.
struct TextMeasure {
    double width = 0;
    double actual_left = 0;
    double actual_right = 0;
    double actual_ascent = 0;
    double actual_descent = 0;
    double font_ascent = 0;
    double font_descent = 0;
    double em_ascent = 0;
    double em_descent = 0;
    double hanging_baseline = 0;
    double alphabetic_baseline = 0;
    double ideographic_baseline = 0;
};

TextMeasure measure_text(std::u32string_view text, Font const& font, TextLayout const& layout);

// The coverage of a run of text drawn at (x, y) of the current space, the
// run squeezed to `max_width` when it is wider; stroked instead of filled
// with a pen of `stroke_width` when that is given.
svg::Mask text_coverage(Surface const& surface, std::u32string_view text, Font const& font, TextLayout const& layout,
    double x, double y, std::optional<double> max_width, Transform const& transform,
    std::optional<double> stroke_width = std::nullopt);

}
