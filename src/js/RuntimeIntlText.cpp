#include "js/Runtime.h"

// The text services of ECMA-402: Intl.Collator (section 10), Intl.DisplayNames
// (section 12), Intl.ListFormat (section 13) and Intl.Segmenter (section 18), and the String
// methods that go through them (section 19.1): localeCompare,
// toLocaleUpperCase and toLocaleLowerCase.
//
// The collator is not the Unicode Collation Algorithm and carries no
// DUCET: it orders over the engine's own Unicode tables in three levels
// the way UCA's first three do -- the base letter (case folded, the
// canonical decomposition's marks set aside; spaces and punctuation
// before digits before letters), then the accents, then the case -- with
// runs of digits compared by value under numeric. The segmenter's
// grapheme clusters follow UAX #29's rules over properties derived from
// those tables; its words and sentences follow the UAX #29 rules with the
// letter, digit and punctuation classes spelled here, and no dictionary
// for the languages that write without spaces.

#include "core/Unicode.h"
#include "js/Intl.h"
#include "js/IntlData.h"
#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

namespace {

std::vector<char32_t> code_points(std::u16string_view text)
{
    std::vector<char32_t> out;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t units = 1;
        out.push_back(code_point_at(text, i, &units));
        i += units;
    }
    return out;
}

// ------------------------------------------------------------- classes

bool is_ascii_digit(char32_t c) { return c >= '0' && c <= '9'; }

bool is_space(char32_t c)
{
    return c == ' ' || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F
        || c == 0x3000 || c == '\t' || c == 0x0B || c == 0x0C;
}

bool is_punctuation_or_symbol(char32_t c)
{
    if (c < 0x80)
        return c > 0x20 && c < 0x7F && !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
    if (c >= 0xA1 && c <= 0xBF && c != 0xAA && c != 0xBA && c != 0xB2 && c != 0xB3 && c != 0xB9 && c != 0xB5)
        return true;
    if (c == 0xD7 || c == 0xF7)
        return true;
    if (c >= 0x2010 && c <= 0x2027)
        return true;
    if (c >= 0x2030 && c <= 0x205E)
        return true;
    if (c >= 0x20A0 && c <= 0x20CF)
        return true;
    if (c >= 0x2190 && c <= 0x23FF)
        return true;
    if (c >= 0x3001 && c <= 0x3003)
        return true;
    if (c >= 0x3008 && c <= 0x3011)
        return true;
    return is_first_letter_punctuation(c);
}

bool is_ignorable(char32_t c)
{
    return is_control(c) || (is_default_ignorable(c) && !(c >= 0xFE00 && c <= 0xFE0F));
}

bool is_extended_pictographic(char32_t c)
{
    if (c == 0xA9 || c == 0xAE || c == 0x203C || c == 0x2049 || c == 0x2122 || c == 0x2139)
        return true;
    if ((c >= 0x2194 && c <= 0x2199) || (c >= 0x21A9 && c <= 0x21AA) || (c >= 0x231A && c <= 0x231B) || c == 0x2328
        || c == 0x23CF || (c >= 0x23E9 && c <= 0x23F3) || (c >= 0x23F8 && c <= 0x23FA) || c == 0x24C2
        || (c >= 0x25AA && c <= 0x25AB) || c == 0x25B6 || c == 0x25C0 || (c >= 0x25FB && c <= 0x25FE))
        return true;
    if (c >= 0x2600 && c <= 0x27BF)
        return true;
    if ((c >= 0x2934 && c <= 0x2935) || (c >= 0x2B05 && c <= 0x2B07) || (c >= 0x2B1B && c <= 0x2B1C) || c == 0x2B50
        || c == 0x2B55 || c == 0x3030 || c == 0x303D || c == 0x3297 || c == 0x3299)
        return true;
    if (c >= 0x1F000 && c <= 0x1FAFF)
        return !(c >= 0x1F1E6 && c <= 0x1F1FF) && !(c >= 0x1F3FB && c <= 0x1F3FF);
    return c >= 0x1FC00 && c <= 0x1FFFD;
}

bool is_regional_indicator(char32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }

bool is_extend(char32_t c)
{
    return is_combining_mark(c) || c == 0x200C || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0020 && c <= 0xE007F)
        || (c >= 0x1F3FB && c <= 0x1F3FF) || c == 0xFF9E || c == 0xFF9F || (c >= 0xE0100 && c <= 0xE01EF);
}

bool is_ideographic(char32_t c)
{
    return (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF)
        || (c >= 0x20000 && c <= 0x3FFFF) || (c >= 0x3040 && c <= 0x309F);
}

bool is_katakana(char32_t c)
{
    return (c >= 0x30A1 && c <= 0x30FA) || (c >= 0x30FC && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF)
        || (c >= 0x32D0 && c <= 0x32FE) || (c >= 0x3300 && c <= 0x3357) || (c >= 0xFF66 && c <= 0xFF9D);
}

bool is_numeric(char32_t c)
{
    return is_ascii_digit(c) || (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9)
        || (c >= 0x0966 && c <= 0x096F) || (c >= 0xFF10 && c <= 0xFF19);
}

// A letter as the word breaker and the collator see one: anything that is
// not a space, a control, punctuation or a symbol, a digit or a mark.
bool is_letter(char32_t c)
{
    if (c < 0x80)
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return !is_space(c) && !is_ignorable(c) && !is_punctuation_or_symbol(c) && !is_numeric(c) && !is_extend(c)
        && !is_extended_pictographic(c) && !is_regional_indicator(c) && !is_ideographic(c) && !is_katakana(c)
        && c != 0x2028 && c != 0x2029 && c != 0x85 && !is_surrogate(c);
}

// ------------------------------------------------------------ Collator

struct CollatorData {
    static constexpr IntlKind kind = IntlKind::Collator;
    std::string locale;
    std::string usage = "sort";
    std::string sensitivity = "variant";
    bool ignore_punctuation = false;
    std::string collation = "default";
    bool numeric = false;
    std::string case_first = "false";
};

struct CollationElement {
    std::uint32_t group = 0; // 1 space and punctuation, 2 digits, 3 letters, 4 other
    std::u32string primary; // the key within the group (a digit run's value, digits without leading zeros)
    std::u32string secondary; // the marks the canonical decomposition put on it
    int tertiary = 0; // 0 lower or uncased, 1 upper
};

