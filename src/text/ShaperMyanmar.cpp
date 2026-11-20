// The Myanmar shaper (Burmese, Shan, Mon and the other languages of the
// script), as Microsoft's "Developing OpenType Fonts for Myanmar Script"
// describes it, for fonts with the 'mym2' script tag. Clusters follow the
// specification's well-formed clusters; before the features run, a kinzi
// is moved after the base, medial ra before it, the pre-base vowels to the
// front and an anusvara before the below-base vowels it follows. The basic
// shaping forms run one at a time ('rphf' on the kinzi, 'pref' on medial
// ra, 'blwf' and 'pstf' after the base), then the presentation forms.

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

// §Analyzing the characters: the classes of the specification's table.
enum Category : std::uint8_t {
    O, // other, punctuation, symbols, white space: a cluster of one
    C, // consonant
    Ra, // the consonants a kinzi is made of: nga, ra, Mon nga
    IV, // independent vowel
    D, // digit but zero
    GB, // generic base
    DottedCircle,
    A, // anusvara
    As, // asat
    DB, // dot below
    H, // virama
    MH,
    MR,
    MW,
    MY,
    PT, // Pwo and other tones
    V, // visarga, Shan tones
    VAbv,
    VBlw,
    VPre,
    VPst,
    VS,
    ZWJ,
    ZWNJ,
};

Category classify(char32_t c)
{
    switch (c) {
    case 0x1004:
    case 0x101B:
    case 0x105A:
        return Ra;
    case 0x1032:
    case 0x1036:
        return A;
    case 0x103A:
        return As;
    case 0x1037:
        return DB;
    case 0x1039:
        return H;
    case 0x103E:
    case 0x1060:
        return MH;
    case 0x103C:
        return MR;
    case 0x103D:
    case 0x1082:
        return MW;
    case 0x103B:
    case 0x105E:
    case 0x105F:
        return MY;
    case 0x1031:
    case 0x1084:
        return VPre;
    case 0x102F:
    case 0x1030:
    case 0x1058:
    case 0x1059:
        return VBlw;
    case 0x102B:
    case 0x102C:
    case 0x1056:
    case 0x1057:
    case 0x1062:
    case 0x1067:
    case 0x1068:
    case 0x1083:
        return VPst;
    case 0x102D:
    case 0x102E:
    case 0x1033:
    case 0x1034:
    case 0x1035:
    case 0x1085:
    case 0x1086:
    case 0x109D:
        return VAbv;
    case 0x1038:
    case 0x108F:
        return V;
    case 0x1063:
    case 0x1064:
    case 0xAA7B:
        return PT;
    case 0x00A0:
    case 0x00D7:
    case 0x2022:
        return GB;
    case 0x25CC:
        return DottedCircle;
    case 0x200C:
        return ZWNJ;
    case 0x200D:
        return ZWJ;
    default:
        break;
    }
    if ((c >= 0x1000 && c <= 0x1020) || c == 0x103F || c == 0x104E || c == 0x1050 || c == 0x1051
        || (c >= 0x105B && c <= 0x105D) || c == 0x1061 || c == 0x1065 || c == 0x1066 || (c >= 0x106E && c <= 0x1070)
        || (c >= 0x1075 && c <= 0x1081) || c == 0x108E || (c >= 0xAA60 && c <= 0xAA6F) || (c >= 0xAA71 && c <= 0xAA76)
        || c == 0xAA7A)
        return C;
    if ((c >= 0x1021 && c <= 0x102A) || (c >= 0x1052 && c <= 0x1055))
        return IV;
    if ((c >= 0x1041 && c <= 0x1049) || (c >= 0x1090 && c <= 0x1099))
        return D;
    if ((c >= 0x1071 && c <= 0x1074))
        return VAbv;
    if ((c >= 0x1069 && c <= 0x106D))
        return PT;
    if ((c >= 0x1087 && c <= 0x108D) || (c >= 0x109A && c <= 0x109C))
        return V;
    if ((c >= 0x2012 && c <= 0x2015) || (c >= 0x25FB && c <= 0x25FE))
        return GB;
    // Myanmar Extended-B, encoded after the specification's table, by its
    // Indic syllabic categories: the Shan and Tai Laing consonants, digits
    // and vowel sign.
    if ((c >= 0xA9E0 && c <= 0xA9E4) || (c >= 0xA9E7 && c <= 0xA9EF) || (c >= 0xA9FA && c <= 0xA9FE))
        return C;
    // The characters since assigned in the table's reserved range: two
    // tone marks (with the other tones) and two consonants. Digit zero
    // takes marks as the other digits do (as engines in browsers have it).
    if (c == 0xAA7C || c == 0xAA7D)
        return PT;
    if (c == 0xAA7E || c == 0xAA7F)
        return C;
    if (c == 0x1040)
        return D;
    if (c >= 0xA9F0 && c <= 0xA9F9)
        return D;
    if (c == 0xA9E5)
        return VAbv;
    if (c >= 0xFE00 && c <= 0xFE0F)
        return VS;
    return O;
}

