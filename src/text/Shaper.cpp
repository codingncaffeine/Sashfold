#include "text/Shaper.h"

#include "core/Bidi.h"
#include "core/Unicode.h"
#include "text/Face.h"
#include "text/ShaperInternal.h"
#include "text/TrueType.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sashfold::text {

namespace shaping {

using otl::make_tag;

// What a face keeps for the shaper: its layout tables, parsed once, and the
// plans made for it, by script, language, direction and features.
struct FaceCache {
    explicit FaceCache(TrueTypeFont const& face_font)
        : font(face_font)
        , tables(otl::LayoutTables::parse(face_font))
    {
    }
    TrueTypeFont const& font;
    otl::LayoutTables tables;
    std::uint64_t const serial = next_serial();
    mutable std::mutex mutex;
    mutable std::unordered_map<std::string, std::unique_ptr<Plan const>> plans;
    static constexpr std::size_t plan_bound = 256;

    static std::uint64_t next_serial()
    {
        static std::atomic<std::uint64_t> counter { 0 };
        return ++counter;
    }
};

std::uint32_t ShapeFont::nominal_glyph(char32_t c) const
{
    return font.glyph_index(c);
}

std::int32_t ShapeFont::advance(std::uint32_t glyph) const
{
    return glyph > 0xFFFF ? 0 : font.advance_width(static_cast<std::uint16_t>(glyph));
}

// The shaper of every script without rules of its own (Latin, Greek,
// Cyrillic, ...): the common features only.
ScriptShaper const default_shaper {};

// --- Character properties as the shaper keeps them -------------------------------

namespace {

constexpr auto category_index(GeneralCategory c) { return static_cast<std::uint8_t>(c); }

// Default_Ignorable_Code_Point, but for the Hangul fillers: a filler stands
// for a missing jamo and is drawn as the blank it is.
bool is_ignorable(char32_t c)
{
    if (c == 0x115F || c == 0x1160 || c == 0x3164 || c == 0xFFA0)
        return false;
    return is_default_ignorable(c);
}

// The order marks are sorted in before shaping: the canonical combining
// class, but for the scripts whose fonts expect another order than
// Unicode's. Hebrew points sort shin and sin dots first, then dagesh and
// rafe, then the vowels, then meteg — the order Hebrew fonts are designed
// to compose in; Arabic sorts shadda before the vowel marks it carries;
// Thai sorts the below-base vowels before the phinthu and the tone marks.
std::uint8_t shaping_combining_class(char32_t c, std::uint8_t ccc)
{
    // Tibetan: the subjoined-letter-like marks after the vowels.
    if (c == 0x1A60 || c == 0x0FC6)
        return 254;
    if (c == 0x0F39)
        return 127;
    static constexpr std::uint8_t hebrew[] = {
        22, // 10 sheva
        15, // 11 hataf segol
        16, // 12 hataf patah
        17, // 13 hataf qamats
        23, // 14 hiriq
        18, // 15 tsere
        19, // 16 segol
        20, // 17 patah
        21, // 18 qamats
        14, // 19 holam
        24, // 20 qubuts
        12, // 21 dagesh
        25, // 22 meteg
        13, // 23 rafe
        10, // 24 shin dot
        11, // 25 sin dot
        26, // 26 point varika
    };
    if (ccc >= 10 && ccc <= 26)
        return hebrew[ccc - 10];
    if (ccc >= 27 && ccc <= 32)
        return static_cast<std::uint8_t>(ccc + 1);
    if (ccc == 33)
        return 27;
    switch (ccc) {
    case 84: // Telugu length marks: vowel signs, sorted before the virama
        return 4;
    case 91:
        return 5;
    case 103: // Thai sara u, sara uu
        return 3;
    case 130: // Tibetan sign i after sign u
        return 132;
    case 132:
        return 131;
    default:
        return ccc;
    }
}

}

bool is_unicode_mark(GlyphInfo const& g)
{
    return is_mark_category(static_cast<GeneralCategory>(g.category));
}

void set_unicode_props(GlyphInfo& g)
{
    char32_t const c = g.codepoint;
    if (c < 0x80) {
        // ASCII: a category, nothing else.
        static std::array<std::uint8_t, 0x80> const ascii = [] {
            std::array<std::uint8_t, 0x80> table {};
            for (char32_t k = 0; k < 0x80; ++k)
                table[k] = category_index(general_category(k));
            return table;
        }();
        g.category = ascii[c];
        g.combining_class = 0;
        g.unicode = 0;
        return;
    }
    CharacterProperties const p = character_properties(c);
    g.category = category_index(p.category);
    g.combining_class = 0;
    std::uint16_t flags = 0;
    if (c >= 0x80) {
        if (is_ignorable(c)) {
            flags |= otl::unicode_flag::ignorable;
            if (c == 0x200C)
                flags |= otl::unicode_flag::zwnj;
            else if (c == 0x200D)
                flags |= otl::unicode_flag::zwj;
            // Ignorables that must stay visible to GSUB: the Mongolian free
            // variation selectors, the tags, the grapheme joiner.
            else if ((c >= 0x180B && c <= 0x180D) || c == 0x180F || (c >= 0xE0020 && c <= 0xE007F) || c == 0x034F)
                flags |= otl::unicode_flag::hidden;
        }
        if (is_mark_category(p.category)) {
            flags |= otl::unicode_flag::continuation;
            g.combining_class = shaping_combining_class(c, p.combining_class);
        }
    }
    g.unicode = flags;
}

void set_glyph(GlyphInfo& g, ShapeFont const& font)
{
    g.glyph = font.nominal_glyph(g.codepoint);
}

namespace {

// Grapheme continuation beyond the marks (UAX #29 in the part shaping
// needs): emoji modifiers, the second regional indicator of a pair, a ZWJ
// and the pictograph after it, the halfwidth sound marks and the tags.
void set_all_unicode_props(Buffer& b)
{
    std::size_t const count = b.len();
    for (std::size_t i = 0; i < count; ++i) {
        GlyphInfo& g = b.info[i];
        set_unicode_props(g);
        char32_t const c = g.codepoint;
        auto const category = static_cast<GeneralCategory>(g.category);
        if (category == GeneralCategory::Ll || category == GeneralCategory::Lo || category == GeneralCategory::Lm
            || category == GeneralCategory::Lt || category == GeneralCategory::Lu || category == GeneralCategory::Zs)
            continue;
        if (category == GeneralCategory::Sk && c >= 0x1F3FB && c <= 0x1F3FF) {
            g.unicode |= otl::unicode_flag::continuation;
        } else if (c >= 0x1F1E6 && c <= 0x1F1FF) {
            if (i && b.info[i - 1].codepoint >= 0x1F1E6 && b.info[i - 1].codepoint <= 0x1F1FF
                && !(b.info[i - 1].unicode & otl::unicode_flag::continuation))
                g.unicode |= otl::unicode_flag::continuation;
        } else if (g.unicode & otl::unicode_flag::zwj) {
            g.unicode |= otl::unicode_flag::continuation;
            if (i + 1 < count && character_properties(b.info[i + 1].codepoint).extended_pictographic) {
                ++i;
                set_unicode_props(b.info[i]);
                b.info[i].unicode |= otl::unicode_flag::continuation;
            }
        } else if ((c >= 0xFF9E && c <= 0xFF9F) || (c >= 0xE0020 && c <= 0xE007F)) {
            g.unicode |= otl::unicode_flag::continuation;
        }
    }
}

bool is_continuation(GlyphInfo const& g)
{
    return (g.unicode & otl::unicode_flag::continuation) != 0;
}

// Clusters are graphemes at least: a base and what continues it share one.
void form_clusters(Buffer& b)
{
    std::size_t start = 0;
    for (std::size_t i = 1; i <= b.len(); ++i) {
        if (i < b.len() && is_continuation(b.info[i]))
            continue;
        b.merge_clusters(start, i);
        start = i;
    }
}

// A run written against its script's own direction is reversed by
// graphemes and shaped in the script's direction.
void reverse_graphemes(Buffer& b)
{
    b.reverse();
    std::size_t start = 0;
    for (std::size_t i = 1; i <= b.len(); ++i) {
        // After reversal a grapheme's base comes last: it ends where a
        // non-continuation is.
        if (i - 1 < b.len() && is_continuation(b.info[i - 1]))
            continue;
        b.reverse_range(start, i);
        start = i;
    }
}

void insert_dotted_circle(Buffer& b, ShapeFont const& font)
{
    if (b.len() == 0 || !is_unicode_mark(b.info[0]))
        return;
    std::uint32_t const glyph = font.nominal_glyph(0x25CC);
    if (glyph == 0)
        return;
    GlyphInfo circle = b.info[0];
    circle.codepoint = 0x25CC;
    circle.glyph = glyph;
    set_unicode_props(circle);
    b.info.insert(b.info.begin(), circle);
}

// --- Normalization (as shapers do it: by what the font has) ------------------------

// Which U+0020-like space a space character is drawn as when the font has
// no glyph of its own for it, and how wide.
enum Space : std::uint8_t {
    NotSpace = 0,
    SpaceEm = 1,
    SpaceEm2 = 2,
    SpaceEm3 = 3,
    SpaceEm4 = 4,
    SpaceEm5 = 5,
    SpaceEm6 = 6,
    SpaceEm16 = 16,
    Space4Em18 = 17, // 4/18 em
    SpacePlain = 18,
    SpaceFigure = 19,
    SpacePunctuation = 20,
    SpaceNarrow = 21,
};

Space space_type(char32_t c)
{
    switch (c) {
    case 0x0020:
    case 0x00A0:
        return SpacePlain;
    case 0x2000:
    case 0x2002:
        return SpaceEm2; // en quad, en space
    case 0x2001:
    case 0x2003:
    case 0x3000:
        return SpaceEm; // em quad, em space, ideographic space
    case 0x2004:
        return SpaceEm3;
    case 0x2005:
        return SpaceEm4;
    case 0x2006:
        return SpaceEm6;
    case 0x2007:
        return SpaceFigure;
    case 0x2008:
        return SpacePunctuation;
    case 0x2009:
        return SpaceEm5; // thin space
    case 0x200A:
        return SpaceEm16; // hair space
    case 0x202F:
        return SpaceNarrow;
    case 0x205F:
        return Space4Em18; // medium mathematical space
    default:
        return NotSpace;
    }
}

struct Normalizer {
    Plan const& plan;
    ShapeFont const& font;
    Buffer& b;

