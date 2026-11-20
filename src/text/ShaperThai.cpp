// The Thai and Lao shaper. SARA AM (U+0E33, Lao U+0EB3) is a NIKHAHIT
// over the consonant and a SARA AA after it: it is split into the two
// before shaping, and the NIKHAHIT is moved before any above-base marks
// (tone marks) the consonant already carries, so that it sits under them
// as the fonts draw it. Everything else is the default features.

#include "text/ShaperInternal.h"

namespace sashfold::text::shaping {

namespace {

bool is_sara_am(char32_t c)
{
    return (c & ~0x80u) == 0x0E33;
}

// The above-base marks of Thai and Lao (the same layout in both blocks).
bool is_above_base_mark(char32_t c)
{
    char32_t const u = c & ~0x80u;
    return (u >= 0x0E34 && u <= 0x0E37) || (u >= 0x0E47 && u <= 0x0E4E) || u == 0x0E31 || u == 0x0E3B;
}

void preprocess(Plan const&, ShapeFont const&, Buffer& b)
{
    b.clear_output();
    std::size_t const count = b.len();
    b.idx = 0;
    while (b.idx < count && !b.failed) {
        char32_t const u = b.cur().codepoint;
        if (!is_sara_am(u)) {
            b.next_glyph();
            continue;
        }
        // NIKHAHIT, then SARA AA in the SARA AM's place.
        GlyphInfo& nikhahit = b.output_glyph(0);
        nikhahit.codepoint = u - 0x0E33 + 0x0E4D;
        set_unicode_props(nikhahit);
        nikhahit.unicode |= otl::unicode_flag::continuation;
        // Kept a mark of class 0, so its width is zeroed as a mark's.
        nikhahit.category = static_cast<std::uint8_t>(GeneralCategory::Mn);
        b.cur().codepoint = u - 1;
        set_unicode_props(b.cur());
        b.next_glyph();
        std::size_t const end = b.out.size();
        std::size_t start = end - 2;
        while (start > 0 && is_above_base_mark(b.out[start - 1].codepoint))
            --start;
        if (start + 2 < end) {
            b.merge_out_clusters(start, end);
            GlyphInfo const moved = b.out[end - 2];
            for (std::size_t k = end - 2; k > start; --k)
                b.out[k] = b.out[k - 1];
            b.out[start] = moved;
        } else if (start) {
            // The NIKHAHIT is a mark of the letter before: one cluster.
            b.merge_out_clusters(start - 1, end);
        }
    }
    b.sync();
}

}

ScriptShaper const thai_shaper {
    nullptr,
    nullptr,
    nullptr,
    preprocess,
    nullptr,
    Normalization::ComposedDiacritics,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    0,
    ZeroMarks::ByGdefLate,
    false,
};

}
