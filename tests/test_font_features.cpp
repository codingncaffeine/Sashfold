#include "Test.h"

#include "css/FontFeatures.h"
#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/RunGlyphs.h"
#include "text/Face.h"
#include "text/FontManager.h"
#include "text/ShapedText.h"
#include "text/Shaper.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The font feature properties of CSS Fonts 4 §6 from a style sheet to the
// glyphs: their values parsed and computed, and the features a style asks
// for reaching the shaper in the order §7.2 gives them, so that a run set
// in a real font has the glyphs the shaper gives for the same features.

using namespace sashfold;
using text::FeatureSetting;
using text::FontManager;
using text::FontRequest;
using text::FontStack;
using text::TextShaping;

namespace {

std::unique_ptr<dom::Document> g_document;
css::StyleMap g_styles;

dom::Element* find_by_id(dom::Node& node, std::string_view id)
{
    if (node.is_element()) {
        auto& element = static_cast<dom::Element&>(node);
        if (dom::Attr const* attribute = element.find_attribute("id"); attribute && attribute->value == id)
            return &element;
    }
    for (dom::Node* child : node.children()) {
        if (dom::Element* found = find_by_id(*child, id))
            return found;
    }
    return nullptr;
}

void resolve(std::string_view sheet, std::string_view body)
{
    g_document = html::parse_document("<!doctype html><body>" + std::string(body));
    css::SheetSource source;
    source.text = std::string(sheet);
    g_styles = css::resolve_styles(*g_document, css::StyleSet({ source }));
}

css::ComputedStyle const& style_of(std::string_view id)
{
    static css::ComputedStyle const fallback;
    dom::Element* const element = find_by_id(*g_document, id);
    if (!element)
        return fallback;
    auto const it = g_styles.find(element);
    return it == g_styles.end() ? fallback : it->second;
}

std::optional<std::vector<std::uint8_t>> font_bytes(std::string const& name)
{
    for (char const* dir : { "/usr/share/fonts/noto/", "/usr/share/fonts/TTF/", "/usr/share/fonts/truetype/noto/",
             "/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/opentype/noto/" }) {
        std::string const path = std::string(dir) + name;
        if (!std::filesystem::exists(path))
            continue;
        std::ifstream in(path, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    std::cerr << "SKIP " << name << ": not installed\n";
    return std::nullopt;
}

constexpr std::uint32_t tag(char const (&name)[5])
{
    return text::feature_tag(name[0], name[1], name[2], name[3]);
}

// The glyph ids a run comes to.
std::vector<std::uint32_t> glyphs_of(text::ShapedText const& shaped)
{
    std::vector<std::uint32_t> out;
    for (text::PlacedGlyph const& g : shaped.glyphs)
        out.push_back(g.glyph);
    return out;
}

// The shaper's own glyphs for a run in one face with these features.
std::vector<std::uint32_t> shaper_glyphs(text::Face const& face, std::u32string_view text, std::vector<FeatureSetting> const& features,
    std::string_view language = {}, text::ScriptCode script = text::scripts::latin)
{
    text::ShapeRequest request;
    request.text = text;
    request.script = script;
    request.language = language;
    request.features = features;
    request.beginning_of_text = true;
    request.end_of_text = true;
    std::vector<std::uint32_t> out;
    for (text::ShapedGlyph const& g : text::shape(face, request).glyphs)
        out.push_back(g.glyph);
    return out;
}

std::u32string office_text()
{
    return U"office";
}

std::string feature_text(std::string_view declaration)
{
    resolve(std::string("#t { ") + std::string(declaration) + " }", "<p id=t>x</p>");
    return css::font_feature_settings_text(style_of("t").font_feature_settings);
}

void test_font_feature_settings_values()
{
    // §6.12: a tag alone is 1, on and off are 1 and 0, the last setting
    // of a tag wins, and the computed value lists the tags in order.
    CHECK_EQ(feature_text("font-feature-settings: normal"), std::string("normal"));
    CHECK_EQ(feature_text("font-feature-settings: \"dlig\""), std::string("\"dlig\""));
    CHECK_EQ(feature_text("font-feature-settings: \"smcp\" on"), std::string("\"smcp\""));
    CHECK_EQ(feature_text("font-feature-settings: 'liga' off"), std::string("\"liga\" 0"));
    CHECK_EQ(feature_text("font-feature-settings: \"tnum\", \"hist\""), std::string("\"hist\", \"tnum\""));
    CHECK_EQ(feature_text("font-feature-settings: \"dlig\", \"smcp\", \"dlig\" 0"), std::string("\"dlig\" 0, \"smcp\""));
    CHECK_EQ(feature_text("font-feature-settings: \"salt\" 3"), std::string("\"salt\" 3"));
    CHECK_EQ(feature_text("font-feature-settings: \"\\22}/*\""), std::string("\"\\\"}/*\""));
    // Not the grammar: dropped, the initial value stands.
    for (char const* wrong : { "font-feature-settings: \"silly\" off", "font-feature-settings: dlig",
             "font-feature-settings: \"tnum\" \"hist\"", "font-feature-settings: normal \"dlig\"",
             "font-feature-settings: \"c2sc\", normal", "font-feature-settings: \"liga\" -1",
             "font-feature-settings: \"liga\" 1.5", "font-feature-settings: \"lig\"", "font-feature-settings: \"liga\",",
             "font-feature-settings: \"lig\xC3\xA9\"" })
        CHECK_EQ(feature_text(wrong), std::string("normal"));
    // Inherited, and the font shorthand resets it (css-fonts-4 §2.7).
    resolve("#a { font-feature-settings: \"onum\" } #c { font: 12px serif }", "<div id=a><p id=b>x</p><p id=c>y</p></div>");
    CHECK_EQ(css::font_feature_settings_text(style_of("b").font_feature_settings), std::string("\"onum\""));
    CHECK_EQ(css::font_feature_settings_text(style_of("c").font_feature_settings), std::string("normal"));
    // The CSS-wide keywords.
    resolve("#a { font-feature-settings: \"onum\" } #b { font-feature-settings: initial } #c { font-feature-settings: \"zero\"; font-feature-settings: inherit }",
        "<div id=a><p id=b>x</p><p id=c>y</p></div>");
    CHECK(!style_of("b").font_feature_settings);
    CHECK_EQ(css::font_feature_settings_text(style_of("c").font_feature_settings), std::string("\"onum\""));
    // @supports and CSS.supports answer from the same parsing.
    CHECK(css::supports_condition_text_matches("font-feature-settings: \"liga\" 0"));
    CHECK(css::supports_condition_text_matches("font-feature-settings: normal"));
    CHECK(!css::supports_condition_text_matches("font-feature-settings: \"silly\""));
}

// A longhand's computed value after one declaration, or the shorthand's.
std::string variant_text(std::string_view property, std::string_view value, std::string_view read = {})
{
    resolve("#t { " + std::string(property) + ": " + std::string(value) + " }", "<p id=t>x</p>");
    std::string_view const asked = read.empty() ? property : read;
    if (asked == "font-variant")
        return css::font_variant_text(style_of("t").font_variant);
    return css::font_variant_longhand_text(asked, style_of("t").font_variant);
}

void test_font_variant_values()
{
    // Each longhand's keywords come back in the grammar's order (§6.4 to
    // §6.10).
    CHECK_EQ(variant_text("font-variant-ligatures", "none"), std::string("none"));
    CHECK_EQ(variant_text("font-variant-ligatures", "no-contextual common-ligatures"), std::string("common-ligatures no-contextual"));
    CHECK_EQ(variant_text("font-variant-ligatures", "historical-ligatures discretionary-ligatures"),
        std::string("discretionary-ligatures historical-ligatures"));
    CHECK_EQ(variant_text("font-variant-caps", "all-petite-caps"), std::string("all-petite-caps"));
    CHECK_EQ(variant_text("font-variant-numeric", "slashed-zero tabular-nums oldstyle-nums diagonal-fractions"),
        std::string("oldstyle-nums tabular-nums diagonal-fractions slashed-zero"));
    CHECK_EQ(variant_text("font-variant-east-asian", "ruby full-width simplified"), std::string("simplified full-width ruby"));
    CHECK_EQ(variant_text("font-variant-position", "super"), std::string("super"));
    CHECK_EQ(variant_text("font-variant-alternates", "swash(a) historical-forms styleset(b, c)"),
        std::string("historical-forms styleset(b, c) swash(a)"));
    // Not the grammar: dropped.
    for (auto const& [property, value] : std::initializer_list<std::pair<char const*, char const*>> {
             { "font-variant-ligatures", "common-ligatures no-common-ligatures" }, { "font-variant-ligatures", "none contextual" },
             { "font-variant-ligatures", "small-caps" }, { "font-variant-caps", "small-caps all-small-caps" },
             { "font-variant-numeric", "lining-nums oldstyle-nums" }, { "font-variant-numeric", "ordinal ordinal" },
             { "font-variant-east-asian", "jis78 jis83" }, { "font-variant-position", "sub super" },
             { "font-variant-alternates", "swash(a, b)" }, { "font-variant-alternates", "swash" },
             { "font-variant-alternates", "annotation()" }, { "font-variant-alternates", "swash(one) swash(two)" },
             { "font-variant-alternates", "stylistic(initial)" } })
        CHECK_EQ(variant_text(property, value), std::string("normal"));

    // The shorthand (§6.11): any of the longhands' values, written back in
    // the grammar's order; none is the ligatures' none alone.
    CHECK_EQ(variant_text("font-variant", "normal"), std::string("normal"));
    CHECK_EQ(variant_text("font-variant", "none"), std::string("none"));
    CHECK_EQ(variant_text("font-variant", "super proportional-width jis83 stacked-fractions tabular-nums oldstyle-nums historical-forms "
                                          "all-small-caps no-contextual no-historical-ligatures no-discretionary-ligatures no-common-ligatures"),
        std::string("no-common-ligatures no-discretionary-ligatures no-historical-ligatures no-contextual all-small-caps "
                    "historical-forms oldstyle-nums tabular-nums stacked-fractions jis83 proportional-width super"));
    CHECK_EQ(variant_text("font-variant", "small-caps tabular-nums", "font-variant-numeric"), std::string("tabular-nums"));
    for (char const* wrong : { "normal none", "small-caps normal", "none small-caps", "sub super", "ruby sub ruby", "bogus" })
        CHECK_EQ(variant_text("font-variant", wrong), std::string("normal"));
    // The shorthand resets the longhands it does not name; a longhand
    // after it changes its part alone.
    resolve("#t { font-variant-numeric: ordinal; font-variant: small-caps; font-variant-position: sub }", "<p id=t>x</p>");
    CHECK_EQ(css::font_variant_text(style_of("t").font_variant), std::string("small-caps sub"));
    // Ligatures none with another longhand cannot be said by the shorthand.
    resolve("#t { font-variant-ligatures: none; font-variant-caps: unicase }", "<p id=t>x</p>");
    CHECK_EQ(css::font_variant_text(style_of("t").font_variant), std::string(""));

    // Inherited; the font shorthand takes small-caps alone of it and
    // resets the rest, font-kerning and font-stretch with it (§2.7).
    resolve("#a { font-variant: oldstyle-nums jis90; font-kerning: none; font-stretch: 150% } #c { font: small-caps condensed 12px serif }",
        "<div id=a><p id=b>x</p><p id=c>y</p></div>");
    CHECK_EQ(css::font_variant_text(style_of("b").font_variant), std::string("oldstyle-nums jis90"));
    CHECK_EQ(css::font_variant_text(style_of("c").font_variant), std::string("small-caps"));
    CHECK(style_of("c").font_kerning == css::FontKerning::Auto);
    CHECK_EQ(style_of("c").font_stretch, 75);
    CHECK_EQ(style_of("c").font_size, 12.0f);
    // CSS 3's caps are not the shorthand's: the declaration is dropped.
    resolve("#a { font-variant: ordinal } #c { font: all-small-caps 12px serif }", "<div id=a><p id=c>y</p></div>");
    CHECK_EQ(css::font_variant_text(style_of("c").font_variant), std::string("ordinal"));
    // The CSS-wide keywords on a longhand reach its part alone.
    resolve("#a { font-variant: small-caps ordinal } #b { font-variant: sub; font-variant-caps: inherit }",
        "<div id=a><p id=b>x</p></div>");
    CHECK_EQ(css::font_variant_text(style_of("b").font_variant), std::string("small-caps sub"));
    CHECK(css::supports_condition_text_matches("font-variant: normal"));
    CHECK(css::supports_condition_text_matches("font-variant-position: normal"));
    CHECK(css::supports_condition_text_matches("font-variant-alternates: normal"));
    CHECK(css::supports_condition_text_matches("font-variant-ligatures: none"));
    CHECK(!css::supports_condition_text_matches("font-variant: sub super"));
}

// font-language-override (§6.13) and the element's language (HTML
// §3.2.6.2), which the fonts' language systems are chosen by.
void test_language_values()
{
    auto const override_text = [](std::string_view value) {
        resolve("#t { font-language-override: " + std::string(value) + " }", "<p id=t>x</p>");
        return css::font_language_override_text(style_of("t").font_language_override);
    };
    CHECK_EQ(override_text("normal"), std::string("normal"));
    CHECK_EQ(override_text("\"KSW\""), std::string("\"KSW\""));
    CHECK_EQ(override_text("\"ENG \""), std::string("\"ENG\""));
    CHECK_EQ(override_text("\" en \""), std::string("\" en\""));
    CHECK_EQ(override_text("\"1 %\""), std::string("\"1 %\""));
    for (char const* wrong : { "auto", "normal \"ksw\"", "\"turkish\"", "\"x\xC3\xB8x\"", "\"\"", "\"ENG  \"", "SRB" })
        CHECK_EQ(override_text(wrong), std::string("normal"));
    CHECK(css::supports_condition_text_matches("font-language-override: normal"));
    CHECK(!css::supports_condition_text_matches("font-language-override: \"turkish\""));
    // Inherited; the font shorthand resets it.
    resolve("#a { font-language-override: \"SRB\" } #c { font: 12px serif }", "<div id=a><p id=b>x</p><p id=c>y</p></div>");
    CHECK_EQ(style_of("b").font_language_override, std::string("SRB"));
    CHECK(style_of("c").font_language_override.empty());

    // The language: the nearest lang above, an empty one saying it is
    // unknown; in foreign content xml:lang over lang.
    resolve("", "<div id=a lang=sr><p id=b>x<span id=c lang=\"\">y</span></p></div><p id=d>z</p>"
                "<svg id=e lang=fr xml:lang=sr-Latn><text id=f>t</text></svg>");
    CHECK_EQ(style_of("a").language, std::string("sr"));
    CHECK_EQ(style_of("b").language, std::string("sr"));
    CHECK_EQ(style_of("c").language, std::string(""));
    CHECK_EQ(style_of("d").language, std::string(""));
    CHECK_EQ(style_of("f").language, std::string("sr-Latn"));
    // The shaping asks for the language, or the override in its place.
    CHECK_EQ(layout::shaping_for(style_of("b"), false).language, std::string("sr"));
    resolve("#b { font-language-override: \"TRK\" }", "<div lang=sr><p id=b>x</p></div>");
    CHECK_EQ(layout::shaping_for(style_of("b"), false).language, std::string("x-ot-TRK"));
    CHECK_EQ(text::opentype_language_tag(layout::shaping_for(style_of("b"), false).language), text::script_code('T', 'R', 'K', ' '));
}

// The features a style's text is shaped with: what font-kerning and the
// ligature switches ask for first, font-feature-settings last (§7.2).
void test_feature_order()
{
    resolve("#k { font-kerning: none; font-feature-settings: \"kern\" 1 } #n { font-feature-settings: \"kern\" 0 }",
        "<p id=k>x</p><p id=n>y</p>");
    TextShaping const shaping = layout::shaping_for(style_of("k"), false);
    CHECK(shaping.features.size() >= 2u);
    if (shaping.features.size() >= 2u) {
        CHECK_EQ(shaping.features.front().tag, tag("kern"));
        CHECK_EQ(shaping.features.front().value, 0u);
        CHECK_EQ(shaping.features.back().tag, tag("kern"));
        CHECK_EQ(shaping.features.back().value, 1u);
    }
    // Text in both may not be shaped as one.
    CHECK(!layout::same_font_features(style_of("k"), style_of("n")));
    CHECK(layout::same_font_features(style_of("k"), style_of("k")));

    // font-variant's features come before font-feature-settings', which
    // can turn one of them off again; the caps and the position are asked
    // of each face as it is met.
    resolve("#v { font-variant: small-caps tabular-nums super; font-feature-settings: \"tnum\" 0; font-synthesis: none }",
        "<p id=v>x</p>");
    TextShaping const variant = layout::shaping_for(style_of("v"), false);
    std::vector<FeatureSetting> const& features = variant.features;
    std::size_t tnum_on = features.size();
    std::size_t tnum_off = features.size();
    for (std::size_t i = 0; i < features.size(); ++i) {
        if (features[i].tag == tag("tnum"))
            (features[i].value ? tnum_on : tnum_off) = i;
    }
    CHECK(tnum_on < tnum_off && tnum_off == features.size() - 1);
    CHECK(variant.caps == text::CapsVariant::SmallCaps);
    CHECK(variant.position == text::PositionVariant::Super);
    CHECK(!variant.synthesize_small_caps && !variant.synthesize_position);
}

}

int main()
{
    test_font_feature_settings_values();
    test_font_variant_values();
    test_language_values();
    test_feature_order();

    // --- Real fonts: the glyphs the shaper gives for the same features -----------------
    FontManager& manager = FontManager::instance();
    manager.set_system_fonts(false);
    std::optional<std::vector<std::uint8_t>> const serif_italic = font_bytes("NotoSerif-Italic.ttf");
    std::optional<std::vector<std::uint8_t>> const sans = font_bytes("DejaVuSans.ttf");
    if (!serif_italic || !sans)
        return test::report("font_features");
    manager.set_page_fonts({ text::PageFont { "TestSerif", 400, true, *serif_italic, 100, {} },
        text::PageFont { "TestSans", 400, false, *sans, 100, {} } });
    FontStack const& serif = manager.resolve(FontRequest { { "TestSerif" }, 400, true });
    FontStack const& dejavu = manager.resolve(FontRequest { { "TestSans" }, 400, false });
    text::Face const& serif_face = *serif.faces()[0];
    text::Face const& sans_face = *dejavu.faces()[0];

    // font-feature-settings reaches the shaper: small capitals and
    // old-style figures from the font's own tables, and a ligature turned
    // off.
    {
        resolve("#f { font-feature-settings: \"smcp\", \"onum\" }", "<p id=f>x</p>");
        TextShaping const shaping = layout::shaping_for(style_of("f"), false);
        std::u32string const sample = U"Small 2024";
        std::vector<std::uint32_t> const plain = glyphs_of(*serif.shape(sample, layout::shaping_for(css::ComputedStyle {}, false)));
        std::vector<std::uint32_t> const featured = glyphs_of(*serif.shape(sample, shaping));
        CHECK(featured != plain);
        CHECK(featured == shaper_glyphs(serif_face, sample, shaping.features));
        CHECK(featured == shaper_glyphs(serif_face, sample, { FeatureSetting { tag("smcp"), 1 }, FeatureSetting { tag("onum"), 1 } }));

        resolve("#l { font-feature-settings: \"liga\" 0 }", "<p id=l>x</p>");
        std::u32string const office = U"office";
        CHECK(dejavu.shape(office, layout::shaping_for(css::ComputedStyle {}, false))->glyphs.size() < 6u);
        auto const unligated = dejavu.shape(office, layout::shaping_for(style_of("l"), false));
        CHECK_EQ(unligated->glyphs.size(), 6u);
        CHECK(glyphs_of(*unligated) == shaper_glyphs(sans_face, office, { FeatureSetting { tag("liga"), 0 } }));
    }

    // font-variant's longhands set the glyphs their features name, the
    // same as font-feature-settings naming those features.
    {
        resolve("#v1 { font-variant-numeric: oldstyle-nums tabular-nums } #f1 { font-feature-settings: \"onum\", \"tnum\" }"
                "#v2 { font-variant-numeric: diagonal-fractions slashed-zero } #f2 { font-feature-settings: \"frac\", \"zero\" }"
                "#v3 { font-variant-caps: all-small-caps } #f3 { font-feature-settings: \"c2sc\", \"smcp\" }"
                "#v4 { font-variant-numeric: ordinal } #f4 { font-feature-settings: \"ordn\" }"
                "#v5 { font-variant-ligatures: none } #f5 { font-feature-settings: \"liga\" 0, \"clig\" 0, \"calt\" 0, \"dlig\" 0, \"hlig\" 0 }"
                "#v6 { font-variant-ligatures: discretionary-ligatures } #f6 { font-feature-settings: \"dlig\" }",
            "<p id=v1>a</p><p id=f1>b</p><p id=v2>c</p><p id=f2>d</p><p id=v3>e</p><p id=f3>f</p><p id=v4>g</p><p id=f4>h</p>"
            "<p id=v5>i</p><p id=f5>j</p><p id=v6>k</p><p id=f6>l</p>");
        std::u32string const figures = U"1/2 2024 Ab 1a 2o No 0";
        TextShaping const plain = layout::shaping_for(css::ComputedStyle {}, false);
        for (auto const& [variant, settings] : std::initializer_list<std::pair<char const*, char const*>> {
                 { "v1", "f1" }, { "v2", "f2" }, { "v3", "f3" }, { "v4", "f4" } }) {
            std::vector<std::uint32_t> const by_variant = glyphs_of(*serif.shape(figures, layout::shaping_for(style_of(variant), false)));
            std::vector<std::uint32_t> const by_settings = glyphs_of(*serif.shape(figures, layout::shaping_for(style_of(settings), false)));
            CHECK(by_variant == by_settings);
            CHECK(by_variant != glyphs_of(*serif.shape(figures, plain)));
        }
        // DejaVu Sans has the ligatures: none of them with none.
        std::u32string const ligated = U"office ct";
        auto const none = dejavu.shape(ligated, layout::shaping_for(style_of("v5"), false));
        CHECK_EQ(none->glyphs.size(), ligated.size());
        CHECK(glyphs_of(*none) == glyphs_of(*dejavu.shape(ligated, layout::shaping_for(style_of("f5"), false))));
        CHECK(glyphs_of(*dejavu.shape(ligated, layout::shaping_for(style_of("v6"), false)))
            == glyphs_of(*dejavu.shape(ligated, layout::shaping_for(style_of("f6"), false))));
        // Justification between letters turns the optional ligatures off,
        // as letter-spacing does (css-text-3 §8.2); between words it does
        // not.
        resolve("#ic { text-align: justify; text-justify: inter-character } #iw { text-align: justify; text-justify: inter-word }",
            "<p id=ic>a</p><p id=iw>b</p>");
        CHECK_EQ(dejavu.shape(office_text(), layout::shaping_for(style_of("ic"), false))->glyphs.size(), 6u);
        CHECK(dejavu.shape(office_text(), layout::shaping_for(style_of("iw"), false))->glyphs.size() < 6u);
        CHECK(!layout::same_font_features(style_of("ic"), style_of("iw")));
    }

    // Small capitals (§6.6): the face's own where it has them; scaled
    // capitals of the lowercase letters where it does not, unless
    // font-synthesis-small-caps says none; caseless characters unchanged.
    {
        resolve("#s { font-variant: small-caps } #n { font-variant: small-caps; font-synthesis-small-caps: none }"
                "#a { font-variant: all-small-caps } #u { font-variant: unicase } #p { font-variant: petite-caps }",
            "<p id=s>a</p><p id=n>b</p><p id=a>c</p><p id=u>d</p><p id=p>e</p>");
        float const size = 32;
        auto const at = [&](char const* id) {
            TextShaping shaping = layout::shaping_for(style_of(id), false);
            shaping.size = size;
            return shaping;
        };
        TextShaping plain;
        plain.size = size;
        std::u32string const word = U"Ab1";
        // Noto Serif has 'smcp': its glyphs, at full size.
        auto const own = serif.shape(word, at("s"));
        CHECK(glyphs_of(*own) == shaper_glyphs(serif_face, word, { FeatureSetting { tag("smcp"), 1 } }));
        CHECK(own->glyphs.size() == 3u && own->glyphs[1].scale == 1.0f);
        // Petite capitals it lacks are small capitals.
        CHECK(glyphs_of(*serif.shape(word, at("p"))) == glyphs_of(*own));
        // Unicase it lacks: the capitals lowercased, in its small capitals.
        auto const unicase = serif.shape(U"Ab", at("u"));
        std::vector<std::uint32_t> const small_a = shaper_glyphs(serif_face, U"a", { FeatureSetting { tag("smcp"), 1 } });
        CHECK(unicase->glyphs.size() == 2u && small_a.size() == 1u && unicase->glyphs[0].glyph == small_a[0]);
        CHECK(unicase->glyphs.size() == 2u && unicase->glyphs[1].glyph == shaper_glyphs(serif_face, U"b", {})[0]);
        // DejaVu Sans has none: "b" is a "B" at seven tenths, "A" and "1"
        // as they are, and the width follows.
        auto const made = dejavu.shape(word, at("s"));
        auto const capitals = dejavu.shape(U"AB1", plain);
        CHECK_EQ(made->glyphs.size(), 3u);
        if (made->glyphs.size() == 3u && capitals->glyphs.size() == 3u) {
            CHECK(made->glyphs[0].glyph == capitals->glyphs[0].glyph && made->glyphs[0].scale == 1.0f);
            CHECK(made->glyphs[1].glyph == capitals->glyphs[1].glyph && made->glyphs[1].scale == 0.7f);
            CHECK(made->glyphs[1].cluster == 1u);
            CHECK(std::fabs(made->glyphs[1].advance - capitals->glyphs[1].advance * 0.7f) < 0.01f);
            CHECK(made->glyphs[2].glyph == capitals->glyphs[2].glyph && made->glyphs[2].scale == 1.0f);
        }
        // Not synthesized when font-synthesis says none.
        CHECK(glyphs_of(*dejavu.shape(word, at("n"))) == glyphs_of(*dejavu.shape(word, plain)));
        // all-small-caps: the capitals scaled too.
        auto const all = dejavu.shape(word, at("a"));
        CHECK(all->glyphs.size() == 3u && all->glyphs[0].scale == 0.7f && all->glyphs[1].scale == 0.7f && all->glyphs[2].scale == 1.0f);
    }

    // The language reaches the shaper: Serbian Cyrillic italics take their
    // own forms ('locl' in the font's SRB language system), from lang or
    // from font-language-override; the text's language alone does not.
    {
        resolve("#sr { font-language-override: normal } #ov { font-language-override: \"SRB\" } #mk { }",
            "<p id=ru lang=ru>a</p><p id=sr lang=sr-Cyrl>b</p><p id=ov lang=ru>c</p><p id=mk lang=mk>d</p><p id=none>e</p>");
        std::u32string const cyrillic = U"бгдпт";
        auto const set = [&](char const* id) { return glyphs_of(*serif.shape(cyrillic, layout::shaping_for(style_of(id), false))); };
        std::vector<std::uint32_t> const russian = set("ru");
        std::vector<std::uint32_t> const serbian = set("sr");
        CHECK(serbian != russian);
        CHECK(serbian == shaper_glyphs(serif_face, cyrillic, {}, "sr", text::scripts::cyrillic));
        CHECK(russian == shaper_glyphs(serif_face, cyrillic, {}, "", text::scripts::cyrillic));
        CHECK(set("none") == russian);
        CHECK(set("ov") == serbian);
        CHECK(set("mk") == shaper_glyphs(serif_face, cyrillic, {}, "mk", text::scripts::cyrillic));
        CHECK(set("mk") != russian);
    }

    // Super- and subscripts (§6.5): the face's own forms when it has one
    // for every character of the run; else every character scaled and
    // moved, so the two never mix.
    {
        resolve("#sup { font-variant-position: super } #sub { font-variant-position: sub }"
                "#none { font-variant-position: super; font-synthesis-position: none }",
            "<p id=sup>a</p><p id=sub>b</p><p id=none>c</p>");
        float const size = 32;
        auto const at = [&](char const* id) {
            TextShaping shaping = layout::shaping_for(style_of(id), false);
            shaping.size = size;
            return shaping;
        };
        TextShaping plain;
        plain.size = size;
        // Noto Serif has superior figures: its own glyphs, full size, on
        // the baseline.
        auto const own = serif.shape(U"123", at("sup"));
        CHECK(glyphs_of(*own) == shaper_glyphs(serif_face, U"123", { FeatureSetting { tag("sups"), 1 } }));
        CHECK(glyphs_of(*own) != glyphs_of(*serif.shape(U"123", plain)));
        CHECK(own->glyphs.size() == 3u && own->glyphs[0].scale == 1.0f && own->glyphs[0].y_offset == 0.0f);
        // DejaVu Sans has none: the plain glyphs, smaller, raised a third
        // of an em, or lowered a fifth.
        auto const made = dejavu.shape(U"x2", at("sup"));
        auto const normal = dejavu.shape(U"x2", plain);
        CHECK(glyphs_of(*made) == glyphs_of(*normal));
        CHECK(made->glyphs.size() == 2u && made->glyphs[0].scale < 0.7f && made->glyphs[0].y_offset < -size / 3.0f + 0.01f);
        CHECK(made->width < normal->width);
        auto const lowered = dejavu.shape(U"x2", at("sub"));
        CHECK(lowered->glyphs.size() == 2u && std::fabs(lowered->glyphs[1].y_offset - size / 5.0f) < 0.01f);
        // In Noto Serif "1x" has a superior one but no superior x: the run
        // is synthesized whole.
        auto const mixed = serif.shape(U"1x", at("sup"));
        CHECK(glyphs_of(*mixed) == glyphs_of(*serif.shape(U"1x", plain)));
        CHECK(mixed->glyphs.size() == 2u && mixed->glyphs[0].scale < 1.0f && mixed->glyphs[1].scale < 1.0f);
        // font-synthesis-position: none: the forms the face has, and the
        // rest as they are.
        auto const unsynthesized = serif.shape(U"1x", at("none"));
        CHECK(glyphs_of(*unsynthesized) == shaper_glyphs(serif_face, U"1x", { FeatureSetting { tag("sups"), 1 } }));
        CHECK(unsynthesized->glyphs.size() == 2u && unsynthesized->glyphs[1].scale == 1.0f);
    }
    return test::report("font_features");
}
