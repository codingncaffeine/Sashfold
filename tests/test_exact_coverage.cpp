// The exact-area scan conversion (svg::rasterize_exact): each pixel's
// coverage is the area of it a shape covers, by either fill rule.

#include "Test.h"

#include "svg/Raster.h"

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace sashfold;

namespace {

svg::Polygon rect(float x0, float y0, float x1, float y1, bool clockwise = true)
{
    if (clockwise)
        return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    return { { x0, y0 }, { x0, y1 }, { x1, y1 }, { x1, y0 } };
}

int at(svg::Mask const& m, int x, int y)
{
    return m.at(x, y);
}

bool near(int actual, double coverage)
{
    bool const ok = std::abs(actual - coverage * 255) <= 1;
    if (!ok)
        std::fprintf(stderr, "  %d, wanted %.1f\n", actual, coverage * 255);
    return ok;
}

void test_areas()
{
    // Edges a quarter into the first pixel and three quarters into the last.
    svg::Mask const m = svg::rasterize_exact({ rect(0.25f, 0, 2.75f, 1) }, svg::FillRule::NonZero, 0, 0, 10, 10);
    CHECK(near(at(m, 0, 0), 0.75));
    CHECK(near(at(m, 1, 0), 1));
    CHECK(near(at(m, 2, 0), 0.75));
    CHECK(near(at(m, 3, 0), 0));
    // Fractional on both axes: the corner pixel is the product.
    svg::Mask const corner = svg::rasterize_exact({ rect(0.5f, 0.25f, 4, 4) }, svg::FillRule::NonZero, 0, 0, 10, 10);
    CHECK(near(at(corner, 0, 0), 0.5 * 0.75));
    CHECK(near(at(corner, 1, 0), 0.75));
    CHECK(near(at(corner, 0, 1), 0.5));
    // A right triangle over one pixel: half of it, whichever way it is wound.
    for (bool clockwise : { true, false }) {
        svg::Polygon triangle = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
        if (!clockwise)
            triangle = { { 0, 0 }, { 0, 1 }, { 1, 0 } };
        svg::Mask const t = svg::rasterize_exact({ triangle }, svg::FillRule::NonZero, 0, 0, 4, 4);
        CHECK(near(at(t, 0, 0), 0.5));
    }
    // A long shallow edge across many pixels: the area under it, pixel by pixel.
    svg::Mask const wedge = svg::rasterize_exact({ { { 0, 0 }, { 8, 1 }, { 0, 1 } } }, svg::FillRule::NonZero, 0, 0, 10, 10);
    for (int x = 0; x < 8; ++x) {
        // Below the line y = x / 8 across [x, x + 1]: 1 - (x + 0.5) / 8.
        CHECK(near(at(wedge, x, 0), 1 - (x + 0.5) / 8));
    }
}

void test_rules()
{
    // Two squares overlapping, wound the same way: nonzero fills the overlap, even-odd leaves it.
    std::vector<svg::Polygon> const two = { rect(0, 0, 4, 4), rect(2, 2, 6, 6) };
    svg::Mask const nonzero = svg::rasterize_exact(two, svg::FillRule::NonZero, 0, 0, 10, 10);
    svg::Mask const evenodd = svg::rasterize_exact(two, svg::FillRule::EvenOdd, 0, 0, 10, 10);
    CHECK(near(at(nonzero, 3, 3), 1));
    CHECK(near(at(evenodd, 3, 3), 0));
    CHECK(near(at(evenodd, 1, 1), 1));
    // Wound against each other: a hole by either rule.
    std::vector<svg::Polygon> const holed = { rect(0, 0, 6, 6), rect(2, 2, 4, 4, false) };
    CHECK(near(at(svg::rasterize_exact(holed, svg::FillRule::NonZero, 0, 0, 10, 10), 3, 3), 0));
    // The even-odd edge of the overlap, half a pixel in: half covered.
    std::vector<svg::Polygon> const half = { rect(0, 0, 4, 4), rect(2.5f, 0, 6, 4) };
    svg::Mask const folded = svg::rasterize_exact(half, svg::FillRule::EvenOdd, 0, 0, 10, 10);
    CHECK(near(at(folded, 2, 1), 0.5));
}

void test_thin_stems()
{
    // A stem 0.3 px wide keeps its weight at every phase: each row's
    // coverage sums to 0.3 of a pixel (4x4 samples give 0, 0.25 or 0.5).
    for (int step = 0; step < 16; ++step) {
        float const x = 3 + static_cast<float>(step) / 16;
        svg::Mask const m = svg::rasterize_exact({ rect(x, 0, x + 0.3f, 2) }, svg::FillRule::NonZero, 0, 0, 10, 10);
        int sum = 0;
        for (int px = 0; px < 10; ++px)
            sum += at(m, px, 0);
        CHECK(std::abs(sum - 0.3 * 255) <= 2);
    }
}

void test_clip_and_bounds()
{
    // Kept to the clip region; a shape reaching far past it is still exact inside it.
    svg::Mask const m = svg::rasterize_exact({ rect(-1000.5f, 2.5f, 3.5f, 4) }, svg::FillRule::NonZero, 0, 0, 5, 5);
    CHECK(m.left == 0 && m.width == 4);
    CHECK(near(at(m, 0, 3), 1));
    CHECK(near(at(m, 3, 3), 0.5));
    CHECK(near(at(m, 2, 2), 0.5));
    // Nothing for empty or degenerate input; a coordinate that is not a number refuses the whole.
    CHECK(svg::rasterize_exact({}, svg::FillRule::NonZero, 0, 0, 5, 5).empty());
    CHECK(svg::rasterize_exact({ { { 0, 0 }, { 4, 0 } } }, svg::FillRule::NonZero, 0, 0, 5, 5).empty());
    CHECK(svg::rasterize_exact({ { { 0, 0 }, { NAN, 1 }, { 0, 2 } } }, svg::FillRule::NonZero, 0, 0, 5, 5).empty());
    // A long thin diagonal over a large area stays fast and bounded.
    svg::Mask const diagonal = svg::rasterize_exact({ { { 0, 0 }, { 4000, 3000 }, { 4000, 3001 }, { 0, 1 } } }, svg::FillRule::NonZero, 0, 0, 4000, 3001);
    CHECK(diagonal.width == 4000 && diagonal.height == 3001);
}

}

int main()
{
    test_areas();
    test_rules();
    test_thin_stems();
    test_clip_and_bounds();
    return sashfold::test::report("test_exact_coverage");
}