Category category(GlyphInfo const& g) { return static_cast<Category>(g.shaper); }

enum SyllableType : unsigned {
    ConsonantSyllable,
    BrokenCluster,
    NonMyanmarCluster,
};

constexpr std::pair<std::string_view, std::uint8_t> category_names[] = {
    { "O", O }, { "C", C }, { "Ra", Ra }, { "IV", IV }, { "D", D }, { "GB", GB }, { "DottedCircle", DottedCircle },
    { "A", A }, { "As", As }, { "DB", DB }, { "H", H }, { "MH", MH }, { "MR", MR }, { "MW", MW }, { "MY", MY },
    { "PT", PT }, { "V", V }, { "VAbv", VAbv }, { "VBlw", VBlw }, { "VPre", VPre }, { "VPst", VPst }, { "VS", VS },
    { "ZWJ", ZWJ }, { "ZWNJ", ZWNJ },
};

// §Well-formed clusters: the cluster terminating in halant and the complex
// cluster, after an optional kinzi; what follows the base with no base is
// a broken cluster.
#define MYANMAR_KINZI "(Ra As H)?"
#define MYANMAR_BASE "[C Ra IV D GB DottedCircle] VS? (H [C Ra IV] VS?)*"
#define MYANMAR_TAIL                                                                                                  \
    "As* (MY As?)? MR? (MW As? | MW? MH As?)? VPre* VAbv* VBlw* A* (DB As?)? (VPst MH? As* VAbv* A* (DB As?)?)*"      \
    " (PT (A? DB? As? | As? A?))* V*"

