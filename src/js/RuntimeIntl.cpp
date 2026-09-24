#include "js/Runtime.h"

// ECMA-402's core: the Intl object (section 8), language tags and their canonical
// form (section 6.2), locale negotiation (section 9.2), the option readers, and
// Intl.Locale (section 14). The formatters are in RuntimeIntlNumber.cpp,
// RuntimeIntlDate.cpp and RuntimeIntlText.cpp.

#include "js/Intl.h"
#include "js/IntlData.h"
#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

// The constructors, one installer each, and the built-ins ECMA-402
// replaces, by the file they live in.
void install_intl_date_time_format(Interpreter&, Object& intl); // RuntimeIntlDate.cpp
void install_intl_date_methods(Interpreter&);
void install_intl_number_format(Interpreter&, Object& intl); // RuntimeIntlNumber.cpp
void install_intl_plural_rules(Interpreter&, Object& intl);
void install_intl_relative_time_format(Interpreter&, Object& intl);
void install_intl_number_methods(Interpreter&);
void install_intl_collator(Interpreter&, Object& intl); // RuntimeIntlText.cpp
void install_intl_display_names(Interpreter&, Object& intl);
void install_intl_list_format(Interpreter&, Object& intl);
void install_intl_segmenter(Interpreter&, Object& intl);
void install_intl_string_methods(Interpreter&);
void install_intl_duration(Interpreter&, Object& intl); // RuntimeIntlDuration.cpp

std::string_view intl_kind_name(IntlKind kind)
{
    switch (kind) {
    case IntlKind::Collator: return "Collator";
    case IntlKind::DateTimeFormat: return "DateTimeFormat";
    case IntlKind::DisplayNames: return "DisplayNames";
    case IntlKind::ListFormat: return "ListFormat";
    case IntlKind::Locale: return "Locale";
    case IntlKind::NumberFormat: return "NumberFormat";
    case IntlKind::PluralRules: return "PluralRules";
    case IntlKind::RelativeTimeFormat: return "RelativeTimeFormat";
    case IntlKind::Segmenter: return "Segmenter";
    case IntlKind::Segments: return "Segments";
    case IntlKind::SegmentIterator: return "Segment Iterator";
    case IntlKind::DurationFormat: return "DurationFormat";
    }
    return "Object";
}

void IntlObject::trace(Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(m_slots[0]);
    tracer.visit(m_slots[1]);
}

// An Intl.Locale's record: the canonical tag.
struct LocaleData {
    static constexpr IntlKind kind = IntlKind::Locale;
    LanguageTag tag;
};

namespace {

// ------------------------------------------------------------ text helpers

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit_char(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return is_alpha(c) || is_digit_char(c); }

bool all_of(std::string_view s, bool (*test)(char))
{
    return std::all_of(s.begin(), s.end(), test);
}

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string upper(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return out;
}

std::string title(std::string_view s)
{
    std::string out = lower(s);
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z')
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    return out;
}

std::vector<std::string_view> split(std::string_view s, char separator)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        std::size_t const end = s.find(separator, start);
        if (end == std::string_view::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
}

template<typename Table>
std::optional<std::string_view> lookup(Table const& table, std::string_view code)
{
    for (auto const& entry : table)
        if (entry.code == code)
            return entry.name;
    return std::nullopt;
}

bool is_alnum_range(std::string_view s, std::size_t minimum, std::size_t maximum)
{
    return s.size() >= minimum && s.size() <= maximum && all_of(s, is_alnum);
}

} // namespace

bool is_unicode_language_subtag(std::string_view s)
{
    return ((s.size() >= 2 && s.size() <= 3) || (s.size() >= 5 && s.size() <= 8)) && all_of(s, is_alpha);
}

bool is_unicode_script_subtag(std::string_view s)
{
    return s.size() == 4 && all_of(s, is_alpha);
}

bool is_unicode_region_subtag(std::string_view s)
{
    return (s.size() == 2 && all_of(s, is_alpha)) || (s.size() == 3 && all_of(s, is_digit_char));
}

bool is_unicode_variant_subtag(std::string_view s)
{
    return is_alnum_range(s, 5, 8) || (s.size() == 4 && is_digit_char(s[0]) && all_of(s, is_alnum));
}

bool is_unicode_type_sequence(std::string_view s)
{
    if (s.empty())
        return false;
    for (std::string_view part : split(s, '-'))
        if (!is_alnum_range(part, 3, 8))
            return false;
    return true;
}

// ------------------------------------------------------------ LanguageTag

std::string LanguageTag::base_name() const
{
    std::string out = language;
    if (!script.empty())
        out += "-" + script;
    if (!region.empty())
        out += "-" + region;
    for (std::string const& variant : variants)
        out += "-" + variant;
    return out;
}

std::string LanguageTag::to_string() const
{
    std::string out = base_name();
    // The extensions in singleton order: the other singletons, -t- and -u-
    // among them where their letter falls.
    struct Extension {
        char singleton;
        std::string text;
    };
    std::vector<Extension> extensions;
    for (auto const& [singleton, text] : other_extensions)
        extensions.push_back({ singleton, text });
    if (has_transformed_extension) {
        std::string text;
        if (!transformed_language.empty())
            text = transformed_language;
        for (auto const& [key, value] : transformed_fields) {
            if (!text.empty())
                text += "-";
            text += key + "-" + value;
        }
        extensions.push_back({ 't', text });
    }
    if (has_unicode_extension) {
        std::string text;
        for (std::string const& attribute : unicode_attributes) {
            if (!text.empty())
                text += "-";
            text += attribute;
        }
        for (auto const& [key, value] : unicode_keywords) {
            if (!text.empty())
                text += "-";
            text += key;
            if (!value.empty())
                text += "-" + value;
        }
        extensions.push_back({ 'u', text });
    }
    std::stable_sort(extensions.begin(), extensions.end(), [](Extension const& a, Extension const& b) {
        return a.singleton < b.singleton;
    });
    for (Extension const& extension : extensions) {
        out += "-";
        out += extension.singleton;
        if (!extension.text.empty())
            out += "-" + extension.text;
    }
    if (!private_use.empty())
        out += "-x-" + private_use;
    return out;
}

std::optional<std::string> LanguageTag::keyword(std::string_view key) const
{
    for (auto const& [k, value] : unicode_keywords)
        if (k == key)
            return value;
    return std::nullopt;
}

