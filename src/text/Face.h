#pragma once

// Face: one font, at any size — what layout measures with and paint draws
// with. Two kinds exist: the built-in Sashfold Mono, and a TrueType file
// read by text::TrueTypeFont and drawn by text::rasterize. Glyph ids are
// the face's own; 0 always means "no glyph here".

#include "core/Bitmap.h"
#include "text/TrueType.h"

#include <cstdint>
#include <memory>
#include <string>

namespace sashfold::text {

struct FaceMetrics {
    float ascent; // px above the baseline
    float descent; // px below the baseline, positive
    float line_gap; // extra leading the face asks for
};

class Face {
public:
    virtual ~Face() = default;

    virtual std::string const& family() const = 0;
    virtual bool is_bold() const = 0;
    virtual bool is_italic() const = 0;
    virtual bool is_monospace() const = 0;

    // 0 when the face has no glyph for the code point.
    virtual std::uint32_t glyph_index(char32_t code_point) const = 0;
    virtual FaceMetrics metrics(float size) const = 0;
    virtual float advance(std::uint32_t glyph, float size) const = 0;
    // Blends one glyph with its origin at (x, baseline_y). A weight or slant
    // the face was not designed with is synthesized.
    virtual void draw_glyph(Bitmap& target, std::uint32_t glyph, float x, float baseline_y,
        float size, Color color, bool bold, bool italic) const = 0;
};

// Sashfold Mono as a Face: glyph ids are code points. Its glyph_index is
// honest about what it lacks, but draw_glyph draws any code point, the
// unknown ones as the box — so it closes every fallback chain.
Face const& builtin_face();

// A face over a parsed TrueType font; owns the font and a mask cache.
std::unique_ptr<Face> make_truetype_face(TrueTypeFont font);

}
