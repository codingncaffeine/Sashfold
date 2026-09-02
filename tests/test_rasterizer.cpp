#include "Test.h"

#include "core/Bitmap.h"
#include "text/Face.h"
#include "text/Rasterizer.h"
#include "text/SashfoldMono.h"
#include "text/TrueType.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

// The outline rasterizer against the stroke rasterizer: both sample the
// same 4x4 grid, and at 32 px the two geometries coincide exactly, so the
// TrueType face read from tests/fixtures/fonts must reproduce Sashfold
// Mono's pixels byte for byte. Then curves, the synthesized styles, and a
// TrueType face's metrics and advances through the Face interface.

using namespace sashfold;

namespace {

Color const white = Color::rgb(255, 255, 255);
Color const black = Color::rgb(0, 0, 0);

std::vector<std::uint8_t> read_file(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
}

int ink(Bitmap const& bitmap)
{
    int count = 0;
    for (int y = 0; y < bitmap.height(); ++y) {
        for (int x = 0; x < bitmap.width(); ++x) {
            if (!(bitmap.pixel(x, y) == white))
                ++count;
        }
    }
    return count;
}

int leftmost_ink(Bitmap const& bitmap)
{
    for (int x = 0; x < bitmap.width(); ++x) {
        for (int y = 0; y < bitmap.height(); ++y) {
            if (!(bitmap.pixel(x, y) == white))
                return x;
        }
    }
    return -1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: test_rasterizer <SashfoldMono.ttf>\n";
        return 2;
    }
    std::optional<text::TrueTypeFont> font = text::TrueTypeFont::parse(read_file(argv[1]));
    if (!CHECK(font.has_value()))
        return test::report("rasterizer");
    std::unique_ptr<text::Face> const face = text::make_truetype_face(std::move(*font));
    text::Face const& builtin = text::builtin_face();

    // --- Two rasterizers, one set of pixels ----------------------------------------------
    // Baseline 40 at 32 px puts the em top at 15: integral for both paths.
    std::vector<char32_t> code_points;
    for (char32_t c = 0x21; c <= 0x7E; ++c)
        code_points.push_back(c);
    for (char32_t const c : { 0xE9u, 0xC9u, 0x1EBFu, 0xEDu, 0x2192u, 0xA9u, 0x2013u })
        code_points.push_back(c);
    int identical = 0;
    int drawn = 0;
    for (char32_t const c : code_points) {
        Bitmap by_strokes(48, 64, white);
        Bitmap by_outline(48, 64, white);
        builtin.draw_glyph(by_strokes, c, 10, 40, 32, black, false, false);
        std::uint32_t const glyph = face->glyph_index(c);
        if (!CHECK(glyph != 0))
            continue;
        face->draw_glyph(by_outline, glyph, 10, 40, 32, black, false, false);
        if (ink(by_strokes) > 0)
            ++drawn;
        if (by_strokes.pixels() == by_outline.pixels())
            ++identical;
        else
            std::cerr << "  differs: U+" << std::hex << static_cast<unsigned>(c) << std::dec << "\n";
    }
    CHECK_EQ(identical, static_cast<int>(code_points.size()));
    CHECK(drawn > 90);

    // The same holds at 64 px, where the geometry also lands on the grid.
    {
        Bitmap by_strokes(96, 128, white);
        Bitmap by_outline(96, 128, white);
        builtin.draw_glyph(by_strokes, U'g', 12, 80, 64, black, false, false);
        face->draw_glyph(by_outline, face->glyph_index(U'g'), 12, 80, 64, black, false, false);
        CHECK(by_strokes.pixels() == by_outline.pixels());
        CHECK(ink(by_strokes) > 100);
    }

    // --- Curves: an off-curve point pulls the edge, and a contour may start off-curve ----
    {
        // A circle of radius 1000 units from four on-curve and four off-curve
        // points, then the same circle starting on an off-curve point.
        text::GlyphOutline circle;
        auto const push = [&](int x, int y, bool on) {
            circle.points.push_back(text::GlyphPoint { static_cast<std::int16_t>(x),
                static_cast<std::int16_t>(y), on });
        };
        push(1000, 0, true);
        push(1000, 1000, false);
        push(0, 1000, true);
        push(-1000, 1000, false);
        push(-1000, 0, true);
        push(-1000, -1000, false);
        push(0, -1000, true);
        push(1000, -1000, false);
        circle.contour_ends.push_back(7);
        text::GlyphMask const mask = text::rasterize(circle, 2048, 32 * 4);
        CHECK(!mask.empty());
        // The mask spans about 31 px each way; corners stay white, the middle is full.
        CHECK(mask.width >= 30 && mask.width <= 33);
        CHECK(mask.height >= 30 && mask.height <= 33);
        CHECK_EQ(mask.alpha[0], 0);
        CHECK_EQ(mask.alpha[static_cast<std::size_t>(mask.width) - 1], 0);
        std::size_t const center = static_cast<std::size_t>(mask.height / 2) * static_cast<std::size_t>(mask.width)
            + static_cast<std::size_t>(mask.width / 2);
        CHECK_EQ(mask.alpha[center], 255);
        // The diamond through the four on-curve points alone is smaller: the
        // curves bulge past its chords toward the control points.
        text::GlyphOutline diamond;
        for (text::GlyphPoint const& point : circle.points) {
            if (point.on_curve)
                diamond.points.push_back(point);
        }
        diamond.contour_ends.push_back(3);
        text::GlyphMask const diamond_mask = text::rasterize(diamond, 2048, 32 * 4);
        long long circle_total = 0;
        long long diamond_total = 0;
        for (std::uint8_t const a : mask.alpha)
            circle_total += a;
        for (std::uint8_t const a : diamond_mask.alpha)
            diamond_total += a;
        CHECK(circle_total > diamond_total);
        CHECK(diamond_total > 0);

        text::GlyphOutline rotated;
        for (std::size_t i = 0; i < 8; ++i)
            rotated.points.push_back(circle.points[(i + 1) % 8]); // begins off-curve
        rotated.contour_ends.push_back(7);
        text::GlyphMask const rotated_mask = text::rasterize(rotated, 2048, 32 * 4);
        CHECK(rotated_mask.alpha == mask.alpha);
    }

