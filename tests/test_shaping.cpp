#include "Test.h"

#include "core/Inflate.h"
#include "core/Unicode.h"
#include "text/CharacterProperties.h"
#include "text/Face.h"
#include "text/Shaper.h"
#include "text/TrueType.h"
#include "text/TrueTypeWriter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// The text shaper. Three parts:
//  - a font built here with GDEF, GSUB and GPOS tables written by hand,
//    whose results follow from the OpenType specification by hand
//    arithmetic: ligatures and the marks on them, chained context, pair
//    kerning, mark-to-base, mark-to-ligature, cursive chains, the Arabic
//    joining forms, Thai SARA AM, Hangul jamo;
//  - the expected glyphs of words in many scripts, recorded from another
//    shaping engine with the machine's fonts (tests/fixtures/shaping/*.txt:
//    each names its font by file, size and CRC-32, and is skipped, loudly,
//    when that font is not here);
//  - hostile fonts: truncated and bit-flipped copies of real fonts shaped
//    with fixed strings in several scripts must neither crash nor hang.

using namespace sashfold;
using namespace sashfold::text;

namespace {

// --- Writing layout tables by hand ------------------------------------------------

// A table as bytes with 16-bit offsets to child tables, laid out after it.
struct Node {
    std::vector<std::uint8_t> bytes;
    std::vector<std::pair<std::size_t, std::shared_ptr<Node>>> links;