    bool decompose_pair(char32_t ab, char32_t& a, char32_t& c2) const
    {
        if (plan.shaper->decompose)
            return plan.shaper->decompose(plan, ab, a, c2);
        return canonical_decompose(ab, a, c2);
    }
    bool compose_pair(char32_t a, char32_t c2, char32_t& ab) const
    {
        if (plan.shaper->compose)
            return plan.shaper->compose(plan, a, c2, ab);
        ab = canonical_compose(a, c2);
        return ab != 0;
    }

    void output_char(char32_t c, std::uint32_t glyph)
    {
        GlyphInfo& g = b.output_glyph(glyph);
        g.codepoint = c;
        set_unicode_props(g);
    }
    void next_char(std::uint32_t glyph)
    {
        b.cur().glyph = glyph;
        b.next_glyph();
    }

    // Writes the decomposition of `ab` (with the font's glyphs) when the
    // font can draw it, and returns how many characters it wrote: 0 when
    // it could not. `shortest` stops at the first level the font has.
    unsigned decompose(bool shortest, char32_t ab, int depth = 0)
    {
        char32_t a = 0;
        char32_t c2 = 0;
        if (depth > 8 || !decompose_pair(ab, a, c2))
            return 0;
        std::uint32_t c2_glyph = 0;
        if (c2 && (c2_glyph = font.nominal_glyph(c2)) == 0)
            return 0;
        std::uint32_t const a_glyph = font.nominal_glyph(a);
        if (shortest && a_glyph) {
            output_char(a, a_glyph);
            if (c2) {
                output_char(c2, c2_glyph);
                return 2;
            }
            return 1;
        }
        if (unsigned const written = decompose(shortest, a, depth + 1)) {
            if (c2) {
                output_char(c2, c2_glyph);
                return written + 1;
            }
            return written;
        }
        if (a_glyph) {
            output_char(a, a_glyph);
            if (c2) {
                output_char(c2, c2_glyph);
                return 2;
            }
            return 1;
        }
        return 0;
    }

