// The Indic shaper: Devanagari, Bengali, Gurmukhi, Gujarati, Oriya, Tamil,
// Telugu, Kannada and Malayalam, as the OpenType script development
// specifications for them describe ("Developing OpenType Fonts for
// <script>", both the original 'deva'-style and the second 'dev2'-style
// script tags). Text is split into syllables by the specifications' cluster
// grammar; each syllable is reordered into the order its glyphs are drawn
// in (the base consonant found, the reph and the pre-base matras set
// aside), the basic shaping features run one at a time, each limited to
// the syllable and to the characters the reordering marked for it; then
// the final reordering puts the pre-base matra, the reph and the pre-base
// reordering consonant where the formed glyphs say they go, and the
// presentation and positioning features run.
//
// Where the specifications leave a choice open (which categories a few
// characters take, where each script sorts its matras, how a joiner after
// a virama moves a matra, which clusters merge), the behavior is the one
// shaping engines in browsers show, settled by comparing outputs.

#include "text/ShaperSyllabic.h"

#include "text/ShapingData.h"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace sashfold::text::shaping {

namespace {

using otl::make_tag;
using shaping_data::IndicPosition;
using shaping_data::IndicSyllabic;

// Character categories of the cluster grammar.
enum Category : std::uint8_t {
    X = 0, // other
    C, // consonant
    V, // independent vowel
    N, // nukta
    H, // virama
    ZWNJ,
    ZWJ,
    M, // dependent vowel (matra)
    SM, // syllable modifier: bindu, visarga
    A, // vedic sign
    PLACEHOLDER,
    DOTTEDCIRCLE,
    RS, // register shifter
    Repha, // a reph written as a character of its own
    Ra,
    CM, // consonant medial
    Symbol,
    CS, // consonant with stacker
    MPst, // a post-base matra a syllable modifier may come before (Gurmukhi ii)
};

// The order the initial reordering sorts a syllable into.
enum Position : std::uint8_t {
    Start,
    RaToBecomeReph,
    PreM,
    PreC,
    BaseC,
    AfterMain,
    AboveC,
    BeforeSub,
    BelowC,
    AfterSub,
    BeforePost,
    PostC,
    AfterPost,
    SMVD,
    End,
};

enum SyllableType : unsigned {
    ConsonantSyllable,
    VowelSyllable,
    StandaloneCluster,
    SymbolCluster,
    BrokenCluster,
    NonIndicCluster,
};

enum class RephPosition : std::uint8_t {
    AfterMain,
    BeforeSub,
    AfterSub,
    BeforePost,
    AfterPost,
};

enum class RephMode : std::uint8_t {
    Implicit, // ra + virama at the start forms a reph
    Explicit, // ra + virama + ZWJ forms it
    Logical, // the reph is a character of its own (Malayalam dot reph)
};

enum class BlwfMode : std::uint8_t {
    PreAndPost, // below-base forms both before and after the base
    PostOnly,
};

enum class BasePosition : std::uint8_t {
    Last, // the last consonant without a below-base or post-base form
};

struct ScriptConfig {
    ScriptCode script;
    char32_t virama;
    bool has_old_spec;
    BasePosition base;
    RephPosition reph_position;
    RephMode reph_mode;
    BlwfMode blwf_mode;
};

// The per-script rules of the specifications: where each script's reph
// goes, how it is written, where below-base forms occur, and whether the
// script had fonts made for the original specification.
constexpr ScriptConfig script_configs[] = {
    { script_code('D', 'e', 'v', 'a'), 0x094D, true, BasePosition::Last, RephPosition::BeforePost, RephMode::Implicit,
        BlwfMode::PreAndPost },
    { script_code('B', 'e', 'n', 'g'), 0x09CD, true, BasePosition::Last, RephPosition::AfterSub, RephMode::Implicit,
        BlwfMode::PreAndPost },
    { script_code('G', 'u', 'r', 'u'), 0x0A4D, true, BasePosition::Last, RephPosition::BeforeSub, RephMode::Implicit,
        BlwfMode::PreAndPost },
    { script_code('G', 'u', 'j', 'r'), 0x0ACD, true, BasePosition::Last, RephPosition::BeforePost, RephMode::Implicit,
        BlwfMode::PreAndPost },
    { script_code('O', 'r', 'y', 'a'), 0x0B4D, true, BasePosition::Last, RephPosition::AfterMain, RephMode::Implicit,
        BlwfMode::PreAndPost },
    { script_code('T', 'a', 'm', 'l'), 0x0BCD, true, BasePosition::Last, RephPosition::AfterPost, RephMode::Implicit,
        BlwfMode::PreAndPost },
    { script_code('T', 'e', 'l', 'u'), 0x0C4D, true, BasePosition::Last, RephPosition::AfterPost, RephMode::Explicit,
        BlwfMode::PostOnly },
    { script_code('K', 'n', 'd', 'a'), 0x0CCD, true, BasePosition::Last, RephPosition::AfterPost, RephMode::Implicit,
        BlwfMode::PostOnly },
    { script_code('M', 'l', 'y', 'm'), 0x0D4D, true, BasePosition::Last, RephPosition::AfterMain, RephMode::Logical,
        BlwfMode::PreAndPost },
};

constexpr ScriptConfig default_config {
    0, 0, false, BasePosition::Last, RephPosition::BeforePost, RephMode::Implicit, BlwfMode::PreAndPost
};

constexpr ScriptCode deva = script_code('D', 'e', 'v', 'a');
constexpr ScriptCode beng = script_code('B', 'e', 'n', 'g');
constexpr ScriptCode guru = script_code('G', 'u', 'r', 'u');
constexpr ScriptCode gujr = script_code('G', 'u', 'j', 'r');
constexpr ScriptCode orya = script_code('O', 'r', 'y', 'a');
constexpr ScriptCode taml = script_code('T', 'a', 'm', 'l');
constexpr ScriptCode telu = script_code('T', 'e', 'l', 'u');
constexpr ScriptCode knda = script_code('K', 'n', 'd', 'a');
constexpr ScriptCode mlym = script_code('M', 'l', 'y', 'm');

enum Feature : std::size_t {
    Nukt,
    Akhn,
    Rphf,
    Rkrf,
    Pref,
    Blwf,
    Abvf,
    Half,
    Pstf,
    Vatu,
    Cjct,
    Init,
    Pres,
    Abvs,
    Blws,
    Psts,
    Haln,
    FeatureCount,
};

struct FeatureSpec {
    std::uint32_t tag;
    bool global;
};

// The basic shaping features in the specifications' order, then the
// presentation features applied after final reordering.
constexpr FeatureSpec features[FeatureCount] = {
    { make_tag('n', 'u', 'k', 't'), true },
    { make_tag('a', 'k', 'h', 'n'), true },
    { make_tag('r', 'p', 'h', 'f'), false },
    { make_tag('r', 'k', 'r', 'f'), true },
    { make_tag('p', 'r', 'e', 'f'), false },
    { make_tag('b', 'l', 'w', 'f'), false },
    { make_tag('a', 'b', 'v', 'f'), false },
    { make_tag('h', 'a', 'l', 'f'), false },
    { make_tag('p', 's', 't', 'f'), false },
    { make_tag('v', 'a', 't', 'u'), true },
    { make_tag('c', 'j', 'c', 't'), true },
    { make_tag('i', 'n', 'i', 't'), false },
    { make_tag('p', 'r', 'e', 's'), true },
    { make_tag('a', 'b', 'v', 's'), true },
    { make_tag('b', 'l', 'w', 's'), true },
    { make_tag('p', 's', 't', 's'), true },
    { make_tag('h', 'a', 'l', 'n'), true },
};
constexpr std::size_t basic_feature_count = Init; // the ones before final reordering

struct IndicPlan {
    ScriptConfig config = default_config;
    bool old_spec = false; // the font's script tag is the original one ('deva', not 'dev2')
    std::array<std::uint32_t, FeatureCount> mask {};
};

IndicPlan const& indic_plan(Plan const& plan)
{
    static IndicPlan const none;
    return plan.shaper_data ? *static_cast<IndicPlan const*>(plan.shaper_data.get()) : none;
}

// --- Character categories ------------------------------------------------------------

bool is_ra(char32_t c)
{
    switch (c) {
    case 0x0930: // Devanagari
    case 0x09B0: // Bengali
    case 0x09F0: // Bengali (Assamese)
    case 0x0A30: // Gurmukhi
    case 0x0AB0: // Gujarati
    case 0x0B30: // Oriya
    case 0x0BB0: // Tamil
    case 0x0C30: // Telugu
    case 0x0CB0: // Kannada
    case 0x0D30: // Malayalam
        return true;
    default:
        return false;
    }
}

// Where a dependent vowel sorts among the consonant forms, by the side of
// the base it is drawn on and the script: the specifications have each
// script's matras drawn after or before its below-base and post-base
// consonant forms.
Position matra_position(ScriptCode script, char32_t c, IndicPosition side)
{
    enum Side { Pre, Post, Above, Below };
    Side s = Below;
    switch (side) {
    case IndicPosition::Left:
    case IndicPosition::Visual_Order_Left:
    case IndicPosition::Left_And_Right:
    case IndicPosition::Top_And_Left:
    case IndicPosition::Top_And_Left_And_Right:
    case IndicPosition::Bottom_And_Left:
    case IndicPosition::Top_And_Bottom_And_Left:
        s = Pre;
        break;
    case IndicPosition::Right:
    case IndicPosition::Top_And_Right:
    case IndicPosition::Bottom_And_Right:
    case IndicPosition::Top_And_Bottom_And_Right:
        s = Post;
        break;
    case IndicPosition::Top:
    case IndicPosition::Top_And_Bottom:
        s = Above;
        break;
    default:
        s = Below;
        break;
    }
    if (s == Pre)
        return PreM;
    switch (script) {
    case deva:
        return AfterSub;
    case beng:
        return s == Post ? AfterPost : AfterSub;
    case guru:
        return AfterPost;
    case gujr:
        return s == Above ? AfterSub : AfterPost;
    case orya:
        return s == Post ? AfterPost : s == Above ? AfterMain : AfterSub;
    case taml:
        return s == Above ? AfterSub : AfterPost;
    case telu:
        if (s == Post)
            return c <= 0x0C42 ? BeforeSub : AfterSub;
        return BeforeSub;
    case knda:
        if (s == Post)
            return (c < 0x0CC3 || c > 0x0CD6) ? BeforeSub : AfterSub;
        return BeforeSub;
    case mlym:
        return s == Above ? AfterSub : AfterPost;
    default:
        return AfterSub;
    }
}

struct Classified {
    Category category;
    Position position;
};

Classified classify(ScriptCode script, char32_t c)
{
    // Characters the specifications let stand as a base for marks.
    switch (c) {
    case 0x00A0: // no-break space
    case 0x00D7: // multiplication sign
    case 0x2010: // hyphen
    case 0x2011: // non-breaking hyphen
    case 0x2012: // figure dash
    case 0x2013: // en dash
    case 0x2014: // em dash
    case 0x2015: // horizontal bar
    case 0x2022: // bullet
    case 0x25FB: // white medium square
    case 0x25FC: // black medium square
    case 0x25FD: // white medium small square
    case 0x25FE: // black medium small square
    case 0x2212: // minus sign
        return { PLACEHOLDER, BaseC };
    case 0x25CC:
        return { DOTTEDCIRCLE, BaseC };
    case 0x200C:
        return { ZWNJ, End };
    case 0x200D:
        return { ZWJ, End };
    default:
        break;
    }
    CharacterProperties const p = character_properties(c);
    auto const syllabic = static_cast<IndicSyllabic>(p.indic_syllabic);
    auto const side = static_cast<IndicPosition>(p.indic_position);
    // A sign written as a letter of its own (a spacing candrabindu, a vedic
    // anusvara: general category Lo) is a base, as in the Universal
    // Shaping Engine's derivation of its classes.
    bool const letter = p.category == GeneralCategory::Lo;
    switch (syllabic) {
    case IndicSyllabic::Bindu:
    case IndicSyllabic::Visarga:
    case IndicSyllabic::Gemination_Mark:
    case IndicSyllabic::Syllable_Modifier:
        if (letter)
            return { PLACEHOLDER, BaseC };
        // The Oriya candrabindu is drawn before the below-base forms.
        return { SM, c == 0x0B01 ? BeforeSub : SMVD };
    case IndicSyllabic::Cantillation_Mark:
    case IndicSyllabic::Tone_Mark:
        if (letter)
            return { PLACEHOLDER, BaseC };
        return { A, SMVD };
    case IndicSyllabic::Avagraha:
        return { Symbol, End };
    case IndicSyllabic::Nukta:
        return { N, End };
    case IndicSyllabic::Virama:
    case IndicSyllabic::Invisible_Stacker:
        return { H, End };
    case IndicSyllabic::Pure_Killer:
    case IndicSyllabic::Vowel_Dependent:
        // The Oriya sign overline modifies its consonant like a nukta.
        if (c == 0x0B55)
            return { N, End };
        // Gurmukhi ii may follow the bindi or tippi it is written with.
        return { c == 0x0A40 ? MPst : M, matra_position(script, c, side) };
    case IndicSyllabic::Vowel_Independent:
    case IndicSyllabic::Vowel:
        return { V, BaseC };
    case IndicSyllabic::Consonant_Placeholder:
    case IndicSyllabic::Number:
    case IndicSyllabic::Brahmi_Joining_Number:
        return { PLACEHOLDER, BaseC };
    case IndicSyllabic::Consonant:
    case IndicSyllabic::Consonant_Dead:
    case IndicSyllabic::Consonant_Head_Letter:
    case IndicSyllabic::Consonant_Final:
    case IndicSyllabic::Consonant_Prefixed:
    case IndicSyllabic::Consonant_Initial_Postfixed:
    case IndicSyllabic::Consonant_Succeeding_Repha:
        return { is_ra(c) ? Ra : C, BaseC };
    case IndicSyllabic::Consonant_With_Stacker:
        return { CS, BaseC };
    case IndicSyllabic::Consonant_Preceding_Repha:
        return { Repha, End };
    case IndicSyllabic::Consonant_Medial:
    case IndicSyllabic::Consonant_Subjoined:
        return { CM, End };
    case IndicSyllabic::Register_Shifter:
        return { RS, End };
    case IndicSyllabic::Non_Joiner:
        return { ZWNJ, End };
    case IndicSyllabic::Joiner:
        return { ZWJ, End };
    default:
        return { X, End };
    }
}

constexpr std::uint32_t flag(unsigned k) { return 1u << k; }
constexpr std::uint32_t consonant_flags
    = flag(C) | flag(CS) | flag(Ra) | flag(CM) | flag(V) | flag(PLACEHOLDER) | flag(DOTTEDCIRCLE);

Category category(GlyphInfo const& g) { return static_cast<Category>(g.shaper); }
Position position(GlyphInfo const& g) { return static_cast<Position>(g.shaper2); }
void set_position(GlyphInfo& g, Position p) { g.shaper2 = p; }
bool is_one_of(GlyphInfo const& g, std::uint32_t flags) { return g.shaper < 32 && (flag(g.shaper) & flags) != 0; }
bool is_consonant(GlyphInfo const& g) { return is_one_of(g, consonant_flags); }
bool is_joiner(GlyphInfo const& g) { return is_one_of(g, flag(ZWJ) | flag(ZWNJ)); }
// Ligated by GSUB, and not a component a later substitution split off.
bool is_matra(GlyphInfo const& g) { return is_one_of(g, flag(M) | flag(MPst)); }
bool is_ligated(GlyphInfo const& g) { return (g.props & otl::glyph_props::ligated) && !otl::is_multiplied(g); }
// A virama still standing as itself, not taken into a ligature.
bool is_halant(GlyphInfo const& g) { return category(g) == H && !is_ligated(g); }

// --- The cluster grammar ----------------------------------------------------------------

constexpr std::pair<std::string_view, std::uint8_t> category_names[] = {
    { "X", X }, { "C", C }, { "V", V }, { "N", N }, { "H", H }, { "ZWNJ", ZWNJ }, { "ZWJ", ZWJ }, { "M", M },
    { "SM", SM }, { "A", A }, { "PLACEHOLDER", PLACEHOLDER }, { "DOTTEDCIRCLE", DOTTEDCIRCLE }, { "RS", RS },
    { "Repha", Repha }, { "Ra", Ra }, { "CM", CM }, { "Symbol", Symbol }, { "CS", CS }, { "MPst", MPst },
};

// The syllable structures of the specifications ("Shaping engine: analyze
// the syllables"), spelled out into each rule: a consonant with its nukta
// and joiner, joined to the next by a virama; matras with their nuktas;
// syllable modifiers and vedic signs at the end.
#define INDIC_C "[C Ra]"
#define INDIC_N "((ZWNJ? RS)? (N N?)?)"
#define INDIC_Z "[ZWJ ZWNJ]"
#define INDIC_REPH "(Ra H | Repha)"
#define INDIC_CN "(" INDIC_C " ZWJ? " INDIC_N ")"
#define INDIC_MATRA_GROUP "(" INDIC_Z "* (M | SM? MPst) N? H?)"
#define INDIC_SYLLABLE_TAIL "((" INDIC_Z "? SM SM? ZWNJ?)? A*)"
#define INDIC_HALANT_GROUP "(" INDIC_Z "? H (ZWJ N?)?)"
#define INDIC_FINAL_HALANT_GROUP "(" INDIC_HALANT_GROUP " | H ZWNJ)"
#define INDIC_HALANT_OR_MATRA_GROUP "(" INDIC_FINAL_HALANT_GROUP " | " INDIC_MATRA_GROUP "*)"
#define INDIC_COMPLEX_TAIL                                                                                            \
    "((" INDIC_HALANT_GROUP " " INDIC_CN ")* CM? " INDIC_HALANT_OR_MATRA_GROUP " " INDIC_SYLLABLE_TAIL ")"

struct Grammar {
    CategoryPattern consonant { "[Repha CS]? " INDIC_CN " " INDIC_COMPLEX_TAIL, category_names };
    CategoryPattern vowel { INDIC_REPH "? V " INDIC_N " (ZWJ | " INDIC_COMPLEX_TAIL ")", category_names };
    CategoryPattern standalone {
        "([Repha CS]? PLACEHOLDER | " INDIC_REPH "? DOTTEDCIRCLE) " INDIC_N " " INDIC_COMPLEX_TAIL, category_names
    };
    CategoryPattern symbol { "Symbol N? " INDIC_SYLLABLE_TAIL, category_names };
    CategoryPattern broken { INDIC_REPH "? " INDIC_N " " INDIC_COMPLEX_TAIL, category_names };
    std::array<SyllableRule, 5> rules { {
        { &consonant, ConsonantSyllable },
        { &vowel, VowelSyllable },
        { &standalone, StandaloneCluster },
        { &symbol, SymbolCluster },
        { &broken, BrokenCluster },
    } };
};

Grammar const& grammar()
{
    static Grammar const g;
    return g;
}

// --- The plan ---------------------------------------------------------------------------

void initial_reordering(Plan const&, ShapeFont const&, Buffer&);
void final_reordering(Plan const&, ShapeFont const&, Buffer&);

void collect_features(PlanBuilder& p)
{
    // The syllables are found before anything substitutes; 'locl' and
    // 'ccmp' come first, within the syllable, then the reordering.
    p.enable_feature(make_tag('l', 'o', 'c', 'l'), feature_flag::per_syllable);
    p.enable_feature(make_tag('c', 'c', 'm', 'p'), feature_flag::per_syllable);
    p.add_gsub_pause(initial_reordering);
    // The basic features, one at a time, each within the syllable; joiners
    // are seen by them (a ZWJ asks for a half form, a ZWNJ blocks one).
    std::uint32_t const joiners = feature_flag::manual_zwj | feature_flag::manual_zwnj | feature_flag::per_syllable;
    for (std::size_t k = 0; k < basic_feature_count; ++k) {
        p.add_feature(features[k].tag, joiners | (features[k].global ? feature_flag::global : 0));
        p.add_gsub_pause(nullptr);
    }
    p.add_gsub_pause(final_reordering);
    for (std::size_t k = basic_feature_count; k < FeatureCount; ++k)
        p.add_feature(features[k].tag, joiners | (features[k].global ? feature_flag::global : 0));
}

void override_features(PlanBuilder& p)
{
    // Ligatures of these scripts are the presentation features' work.
    p.disable_feature(make_tag('l', 'i', 'g', 'a'));
}

void init_plan(Plan& plan)
{
    auto data = std::make_shared<IndicPlan>();
    for (ScriptConfig const& c : script_configs) {
        if (c.script == plan.script)
            data->config = c;
    }
    // The second version's tags end in '2' (and a third version's in '3').
    std::uint32_t const tag = plan.chosen_script[0];
    char const last = static_cast<char>(tag & 0xFF);
    data->old_spec = data->config.has_old_spec && last != '2' && last != '3';
    for (std::size_t k = 0; k < FeatureCount; ++k)
        data->mask[k] = plan.mask(features[k].tag);
    plan.shaper_data = std::move(data);
}

// --- Setting up -------------------------------------------------------------------------

void setup_masks(Plan const& plan, ShapeFont const&, Buffer& b, std::u32string_view, std::u32string_view)
{
    for (GlyphInfo& g : b.info) {
        Classified const k = classify(plan.script, g.codepoint);
        g.shaper = k.category;
        g.shaper2 = k.position;
    }
    find_syllables(b, grammar().rules, NonIndicCluster, [](GlyphInfo const& g) { return g.shaper; });
}

// --- Initial reordering -------------------------------------------------------------------

// The forms the font has, by the lookups of the features that make them.
struct FormLookups {
    FormLookups(Plan const& plan, ShapeFont const& font)
        : rphf(plan, font, features[Rphf].tag)
        , pref(plan, font, features[Pref].tag)
        , blwf(plan, font, features[Blwf].tag)
        , pstf(plan, font, features[Pstf].tag)
        , vatu(plan, font, features[Vatu].tag)
    {
    }
    FeatureLookups rphf;
    FeatureLookups pref;
    FeatureLookups blwf;
    FeatureLookups pstf;
    FeatureLookups vatu;
};

// Where a consonant's form puts it, by what the font has: below the base
// ('blwf', or 'vatu' for a vattu), after it ('pstf'), or before it after
// reordering ('pref', sorted as post-base until the final reordering) —
// tried with the virama on either side, for both versions of the fonts.
Position consonant_position(FormLookups const& forms, ShapeFont const& font, std::uint32_t virama,
    std::uint32_t glyph)
{
    std::array<std::uint32_t, 3> const glyphs { virama, glyph, virama };
    auto would = [&](FeatureLookups const& f) {
        return !f.empty()
            && (f.would_substitute(font, std::span(glyphs).first(2)) || f.would_substitute(font, std::span(glyphs).last(2)));
    };
    if (would(forms.blwf) || would(forms.vatu))
        return BelowC;
    if (would(forms.pstf))
        return PostC;
    if (would(forms.pref))
        return PostC;
    return BaseC;
}

// Sorts [start, end) by position, stably. Pre-base items keep their own
// clusters here (the final reordering decides how a pre-base matra joins
// its consonant); from the base on, each group of items that traded places
// shares one cluster.
void sort_syllable(Buffer& b, std::size_t start, std::size_t end, bool merge_after_base)
{
    std::vector<GlyphInfo>& info = b.info;
    std::size_t const count = end - start;
    thread_local std::vector<std::uint16_t> order;
    thread_local std::vector<GlyphInfo> sorted;
    order.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        order[i] = static_cast<std::uint16_t>(i);
    std::stable_sort(order.begin(), order.end(),
        [&](std::uint16_t x, std::uint16_t y) { return position(info[start + x]) < position(info[start + y]); });
    sorted.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        sorted[i] = info[start + order[i]];
    std::copy(sorted.begin(), sorted.end(), info.begin() + static_cast<std::ptrdiff_t>(start));

    std::size_t base = count;
    for (std::size_t i = 0; i < count; ++i) {
        if (position(info[start + i]) == BaseC) {
            base = i;
            break;
        }
    }
    if (merge_after_base) {
        b.merge_clusters(start + base, end);
        return;
    }
    // Each permutation cycle among the items from the base on: an item
    // that came from further on, or went there, shares its cluster with
    // everything up to the furthest place the cycle reaches.
    thread_local std::vector<bool> seen;
    seen.assign(count, false);
    for (std::size_t i = base; i < count; ++i) {
        if (seen[i])
            continue;
        std::size_t furthest = i;
        std::size_t j = order[i];
        seen[i] = true;
        while (j != i && j < count && !seen[j]) {
            furthest = std::max(furthest, j);
            seen[j] = true;
            j = order[j];
        }
        if (furthest != i)
            b.merge_clusters(start + i, start + furthest + 1);
    }
}

void reorder_consonant_syllable(IndicPlan const& indic, FormLookups const& forms, ShapeFont const& font, ScriptCode script,
    Buffer& b, std::size_t start, std::size_t end)
{
    std::vector<GlyphInfo>& info = b.info;
    ScriptConfig const& config = indic.config;

    // Kannada ra + virama + ZWJ asks for the ra's subjoined form under
    // what precedes it, which fonts make of ra + ZWJ + virama.
    if (script == knda && start + 3 <= end && category(info[start]) == Ra && category(info[start + 1]) == H
        && category(info[start + 2]) == ZWJ) {
        b.merge_clusters(start + 1, start + 3);
        std::swap(info[start + 1], info[start + 2]);
    }

    // 1. The reph: ra and virama at the start that the font forms into
    // one ('rphf'), or a reph character of its own.
    std::size_t base = end;
    bool has_reph = false;
    std::size_t limit = start;
    if (indic.mask[Rphf] && start + 3 <= end
        && ((config.reph_mode == RephMode::Implicit && !is_joiner(info[start + 2]))
            || (config.reph_mode == RephMode::Explicit && category(info[start + 2]) == ZWJ))) {
        std::array<std::uint32_t, 3> const glyphs { info[start].glyph, info[start + 1].glyph, info[start + 2].glyph };
        if (forms.rphf.would_substitute(font, std::span(glyphs).first(2))
            || (config.reph_mode == RephMode::Explicit && forms.rphf.would_substitute(font, glyphs))) {
            limit += 2;
            while (limit < end && is_joiner(info[limit]))
                ++limit;
            base = start;
            has_reph = true;
        }
    } else if (config.reph_mode == RephMode::Logical && category(info[start]) == Repha) {
        limit += 1;
        while (limit < end && is_joiner(info[limit]))
            ++limit;
        base = start;
        has_reph = true;
    }

    // 2. The base consonant.
    if (config.base == BasePosition::Last) {
        // From the end backwards: the last consonant without a below-base
        // or post-base form (a post-base form cannot come before a
        // below-base one); failing that, the first consonant.
        bool seen_below = false;
        std::size_t i = end;
        do {
            --i;
            if (is_consonant(info[i])) {
                if (position(info[i]) != BelowC && (position(info[i]) != PostC || seen_below)) {
                    base = i;
                    break;
                }
                if (position(info[i]) == BelowC)
                    seen_below = true;
                base = i;
            } else if (start < i && category(info[i]) == ZWJ && category(info[i - 1]) == H) {
                // A virama and ZWJ ask for a half form: nothing before them
                // is the base. (A ZWJ before the virama asks for a
                // subjoined form instead, and the search goes on.)
                break;
            }
        } while (i > limit);
    }
    // A reph with no consonant after it but its own is that consonant.
    if (has_reph && base == start && limit - base <= 2)
        has_reph = false;

    // 3. Positions: what precedes the base is pre-base, the reph is set
    // aside.
    for (std::size_t i = start; i < base; ++i)
        set_position(info[i], std::min(PreC, position(info[i])));
    if (base < end)
        set_position(info[base], BaseC);
    if (has_reph)
        set_position(info[start], RaToBecomeReph);

    // The original specification puts the virama of a post-base form after
    // the last consonant of the syllable (Kannada: never after another
    // virama).
    if (indic.old_spec) {
        bool const no_double_halant = script == knda;
        for (std::size_t i = base + 1; i < end; ++i) {
            if (category(info[i]) != H)
                continue;
            std::size_t j = end - 1;
            while (j > i && !is_consonant(info[j]) && !(no_double_halant && category(info[j]) == H))
                --j;
            if (category(info[j]) != H && j > i) {
                GlyphInfo const halant = info[i];
                std::copy(info.begin() + static_cast<std::ptrdiff_t>(i) + 1, info.begin() + static_cast<std::ptrdiff_t>(j) + 1,
                    info.begin() + static_cast<std::ptrdiff_t>(i));
                info[j] = halant;
            }
            break;
        }
    }

    // Marks move with what they follow: a nukta, virama, joiner or medial
    // takes the position of the character before it (but a virama after a
    // pre-base matra stays with the consonant before the matra).
    {
        Position last = Start;
        for (std::size_t i = start; i < end; ++i) {
            if (is_one_of(info[i], flag(ZWJ) | flag(ZWNJ) | flag(N) | flag(RS) | flag(CM) | flag(H))) {
                set_position(info[i], last);
                if (category(info[i]) == H && position(info[i]) == PreM) {
                    for (std::size_t j = i; j > start; --j) {
                        if (position(info[j - 1]) != PreM) {
                            set_position(info[i], position(info[j - 1]));
                            break;
                        }
                    }
                }
            } else if (position(info[i]) != SMVD) {
                // A syllable modifier written before its post-base matra
                // goes where the matra goes.
                if (category(info[i]) == MPst && i > start && category(info[i - 1]) == SM)
                    set_position(info[i - 1], position(info[i]));
                last = position(info[i]);
            }
        }
    }
    // A post-base consonant takes along what stands between it and the
    // consonant or matra before it.
    {
        std::size_t last = base;
        for (std::size_t i = base + 1; i < end; ++i) {
            if (is_consonant(info[i])) {
                for (std::size_t j = last + 1; j < i; ++j) {
                    if (position(info[j]) < SMVD)
                        set_position(info[j], position(info[i]));
                }
                last = i;
            } else if (is_matra(info[i])) {
                last = i;
            }
        }
    }

    // 4. Into the drawing order.
    sort_syllable(b, start, end, indic.old_spec || end - start > 127);
    base = end;
    std::size_t first_pre_matra = end;
    std::size_t last_pre_matra = end;
    for (std::size_t i = start; i < end; ++i) {
        if (position(info[i]) == BaseC) {
            base = i;
            break;
        }
        if (position(info[i]) == PreM) {
            if (first_pre_matra == end)
                first_pre_matra = i;
            last_pre_matra = i;
        }
    }
    // Several pre-base matras are drawn in the reverse of their logical
    // order, each with its nukta and virama after it.
    if (first_pre_matra < last_pre_matra) {
        b.reverse_range(first_pre_matra, last_pre_matra + 1);
        std::size_t from = first_pre_matra;
        for (std::size_t j = first_pre_matra; j <= last_pre_matra; ++j) {
            if (is_matra(info[j])) {
                b.reverse_range(from, j + 1);
                from = j + 1;
            }
        }
    }

    // 5. The features each part gets.
    for (std::size_t i = start; i < end && position(info[i]) == RaToBecomeReph; ++i)
        info[i].mask |= indic.mask[Rphf];
    std::uint32_t pre_base = indic.mask[Half];
    if (!indic.old_spec && config.blwf_mode == BlwfMode::PreAndPost)
        pre_base |= indic.mask[Blwf];
    for (std::size_t i = start; i < base; ++i)
        info[i].mask |= pre_base;
    std::uint32_t const post_base = indic.mask[Blwf] | indic.mask[Abvf] | indic.mask[Pstf];
    for (std::size_t i = base + 1; i < end; ++i)
        info[i].mask |= post_base;

    // The original Devanagari specification: a ra + virama before the
    // base takes its below-base form (vattu) too, but not as ra + virama +
    // ZWJ, which asks for the eyelash form.
    if (indic.old_spec && script == deva) {
        for (std::size_t i = start; i + 1 < base; ++i) {
            if (category(info[i]) == Ra && category(info[i + 1]) == H
                && (i + 2 == base || category(info[i + 2]) != ZWJ)) {
                info[i].mask |= indic.mask[Blwf];
                info[i + 1].mask |= indic.mask[Blwf];
            }
        }
    }

    // The pre-base reordering form: the first two glyphs after the base
    // that the font forms one of.
    if (indic.mask[Pref] && base + 2 < end) {
        for (std::size_t i = base + 1; i + 1 < end; ++i) {
            std::array<std::uint32_t, 2> const glyphs { info[i].glyph, info[i + 1].glyph };
            if (forms.pref.would_substitute(font, glyphs)) {
                info[i].mask |= indic.mask[Pref];
                info[i + 1].mask |= indic.mask[Pref];
                break;
            }
        }
    }

    // A ZWNJ keeps what it follows, back to the consonant, from a half
    // form.
    for (std::size_t i = start + 1; i < end; ++i) {
        if (category(info[i]) != ZWNJ)
            continue;
        std::size_t j = i;
        do {
            --j;
            info[j].mask &= ~indic.mask[Half];
        } while (j > start && !is_consonant(info[j]));
    }
}

void initial_reordering(Plan const& plan, ShapeFont const& font, Buffer& b)
{
    IndicPlan const& indic = indic_plan(plan);
    FormLookups const forms(plan, font);
    // Each consonant's position by the forms the font has for it.
    if (std::uint32_t const virama = indic.config.virama ? font.nominal_glyph(indic.config.virama) : 0) {
        for (GlyphInfo& g : b.info) {
            if (position(g) == BaseC)
                set_position(g, consonant_position(forms, font, virama, g.glyph));
        }
    }
    // A broken cluster's marks get a dotted circle to stand on.
    insert_dotted_circles(font, b, BrokenCluster, DOTTEDCIRCLE, End, plan.global_mask,
        [](GlyphInfo const& g) { return category(g) == Repha; });
    for (std::size_t start = 0; start < b.len();) {
        std::size_t const end = syllable_end(b, start);
        unsigned const type = syllable_type(b.info[start]);
        if (type == ConsonantSyllable || type == VowelSyllable || type == StandaloneCluster || type == BrokenCluster)
            reorder_consonant_syllable(indic, forms, font, plan.script, b, start, end);
        start = end;
    }
}

// --- Final reordering ---------------------------------------------------------------------

// Moves info[from] to `to`, the items between shifting over by one.
void move_glyph(std::vector<GlyphInfo>& info, std::size_t from, std::size_t to)
{
    auto const at = [&](std::size_t i) { return info.begin() + static_cast<std::ptrdiff_t>(i); };
    if (from < to)
        std::rotate(at(from), at(from + 1), at(to + 1));
    else if (to < from)
        std::rotate(at(to), at(from), at(from + 1));
}

void final_reorder_syllable(IndicPlan const& indic, ScriptCode script, std::uint32_t virama, Buffer& b,
    std::size_t start, std::size_t end)
{
    std::vector<GlyphInfo>& info = b.info;
    ScriptConfig const& config = indic.config;

    // A virama a multiple substitution split off a ligature again is a
    // virama for what follows.
    if (virama) {
        for (std::size_t i = start; i < end; ++i) {
            GlyphInfo& g = info[i];
            if (g.glyph == virama && (g.props & otl::glyph_props::ligated) && otl::is_multiplied(g)) {
                g.shaper = H;
                g.props &= static_cast<std::uint16_t>(~(otl::glyph_props::ligated | otl::glyph_props::multiplied));
            }
        }
    }

    // The main consonant: the first glyph at or after the base position.
    bool try_pref = indic.mask[Pref] != 0;
    std::size_t base = start;
    for (; base < end; ++base) {
        if (position(info[base]) < BaseC)
            continue;
        if (try_pref && base + 1 < end) {
            // A pre-base reordering pair the font did not form leaves the
            // base after it.
            for (std::size_t i = base + 1; i < end; ++i) {
                if (!(info[i].mask & indic.mask[Pref]))
                    continue;
                if (!(otl::is_substituted(info[i]) && is_ligated(info[i]))) {
                    base = i;
                    while (base < end && is_halant(info[base]))
                        ++base;
                    if (base < end)
                        set_position(info[base], BaseC);
                    try_pref = false;
                }
                break;
            }
            if (base == end)
                break;
        }
        // Malayalam: a below-base consonant the font did not form is the
        // main consonant.
        if (script == mlym) {
            for (std::size_t i = base + 1; i < end; ++i) {
                while (i < end && is_joiner(info[i]))
                    ++i;
                if (i == end || !is_halant(info[i]))
                    break;
                ++i;
                while (i < end && is_joiner(info[i]))
                    ++i;
                if (i < end && is_consonant(info[i]) && position(info[i]) == BelowC) {
                    base = i;
                    set_position(info[base], BaseC);
                }
            }
        }
        if (start < base && position(info[base]) > BaseC)
            --base;
        break;
    }
    if (base == end && start < base && category(info[base - 1]) == ZWJ)
        --base;
    if (base < end) {
        while (start < base && is_one_of(info[base], flag(N) | flag(H)))
            --base;
    }

    // 1. A pre-base matra goes after the last virama left standing before
    // the main consonant (the half forms are drawn before it), else stays
    // at the start. Malayalam and Tamil have no half forms: what 'half'
    // made there (chillus, explicit viramas) comes before the matra.
    if (start + 1 < end && start < base) {
        std::size_t new_pos = base == end ? base - 2 : base - 1;
        if (script != mlym && script != taml) {
            for (;;) {
                while (new_pos > start && !is_one_of(info[new_pos], flag(M) | flag(MPst) | flag(H)))
                    --new_pos;
                if (is_halant(info[new_pos]) && position(info[new_pos]) != PreM) {
                    // A virama followed by ZWJ is part of a half form that
                    // asked for it: the matra goes before that.
                    if (new_pos + 1 < end && category(info[new_pos + 1]) == ZWJ && new_pos > start) {
                        --new_pos;
                        continue;
                    }
                } else {
                    new_pos = start;
                }
                break;
            }
        }
        if (start < new_pos && position(info[new_pos]) != PreM) {
            for (std::size_t i = new_pos; i > start; --i) {
                if (position(info[i - 1]) != PreM)
                    continue;
                std::size_t const old_pos = i - 1;
                if (old_pos < base && base <= new_pos)
                    --base;
                move_glyph(info, old_pos, new_pos);
                // The matra shares the cluster of the main consonant it
                // now stands before, not of the half forms it passed.
                b.merge_clusters(new_pos, std::min(end, base + 1));
                --new_pos;
            }
        } else {
            for (std::size_t i = start; i < base; ++i) {
                if (position(info[i]) == PreM) {
                    b.merge_clusters(i, std::min(end, base + 1));
                    break;
                }
            }
        }
    }

    // 2. The reph, when the font formed one: the specification's steps in
    // order, by the script's reph position.
    if (start + 1 < end && position(info[start]) == RaToBecomeReph
        && ((category(info[start]) == Repha) != is_ligated(info[start]))) {
        std::size_t new_pos = start;
        // After the first virama left standing between the reph and the
        // main consonant (and a joiner after it).
        auto after_explicit_halant = [&]() {
            std::size_t p = start + 1;
            while (p < base && !is_halant(info[p]))
                ++p;
            if (p < base && is_halant(info[p])) {
                if (p + 1 < base && is_joiner(info[p + 1]))
                    ++p;
                new_pos = p;
                return true;
            }
            return false;
        };
        bool found = false;
        if (config.reph_position != RephPosition::AfterPost)
            found = after_explicit_halant(); // step 2
        if (!found && config.reph_position == RephPosition::AfterMain) {
            // Step 3: after the main consonant and what is drawn with it.
            new_pos = base;
            while (new_pos + 1 < end && position(info[new_pos + 1]) <= AfterMain)
                ++new_pos;
            found = new_pos < end;
        }
        if (!found && config.reph_position == RephPosition::AfterSub) {
            // Step 4: after the below-base forms, before the first
            // post-base form, post-base matra or modifier.
            new_pos = base;
            while (new_pos + 1 < end) {
                Position const p = position(info[new_pos + 1]);
                if (p == PostC || p == AfterPost || p == SMVD)
                    break;
                ++new_pos;
            }
            found = new_pos < end;
        }
        if (!found)
            found = after_explicit_halant(); // step 5
        if (!found) {
            // Step 6: the end of the syllable, before the syllable
            // modifiers; but before a virama that ends it after a matra,
            // so that the reph meets the matra.
            new_pos = end - 1;
            while (new_pos > start && position(info[new_pos]) == SMVD)
                --new_pos;
            if (is_halant(info[new_pos])) {
                for (std::size_t i = base + 1; i < new_pos; ++i) {
                    if (is_matra(info[i]))
                        --new_pos;
                }
            }
        }
        b.merge_clusters(start, new_pos + 1);
        move_glyph(info, start, new_pos);
        if (start < base && base <= new_pos)
            --base;
    }

    // 3. The pre-base reordering consonant the 'pref' feature formed: where
    // a pre-base matra would go, else right before the main consonant.
    if (try_pref && base + 1 < end) {
        for (std::size_t i = base + 1; i < end; ++i) {
            if (!(info[i].mask & indic.mask[Pref]))
                continue;
            if (is_ligated(info[i])) {
                std::size_t new_pos = base;
                if (script != mlym && script != taml) {
                    while (new_pos > start && !is_one_of(info[new_pos - 1], flag(M) | flag(MPst) | flag(H)))
                        --new_pos;
                }
                if (new_pos > start && is_halant(info[new_pos - 1])) {
                    if (new_pos < end && is_joiner(info[new_pos]))
                        ++new_pos;
                }
                b.merge_clusters(new_pos, i + 1);
                move_glyph(info, i, new_pos);
                if (new_pos <= base && base < i)
                    ++base;
            }
            break;
        }
    }

    // 4. 'init' on a pre-base matra that starts a word: one with no letter,
    // mark or format character before it.
    if (indic.mask[Init] && position(info[start]) == PreM) {
        auto const before = start ? static_cast<GeneralCategory>(info[start - 1].category) : GeneralCategory::Cc;
        if (!start || before < GeneralCategory::Cf || before > GeneralCategory::Mn)
            info[start].mask |= indic.mask[Init];
    }
}

void final_reordering(Plan const& plan, ShapeFont const& font, Buffer& b)
{
    IndicPlan const& indic = indic_plan(plan);
    std::uint32_t const virama = indic.config.virama ? font.nominal_glyph(indic.config.virama) : 0;
    for (std::size_t start = 0; start < b.len();) {
        std::size_t const end = syllable_end(b, start);
        final_reorder_syllable(indic, plan.script, virama, b, start, end);
        start = end;
    }
}

// --- Normalization -------------------------------------------------------------------------

// The sequences the specifications prohibit get a dotted circle before
// anything else is done, so that the circle forms a cluster of its own.
void preprocess_text(Plan const& plan, ShapeFont const& font, Buffer& b)
{
    insert_vowel_constraint_circles(plan.script, font, b);
}

bool decompose(Plan const&, char32_t ab, char32_t& a, char32_t& b)
{
    switch (ab) {
    case 0x0931: // Devanagari rra and Tamil au are drawn as the letters they are.
    case 0x0B94:
        return false;
    default:
        return canonical_decompose(ab, a, b);
    }
}

// Split matras are decomposed (their parts are placed apart) and nothing is
// recomposed onto a mark, so that they stay apart. Bengali yya, a
// composition exclusion, is one letter to the fonts and is recomposed.
bool compose(Plan const&, char32_t a, char32_t b, char32_t& ab)
{
    if (is_mark_category(general_category(a)))
        return false;
    if (a == 0x09AF && b == 0x09BC) {
        ab = 0x09DF;
        return true;
    }
    ab = canonical_compose(a, b);
    return ab != 0;
}

}

ScriptShaper const indic_shaper {
    .collect_features = collect_features,
    .override_features = override_features,
    .init_plan = init_plan,
    .preprocess_text = preprocess_text,
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
