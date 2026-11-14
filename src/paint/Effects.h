#pragma once

// What a box painted as one picture goes through on its way to the page:
// its filters (filter-effects-1 §11), then the shape it is cut to
// (css-masking-1 §5.1, with the basic shapes of css-shapes-1 §3).

#include "core/Bitmap.h"
#include "css/ComputedStyle.h"
#include "svg/Raster.h"

#include <vector>

namespace sashfold::paint {

// A picture with transparency of its own: four planes of width * height
// values from 0 to 1, row-major, the colors premultiplied by the alpha.
struct EffectLayer {
    int width = 0;
    int height = 0;
    std::vector<float> red;
    std::vector<float> green;
    std::vector<float> blue;
    std::vector<float> alpha;
};

// A group painted twice, once over black and once over white, read back
// as the group alone: where the two pictures agree the group is opaque,
// where they differ by the whole range it left the backdrop untouched, and
// in between it is that transparent. It needs nothing of the painter but
// that it composites source-over, which is all it does.
EffectLayer layer_from(Bitmap const& over_black, Bitmap const& over_white);

// An opaque picture as a layer: a backdrop, for the filters laid over it.
EffectLayer layer_of(Bitmap const& picture);

// The layer as a bitmap of straight colors and alpha, for Bitmap::draw.
Bitmap bitmap_of(EffectLayer const& layer);

// How far a filter list draws beyond the picture it is given, and so how
// far beyond what is shown it reads: a blur by three standard deviations
// on every side, a drop shadow by its offset and its own blur.
struct Reach {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};
Reach reach_of(css::FilterList const& filters);

// Runs the list over the layer, in the order written. `current` is the
// color a drop shadow written without one takes.
void apply_filters(EffectLayer& layer, css::FilterList const& filters, Color current);

// The box a clip path is sized against, in the coordinates the outline is
// wanted in.
struct ShapeBox {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

// The clip path's outline against that box, as polygons to fill by
// `even_odd` or the nonzero rule.
std::vector<svg::Polygon> clip_outline(css::ClipPath const& clip, ShapeBox const& box, bool& even_odd);

// Whether a point is inside it: what is outside neither paints nor answers
// the pointer.
bool clip_holds(css::ClipPath const& clip, ShapeBox const& box, float px, float py);

// Fades the layer by how much of each pixel the outline covers; the
// layer's corner is at (left, top) in the outline's coordinates.
void cut_to(EffectLayer& layer, std::vector<svg::Polygon> const& outline, bool even_odd, int left, int top);

}