    void decompose_current(bool shortest)
    {
        char32_t const u = b.cur().codepoint;
        std::uint32_t const glyph = font.nominal_glyph(u);
        if (shortest && glyph) {
            next_char(glyph);
            return;
        }
        if (decompose(shortest, u)) {
            b.skip_glyph();
            return;
        }
        if (!shortest && glyph) {
            next_char(glyph);
            return;
        }
        if (static_cast<GeneralCategory>(b.cur().category) == GeneralCategory::Zs) {
            // A space the font lacks is drawn as the font's space, sized as
            // the space it stands for.
            Space const type = space_type(u);
            std::uint32_t const space = font.nominal_glyph(0x20);
            if (type != NotSpace && space) {
                b.cur().space = type;
                next_char(space);
                return;
            }
        }
        if (u == 0x2011) {
            // The non-breaking hyphen as the hyphen.
            if (std::uint32_t const hyphen = font.nominal_glyph(0x2010)) {
                next_char(hyphen);
                return;
            }
        }
        next_char(0);
    }

    static bool is_variation_selector(char32_t c)
    {
        return (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF) || (c >= 0x180B && c <= 0x180D)
            || c == 0x180F;
    }

    // A cluster with a variation selector is not normalized: each base
    // with its selector is looked up as a variation sequence (cmap format
    // 14), and what the font has no sequence for is passed on as two
    // characters for GSUB.
    void variation_selector_cluster(std::size_t end)
    {
        while (b.idx + 1 < end && !b.failed) {
            if (is_variation_selector(b.cur(1).codepoint)) {
                std::int64_t const variant = font.tables.variation_glyph(b.cur().codepoint, b.cur(1).codepoint);
                std::uint32_t const glyph = variant > 0 ? static_cast<std::uint32_t>(variant)
                    : variant < 0                       ? font.nominal_glyph(b.cur().codepoint)
                                                        : 0;
                if (glyph) {
                    // The pair becomes one glyph, one cluster.
                    b.merge_clusters(b.idx, b.idx + 2);
                    GlyphInfo& g = b.output_glyph(glyph);
                    (void)g;
                    b.skip_glyph();
                    b.skip_glyph();
                } else {
                    set_glyph(b.cur(), font);
                    b.next_glyph();
                    b.cur().unicode |= otl::unicode_flag::variation_selector_fallback;
                    set_glyph(b.cur(), font);
                    b.next_glyph();
                }
                while (b.idx < end && is_variation_selector(b.cur().codepoint)) {
                    set_glyph(b.cur(), font);
                    b.next_glyph();
                }
            } else {
                set_glyph(b.cur(), font);
                b.next_glyph();
            }
        }
        if (b.idx < end) {
            set_glyph(b.cur(), font);
            b.next_glyph();
        }
    }

