#pragma once

// ECMA-402, the Internationalization API: what the three Intl runtime
// files share. The objects every Intl constructor makes (one cell class,
// a kind and the kind's record), the locale machinery of section 6 and section 9
// (language tags, their canonical form, CanonicalizeLocaleList,
// ResolveLocale, SupportedLocales), the option readers of section 9.2, and the
// number formatting core that PluralRules, RelativeTimeFormat and the
// date formats' fractional seconds are written in terms of.
//
// The engine carries English only, as a small-ICU build does: en, en-US
// and en-GB are the available locales, en-US is the default, and every
// other request negotiates to the default. The data is IntlData.h.

#include "js/Interpreter.h"
#include "js/Object.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

enum class IntlKind : std::uint8_t {
    Collator,
    DateTimeFormat,
    DisplayNames,
    ListFormat,
    Locale,
    NumberFormat,
    PluralRules,
    RelativeTimeFormat,
    Segmenter,
    Segments, // %IntlSegmentsPrototype%'s instances
    SegmentIterator, // %IntlSegmentIteratorPrototype%'s instances
    DurationFormat,
};
inline constexpr std::size_t intl_kind_count = 12;
static_assert(sizeof(Intrinsics::intl_prototypes) / sizeof(Object*) == intl_kind_count);

// The constructor's name for a kind ("Collator"), and the name a message
// or Object.prototype.toString gives an instance.
std::string_view intl_kind_name(IntlKind);

// Every object an Intl constructor makes: the kind, and two traced slots
// for the cells a kind keeps (a bound format function, a segmenter's
// string). The kind's own record is in IntlObjectOf below.
class IntlObject : public Object {
public:
    IntlObject(Object* prototype, IntlKind kind)
        : Object(prototype, Class::Intl)
        , m_kind(kind)
    {
    }
    IntlKind kind() const { return m_kind; }
    Value const& slot(std::size_t index) const { return m_slots[index]; }
    void set_slot(std::size_t index, Value const& value) { m_slots[index] = value; }
    void trace(Tracer&) override;

private:
    IntlKind m_kind;
    Value m_slots[2];
};

template<typename Data>
class IntlObjectOf final : public IntlObject {
public:
    explicit IntlObjectOf(Object* prototype)
        : IntlObject(prototype, Data::kind)
    {
    }
    Data data;
    std::size_t size_in_bytes() const override { return IntlObject::size_in_bytes() + sizeof(Data); }
};

// The record behind `value` when it is an Intl object of Data's kind, else
// null.
template<typename Data>
IntlObjectOf<Data>* intl_cast(Value const& value)
{
    if (!value.is_object() || value.as_object()->class_id() != Object::Class::Intl)
        return nullptr;
    auto* object = static_cast<IntlObject*>(value.as_object());
    if (object->kind() != Data::kind)
        return nullptr;
    return static_cast<IntlObjectOf<Data>*>(object);
}

// The record behind `this`, or V8's TypeError naming the method.
template<typename Data>
std::optional<IntlObjectOf<Data>*> intl_this(Interpreter& in, Value const& this_value, std::string_view method)
{
    if (IntlObjectOf<Data>* object = intl_cast<Data>(this_value))
        return object;
    return in.throw_type_error("Method Intl." + std::string(intl_kind_name(Data::kind)) + ".prototype." + std::string(method)
        + " called on incompatible receiver " + in.describe(this_value));
}

// ------------------------------------------------------------ language tags

// A unicode_locale_id (UTS #35 section 3.2) taken apart. Subtags keep the case
// they were written in until canonicalize() runs.
struct LanguageTag {
    std::string language;
    std::string script;
    std::string region;
    std::vector<std::string> variants;
    // -u-: attributes, then keywords (a key and its type, "" = true).
    std::vector<std::string> unicode_attributes;
    std::vector<std::pair<std::string, std::string>> unicode_keywords;
    bool has_unicode_extension = false;
    // -t-: an optional tlang, then fields.
    std::string transformed_language; // the tlang, canonical text, or ""
    std::vector<std::pair<std::string, std::string>> transformed_fields;
    bool has_transformed_extension = false;
    // Any other singleton, with its text ("a-foo-bar" without the singleton).
    std::vector<std::pair<char, std::string>> other_extensions;
    std::string private_use; // the subtags after -x-, or ""

    // language[-script][-region][-variant]*
    std::string base_name() const;
    // The whole tag, extensions in singleton order.
    std::string to_string() const;
    // The keyword's type when the -u- extension carries `key`.
    std::optional<std::string> keyword(std::string_view key) const;
    void set_keyword(std::string const& key, std::string const& type); // replaces, or inserts in order
    void remove_keyword(std::string_view key);
};

