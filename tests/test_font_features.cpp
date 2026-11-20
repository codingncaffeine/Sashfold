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
}

}

int main()
{
    test_font_feature_settings_values();
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
    return test::report("font_features");
}