std::vector<CollationElement> collation_elements(std::u16string_view text, CollatorData const& collator)
{
    std::u32string decomposed;
    for (char32_t c : code_points(text)) {
        if (c >= 0xAC00 && c <= 0xD7A3) {
            // A Hangul syllable decomposes by arithmetic (Unicode section 3.12).
            char32_t const index = c - 0xAC00;
            decomposed += static_cast<char32_t>(0x1100 + index / 588);
            decomposed += static_cast<char32_t>(0x1161 + (index % 588) / 28);
            if (index % 28 != 0)
                decomposed += static_cast<char32_t>(0x11A7 + index % 28);
            continue;
        }
        std::u32string_view const d = canonical_decomposition(c);
        if (d.empty())
            decomposed += c;
        else
            decomposed += d;
    }
    // The canonical ordering (UAX #15): marks of different classes after
    // one base sort by class, so equivalent spellings compare equal.
    for (std::size_t i = 1; i < decomposed.size(); ++i) {
        for (std::size_t j = i; j > 0; --j) {
            std::uint8_t const a = canonical_combining_class(decomposed[j - 1]);
            std::uint8_t const b = canonical_combining_class(decomposed[j]);
            if (a == 0 || b == 0 || a <= b)
                break;
            std::swap(decomposed[j - 1], decomposed[j]);
        }
    }
    std::vector<CollationElement> out;
    for (std::size_t i = 0; i < decomposed.size(); ++i) {
        char32_t const c = decomposed[i];
        if (is_combining_mark(c)) {
            if (!out.empty())
                out.back().secondary += c;
            continue;
        }
        if (is_ignorable(c))
            continue;
        CollationElement element;
        if (is_space(c) || is_punctuation_or_symbol(c)) {
            if (collator.ignore_punctuation)
                continue;
            element.group = 1;
            element.primary = std::u32string(1, c);
        } else if (is_ascii_digit(c)) {
            element.group = 2;
            if (collator.numeric) {
                std::size_t j = i;
                while (j < decomposed.size() && is_ascii_digit(decomposed[j]))
                    ++j;
                std::u32string digits = decomposed.substr(i, j - i);
                std::size_t const nonzero = digits.find_first_not_of(U'0');
                digits = nonzero == std::u32string::npos ? std::u32string(U"0") : digits.substr(nonzero);
                // The length first, so that the longer number sorts after.
                element.primary = std::u32string(1, static_cast<char32_t>(digits.size())) + digits;
                i = j - 1;
            } else {
                element.primary = std::u32string(1, c);
            }
        } else {
            char32_t const lower = to_lowercase(c);
            element.group = is_letter(c) ? 3 : 4;
            element.primary = std::u32string(1, lower);
            element.tertiary = lower != c ? 1 : 0;
        }
        out.push_back(std::move(element));
    }
    return out;
}

int compare_strings(CollatorData const& collator, std::u16string_view x, std::u16string_view y)
{
    std::vector<CollationElement> const a = collation_elements(x, collator);
    std::vector<CollationElement> const b = collation_elements(y, collator);
    // Primary.
    std::size_t const n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i].group != b[i].group)
            return a[i].group < b[i].group ? -1 : 1;
        if (a[i].primary != b[i].primary)
            return a[i].primary < b[i].primary ? -1 : 1;
    }
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    std::string const& s = collator.sensitivity;
    if (s == "accent" || s == "variant") {
        for (std::size_t i = 0; i < n; ++i)
            if (a[i].secondary != b[i].secondary)
                return a[i].secondary < b[i].secondary ? -1 : 1;
    }
    if (s == "case" || s == "variant") {
        bool const upper_first = collator.case_first == "upper";
        for (std::size_t i = 0; i < n; ++i) {
            if (a[i].tertiary != b[i].tertiary) {
                bool const a_first = upper_first ? a[i].tertiary > b[i].tertiary : a[i].tertiary < b[i].tertiary;
                return a_first ? -1 : 1;
            }
        }
    }
    return 0;
}

std::optional<bool> initialize_collator(Interpreter& in, CollatorData& collator, Value const& locales, Value const& options_value)
{
    // section 10.1.2.
    std::optional<std::vector<std::string>> const requested = canonicalize_locale_list(in, locales);
    if (!requested)
        return std::nullopt;
    std::optional<Object*> const options = coerce_options_to_object(in, options_value);
    if (!options)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    if (*options)
        in.root(Value::object(*options));
    std::optional<std::optional<std::string>> const usage = get_string_option(in, *options, "usage", { "sort", "search" });
    if (!usage)
        return std::nullopt;
    collator.usage = usage->value_or("sort");
    if (!read_locale_matcher(in, *options))
        return std::nullopt;
    std::optional<std::optional<std::string>> const collation = get_string_option(in, *options, "collation");
    if (!collation)
        return std::nullopt;
    if (*collation && !is_unicode_type_sequence(**collation))
        return in.throw_range_error("Invalid collation : " + **collation);
    std::optional<std::optional<bool>> const numeric = get_boolean_option(in, *options, "numeric");
    if (!numeric)
        return std::nullopt;
    std::optional<std::optional<std::string>> const case_first = get_string_option(in, *options, "caseFirst", { "upper", "lower", "false" });
    if (!case_first)
        return std::nullopt;
    std::optional<std::string> numeric_text;
    if (*numeric)
        numeric_text = **numeric ? "true" : "false";
    ResolvedLocale const resolved = resolve_locale(*requested, {
                                                                   { "co", { "default", "emoji", "eor" }, *collation },
                                                                   { "kf", { "false", "lower", "upper" }, *case_first },
                                                                   { "kn", { "false", "true" }, numeric_text },
                                                               });
    collator.locale = resolved.locale;
    collator.collation = resolved.value("co");
    collator.numeric = resolved.value("kn") == "true";
    collator.case_first = resolved.value("kf");
    std::optional<std::optional<std::string>> const sensitivity = get_string_option(in, *options, "sensitivity",
        { "base", "accent", "case", "variant" });
    if (!sensitivity)
        return std::nullopt;
    collator.sensitivity = sensitivity->value_or("variant");
    std::optional<std::optional<bool>> const ignore = get_boolean_option(in, *options, "ignorePunctuation");
    if (!ignore)
        return std::nullopt;
    collator.ignore_punctuation = ignore->value_or(false);
    return true;
}

std::optional<Value> collator_construct(Interpreter& in, Args args, Object* new_target)
{
    Interpreter::Roots const roots(in);
    in.root(Value::object(new_target));
    std::optional<IntlObjectOf<CollatorData>*> const object = allocate_intl<CollatorData>(in, new_target);
    if (!object)
        return std::nullopt;
    in.root(Value::object(*object));
    if (!initialize_collator(in, (*object)->data, argument(args, 0), argument(args, 1)))
        return std::nullopt;
    return Value::object(*object);
}

std::optional<Value> compare_values(Interpreter& in, CollatorData const& collator, Value const& x, Value const& y)
{
    Interpreter::Roots const roots(in);
    in.root(y);
    std::optional<JsString*> const a = in.to_string(x);
    if (!a)
        return std::nullopt;
    in.root(Value::string(*a));
    std::optional<JsString*> const b = in.to_string(y);
    if (!b)
        return std::nullopt;
    return Value::number(compare_strings(collator, (*a)->view(), (*b)->view()));
}

