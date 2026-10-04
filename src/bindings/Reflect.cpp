// The IDL attributes an element reflects from its content attributes
// (HTML §2.6.1), and the rules for reading numbers out of a content
// attribute that those reflections are built on (§2.4.4).
//
// Every helper here defines one accessor pair on a prototype. The pair is
// installed for every element interface by generated/HtmlElements.gen.cpp,
// which tools/gen-bindings.cpp writes from idl/html-elements.idl; the
// hand-written bindings install theirs afterwards, so an attribute that
// needs to read live state overrides the reflection of the same name.

#include "bindings/Internal.h"

#include "core/Ascii.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

constexpr double max_long = 2147483647.0;
constexpr double min_long = -2147483648.0;

bool is_html_space(char c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

// The element `this` names, or a TypeError already thrown.
std::optional<dom::Element*> element_of(js::Interpreter& interpreter, js::Value const& this_value)
{
    return this_element(interpreter, this_value);
}

// The value of a content attribute, or nothing when the element has none.
std::optional<std::string> attribute_value(dom::Element const& element, std::string const& name)
{
    dom::Attr const* attribute = element.find_attribute(name);
    if (!attribute)
        return std::nullopt;
    return attribute->value;
}

// An integer written the shortest way a valid integer can be, which for the
// values these setters hold is what JavaScript prints.
std::string shortest(double value)
{
    return js::number_to_utf8(value);
}

} // namespace

// --- HTML's number rules ------------------------------------------------------

std::optional<double> parse_html_integer(std::string_view input)
{
    std::size_t position = 0;
    double sign = 1;
    while (position < input.size() && is_html_space(input[position]))
        ++position;
    if (position >= input.size())
        return std::nullopt;
    if (input[position] == '-') {
        sign = -1;
        ++position;
    } else if (input[position] == '+') {
        ++position;
    }
    if (position >= input.size() || !is_digit(input[position]))
        return std::nullopt;
    double value = 0;
    while (position < input.size() && is_digit(input[position])) {
        value = value * 10 + (input[position] - '0');
        ++position;
    }
    // Spec arithmetic has no negative zero: the sign of nothing is nothing.
    if (value == 0)
        return 0.0;
    return sign * value;
}

std::optional<double> parse_html_non_negative_integer(std::string_view input)
{
    std::optional<double> const value = parse_html_integer(input);
    if (!value || *value < 0)
        return std::nullopt;
    return value;
}

std::optional<double> parse_html_double(std::string_view input)
{
    std::size_t position = 0;
    double value = 1;
    double divisor = 1;
    double exponent = 1;
    while (position < input.size() && is_html_space(input[position]))
        ++position;
    if (position >= input.size())
        return std::nullopt;
    if (input[position] == '-') {
        value = -1;
        divisor = -1;
        ++position;
    } else if (input[position] == '+') {
        ++position;
    }
    if (position >= input.size())
        return std::nullopt;
    if (input[position] == '.' && position + 1 < input.size() && is_digit(input[position + 1])) {
        value = 0;
    } else if (!is_digit(input[position])) {
        return std::nullopt;
    } else {
        double whole = 0;
        while (position < input.size() && is_digit(input[position])) {
            whole = whole * 10 + (input[position] - '0');
            ++position;
        }
        value *= whole;
    }
    if (position < input.size() && input[position] == '.') {
        ++position;
        while (position < input.size() && is_digit(input[position])) {
            divisor *= 10;
            value += (input[position] - '0') / divisor;
            ++position;
        }
    }
    if (position < input.size() && (input[position] == 'e' || input[position] == 'E')) {
        ++position;
        if (position < input.size()) {
            if (input[position] == '-') {
                exponent = -1;
                ++position;
            } else if (input[position] == '+') {
                ++position;
            }
            if (position < input.size() && is_digit(input[position])) {
                double magnitude = 0;
                while (position < input.size() && is_digit(input[position])) {
                    magnitude = magnitude * 10 + (input[position] - '0');
                    ++position;
                }
                exponent *= magnitude;
                value *= std::pow(10.0, exponent);
            }
        }
    }
    if (!std::isfinite(value))
        return std::nullopt;
    if (value == 0)
        return 0.0;
    return value;
}

// --- The reflections ----------------------------------------------------------
//
// One getter and one setter per kind of reflection serve every attribute of
// that kind: what differs between two of them — the content attribute's
// name, an enumeration's keywords, a number's rules — is a record said once
// by whoever defines the attribute and kept for the process, and the
// accessor called finds its own through the number its description carries.
// So a realm holds no closure and no copy of a name for any of them, and an
// accessor nobody reads is never made.