void LanguageTag::set_keyword(std::string const& key, std::string const& type)
{
    has_unicode_extension = true;
    for (auto& [k, value] : unicode_keywords) {
        if (k == key) {
            value = type;
            return;
        }
    }
    auto position = std::find_if(unicode_keywords.begin(), unicode_keywords.end(),
        [&](auto const& entry) { return entry.first > key; });
    unicode_keywords.insert(position, { key, type });
}

void LanguageTag::remove_keyword(std::string_view key)
{
    std::erase_if(unicode_keywords, [&](auto const& entry) { return entry.first == key; });
    if (unicode_keywords.empty() && unicode_attributes.empty())
        has_unicode_extension = false;
}

namespace {

// unicode_language_id from subtags[i...]: language, script, region,
// variants. Advances i. False when the first subtag is not a language or a
// variant repeats.
bool parse_language_id(std::vector<std::string_view> const& subtags, std::size_t& i, LanguageTag& tag)
{
    if (i >= subtags.size() || !is_unicode_language_subtag(subtags[i]))
        return false;
    tag.language = std::string(subtags[i++]);
    if (i < subtags.size() && is_unicode_script_subtag(subtags[i]))
        tag.script = std::string(subtags[i++]);
    if (i < subtags.size() && is_unicode_region_subtag(subtags[i]))
        tag.region = std::string(subtags[i++]);
    while (i < subtags.size() && is_unicode_variant_subtag(subtags[i])) {
        std::string const variant = lower(subtags[i]);
        for (std::string const& seen : tag.variants)
            if (lower(seen) == variant)
                return false;
        tag.variants.push_back(std::string(subtags[i++]));
    }
    return true;
}

} // namespace

std::optional<LanguageTag> parse_language_tag(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    for (char c : text)
        if (!is_alnum(c) && c != '-')
            return std::nullopt;
    std::vector<std::string_view> const subtags = split(text, '-');
    for (std::string_view subtag : subtags)
        if (subtag.empty() || subtag.size() > 8)
            return std::nullopt;
    LanguageTag tag;
    std::size_t i = 0;
    if (!parse_language_id(subtags, i, tag))
        return std::nullopt;
    std::string seen_singletons;
    while (i < subtags.size()) {
        std::string_view const subtag = subtags[i];
        if (subtag.size() != 1)
            return std::nullopt;
        char const singleton = lower(subtag)[0];
        ++i;
        if (singleton == 'x') {
            std::string text_after;
            if (i >= subtags.size())
                return std::nullopt;
            for (; i < subtags.size(); ++i) {
                if (!is_alnum_range(subtags[i], 1, 8))
                    return std::nullopt;
                if (!text_after.empty())
                    text_after += "-";
                text_after += subtags[i];
            }
            tag.private_use = text_after;
            break;
        }
        if (seen_singletons.find(singleton) != std::string::npos)
            return std::nullopt;
        seen_singletons += singleton;
        if (singleton == 'u') {
            tag.has_unicode_extension = true;
            std::size_t const start = i;
            while (i < subtags.size() && is_alnum_range(subtags[i], 3, 8))
                tag.unicode_attributes.push_back(std::string(subtags[i++]));
            while (i < subtags.size() && subtags[i].size() == 2 && is_alnum(subtags[i][0]) && is_alpha(subtags[i][1])) {
                std::string key(subtags[i++]);
                std::string type;
                while (i < subtags.size() && is_alnum_range(subtags[i], 3, 8)) {
                    if (!type.empty())
                        type += "-";
                    type += subtags[i++];
                }
                tag.unicode_keywords.push_back({ key, type });
            }
            if (i == start)
                return std::nullopt;
        } else if (singleton == 't') {
            tag.has_transformed_extension = true;
            std::size_t const start = i;
            if (i < subtags.size() && is_unicode_language_subtag(subtags[i])) {
                LanguageTag tlang;
                if (!parse_language_id(subtags, i, tlang))
                    return std::nullopt;
                tag.transformed_language = tlang.base_name();
            }
            while (i < subtags.size() && subtags[i].size() == 2 && is_alpha(subtags[i][0]) && is_digit_char(subtags[i][1])) {
                std::string key(subtags[i++]);
                std::string value;
                while (i < subtags.size() && is_alnum_range(subtags[i], 3, 8)) {
                    if (!value.empty())
                        value += "-";
                    value += subtags[i++];
                }
                if (value.empty())
                    return std::nullopt;
                tag.transformed_fields.push_back({ key, value });
            }
            if (i == start)
                return std::nullopt;
        } else {
            std::string text_after;
            while (i < subtags.size() && is_alnum_range(subtags[i], 2, 8)) {
                if (!text_after.empty())
                    text_after += "-";
                text_after += subtags[i++];
            }
            if (text_after.empty())
                return std::nullopt;
            tag.other_extensions.push_back({ singleton, text_after });
        }
    }
    return tag;
}