    void run(Normalization mode)
    {
        bool const might_short_circuit
            = mode != Normalization::Decomposed && mode != Normalization::ComposedDiacriticsNoShortCircuit;
        bool const always_short_circuit = mode == Normalization::None;
        bool all_simple = true;
        // Text without marks whose every character the font has needs
        // nothing more than its glyphs.
        if (might_short_circuit) {
            bool simple = true;
            for (GlyphInfo& g : b.info) {
                if (is_unicode_mark(g) || (g.glyph = font.nominal_glyph(g.codepoint)) == 0) {
                    simple = false;
                    break;
                }
            }
            if (simple)
                return;
        }
        // Round one: decompose what the font cannot draw as it is, and
        // every base with marks after it, as far as the font can.
        b.clear_output();
        std::size_t const count = b.len();
        b.idx = 0;
        while (b.idx < count && !b.failed) {
            std::size_t end = b.idx + 1;
            while (end < count && !is_unicode_mark(b.info[end]))
                ++end;
            if (end < count)
                --end; // the last one is the base of the marks after it
            while (b.idx < end && !b.failed)
                decompose_current(might_short_circuit);
            if (b.idx == count || b.failed)
                break;
            all_simple = false;
            end = b.idx + 1;
            while (end < count && is_unicode_mark(b.info[end]))
                ++end;
            if (b.idx + 1 == end) {
                decompose_current(might_short_circuit);
            } else {
                bool has_selector = false;
                for (std::size_t i = b.idx; i < end; ++i)
                    has_selector = has_selector || is_variation_selector(b.info[i].codepoint);
                if (has_selector)
                    variation_selector_cluster(end);
                else
                    while (b.idx < end && !b.failed)
                        decompose_current(always_short_circuit);
            }
        }
        b.sync();
        if (all_simple)
            return;

        // Round two: sort each run of marks by combining class (UAX #15
        // canonical ordering, by the shaping classes), then let the script
        // reorder within it. Long runs are left as they are.
        for (std::size_t i = 0; i < b.len(); ++i) {
            if (b.info[i].combining_class == 0)
                continue;
            std::size_t end = i + 1;
            while (end < b.len() && b.info[end].combining_class != 0)
                ++end;
            if (end - i <= 32) {
                b.sort(i, end, [](GlyphInfo const& x, GlyphInfo const& y) { return x.combining_class < y.combining_class; });
                if (plan.shaper->reorder_marks)
                    plan.shaper->reorder_marks(plan, b, i, end);
            }
            i = end;
        }
        // A grapheme joiner that kept nothing apart is stepped over by GSUB.
        for (std::size_t i = 1; i + 1 < b.len(); ++i) {
            if (b.info[i].codepoint == 0x034F
                && (b.info[i + 1].combining_class == 0 || b.info[i - 1].combining_class <= b.info[i + 1].combining_class))
                b.info[i].unicode &= static_cast<std::uint16_t>(~otl::unicode_flag::hidden);
        }

        // Round three: compose each mark with its starter where the font
        // has the composite and nothing between blocks it.
        if (mode != Normalization::ComposedDiacritics && mode != Normalization::ComposedDiacriticsNoShortCircuit)
            return;
        b.clear_output();
        std::size_t const total = b.len();
        std::size_t starter = 0;
        b.idx = 0;
        b.next_glyph();
        while (b.idx < total && !b.failed) {
            if (is_unicode_mark(b.cur())) {
                char32_t composed = 0;
                std::uint32_t glyph = 0;
                if ((starter == b.out.size() - 1 || b.prev().combining_class < b.cur().combining_class)
                    && compose_pair(b.out[starter].codepoint, b.cur().codepoint, composed)
                    && (glyph = font.nominal_glyph(composed)) != 0) {
                    b.next_glyph();
                    b.merge_out_clusters(starter, b.out.size());
                    b.out.pop_back();
                    b.out[starter].codepoint = composed;
                    b.out[starter].glyph = glyph;
                    set_unicode_props(b.out[starter]);
                    continue;
                }
            }
            b.next_glyph();
            if (b.prev().combining_class == 0)
                starter = b.out.size() - 1;
        }
        b.sync();
    }
};

// --- Plans ------------------------------------------------------------------------

constexpr std::uint32_t tag_dflt_script = make_tag('D', 'F', 'L', 'T');
constexpr std::uint32_t tag_dflt_lang = make_tag('d', 'f', 'l', 't');
constexpr std::uint32_t tag_latn = make_tag('l', 'a', 't', 'n');

ScriptShaper const& choose_shaper(ScriptCode script, std::uint32_t chosen_gsub_script)
{
    switch (script) {
    case scripts::arabic:
    case scripts::syriac:
    case scripts::mongolian:
    case scripts::nko:
    case scripts::phags_pa:
    case scripts::mandaic:
    case scripts::manichaean:
    case scripts::psalter_pahlavi:
    case scripts::adlam:
    case scripts::hanifi_rohingya:
    case scripts::sogdian:
    case scripts::old_uyghur:
    case scripts::chorasmian:
        // The joining scripts shape as Arabic when the font knows the
        // script (Arabic always: its fallback needs no font support).
        if (chosen_gsub_script != tag_dflt_script || script == scripts::arabic)
            return arabic_shaper;
        return default_shaper;
    case scripts::hebrew:
        return hebrew_shaper;
    case scripts::thai:
    case scripts::lao:
        return thai_shaper;
    case scripts::hangul:
        return hangul_shaper;
    case script_code('D', 'e', 'v', 'a'):
    case script_code('B', 'e', 'n', 'g'):
    case script_code('G', 'u', 'r', 'u'):
    case script_code('G', 'u', 'j', 'r'):
    case script_code('O', 'r', 'y', 'a'):
    case script_code('T', 'a', 'm', 'l'):
    case script_code('T', 'e', 'l', 'u'):
    case script_code('K', 'n', 'd', 'a'):
    case script_code('M', 'l', 'y', 'm'):
        // The Indic model of the script development specifications, when
        // the font knows the script: one whose substitutions were made
        // for the default script (or put under 'latn') gets none of the
        // Indic features.
        if (chosen_gsub_script == tag_dflt_script || chosen_gsub_script == tag_latn)
            return default_shaper;
        return indic_shaper;
    case script_code('K', 'h', 'm', 'r'):
        return khmer_shaper;
    default:
        // The Universal Shaping Engine's scripts, when the font knows the
        // script (as for the Indic scripts above).
        if (is_use_script(script) && chosen_gsub_script != tag_dflt_script && chosen_gsub_script != tag_latn)
            return use_shaper;
        return default_shaper;
    }
}

}

void PlanBuilder::add_feature(std::uint32_t tag, std::uint32_t flags, std::uint32_t value)
{
    if (tag == 0)
        return;
    Request r;
    r.tag = tag;
    r.flags = flags;
    r.max_value = value;
    r.default_value = (flags & feature_flag::global) ? value : 0;
    r.stage = current_stage;
    r.seq = requests.size() + 1;
    requests.push_back(r);
}

FeatureMap const* Plan::feature(std::uint32_t tag) const
{
    auto const it = std::lower_bound(features.begin(), features.end(), tag,
        [](FeatureMap const& f, std::uint32_t t) { return f.tag < t; });
    return it != features.end() && it->tag == tag ? &*it : nullptr;
}

namespace {

struct Selection {
    std::uint32_t script = 0; // the script table, 0 for none
    std::uint32_t lang_sys = 0;
    std::uint32_t chosen = 0;
    bool found = false;
    int variation = -1;
};

Selection select(otl::LayoutTable const& t, OpenTypeScriptTags const& scripts_wanted, std::uint32_t language)
{
    Selection s;
    if (!t.present())
        return s;
    for (std::size_t k = 0; k < scripts_wanted.count && !s.script; ++k) {
        if ((s.script = t.find_script(scripts_wanted.tags[k]))) {
            s.chosen = scripts_wanted.tags[k];
            s.found = true;
        }
    }
    // The default script; then the 'latn' some fonts put everything under.
    for (std::uint32_t fallback : { tag_dflt_script, tag_dflt_lang, tag_latn }) {
        if (s.script)
            break;
        if ((s.script = t.find_script(fallback)))
            s.chosen = fallback;
    }
    if (s.script) {
        s.lang_sys = language ? t.find_lang_sys(s.script, language) : 0;
        if (!s.lang_sys)
            s.lang_sys = t.find_lang_sys(s.script, tag_dflt_lang);
        if (!s.lang_sys)
            s.lang_sys = t.default_lang_sys(s.script);
    }
    s.variation = t.feature_variation_at_default();
    return s;
}

unsigned bit_storage(std::uint32_t v)
{
    return v ? 32u - static_cast<unsigned>(std::countl_zero(v)) : 0u;
}

void collect_features(PlanBuilder& p, std::span<FeatureSetting const> user)
{
    p.enable_feature(make_tag('r', 'v', 'r', 'n'));
    p.add_gsub_pause(nullptr);
    if (p.direction == TextDirection::LeftToRight) {
        p.enable_feature(make_tag('l', 't', 'r', 'a'));
        p.enable_feature(make_tag('l', 't', 'r', 'm'));
    } else {
        p.enable_feature(make_tag('r', 't', 'l', 'a'));
        p.add_feature(make_tag('r', 't', 'l', 'm'));
    }
    // Fractions around U+2044 (setup_fraction_masks).
    p.add_feature(make_tag('f', 'r', 'a', 'c'));
    p.add_feature(make_tag('n', 'u', 'm', 'r'));
    p.add_feature(make_tag('d', 'n', 'o', 'm'));
    p.enable_feature(make_tag('r', 'a', 'n', 'd'), feature_flag::random, max_feature_value);
    // The script's own features.
    ScriptShaper const& shaper = choose_shaper(p.script, p.chosen_script[0]);
    if (shaper.collect_features)
        shaper.collect_features(p);
    // The features every script gets (§Shaping: the common and horizontal
    // features).
    for (std::uint32_t tag : { make_tag('a', 'b', 'v', 'm'), make_tag('b', 'l', 'w', 'm'), make_tag('c', 'c', 'm', 'p'),
             make_tag('l', 'o', 'c', 'l'), make_tag('r', 'l', 'i', 'g') })
        p.enable_feature(tag);
    // A joiner between a mark and its base keeps the mark off it: mark
    // attachment does not step over ZWJ.
    p.enable_feature(make_tag('m', 'a', 'r', 'k'), feature_flag::manual_zwj | feature_flag::manual_zwnj);
    p.enable_feature(make_tag('m', 'k', 'm', 'k'), feature_flag::manual_zwj | feature_flag::manual_zwnj);
    for (std::uint32_t tag : { make_tag('c', 'a', 'l', 't'), make_tag('c', 'l', 'i', 'g'), make_tag('c', 'u', 'r', 's'),
             make_tag('d', 'i', 's', 't'), make_tag('l', 'i', 'g', 'a'), make_tag('r', 'c', 'l', 't') })
        p.enable_feature(tag);
    p.enable_feature(make_tag('k', 'e', 'r', 'n'), feature_flag::has_fallback);
    for (FeatureSetting const& f : user)
        p.add_feature(f.tag, f.global() ? feature_flag::global : 0, f.value);
    if (shaper.override_features)
        shaper.override_features(p);
}

std::unique_ptr<Plan> compile_plan(otl::LayoutTables const& tables, ScriptCode script, std::uint32_t language,
    TextDirection direction, std::span<FeatureSetting const> user)
{
    auto plan = std::make_unique<Plan>();
    plan->script = script;
    plan->direction = direction;
    OpenTypeScriptTags const wanted = opentype_script_tags(script);
    std::array<Selection, 2> const selected { select(tables.gsub, wanted, language), select(tables.gpos, wanted, language) };
    std::array<otl::LayoutTable const*, 2> const table { &tables.gsub, &tables.gpos };

    PlanBuilder builder;
    builder.script = script;
    builder.direction = direction;
    for (std::size_t t = 0; t < 2; ++t) {
        builder.chosen_script[t] = selected[t].chosen;
        builder.found_script[t] = selected[t].found;
        plan->chosen_script[t] = selected[t].chosen;
        plan->found_script[t] = selected[t].found;
    }
    plan->shaper = &choose_shaper(script, selected[0].chosen);
    collect_features(builder, user);
    // The last stage of each table.
    builder.add_gsub_pause(nullptr);
    builder.add_gpos_pause(nullptr);

    // Merge the requests for one tag: a later global request decides; a
    // ranged one turns the feature into a masked one with the larger value.
    std::vector<PlanBuilder::Request> requests = builder.requests;
    std::stable_sort(requests.begin(), requests.end(), [](PlanBuilder::Request const& a, PlanBuilder::Request const& b) {
        return a.tag < b.tag || (a.tag == b.tag && a.seq < b.seq);
    });
    std::vector<PlanBuilder::Request> merged;
    for (PlanBuilder::Request const& r : requests) {
        if (merged.empty() || merged.back().tag != r.tag) {
            merged.push_back(r);
            continue;
        }
        PlanBuilder::Request& m = merged.back();
        if (r.flags & feature_flag::global) {
            m.flags |= feature_flag::global;
            m.max_value = r.max_value;
            m.default_value = r.default_value;
        } else {
            m.flags &= ~feature_flag::global;
            m.max_value = std::max(m.max_value, r.max_value);
        }
        m.flags |= r.flags & feature_flag::has_fallback;
        m.stage[0] = std::min(m.stage[0], r.stage[0]);
        m.stage[1] = std::min(m.stage[1], r.stage[1]);
    }

    // The required features of the selected language systems.
    std::array<int, 2> required_index { -1, -1 };
    std::array<std::uint32_t, 2> required_tag { 0, 0 };
    std::array<unsigned, 2> required_stage { 0, 0 };
    for (std::size_t t = 0; t < 2; ++t) {
        std::uint16_t const r = table[t]->required_feature(selected[t].lang_sys);
        if (r != 0xFFFF && r < table[t]->feature_count()) {
            required_index[t] = r;
            required_tag[t] = table[t]->feature_tag(r);
        }
    }

    // Mask bits: bit 0 is the global bit every glyph has; features with
    // values or ranges get bits of their own.
    constexpr unsigned global_shift = 0;
    constexpr std::uint32_t global_bit = 1u << global_shift;
    plan->global_mask = global_bit;
    unsigned next_bit = 1;
    for (PlanBuilder::Request const& r : merged) {
        bool const uses_global = (r.flags & feature_flag::global) && r.max_value == 1;
        unsigned const bits = uses_global ? 0 : bit_storage(r.max_value);
        if (!r.max_value || next_bit + bits >= 32)
            continue;
        FeatureMap map;
        bool found = false;
        for (std::size_t t = 0; t < 2; ++t) {
            if (required_tag[t] == r.tag)
                required_stage[t] = r.stage[t];
            int const index = table[t]->find_feature(selected[t].lang_sys, r.tag);
            map.index[t] = index;
            found = found || index >= 0;
        }
        if (!found && (r.flags & feature_flag::global_search)) {
            for (std::size_t t = 0; t < 2; ++t) {
                for (std::uint16_t k = 0; k < table[t]->feature_count(); ++k) {
                    if (table[t]->feature_tag(k) == r.tag) {
                        map.index[t] = k;
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found && !(r.flags & feature_flag::has_fallback))
            continue;
        map.tag = r.tag;
        map.stage = r.stage;
        map.auto_zwnj = !(r.flags & feature_flag::manual_zwnj);
        map.auto_zwj = !(r.flags & feature_flag::manual_zwj);
        map.random = (r.flags & feature_flag::random) != 0;
        map.per_syllable = (r.flags & feature_flag::per_syllable) != 0;
        if (uses_global) {
            map.shift = global_shift;
            map.mask = global_bit;
        } else {
            map.shift = next_bit;
            map.mask = static_cast<std::uint32_t>((1ull << (next_bit + bits)) - (1ull << next_bit));
            next_bit += bits;
            plan->global_mask |= (r.default_value << map.shift) & map.mask;
        }
        map.one_mask = (1u << map.shift) & map.mask;
        map.needs_fallback = !found;
        plan->features.push_back(map);
    }
    std::sort(plan->features.begin(), plan->features.end(), [](FeatureMap const& a, FeatureMap const& b) { return a.tag < b.tag; });

    // The lookups, stage by stage, each once, in lookup order.
    for (std::size_t t = 0; t < 2; ++t) {
        otl::LayoutTable const& lt = *table[t];
        std::vector<LookupMap>& lookups = plan->lookups[t];
        auto add_lookups = [&](int feature_index, std::uint32_t mask, bool auto_zwnj, bool auto_zwj, bool random,
                               bool per_syllable) {
            if (feature_index < 0)
                return;
            for (std::uint16_t index : lt.feature_lookups(static_cast<std::uint16_t>(feature_index), selected[t].variation)) {
                if (index >= lt.lookups.size())
                    continue;
                lookups.push_back(LookupMap { index, mask, auto_zwnj, auto_zwj, random, per_syllable });
            }
        };
        std::size_t pause = 0;
        for (unsigned stage = 0; stage < builder.current_stage[t]; ++stage) {
            std::size_t const stage_start = lookups.size();
            if (required_index[t] >= 0 && required_stage[t] == stage)
                add_lookups(required_index[t], global_bit, true, true, false, false);
            for (FeatureMap const& f : plan->features) {
                if (f.stage[t] == stage)
                    add_lookups(f.index[t], f.mask, f.auto_zwnj, f.auto_zwj, f.random, f.per_syllable);
            }
            // Sort this stage's lookups and merge the repeats.
            std::stable_sort(lookups.begin() + static_cast<std::ptrdiff_t>(stage_start), lookups.end(),
                [](LookupMap const& a, LookupMap const& b) { return a.index < b.index; });
            std::size_t write = stage_start;
            for (std::size_t read = stage_start; read < lookups.size(); ++read) {
                if (write > stage_start && lookups[write - 1].index == lookups[read].index) {
                    lookups[write - 1].mask |= lookups[read].mask;
                    lookups[write - 1].auto_zwnj = lookups[write - 1].auto_zwnj && lookups[read].auto_zwnj;
                    lookups[write - 1].auto_zwj = lookups[write - 1].auto_zwj && lookups[read].auto_zwj;
                    continue;
                }
                lookups[write++] = lookups[read];
            }
            lookups.resize(write);
            StageMap stage_map;
            stage_map.last_lookup = lookups.size();
            if (pause < builder.pauses[t].size() && builder.pauses[t][pause].stage == stage) {
                stage_map.pause = builder.pauses[t][pause].function;
                ++pause;
            }
            plan->stages[t].push_back(stage_map);
        }
    }

    // What the pipeline asks of the plan.
    plan->frac_mask = plan->mask(make_tag('f', 'r', 'a', 'c'));
    plan->numr_mask = plan->mask(make_tag('n', 'u', 'm', 'r'));
    plan->dnom_mask = plan->mask(make_tag('d', 'n', 'o', 'm'));
    plan->has_frac = plan->frac_mask || (plan->numr_mask && plan->dnom_mask);
    plan->rtlm_mask = plan->mask(make_tag('r', 't', 'l', 'm'));
    plan->kern_mask = plan->mask(make_tag('k', 'e', 'r', 'n'));
    plan->has_gpos_mark = plan->one_mask(make_tag('m', 'a', 'r', 'k')) != 0 && plan->has_feature_in_font(1, make_tag('m', 'a', 'r', 'k'));
    bool const has_gpos_kern = plan->has_feature_in_font(1, make_tag('k', 'e', 'r', 'n'));
    bool const disable_gpos = plan->shaper->gpos_tag && plan->shaper->gpos_tag != plan->chosen_script[1];
    plan->apply_gpos = tables.gpos.present() && !disable_gpos;
    // The `kern` table serves a font whose GPOS does not kern.
    plan->apply_kern = (!has_gpos_kern || !plan->apply_gpos) && tables.kern.size >= 4;
    plan->zero_marks = plan->shaper->zero_marks != ZeroMarks::None;
    plan->adjust_mark_positioning_when_zeroing = !plan->apply_gpos;
    plan->fallback_mark_positioning = plan->adjust_mark_positioning_when_zeroing && plan->shaper->fallback_position;
    plan->fallback_glyph_classes = !tables.gdef.has_glyph_classes();
    if (plan->shaper->init_plan)
        plan->shaper->init_plan(*plan);
    return plan;
}

std::string plan_key(ScriptCode script, std::uint32_t language, TextDirection direction, std::span<FeatureSetting const> user)
{
    std::string key;
    auto put = [&](std::uint32_t v) {
        for (int k = 0; k < 4; ++k)
            key.push_back(static_cast<char>(v >> (k * 8)));
    };
    put(script);
    put(language);
    key.push_back(static_cast<char>(direction));
    for (FeatureSetting const& f : user) {
        put(f.tag);
        put(f.value);
        key.push_back(f.global() ? 'g' : 'r');
    }
    return key;
}

// --- The pipeline ----------------------------------------------------------------

void setup_fraction_masks(Plan const& plan, Buffer& b)
{
    if (!plan.has_frac)
        return;
    bool const forward = plan.direction == TextDirection::LeftToRight;
    std::uint32_t const pre = forward ? plan.numr_mask | plan.frac_mask : plan.frac_mask | plan.dnom_mask;
    std::uint32_t const post = forward ? plan.frac_mask | plan.dnom_mask : plan.numr_mask | plan.frac_mask;
    std::size_t const count = b.len();
    for (std::size_t i = 0; i < count; ++i) {
        if (b.info[i].codepoint != 0x2044) // FRACTION SLASH
            continue;
        std::size_t start = i;
        std::size_t end = i + 1;
        while (start && static_cast<GeneralCategory>(b.info[start - 1].category) == GeneralCategory::Nd)
            --start;
        while (end < count && static_cast<GeneralCategory>(b.info[end].category) == GeneralCategory::Nd)
            ++end;
        if (start == i || end == i + 1)
            continue;
        for (std::size_t j = start; j < i; ++j)
            b.info[j].mask |= pre;
        b.info[i].mask |= plan.frac_mask;
        for (std::size_t j = i + 1; j < end; ++j)
            b.info[j].mask |= post;
        i = end - 1;
    }
}

void apply_stages(Plan const& plan, ShapeFont const& font, Buffer& b, int t)
{
    otl::LayoutTable const& table = t == 0 ? font.tables.gsub : font.tables.gpos;
    otl::ApplyContext c(font.tables, table, b);
    b.update_digest();
    std::size_t i = 0;
    for (StageMap const& stage : plan.stages[static_cast<std::size_t>(t)]) {
        for (; i < stage.last_lookup && !b.failed; ++i) {
            LookupMap const& l = plan.lookups[static_cast<std::size_t>(t)][i];
            c.lookup_mask = l.mask;
            c.auto_zwnj = l.auto_zwnj;
            c.auto_zwj = l.auto_zwj;
            c.random = l.random;
            c.per_syllable = l.per_syllable;
            c.apply_lookup(l.index);
        }
        if (stage.pause) {
            stage.pause(plan, font, b);
            b.update_digest();
        }
    }
}

void zero_mark_widths(Buffer& b, bool adjust_offsets)
{
    for (std::size_t i = 0; i < b.len(); ++i) {
        if (!otl::is_mark(b.info[i]))
            continue;
        if (adjust_offsets) {
            b.pos[i].x_offset -= b.pos[i].x_advance;
            b.pos[i].y_offset -= b.pos[i].y_advance;
        }
        b.pos[i].x_advance = 0;
        b.pos[i].y_advance = 0;
    }
}

void space_fallback_widths(ShapeFont const& font, Buffer& b)
{
    std::int32_t const em = font.font.units_per_em();
    for (std::size_t i = 0; i < b.len(); ++i) {
        std::uint8_t const type = b.info[i].space;
        if (type == NotSpace || type == SpacePlain)
            continue;
        GlyphPosition& p = b.pos[i];
        switch (type) {
        case SpaceEm:
        case SpaceEm2:
        case SpaceEm3:
        case SpaceEm4:
        case SpaceEm5:
        case SpaceEm6:
        case SpaceEm16:
            p.x_advance = (em + type / 2) / type;
            break;
        case Space4Em18:
            p.x_advance = static_cast<std::int32_t>(static_cast<std::int64_t>(em) * 4 / 18);
            break;
        case SpaceFigure:
            for (char32_t digit = U'0'; digit <= U'9'; ++digit) {
                if (std::uint32_t const glyph = font.nominal_glyph(digit)) {
                    p.x_advance = font.advance(glyph);
                    break;
                }
            }
            break;
        case SpacePunctuation: {
            std::uint32_t glyph = font.nominal_glyph(U'.');
            if (!glyph)
                glyph = font.nominal_glyph(U',');
            if (glyph)
                p.x_advance = font.advance(glyph);
            break;
        }
        case SpaceNarrow:
            p.x_advance /= 2;
            break;
        default:
            break;
        }
    }
}

// Default ignorables draw nothing: a zero-width space glyph in their place,
// or, in a font with no space, nothing at all.
void hide_default_ignorables(ShapeFont const& font, Buffer& b)
{
    bool any = false;
    for (GlyphInfo const& g : b.info)
        any = any || otl::is_default_ignorable(g);
    if (!any)
        return;
    if (std::uint32_t const space = font.nominal_glyph(0x20)) {
        for (GlyphInfo& g : b.info) {
            if (otl::is_default_ignorable(g))
                g.glyph = space;
        }
        return;
    }
    b.clear_output();
    std::vector<GlyphPosition> kept;
    for (b.idx = 0; b.idx < b.len();) {
        if (otl::is_default_ignorable(b.cur())) {
            b.delete_glyph();
            continue;
        }
        kept.push_back(b.pos[b.idx]);
        b.next_glyph();
    }
    b.sync();
    b.pos = std::move(kept);
}

void shape_simple(Face const& face, ShapeRequest const& r, ShapedRun& out)
{
    // The built-in face: one glyph per character at its advance, scaled to
    // a 1000-unit em.
    out.units_per_em = 1000;
    out.glyphs.clear();
    for (std::size_t i = 0; i < r.text.size(); ++i) {
        std::uint32_t const glyph = face.glyph_index(r.text[i]);
        ShapedGlyph g;
        g.glyph = glyph ? glyph : static_cast<std::uint32_t>(r.text[i]);
        g.cluster = static_cast<std::uint32_t>(i);
        g.x_advance = static_cast<std::int32_t>(face.advance(g.glyph, 1000.0f) + 0.5f);
        out.glyphs.push_back(g);
    }
    if (r.direction == TextDirection::RightToLeft)
        std::reverse(out.glyphs.begin(), out.glyphs.end());
}

}

void shape_run(FaceCache const& cache, ShapeRequest const& r, ShapedRun& out)
{
    out.glyphs.clear();
    out.units_per_em = cache.font.units_per_em();
    if (r.text.empty())
        return;
    std::uint32_t const language = opentype_language_tag(r.language);

    // The plan, made once per script, language, direction and features.
    Plan const* plan = nullptr;
    std::unique_ptr<Plan> uncached;
    std::string const key = plan_key(r.script, language, r.direction, r.features);
    // The last plan this thread used, by the cache's serial number (a cache
    // made later at the same address has another), spares the lock.
    struct LastPlan {
        std::uint64_t serial = 0;
        std::string key;
        Plan const* plan = nullptr;
    };
    thread_local LastPlan last;
    if (last.serial == cache.serial && last.key == key) {
        plan = last.plan;
    } else {
        std::lock_guard<std::mutex> const lock(cache.mutex);
        auto const it = cache.plans.find(key);
        if (it != cache.plans.end()) {
            plan = it->second.get();
        } else {
            std::unique_ptr<Plan> made = compile_plan(cache.tables, r.script, language, r.direction, r.features);
            if (cache.plans.size() < FaceCache::plan_bound) {
                plan = made.get();
                cache.plans.emplace(key, std::move(made));
            } else {
                uncached = std::move(made);
                plan = uncached.get();
            }
        }
        if (!uncached)
            last = LastPlan { cache.serial, key, plan };
    }
    ShapeFont const font { cache.font, cache.tables };
    ScriptShaper const& shaper = *plan->shaper;

    thread_local Buffer b;
    b.info.clear();
    b.begin(r.text.size());
    b.info.resize(r.text.size());
    for (std::size_t i = 0; i < r.text.size(); ++i) {
        GlyphInfo& g = b.info[i];
        g = GlyphInfo {};
        g.codepoint = r.text[i];
        g.cluster = static_cast<std::uint32_t>(i);
        g.mask = plan->global_mask;
    }
    bool const target_rtl = r.direction == TextDirection::RightToLeft;
    b.right_to_left = target_rtl;

    set_all_unicode_props(b);
    if (r.beginning_of_text && r.before.empty())
        insert_dotted_circle(b, font);
    bool any_continuation = false;
    for (GlyphInfo const& g : b.info)
        any_continuation = any_continuation || is_continuation(g);
    if (any_continuation)
        form_clusters(b);
    // A run against its script's direction is shaped in the script's.
    if (r.script != scripts::common && r.script != scripts::inherited && r.script != scripts::unknown
        && script_is_right_to_left(r.script) != target_rtl) {
        reverse_graphemes(b);
        b.right_to_left = !target_rtl;
    }
    if (shaper.preprocess_text)
        shaper.preprocess_text(*plan, font, b);

    // Mirroring (UAX #9 L4, with the font's 'rtlm' for what has no
    // mirrored character).
    if (target_rtl) {
        for (GlyphInfo& g : b.info) {
            char32_t const mirrored = bidi_mirrored(g.codepoint);
            if (mirrored != g.codepoint && font.has_glyph(mirrored))
                g.codepoint = mirrored;
            else
                g.mask |= plan->rtlm_mask;
        }
    }

    Normalizer { *plan, font, b }.run(shaper.normalization);

    setup_fraction_masks(*plan, b);
    if (shaper.setup_masks)
        shaper.setup_masks(*plan, font, b, r.before, r.after);
    for (FeatureSetting const& f : r.features) {
        if (f.global())
            continue;
        unsigned shift = 0;
        std::uint32_t const mask = plan->mask(f.tag, &shift);
        if (!mask)
            continue;
        std::uint32_t const value = (f.value << shift) & mask;
        for (GlyphInfo& g : b.info) {
            if (g.cluster >= f.start && g.cluster < f.end)
                g.mask = (g.mask & ~mask) | value;
        }
    }
    if (plan->fallback_mark_positioning)
        fallback_recategorize_marks(*plan, b);

    // GSUB: every glyph's class from GDEF (or from its character when the
    // font has no classes), then the stages.
    if (!plan->fallback_glyph_classes) {
        for (GlyphInfo& g : b.info) {
            g.props = font.tables.gdef.props_of(g.glyph);
            g.lig_props = 0;
        }
    } else {
        for (GlyphInfo& g : b.info) {
            bool const mark = static_cast<GeneralCategory>(g.category) == GeneralCategory::Mn && !otl::is_default_ignorable(g);
            g.props = mark ? otl::glyph_props::mark : otl::glyph_props::base_glyph;
            g.lig_props = 0;
        }
    }
    apply_stages(*plan, font, b, 0);

    // Positioning: the advances, then GPOS (or the `kern` table), the marks
    // zeroed, the ignorables zeroed, the attachments resolved.
    b.pos.assign(b.len(), GlyphPosition {});
    for (std::size_t i = 0; i < b.len(); ++i)
        b.pos[i].x_advance = font.advance(b.info[i].glyph);
    space_fallback_widths(font, b);
    bool const forward = !b.right_to_left;
    bool const adjust_offsets = plan->adjust_mark_positioning_when_zeroing && forward;
    if (plan->zero_marks && shaper.zero_marks == ZeroMarks::ByGdefEarly)
        zero_mark_widths(b, adjust_offsets);
    if (plan->apply_gpos)
        apply_stages(*plan, font, b, 1);
    if (plan->apply_kern)
        apply_kern_table(*plan, font, b);
    if (plan->zero_marks && shaper.zero_marks == ZeroMarks::ByGdefLate)
        zero_mark_widths(b, adjust_offsets);
    for (std::size_t i = 0; i < b.len(); ++i) {
        if (otl::is_default_ignorable(b.info[i]))
            b.pos[i] = GlyphPosition {};
    }
    otl::resolve_attachments(b);
    if (plan->fallback_mark_positioning)
        fallback_position_marks(*plan, font, b, adjust_offsets);
    hide_default_ignorables(font, b);
    if (shaper.postprocess_glyphs)
        shaper.postprocess_glyphs(*plan, font, b);

    if (target_rtl)
        b.reverse();
    out.glyphs.resize(b.len());
    for (std::size_t i = 0; i < b.len(); ++i) {
        ShapedGlyph& g = out.glyphs[i];
        g.glyph = b.info[i].glyph;
        g.cluster = b.info[i].cluster;
        g.x_advance = b.pos[i].x_advance;
        g.y_advance = b.pos[i].y_advance;
        g.x_offset = b.pos[i].x_offset;
        g.y_offset = b.pos[i].y_offset;
    }
}

}

void shape(Face const& face, ShapeRequest const& request, ShapedRun& out)
{
    shaping::FaceCache const* cache = face.shaping_cache();
    if (!cache) {
        shaping::shape_simple(face, request, out);
        return;
    }
    shaping::shape_run(*cache, request, out);
}

ShapedRun shape(Face const& face, ShapeRequest const& request)
{
    ShapedRun out;
    shape(face, request, out);
    return out;
}

void shaping_features_for_css(CssFontFeatures const& css, std::vector<FeatureSetting>& out)
{
    using otl::make_tag;
    if (css.kerning == CssFontFeatures::Kerning::None)
        out.push_back(FeatureSetting { make_tag('k', 'e', 'r', 'n'), 0 });
    bool const optional_off = css.letter_spacing;
    if (!css.common_ligatures || optional_off) {
        out.push_back(FeatureSetting { make_tag('l', 'i', 'g', 'a'), 0 });
        out.push_back(FeatureSetting { make_tag('c', 'l', 'i', 'g'), 0 });
    }
    if (css.discretionary_ligatures && !optional_off)
        out.push_back(FeatureSetting { make_tag('d', 'l', 'i', 'g'), 1 });
    if (css.historical_ligatures && !optional_off)
        out.push_back(FeatureSetting { make_tag('h', 'l', 'i', 'g'), 1 });
    if (!css.contextual_alternates)
        out.push_back(FeatureSetting { make_tag('c', 'a', 'l', 't'), 0 });
}

shaping::FaceCache const* Face::shaping_cache() const
{
    Face const& owner = layout_face();
    if (&owner != this)
        return owner.shaping_cache();
    TrueTypeFont const* const font = truetype_font();
    if (!font)
        return nullptr;
    std::call_once(m_shaping_once, [&] { m_shaping = std::make_shared<shaping::FaceCache const>(*font); });
    return m_shaping.get();
}

}