namespace {

struct Reflection {
    enum class Kind : std::uint8_t { String, Boolean, Url, Enum, Number };
    struct Keyword {
        std::string keyword; // lowercased
        std::string canonical;
        bool operator==(Keyword const&) const = default;
    };
    Kind kind = Kind::String;
    std::string attribute;
    bool null_to_empty = false; // String: [LegacyNullToEmptyString]
    bool document_url_if_empty = false; // Url
    std::vector<Keyword> keywords; // Enum
    std::optional<std::string> missing;
    std::optional<std::string> invalid;
    bool nullable = false;
    ReflectedNumber number = ReflectedNumber::Long; // Number
    double fallback = 0;
    double minimum = 0;
    double maximum = 0;
    bool operator==(Reflection const&) const = default;
};

struct Reflections {
    std::mutex lock;
    // Never freed and never moved: a description on any thread names one by
    // its place here.
    std::vector<Reflection const*> records;
    std::unordered_map<std::string, std::uint32_t> by_text;
};

Reflections& reflections()
{
    static auto* const all = new Reflections();
    return *all;
}

// The record's place, the same for the same record whichever realm, engine
// or thread asks. A thread's own table answers what it has met without the
// lock.
std::uint32_t reflection_id(Reflection record)
{
    std::string text;
    text += static_cast<char>('0' + static_cast<int>(record.kind));
    text += record.attribute;
    text += '\x1f';
    text += record.null_to_empty ? '1' : '0';
    text += record.document_url_if_empty ? '1' : '0';
    text += record.nullable ? '1' : '0';
    for (Reflection::Keyword const& keyword : record.keywords)
        text += keyword.keyword + '\x1e' + keyword.canonical + '\x1f';
    text += record.missing ? "m" + *record.missing : std::string("-");
    text += '\x1f';
    text += record.invalid ? "i" + *record.invalid : std::string("-");
    text += '\x1f';
    text += std::to_string(static_cast<int>(record.number)) + ',' + js::number_to_utf8(record.fallback) + ',' + js::number_to_utf8(record.minimum) + ','
        + js::number_to_utf8(record.maximum);
    thread_local std::unordered_map<std::string, std::uint32_t> seen;
    if (auto const found = seen.find(text); found != seen.end())
        return found->second;
    Reflections& all = reflections();
    std::uint32_t id = 0;
    {
        std::lock_guard<std::mutex> const held(all.lock);
        auto const found = all.by_text.find(text);
        if (found != all.by_text.end()) {
            id = found->second;
        } else {
            id = static_cast<std::uint32_t>(all.records.size());
            all.records.push_back(new Reflection(std::move(record)));
            all.by_text.emplace(text, id);
        }
    }
    seen.emplace(std::move(text), id);
    return id;
}

// The record of the reflected attribute whose accessor is running: read
// first thing, before the accessor calls anything.
Reflection const& reflection_called(js::Interpreter& interpreter)
{
    static Reflection const none;
    js::NativeFunction const* const accessor = interpreter.active_native();
    if (accessor == nullptr || accessor->spec() == nullptr)
        return none;
    std::uint32_t const id = accessor->spec()->datum;
    thread_local std::vector<Reflection const*> known;
    if (id >= known.size()) {
        Reflections& all = reflections();
        std::lock_guard<std::mutex> const held(all.lock);
        known = all.records;
    }
    return id < known.size() ? *known[id] : none;
}

void define_reflection(Realm::Internals& in, js::Object& prototype, std::string_view property, Reflection record,
    js::NativeFunction::Entry getter, js::NativeFunction::Entry setter)
{
    define_plain_attribute(in.interpreter, prototype, property, getter, setter, MemberKind::Plain, reflection_id(std::move(record)));
}

Native string_getter(js::Interpreter& interp, js::Value const& this_value, Args)
{
    Reflection const& reflection = reflection_called(interp);
    std::optional<dom::Element*> const element = element_of(interp, this_value);
    if (!element)
        return std::nullopt;
    return internals_of(interp).string(attribute_or_empty(**element, reflection.attribute));
}

Native string_setter(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    Reflection const& reflection = reflection_called(interp);
    std::optional<dom::Element*> const element = element_of(interp, this_value);
    if (!element)
        return std::nullopt;
    js::Value const& given = js::argument(args, 0);
    if (reflection.null_to_empty && given.is_null()) {
        set_attribute(internals_of(interp), **element, reflection.attribute, "");
        return js::Value::undefined();
    }
    std::optional<std::string> value = internals_of(interp).to_utf8(given);
    if (!value)
        return std::nullopt;
    set_attribute(internals_of(interp), **element, reflection.attribute, std::move(*value));
    return js::Value::undefined();
}

Native boolean_getter(js::Interpreter& interp, js::Value const& this_value, Args)
{
    Reflection const& reflection = reflection_called(interp);
    std::optional<dom::Element*> const element = element_of(interp, this_value);
    if (!element)
        return std::nullopt;
    return js::Value::boolean((*element)->has_attribute(reflection.attribute));
}

Native boolean_setter(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    Reflection const& reflection = reflection_called(interp);
    std::optional<dom::Element*> const element = element_of(interp, this_value);
    if (!element)
        return std::nullopt;
    if (js::Interpreter::to_boolean(js::argument(args, 0)))
        set_attribute(internals_of(interp), **element, reflection.attribute, "");
    else
        remove_attribute(internals_of(interp), **element, reflection.attribute);
    return js::Value::undefined();
}

Native url_getter(js::Interpreter& interp, js::Value const& this_value, Args)
{
    Reflection const& reflection = reflection_called(interp);
    std::optional<dom::Element*> const element = element_of(interp, this_value);
    if (!element)
        return std::nullopt;
    Realm::Internals& internals = internals_of(interp);
    std::optional<std::string> const value = attribute_value(**element, reflection.attribute);
    if (reflection.document_url_if_empty && (!value || value->empty()))
        return internals.string(internals.url.serialize());
    if (!value)
        return internals.string("");
    if (std::optional<net::Url> const resolved = net::parse_url(*value, &internals.base_url()))
        return internals.string(resolved->serialize());
    return internals.string(*value);
}

// A URL's setter and an enumeration's that is not nullable write what they
// are given, as a string's does.
Native enum_getter(js::Interpreter& interp, js::Value const& this_value, Args)
{
    Reflection const& reflection = reflection_called(interp);
    std::optional<dom::Element*> const element = element_of(interp, this_value);
    if (!element)
        return std::nullopt;
    Realm::Internals& internals = internals_of(interp);
    std::optional<std::string> const value = attribute_value(**element, reflection.attribute);
    std::optional<std::string> state = reflection.missing;
    if (value) {
        std::string const folded = ascii_lower(*value);
        state = reflection.invalid;
        for (Reflection::Keyword const& keyword : reflection.keywords) {
            if (keyword.keyword == folded) {
                state = keyword.canonical;
                break;
            }
        }
    }
    // A state with no keyword of its own: the empty string, or null
    // where the IDL attribute is nullable.
    if (!state)
        return reflection.nullable ? js::Value::null() : internals.string("");
    return internals.string(*state);
}

Native enum_setter(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    Reflection const& reflection = reflection_called(interp);
    std::optional<dom::Element*> const element = element_of(interp, this_value);
    if (!element)
        return std::nullopt;
    js::Value const& given = js::argument(args, 0);
    if (reflection.nullable && (given.is_null() || given.is_undefined())) {
        remove_attribute(internals_of(interp), **element, reflection.attribute);
        return js::Value::undefined();
    }
    std::optional<std::string> value = internals_of(interp).to_utf8(given);
    if (!value)
        return std::nullopt;
    set_attribute(internals_of(interp), **element, reflection.attribute, std::move(*value));
    return js::Value::undefined();
}

Native number_getter(js::Interpreter& interp, js::Value const& this_value, Args)
{
    Reflection const& reflection = reflection_called(interp);
    ReflectedNumber const kind = reflection.number;
    double const fallback = reflection.fallback;
    std::optional<dom::Element*> const element = element_of(interp, this_value);
    if (!element)
        return std::nullopt;
    std::optional<std::string> const text = attribute_value(**element, reflection.attribute);
    if (!text)
        return js::Value::number(fallback);
    switch (kind) {
    case ReflectedNumber::Long: {
        std::optional<double> const value = parse_html_integer(*text);
        if (!value || *value > max_long || *value < min_long)
            return js::Value::number(fallback);
        return js::Value::number(*value);
    }
    case ReflectedNumber::LimitedLong:
    case ReflectedNumber::UnsignedLong:
    case ReflectedNumber::LimitedUnsignedLong:
    case ReflectedNumber::FallbackUnsignedLong: {
        double const lowest = kind == ReflectedNumber::LimitedLong || kind == ReflectedNumber::UnsignedLong ? 0 : 1;
        std::optional<double> const value = parse_html_non_negative_integer(*text);
        if (!value || *value > max_long || *value < lowest)
            return js::Value::number(fallback);
        return js::Value::number(*value);
    }
    case ReflectedNumber::ClampedUnsignedLong: {
        std::optional<double> const value = parse_html_non_negative_integer(*text);
        if (!value)
            return js::Value::number(fallback);
        return js::Value::number(std::min(std::max(*value, reflection.minimum), reflection.maximum));
    }
    case ReflectedNumber::Double:
    case ReflectedNumber::LimitedDouble: {
        std::optional<double> const value = parse_html_double(*text);
        if (!value || (kind == ReflectedNumber::LimitedDouble && *value <= 0))
            return js::Value::number(fallback);
        return js::Value::number(*value);
    }
    }
    return js::Value::number(fallback);
}

Native number_setter(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    Reflection const& reflection = reflection_called(interp);
    ReflectedNumber const kind = reflection.number;
    double const fallback = reflection.fallback;
    std::optional<dom::Element*> const element = element_of(interp, this_value);
    if (!element)
        return std::nullopt;
    Realm::Internals& internals = internals_of(interp);
    js::Value const& given = js::argument(args, 0);
    std::string written;
    switch (kind) {
    case ReflectedNumber::Long:
    case ReflectedNumber::LimitedLong: {
        std::optional<std::int32_t> const value = interp.to_int32(given);
        if (!value)
            return std::nullopt;
        if (kind == ReflectedNumber::LimitedLong && *value < 0)
            return internals.throw_dom_exception("IndexSizeError", "The value is negative");
        written = shortest(*value);
        break;
    }
    case ReflectedNumber::UnsignedLong:
    case ReflectedNumber::LimitedUnsignedLong:
    case ReflectedNumber::FallbackUnsignedLong:
    case ReflectedNumber::ClampedUnsignedLong: {
        std::optional<std::uint32_t> const value = interp.to_uint32(given);
        if (!value)
            return std::nullopt;
        if (kind == ReflectedNumber::LimitedUnsignedLong && *value == 0)
            return internals.throw_dom_exception("IndexSizeError", "The value is zero");
        double const lowest = kind == ReflectedNumber::UnsignedLong || kind == ReflectedNumber::ClampedUnsignedLong ? 0 : 1;
        double const number = static_cast<double>(*value);
        written = shortest(number >= lowest && number <= max_long ? number : fallback);
        break;
    }
    case ReflectedNumber::Double:
    case ReflectedNumber::LimitedDouble: {
        std::optional<double> const value = interp.to_number(given);
        if (!value)
            return std::nullopt;
        if (!std::isfinite(*value))
            return interp.throw_type_error("The value is not a finite number");
        // Greater than zero or nothing happens: the attribute keeps
        // whatever it had.
        if (kind == ReflectedNumber::LimitedDouble && *value <= 0)
            return js::Value::undefined();
        written = shortest(*value);
        break;
    }
    }
    set_attribute(internals, **element, reflection.attribute, std::move(written));
    return js::Value::undefined();
}

} // namespace