namespace {

std::string likely_region_for(std::string const& language, std::string const& script)
{
    LanguageTag probe;
    probe.language = language;
    probe.script = script;
    add_likely_subtags(probe);
    return probe.region;
}

void canonicalize_base(LanguageTag& tag)
{
    tag.language = lower(tag.language);
    tag.script = title(tag.script);
    tag.region = upper(tag.region);
    for (std::string& variant : tag.variants)
        variant = lower(variant);
    std::sort(tag.variants.begin(), tag.variants.end());

    // Variant aliases: hepburn with heploc is alalc97.
    auto has_variant = [&](std::string_view v) {
        return std::find(tag.variants.begin(), tag.variants.end(), v) != tag.variants.end();
    };
    if (has_variant("heploc")) {
        std::erase(tag.variants, std::string("heploc"));
        std::erase(tag.variants, std::string("hepburn"));
        tag.variants.push_back("alalc97");
        std::sort(tag.variants.begin(), tag.variants.end());
    }

    // Language aliases: the language with one of its variants (the
    // grandfathered regular tags and hy's two spellings), with its region,
    // then alone.
    auto apply_replacement = [&](std::string_view replacement) {
        std::optional<LanguageTag> const parsed = parse_language_tag(replacement);
        if (!parsed)
            return;
        tag.language = parsed->language;
        if (tag.script.empty())
            tag.script = parsed->script;
        if (tag.region.empty())
            tag.region = parsed->region;
    };
    bool replaced = false;
    for (std::size_t v = 0; v < tag.variants.size() && !replaced; ++v) {
        if (std::optional<std::string_view> const to = lookup(intl_data::language_aliases, tag.language + "-" + tag.variants[v])) {
            tag.variants.erase(tag.variants.begin() + static_cast<std::ptrdiff_t>(v));
            apply_replacement(*to);
            replaced = true;
        }
    }
    if (!replaced && !tag.region.empty()) {
        if (std::optional<std::string_view> const to = lookup(intl_data::language_aliases, tag.language + "-" + tag.region)) {
            tag.region.clear();
            apply_replacement(*to);
            replaced = true;
        }
    }
    if (!replaced) {
        if (std::optional<std::string_view> const to = lookup(intl_data::language_aliases, tag.language))
            apply_replacement(*to);
    }

    // Region aliases; the Soviet Union takes the language's own successor.
    if (tag.region == "SU" || tag.region == "810") {
        std::string const likely = likely_region_for(tag.language, tag.script);
        std::string replacement = "RU";
        for (std::string_view successor : intl_data::soviet_successors)
            if (successor == likely)
                replacement = std::string(successor);
        tag.region = replacement;
    } else if (std::optional<std::string_view> const to = lookup(intl_data::region_aliases, tag.region)) {
        tag.region = std::string(*to);
    }
}

std::string canonical_keyword_type(std::string_view key, std::string type)
{
    for (auto const& alias : intl_data::keyword_aliases)
        if (alias.key == key && alias.alias == type)
            return std::string(alias.replacement);
    return type;
}

} // namespace

