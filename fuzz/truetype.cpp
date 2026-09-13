// libFuzzer harness for the TrueType reader: hostile bytes through
// face_count, parse, and every accessor a renderer would call — no crash,
// no sanitizer finding.

#include "text/Rasterizer.h"
#include "text/TrueType.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    using sashfold::text::TrueTypeFont;
    std::vector<std::uint8_t> const bytes(data, data + size);
    std::size_t const faces = std::min<std::size_t>(TrueTypeFont::face_count(bytes), 4);
    for (std::size_t face = 0; face < faces; ++face) {
        auto const font = TrueTypeFont::parse(bytes, face);
        if (!font)
            continue;
        (void)font->family_name();
        (void)font->mapped_code_points();
        std::size_t const glyphs = std::min<std::size_t>(font->glyph_count(), 2048);
        for (std::size_t glyph = 0; glyph < glyphs; ++glyph) {
            auto const g = static_cast<std::uint16_t>(glyph);
            auto const outline = font->outline(g);
            (void)font->advance_width(g);
            (void)font->left_side_bearing(g);
            // Outlines are hostile too: rasterize the first few at a text size.
            if (outline && glyph < 32)
                (void)sashfold::text::rasterize(*outline, font->units_per_em(), 16 * 4);
        }
        for (char32_t const c : { 0x41u, 0x20u, 0xE9u, 0x4E00u, 0x1F600u, 0xFFFFu })
            (void)font->glyph_index(c);
        // The kerning tables are hostile too: every pair among the first
        // glyphs, and a pair beyond the glyph count.
        (void)font->has_kerning();
        for (std::size_t left = 0; left < std::min<std::size_t>(glyphs, 24); ++left) {
            for (std::size_t right = 0; right < std::min<std::size_t>(glyphs, 24); ++right)
                (void)font->kerning(static_cast<std::uint16_t>(left), static_cast<std::uint16_t>(right));
        }
        (void)font->kerning(0xFFFF, 0xFFFF);
        // So are the colour tables: a picture and the layers of the first
        // glyphs at a few sizes.
        for (std::size_t glyph = 0; glyph < std::min<std::size_t>(glyphs, 64); ++glyph) {
            auto const g = static_cast<std::uint16_t>(glyph);
            for (float const size : { 12.0f, 40.0f, 200.0f })
                (void)font->bitmap_glyph(g, size);
            (void)font->color_layers(g);
        }
    }
    return 0;
}
