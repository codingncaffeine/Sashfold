#pragma once

// The SVG subset: an <svg> element's content — paths, the basic shapes,
// groups, nested viewports, <use>, gradients, clip paths, a first cut of
// text — drawn into a bitmap through our own rasterizer, styled by the
// computed styles the cascade gave its elements, presentation attributes
// included. Inline in an HTML page it is a replaced element laid out and
// painted as a picture of the box's size; as a file it decodes like any
// other image, at its own intrinsic size.

#include "core/Bitmap.h"
#include "css/StyleResolver.h"
#include "svg/Raster.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace sashfold::dom {
class Element;
}

namespace sashfold::svg {

// What an <svg> says about its own size: a width and a height when they
// are written as absolute lengths (a percentage is not a size of its
// own), and the ratio its viewBox gives, width over height.
struct IntrinsicSize {
    std::optional<float> width;
    std::optional<float> height;
    std::optional<float> ratio;
};
IntrinsicSize intrinsic_size(dom::Element const& svg);

// Draws the element's content into a transparent bitmap of the given size
// in CSS px, the viewBox fitted to it as preserveAspectRatio says.
Bitmap render(dom::Element const& svg, css::StyleMap const& styles, float width, float height);

// A standalone SVG document's bytes, decoded at the size it declares — the
// viewBox's when it declares none, 300 by 150 when it has neither — and
// held to `max_pixels`. With `scale` given, a small picture is drawn
// larger — as many times its size as makes its shorter side at least 128
// px, up to four — and the factor is written there, so that a page
// showing the picture bigger than its declared size has the extra detail
// a vector picture owes it: the caller reports that factor as the
// picture's density.
bool looks_like_svg(std::vector<std::uint8_t> const& bytes);
std::optional<Bitmap> decode_svg(std::vector<std::uint8_t> const& bytes, std::size_t max_pixels = 16u << 20,
    float* scale = nullptr);

// --- Geometry for script (SVG 2 §5.13, §8.4, §9.2) -------------------------
//
// What getBBox, getCTM, getScreenCTM and the SVGGeometryElement methods ask,
// worked out by the walk render() draws by: the same transforms, viewBox
// fitting and shape geometry, at the size the outermost <svg> is laid out at.

// The computed style of an element, from whoever holds the styles.
using StyleOf = std::function<css::ComputedStyle const*(dom::Element const&)>;

// An element's user space: the matrix from it (its own transform applied,
// and for an <svg> its viewBox) to the outermost <svg>'s box in CSS px, and
// the viewport a percentage in it is of.
struct UserSpace {
    Matrix ctm;
    float viewport_width = 0;
    float viewport_height = 0;
};

// The outermost <svg> of the SVG fragment an element is in (the element
// itself when it is one with no <svg> above it), or null.
dom::Element const* outermost_svg(dom::Element const& element);

// The element's user space inside `root` drawn at width × height; nullopt when
// the element is not inside root or is not rendered (display: none).
std::optional<UserSpace> user_space(dom::Element const& root, dom::Element const& element, float width, float height, StyleOf const& style_of);

// A basic shape's or a path's geometry in its user space; nullopt for any
// other element.
std::optional<Path> geometry_of(dom::Element const& element, UserSpace const& space, StyleOf const& style_of);

// getBBox (§5.13.1): the tight bounds of the fill geometry in the element's
// user space — a container's the union of its children's, each through its
// own transform; an empty box at the origin for nothing.
Box bounding_box(dom::Element const& element, UserSpace const& space, StyleOf const& style_of);

// The length of a path and the point a distance along it (§9.4), measured
// on the path flattened finely; and whether a point is in its fill or
// within half a stroke width of it.
float path_length(Path const& path);
Point point_at_length(Path const& path, float distance);
bool point_in_fill(Path const& path, Point point, bool even_odd);
bool point_in_stroke(Path const& path, Point point, float stroke_width);

// A text content element's advance in user units (getComputedTextLength).
float text_length(dom::Element const& element, StyleOf const& style_of);

// The microsyntaxes, for the tests: path data (§9.3) and a transform
// list (§8.5). An error in path data keeps what came before it, as the
// specification asks.
Path parse_path_data(std::string_view data);
Matrix parse_transform(std::string_view text);

}