void install_collator(Interpreter& in, Object& intl)
{
    define_intl_constructor(in, intl, IntlKind::Collator, 0,
        [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
            return collator_construct(interp, args, interp.intrinsics().intl_constructors[static_cast<std::size_t>(IntlKind::Collator)]);
        },
        collator_construct);
    Object& prototype = *intl_prototype(in, IntlKind::Collator);
    Heap::NoCollect const guard(in.heap());
    define_accessor(in, prototype, "compare", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<CollatorData>*> const collator = intl_this<CollatorData>(interp, this_value, "compare");
        if (!collator)
            return std::nullopt;
        if ((*collator)->slot(0).is_undefined()) {
            ClosureFunction* bound = interp.new_closure("", 2, { Value::object(*collator) },
                [](Interpreter& in2, ClosureFunction& self, Value const&, Args args) -> std::optional<Value> {
                    auto* object = static_cast<IntlObjectOf<CollatorData>*>(self.slot(0).as_object());
                    return compare_values(in2, object->data, argument(args, 0), argument(args, 1));
                });
            (*collator)->set_slot(0, Value::object(bound));
        }
        return (*collator)->slot(0);
    });
    define_method(in, prototype, "resolvedOptions", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<CollatorData>*> const found = intl_this<CollatorData>(interp, this_value, "resolvedOptions");
        if (!found)
            return std::nullopt;
        CollatorData const& collator = (*found)->data;
        Heap::NoCollect const no_collect(interp.heap());
        Object* result = interp.new_object();
        result->put(interp.key("locale"), intl_string(interp, collator.locale));
        result->put(interp.key("usage"), intl_string(interp, collator.usage));
        result->put(interp.key("sensitivity"), intl_string(interp, collator.sensitivity));
        result->put(interp.key("ignorePunctuation"), Value::boolean(collator.ignore_punctuation));
        result->put(interp.key("collation"), intl_string(interp, collator.collation));
        result->put(interp.key("numeric"), Value::boolean(collator.numeric));
        result->put(interp.key("caseFirst"), intl_string(interp, collator.case_first));
        return Value::object(result);
    });
}

// ------------------------------------------------------------ DisplayNames

struct DisplayNamesData {
    static constexpr IntlKind kind = IntlKind::DisplayNames;
    std::string locale;
    std::string style = "long";
    std::string type;
    std::string fallback = "code";
    std::string language_display = "dialect";
};

template<typename Table>
std::optional<std::string_view> find_name(Table const& table, std::string_view code)
{
    for (auto const& entry : table)
        if (entry.code == code)
            return entry.name;
    return std::nullopt;
}

std::optional<std::string> language_display_name(DisplayNamesData const& names, LanguageTag const& tag)
{
    std::string const base = tag.base_name();
    if (names.language_display == "dialect") {
        if (std::optional<std::string_view> const dialect = find_name(intl_data::dialect_names, base))
            return std::string(*dialect);
    }
    std::optional<std::string_view> const language = find_name(intl_data::language_names, tag.language);
    if (!language)
        return std::nullopt;
    std::string name(*language);
    std::vector<std::string> qualifiers;
    if (!tag.script.empty()) {
        std::optional<std::string_view> const script = find_name(intl_data::script_names, tag.script);
        qualifiers.push_back(script ? std::string(*script) : tag.script);
    }
    if (!tag.region.empty()) {
        std::optional<std::string_view> const region = find_name(intl_data::region_names, tag.region);
        qualifiers.push_back(region ? std::string(*region) : tag.region);
    }
    for (std::string const& variant : tag.variants)
        qualifiers.push_back(variant);
    if (!qualifiers.empty()) {
        name += " (";
        for (std::size_t i = 0; i < qualifiers.size(); ++i)
            name += (i ? ", " : "") + qualifiers[i];
        name += ")";
    }
    return name;
}

void install_display_names(Interpreter& in, Object& intl)
{
    define_intl_constructor(in, intl, IntlKind::DisplayNames, 2, {},
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            // section 12.1.1.
            Interpreter::Roots const roots(interp);
            interp.root(Value::object(new_target));
            std::optional<IntlObjectOf<DisplayNamesData>*> const object = allocate_intl<DisplayNamesData>(interp, new_target);
            if (!object)
                return std::nullopt;
            interp.root(Value::object(*object));
            DisplayNamesData& names = (*object)->data;
            std::optional<std::vector<std::string>> const requested = canonicalize_locale_list(interp, argument(args, 0));
            if (!requested)
                return std::nullopt;
            std::optional<Object*> const options = get_options_object(interp, argument(args, 1));
            if (!options)
                return std::nullopt;
            if (!read_locale_matcher(interp, *options))
                return std::nullopt;
            names.locale = resolve_locale(*requested, {}).locale;
            std::optional<std::optional<std::string>> const style = get_string_option(interp, *options, "style", { "narrow", "short", "long" });
            if (!style)
                return std::nullopt;
            names.style = style->value_or("long");
            std::optional<std::optional<std::string>> const type = get_string_option(interp, *options, "type",
                { "language", "region", "script", "currency", "calendar", "dateTimeField" });
            if (!type)
                return std::nullopt;
            if (!*type)
                return interp.throw_type_error("Required option 'type' is missing");
            names.type = **type;
            std::optional<std::optional<std::string>> const fallback = get_string_option(interp, *options, "fallback", { "code", "none" });
            if (!fallback)
                return std::nullopt;
            names.fallback = fallback->value_or("code");
            std::optional<std::optional<std::string>> const language_display = get_string_option(interp, *options, "languageDisplay",
                { "dialect", "standard" });
            if (!language_display)
                return std::nullopt;
            if (names.type == "language")
                names.language_display = language_display->value_or("dialect");
            return Value::object(*object);
        });
    Object& prototype = *intl_prototype(in, IntlKind::DisplayNames);
    Heap::NoCollect const guard(in.heap());
    define_method(in, prototype, "of", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<DisplayNamesData>*> const found = intl_this<DisplayNamesData>(interp, this_value, "of");
        if (!found)
            return std::nullopt;
        DisplayNamesData const& names = (*found)->data;
        std::optional<JsString*> const code_string = interp.to_string(argument(args, 0));
        if (!code_string)
            return std::nullopt;
        std::string const code = (*code_string)->to_utf8();
        auto invalid = [&]() { return interp.throw_range_error("invalid_argument"); };
        // CanonicalCodeForDisplayNames (section 12.5.1), then the name.
        std::string canonical;
        std::optional<std::string> name;
        if (names.type == "language") {
            std::optional<LanguageTag> tag = parse_language_tag(code);
            if (!tag || tag->has_unicode_extension || tag->has_transformed_extension || !tag->other_extensions.empty()
                || !tag->private_use.empty())
                return invalid();
            canonicalize(*tag);
            canonical = tag->to_string();
            name = language_display_name(names, *tag);
        } else if (names.type == "region") {
            if (!is_unicode_region_subtag(code))
                return invalid();
            std::string upper_code = code;
            for (char& c : upper_code)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            canonical = upper_code;
            if (names.style != "long") {
                if (std::optional<std::string_view> const short_name = find_name(intl_data::region_short_names, canonical))
                    name = std::string(*short_name);
            }
            if (!name) {
                if (std::optional<std::string_view> const found_name = find_name(intl_data::region_names, canonical))
                    name = std::string(*found_name);
            }
        } else if (names.type == "script") {
            if (!is_unicode_script_subtag(code))
                return invalid();
            canonical = code;
            for (std::size_t i = 0; i < canonical.size(); ++i) {
                char& c = canonical[i];
                if (i == 0 && c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
                else if (i > 0 && c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            }
            if (std::optional<std::string_view> const found_name = find_name(intl_data::script_names, canonical))
                name = std::string(*found_name);
        } else if (names.type == "currency") {
            bool const well_formed = code.size() == 3 && std::all_of(code.begin(), code.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            });
            if (!well_formed)
                return invalid();
            canonical = code;
            for (char& c : canonical)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            for (auto const& currency : intl_data::currencies)
                if (currency.code == canonical)
                    name = std::string(currency.name);
        } else if (names.type == "calendar") {
            if (!is_unicode_type_sequence(code))
                return invalid();
            canonical = code;
            for (char& c : canonical)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            if (canonical == "gregorian")
                canonical = "gregory";
            if (std::optional<std::string_view> const found_name = find_name(intl_data::calendar_names, canonical))
                name = std::string(*found_name);
        } else {
            bool known = false;
            for (auto const& field : intl_data::field_names) {
                if (field.code == code) {
                    known = true;
                    name = std::string(names.style == "short" ? field.short_name : names.style == "narrow" ? field.narrow_name : field.long_name);
                }
            }
            if (!known)
                return invalid();
            canonical = code;
        }
        if (name)
            return intl_string(interp, *name);
        if (names.fallback == "code")
            return intl_string(interp, canonical);
        return Value::undefined();
    });
    define_method(in, prototype, "resolvedOptions", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<DisplayNamesData>*> const found = intl_this<DisplayNamesData>(interp, this_value, "resolvedOptions");
        if (!found)
            return std::nullopt;
        DisplayNamesData const& names = (*found)->data;
        Heap::NoCollect const no_collect(interp.heap());
        Object* result = interp.new_object();
        result->put(interp.key("locale"), intl_string(interp, names.locale));
        result->put(interp.key("style"), intl_string(interp, names.style));
        result->put(interp.key("type"), intl_string(interp, names.type));
        result->put(interp.key("fallback"), intl_string(interp, names.fallback));
        if (names.type == "language")
            result->put(interp.key("languageDisplay"), intl_string(interp, names.language_display));
        return Value::object(result);
    });
}

