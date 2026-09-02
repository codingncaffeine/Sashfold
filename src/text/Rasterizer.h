#pragma once

// Rasterizer: TrueType outlines to coverage masks. Quadratic contours are
// flattened in 26.6 fixed point, then scan-converted with the nonzero
// winding rule and 4x4 subsamples per pixel — the same sampling as Sashfold
// Mono, so a glyph has the same edge on every OS. No hinting, grayscale
// only, integer arithmetic throughout.

#include "text/TrueType.h"

#include <cstdint>
#include <vector>

namespace sashfold::text {

struct GlyphMask {
    int left = 0; // pixel offset from the glyph origin (the pen position on the baseline)
    int top = 0; // pixel offset from the baseline; negative above it
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha; // width * height, row-major

    bool empty() const { return alpha.empty(); }
};

struct RasterOptions {
    bool embolden = false; // one pixel of synthetic weight
    bool oblique = false; // synthetic italic: the same one-in-four shear as Sashfold Mono
};

// Renders an outline in font units at `size_q` quarter pixels per em.
GlyphMask rasterize(GlyphOutline const& outline, int units_per_em, int size_q,
    RasterOptions options = {});

}
