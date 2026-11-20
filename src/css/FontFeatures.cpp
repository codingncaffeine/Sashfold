#include "css/FontFeatures.h"

#include "core/Ascii.h"
#include "css/Parser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace sashfold::css {

namespace {

bool is_ident(ComponentValue const* value, std::string_view name)
{
    return value->is_token(Token::Type::Ident) && ascii_ci_equals(value->token().value, name);
}

// <opentype-tag>: a string of exactly four characters, each from U+20 to
// U+7E (§6.12); a byte outside that range is a character outside it, since
// the string is UTF-8.
bool opentype_tag(ComponentValue const* value, std::uint32_t& tag)
{
    if (!value->is_token(Token::Type::String))
        return false;
    std::string const& text = value->token().value;
    if (text.size() != 4)
        return false;
    tag = 0;
    for (char const c : text) {
        unsigned char const byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7E)
            return false;
        tag = tag << 8 | byte;
    }
    return true;
}

}

bool parse_font_feature_settings(std::vector<ComponentValue const*> const& values,
    std::shared_ptr<std::vector<FontFeature> const>& out)
{
    if (values.size() == 1 && is_ident(values[0], "normal")) {
        out = nullptr;
        return true;
    }
    std::vector<FontFeature> features;
    std::size_t at = 0;
    while (at < values.size()) {
        FontFeature feature;
        if (!opentype_tag(values[at], feature.tag))
            return false;
        ++at;
        if (at < values.size() && !values[at]->is_token(Token::Type::Comma)) {
            ComponentValue const* const setting = values[at];
            if (is_ident(setting, "on")) {
                feature.value = 1;
            } else if (is_ident(setting, "off")) {
                feature.value = 0;
            } else if (setting->is_token(Token::Type::Number)
                && setting->token().numeric_type == Token::NumericType::Integer && setting->token().numeric_value >= 0) {
                // Past what a font can hold, the value is the largest one
                // there is: what a font does with it is undefined anyway.
                feature.value = static_cast<std::uint32_t>(std::min(setting->token().numeric_value, 4294967295.0));
            } else {
                return false;
            }
            ++at;
        }
        features.push_back(feature);
        if (at == values.size())
            break;
        if (!values[at]->is_token(Token::Type::Comma) || at + 1 == values.size())
            return false;
        ++at;
    }
    if (features.empty())
        return false;
    // A map, in effect: the last setting of a tag wins, and the computed
    // value lists the tags in ascending order (§6.12).
    std::stable_sort(features.begin(), features.end(),
        [](FontFeature const& a, FontFeature const& b) { return a.tag < b.tag; });
    std::vector<FontFeature> kept;
    for (std::size_t i = 0; i < features.size(); ++i) {
        if (i + 1 < features.size() && features[i + 1].tag == features[i].tag)
            continue;
        kept.push_back(features[i]);
    }
    out = std::make_shared<std::vector<FontFeature> const>(std::move(kept));
    return true;
}

std::string css_string_text(std::string_view text)
{
    std::string out = "\"";
    for (char const c : text) {
        unsigned char const byte = static_cast<unsigned char>(c);
        if (byte == 0) {
            out += "\xEF\xBF\xBD";
        } else if (byte < 0x20 || byte == 0x7F) {
            constexpr char digits[] = "0123456789abcdef";
            out += '\\';
            if (byte >= 0x10)
                out += digits[byte >> 4];
            out += digits[byte & 0xF];
            out += ' ';
        } else {
            if (c == '"' || c == '\\')
                out += '\\';
            out += c;
        }
    }
    return out + "\"";
}

std::string font_feature_settings_text(std::shared_ptr<std::vector<FontFeature> const> const& features)
{
    if (!features || features->empty())
        return "normal";
    std::string out;
    for (FontFeature const& feature : *features) {
        if (!out.empty())
            out += ", ";
        std::string tag;
        for (int shift = 24; shift >= 0; shift -= 8)
            tag += static_cast<char>((feature.tag >> shift) & 0xFF);
        out += css_string_text(tag);
        // 1 is what a tag alone means, and is left out (§6.12).
        if (feature.value != 1)
            out += " " + std::to_string(feature.value);
    }
    return out;
}

}