void canonicalize(LanguageTag& tag)
{
    canonicalize_base(tag);

    if (tag.has_unicode_extension) {
        for (std::string& attribute : tag.unicode_attributes)
            attribute = lower(attribute);
        std::sort(tag.unicode_attributes.begin(), tag.unicode_attributes.end());
        tag.unicode_attributes.erase(std::unique(tag.unicode_attributes.begin(), tag.unicode_attributes.end()),
            tag.unicode_attributes.end());
        std::vector<std::pair<std::string, std::string>> keywords;
        for (auto const& [key, type] : tag.unicode_keywords) {
            std::string const k = lower(key);
            if (std::any_of(keywords.begin(), keywords.end(), [&](auto const& entry) { return entry.first == k; }))
                continue;
            std::string t = canonical_keyword_type(k, lower(type));
            if (t == "yes" && (k == "kb" || k == "kc" || k == "kh" || k == "kk" || k == "kn"))
                t = "true";
            if (t == "true")
                t.clear();
            keywords.push_back({ k, t });
        }
        std::stable_sort(keywords.begin(), keywords.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        tag.unicode_keywords = std::move(keywords);
    }
    if (tag.has_transformed_extension) {
        if (!tag.transformed_language.empty()) {
            std::optional<LanguageTag> tlang = parse_language_tag(tag.transformed_language);
            if (tlang) {
                canonicalize_base(*tlang);
                tag.transformed_language = lower(tlang->base_name());
            }
        }
        for (auto& [key, value] : tag.transformed_fields) {
            key = lower(key);
            value = canonical_keyword_type(key, lower(value));
        }
        std::stable_sort(tag.transformed_fields.begin(), tag.transformed_fields.end(),
            [](auto const& a, auto const& b) { return a.first < b.first; });
    }
    for (auto& [singleton, text] : tag.other_extensions)
        text = lower(text);
    tag.private_use = lower(tag.private_use);
}

std::optional<std::string> canonicalize_language_tag(std::string_view text)
{
    std::optional<LanguageTag> tag = parse_language_tag(text);
    if (!tag)
        return std::nullopt;
    canonicalize(*tag);
    return tag->to_string();
}

void add_likely_subtags(LanguageTag& tag)
{
    std::string const& l = tag.language;
    std::string const& s = tag.script;
    std::string const& r = tag.region;
    // Lookup (UTS #35 section 4.3): language_script_region, language_region,
    // language_script, language -- und among them only when it is the
    // language. A language the table does not know is left as it is.
    std::vector<std::string> keys;
    if (!s.empty() && !r.empty())
        keys.push_back(l + "-" + s + "-" + r);
    if (!r.empty())
        keys.push_back(l + "-" + r);
    if (!s.empty())
        keys.push_back(l + "-" + s);
    keys.push_back(l);
    for (std::string const& key : keys) {
        std::optional<std::string_view> const found = lookup(intl_data::likely_subtags, key);
        if (!found)
            continue;
        std::optional<LanguageTag> const maximal = parse_language_tag(*found);
        if (!maximal)
            continue;
        if (tag.language == "und")
            tag.language = maximal->language;
        if (tag.script.empty())
            tag.script = maximal->script;
        if (tag.region.empty())
            tag.region = maximal->region;
        return;
    }
}

void remove_likely_subtags(LanguageTag& tag)
{
    LanguageTag maximal = tag;
    add_likely_subtags(maximal);
    auto trial = [&](std::string const& script, std::string const& region) {
        LanguageTag probe;
        probe.language = maximal.language;
        probe.script = script;
        probe.region = region;
        add_likely_subtags(probe);
        return probe.script == maximal.script && probe.region == maximal.region && probe.language == maximal.language;
    };
    tag.language = maximal.language;
    if (trial("", "")) {
        tag.script.clear();
        tag.region.clear();
    } else if (trial("", maximal.region)) {
        tag.script.clear();
        tag.region = maximal.region;
    } else if (trial(maximal.script, "")) {
        tag.script = maximal.script;
        tag.region.clear();
    } else {
        tag.script = maximal.script;
        tag.region = maximal.region;
    }
}

// ------------------------------------------------------ negotiation

std::string const& default_locale()
{
    static std::string const locale = "en-US";
    return locale;
}

bool is_available_locale(std::string_view base_name)
{
    return base_name == "en" || base_name == "en-US" || base_name == "en-GB";
}

namespace {

// RemoveUnicodeExtensions: the tag without its -u- extension.
std::string without_unicode_extension(std::string const& locale)
{
    std::optional<LanguageTag> tag = parse_language_tag(locale);
    if (!tag)
        return locale;
    tag->has_unicode_extension = false;
    tag->unicode_attributes.clear();
    tag->unicode_keywords.clear();
    return tag->to_string();
}

// BestAvailableLocale (section 9.2.2): the longest available prefix.
std::optional<std::string> best_available_locale(std::string candidate)
{
    // Only the base name can be available; extensions are cut first.
    if (std::optional<LanguageTag> tag = parse_language_tag(candidate))
        candidate = tag->base_name();
    while (true) {
        if (is_available_locale(candidate))
            return candidate;
        std::size_t position = candidate.rfind('-');
        if (position == std::string::npos)
            return std::nullopt;
        if (position >= 2 && candidate[position - 2] == '-')
            position -= 2;
        candidate = candidate.substr(0, position);
    }
}

} // namespace

std::string const& ResolvedLocale::value(std::string_view key) const
{
    static std::string const none;
    for (auto const& [k, v] : values)
        if (k == key)
            return v;
    return none;
}

ResolvedLocale resolve_locale(std::vector<std::string> const& requested, std::vector<RelevantKey> const& keys)
{
    // LookupMatcher (section 9.2.3); "best fit" is the same matcher here.
    std::string found = default_locale();
    std::optional<LanguageTag> extension;
    for (std::string const& locale : requested) {
        std::string const plain = without_unicode_extension(locale);
        if (std::optional<std::string> const available = best_available_locale(plain)) {
            found = *available;
            std::optional<LanguageTag> tag = parse_language_tag(locale);
            if (tag && tag->has_unicode_extension)
                extension = std::move(tag);
            break;
        }
    }
    ResolvedLocale result;
    result.data_locale = found;
    LanguageTag out = *parse_language_tag(found);
    for (RelevantKey const& key : keys) {
        std::string value = key.values.empty() ? std::string() : key.values.front();
        std::optional<std::string> addition;
        if (extension) {
            if (std::optional<std::string> const requested_value = extension->keyword(key.key)) {
                if (!requested_value->empty()) {
                    if (std::find(key.values.begin(), key.values.end(), *requested_value) != key.values.end()) {
                        value = *requested_value;
                        addition = value;
                    }
                } else if (std::find(key.values.begin(), key.values.end(), "true") != key.values.end()) {
                    value = "true";
                    addition = "";
                }
            }
        }
        if (key.option) {
            // An option's value is compared in lower case (section 9.2.7 step 9.i).
            std::string const option = lower(*key.option);
            if (std::find(key.values.begin(), key.values.end(), option) != key.values.end() && option != value) {
                value = option;
                addition.reset();
            }
        }
        if (addition)
            out.set_keyword(key.key, *addition);
        result.values.push_back({ key.key, value });
    }
    result.locale = out.to_string();
    return result;
}

// ------------------------------------------------------------ options

std::optional<Object*> coerce_options_to_object(Interpreter& in, Value const& options)
{
    if (options.is_undefined())
        return static_cast<Object*>(nullptr);
    return in.to_object(options);
}

std::optional<Object*> get_options_object(Interpreter& in, Value const& options)
{
    if (options.is_undefined())
        return static_cast<Object*>(nullptr);
    if (options.is_object())
        return options.as_object();
    return in.throw_type_error("Options must be an object");
}

std::optional<std::optional<std::string>> get_string_option(Interpreter& in, Object* options, std::string_view property,
    std::initializer_list<std::string_view> values)
{
    if (options == nullptr)
        return std::optional<std::string>();
    std::optional<Value> const value = in.get(*options, in.key(property));
    if (!value)
        return std::nullopt;
    if (value->is_undefined())
        return std::optional<std::string>();
    std::optional<JsString*> const text = in.to_string(*value);
    if (!text)
        return std::nullopt;
    std::string result = (*text)->to_utf8();
    if (values.size() != 0 && std::find(values.begin(), values.end(), std::string_view(result)) == values.end())
        return in.throw_range_error("Value " + result + " out of range for Intl options property " + std::string(property));
    return std::optional<std::string>(std::move(result));
}

std::optional<std::optional<bool>> get_boolean_option(Interpreter& in, Object* options, std::string_view property)
{
    if (options == nullptr)
        return std::optional<bool>();
    std::optional<Value> const value = in.get(*options, in.key(property));
    if (!value)
        return std::nullopt;
    if (value->is_undefined())
        return std::optional<bool>();
    return std::optional<bool>(Interpreter::to_boolean(*value));
}

std::optional<std::optional<int>> default_number_option(Interpreter& in, Value const& value, int minimum, int maximum,
    std::string_view property)
{
    if (value.is_undefined())
        return std::optional<int>();
    std::optional<double> const number = in.to_number(value);
    if (!number)
        return std::nullopt;
    if (std::isnan(*number) || *number < minimum || *number > maximum)
        return in.throw_range_error(std::string(property) + " value is out of range.");
    return std::optional<int>(static_cast<int>(std::floor(*number)));
}

std::optional<std::optional<int>> get_number_option(Interpreter& in, Object* options, std::string_view property,
    int minimum, int maximum)
{
    if (options == nullptr)
        return std::optional<int>();
    std::optional<Value> const value = in.get(*options, in.key(property));
    if (!value)
        return std::nullopt;
    return default_number_option(in, *value, minimum, maximum, property);
}

std::optional<bool> read_locale_matcher(Interpreter& in, Object* options)
{
    std::optional<std::optional<std::string>> const matcher = get_string_option(in, options, "localeMatcher", { "lookup", "best fit" });
    if (!matcher)
        return std::nullopt;
    return true;
}

// ------------------------------------------------------------ results

Value intl_string(Interpreter& in, std::string_view utf8)
{
    return Value::string(in.string(utf8));
}

Value intl_string(Interpreter& in, std::u16string_view text)
{
    return Value::string(in.string(text));
}

Value parts_to_array(Interpreter& in, std::vector<IntlPart> const& parts)
{
    // A key and a value are made for each put, in either order; nothing is
    // collected until the array is whole.
    Heap::NoCollect const guard(in.heap());
    ArrayObject* array = in.new_array();
    std::uint32_t index = 0;
    for (IntlPart const& part : parts) {
        Object* object = in.new_object();
        object->put(in.key("type"), intl_string(in, part.type));
        object->put(in.key("value"), intl_string(in, part.value));
        if (!part.extra_name.empty())
            object->put(in.key(part.extra_name), intl_string(in, part.extra_value));
        array->put(PropertyKey::index(index++), Value::object(object));
    }
    return Value::object(array);
}

std::u16string join_parts(std::vector<IntlPart> const& parts)
{
    std::u16string out;
    for (IntlPart const& part : parts)
        out += part.value;
    return out;
}

Object* intl_prototype(Interpreter& in, IntlKind kind)
{
    return in.intrinsics().intl_prototypes[static_cast<std::size_t>(kind)];
}

std::vector<std::string> const& numbering_system_names()
{
    static std::vector<std::string> const names = [] {
        std::vector<std::string> out = { "latn" };
        for (auto const& system : intl_data::numbering_systems)
            if (system.name != "latn")
                out.push_back(std::string(system.name));
        return out;
    }();
    return names;
}

std::u16string transliterate_digits(std::string_view numbering_system, std::u16string_view text)
{
    if (numbering_system == "latn" || numbering_system.empty())
        return std::u16string(text);
    char32_t zero = U'0';
    bool hanidec = false;
    for (auto const& system : intl_data::numbering_systems) {
        if (system.name == numbering_system) {
            zero = system.zero;
            hanidec = system.name == "hanidec";
        }
    }
    std::u16string out;
    for (char16_t c : text) {
        if (c >= u'0' && c <= u'9')
            append_code_point(out, hanidec ? intl_data::hanidec_digits[c - u'0'] : zero + static_cast<char32_t>(c - u'0'));
        else
            out += c;
    }
    return out;
}

std::u16string_view decimal_separator(std::string_view numbering_system)
{
    // U+066B ARABIC DECIMAL SEPARATOR.
    static constexpr char16_t arabic[] = { 0x066B, 0 };
    return numbering_system == "arab" || numbering_system == "arabext" ? std::u16string_view(arabic) : u".";
}

std::u16string_view group_separator(std::string_view numbering_system)
{
    // U+066C ARABIC THOUSANDS SEPARATOR.
    static constexpr char16_t arabic[] = { 0x066C, 0 };
    return numbering_system == "arab" || numbering_system == "arabext" ? std::u16string_view(arabic) : u",";
}

Value intl_string_array(Interpreter& in, std::vector<std::string> const& values)
{
    Interpreter::Roots const roots(in);
    std::vector<Value> out;
    out.reserve(values.size());
    for (std::string const& value : values) {
        out.push_back(intl_string(in, value));
        in.root(out.back());
    }
    return Value::object(in.new_array(out));
}

NativeFunction* define_intl_constructor(Interpreter& in, Object& intl, IntlKind kind, int length,
    NativeFunction::Callback call, NativeFunction::ConstructCallback construct)
{
    Heap::NoCollect const guard(in.heap());
    Intrinsics& i = in.intrinsics();
    std::string const name(intl_kind_name(kind));
    if (!call) {
        call = [name](Interpreter& interp, Value const&, Args) -> std::optional<Value> {
            return interp.throw_type_error("Constructor Intl." + name + " requires 'new'");
        };
    }
    Object* prototype = in.new_object();
    NativeFunction* constructor = in.new_native(name, length, std::move(call), std::move(construct));
    constructor->put(PropertyKey::atom(in.atoms().prototype), Value::object(prototype), frozen_attributes);
    prototype->put(PropertyKey::atom(in.atoms().constructor), Value::object(constructor), builtin_attributes);
    prototype->put(PropertyKey::symbol(in.atoms().symbol_to_string_tag), intl_string(in, "Intl." + name), Configurable);
    intl.put(in.key(name), Value::object(constructor), builtin_attributes);
    i.intl_prototypes[static_cast<std::size_t>(kind)] = prototype;
    i.intl_constructors[static_cast<std::size_t>(kind)] = constructor;
    define_method(in, *constructor, "supportedLocalesOf", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<std::vector<std::string>> const requested = canonicalize_locale_list(interp, argument(args, 0));
        if (!requested)
            return std::nullopt;
        return supported_locales(interp, *requested, argument(args, 1));
    });
    return constructor;
}

