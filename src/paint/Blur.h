#pragma once

// A Gaussian blur as SVG's feGaussianBlur approximates one (Filter Effects
// §9.3): three box blurs one after another along each axis. Shared by the
// canvas's shadows and the page's box and text shadows.

#include <vector>

namespace sashfold::paint {

// One row or column blurred in place; `scratch` is working room the caller
// keeps between calls.
void gaussian_line(std::vector<float>& line, std::vector<float>& scratch, double sigma);

// A plane of `width` * `height` values, row-major, blurred along both axes;
// what lies beyond its edges counts as zero.
void gaussian_plane(std::vector<float>& plane, int width, int height, double sigma);

}