// IsStructurallyValidLanguageTag (section 6.2.1): the parse, or nothing.
std::optional<LanguageTag> parse_language_tag(std::string_view);
// CanonicalizeUnicodeLocaleId (section 6.2.2): case, order, aliases.
void canonicalize(LanguageTag&);
// Both, from text: the canonical tag, or nothing when the text is not one.
std::optional<std::string> canonicalize_language_tag(std::string_view);
// The grammar pieces the Locale constructor's options are checked against.
bool is_unicode_language_subtag(std::string_view);
bool is_unicode_script_subtag(std::string_view);
bool is_unicode_region_subtag(std::string_view);
bool is_unicode_variant_subtag(std::string_view);
bool is_unicode_type_sequence(std::string_view); // (3*8alphanum) *("-" (3*8alphanum))
// UTS #35's Add Likely Subtags and Remove Likely Subtags over the engine's
// small table; the extensions are left as they were.
void add_likely_subtags(LanguageTag&);
void remove_likely_subtags(LanguageTag&);

// ------------------------------------------------------ locale negotiation

std::string const& default_locale(); // "en-US"
// The locales the engine has data for (en, en-GB, en-US), for a base name.
bool is_available_locale(std::string_view base_name);

// CanonicalizeLocaleList (section 9.2.1): nullopt = a throw.
std::optional<std::vector<std::string>> canonicalize_locale_list(Interpreter&, Value const& locales);

// One relevant extension key of ResolveLocale (section 9.2.7): the values the
// engine knows for it (the first is the default), and the value an option
// asked for, which overrides the tag's keyword when it is one of them.
struct RelevantKey {
    std::string key;
    std::vector<std::string> values;
    std::optional<std::string> option;
};
struct ResolvedLocale {
    std::string locale; // the negotiated tag with the keywords that were honoured
    std::string data_locale; // its base name: "en", "en-US" or "en-GB"
    std::vector<std::pair<std::string, std::string>> values; // key -> the resolved value
    std::string const& value(std::string_view key) const;
};
ResolvedLocale resolve_locale(std::vector<std::string> const& requested, std::vector<RelevantKey> const& keys);
// SupportedLocales (section 9.2.10): the localeMatcher option read, then the
// requested locales the engine has, as an array. nullopt = a throw.
std::optional<Value> supported_locales(Interpreter&, std::vector<std::string> const& requested, Value const& options);

// --------------------------------------------------------------- options

// CoerceOptionsToObject (section 9.2.12): undefined -> null (read as "no
// options"), anything else ToObject. GetOptionsObject (section 9.2.13): undefined
// -> null, an object -> itself, anything else a TypeError.
std::optional<Object*> coerce_options_to_object(Interpreter&, Value const&);
std::optional<Object*> get_options_object(Interpreter&, Value const&);
// GetOption (section 9.2.12) of type string: absent = the inner nullopt; a value
// outside `values` (when given) is a RangeError naming the property.
std::optional<std::optional<std::string>> get_string_option(Interpreter&, Object* options, std::string_view property,
    std::initializer_list<std::string_view> values = {});
std::optional<std::optional<bool>> get_boolean_option(Interpreter&, Object* options, std::string_view property);
// DefaultNumberOption / GetNumberOption (section 9.2.15, section 9.2.16): an integer in
// [minimum, maximum], a RangeError outside it, absent = the inner nullopt.
std::optional<std::optional<int>> default_number_option(Interpreter&, Value const&, int minimum, int maximum,
    std::string_view property);
std::optional<std::optional<int>> get_number_option(Interpreter&, Object* options, std::string_view property,
    int minimum, int maximum);
// The whole localeMatcher step every constructor shares.
std::optional<bool> read_locale_matcher(Interpreter&, Object* options);

// ---------------------------------------------------------- results

// A string value from UTF-8.
Value intl_string(Interpreter&, std::string_view utf8);
Value intl_string(Interpreter&, std::u16string_view);
// One element of a formatToParts result.
struct IntlPart {
    std::string type;
    std::u16string value;
    std::string extra_name; // a third property ("unit", "source") or ""
    std::string extra_value;
};
// The parts as an array of { type, value[, extra] } objects.
Value parts_to_array(Interpreter&, std::vector<IntlPart> const&);
std::u16string join_parts(std::vector<IntlPart> const&);
// A constructor and its prototype in the current realm, for a kind.
Object* intl_prototype(Interpreter&, IntlKind);
// Makes Intl.<kind>: the constructor (length as given) and its prototype,
// registered in the intrinsics, the prototype's @@toStringTag
// "Intl.<kind>", and the constructor's supportedLocalesOf. A null `call`
// makes a constructor that throws without new.
NativeFunction* define_intl_constructor(Interpreter&, Object& intl, IntlKind, int length, NativeFunction::Callback call,
    NativeFunction::ConstructCallback construct);