std::optional<std::vector<std::string>> canonicalize_locale_list(Interpreter& in, Value const& locales)
{
    // section 9.2.1.
    std::vector<std::string> seen;
    if (locales.is_undefined())
        return seen;
    Interpreter::Roots const roots(in);
    Object* object = nullptr;
    if (locales.is_string() || intl_cast<LocaleData>(locales) != nullptr) {
        object = in.new_array(std::span<Value const>(&locales, 1));
    } else {
        std::optional<Object*> const converted = in.to_object(locales);
        if (!converted)
            return std::nullopt;
        object = *converted;
    }
    in.root(Value::object(object));
    std::optional<double> const length = in.length_of_array_like(*object);
    if (!length)
        return std::nullopt;
    for (double k = 0; k < *length; ++k) {
        PropertyKey const key = k < 4294967295.0 ? PropertyKey::index(static_cast<std::uint32_t>(k))
                                                 : in.key(number_to_utf8(k));
        std::optional<bool> const present = in.has_property(*object, key);
        if (!present)
            return std::nullopt;
        if (!*present)
            continue;
        std::optional<Value> const value = in.get(*object, key);
        if (!value)
            return std::nullopt;
        if (!value->is_string() && !value->is_object())
            return in.throw_type_error("Language ID should be string or object.");
        in.root(*value);
        std::string text;
        if (auto* locale = intl_cast<LocaleData>(*value)) {
            text = locale->data.tag.to_string();
        } else {
            std::optional<JsString*> const string = in.to_string(*value);
            if (!string)
                return std::nullopt;
            text = (*string)->to_utf8();
        }
        std::optional<std::string> const canonical = canonicalize_language_tag(text);
        if (!canonical)
            return in.throw_range_error("Incorrect locale information provided");
        if (std::find(seen.begin(), seen.end(), *canonical) == seen.end())
            seen.push_back(*canonical);
    }
    return seen;
}

std::optional<Value> supported_locales(Interpreter& in, std::vector<std::string> const& requested, Value const& options)
{
    std::optional<Object*> const object = coerce_options_to_object(in, options);
    if (!object)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    if (*object)
        in.root(Value::object(*object));
    if (!read_locale_matcher(in, *object))
        return std::nullopt;
    std::vector<Value> values;
    for (std::string const& locale : requested) {
        if (best_available_locale(without_unicode_extension(locale))) {
            values.push_back(intl_string(in, locale));
            in.root(values.back());
        }
    }
    return Value::object(in.new_array(values));
}

// ------------------------------------------------------------ Intl.Locale