    Node& u16(unsigned v)
    {
        bytes.push_back(static_cast<std::uint8_t>(v >> 8));
        bytes.push_back(static_cast<std::uint8_t>(v));
        return *this;
    }
    Node& i16(int v) { return u16(static_cast<unsigned>(v) & 0xFFFF); }
    Node& tag(char const* t)
    {
        for (int k = 0; k < 4; ++k)
            bytes.push_back(static_cast<std::uint8_t>(t[k]));
        return *this;
    }
    Node& off(std::shared_ptr<Node> child)
    {
        links.emplace_back(bytes.size(), std::move(child));
        return u16(0);
    }
};
using NodeP = std::shared_ptr<Node>;

NodeP node() { return std::make_shared<Node>(); }

std::vector<std::uint8_t> serialize(NodeP const& root)
{
    std::vector<std::uint8_t> out;
    std::vector<std::pair<Node*, std::size_t>> queue { { root.get(), 0 } };
    out = root->bytes;
    for (std::size_t q = 0; q < queue.size(); ++q) {
        auto [n, at] = queue[q];
        for (auto const& [field, child] : n->links) {
            std::size_t const child_at = out.size();
            out.insert(out.end(), child->bytes.begin(), child->bytes.end());
            std::size_t const offset = child_at - at;
            out[at + field] = static_cast<std::uint8_t>(offset >> 8);
            out[at + field + 1] = static_cast<std::uint8_t>(offset);
            queue.emplace_back(child.get(), child_at);
        }
    }
    return out;
}

NodeP coverage(std::vector<unsigned> glyphs)
{
    NodeP c = node();
    c->u16(1).u16(static_cast<unsigned>(glyphs.size()));
    for (unsigned g : glyphs)
        c->u16(g);
    return c;
}

NodeP anchor(int x, int y)
{
    NodeP a = node();
    a->u16(1).i16(x).i16(y);
    return a;
}

NodeP lookup(unsigned type, unsigned flag, std::vector<NodeP> subtables)
{
    NodeP l = node();
    l->u16(type).u16(flag).u16(static_cast<unsigned>(subtables.size()));
    for (NodeP const& s : subtables)
        l->off(s);
    return l;
}

NodeP single_subst(std::vector<unsigned> from, std::vector<unsigned> to)
{
    NodeP s = node();
    s->u16(2).off(coverage(from)).u16(static_cast<unsigned>(to.size()));
    for (unsigned g : to)
        s->u16(g);
    return s;
}

// GSUB or GPOS with one script (DFLT) whose default language has every
// feature; `features` sorted by tag, each with its lookup indices.
std::vector<std::uint8_t> layout_table(std::vector<std::pair<char const*, std::vector<unsigned>>> const& features,
    std::vector<NodeP> const& lookups)
{
    NodeP lang = node();
    lang->u16(0).u16(0xFFFF).u16(static_cast<unsigned>(features.size()));
    for (unsigned k = 0; k < features.size(); ++k)
        lang->u16(k);
    NodeP script = node();
    script->off(lang).u16(0);
    NodeP scripts = node();
    scripts->u16(1).tag("DFLT").off(script);
    NodeP feature_list = node();
    feature_list->u16(static_cast<unsigned>(features.size()));
    for (auto const& [tag, indices] : features) {
        NodeP f = node();
        f->u16(0).u16(static_cast<unsigned>(indices.size()));
        for (unsigned i : indices)
            f->u16(i);
        feature_list->tag(tag).off(f);
    }
    NodeP lookup_list = node();
    lookup_list->u16(static_cast<unsigned>(lookups.size()));
    for (NodeP const& l : lookups)
        lookup_list->off(l);
    NodeP header = node();
    header->u16(1).u16(0).off(scripts).off(feature_list).off(lookup_list);
    return serialize(header);
}

constexpr std::uint32_t tag_of(char const* t)
{
    return feature_tag(t[0], t[1], t[2], t[3]);
}

// Glyphs: 1 f, 2 i, 3 fi, 4 a, 5 b, 6 b.alt, 7 acute, 8 x, 9 V, 10 A,
// 11 beh, 12 beh.init, 13 beh.medi, 14 beh.fina, 15 alef, 16 alef.fina,
// 17 c (cursive), 18 ko kai, 19 mai tho, 20 nikhahit, 21 sara aa,
// 22-24 the jamo of 각.
std::vector<std::uint8_t> hand_built_font()
{
    FontDescription d;
    d.family = "Shaping Test";
    d.units_per_em = 1000;
    d.ascender = 800;
    d.descender = -200;
    for (int g = 0; g < 25; ++g) {
        WriterGlyph glyph;
        glyph.advance = g == 3 ? 1000 : g == 7 ? 0 : 500;
        glyph.outline.points = { { 50, 0, true }, { 50, 500, true }, { 450, 500, true }, { 450, 0, true } };
        glyph.outline.contour_ends = { 3 };
        d.glyphs.push_back(glyph);
    }
    d.mappings = { { U'f', 1 }, { U'i', 2 }, { U'a', 4 }, { U'b', 5 }, { 0x0301, 7 }, { U'x', 8 }, { U'V', 9 },
        { U'A', 10 }, { 0x0628, 11 }, { 0x0627, 15 }, { U'c', 17 }, { 0x0E01, 18 }, { 0x0E49, 19 }, { 0x0E4D, 20 },
        { 0x0E32, 21 }, { 0x1100, 22 }, { 0x1161, 23 }, { 0x11A8, 24 } };

    // GDEF: the ligature, the mark, and every other glyph a base.
    NodeP classes = node();
    classes->u16(2).u16(5);
    classes->u16(1).u16(2).u16(1);
    classes->u16(3).u16(3).u16(2);
    classes->u16(4).u16(6).u16(1);
    classes->u16(7).u16(7).u16(3);
    classes->u16(8).u16(24).u16(1);
    NodeP gdef = node();
    gdef->u16(1).u16(0).off(classes).u16(0).u16(0).u16(0);
    d.extra_tables.emplace_back(tag_of("GDEF"), serialize(gdef));

    // GSUB. 0: f i -> fi (marks ignored); 1: b after a -> lookup 2;
    // 2: b -> b.alt; 3: init; 4: medi; 5: fina.
    NodeP ligature = node();
    ligature->u16(3).u16(2).u16(2);
    NodeP ligature_set = node();
    ligature_set->u16(1).off(ligature);
    NodeP liga = node();
    liga->u16(1).off(coverage({ 1 })).u16(1).off(ligature_set);
    NodeP chain = node();
    chain->u16(3).u16(1).off(coverage({ 4 })).u16(1).off(coverage({ 5 })).u16(0).u16(1).u16(0).u16(2);
    std::vector<NodeP> gsub_lookups {
        lookup(4, 0x0008, { liga }),
        lookup(6, 0, { chain }),
        lookup(1, 0, { single_subst({ 5 }, { 6 }) }),
        lookup(1, 0, { single_subst({ 11 }, { 12 }) }),
        lookup(1, 0, { single_subst({ 11 }, { 13 }) }),
        lookup(1, 0, { single_subst({ 11, 15 }, { 14, 16 }) }),
    };
    d.extra_tables.emplace_back(tag_of("GSUB"),
        layout_table({ { "calt", { 1 } }, { "fina", { 5 } }, { "init", { 3 } }, { "liga", { 0 } }, { "medi", { 4 } } },
            gsub_lookups));

    // GPOS. 0: A V kerned by -100; 1: mark-to-base; 2: mark-to-ligature;
    // 3: cursive.
    NodeP pair_set = node();
    pair_set->u16(1).u16(9).i16(-100);
    NodeP pair = node();
    pair->u16(1).off(coverage({ 10 })).u16(0x0004).u16(0).u16(1).off(pair_set);
    NodeP mark_array = node();
    mark_array->u16(1).u16(0).off(anchor(50, 500));
    NodeP base_array = node();
    base_array->u16(2).off(anchor(250, 600)).off(anchor(300, 650));
    NodeP mark_base = node();
    mark_base->u16(1).off(coverage({ 7 })).off(coverage({ 4, 8 })).u16(1).off(mark_array).off(base_array);
    NodeP lig_attach = node();
    lig_attach->u16(2).off(anchor(100, 700)).off(anchor(400, 700));
    NodeP lig_array = node();
    lig_array->u16(1).off(lig_attach);
    NodeP mark_array2 = node();
    mark_array2->u16(1).u16(0).off(anchor(50, 500));
    NodeP mark_lig = node();
    mark_lig->u16(1).off(coverage({ 7 })).off(coverage({ 3 })).u16(1).off(mark_array2).off(lig_array);
    NodeP cursive = node();
    cursive->u16(1).off(coverage({ 17 })).u16(1).off(anchor(0, 0)).off(anchor(500, 100));
    std::vector<NodeP> gpos_lookups {
        lookup(2, 0, { pair }),
        lookup(4, 0, { mark_base }),
        lookup(5, 0, { mark_lig }),
        lookup(3, 0, { cursive }),
    };
    d.extra_tables.emplace_back(tag_of("GPOS"),
        layout_table({ { "curs", { 3 } }, { "kern", { 0 } }, { "mark", { 1, 2 } } }, gpos_lookups));
    return write_truetype(d);
}

std::unique_ptr<Face> face_from(std::vector<std::uint8_t> bytes, std::size_t index = 0)
{
    std::optional<TrueTypeFont> font = TrueTypeFont::parse(std::move(bytes), index);
    if (!font)
        return nullptr;
    return make_truetype_face(std::move(*font));
}

ShapedRun run_of(Face const& face, std::u32string const& text, ScriptCode script,
    TextDirection direction = TextDirection::LeftToRight, std::vector<FeatureSetting> const& features = {})
{
    ShapeRequest request;
    request.text = text;
    request.script = script;
    request.direction = direction;
    request.features = features;
    return shape(face, request);
}

std::vector<std::uint32_t> glyphs_of(ShapedRun const& run)
{
    std::vector<std::uint32_t> out;
    for (ShapedGlyph const& g : run.glyphs)
        out.push_back(g.glyph);
    return out;
}

std::vector<std::uint32_t> clusters_of(ShapedRun const& run)
{
    std::vector<std::uint32_t> out;
    for (ShapedGlyph const& g : run.glyphs)
        out.push_back(g.cluster);
    return out;
}

std::string describe(std::vector<std::uint32_t> const& v)
{
    std::string s;
    for (std::uint32_t x : v)
        s += (s.empty() ? "" : " ") + std::to_string(x);
    return s;
}

void hand_built_tests()
{
    std::unique_ptr<Face> const face = face_from(hand_built_font());
    if (!CHECK(face != nullptr))
        return;
    using V = std::vector<std::uint32_t>;
    // Ligature, and switching it off.
    {
        ShapedRun const run = run_of(*face, U"fi", scripts::latin);
        CHECK_EQ(describe(glyphs_of(run)), describe(V { 3 }));
        CHECK_EQ(run.advance(), 1000);
        ShapedRun const off = run_of(*face, U"fi", scripts::latin, TextDirection::LeftToRight,
            { FeatureSetting { tag_of("liga"), 0 } });
        CHECK_EQ(describe(glyphs_of(off)), describe(V { 1, 2 }));
    }
    // Chained context: b after a only.
    CHECK_EQ(describe(glyphs_of(run_of(*face, U"ab", scripts::latin))), describe(V { 4, 6 }));
    CHECK_EQ(describe(glyphs_of(run_of(*face, U"bb", scripts::latin))), describe(V { 5, 5 }));
    // Pair kerning, and font-kerning: none.
    {
        ShapedRun const run = run_of(*face, U"AV", scripts::latin);
        CHECK_EQ(run.glyphs.size(), 2u);
        if (run.glyphs.size() == 2) {
            CHECK_EQ(run.glyphs[0].x_advance, 400);
            CHECK_EQ(run.glyphs[1].x_advance, 500);
        }
        std::vector<FeatureSetting> features;
        shaping_features_for_css(CssFontFeatures { CssFontFeatures::Kerning::None }, features);
        ShapedRun const unkerned = run_of(*face, U"AV", scripts::latin, TextDirection::LeftToRight, features);
        CHECK_EQ(unkerned.advance(), 1000);
    }
    // Mark-to-base: the mark's anchor on the base's, the base's advance
    // taken back: x = 250 - 50 - 500, y = 600 - 500.
    {
        ShapedRun const run = run_of(*face, U"á", scripts::latin);
        CHECK_EQ(describe(glyphs_of(run)), describe(V { 4, 7 }));
        if (run.glyphs.size() == 2) {
            CHECK_EQ(run.glyphs[1].x_offset, -300);
            CHECK_EQ(run.glyphs[1].y_offset, 100);
            CHECK_EQ(run.glyphs[1].x_advance, 0);
            CHECK_EQ(describe(clusters_of(run)), describe(V { 0, 0 }));
        }
    }
    // Mark-to-ligature: a mark after the ligature sits on its last
    // component (400 - 50 - 1000), a mark between f and i on the first
    // (100 - 50 - 1000).
    {
        ShapedRun const after = run_of(*face, U"fí", scripts::latin);
        CHECK_EQ(describe(glyphs_of(after)), describe(V { 3, 7 }));
        if (after.glyphs.size() == 2) {
            CHECK_EQ(after.glyphs[1].x_offset, -650);
            CHECK_EQ(after.glyphs[1].y_offset, 200);
        }
        ShapedRun const between = run_of(*face, U"f́i", scripts::latin);
        CHECK_EQ(describe(glyphs_of(between)), describe(V { 3, 7 }));
        CHECK_EQ(describe(clusters_of(between)), describe(V { 0, 0 }));
        if (between.glyphs.size() == 2)
            CHECK_EQ(between.glyphs[1].x_offset, -950);
    }
    // Cursive: each exit meets the next entry, each glyph 100 units above
    // the one before it (no RightToLeft flag: the later hangs from the
    // earlier).
    {
        ShapedRun const run = run_of(*face, U"ccc", scripts::latin);
        CHECK_EQ(run.glyphs.size(), 3u);
        if (run.glyphs.size() == 3) {
            CHECK_EQ(run.glyphs[0].y_offset, 0);
            CHECK_EQ(run.glyphs[1].y_offset, 100);
            CHECK_EQ(run.glyphs[2].y_offset, 200);
            CHECK_EQ(run.glyphs[0].x_advance, 500);
        }
    }
    // Arabic joining: beh beh beh is initial, medial, final; drawn right to
    // left. Beh alef: initial and final. Without 'fina' the last letters
    // keep their isolated glyphs.
    {
        ShapedRun const run = run_of(*face, U"ببب", scripts::arabic, TextDirection::RightToLeft);
        CHECK_EQ(describe(glyphs_of(run)), describe(V { 14, 13, 12 }));
        CHECK_EQ(describe(clusters_of(run)), describe(V { 2, 1, 0 }));
        CHECK_EQ(describe(glyphs_of(run_of(*face, U"با", scripts::arabic, TextDirection::RightToLeft))),
            describe(V { 16, 12 }));
        CHECK_EQ(describe(glyphs_of(run_of(*face, U"با", scripts::arabic, TextDirection::RightToLeft,
                     { FeatureSetting { tag_of("fina"), 0 } }))),
            describe(V { 15, 12 }));
        // The text before the run joins its first letter: a beh after a
        // beh in another run is final, not isolated.
        ShapeRequest request;
        std::u32string const text = U"ب";
        request.text = text;
        request.before = U"ب";
        request.script = scripts::arabic;
        request.direction = TextDirection::RightToLeft;
        CHECK_EQ(describe(glyphs_of(shape(*face, request))), describe(V { 14 }));
    }
    // Thai: SARA AM is NIKHAHIT and SARA AA, the NIKHAHIT before the tone
    // mark; one cluster.
    {
        ShapedRun const run = run_of(*face, U"ก้ำ", scripts::thai);
        CHECK_EQ(describe(glyphs_of(run)), describe(V { 18, 20, 19, 21 }));
        CHECK_EQ(describe(clusters_of(run)), describe(V { 0, 0, 0, 0 }));
    }
    // Hangul: a syllable the font lacks is drawn from its jamo, one cluster.
    {
        ShapedRun const run = run_of(*face, U"각", scripts::hangul);
        CHECK_EQ(describe(glyphs_of(run)), describe(V { 22, 23, 24 }));
        CHECK_EQ(describe(clusters_of(run)), describe(V { 0, 0, 0 }));
    }
}

// --- Itemization and tags ----------------------------------------------------------

void itemization_tests()
{
    std::u32string const text = U"Hello (مرحبا) 12 שלום!";
    std::vector<ScriptRun> const runs = itemize_scripts(text);
    // The parentheses pair up: the closing one goes back to the Latin the
    // opening one was in, and so do the spaces and digits after it.
    CHECK_EQ(runs.size(), 4u);
    if (runs.size() == 4) {
        CHECK(runs[0].script == scripts::latin);
        CHECK(runs[1].script == scripts::arabic);
        CHECK(runs[2].script == scripts::latin);
        CHECK(runs[3].script == scripts::hebrew);
        CHECK_EQ(runs[0].start, 0u);
        CHECK_EQ(runs[1].start, 7u);
        CHECK_EQ(runs[2].start, 12u);
        CHECK_EQ(runs[3].start, 17u);
        CHECK_EQ(runs[3].end, text.size());
    }
    // Marks and Common characters take their neighbors' scripts.
    std::vector<ScriptRun> const marked = itemize_scripts(U"́abć def");
    CHECK_EQ(marked.size(), 1u);
    CHECK(script_of(U'a') == scripts::latin);
    CHECK(script_of(0x0301) == scripts::inherited);
    CHECK(script_is_right_to_left(scripts::arabic));
    CHECK(script_is_right_to_left(scripts::hebrew));
    CHECK(!script_is_right_to_left(scripts::latin));
    CHECK(joining_type(0x0628) == JoiningType::DualJoining);
    CHECK(joining_type(0x0710) == JoiningType::Alaph);
    CHECK(joining_type(0x064E) == JoiningType::Transparent);
    OpenTypeScriptTags const deva = opentype_script_tags(script_code('D', 'e', 'v', 'a'));
    CHECK_EQ(deva.count, 3u);
    CHECK(deva.tags[0] == tag_of("dev3"));
    CHECK(deva.tags[1] == tag_of("dev2"));
    CHECK(deva.tags[2] == tag_of("deva"));
    CHECK_EQ(opentype_script_tags(script_code('M', 'y', 'm', 'r')).count, 2u);
    CHECK(opentype_script_tags(scripts::lao).tags[0] == tag_of("lao "));
    CHECK(opentype_script_tags(scripts::arabic).tags[0] == tag_of("arab"));
    CHECK(opentype_language_tag("fa-IR") == tag_of("FAR "));
    CHECK(opentype_language_tag("zh-Hant") == tag_of("ZHT "));
    CHECK(opentype_language_tag("x-ot-URD") == tag_of("URD "));
}

// --- Recorded results with the machine's fonts --------------------------------------

std::optional<std::string> find_font(std::string const& name)
{
    for (char const* dir : { "/usr/share/fonts/noto/", "/usr/share/fonts/TTF/", "/usr/share/fonts/liberation/",
             "/usr/share/fonts/noto-cjk/", "/usr/share/fonts/truetype/noto/", "/usr/share/fonts/truetype/dejavu/",
             "/usr/share/fonts/truetype/liberation/", "/usr/share/fonts/opentype/noto/", "/usr/local/share/fonts/" }) {
        std::string const path = std::string(dir) + name;
        if (std::filesystem::exists(path))
            return path;
    }
    return std::nullopt;
}

std::vector<std::uint8_t> read_file(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

struct FixtureResult {
    std::size_t words = 0;
    std::size_t same = 0;
    bool skipped = false;
};

FixtureResult run_fixture(std::filesystem::path const& file)
{
    FixtureResult result;
    std::ifstream in(file);
    std::string line;
    std::string font_name;
    std::size_t face_index = 0;
    std::size_t size = 0;
    std::string crc;
    std::string script;
    std::string language;
    std::string direction;
    std::string features;
    std::vector<std::pair<std::string, std::string>> cases;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::size_t const tab = line.find('\t');
        if (tab != std::string::npos) {
            cases.emplace_back(line.substr(0, tab), line.substr(tab + 1));
            continue;
        }
        std::istringstream header(line);
        std::string key;
        header >> key;
        if (key == "font")
            header >> font_name >> face_index >> size >> crc;
        else if (key == "script")
            header >> script;
        else if (key == "language")
            header >> language;
        else if (key == "direction")
            header >> direction;
        else if (key == "features")
            header >> features;
    }
    std::optional<std::string> const path = find_font(font_name);
    std::vector<std::uint8_t> bytes = path ? read_file(*path) : std::vector<std::uint8_t> {};
    char actual_crc[16];
    std::snprintf(actual_crc, sizeof actual_crc, "%08x", crc32_of(bytes));
    if (!path || bytes.size() != size || crc != actual_crc) {
        std::cerr << "SKIP " << file.filename().string() << ": " << font_name
                  << (path ? " is a different version here" : " is not installed") << "; " << cases.size()
                  << " recorded words not checked\n";
        result.skipped = true;
        return result;
    }
    std::unique_ptr<Face> const face = face_from(std::move(bytes), face_index);
    if (!CHECK(face != nullptr))
        return result;
    std::vector<FeatureSetting> settings;
    if (features != "-") {
        std::istringstream list(features);
        for (std::string item; std::getline(list, item, ',');) {
            FeatureSetting f;
            if (item[0] == '-' || item[0] == '+') {
                f.value = item[0] == '+' ? 1 : 0;
                item.erase(0, 1);
            }
            f.tag = tag_of((item + "    ").c_str());
            settings.push_back(f);
        }
    }
    ShapeRequest request;
    request.script = script_code(script[0], script[1], script[2], script[3]);
    std::string const lang = language == "-" ? "" : language;
    request.language = lang;
    request.direction = direction == "rtl" ? TextDirection::RightToLeft : TextDirection::LeftToRight;
    request.features = settings;
    ShapedRun run;
    std::size_t shown = 0;
    for (auto const& [word, expected] : cases) {
        std::u32string const text = decode_utf8(word);
        request.text = text;
        shape(*face, request, run);
        std::string got;
        for (ShapedGlyph const& g : run.glyphs) {
            char item[96];
            std::snprintf(item, sizeof item, "%s%u:%u:%d:%d:%d:%d", got.empty() ? "" : " ", g.glyph, g.cluster,
                g.x_advance, g.y_advance, g.x_offset, g.y_offset);
            got += item;
        }
        ++result.words;
        if (got == expected) {
            ++result.same;
        } else if (shown++ < 3) {
            std::cerr << "  " << file.filename().string() << ": " << word << "\n    expected " << expected << "\n    got      "
                      << got << "\n";
        }
    }
    return result;
}

// --- Hostile fonts --------------------------------------------------------------------

void shape_everything(Face const& face)
{
    struct Sample {
        char32_t const* text;
        ScriptCode script;
        TextDirection direction;
    };
    static constexpr Sample samples[] = {
        { U"office fi ́á̂ ÄV", scripts::latin, TextDirection::LeftToRight },
        { U"بِسْمِ اللَّهِ لا", scripts::arabic,
            TextDirection::RightToLeft },
        { U"שָּׁלוֹם", scripts::hebrew, TextDirection::RightToLeft },
        { U"น้ำกิน", scripts::thai, TextDirection::LeftToRight },
        { U"각가〮ᄒᆞ", scripts::hangul, TextDirection::LeftToRight },
        { U"ܐܒܐܕܐ", scripts::syriac, TextDirection::RightToLeft },
        { U"ᾀἆ", scripts::greek, TextDirection::LeftToRight },
    };
    for (Sample const& s : samples) {
        std::u32string const text = s.text;
        ShapedRun const run = run_of(face, text, s.script, s.direction);
        // Whatever it drew, every cluster names a character of the text.
        for (ShapedGlyph const& g : run.glyphs) {
            if (g.cluster >= text.size()) {
                CHECK(g.cluster < text.size());
                break;
            }
        }
    }
}

void hostile_tests(std::vector<std::vector<std::uint8_t>> const& fonts)
{
    std::uint32_t random = 12345;
    auto next = [&] {
        random = random * 1103515245u + 12345u;
        return random >> 8;
    };
    std::size_t shaped = 0;
    auto const start = std::chrono::steady_clock::now();
    for (std::vector<std::uint8_t> const& original : fonts) {
        // Truncated at many lengths.
        for (int k = 1; k <= 24; ++k) {
            std::vector<std::uint8_t> cut(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(original.size() * static_cast<std::size_t>(k) / 25));
            if (std::unique_ptr<Face> const face = face_from(std::move(cut))) {
                shape_everything(*face);
                ++shaped;
            }
        }
        // Bits flipped: a few at a time, all over the file.
        for (int k = 0; k < 120; ++k) {
            std::vector<std::uint8_t> flipped = original;
            int const flips = 1 + static_cast<int>(next() % 8);
            for (int f = 0; f < flips; ++f)
                flipped[next() % flipped.size()] ^= static_cast<std::uint8_t>(1u << (next() % 8));
            if (std::unique_ptr<Face> const face = face_from(std::move(flipped))) {
                shape_everything(*face);
                ++shaped;
            }
        }
    }
    double const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "  shaped with " << shaped << " damaged fonts in " << seconds << " s\n";
    CHECK(shaped > 0);
}

}

