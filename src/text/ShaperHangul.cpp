// The Hangul shaper. A syllable is a leading consonant, a vowel and an
// optional trailing consonant (Unicode §3.12), written precomposed (LV,
// LVT), partly (LV + T) or as jamo (L V T). A syllable the font can draw
// precomposed is drawn so; one it cannot (an Old Hangul syllable, or a
// font without the syllable) is drawn from its jamo with the 'ljmo',
// 'vjmo' and 'tjmo' features that make them combine. A tone mark after a
// syllable is moved before it, where it is drawn, unless the font draws it
// with no width over the syllable; a tone mark with no syllable gets a
// dotted circle to sit on.

#include "text/ShaperInternal.h"

#include <initializer_list>

namespace sashfold::text::shaping {

namespace {

using otl::make_tag;

enum Jamo : std::uint8_t {
    NotJamo,
    Leading,
    Vowel,
    Trailing,
};

constexpr char32_t s_base = 0xAC00;
constexpr char32_t l_base = 0x1100;
constexpr char32_t v_base = 0x1161;
constexpr char32_t t_base = 0x11A7;
constexpr unsigned l_count = 19;
constexpr unsigned v_count = 21;
constexpr unsigned t_count = 28;
constexpr unsigned n_count = v_count * t_count;
constexpr unsigned s_count = l_count * n_count;

bool is_l(char32_t c) { return (c >= 0x1100 && c <= 0x115F) || (c >= 0xA960 && c <= 0xA97C); }
bool is_v(char32_t c) { return (c >= 0x1160 && c <= 0x11A7) || (c >= 0xD7B0 && c <= 0xD7C6); }
bool is_t(char32_t c) { return (c >= 0x11A8 && c <= 0x11FF) || (c >= 0xD7CB && c <= 0xD7FB); }
bool is_tone_mark(char32_t c) { return c == 0x302E || c == 0x302F; }
// The jamo the precomposed syllables are made of.
bool combines_l(char32_t c) { return c >= l_base && c < l_base + l_count; }
bool combines_v(char32_t c) { return c >= v_base && c < v_base + v_count; }
bool combines_t(char32_t c) { return c > t_base && c < t_base + t_count; }
bool is_syllable(char32_t c) { return c >= s_base && c < s_base + s_count; }

bool zero_width(ShapeFont const& font, char32_t c)
{
    std::uint32_t const glyph = font.nominal_glyph(c);
    return glyph && font.advance(glyph) == 0;
}

// Replaces `count` characters of the input with `chars`, one cluster.
void replace_chars(Buffer& b, std::size_t count, std::initializer_list<char32_t> chars)
{
    b.merge_clusters(b.idx, b.idx + count);
    for (char32_t c : chars) {
        GlyphInfo& g = b.output_glyph(0);
        g.codepoint = c;
        set_unicode_props(g);
    }
    b.idx += count;
}

void preprocess(Plan const&, ShapeFont const& font, Buffer& b)
{
    b.clear_output();
    std::size_t const count = b.len();
    // The syllable just written to the output, [start, end): a tone mark
    // right after it moves in front of it.
    std::size_t start = 0;
    std::size_t end = 0;
    b.idx = 0;
    while (b.idx < count && !b.failed) {
        char32_t const u = b.cur().codepoint;
        if (is_tone_mark(u)) {
            if (start < end && end == b.out.size()) {
                b.next_glyph();
                if (!zero_width(font, u)) {
                    b.merge_out_clusters(start, end + 1);
                    GlyphInfo const tone = b.out[end];
                    for (std::size_t k = end; k > start; --k)
                        b.out[k] = b.out[k - 1];
                    b.out[start] = tone;
                }
            } else if (font.has_glyph(0x25CC)) {
                if (!zero_width(font, u))
                    replace_chars(b, 1, { u, 0x25CC });
                else
                    replace_chars(b, 1, { 0x25CC, u });
            } else {
                b.next_glyph();
            }
            start = end = b.out.size();
            continue;
        }
        start = b.out.size();
        if (is_l(u) && b.idx + 1 < count) {
            char32_t const v = b.cur(1).codepoint;
            if (is_v(v)) {
                char32_t t = 0;
                if (b.idx + 2 < count && is_t(b.cur(2).codepoint))
                    t = b.cur(2).codepoint;
                if (combines_l(u) && combines_v(v) && (t == 0 || combines_t(t))) {
                    char32_t const s = s_base + ((u - l_base) * v_count + (v - v_base)) * t_count + (t ? t - t_base : 0);
                    if (font.has_glyph(s)) {
                        replace_chars(b, t ? 3 : 2, { s });
                        end = start + 1;
                        continue;
                    }
                }
                // Old Hangul, or no precomposed glyph: the jamo, joined by
                // the font's jamo features.
                b.cur().shaper = Leading;
                b.next_glyph();
                b.cur().shaper = Vowel;
                b.next_glyph();
                if (t) {
                    b.cur().shaper = Trailing;
                    b.next_glyph();
                    end = start + 3;
                } else {
                    end = start + 2;
                }
                b.merge_out_clusters(start, end);
                continue;
            }
        } else if (is_syllable(u)) {
            bool const has = font.has_glyph(u);
            unsigned const index = static_cast<unsigned>(u - s_base);
            unsigned const l = index / n_count;
            unsigned const v = (index % n_count) / t_count;
            unsigned const t = index % t_count;
            bool const next_t = t == 0 && b.idx + 1 < count && is_t(b.cur(1).codepoint);
            if (t == 0 && b.idx + 1 < count && combines_t(b.cur(1).codepoint)) {
                // LV + T: one LVT when the font has it.
                char32_t const s = u + (b.cur(1).codepoint - t_base);
                if (font.has_glyph(s)) {
                    replace_chars(b, 2, { s });
                    end = start + 1;
                    continue;
                }
            }
            if (!has || next_t) {
                char32_t const parts[3] = { l_base + l, v_base + v, t_base + t };
                if (font.has_glyph(parts[0]) && font.has_glyph(parts[1]) && (!t || font.has_glyph(parts[2]))) {
                    std::size_t length = t ? 3 : 2;
                    if (t)
                        replace_chars(b, 1, { parts[0], parts[1], parts[2] });
                    else
                        replace_chars(b, 1, { parts[0], parts[1] });
                    // An LV split for the trailing consonant after it takes
                    // that consonant into the syllable.
                    if (has && !t) {
                        b.next_glyph();
                        ++length;
                    }
                    end = start + length;
                    std::size_t i = start;
                    b.out[i++].shaper = Leading;
                    b.out[i++].shaper = Vowel;
                    if (i < end)
                        b.out[i++].shaper = Trailing;
                    b.merge_out_clusters(start, end);
                    continue;
                }
            }
            if (has)
                end = start + 1;
        }
        b.next_glyph();
    }
    b.sync();
}

void collect_features(PlanBuilder& p)
{
    p.add_feature(make_tag('l', 'j', 'm', 'o'));
    p.add_feature(make_tag('v', 'j', 'm', 'o'));
    p.add_feature(make_tag('t', 'j', 'm', 'o'));
}

// 'calt' is not applied to Hangul: fonts that put their jamo lookups there
// would compose them twice.
void override_features(PlanBuilder& p)
{
    p.disable_feature(make_tag('c', 'a', 'l', 't'));
}

void init_plan(Plan& plan)
{
    plan.shaper_masks[Leading] = plan.one_mask(make_tag('l', 'j', 'm', 'o'));
    plan.shaper_masks[Vowel] = plan.one_mask(make_tag('v', 'j', 'm', 'o'));
    plan.shaper_masks[Trailing] = plan.one_mask(make_tag('t', 'j', 'm', 'o'));
}

void setup_masks(Plan const& plan, ShapeFont const&, Buffer& b, std::u32string_view, std::u32string_view)
{
    for (GlyphInfo& g : b.info) {
        if (g.shaper != NotJamo && g.shaper <= Trailing)
            g.mask |= plan.shaper_masks[g.shaper];
    }
}

}

ScriptShaper const hangul_shaper {
    collect_features,
    override_features,
    init_plan,
    preprocess,
    nullptr,
    Normalization::None,
    nullptr,
    nullptr,
    setup_masks,
    nullptr,
    0,
    ZeroMarks::None,
    false,
};

}
