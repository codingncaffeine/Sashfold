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

// A string as CSSOM §2.1 serializes one: in double quotes, a quote or a
// backslash escaped, control characters as code points.
std::string css_string_text(std::string_view);

}