    // --- Nothing from nothing, and a hole under nonzero winding --------------------------
    {
        text::GlyphOutline empty;
        CHECK(text::rasterize(empty, 2048, 128).empty());
        // Two nested squares wound in opposite directions leave a hole.
        text::GlyphOutline ring;
        auto const square = [&](int half, bool clockwise) {
            std::vector<text::GlyphPoint> corners {
                { static_cast<std::int16_t>(-half), static_cast<std::int16_t>(-half), true },
                { static_cast<std::int16_t>(-half), static_cast<std::int16_t>(half), true },
                { static_cast<std::int16_t>(half), static_cast<std::int16_t>(half), true },
                { static_cast<std::int16_t>(half), static_cast<std::int16_t>(-half), true },
            };
            if (!clockwise)
                std::reverse(corners.begin(), corners.end());
            for (text::GlyphPoint const& corner : corners)
                ring.points.push_back(corner);
            ring.contour_ends.push_back(static_cast<std::uint16_t>(ring.points.size() - 1));
        };
        square(1000, true);
        square(500, false);
        text::GlyphMask const ring_mask = text::rasterize(ring, 2048, 128);
        std::size_t const middle = static_cast<std::size_t>(ring_mask.height / 2) * static_cast<std::size_t>(ring_mask.width)
            + static_cast<std::size_t>(ring_mask.width / 2);
        CHECK_EQ(ring_mask.alpha[middle], 0);
        CHECK_EQ(ring_mask.alpha[1 + static_cast<std::size_t>(ring_mask.width)], 255);
        // Wound the same way, the inner square adds nothing: nonzero fills it.
        ring = {};
        square(1000, true);
        square(500, true);
        text::GlyphMask const filled_mask = text::rasterize(ring, 2048, 128);
        CHECK_EQ(filled_mask.alpha[middle], 255);
    }

    // --- Synthesized styles --------------------------------------------------------------
    {
        Bitmap regular(48, 64, white);
        Bitmap bold(48, 64, white);
        Bitmap italic(48, 64, white);
        std::uint32_t const l = face->glyph_index(U'l');
        face->draw_glyph(regular, l, 10, 40, 32, black, false, false);
        face->draw_glyph(bold, l, 10, 40, 32, black, true, false);
        face->draw_glyph(italic, l, 10, 40, 32, black, false, true);
        CHECK(ink(bold) > ink(regular));
        CHECK(!(italic.pixels() == regular.pixels()));
        CHECK_EQ(ink(italic), ink(regular)); // a shear moves ink, it does not add any
    }

    // --- The Face interface over a TrueType font -------------------------------------
    CHECK_EQ(face->family(), std::string("Sashfold Mono"));
    CHECK(!face->is_bold());
    CHECK(!face->is_italic());
    CHECK(face->is_monospace());
    text::FaceMetrics const at32 = face->metrics(32);
    CHECK_EQ(at32.ascent, 25.0f);
    CHECK_EQ(at32.descent, 7.0f);
    CHECK_EQ(at32.line_gap, 0.0f);
    CHECK_EQ(face->advance(face->glyph_index(U'W'), 32), 20.0f);
    CHECK_EQ(face->advance(face->glyph_index(U'i'), 16), 10.0f);
    CHECK_EQ(face->glyph_index(0x0430), 0u);
    CHECK_EQ(builtin.glyph_index(0x0430), 0u);
    CHECK(builtin.glyph_index(U'A') != 0);
    CHECK(builtin.glyph_index(0x00E9) != 0);
    CHECK_EQ(builtin.metrics(32).ascent, 25.0f);
    // Drawing a glyph the face lacks draws nothing; the builtin draws the box.
    Bitmap missing(48, 64, white);
    face->draw_glyph(missing, 0, 10, 40, 32, black, false, false);
    CHECK_EQ(ink(missing), 0); // glyph 0 is .notdef: the writer gave it the box, but 0 means "none" here
    Bitmap box(48, 64, white);
    builtin.draw_glyph(box, 0x0430, 10, 40, 32, black, false, false);
    CHECK(ink(box) > 0);
    CHECK_EQ(leftmost_ink(box), leftmost_ink([&] {
        Bitmap b(48, 64, white);
        builtin.draw_glyph(b, 0xFFFD, 10, 40, 32, black, false, false);
        return b;
    }()));

    return test::report("rasterizer");
}
