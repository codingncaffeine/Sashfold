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

// The microsyntaxes, for the tests: path data (§9.3) and a transform
// list (§8.5). An error in path data keeps what came before it, as the
// specification asks.
Path parse_path_data(std::string_view data);
Matrix parse_transform(std::string_view text);

}