// ------------------------------------------------------------ ListFormat

struct ListFormatData {
    static constexpr IntlKind kind = IntlKind::ListFormat;
    std::string locale;
    std::string type = "conjunction";
    std::string style = "long";
};

std::vector<IntlPart> partition_list(ListFormatData const& list, std::vector<std::u16string> const& items)
{
    std::size_t const type = list.type == "disjunction" ? 1 : list.type == "unit" ? 2 : 0;
    std::size_t const width = list.style == "short" ? 1 : list.style == "narrow" ? 2 : 0;
    intl_data::ListPattern pattern = intl_data::list_patterns[type][width];
    bool const british = list.locale.starts_with("en-GB");
    if (british && type == 0 && width < 2) {
        pattern.end = " and ";
        pattern.pair = " and ";
    } else if (british && type == 1) {
        pattern.end = " or ";
    }
    std::vector<IntlPart> parts;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            std::string_view joiner = pattern.middle;
            if (items.size() == 2)
                joiner = pattern.pair;
            else if (i == items.size() - 1)
                joiner = pattern.end;
            parts.push_back({ "literal", utf16_from_utf8(joiner), {}, {} });
        }
        parts.push_back({ "element", items[i], {}, {} });
    }
    return parts;
}

// StringListFromIterable (section 13.5.3).
std::optional<std::vector<std::u16string>> string_list_from_iterable(Interpreter& in, Value const& iterable)
{
    std::vector<std::u16string> list;
    if (iterable.is_undefined())
        return list;
    Interpreter::Roots const roots(in);
    std::optional<IteratorRecord> record = in.get_iterator(iterable);
    if (!record)
        return std::nullopt;
    in.root(record->iterator);
    in.root(record->next_method);
    while (true) {
        Value value;
        std::optional<bool> const stepped = in.iterator_step(*record, value);
        if (!stepped)
            return std::nullopt;
        if (!*stepped)
            return list;
        if (!value.is_string()) {
            in.throw_type_error("Iterable yielded " + in.describe(value) + " which is not a string");
            in.iterator_close(*record, true);
            return std::nullopt;
        }
        list.push_back(std::u16string(value.as_string()->view()));
    }
}

void install_list_format(Interpreter& in, Object& intl)
{
    define_intl_constructor(in, intl, IntlKind::ListFormat, 0, {},
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            // section 13.1.1.
            Interpreter::Roots const roots(interp);
            interp.root(Value::object(new_target));
            std::optional<IntlObjectOf<ListFormatData>*> const object = allocate_intl<ListFormatData>(interp, new_target);
            if (!object)
                return std::nullopt;
            interp.root(Value::object(*object));
            ListFormatData& list = (*object)->data;
            std::optional<std::vector<std::string>> const requested = canonicalize_locale_list(interp, argument(args, 0));
            if (!requested)
                return std::nullopt;
            std::optional<Object*> const options = get_options_object(interp, argument(args, 1));
            if (!options)
                return std::nullopt;
            if (!read_locale_matcher(interp, *options))
                return std::nullopt;
            list.locale = resolve_locale(*requested, {}).locale;
            std::optional<std::optional<std::string>> const type = get_string_option(interp, *options, "type", { "conjunction", "disjunction", "unit" });
            if (!type)
                return std::nullopt;
            list.type = type->value_or("conjunction");
            std::optional<std::optional<std::string>> const style = get_string_option(interp, *options, "style", { "long", "short", "narrow" });
            if (!style)
                return std::nullopt;
            list.style = style->value_or("long");
            return Value::object(*object);
        });
    Object& prototype = *intl_prototype(in, IntlKind::ListFormat);
    Heap::NoCollect const guard(in.heap());
    define_method(in, prototype, "format", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<ListFormatData>*> const list = intl_this<ListFormatData>(interp, this_value, "format");
        if (!list)
            return std::nullopt;
        std::optional<std::vector<std::u16string>> const items = string_list_from_iterable(interp, argument(args, 0));
        if (!items)
            return std::nullopt;
        return intl_string(interp, join_parts(partition_list((*list)->data, *items)));
    });
    define_method(in, prototype, "formatToParts", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<ListFormatData>*> const list = intl_this<ListFormatData>(interp, this_value, "formatToParts");
        if (!list)
            return std::nullopt;
        std::optional<std::vector<std::u16string>> const items = string_list_from_iterable(interp, argument(args, 0));
        if (!items)
            return std::nullopt;
        return parts_to_array(interp, partition_list((*list)->data, *items));
    });
    define_method(in, prototype, "resolvedOptions", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<ListFormatData>*> const found = intl_this<ListFormatData>(interp, this_value, "resolvedOptions");
        if (!found)
            return std::nullopt;
        Heap::NoCollect const no_collect(interp.heap());
        Object* result = interp.new_object();
        result->put(interp.key("locale"), intl_string(interp, (*found)->data.locale));
        result->put(interp.key("type"), intl_string(interp, (*found)->data.type));
        result->put(interp.key("style"), intl_string(interp, (*found)->data.style));
        return Value::object(result);
    });
}

// ------------------------------------------------------------ Segmenter

enum class Granularity : std::uint8_t { Grapheme, Word, Sentence };

struct SegmenterData {
    static constexpr IntlKind kind = IntlKind::Segmenter;
    std::string locale;
    Granularity granularity = Granularity::Grapheme;
};
// slot 0: the segmenter; slot 1: the string. The boundaries (code-unit
// offsets, 0 and the length among them) are found once, on first use, so
// walking a long text is linear.
struct SegmentsData {
    static constexpr IntlKind kind = IntlKind::Segments;
    Granularity granularity = Granularity::Grapheme;
    std::optional<std::vector<std::size_t>> boundaries;
};
struct SegmentIteratorData {
    static constexpr IntlKind kind = IntlKind::SegmentIterator;
    Granularity granularity = Granularity::Grapheme;
    std::size_t position = 0;
    std::optional<std::vector<std::size_t>> boundaries;
};

