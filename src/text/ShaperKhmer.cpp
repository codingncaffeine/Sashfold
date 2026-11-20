// The Khmer shaper, as Microsoft's "Developing OpenType Fonts for Khmer
// Script" describes it. A syllable is a consonant (or independent vowel)
// with up to two subscripts written as COENG + consonant, then one vowel,
// a register shifter and signs. Before the features run, the subscript
// COENG + RO is moved before the base (it is drawn there, its 'pref'
// form), the split vowels are given their pre-base part (U+17C1), and the
// pre-base vowel is moved before everything; the language forms run one
// at a time within the syllable, then the presentation forms.

#include "text/ShaperInternal.h"
#include "text/ShaperSyllabic.h"

#include "text/CharacterProperties.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::text::shaping {

namespace {

using otl::make_tag;

// §Character classes of the specification's chart.
enum Category : std::uint8_t {
    X, // other
    C, // consonant, independent vowel
    Ra, // RO, the subscript of type 2
    Coeng,
    VPre,
    VAbv,
    VBlw,
    VPst,
    Rs, // register shifter
    Robat,
    SAbv, // above-base sign
    SPst, // post-base sign
    ZWJ,
    ZWNJ,
    Placeholder, // NBSP and the like, a base for marks written alone
    DottedCircle,
    Number, // begins a syllable, has no subscript form
};

Category classify(char32_t c)
{
    if (c >= 0x1780 && c <= 0x17B3)
        return c == 0x179A ? Ra : C;
    if (c == 0x17D2)
        return Coeng;
    if ((c >= 0x17B7 && c <= 0x17BA) || c == 0x17BE)
        return VAbv;
    if (c >= 0x17BB && c <= 0x17BD)
        return VBlw;
    if (c >= 0x17C1 && c <= 0x17C3)
        return VPre;
    if (c == 0x17B6 || c == 0x17BF || c == 0x17C0 || c == 0x17C4 || c == 0x17C5)
        return VPst;
    if (c == 0x17C9 || c == 0x17CA)
        return Rs;
    if (c == 0x17CC)
        return Robat;
    if (c == 0x17C6 || c == 0x17CB || (c >= 0x17CD && c <= 0x17D1))
        return SAbv;
    // U+17D3 and U+17DD, above-base signs, may follow the post-base signs
    // as they follow each other.
    if (c == 0x17C7 || c == 0x17C8 || c == 0x17D3 || c == 0x17DD)
        return SPst;
    // §Analyze syllables: a syllable may begin with a number.
    if (c >= 0x17E0 && c <= 0x17E9)
        return Number;
    if (c == 0x200D)
        return ZWJ;
    if (c == 0x200C)
        return ZWNJ;
    if (c == 0x25CC)
        return DottedCircle;
    if (c == 0x00A0 || (c >= 0x2010 && c <= 0x2014) || c == 0x2022 || c == 0x25FB || c == 0x25FC || c == 0x25FD
        || c == 0x25FE)
        return Placeholder;
    return X;
}

Category category(GlyphInfo const& g) { return static_cast<Category>(g.shaper); }

enum SyllableType : unsigned {
    ConsonantSyllable,
    BrokenCluster,
    NonKhmerCluster,
};

constexpr std::pair<std::string_view, std::uint8_t> category_names[] = {
    { "X", X }, { "C", C }, { "Ra", Ra }, { "Coeng", Coeng }, { "VPre", VPre }, { "VAbv", VAbv }, { "VBlw", VBlw },
    { "VPst", VPst }, { "Rs", Rs }, { "Robat", Robat }, { "SAbv", SAbv }, { "SPst", SPst }, { "ZWJ", ZWJ },
    { "ZWNJ", ZWNJ }, { "Placeholder", Placeholder }, { "DottedCircle", DottedCircle }, { "Number", Number },
};

// §Analyze syllables: Cons + {COENG + (Cons | IndV)} + [PreV | BlwV] +
// [RegShift] + [AbvV] + {AbvS} + [PstV] + [PstS], as engines in browsers
// read it (settled by comparing outputs over every pair of classes): the
// register shifters and the robat follow a consonant directly (two at
// most), the above-base signs may stand between the vowels, a subscript
// may follow the vowels, a joiner may come before an above-base vowel, a
// register shifter or a sign, and the subscripts may end in a bare COENG.
#define KHMER_J "[ZWJ ZWNJ]"
#define KHMER_CN "[C Ra] (" KHMER_J "? [Rs Robat])? (" KHMER_J "? [Rs Robat])?"
#define KHMER_X "(" KHMER_J "* SAbv)*"
#define KHMER_MATRA "VPre? " KHMER_X " VBlw? " KHMER_X " (" KHMER_J "? VAbv)? " KHMER_X " VPst?"
#define KHMER_TAIL KHMER_X " " KHMER_MATRA " " KHMER_X " (Coeng [C Ra])? SPst*"
#define KHMER_SUBSCRIPTS "(Coeng " KHMER_CN ")* (Coeng | " KHMER_TAIL ")"

struct Grammar {
    CategoryPattern consonant {
        "[C Ra Number Placeholder DottedCircle] (" KHMER_J "? [Rs Robat])? (" KHMER_J "? [Rs Robat])? " KHMER_SUBSCRIPTS,
        category_names
    };
    CategoryPattern broken { "[Rs Robat]? " KHMER_SUBSCRIPTS, category_names };
    std::array<SyllableRule, 2> rules { {
        { &consonant, ConsonantSyllable },
        { &broken, BrokenCluster },
    } };
};

Grammar const& grammar()
{
    static Grammar const g;
    return g;
}

// --- The plan -----------------------------------------------------------------------------

enum Feature : std::size_t {
    Pref,
    Blwf,
    Abvf,
    Pstf,
    Cfar,
    FeatureCount,
};
constexpr std::array<std::uint32_t, FeatureCount> feature_tags {
    make_tag('p', 'r', 'e', 'f'),
    make_tag('b', 'l', 'w', 'f'),
    make_tag('a', 'b', 'v', 'f'),
    make_tag('p', 's', 't', 'f'),
    make_tag('c', 'f', 'a', 'r'),
};

struct KhmerPlan {
    std::array<std::uint32_t, FeatureCount> mask {};
};

KhmerPlan const& khmer_plan(Plan const& plan) { return *static_cast<KhmerPlan const*>(plan.shaper_data.get()); }

void reorder(Plan const&, ShapeFont const&, Buffer&);

void collect_features(PlanBuilder& p)
{
    std::uint32_t const joiners = feature_flag::manual_zwj | feature_flag::manual_zwnj | feature_flag::per_syllable;
    p.add_gsub_pause(reorder);
    p.enable_feature(make_tag('l', 'o', 'c', 'l'), feature_flag::per_syllable);
    p.enable_feature(make_tag('c', 'c', 'm', 'p'), feature_flag::per_syllable);
    // §Shaping features: the language forms one by one, in this order.
    for (std::uint32_t tag : feature_tags) {
        p.add_feature(tag, joiners);
        p.add_gsub_pause(nullptr);
    }
    // §Conjuncts and typographical forms.
    for (std::uint32_t tag : { make_tag('p', 'r', 'e', 's'), make_tag('a', 'b', 'v', 's'), make_tag('b', 'l', 'w', 's'),
             make_tag('p', 's', 't', 's') })
        p.enable_feature(tag, joiners);
}

void init_plan(Plan& plan)
{
    auto data = std::make_shared<KhmerPlan>();
    for (std::size_t k = 0; k < FeatureCount; ++k)
        data->mask[k] = plan.mask(feature_tags[k]);
    plan.shaper_data = std::move(data);
}

void setup_masks(Plan const&, ShapeFont const&, Buffer& b, std::u32string_view, std::u32string_view)
{
    for (GlyphInfo& g : b.info)
        g.shaper = classify(g.codepoint);
    find_syllables(b, grammar().rules, NonKhmerCluster, [](GlyphInfo const& g) { return g.shaper; });
    // A Khmer mark no syllable takes is a broken cluster of its own.
    for (GlyphInfo& g : b.info) {
        if (syllable_type(g) == NonKhmerCluster && category(g) != X && is_unicode_mark(g))
            g.syllable = static_cast<std::uint8_t>((g.syllable & 0xF0) | BrokenCluster);
    }
}

// --- Reordering ---------------------------------------------------------------------------

void move_glyph(std::vector<GlyphInfo>& info, std::size_t from, std::size_t to)
{
    GlyphInfo const moved = info[from];
    if (from < to)
        std::copy(info.begin() + static_cast<std::ptrdiff_t>(from + 1), info.begin() + static_cast<std::ptrdiff_t>(to + 1),
            info.begin() + static_cast<std::ptrdiff_t>(from));
    else
        std::copy_backward(info.begin() + static_cast<std::ptrdiff_t>(to), info.begin() + static_cast<std::ptrdiff_t>(from),
            info.begin() + static_cast<std::ptrdiff_t>(from + 1));
    info[to] = moved;
}

// §Analyze syllables and reorder characters: every glyph of the syllable
// gets the language forms' masks but 'pref', which only COENG + RO gets
// (moved before the base, the rest of the syllable then takes 'cfar');
// the pre-base vowel goes first.
void reorder_syllable(KhmerPlan const& khmer, Buffer& b, std::size_t start, std::size_t end)
{
    std::vector<GlyphInfo>& info = b.info;
    std::uint32_t const forms = khmer.mask[Blwf] | khmer.mask[Abvf] | khmer.mask[Pstf];
    for (std::size_t i = start; i < end; ++i)
        info[i].mask |= forms;
    std::size_t const base = start;
    for (std::size_t i = base + 1; i + 1 < end; ++i) {
        if (category(info[i]) == Coeng && category(info[i + 1]) == Ra) {
            info[i].mask |= khmer.mask[Pref];
            info[i + 1].mask |= khmer.mask[Pref];
            b.merge_clusters(base, i + 2);
            move_glyph(info, i, base);
            move_glyph(info, i + 1, base + 1);
            for (std::size_t k = i + 2; k < end; ++k)
                info[k].mask |= khmer.mask[Cfar];
            break;
        }
    }
    for (std::size_t i = start + 1; i < end; ++i) {
        if (category(info[i]) == VPre) {
            b.merge_clusters(start, i + 1);
            move_glyph(info, i, start);
        }
    }
}

void reorder(Plan const& plan, ShapeFont const& font, Buffer& b)
{
    KhmerPlan const& khmer = khmer_plan(plan);
    insert_dotted_circles(font, b, BrokenCluster, DottedCircle, 0, plan.global_mask, [](GlyphInfo const&) { return false; });
    for (std::size_t start = 0; start < b.len();) {
        std::size_t const end = syllable_end(b, start);
        if (syllable_type(b.info[start]) != NonKhmerCluster)
            reorder_syllable(khmer, b, start, end);
        start = end;
    }
}

// --- Normalization --------------------------------------------------------------------------

// The split vowels are written with their pre-base part (U+17C1) first.
bool decompose(Plan const&, char32_t ab, char32_t& a, char32_t& b)
{
    switch (ab) {
    case 0x17BE:
    case 0x17BF:
    case 0x17C0:
    case 0x17C4:
    case 0x17C5:
        a = 0x17C1;
        b = ab;
        return true;
    default:
        return canonical_decompose(ab, a, b);
    }
}

bool compose(Plan const&, char32_t a, char32_t b, char32_t& ab)
{
    if (is_mark_category(general_category(a)))
        return false;
    ab = canonical_compose(a, b);
    return ab != 0;
}

}

ScriptShaper const khmer_shaper {
    .collect_features = collect_features,
    .override_features = nullptr,
    .init_plan = init_plan,
    .preprocess_text = nullptr,
    .postprocess_glyphs = nullptr,
    .normalization = Normalization::ComposedDiacriticsNoShortCircuit,
    .decompose = decompose,
    .compose = compose,
    .setup_masks = setup_masks,
    .reorder_marks = nullptr,
    .gpos_tag = 0,
    .zero_marks = ZeroMarks::None,
    .fallback_position = false,
};

}