namespace {

std::optional<IntlObjectOf<LocaleData>*> this_locale(Interpreter& in, Value const& this_value, std::string_view method)
{
    return intl_this<LocaleData>(in, this_value, method);
}

std::optional<Value> new_locale_object(Interpreter& in, Object* prototype, LanguageTag tag)
{
    auto* object = in.heap().allocate<IntlObjectOf<LocaleData>>(prototype);
    object->data.tag = std::move(tag);
    return Value::object(object);
}

std::string week_day_name(std::string const& value)
{
    static constexpr std::string_view names[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
    if (value.size() == 1 && value[0] >= '0' && value[0] <= '7')
        return std::string(names[value[0] - '0']);
    return value;
}

std::optional<Value> locale_constructor(Interpreter& in, Args args, Object* new_target)
{
    // section 14.1.1.
    Interpreter::Roots const roots(in);
    in.root(Value::object(new_target));
    std::optional<Object*> const prototype = in.get_prototype_from_constructor(new_target,
        [](Intrinsics const& i) { return i.intl_prototypes[static_cast<std::size_t>(IntlKind::Locale)]; });
    if (!prototype)
        return std::nullopt;
    in.root(Value::object(*prototype));
    Value const tag_value = argument(args, 0);
    if (!tag_value.is_string() && !tag_value.is_object())
        return in.throw_type_error("First argument to Intl.Locale constructor can't be empty or missing");
    std::string text;
    if (auto* locale = intl_cast<LocaleData>(tag_value)) {
        text = locale->data.tag.to_string();
    } else {
        std::optional<JsString*> const string = in.to_string(tag_value);
        if (!string)
            return std::nullopt;
        text = (*string)->to_utf8();
    }
    std::optional<Object*> const options = coerce_options_to_object(in, argument(args, 1));
    if (!options)
        return std::nullopt;
    if (*options)
        in.root(Value::object(*options));

    // ApplyOptionsToTag (section 14.1.2).
    std::optional<LanguageTag> tag = parse_language_tag(text);
    if (!tag)
        return in.throw_range_error("Incorrect locale information provided");
    auto read = [&](std::string_view name, bool (*valid)(std::string_view)) -> std::optional<std::optional<std::string>> {
        std::optional<std::optional<std::string>> value = get_string_option(in, *options, name);
        if (!value)
            return std::nullopt;
        if (*value && !valid(**value))
            return in.throw_range_error("Incorrect locale information provided");
        return value;
    };
    auto const language = read("language", is_unicode_language_subtag);
    if (!language)
        return std::nullopt;
    auto const script = read("script", is_unicode_script_subtag);
    if (!script)
        return std::nullopt;
    auto const region = read("region", is_unicode_region_subtag);
    if (!region)
        return std::nullopt;
    auto const variants = read("variants", [](std::string_view s) {
        if (s.empty())
            return false;
        std::vector<std::string> seen;
        for (std::string_view part : split(s, '-')) {
            if (!is_unicode_variant_subtag(part))
                return false;
            std::string const v = lower(part);
            if (std::find(seen.begin(), seen.end(), v) != seen.end())
                return false;
            seen.push_back(v);
        }
        return true;
    });
    if (!variants)
        return std::nullopt;
    canonicalize(*tag);
    if (*language)
        tag->language = **language;
    if (*script)
        tag->script = **script;
    if (*region)
        tag->region = **region;
    if (*variants) {
        tag->variants.clear();
        for (std::string_view part : split(**variants, '-'))
            tag->variants.push_back(std::string(part));
    }
    canonicalize(*tag);

    // The keywords the options set (section 14.1.1 steps 12-30), in their order.
    auto set_type = [&](char const* name, char const* key, bool type_sequence,
                        std::initializer_list<std::string_view> values) -> bool {
        std::optional<std::optional<std::string>> value = get_string_option(in, *options, name, values);
        if (!value)
            return false;
        if (!*value)
            return true;
        std::string type = **value;
        if (std::string_view(key) == "fw")
            type = week_day_name(type);
        if (type_sequence && !is_unicode_type_sequence(type)) {
            in.throw_range_error("Incorrect locale information provided");
            return false;
        }
        tag->set_keyword(key, lower(type));
        return true;
    };
    if (!set_type("calendar", "ca", true, {}))
        return std::nullopt;
    if (!set_type("collation", "co", true, {}))
        return std::nullopt;
    if (!set_type("firstDayOfWeek", "fw", true, {}))
        return std::nullopt;
    if (!set_type("hourCycle", "hc", false, { "h11", "h12", "h23", "h24" }))
        return std::nullopt;
    if (!set_type("caseFirst", "kf", false, { "upper", "lower", "false" }))
        return std::nullopt;
    std::optional<std::optional<bool>> const numeric = get_boolean_option(in, *options, "numeric");
    if (!numeric)
        return std::nullopt;
    if (*numeric)
        tag->set_keyword("kn", **numeric ? "true" : "false");
    if (!set_type("numberingSystem", "nu", true, {}))
        return std::nullopt;
    canonicalize(*tag);
    return new_locale_object(in, *prototype, std::move(*tag));
}

Value keyword_value(Interpreter& in, LanguageTag const& tag, std::string_view key)
{
    std::optional<std::string> const value = tag.keyword(key);
    if (!value)
        return Value::undefined();
    return intl_string(in, *value);
}

// The region a locale's regional preferences come from (UTS #35 section 3.6.5):
// the -u-rg- override, else the region subtag, else the -u-sd-
// subdivision's region, else the language's likely region.
std::string effective_region(LanguageTag const& tag)
{
    auto region_of = [](std::optional<std::string> const& value) -> std::string {
        if (!value || value->size() < 3 || value->size() > 6)
            return {};
        if (!is_alpha((*value)[0]) || !is_alpha((*value)[1]))
            return {};
        return upper(value->substr(0, 2));
    };
    if (std::string const rg = region_of(tag.keyword("rg")); !rg.empty())
        return rg;
    if (!tag.region.empty())
        return tag.region;
    if (std::string const sd = region_of(tag.keyword("sd")); !sd.empty())
        return sd;
    LanguageTag likely = tag;
    add_likely_subtags(likely);
    return likely.region;
}

std::optional<Value> locale_info_array(Interpreter& in, std::vector<std::string> const& values)
{
    Interpreter::Roots const roots(in);
    std::vector<Value> out;
    for (std::string const& value : values) {
        out.push_back(intl_string(in, value));
        in.root(out.back());
    }
    return Value::object(in.new_array(out));
}

void install_locale(Interpreter& in, Object& intl)
{
    Intrinsics& i = in.intrinsics();
    Heap::NoCollect const guard(in.heap());
    Object* prototype = in.new_object();
    i.intl_prototypes[static_cast<std::size_t>(IntlKind::Locale)] = prototype;
    NativeFunction* constructor = in.new_native("Locale", 1,
        [](Interpreter& interp, Value const&, Args) -> std::optional<Value> {
            return interp.throw_type_error("Constructor Intl.Locale requires 'new'");
        },
        locale_constructor);
    constructor->put(PropertyKey::atom(in.atoms().prototype), Value::object(prototype), frozen_attributes);
    prototype->put(PropertyKey::atom(in.atoms().constructor), Value::object(constructor), builtin_attributes);
    intl.put(in.key("Locale"), Value::object(constructor), builtin_attributes);
    i.intl_constructors[static_cast<std::size_t>(IntlKind::Locale)] = constructor;
    prototype->put(PropertyKey::symbol(in.atoms().symbol_to_string_tag), intl_string(in, "Intl.Locale"), Configurable);

    define_method(in, *prototype, "maximize", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<LocaleData>*> const locale = this_locale(interp, this_value, "maximize");
        if (!locale)
            return std::nullopt;
        LanguageTag tag = (*locale)->data.tag;
        add_likely_subtags(tag);
        return new_locale_object(interp, intl_prototype(interp, IntlKind::Locale), std::move(tag));
    });
    define_method(in, *prototype, "minimize", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<LocaleData>*> const locale = this_locale(interp, this_value, "minimize");
        if (!locale)
            return std::nullopt;
        LanguageTag tag = (*locale)->data.tag;
        remove_likely_subtags(tag);
        return new_locale_object(interp, intl_prototype(interp, IntlKind::Locale), std::move(tag));
    });
    define_method(in, *prototype, "toString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<LocaleData>*> const locale = this_locale(interp, this_value, "toString");
        if (!locale)
            return std::nullopt;
        return intl_string(interp, (*locale)->data.tag.to_string());
    });

    struct Getter {
        char const* name;
        Value (*read)(Interpreter&, LanguageTag const&);
    };
    static constexpr Getter getters[] = {
        { "baseName", [](Interpreter& interp, LanguageTag const& tag) { return intl_string(interp, tag.base_name()); } },
        { "calendar", [](Interpreter& interp, LanguageTag const& tag) { return keyword_value(interp, tag, "ca"); } },
        { "caseFirst", [](Interpreter& interp, LanguageTag const& tag) { return keyword_value(interp, tag, "kf"); } },
        { "collation", [](Interpreter& interp, LanguageTag const& tag) { return keyword_value(interp, tag, "co"); } },
        { "firstDayOfWeek", [](Interpreter& interp, LanguageTag const& tag) { return keyword_value(interp, tag, "fw"); } },
        { "hourCycle", [](Interpreter& interp, LanguageTag const& tag) { return keyword_value(interp, tag, "hc"); } },
        { "numeric", [](Interpreter&, LanguageTag const& tag) {
             std::optional<std::string> const value = tag.keyword("kn");
             return Value::boolean(value && (value->empty() || *value == "true"));
         } },
        { "numberingSystem", [](Interpreter& interp, LanguageTag const& tag) { return keyword_value(interp, tag, "nu"); } },
        { "language", [](Interpreter& interp, LanguageTag const& tag) { return intl_string(interp, tag.language); } },
        { "script", [](Interpreter& interp, LanguageTag const& tag) {
             return tag.script.empty() ? Value::undefined() : intl_string(interp, tag.script);
         } },
        { "region", [](Interpreter& interp, LanguageTag const& tag) {
             return tag.region.empty() ? Value::undefined() : intl_string(interp, tag.region);
         } },
        { "variants", [](Interpreter& interp, LanguageTag const& tag) {
             if (tag.variants.empty())
                 return Value::undefined();
             std::string joined;
             for (std::string const& v : tag.variants)
                 joined += (joined.empty() ? "" : "-") + v;
             return intl_string(interp, joined);
         } },
    };
    for (Getter const& getter : getters) {
        auto read = getter.read;
        std::string const name = getter.name;
        define_accessor(in, *prototype, getter.name, [read, name](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
            std::optional<IntlObjectOf<LocaleData>*> const locale = this_locale(interp, this_value, name);
            if (!locale)
                return std::nullopt;
            return read(interp, (*locale)->data.tag);
        });
    }

    // The Locale Info methods: what the engine's data says for the locale.
    define_method(in, *prototype, "getCalendars", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<LocaleData>*> const locale = this_locale(interp, this_value, "getCalendars");
        if (!locale)
            return std::nullopt;
        std::optional<std::string> const ca = (*locale)->data.tag.keyword("ca");
        return locale_info_array(interp, { ca && !ca->empty() ? *ca : std::string("gregory") });
    });
    define_method(in, *prototype, "getCollations", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<LocaleData>*> const locale = this_locale(interp, this_value, "getCollations");
        if (!locale)
            return std::nullopt;
        std::optional<std::string> const co = (*locale)->data.tag.keyword("co");
        if (co && !co->empty())
            return locale_info_array(interp, { *co });
        return locale_info_array(interp, { "emoji", "eor" });
    });
    define_method(in, *prototype, "getHourCycles", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<LocaleData>*> const locale = this_locale(interp, this_value, "getHourCycles");
        if (!locale)
            return std::nullopt;
        LanguageTag const& tag = (*locale)->data.tag;
        std::optional<std::string> const hc = tag.keyword("hc");
        if (hc && !hc->empty())
            return locale_info_array(interp, { *hc });
        std::string const region = effective_region(tag);
        bool const twelve = region == "US" || region == "CA" || region == "AU" || region == "IN" || region == "NZ"
            || region == "PH" || region == "EG" || region == "SA" || region == "MX" || region == "KR" || region == "PK";
        return locale_info_array(interp, { twelve ? "h12" : "h23" });
    });
    define_method(in, *prototype, "getNumberingSystems", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<LocaleData>*> const locale = this_locale(interp, this_value, "getNumberingSystems");
        if (!locale)
            return std::nullopt;
        std::optional<std::string> const nu = (*locale)->data.tag.keyword("nu");
        return locale_info_array(interp, { nu && !nu->empty() ? *nu : std::string("latn") });
    });
    define_method(in, *prototype, "getTimeZones", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<LocaleData>*> const locale = this_locale(interp, this_value, "getTimeZones");
        if (!locale)
            return std::nullopt;
        std::string const& region = (*locale)->data.tag.region;
        if (region.empty())
            return Value::undefined();
        // The engine knows the zones of only a few regions by name.
        if (region == "GB")
            return locale_info_array(interp, { "Europe/London" });
        if (region == "US")
            return locale_info_array(interp, { "America/Adak", "America/Anchorage", "America/Boise", "America/Chicago",
                                                 "America/Denver", "America/Detroit", "America/Indiana/Indianapolis",
                                                 "America/Juneau", "America/Los_Angeles", "America/New_York",
                                                 "America/Phoenix", "Pacific/Honolulu" });
        if (region == "JP")
            return locale_info_array(interp, { "Asia/Tokyo" });
        if (region == "FR")
            return locale_info_array(interp, { "Europe/Paris" });
        if (region == "DE")
            return locale_info_array(interp, { "Europe/Berlin", "Europe/Busingen" });
        return locale_info_array(interp, {});
    });
    define_method(in, *prototype, "getTextInfo", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<LocaleData>*> const locale = this_locale(interp, this_value, "getTextInfo");
        if (!locale)
            return std::nullopt;
        LanguageTag tag = (*locale)->data.tag;
        add_likely_subtags(tag);
        bool rtl = false;
        for (std::string_view script : intl_data::rtl_scripts)
            if (script == tag.script)
                rtl = true;
        Heap::NoCollect const no_collect(interp.heap());
        Object* result = interp.new_object();
        result->put(interp.key("direction"), intl_string(interp, rtl ? "rtl" : "ltr"));
        return Value::object(result);
    });
    define_method(in, *prototype, "getWeekInfo", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<LocaleData>*> const locale = this_locale(interp, this_value, "getWeekInfo");
        if (!locale)
            return std::nullopt;
        LanguageTag const& tag = (*locale)->data.tag;
        std::string const region = effective_region(tag);
        int first_day = 1;
        std::optional<std::string> const fw = tag.keyword("fw");
        static constexpr std::string_view days[] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
        if (fw && !fw->empty()) {
            for (int d = 0; d < 7; ++d)
                if (days[d] == *fw)
                    first_day = d + 1;
        } else if (region == "IR") {
            first_day = 6;
        } else if (region == "US" || region == "CA" || region == "JP" || region == "BR" || region == "MX" || region == "IN"
            || region == "IL" || region == "KR" || region == "TW" || region == "HK" || region == "PH" || region == "ZA") {
            first_day = 7;
        }
        Heap::NoCollect const no_collect(interp.heap());
        Object* result = interp.new_object();
        result->put(interp.key("firstDay"), Value::number(first_day));
        std::vector<Value> weekend;
        if (region == "IR")
            weekend = { Value::number(5) };
        else
            weekend = { Value::number(6), Value::number(7) };
        result->put(interp.key("weekend"), Value::object(interp.new_array(weekend)));
        return Value::object(result);
    });
}