// The text as code points with the UTF-16 offset of each.
struct PointText {
    std::vector<char32_t> points;
    std::vector<std::size_t> offsets; // one more than points: the end
};

PointText point_text(std::u16string_view text)
{
    PointText out;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t units = 1;
        out.points.push_back(code_point_at(text, i, &units));
        out.offsets.push_back(i);
        i += units;
    }
    out.offsets.push_back(text.size());
    return out;
}

enum class GraphemeClass : std::uint8_t { Other, CR, LF, Control, Extend, ZWJ, RegionalIndicator, L, V, T, LV, LVT, Pictographic };

GraphemeClass grapheme_class(char32_t c)
{
    if (c == '\r')
        return GraphemeClass::CR;
    if (c == '\n')
        return GraphemeClass::LF;
    if (c == 0x200D)
        return GraphemeClass::ZWJ;
    if (is_extend(c))
        return GraphemeClass::Extend;
    if (is_control(c) || c == 0x2028 || c == 0x2029 || (is_default_ignorable(c) && c != 0x200C && c != 0x034F))
        return GraphemeClass::Control;
    if (is_regional_indicator(c))
        return GraphemeClass::RegionalIndicator;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0xA960 && c <= 0xA97C))
        return GraphemeClass::L;
    if ((c >= 0x1160 && c <= 0x11A7) || (c >= 0xD7B0 && c <= 0xD7C6))
        return GraphemeClass::V;
    if ((c >= 0x11A8 && c <= 0x11FF) || (c >= 0xD7CB && c <= 0xD7FB))
        return GraphemeClass::T;
    if (c >= 0xAC00 && c <= 0xD7A3)
        return (c - 0xAC00) % 28 == 0 ? GraphemeClass::LV : GraphemeClass::LVT;
    if (is_extended_pictographic(c))
        return GraphemeClass::Pictographic;
    return GraphemeClass::Other;
}

// Whether UAX #29's grapheme rules put a boundary before point i.
bool grapheme_break_before(std::vector<char32_t> const& points, std::size_t i)
{
    using G = GraphemeClass;
    G const before = grapheme_class(points[i - 1]);
    G const after = grapheme_class(points[i]);
    if (before == G::CR && after == G::LF)
        return false; // GB3
    if (before == G::CR || before == G::LF || before == G::Control)
        return true; // GB4
    if (after == G::CR || after == G::LF || after == G::Control)
        return true; // GB5
    if (before == G::L && (after == G::L || after == G::V || after == G::LV || after == G::LVT))
        return false; // GB6
    if ((before == G::LV || before == G::V) && (after == G::V || after == G::T))
        return false; // GB7
    if ((before == G::LVT || before == G::T) && after == G::T)
        return false; // GB8
    if (after == G::Extend || after == G::ZWJ)
        return false; // GB9, GB9a
    if (before == G::ZWJ && after == G::Pictographic) {
        // GB11: ExtPict Extend* ZWJ x ExtPict.
        std::size_t j = i - 1;
        while (j > 0 && grapheme_class(points[j - 1]) == G::Extend)
            --j;
        if (j > 0 && grapheme_class(points[j - 1]) == G::Pictographic)
            return false;
    }
    if (before == G::RegionalIndicator && after == G::RegionalIndicator) {
        // GB12, GB13: pairs.
        std::size_t count = 0;
        std::size_t j = i;
        while (j > 0 && grapheme_class(points[j - 1]) == G::RegionalIndicator) {
            ++count;
            --j;
        }
        return count % 2 == 0;
    }
    return true;
}

enum class WordClass : std::uint8_t { Other, CR, LF, Newline, Extend, ZWJ, RegionalIndicator, Katakana, Letter, Ideographic, Numeric, MidLetter, MidNum, MidNumLet, SingleQuote, ExtendNumLet, Space, Pictographic };

WordClass word_class(char32_t c)
{
    using W = WordClass;
    if (c == '\r')
        return W::CR;
    if (c == '\n')
        return W::LF;
    if (c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028 || c == 0x2029)
        return W::Newline;
    if (c == 0x200D)
        return W::ZWJ;
    if (is_extend(c) || (is_default_ignorable(c) && !is_control(c)))
        return W::Extend;
    if (is_regional_indicator(c))
        return W::RegionalIndicator;
    if (is_katakana(c))
        return W::Katakana;
    if (is_ideographic(c))
        return W::Ideographic;
    if (is_numeric(c))
        return W::Numeric;
    if (c == ':' || c == 0xB7 || c == 0x387 || c == 0x5F4 || c == 0x2027 || c == 0xFE13 || c == 0xFE55 || c == 0xFF1A)
        return W::MidLetter;
    if (c == ',' || c == ';' || c == 0x37E || c == 0x589 || c == 0x60C || c == 0x60D || c == 0x66C || c == 0x7F8
        || c == 0x2044 || c == 0xFE10 || c == 0xFE14 || c == 0xFE50 || c == 0xFE54 || c == 0xFF0C || c == 0xFF1B)
        return W::MidNum;
    if (c == '.' || c == 0x2018 || c == 0x2019 || c == 0x2024 || c == 0xFE52 || c == 0xFF07 || c == 0xFF0E)
        return W::MidNumLet;
    if (c == '\'')
        return W::SingleQuote;
    if (c == '_' || c == 0x202F || c == 0x203F || c == 0x2040 || c == 0x2054 || c == 0xFE33 || c == 0xFE34
        || (c >= 0xFE4D && c <= 0xFE4F) || c == 0xFF3F)
        return W::ExtendNumLet;
    if (c == ' ' || c == 0x1680 || (c >= 0x2000 && c <= 0x2006) || (c >= 0x2008 && c <= 0x200A) || c == 0x205F || c == 0x3000)
        return W::Space;
    if (is_extended_pictographic(c))
        return W::Pictographic;
    if (is_letter(c))
        return W::Letter;
    return W::Other;
}

