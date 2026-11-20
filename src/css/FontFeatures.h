#pragma once

// The font feature properties of CSS Fonts 4 §6: their values parsed from
// a declaration's component values, and their computed values written as
// getComputedStyle answers them. StyleResolver.cpp applies them, and
// layout turns them into the OpenType features a run is shaped with.

#include "css/ComputedStyle.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::css {

struct ComponentValue;

// font-feature-settings: normal | <feature-tag-value>#, where
// <feature-tag-value> = <opentype-tag> [ <integer [0,∞]> | on | off ]?
// (§6.12). A tag is a string of exactly four characters from U+20 to U+7E.
// The computed value keeps the last setting of each tag, the tags in
// ascending order; normal is null. False when the value is not the
// grammar (`out` is then untouched).
bool parse_font_feature_settings(std::vector<ComponentValue const*> const& values,
    std::shared_ptr<std::vector<FontFeature> const>& out);
std::string font_feature_settings_text(std::shared_ptr<std::vector<FontFeature> const> const&);

// Whether `property` is one of the longhands of font-variant this engine
// keeps: font-variant-ligatures, -caps, -numeric, -east-asian, -position
// and -alternates.
bool is_font_variant_longhand(std::string_view property);
// One longhand's value (§6.4 to §6.10) into its part of `variant`, the
// other parts untouched; false when the value is not its grammar.
bool parse_font_variant_longhand(std::string_view property, std::vector<ComponentValue const*> const& values, FontVariant& variant);
// The shorthand (§6.11): normal resets every longhand, none sets
// font-variant-ligatures to none and resets the rest, and any other value
// is the longhands' values in any order, each longhand not named reset.
bool parse_font_variant(std::vector<ComponentValue const*> const& values, FontVariant& variant);
// A longhand's computed value, its keywords in the grammar's order.
std::string font_variant_longhand_text(std::string_view property, FontVariant const&);
// The shorthand's: normal, none, or the longhands' values in the
// grammar's order; empty when it cannot say them (ligatures none with
// another longhand set).
std::string font_variant_text(FontVariant const&);
// The OpenType features the longhands ask for (§6.4 to §6.10), appended
// in the longhands' order: what layout shapes with before
// font-feature-settings. Ligatures are left to the caller's
// shaping_features_for_css, which knows letter-spacing; small capitals
// and super- and subscripts the font lacks are synthesized by the caller.
void append_font_variant_features(FontVariant const&, std::vector<FontFeature>& out);

// font-language-override: normal | <string> (§6.13), the string an
// OpenType language system tag: one to four characters from U+20 to U+7E,
// padded with spaces to four. Kept with its trailing spaces dropped (the
// shortest serialization), empty for normal.
bool parse_font_language_override(std::vector<ComponentValue const*> const& values, std::string& out);
std::string font_language_override_text(std::string const&);

// A string as CSSOM §2.1 serializes one: in double quotes, a quote or a
// backslash escaped, control characters as code points.
std::string css_string_text(std::string_view);

}
