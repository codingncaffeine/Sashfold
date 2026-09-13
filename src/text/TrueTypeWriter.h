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

// A kerning pair, font units; negative pulls the pair together.
struct WriterKerningPair {
    std::uint16_t left = 0;
    std::uint16_t right = 0;
    std::int16_t value = 0;
};

// Which table carries the kerning pairs: the `kern` table (format 0), or
// GPOS pair positioning under a `kern` feature — by pairs (format 1), or
// by classes (format 2, one class per glyph named).
enum class WriterKerningTable : std::uint8_t {
    Kern,
    GposPairs,
    GposClasses,
};

// A glyph drawn as a picture: a PNG at the strike's ppem, its bottom-left
// corner `left` px right of the pen and `bottom` px above the baseline,
// `width` by `height` px.
struct WriterBitmapGlyph {
    std::uint16_t glyph = 0;
    std::vector<std::uint8_t> png;
    int left = 0;
    int bottom = 0;
    int width = 0;
    int height = 0;
};

// Which table carries the pictures: CBDT with its CBLC index (one strike,
// index format 1, image format 17), or sbix (one strike of PNG graphics).
enum class WriterBitmapTable : std::uint8_t {
    Cbdt,
    Sbix,
};

// A glyph drawn as layers of other glyphs in palette colours (COLR
// version 0 over CPAL's one palette), first to last; palette index
// 0xFFFF is the text's own colour.
struct WriterColorGlyph {
    std::uint16_t glyph = 0;
    std::vector<std::pair<std::uint16_t, std::uint16_t>> layers; // glyph, palette index
};

struct WriterPaletteColor {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 255;
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
    std::vector<WriterKerningPair> kerning; // any order; none writes no kerning table
    WriterKerningTable kerning_table = WriterKerningTable::Kern;
    std::vector<WriterBitmapGlyph> bitmap_glyphs; // one strike, at bitmap_ppem; none writes no bitmap table
    std::uint16_t bitmap_ppem = 0;
    WriterBitmapTable bitmap_table = WriterBitmapTable::Cbdt;
    std::vector<WriterColorGlyph> color_glyphs; // none writes no COLR or CPAL
    std::vector<WriterPaletteColor> palette;
};

// A complete TTF: OS/2, cmap (formats 4 and 12), glyf, head, hhea, hmtx,
// loca, maxp, name, post; kern or GPOS when there are kerning pairs;
// CBDT and CBLC or sbix when there are pictures; COLR and CPAL when
// there are colour layers. Empty when the description is unusable (no
// glyphs, or more than 65535 of them).
std::vector<std::uint8_t> write_truetype(FontDescription const& font);

// Gathers TTFs written above into one TrueType collection, faces in order.
std::vector<std::uint8_t> write_collection(std::vector<std::vector<std::uint8_t>> const& fonts);

}
