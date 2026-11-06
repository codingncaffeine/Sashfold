#pragma once

// PDF's shadings (ISO 32000-2 §8.7.4.5): function-based (1), axial (2),
// radial (3), free-form and lattice-form triangle meshes (4, 5), Coons and
// tensor-product patch meshes (6, 7). Each is painted pixel by pixel from
// its definition — the parameter of the point found from the geometry, the
// color from the function — not approximated by a gradient, so extends,
// domains and degenerate circles come out as the standard says.

#include "paint/Canvas2D.h"
#include "pdf/ColorSpace.h"
#include "pdf/Function.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace sashfold::pdf {

class Document;

struct ShadingVertex {
    double x = 0;
    double y = 0;
    std::array<float, 32> color {}; // the space's components, or just t when there is a function
};

struct Shading {
    int type = 0;
    std::shared_ptr<ColorSpace const> space;
    std::shared_ptr<Function const> function;
    std::optional<std::array<float, 32>> background; // used only as a pattern's fill
    std::optional<std::array<double, 4>> bbox; // x0 y0 x1 y1 in shading space
    std::array<double, 6> coords {}; // axial: x0 y0 x1 y1; radial: x0 y0 r0 x1 y1 r1
    std::array<double, 4> domain { 0, 1, 0, 1 };
    std::array<bool, 2> extend { false, false };
    canvas::Transform matrix; // type 1: domain to shading space
    // Meshes, as triangles (three vertices each).
    std::vector<ShadingVertex> triangles;

    int color_values() const { return function ? 1 : space->components(); }
    // sRGB of a point's color values (or t through the function).
    void rgb_of(float const* values, float* rgb) const;
};

std::shared_ptr<Shading const> parse_shading(Document const& doc, Object const& object, Dict const* resources);

// Paints the shading into `layer` (premultiplied, transparent where it
// leaves nothing) over the pixels of `area` (x0 y0 x1 y1 in device
// pixels), shading space mapped by `to_device`. `use_background` fills
// what the shading leaves with its /Background, as a pattern does.
void paint_shading(canvas::Surface& layer, Shading const& shading, canvas::Transform const& to_device,
    std::array<int, 4> area, bool use_background);

}