// ------------------------------------------------------ Intl's functions

std::vector<std::string> available_time_zones_list(); // below

std::optional<Value> supported_values_of(Interpreter& in, Args args)
{
    std::optional<JsString*> const key = in.to_string(argument(args, 0));
    if (!key)
        return std::nullopt;
    std::string const k = (*key)->to_utf8();
    std::vector<std::string> values;
    if (k == "calendar") {
        values = { "gregory" };
    } else if (k == "collation") {
        values = { "emoji", "eor" };
    } else if (k == "currency") {
        for (auto const& currency : intl_data::currencies)
            values.push_back(std::string(currency.code));
    } else if (k == "numberingSystem") {
        values = numbering_system_names();
    } else if (k == "timeZone") {
        values = available_time_zones_list();
    } else if (k == "unit") {
        for (auto const& unit : intl_data::units)
            values.push_back(std::string(unit.name));
    } else {
        return in.throw_range_error("Invalid key : " + k);
    }
    std::sort(values.begin(), values.end());
    return locale_info_array(in, values);
}

} // namespace

// The zones the engine formats in: UTC and the system's own, by name.
std::string system_time_zone_name(); // RuntimeIntlDate.cpp

namespace {

std::vector<std::string> available_time_zones_list()
{
    // UTC, the fixed Etc/GMT zones, and the system's own.
    std::vector<std::string> zones = { "UTC" };
    for (int n = 1; n <= 14; ++n)
        zones.push_back("Etc/GMT-" + std::to_string(n));
    for (int n = 1; n <= 12; ++n)
        zones.push_back("Etc/GMT+" + std::to_string(n));
    std::string const system = system_time_zone_name();
    if (system != "UTC")
        zones.push_back(system);
    return zones;
}

} // namespace