// UAX #29 word boundaries over the whole text: true at i = a boundary
// before point i.
std::vector<bool> word_boundaries(std::vector<char32_t> const& points)
{
    using W = WordClass;
    std::size_t const n = points.size();
    std::vector<W> classes(n);
    for (std::size_t i = 0; i < n; ++i)
        classes[i] = word_class(points[i]);
    auto skippable = [](W w) { return w == W::Extend || w == W::ZWJ; };
    // The previous and next classes with Extend, Format and ZWJ skipped (WB4).
    auto previous = [&](std::size_t i) -> std::optional<std::size_t> {
        while (i > 0) {
            --i;
            if (!skippable(classes[i]))
                return i;
        }
        return std::nullopt;
    };
    auto next = [&](std::size_t i) -> std::optional<std::size_t> {
        for (std::size_t j = i + 1; j < n; ++j)
            if (!skippable(classes[j]))
                return j;
        return std::nullopt;
    };
    auto ah_letter = [](W w) { return w == W::Letter; };
    auto mid_letter_like = [](W w) { return w == W::MidLetter || w == W::MidNumLet || w == W::SingleQuote; };
    auto mid_num_like = [](W w) { return w == W::MidNum || w == W::MidNumLet || w == W::SingleQuote; };
    std::vector<bool> result(n + 1, false);
    result[0] = true;
    result[n] = true;
    for (std::size_t i = 1; i < n; ++i) {
        W const b = classes[i - 1];
        W const a = classes[i];
        bool boundary = true;
        if (b == W::CR && a == W::LF)
            boundary = false; // WB3
        else if (b == W::CR || b == W::LF || b == W::Newline || a == W::CR || a == W::LF || a == W::Newline)
            boundary = true; // WB3a, WB3b
        else if (b == W::ZWJ && a == W::Pictographic)
            boundary = false; // WB3c
        else if (b == W::Space && a == W::Space)
            boundary = false; // WB3d
        else if (skippable(a))
            boundary = false; // WB4
        else {
            std::optional<std::size_t> const p = previous(i);
            if (!p) {
                boundary = true;
            } else {
                W const pb = classes[*p];
                std::optional<std::size_t> const pp = previous(*p);
                std::optional<std::size_t> const nn = next(i);
                W const ppb = pp ? classes[*pp] : W::Other;
                W const na = nn ? classes[*nn] : W::Other;
                if (ah_letter(pb) && ah_letter(a))
                    boundary = false; // WB5
                else if (ah_letter(pb) && mid_letter_like(a) && ah_letter(na))
                    boundary = false; // WB6
                else if (ah_letter(ppb) && mid_letter_like(pb) && ah_letter(a))
                    boundary = false; // WB7
                else if (pb == W::Numeric && a == W::Numeric)
                    boundary = false; // WB8
                else if (ah_letter(pb) && a == W::Numeric)
                    boundary = false; // WB9
                else if (pb == W::Numeric && ah_letter(a))
                    boundary = false; // WB10
                else if (ppb == W::Numeric && mid_num_like(pb) && a == W::Numeric)
                    boundary = false; // WB11
                else if (pb == W::Numeric && mid_num_like(a) && na == W::Numeric)
                    boundary = false; // WB12
                else if (pb == W::Katakana && a == W::Katakana)
                    boundary = false; // WB13
                else if ((ah_letter(pb) || pb == W::Numeric || pb == W::Katakana || pb == W::ExtendNumLet) && a == W::ExtendNumLet)
                    boundary = false; // WB13a
                else if (pb == W::ExtendNumLet && (ah_letter(a) || a == W::Numeric || a == W::Katakana))
                    boundary = false; // WB13b
                else if (pb == W::RegionalIndicator && a == W::RegionalIndicator) {
                    std::size_t count = 0;
                    std::size_t j = *p + 1;
                    while (j > 0 && (classes[j - 1] == W::RegionalIndicator || skippable(classes[j - 1]))) {
                        if (classes[j - 1] == W::RegionalIndicator)
                            ++count;
                        --j;
                    }
                    boundary = count % 2 == 0; // WB15, WB16
                }
            }
        }
        result[i] = boundary;
    }
    return result;
}

bool is_sentence_terminator(char32_t c)
{
    return c == '!' || c == '?' || c == 0x589 || c == 0x61F || c == 0x6D4 || (c >= 0x700 && c <= 0x702) || c == 0x964
        || c == 0x965 || c == 0x203C || c == 0x203D || (c >= 0x2047 && c <= 0x2049) || c == 0x3002 || c == 0xFF01
        || c == 0xFF1F || c == 0xFF61;
}

bool is_close(char32_t c)
{
    return c == '"' || c == '\'' || c == ')' || c == ']' || c == '}' || c == 0xAB || c == 0xBB || (c >= 0x2018 && c <= 0x201F)
        || c == 0x2039 || c == 0x203A || c == 0x3009 || c == 0x300B || c == 0x300D || c == 0x300F || c == 0x3011;
}

bool is_paragraph_separator(char32_t c) { return c == '\r' || c == '\n' || c == 0x85 || c == 0x2028 || c == 0x2029; }

bool is_upper(char32_t c) { return to_lowercase(c) != c; }
bool is_lower(char32_t c) { return to_uppercase(c) != c; }

// UAX #29 sentence boundaries (SB1-SB11), the grammar reduced to the
// terminators, closers, spaces and separators above.
std::vector<bool> sentence_boundaries(std::vector<char32_t> const& points)
{
    std::size_t const n = points.size();
    std::vector<bool> result(n + 1, false);
    result[0] = true;
    result[n] = true;
    std::size_t i = 0;
    while (i < n) {
        char32_t const c = points[i];
        if (is_paragraph_separator(c)) {
            std::size_t j = i + 1;
            if (c == '\r' && j < n && points[j] == '\n')
                ++j;
            if (j < n)
                result[j] = true;
            i = j;
            continue;
        }
        bool const aterm = c == '.' || c == 0x2024 || c == 0xFE52 || c == 0xFF0E;
        if (!aterm && !is_sentence_terminator(c)) {
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        while (j < n && is_extend(points[j]))
            ++j;
        if (aterm && j < n && is_numeric(points[j])) {
            i = j; // SB6
            continue;
        }
        if (aterm && j < n && is_upper(points[j]) && i > 0 && (is_upper(points[i - 1]) || is_lower(points[i - 1]))) {
            i = j; // SB7
            continue;
        }
        while (j < n && (is_close(points[j]) || is_extend(points[j])))
            ++j;
        std::size_t const after_close = j;
        while (j < n && (is_space(points[j]) && points[j] != 0x85))
            ++j;
        if (aterm) {
            // SB8: no break when a lower-case letter follows before any
            // other letter or terminator.
            std::size_t k = j;
            while (k < n && !is_letter(points[k]) && !is_paragraph_separator(points[k]) && points[k] != '.'
                && !is_sentence_terminator(points[k]) && !is_numeric(points[k]))
                ++k;
            if (k < n && is_lower(points[k])) {
                i = j;
                continue;
            }
        }
        // SB8a: a continuation or another terminator stays in the sentence.
        if (j < n && (points[j] == ',' || points[j] == ';' || points[j] == ':' || points[j] == '-' || points[j] == 0x2013
                || points[j] == 0x2014 || points[j] == '.' || is_sentence_terminator(points[j]))
            && j == after_close) {
            i = j;
            continue;
        }
        if (j < n && is_paragraph_separator(points[j])) {
            ++j;
            if (points[j - 1] == '\r' && j < n && points[j] == '\n')
                ++j;
        }
        if (j < n)
            result[j] = true;
        i = j;
    }
    return result;
}

// Every boundary of the text, as code-point booleans.
std::vector<bool> boundaries(Granularity granularity, std::vector<char32_t> const& points)
{
    if (granularity == Granularity::Word)
        return word_boundaries(points);
    if (granularity == Granularity::Sentence)
        return sentence_boundaries(points);
    std::vector<bool> result(points.size() + 1, false);
    result[0] = true;
    result[points.size()] = true;
    for (std::size_t i = 1; i < points.size(); ++i)
        result[i] = grapheme_break_before(points, i);
    return result;
}

// Every boundary of the text as a code-unit offset, ascending.
std::vector<std::size_t> boundary_offsets(Granularity granularity, std::u16string_view text)
{
    PointText const pt = point_text(text);
    std::vector<bool> const b = boundaries(granularity, pt.points);
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < b.size(); ++i)
        if (b[i])
            out.push_back(pt.offsets[i]);
    return out;
}

