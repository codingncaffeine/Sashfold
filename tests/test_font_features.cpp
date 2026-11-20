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
    std::string_view language = {})
{
    text::ShapeRequest request;
    request.text = text;
    request.script = text::scripts::latin;
    request.language = language;
    request.features = features;
    request.beginning_of_text = true;
    request.end_of_text = true;
    std::vector<std::uint32_t> out;
    for (text::ShapedGlyph const& g : text::shape(face, request).glyphs)
        out.push_back(g.glyph);
    return out;
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
    // can turn one of them off again.
    resolve("#v { font-variant: small-caps tabular-nums; font-feature-settings: \"tnum\" 0 }", "<p id=v>x</p>");
    std::vector<FeatureSetting> const features = layout::shaping_for(style_of("v"), false).features;
    std::size_t smcp = features.size();
    std::size_t tnum_on = features.size();
    std::size_t tnum_off = features.size();
    for (std::size_t i = 0; i < features.size(); ++i) {
        if (features[i].tag == tag("smcp") && features[i].value == 1)
            smcp = i;
        if (features[i].tag == tag("tnum"))
            (features[i].value ? tnum_on : tnum_off) = i;
    }
    CHECK(smcp < features.size() && tnum_on < tnum_off && tnum_off == features.size() - 1);
}

}

int main()
{
    test_font_feature_settings_values();
    test_font_variant_values();
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
    }
    return test::report("font_features");
}
