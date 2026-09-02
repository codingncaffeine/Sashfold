#pragma once

// Sashfold Mono — the bootstrap font. An original, angular
// monospace face authored as stroke segments on a 20x32 design grid
// (units-per-em 32, advance 20, baseline at y=25). No font files, no
// parsing: the tables below ARE the font, and tools/gen-font will emit
// them as a real TTF once the font pipeline exists.
//
// Determinism: rasterization is pure integer math — segment quads expand
// on the grid, coordinates scale to 1/4-subpixel fixed point, and coverage
// comes from a 4x4 point-in-polygon subsample per pixel. No libm anywhere,
// so the pixels are byte-identical on every platform.

#include "core/Bitmap.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace sashfold::text {

struct TrueTypeOptions {
    bool bold = false;
    bool italic = false;
    bool long_loca = false; // 32-bit glyph offsets, so that reader path gets exercised too
    std::u32string_view only; // when non-empty, only these code points are mapped
};

struct FontMetrics {
    float ascent; // baseline offset from the line box top, px
    float descent; // px below the baseline
    float advance; // fixed advance per glyph, px
    float line_gap; // extra leading the font asks for (zero here)
};

class SashfoldMono {
public:
    static SashfoldMono const& instance();

    static constexpr int units_per_em = 32;
    static constexpr int design_advance = 20;
    static constexpr int design_ascent = 25; // baseline y on the grid
    static constexpr int design_descent = 7;

    static FontMetrics metrics(float size)
    {
        // Spelled as design * size / em, the same expression as advance():
        // layout's numbers must not shift by a rounding between the two.
        return { design_ascent * size / static_cast<float>(units_per_em),
            design_descent * size / static_cast<float>(units_per_em), advance(size), 0 };
    }

    static float advance(float size)
    {
        return design_advance * size / static_cast<float>(units_per_em);
    }

    // Blends one glyph at the given baseline origin. Unknown code points draw
    // the replacement box; control characters and space draw nothing.
    void draw_glyph(Bitmap& target, char32_t code_point, float x, float baseline_y, float size,
        Color color, bool bold, bool italic) const;

    // Whether draw_glyph would draw the code point itself (or, for blanks,
    // deliberately nothing) rather than the box.
    bool has_glyph(char32_t code_point) const;

    // Sum of advances (monospace: count * advance), for layout measurement.
    static float measure(std::u32string_view text, float size)
    {
        return static_cast<float>(text.size()) * advance(size);
    }

    // The face as a TrueType file, 2048 units per em: every stroke a
    // clockwise contour, every composed letter a composite glyph, every
    // alias a second cmap entry, the same bytes on every OS. What gen_font
    // writes to tests/fixtures/fonts and the reader's tests read back.
    std::vector<std::uint8_t> to_truetype(TrueTypeOptions const& options = {}) const;

private:
    SashfoldMono() = default;
};

}