struct Grammar {
    CategoryPattern halant { MYANMAR_KINZI " " MYANMAR_BASE " H [ZWJ ZWNJ]?", category_names };
    CategoryPattern complex { MYANMAR_KINZI " " MYANMAR_BASE " " MYANMAR_TAIL " [ZWJ ZWNJ]?", category_names };
    CategoryPattern broken { MYANMAR_TAIL, category_names };
    std::array<SyllableRule, 3> rules { {
        { &halant, ConsonantSyllable },
        { &complex, ConsonantSyllable },
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
    Rphf,
    Pref,
    Blwf,
    Pstf,
    FeatureCount,
};
constexpr std::array<std::uint32_t, FeatureCount> feature_tags {
    make_tag('r', 'p', 'h', 'f'),
    make_tag('p', 'r', 'e', 'f'),
    make_tag('b', 'l', 'w', 'f'),
    make_tag('p', 's', 't', 'f'),
};

struct MyanmarPlan {
    std::array<std::uint32_t, FeatureCount> mask {};
};

MyanmarPlan const& myanmar_plan(Plan const& plan) { return *static_cast<MyanmarPlan const*>(plan.shaper_data.get()); }

void reorder(Plan const&, ShapeFont const&, Buffer&);

void collect_features(PlanBuilder& p)
{
    std::uint32_t const joiners = feature_flag::manual_zwj | feature_flag::manual_zwnj | feature_flag::per_syllable;
    p.enable_feature(make_tag('l', 'o', 'c', 'l'), feature_flag::per_syllable);
    p.enable_feature(make_tag('c', 'c', 'm', 'p'), feature_flag::per_syllable);
    p.add_gsub_pause(reorder);
    // §Basic shaping forms, one at a time.
    for (std::uint32_t tag : feature_tags) {
        p.add_feature(tag, joiners);
        p.add_gsub_pause(nullptr);
    }
    // §Presentation forms, over the whole run (§Analyzing the characters:
    // "GSUB and GPOS features are applied to the entire run").
    for (std::uint32_t tag : { make_tag('p', 'r', 'e', 's'), make_tag('a', 'b', 'v', 's'), make_tag('b', 'l', 'w', 's'),
             make_tag('p', 's', 't', 's') })
        p.enable_feature(tag, feature_flag::manual_zwj | feature_flag::manual_zwnj);
}

void init_plan(Plan& plan)
{
    auto data = std::make_shared<MyanmarPlan>();
    for (std::size_t k = 0; k < FeatureCount; ++k)
        data->mask[k] = plan.mask(feature_tags[k]);
    plan.shaper_data = std::move(data);
}

void setup_masks(Plan const&, ShapeFont const&, Buffer& b, std::u32string_view, std::u32string_view)
{
    for (GlyphInfo& g : b.info)
        g.shaper = classify(g.codepoint);
    find_syllables(b, grammar().rules, NonMyanmarCluster, [](GlyphInfo const& g) { return g.shaper; });
    // A mark no cluster takes is a broken cluster of its own.
    for (GlyphInfo& g : b.info) {
        if (syllable_type(g) == NonMyanmarCluster && category(g) != O && category(g) != VS && is_unicode_mark(g))
            g.syllable = static_cast<std::uint8_t>((g.syllable & 0xF0) | BrokenCluster);
    }
}

// --- Reordering ---------------------------------------------------------------------------

// Where each glyph of a syllable goes, in the order of the specification's
// reordering: the pre-base vowels, medial ra, the base, the kinzi, the
// rest in their order with an anusvara before the below-base vowels it
// follows.
enum Position : std::uint8_t {
    PosVPre,
    PosMR,
    PosBase,
    PosKinzi,
    PosAfter,
};

void reorder_syllable(MyanmarPlan const& myanmar, Buffer& b, std::size_t start, std::size_t end)
{
    std::vector<GlyphInfo>& info = b.info;
    std::size_t base = start;
    bool kinzi = false;
    if (end - start >= 4 && category(info[start]) == Ra && category(info[start + 1]) == As
        && category(info[start + 2]) == H) {
        kinzi = true;
        base = start + 3;
    }
    // A syllable a font's composition features multiplied past the bound
    // syllables are found with is left in its order: the sort below is
    // quadratic in its length.
    if (base >= end || end - start > max_syllable_length)
        return;
    for (std::size_t i = start; i < end; ++i) {
        Position p = PosAfter;
        if (kinzi && i < base)
            p = PosKinzi;
        else if (i == base)
            p = PosBase;
        else if (category(info[i]) == VPre)
            p = PosVPre;
        else if (category(info[i]) == MR)
            p = PosMR;
        info[i].shaper2 = p;
    }
    // An anusvara right after below-base vowels goes before them.
    for (std::size_t i = start + 1; i < end; ++i) {
        if (category(info[i]) != A || category(info[i - 1]) != VBlw)
            continue;
        std::size_t first = i - 1;
        while (first > start && category(info[first - 1]) == VBlw)
            --first;
        b.merge_clusters(first, i + 1);
        GlyphInfo const moved = info[i];
        for (std::size_t k = i; k > first; --k)
            info[k] = info[k - 1];
        info[first] = moved;
    }
    b.sort(start, end, [](GlyphInfo const& x, GlyphInfo const& y) { return x.shaper2 < y.shaper2; });
    // 'rphf' for the kinzi, 'pref' for medial ra; the below-base and
    // post-base forms are looked for over the whole syllable (a form may be
    // a ligature with the base).
    for (std::size_t i = start; i < end; ++i) {
        info[i].mask |= myanmar.mask[Blwf] | myanmar.mask[Pstf];
        if (info[i].shaper2 == PosKinzi)
            info[i].mask |= myanmar.mask[Rphf];
        else if (info[i].shaper2 == PosMR)
            info[i].mask |= myanmar.mask[Pref];
    }
}

void reorder(Plan const& plan, ShapeFont const& font, Buffer& b)
{
    MyanmarPlan const& myanmar = myanmar_plan(plan);
    insert_dotted_circles(font, b, BrokenCluster, DottedCircle, 0, plan.global_mask, [](GlyphInfo const&) { return false; });
    for (std::size_t start = 0; start < b.len();) {
        std::size_t const end = syllable_end(b, start);
        if (syllable_type(b.info[start]) != NonMyanmarCluster)
            reorder_syllable(myanmar, b, start, end);
        start = end;
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

ScriptShaper const myanmar_shaper {
    .collect_features = collect_features,
    .override_features = nullptr,
    .init_plan = init_plan,
    .preprocess_text = nullptr,
    .postprocess_glyphs = nullptr,
    .normalization = Normalization::ComposedDiacriticsNoShortCircuit,
    .decompose = nullptr,
    .compose = compose,
    .setup_masks = setup_masks,
    .reorder_marks = nullptr,
    .gpos_tag = 0,
    .zero_marks = ZeroMarks::ByGdefEarly,
    .fallback_position = false,
};

}
