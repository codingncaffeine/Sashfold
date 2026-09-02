#pragma once

// TrueTypeWriter: emits a TrueType file from outlines and metrics — the
// other half of the reader's loop. Sashfold Mono becomes a real font file
// this way, and the reader's tests read back what the writer wrote. The
// output is deterministic: no timestamps, no floating point, so the same
// description yields the same bytes on every OS.

#include "text/TrueType.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::text {

struct WriterComponent {
    std::uint16_t glyph = 0;
    std::int16_t dx = 0; // font units
    std::int16_t dy = 0;
};

struct WriterGlyph {
    GlyphOutline outline; // the contours, when components is empty
    std::vector<WriterComponent> components; // a composite glyph when non-empty
    std::uint16_t advance = 0; // font units
};

struct FontDescription {
    std::string family = "Untitled";
    std::string subfamily = "Regular";
    std::string version = "Version 1.0";
    std::uint16_t units_per_em = 2048;
    std::int16_t ascender = 0;
    std::int16_t descender = 0; // negative below the baseline
    std::int16_t line_gap = 0;
    std::int16_t x_height = 0;
    std::int16_t cap_height = 0;
    std::uint16_t weight_class = 400;
    bool italic = false;
    bool fixed_pitch = false;
    bool long_loca = false; // 32-bit glyph offsets even when 16-bit would do
    std::vector<WriterGlyph> glyphs; // glyph 0 is .notdef
    std::vector<std::pair<char32_t, std::uint16_t>> mappings; // code point -> glyph, any order
};

// A complete TTF: OS/2, cmap (formats 4 and 12), glyf, head, hhea, hmtx,
// loca, maxp, name, post. Empty when the description is unusable (no
// glyphs, or more than 65535 of them).
std::vector<std::uint8_t> write_truetype(FontDescription const& font);

// Gathers TTFs written above into one TrueType collection, faces in order.
std::vector<std::uint8_t> write_collection(std::vector<std::vector<std::uint8_t>> const& fonts);

}