// The segment around code unit `index` (below the length): its start and
// end in code units.
std::pair<std::size_t, std::size_t> segment_around(std::vector<std::size_t> const& offsets, std::size_t index)
{
    auto const after = std::upper_bound(offsets.begin(), offsets.end(), index);
    return { *(after - 1), *after };
}

bool is_word_like(std::u16string_view segment)
{
    for (char32_t c : code_points(segment)) {
        WordClass const w = word_class(c);
        if (w == WordClass::Letter || w == WordClass::Numeric || w == WordClass::Katakana || w == WordClass::Ideographic)
            return true;
    }
    return false;
}

Value segment_data(Interpreter& in, Granularity granularity, JsString* string, std::size_t start, std::size_t end)
{
    // CreateSegmentDataObject (section 18.7.1).
    Heap::NoCollect const guard(in.heap());
    Object* result = in.new_object();
    std::u16string_view const segment = string->view().substr(start, end - start);
    result->put(in.key("segment"), intl_string(in, segment));
    result->put(in.key("index"), Value::number(static_cast<double>(start)));
    result->put(in.key("input"), Value::string(string));
    if (granularity == Granularity::Word)
        result->put(in.key("isWordLike"), Value::boolean(is_word_like(segment)));
    return Value::object(result);
}

std::string_view granularity_name(Granularity granularity)
{
    switch (granularity) {
    case Granularity::Grapheme: return "grapheme";
    case Granularity::Word: return "word";
    case Granularity::Sentence: return "sentence";
    }
    return "grapheme";
}

void install_segmenter(Interpreter& in, Object& intl)
{
    define_intl_constructor(in, intl, IntlKind::Segmenter, 0, {},
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            // section 18.1.1.
            Interpreter::Roots const roots(interp);
            interp.root(Value::object(new_target));
            std::optional<IntlObjectOf<SegmenterData>*> const object = allocate_intl<SegmenterData>(interp, new_target);
            if (!object)
                return std::nullopt;
            interp.root(Value::object(*object));
            std::optional<std::vector<std::string>> const requested = canonicalize_locale_list(interp, argument(args, 0));
            if (!requested)
                return std::nullopt;
            std::optional<Object*> const options = get_options_object(interp, argument(args, 1));
            if (!options)
                return std::nullopt;
            if (!read_locale_matcher(interp, *options))
                return std::nullopt;
            (*object)->data.locale = resolve_locale(*requested, {}).locale;
            std::optional<std::optional<std::string>> const granularity = get_string_option(interp, *options, "granularity",
                { "grapheme", "word", "sentence" });
            if (!granularity)
                return std::nullopt;
            std::string const g = granularity->value_or("grapheme");
            (*object)->data.granularity = g == "word" ? Granularity::Word : g == "sentence" ? Granularity::Sentence : Granularity::Grapheme;
            return Value::object(*object);
        });
    Intrinsics& i = in.intrinsics();
    Object& prototype = *intl_prototype(in, IntlKind::Segmenter);
    Heap::NoCollect const guard(in.heap());

    // %IntlSegmentsPrototype% and %IntlSegmentIteratorPrototype%.
    Object* segments_prototype = in.new_object();
    i.intl_prototypes[static_cast<std::size_t>(IntlKind::Segments)] = segments_prototype;
    Object* iterator_prototype = in.new_object(i.iterator_prototype);
    i.intl_prototypes[static_cast<std::size_t>(IntlKind::SegmentIterator)] = iterator_prototype;
    iterator_prototype->put(PropertyKey::symbol(in.atoms().symbol_to_string_tag), intl_string(in, "Segmenter String Iterator"), Configurable);

    define_method(in, prototype, "segment", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<SegmenterData>*> const segmenter = intl_this<SegmenterData>(interp, this_value, "segment");
        if (!segmenter)
            return std::nullopt;
        std::optional<JsString*> const string = interp.to_string(argument(args, 0));
        if (!string)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        interp.root(Value::string(*string));
        auto* segments = interp.heap().allocate<IntlObjectOf<SegmentsData>>(intl_prototype(interp, IntlKind::Segments));
        segments->data.granularity = (*segmenter)->data.granularity;
        segments->set_slot(0, this_value);
        segments->set_slot(1, Value::string(*string));
        return Value::object(segments);
    });
    define_method(in, prototype, "resolvedOptions", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<SegmenterData>*> const found = intl_this<SegmenterData>(interp, this_value, "resolvedOptions");
        if (!found)
            return std::nullopt;
        Heap::NoCollect const no_collect(interp.heap());
        Object* result = interp.new_object();
        result->put(interp.key("locale"), intl_string(interp, (*found)->data.locale));
        result->put(interp.key("granularity"), intl_string(interp, granularity_name((*found)->data.granularity)));
        return Value::object(result);
    });

    define_method(in, *segments_prototype, "containing", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        IntlObjectOf<SegmentsData>* segments = intl_cast<SegmentsData>(this_value);
        if (!segments)
            return interp.throw_type_error("Method %Segments.prototype%.containing called on incompatible receiver " + interp.describe(this_value));
        std::optional<double> const n = interp.to_integer_or_infinity(argument(args, 0));
        if (!n)
            return std::nullopt;
        JsString* string = segments->slot(1).as_string();
        if (*n < 0 || *n >= static_cast<double>(string->length()))
            return Value::undefined();
        if (!segments->data.boundaries)
            segments->data.boundaries = boundary_offsets(segments->data.granularity, string->view());
        auto const [start, end] = segment_around(*segments->data.boundaries, static_cast<std::size_t>(*n));
        return segment_data(interp, segments->data.granularity, string, start, end);
    });
    NativeFunction* iterator_method = define_method(in, *segments_prototype, "[Symbol.iterator]", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        IntlObjectOf<SegmentsData>* segments = intl_cast<SegmentsData>(this_value);
        if (!segments)
            return interp.throw_type_error("Method %Segments.prototype%[@@iterator] called on incompatible receiver " + interp.describe(this_value));
        auto* iterator = interp.heap().allocate<IntlObjectOf<SegmentIteratorData>>(intl_prototype(interp, IntlKind::SegmentIterator));
        iterator->data.granularity = segments->data.granularity;
        iterator->set_slot(0, segments->slot(0));
        iterator->set_slot(1, segments->slot(1));
        return Value::object(iterator);
    });
    // The method above was defined under a string name for its function
    // name; move it to the symbol.
    segments_prototype->delete_property(in.key("[Symbol.iterator]"));
    segments_prototype->put(PropertyKey::symbol(in.atoms().symbol_iterator), Value::object(iterator_method), builtin_attributes);
    define_method(in, *iterator_prototype, "next", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        IntlObjectOf<SegmentIteratorData>* iterator = intl_cast<SegmentIteratorData>(this_value);
        if (!iterator)
            return interp.throw_type_error("Method %SegmentIterator.prototype%.next called on incompatible receiver " + interp.describe(this_value));
        JsString* string = iterator->slot(1).as_string();
        std::size_t const start = iterator->data.position;
        if (start >= string->length())
            return Value::object(interp.create_iter_result(Value::undefined(), true));
        if (!iterator->data.boundaries)
            iterator->data.boundaries = boundary_offsets(iterator->data.granularity, string->view());
        std::size_t const end = segment_around(*iterator->data.boundaries, start).second;
        iterator->data.position = end;
        Interpreter::Roots const roots(interp);
        Value const data = segment_data(interp, iterator->data.granularity, string, start, end);
        interp.root(data);
        return Value::object(interp.create_iter_result(data, false));
    });
}