int main(int argc, char** argv)
{
    hand_built_tests();
    itemization_tests();

    std::filesystem::path const fixtures = argc > 1 ? argv[1] : "tests/fixtures/shaping";
    std::size_t words = 0;
    std::size_t same = 0;
    std::size_t skipped = 0;
    std::vector<std::filesystem::path> files;
    for (auto const& entry : std::filesystem::directory_iterator(fixtures))
        files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (std::filesystem::path const& file : files) {
        FixtureResult const r = run_fixture(file);
        if (r.skipped) {
            ++skipped;
            continue;
        }
        std::cout << "  " << file.filename().string() << ": " << r.same << "/" << r.words << " identical\n";
        CHECK_EQ(r.same, r.words);
        words += r.words;
        same += r.same;
    }
    CHECK(!files.empty());
    if (skipped)
        std::cerr << "SKIPPED " << skipped << " of " << files.size() << " recorded-result files: their fonts are not on this machine\n";
    std::cout << "  recorded words: " << same << "/" << words << " identical\n";

    std::vector<std::vector<std::uint8_t>> hostile { hand_built_font() };
    for (char const* name : { "NotoSansHebrew-Regular.ttf", "NotoNaskhArabic-Regular.ttf", "DejaVuSans.ttf" }) {
        if (std::optional<std::string> const path = find_font(name))
            hostile.push_back(read_file(*path));
        else
            std::cerr << "SKIP hostile copies of " << name << ": not installed\n";
    }
    hostile_tests(hostile);
    return test::report("shaping");
}