// OrdinaryCreateFromConstructor for a kind: the object, its prototype read
// from new.target (the kind's intrinsic when that is not an object).
template<typename Data>
std::optional<IntlObjectOf<Data>*> allocate_intl(Interpreter& in, Object* new_target)
{
    std::optional<Object*> const prototype = in.get_prototype_from_constructor(new_target,
        [](Intrinsics const& i) { return i.intl_prototypes[static_cast<std::size_t>(Data::kind)]; });
    if (!prototype)
        return std::nullopt;
    return in.heap().allocate<IntlObjectOf<Data>>(*prototype);
}
// An array of strings.
Value intl_string_array(Interpreter&, std::vector<std::string> const&);
// The numbering systems the engine writes digits in ("latn" first, the
// default), and ASCII digits rewritten in one of them.
std::vector<std::string> const& numbering_system_names();
std::u16string transliterate_digits(std::string_view numbering_system, std::u16string_view text);
// The decimal and grouping separators written with a numbering system:
// the Arabic ones with the Arabic-Indic digits, "." and "," elsewhere.
std::u16string_view decimal_separator(std::string_view numbering_system);
std::u16string_view group_separator(std::string_view numbering_system);

// ------------------------------------------------ the number formatting core

// An exact decimal: the value is (-1)^negative * digits * 10^exponent,
// digits without leading zeros ("" is zero). NaN and the infinities are
// flagged, not spelled.
struct Decimal {
    enum class Kind : std::uint8_t { Finite, NaN, Infinity };
    Kind kind = Kind::Finite;
    bool negative = false;
    std::string digits;
    int exponent = 0;

    bool is_zero() const { return kind == Kind::Finite && digits.empty(); }
    // floor(log10(|x|)); only for a non-zero finite value.
    int magnitude() const { return static_cast<int>(digits.size()) - 1 + exponent; }
    double to_double() const;
    int compare(Decimal const&) const; // -1, 0, 1 (NaN never reaches here)
};
Decimal decimal_from_double(double);
// ToIntlMathematicalValue (section 15.5.16): a Number exactly, a BigInt exactly,
// a String by the StringNumericLiteral grammar with its digits kept.
std::optional<Decimal> to_intl_mathematical_value(Interpreter&, Value const&);

enum class RoundingType : std::uint8_t { FractionDigits, SignificantDigits, MorePrecision, LessPrecision };

struct DigitOptions {
    int minimum_integer_digits = 1;
    int minimum_fraction_digits = 0;
    int maximum_fraction_digits = 3;
    int minimum_significant_digits = 1;
    int maximum_significant_digits = 21;
    RoundingType rounding_type = RoundingType::FractionDigits;
    std::string computed_rounding_priority = "auto";
    std::string rounding_mode = "halfExpand";
    int rounding_increment = 1;
    std::string trailing_zero_display = "auto";
};
// SetNumberFormatDigitOptions (section 15.1.3). nullopt = a throw.
std::optional<bool> set_number_format_digit_options(Interpreter&, DigitOptions&, Object* options, int mnfd_default,
    int mxfd_default, std::string_view notation);
// FormatNumericToString (section 15.5.3) on a non-negative value: the rounded
// value and its digits ("1,234" is not made here: "1234.50").
struct RawNumber {
    Decimal rounded;
    std::string text;
};
RawNumber format_numeric_to_string(DigitOptions const&, Decimal const& x);
// The value's operands per UTS #35 and the English rules: "one", "two",
// "few" or "other".
std::string plural_category(bool ordinal, Decimal const& value, std::string_view formatted);
// The resolvedOptions() fields the digit options put in: the digit counts
// (put_digit_counts) and the rounding fields after them.
void put_digit_counts(Interpreter&, Object& result, DigitOptions const&);
void put_rounding_options(Interpreter&, Object& result, DigitOptions const&);

struct NumberFormatData {
    static constexpr IntlKind kind = IntlKind::NumberFormat;
    std::string locale;
    std::string data_locale;
    std::string numbering_system = "latn";
    std::string style = "decimal";
    std::string currency; // upper case
    std::string currency_display = "symbol";
    std::string currency_sign = "standard";
    std::string unit;
    std::string unit_display = "short";
    std::string notation = "standard";
    std::string compact_display = "short";
    std::string use_grouping = "auto"; // "always", "auto", "min2" or "false"
    std::string sign_display = "auto";
    DigitOptions digits;
};
// InitializeNumberFormat (section 15.1.2) into a fresh object of the intrinsic
// prototype, for the built-ins that format a number (toLocaleString, a
// relative time).
std::optional<IntlObjectOf<NumberFormatData>*> create_number_format(Interpreter&, Value const& locales, Value const& options);
// PartitionNumberPattern (section 15.5.4).
std::vector<IntlPart> partition_number_pattern(NumberFormatData const&, Decimal x);

}