// ------------------------------------------------ String's locale methods

std::optional<Value> transform_case(Interpreter& in, Value const& this_value, Value const& locales, bool upper, std::string_view method)
{
    // TransformCase (section 19.1.2.1).
    if (this_value.is_nullish())
        return in.throw_type_error("String.prototype." + std::string(method) + " called on null or undefined");
    std::optional<JsString*> const string = in.to_string(this_value);
    if (!string)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(Value::string(*string));
    std::optional<std::vector<std::string>> const requested = canonicalize_locale_list(in, locales);
    if (!requested)
        return std::nullopt;
    std::string language = "en";
    if (!requested->empty()) {
        if (std::optional<LanguageTag> const tag = parse_language_tag(requested->front()))
            language = tag->language;
    }
    bool const turkic = language == "tr" || language == "az";
    bool const lithuanian = language == "lt";
    std::vector<char32_t> const points = code_points((*string)->view());
    // SpecialCasing's conditions: More_Above (a class-230 mark follows
    // before the next base) and After_Soft_Dotted (a soft-dotted letter
    // precedes with no base or class-230 mark between).
    auto more_above = [&](std::size_t i) {
        for (std::size_t j = i + 1; j < points.size(); ++j) {
            std::uint8_t const ccc = canonical_combining_class(points[j]);
            if (ccc == 0)
                return false;
            if (ccc == 230)
                return true;
        }
        return false;
    };
    auto soft_dotted = [](char32_t c) {
        static constexpr char32_t set[] = { 0x69, 0x6A, 0x12F, 0x249, 0x268, 0x29D, 0x2B2, 0x3F3, 0x456, 0x458, 0x1D62,
            0x1D96, 0x1DA4, 0x1DA8, 0x1E2D, 0x1ECB, 0x2071, 0x2148, 0x2149, 0x2C7C, 0x1DF1A, 0x1E04C, 0x1E04D, 0x1E068 };
        if (c >= 0x1D422 && c <= 0x1D693)
            return (c - 0x1D422) % 52 <= 1;
        return std::find(std::begin(set), std::end(set), c) != std::end(set);
    };
    auto after_soft_dotted = [&](std::size_t i) {
        for (std::size_t j = i; j > 0; --j) {
            char32_t const p = points[j - 1];
            if (soft_dotted(p))
                return true;
            std::uint8_t const ccc = canonical_combining_class(p);
            if (ccc == 0 || ccc == 230)
                return false;
        }
        return false;
    };
    std::u16string out;
    for (std::size_t i = 0; i < points.size(); ++i) {
        char32_t const c = points[i];
        bool const next_is_above = more_above(i);
        if (upper) {
            if (turkic && c == 'i') {
                append_code_point(out, 0x130);
            } else if (lithuanian && c == 0x307 && after_soft_dotted(i)) {
                // The dot above a soft-dotted letter goes with the dot.
            } else if (c == 0xDF) {
                out += u"SS";
            } else {
                append_code_point(out, to_uppercase(c));
            }
        } else {
            if (turkic && c == 0x130) {
                out += u'i';
            } else if (turkic && c == 'I') {
                // Before_Dot: a dot above after marks of other classes
                // joins the I into a plain i.
                std::size_t j = i + 1;
                while (j < points.size() && canonical_combining_class(points[j]) != 0 && canonical_combining_class(points[j]) != 230)
                    ++j;
                if (j < points.size() && points[j] == 0x307) {
                    out += u'i';
                    for (std::size_t k = i + 1; k < j; ++k)
                        append_code_point(out, points[k]);
                    i = j;
                } else {
                    append_code_point(out, 0x131);
                }
            } else if (c == 0x130) {
                // An i with its dot kept as a combining dot above.
                out += u'i';
                append_code_point(out, 0x307);
            } else if (lithuanian && (c == 'I' || c == 'J' || c == 0x12E) && next_is_above) {
                append_code_point(out, to_lowercase(c));
                append_code_point(out, 0x307);
            } else if (lithuanian && (c == 0xCC || c == 0xCD || c == 0x128)) {
                // I with grave, acute or tilde: i, the dot, then the accent.
                out += u'i';
                append_code_point(out, 0x307);
                append_code_point(out, c == 0xCC ? 0x300 : c == 0xCD ? 0x301 : 0x303);
            } else if (c == 0x3A3) {
                // Final sigma: at the end of a word, after a letter.
                bool const after_letter = i > 0 && is_letter(points[i - 1]);
                bool const before_letter = i + 1 < points.size() && is_letter(points[i + 1]);
                append_code_point(out, after_letter && !before_letter ? 0x3C2 : 0x3C3);
            } else {
                append_code_point(out, to_lowercase(c));
            }
        }
    }
    return intl_string(in, std::u16string_view(out));
}

} // namespace

void install_intl_collator(Interpreter& in, Object& intl)
{
    install_collator(in, intl);
}

void install_intl_display_names(Interpreter& in, Object& intl)
{
    install_display_names(in, intl);
}

void install_intl_list_format(Interpreter& in, Object& intl)
{
    install_list_format(in, intl);
}

void install_intl_segmenter(Interpreter& in, Object& intl)
{
    install_segmenter(in, intl);
}

void install_intl_string_methods(Interpreter& in)
{
    Object& string_prototype = *in.intrinsics().string_prototype;
    Heap::NoCollect const guard(in.heap());
    define_method(in, string_prototype, "localeCompare", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // section 19.1.1: a Collator of the arguments compares the two strings.
        if (this_value.is_nullish())
            return interp.throw_type_error("String.prototype.localeCompare called on null or undefined");
        std::optional<JsString*> const string = interp.to_string(this_value);
        if (!string)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        interp.root(Value::string(*string));
        std::optional<JsString*> const that = interp.to_string(argument(args, 0));
        if (!that)
            return std::nullopt;
        interp.root(Value::string(*that));
        Value const collator_args[] = { argument(args, 1), argument(args, 2) };
        std::optional<Value> const collator = collator_construct(interp, collator_args,
            interp.intrinsics().intl_constructors[static_cast<std::size_t>(IntlKind::Collator)]);
        if (!collator)
            return std::nullopt;
        auto* object = intl_cast<CollatorData>(*collator);
        return Value::number(compare_strings(object->data, (*string)->view(), (*that)->view()));
    });
    define_method(in, string_prototype, "toLocaleLowerCase", 0, [](Interpreter& interp, Value const& this_value, Args args) {
        return transform_case(interp, this_value, argument(args, 0), false, "toLocaleLowerCase");
    });
    define_method(in, string_prototype, "toLocaleUpperCase", 0, [](Interpreter& interp, Value const& this_value, Args args) {
        return transform_case(interp, this_value, argument(args, 0), true, "toLocaleUpperCase");
    });
}

}