void install_intl(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    Object* intl = nullptr;
    {
        Heap::NoCollect const guard(in.heap());
        intl = in.new_object();
        i.intl = intl;
        intl->put(PropertyKey::symbol(in.atoms().symbol_to_string_tag), intl_string(in, "Intl"), Configurable);
        in.global()->put(in.key("Intl"), Value::object(intl), builtin_attributes);
        i.intl_fallback_symbol = in.heap().symbol(in.string(std::string_view("IntlLegacyConstructedSymbol")));

        define_method(in, *intl, "getCanonicalLocales", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
            std::optional<std::vector<std::string>> const list = canonicalize_locale_list(interp, argument(args, 0));
            if (!list)
                return std::nullopt;
            return locale_info_array(interp, *list);
        });
        define_method(in, *intl, "supportedValuesOf", 1, [](Interpreter& interp, Value const&, Args args) {
            return supported_values_of(interp, args);
        });
    }
    // The constructors in the order V8 has them on Intl.
    install_intl_date_time_format(in, *intl);
    install_intl_number_format(in, *intl);
    install_intl_collator(in, *intl);
    install_intl_plural_rules(in, *intl);
    install_intl_relative_time_format(in, *intl);
    install_intl_list_format(in, *intl);
    install_locale(in, *intl);
    install_intl_display_names(in, *intl);
    install_intl_segmenter(in, *intl);
    install_intl_duration(in, *intl);
    install_intl_date_methods(in);
    install_intl_number_methods(in);
    install_intl_string_methods(in);
}

}