void reflect_string(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute,
    bool null_to_empty)
{
    Reflection record;
    record.kind = Reflection::Kind::String;
    record.attribute = std::string(attribute);
    record.null_to_empty = null_to_empty;
    define_reflection(in, prototype, property, std::move(record), string_getter, string_setter);
}

void reflect_boolean(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute)
{
    Reflection record;
    record.kind = Reflection::Kind::Boolean;
    record.attribute = std::string(attribute);
    define_reflection(in, prototype, property, std::move(record), boolean_getter, boolean_setter);
}

void reflect_url(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute,
    bool document_url_if_empty)
{
    Reflection record;
    record.kind = Reflection::Kind::Url;
    record.attribute = std::string(attribute);
    record.document_url_if_empty = document_url_if_empty;
    // Written as given, like a string.
    define_reflection(in, prototype, property, std::move(record), url_getter, string_setter);
}

void reflect_enum(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute,
    ReflectedEnum states)
{
    Reflection record;
    record.kind = Reflection::Kind::Enum;
    record.attribute = std::string(attribute);
    record.keywords.reserve(states.keywords.size());
    for (ReflectedKeyword const& keyword : states.keywords) {
        record.keywords.push_back(Reflection::Keyword { ascii_lower(keyword.keyword),
            std::string(keyword.canonical.empty() ? keyword.keyword : keyword.canonical) });
    }
    record.missing = states.missing ? std::optional<std::string>(std::string(*states.missing)) : std::nullopt;
    record.invalid = states.invalid ? std::optional<std::string>(std::string(*states.invalid)) : std::nullopt;
    record.nullable = states.nullable;
    define_reflection(in, prototype, property, std::move(record), enum_getter, enum_setter);
}

void reflect_number(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute,
    ReflectedNumber kind, double fallback, double minimum, double maximum)
{
    Reflection record;
    record.kind = Reflection::Kind::Number;
    record.attribute = std::string(attribute);
    record.number = kind;
    record.fallback = fallback;
    record.minimum = minimum;
    record.maximum = maximum;
    define_reflection(in, prototype, property, std::move(record), number_getter, number_setter);
}

}
