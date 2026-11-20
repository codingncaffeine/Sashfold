#include "layout/RunGlyphs.h"

#include "core/Bidi.h"
#include "css/ComputedStyle.h"
#include "css/FontFeatures.h"
#include "text/FontManager.h"

#include <algorithm>
#include <cstdint>

namespace sashfold::layout {

text::TextShaping shaping_for(css::ComputedStyle const& style, bool right_to_left, bool paragraph)
{
    text::TextShaping shaping;
    shaping.size = style.font_size;
    shaping.letter_spacing = style.letter_spacing;
    shaping.word_spacing = style.word_spacing;
    shaping.direction = right_to_left ? text::TextDirection::RightToLeft : text::TextDirection::LeftToRight;
    shaping.paragraph = paragraph;
    text::CssFontFeatures css;
    switch (style.font_kerning) {
    case css::FontKerning::Auto: css.kerning = text::CssFontFeatures::Kerning::Auto; break;
    case css::FontKerning::Normal: css.kerning = text::CssFontFeatures::Kerning::Normal; break;
    case css::FontKerning::None: css.kerning = text::CssFontFeatures::Kerning::None; break;
    }
    // font-variant-ligatures (css-fonts-4 §6.4): none turns off every
    // ligature and contextual form the property covers.
    using V = css::FontVariant;
    std::uint16_t const ligatures = style.font_variant.ligatures;
    bool const none = (ligatures & V::ligatures_none) != 0;
    css.common_ligatures = !none && !(ligatures & V::no_common_ligatures);
    css.discretionary_ligatures = !none && (ligatures & V::discretionary_ligatures);
    css.historical_ligatures = !none && (ligatures & V::historical_ligatures);
    css.contextual_alternates = !none && !(ligatures & V::no_contextual);
    css.letter_spacing = style.letter_spacing != 0;
    text::shaping_features_for_css(css, shaping.features);
    // The other longhands of font-variant (§6.5 to §6.10), then
    // font-feature-settings last, over everything the other properties
    // asked for (§7.2): a later setting of a tag wins.
    css::FontVariant const& asked = style.font_variant;
    if (asked.caps != css::FontVariantCaps::Normal || asked.numeric != 0 || asked.east_asian != 0
        || asked.position != css::FontVariantPosition::Normal || asked.historical_forms) {
        std::vector<css::FontFeature> variant;
        css::append_font_variant_features(asked, variant);
        for (css::FontFeature const& feature : variant)
            shaping.features.push_back(text::FeatureSetting { feature.tag, feature.value });
    }
    if (style.font_feature_settings) {
        for (css::FontFeature const& feature : *style.font_feature_settings)
            shaping.features.push_back(text::FeatureSetting { feature.tag, feature.value });
    }
    return shaping;
}

bool same_font_features(css::ComputedStyle const& a, css::ComputedStyle const& b)
{
    bool const same_settings = a.font_feature_settings == b.font_feature_settings
        || (a.font_feature_settings && b.font_feature_settings && *a.font_feature_settings == *b.font_feature_settings);
    return a.font_kerning == b.font_kerning && same_settings && a.font_variant == b.font_variant;
}

std::u32string logical_text_of(TextRun const& run)
{
    if (!run.source_reversed)
        return run.text;
    std::u32string logical(run.text.rbegin(), run.text.rend());
    for (char32_t& c : logical)
        c = bidi_mirrored(c);
    return logical;
}

std::shared_ptr<text::ShapedText const> shaped_of(TextRun const& run)
{
    if (run.shaped)
        return run.shaped;
    if (!run.style || !run.fonts)
        return std::make_shared<text::ShapedText>();
    return run.fonts->shape(logical_text_of(run), shaping_for(*run.style, run.source_reversed, !run.source_reversed));
}

std::shared_ptr<text::ShapedText const> shaped_slice(TextRun const& run, std::size_t from, std::size_t to)
{
    to = std::min(to, run.text.size());
    from = std::min(from, to);
    if (!run.style || !run.fonts)
        return std::make_shared<text::ShapedText>();
    std::u32string const logical = logical_text_of(run);
    // The slice in reading order.
    std::size_t const start = run.source_reversed ? run.text.size() - to : from;
    std::size_t const end = run.source_reversed ? run.text.size() - from : to;
    std::u32string_view const all(logical);
    return run.fonts->shape(all.substr(start, end - start), shaping_for(*run.style, run.source_reversed, !run.source_reversed),
        all.substr(0, start), all.substr(end));
}

std::vector<float> run_character_stops(TextRun const& run)
{
    std::shared_ptr<text::ShapedText const> const shaped = shaped_of(run);
    std::size_t const n = run.text.size();
    std::vector<float> stops(n + 1, 0.0f);
    std::vector<float> left;
    std::vector<float> right;
    shaped->character_edges(left, right);
    if (left.size() != n) {
        // Shaped from other text than the run holds (cannot happen through
        // layout): the run's own steps, evenly.
        for (std::size_t i = 0; i <= n; ++i)
            stops[i] = n == 0 ? 0.0f : shaped->pen_end * static_cast<float>(i) / static_cast<float>(n);
        return stops;
    }
    for (std::size_t i = 0; i < n; ++i)
        stops[i] = run.source_reversed ? left[n - 1 - i] : left[i];
    stops[n] = shaped->pen_end;
    return stops;
}

std::vector<float> caret_stops(text::ShapedText const& shaped)
{
    std::size_t const n = shaped.length;
    std::vector<float> stops(n + 1, 0.0f);
    if (n == 0)
        return stops;
    std::vector<float> left;
    std::vector<float> right;
    shaped.character_edges(left, right);
    auto const rtl = [&](std::size_t i) { return !shaped.right_to_left.empty() && shaped.right_to_left[i]; };
    for (std::size_t i = 0; i < n; ++i)
        stops[i] = rtl(i) ? right[i] : left[i];
    stops[n] = rtl(n - 1) ? left[n - 1] : right[n - 1];
    return stops;
}

bool run_cut_allowed(TextRun const& run, std::size_t at)
{
    std::size_t const n = run.text.size();
    if (at == 0 || at >= n)
        return true;
    // The built-in face alone sets a glyph per character.
    if (!run.shaped && (!run.fonts || run.fonts->builtin_alone()))
        return true;
    std::shared_ptr<text::ShapedText const> const shaped = shaped_of(run);
    if (shaped->length != n)
        return true;
    return shaped->starts_cluster(run.source_reversed ? n - at : at);
}

}
