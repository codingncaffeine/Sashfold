// libFuzzer harness for the TrueType reader: hostile bytes through
// face_count, parse, and every accessor a renderer would call — no crash,
// no sanitizer finding.

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
            (void)font->outline(g);
            (void)font->advance_width(g);
            (void)font->left_side_bearing(g);
        }
        for (char32_t const c : { 0x41u, 0x20u, 0xE9u, 0x4E00u, 0x1F600u, 0xFFFFu })
            (void)font->glyph_index(c);
    }
    return 0;
}
