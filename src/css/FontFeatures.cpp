#include "css/FontFeatures.h"

#include "core/Ascii.h"
#include "css/Parser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>

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

bool parse_font_language_override(std::vector<ComponentValue const*> const& values, std::string& out)
{
    if (values.size() != 1)
        return false;
    if (is_ident(values[0], "normal")) {
        out.clear();
        return true;
    }
    if (!values[0]->is_token(Token::Type::String))
        return false;
    std::string tag = values[0]->token().value;
    if (tag.empty() || tag.size() > 4)
        return false;
    for (char const c : tag) {
        unsigned char const byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7E)
            return false;
    }
    while (!tag.empty() && tag.back() == ' ')
        tag.pop_back();
    // All spaces is the tag of four spaces, written as one.
    out = tag.empty() ? std::string(" ") : tag;
    return true;
}

std::string font_language_override_text(std::string const& tag)
{
    return tag.empty() ? "normal" : css_string_text(tag);
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

namespace {

// --- font-variant -------------------------------------------------------------------

enum class VariantPart : std::uint8_t {
    Ligatures,
    Caps,
    Numeric,
    EastAsian,
    Position,
    Alternates,
};

constexpr std::string_view variant_longhands[] = { "font-variant-ligatures", "font-variant-caps",
    "font-variant-numeric", "font-variant-east-asian", "font-variant-position", "font-variant-alternates" };

// The keywords of the longhands, in their grammars' order. Keywords of one
// group exclude one another (common-ligatures and no-common-ligatures,
// lining-nums and oldstyle-nums, ...); a value names each group at most
// once. `bit` is the keyword's bit in its longhand's set, or the value of
// its enumeration for caps and position.
struct VariantKeyword {
    std::string_view name;
    VariantPart part;
    std::uint8_t group;
    std::uint16_t bit;
};

using V = FontVariant;
constexpr VariantKeyword variant_keywords[] = {
    { "common-ligatures", VariantPart::Ligatures, 0, V::common_ligatures },
    { "no-common-ligatures", VariantPart::Ligatures, 0, V::no_common_ligatures },
    { "discretionary-ligatures", VariantPart::Ligatures, 1, V::discretionary_ligatures },
    { "no-discretionary-ligatures", VariantPart::Ligatures, 1, V::no_discretionary_ligatures },
    { "historical-ligatures", VariantPart::Ligatures, 2, V::historical_ligatures },
    { "no-historical-ligatures", VariantPart::Ligatures, 2, V::no_historical_ligatures },
    { "contextual", VariantPart::Ligatures, 3, V::contextual },
    { "no-contextual", VariantPart::Ligatures, 3, V::no_contextual },
    { "small-caps", VariantPart::Caps, 4, static_cast<std::uint16_t>(FontVariantCaps::SmallCaps) },
    { "all-small-caps", VariantPart::Caps, 4, static_cast<std::uint16_t>(FontVariantCaps::AllSmallCaps) },
    { "petite-caps", VariantPart::Caps, 4, static_cast<std::uint16_t>(FontVariantCaps::PetiteCaps) },
    { "all-petite-caps", VariantPart::Caps, 4, static_cast<std::uint16_t>(FontVariantCaps::AllPetiteCaps) },
    { "unicase", VariantPart::Caps, 4, static_cast<std::uint16_t>(FontVariantCaps::Unicase) },
    { "titling-caps", VariantPart::Caps, 4, static_cast<std::uint16_t>(FontVariantCaps::TitlingCaps) },
    { "lining-nums", VariantPart::Numeric, 5, V::lining_nums },
    { "oldstyle-nums", VariantPart::Numeric, 5, V::oldstyle_nums },
    { "proportional-nums", VariantPart::Numeric, 6, V::proportional_nums },
    { "tabular-nums", VariantPart::Numeric, 6, V::tabular_nums },
    { "diagonal-fractions", VariantPart::Numeric, 7, V::diagonal_fractions },
    { "stacked-fractions", VariantPart::Numeric, 7, V::stacked_fractions },
    { "ordinal", VariantPart::Numeric, 8, V::ordinal },
    { "slashed-zero", VariantPart::Numeric, 9, V::slashed_zero },
    { "jis78", VariantPart::EastAsian, 10, V::jis78 },
    { "jis83", VariantPart::EastAsian, 10, V::jis83 },
    { "jis90", VariantPart::EastAsian, 10, V::jis90 },
    { "jis04", VariantPart::EastAsian, 10, V::jis04 },
    { "simplified", VariantPart::EastAsian, 10, V::simplified },
    { "traditional", VariantPart::EastAsian, 10, V::traditional },
    { "full-width", VariantPart::EastAsian, 11, V::full_width },
    { "proportional-width", VariantPart::EastAsian, 11, V::proportional_width },
    { "ruby", VariantPart::EastAsian, 12, V::ruby },
    { "sub", VariantPart::Position, 13, static_cast<std::uint16_t>(FontVariantPosition::Sub) },
    { "super", VariantPart::Position, 13, static_cast<std::uint16_t>(FontVariantPosition::Super) },
};

// font-variant-alternates (§6.8): historical-forms and the functions that
// name @font-feature-values entries, in the grammar's order; a function
// that takes a list takes one or more names.
struct AlternateForm {
    std::string_view name;
    bool function;
    bool list;
};
constexpr AlternateForm alternate_forms[] = {
    { "stylistic", true, false },
    { "historical-forms", false, false },
    { "styleset", true, true },
    { "character-variant", true, true },
    { "swash", true, false },
    { "ornaments", true, false },
    { "annotation", true, false },
};
constexpr std::uint8_t alternates_first_group = 14;

// <feature-value-name>: a <custom-ident>, which no CSS-wide keyword or
// `default` is (css-values-4 §4.2).
bool custom_ident(ComponentValue const& value)
{
    if (!value.is_token(Token::Type::Ident))
        return false;
    for (std::string_view const reserved : { "initial", "inherit", "unset", "revert", "revert-layer", "default" }) {
        if (ascii_ci_equals(value.token().value, reserved))
            return false;
    }
    return true;
}

// A functional alternate's names, written as its computed value does
// ("styleset(a, b)"); false when the arguments are not its grammar.
bool alternate_function_text(FunctionValue const& function, AlternateForm const& form, std::string& out)
{
    std::vector<ComponentValue const*> names;
    bool expect_name = true;
    for (ComponentValue const& value : function.values) {
        if (value.is_token(Token::Type::Whitespace))
            continue;
        if (expect_name) {
            if (!custom_ident(value))
                return false;
            names.push_back(&value);
        } else if (!value.is_token(Token::Type::Comma) || !form.list) {
            return false;
        }
        expect_name = !expect_name;
    }
    if (names.empty() || expect_name)
        return false;
    out = std::string(form.name) + "(";
    for (std::size_t i = 0; i < names.size(); ++i)
        out += (i ? ", " : "") + names[i]->token().value;
    out += ")";
    return true;
}

// The longhands' values in any order, each group at most once, the parts
// not in `allowed` refused; written into `out`, which starts as all
// normal.
bool parse_variant_values(std::vector<ComponentValue const*> const& values, std::uint8_t allowed, FontVariant& out)
{
    if (values.empty())
        return false;
    std::uint32_t seen = 0;
    std::string alternates[std::size(alternate_forms)];
    auto const take = [&](std::uint8_t group) {
        if (seen & (1u << group))
            return false;
        seen |= 1u << group;
        return true;
    };
    for (ComponentValue const* value : values) {
        bool found = false;
        if (value->is_token(Token::Type::Ident)) {
            for (VariantKeyword const& keyword : variant_keywords) {
                if (!ascii_ci_equals(value->token().value, keyword.name))
                    continue;
                if (!(allowed & (1u << static_cast<unsigned>(keyword.part))) || !take(keyword.group))
                    return false;
                switch (keyword.part) {
                case VariantPart::Ligatures: out.ligatures |= keyword.bit; break;
                case VariantPart::Caps: out.caps = static_cast<FontVariantCaps>(keyword.bit); break;
                case VariantPart::Numeric: out.numeric |= static_cast<std::uint8_t>(keyword.bit); break;
                case VariantPart::EastAsian: out.east_asian |= keyword.bit; break;
                case VariantPart::Position: out.position = static_cast<FontVariantPosition>(keyword.bit); break;
                case VariantPart::Alternates: break;
                }
                found = true;
                break;
            }
        }
        if (!found && (allowed & (1u << static_cast<unsigned>(VariantPart::Alternates)))) {
            for (std::size_t k = 0; k < std::size(alternate_forms) && !found; ++k) {
                AlternateForm const& form = alternate_forms[k];
                bool const matches = form.function
                    ? value->is_function() && ascii_ci_equals(value->function().name, form.name)
                    : value->is_token(Token::Type::Ident) && ascii_ci_equals(value->token().value, form.name);
                if (!matches)
                    continue;
                if (!take(static_cast<std::uint8_t>(alternates_first_group + k)))
                    return false;
                if (form.function) {
                    if (!alternate_function_text(value->function(), form, alternates[k]))
                        return false;
                } else {
                    alternates[k] = std::string(form.name);
                    out.historical_forms = true;
                }
                found = true;
            }
        }
        if (!found)
            return false;
    }
    for (std::string const& form : alternates) {
        if (!form.empty())
            out.alternates += (out.alternates.empty() ? "" : " ") + form;
    }
    return true;
}

int part_of_longhand(std::string_view property)
{
    for (std::size_t i = 0; i < std::size(variant_longhands); ++i) {
        if (ascii_ci_equals(property, variant_longhands[i]))
            return static_cast<int>(i);
    }
    return -1;
}

// Copies one part of a variant over another's.
void copy_part(VariantPart part, FontVariant const& from, FontVariant& to)
{
    switch (part) {
    case VariantPart::Ligatures: to.ligatures = from.ligatures; break;
    case VariantPart::Caps: to.caps = from.caps; break;
    case VariantPart::Numeric: to.numeric = from.numeric; break;
    case VariantPart::EastAsian: to.east_asian = from.east_asian; break;
    case VariantPart::Position: to.position = from.position; break;
    case VariantPart::Alternates:
        to.alternates = from.alternates;
        to.historical_forms = from.historical_forms;
        break;
    }
}

// One part's keywords, in the grammar's order; empty for normal.
std::string part_text(VariantPart part, FontVariant const& variant)
{
    if (part == VariantPart::Alternates)
        return variant.alternates;
    if (part == VariantPart::Ligatures && (variant.ligatures & FontVariant::ligatures_none))
        return "none";
    std::string out;
    for (VariantKeyword const& keyword : variant_keywords) {
        if (keyword.part != part)
            continue;
        bool on = false;
        switch (part) {
        case VariantPart::Ligatures: on = (variant.ligatures & keyword.bit) != 0; break;
        case VariantPart::Caps: on = static_cast<std::uint16_t>(variant.caps) == keyword.bit; break;
        case VariantPart::Numeric: on = (variant.numeric & keyword.bit) != 0; break;
        case VariantPart::EastAsian: on = (variant.east_asian & keyword.bit) != 0; break;
        case VariantPart::Position: on = static_cast<std::uint16_t>(variant.position) == keyword.bit; break;
        case VariantPart::Alternates: break;
        }
        if (on)
            out += (out.empty() ? "" : " ") + std::string(keyword.name);
    }
    return out;
}

constexpr std::uint8_t all_parts = 0x3F;

}

bool is_font_variant_longhand(std::string_view property)
{
    return part_of_longhand(property) >= 0;
}

bool parse_font_variant_longhand(std::string_view property, std::vector<ComponentValue const*> const& values, FontVariant& variant)
{
    int const index = part_of_longhand(property);
    if (index < 0 || values.empty())
        return false;
    VariantPart const part = static_cast<VariantPart>(index);
    FontVariant parsed;
    if (values.size() == 1 && is_ident(values[0], "normal")) {
        copy_part(part, parsed, variant);
        return true;
    }
    if (part == VariantPart::Ligatures && values.size() == 1 && is_ident(values[0], "none")) {
        parsed.ligatures = FontVariant::ligatures_none;
        copy_part(part, parsed, variant);
        return true;
    }
    if (!parse_variant_values(values, static_cast<std::uint8_t>(1u << index), parsed))
        return false;
    copy_part(part, parsed, variant);
    return true;
}

bool parse_font_variant(std::vector<ComponentValue const*> const& values, FontVariant& variant)
{
    FontVariant parsed;
    if (values.size() == 1 && is_ident(values[0], "normal")) {
        variant = parsed;
        return true;
    }
    if (values.size() == 1 && is_ident(values[0], "none")) {
        parsed.ligatures = FontVariant::ligatures_none;
        variant = parsed;
        return true;
    }
    if (!parse_variant_values(values, all_parts, parsed))
        return false;
    variant = std::move(parsed);
    return true;
}

std::string font_variant_longhand_text(std::string_view property, FontVariant const& variant)
{
    int const index = part_of_longhand(property);
    if (index < 0)
        return "";
    std::string const text = part_text(static_cast<VariantPart>(index), variant);
    return text.empty() ? "normal" : text;
}

std::string font_variant_text(FontVariant const& variant)
{
    if (variant == FontVariant {})
        return "normal";
    FontVariant rest = variant;
    rest.ligatures = 0;
    if (variant.ligatures & FontVariant::ligatures_none)
        return rest == FontVariant {} ? "none" : "";
    std::string out;
    for (VariantPart const part : { VariantPart::Ligatures, VariantPart::Caps, VariantPart::Alternates, VariantPart::Numeric,
             VariantPart::EastAsian, VariantPart::Position }) {
        std::string const text = part_text(part, variant);
        if (!text.empty())
            out += (out.empty() ? "" : " ") + text;
    }
    return out;
}

void append_font_variant_features(FontVariant const& variant, std::vector<FontFeature>& out)
{
    auto const add = [&](char const (&name)[5], std::uint32_t value = 1) {
        std::uint32_t tag = 0;
        for (int i = 0; i < 4; ++i)
            tag = tag << 8 | static_cast<unsigned char>(name[i]);
        out.push_back(FontFeature { tag, value });
    };
    // §6.6.
    switch (variant.caps) {
    case FontVariantCaps::Normal: break;
    case FontVariantCaps::SmallCaps: add("smcp"); break;
    case FontVariantCaps::AllSmallCaps:
        add("c2sc");
        add("smcp");
        break;
    case FontVariantCaps::PetiteCaps: add("pcap"); break;
    case FontVariantCaps::AllPetiteCaps:
        add("c2pc");
        add("pcap");
        break;
    case FontVariantCaps::Unicase: add("unic"); break;
    case FontVariantCaps::TitlingCaps: add("titl"); break;
    }
    // §6.7.
    using F = FontVariant;
    std::uint8_t const numeric = variant.numeric;
    if (numeric & F::lining_nums)
        add("lnum");
    if (numeric & F::oldstyle_nums)
        add("onum");
    if (numeric & F::proportional_nums)
        add("pnum");
    if (numeric & F::tabular_nums)
        add("tnum");
    if (numeric & F::diagonal_fractions)
        add("frac");
    if (numeric & F::stacked_fractions)
        add("afrc");
    if (numeric & F::ordinal)
        add("ordn");
    if (numeric & F::slashed_zero)
        add("zero");
    // §6.8: only historical-forms needs no @font-feature-values.
    if (variant.historical_forms)
        add("hist");
    // §6.10.
    std::uint16_t const east_asian = variant.east_asian;
    if (east_asian & F::jis78)
        add("jp78");
    if (east_asian & F::jis83)
        add("jp83");
    if (east_asian & F::jis90)
        add("jp90");
    if (east_asian & F::jis04)
        add("jp04");
    if (east_asian & F::simplified)
        add("smpl");
    if (east_asian & F::traditional)
        add("trad");
    if (east_asian & F::full_width)
        add("fwid");
    if (east_asian & F::proportional_width)
        add("pwid");
    if (east_asian & F::ruby)
        add("ruby");
    // §6.5.
    if (variant.position == FontVariantPosition::Sub)
        add("subs");
    else if (variant.position == FontVariantPosition::Super)
        add("sups");
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
