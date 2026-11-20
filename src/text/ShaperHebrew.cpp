// The Hebrew shaper: the default features, with two things of its own. A
// font that positions no marks (no GPOS 'mark') may still draw pointed
// letters from the presentation forms of U+FB1D–FB4F, which Unicode keeps
// out of canonical composition; they are composed here when the font has
// them. And GPOS applies only when the font has the Hebrew script there:
// otherwise the marks are placed from glyph extents.

#include "text/ShaperInternal.h"

#include <algorithm>
#include <string>
#include <utility>

namespace sashfold::text::shaping {

namespace {

// The full canonical decomposition of a character.
void full_decomposition(char32_t c, std::u32string& out, int depth = 0)
{
    char32_t first = 0;
    char32_t second = 0;
    if (depth > 4 || !canonical_decompose(c, first, second)) {
        out.push_back(c);
        return;
    }
    full_decomposition(first, out, depth + 1);
    if (second)
        full_decomposition(second, out, depth + 1);
}

// The marks after the letter in canonical order (all the Hebrew points
// have non-zero classes).
void canonical_order(std::u32string& s)
{
    if (s.size() > 2)
        std::stable_sort(s.begin() + 1, s.end(),
            [](char32_t a, char32_t b) { return combining_class(a) < combining_class(b); });
}

bool compose(Plan const& plan, char32_t a, char32_t b, char32_t& ab)
{
    ab = canonical_compose(a, b);
    if (ab || plan.has_gpos_mark)
        return ab != 0;
    // The presentation form canonically equivalent to the pair: shin with
    // its dot and then dagesh is shin with dagesh and shin dot.
    std::u32string wanted;
    full_decomposition(a, wanted);
    wanted.push_back(b);
    canonical_order(wanted);
    for (char32_t c = 0xFB1D; c <= 0xFB4E; ++c) {
        std::u32string form;
        full_decomposition(c, form);
        if (form.size() > 1 && form == wanted) {
            ab = c;
            return true;
        }
    }
    return false;
}

// A meteg or a below mark that follows patah or qamats and then sheva or
// hiriq goes before the sheva or hiriq: the order in which fonts stack
// the three under one letter.
void reorder_marks(Plan const&, Buffer& b, std::size_t start, std::size_t end)
{
    for (std::size_t i = start + 2; i < end; ++i) {
        std::uint8_t const c0 = b.info[i - 2].combining_class;
        std::uint8_t const c1 = b.info[i - 1].combining_class;
        std::uint8_t const c2 = b.info[i].combining_class;
        bool const patah_or_qamats = c0 == 20 || c0 == 21;
        bool const sheva_or_hiriq = c1 == 22 || c1 == 23;
        bool const meteg_or_below = c2 == 25 || c2 == 220;
        if (patah_or_qamats && sheva_or_hiriq && meteg_or_below) {
            b.merge_clusters(i - 1, i + 1);
            std::swap(b.info[i - 1], b.info[i]);
            break;
        }
    }
}

}

ScriptShaper const hebrew_shaper {
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    Normalization::ComposedDiacritics,
    nullptr,
    compose,
    nullptr,
    reorder_marks,
    otl::make_tag('h', 'e', 'b', 'r'),
    ZeroMarks::ByGdefLate,
    true,
};

}
