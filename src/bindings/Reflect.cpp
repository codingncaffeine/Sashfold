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

#include <cmath>
#include <string>
#include <utility>

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

void reflect_string(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute,
    bool null_to_empty)
{
    std::string const attribute_name(attribute);
    define_getter(in, prototype, property,
        [attribute_name](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Element*> const element = element_of(interp, this_value);
            if (!element)
                return std::nullopt;
            return internals_of(interp).string(attribute_or_empty(**element, attribute_name));
        },
        [attribute_name, null_to_empty](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Element*> const element = element_of(interp, this_value);
            if (!element)
                return std::nullopt;
            js::Value const& given = js::argument(args, 0);
            if (null_to_empty && given.is_null()) {
                set_attribute(internals_of(interp), **element, attribute_name, "");
                return js::Value::undefined();
            }
            std::optional<std::string> value = internals_of(interp).to_utf8(given);
            if (!value)
                return std::nullopt;
            set_attribute(internals_of(interp), **element, attribute_name, std::move(*value));
            return js::Value::undefined();
        });
}

void reflect_boolean(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute)
{
    std::string const attribute_name(attribute);
    define_getter(in, prototype, property,
        [attribute_name](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Element*> const element = element_of(interp, this_value);
            if (!element)
                return std::nullopt;
            return js::Value::boolean((*element)->has_attribute(attribute_name));
        },
        [attribute_name](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Element*> const element = element_of(interp, this_value);
            if (!element)
                return std::nullopt;
            if (js::Interpreter::to_boolean(js::argument(args, 0)))
                set_attribute(internals_of(interp), **element, attribute_name, "");
            else
                remove_attribute(internals_of(interp), **element, attribute_name);
            return js::Value::undefined();
        });
}

void reflect_url(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute,
    bool document_url_if_empty)
{
    std::string const attribute_name(attribute);
    define_getter(in, prototype, property,
        [attribute_name, document_url_if_empty](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Element*> const element = element_of(interp, this_value);
            if (!element)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            std::optional<std::string> const value = attribute_value(**element, attribute_name);
            if (document_url_if_empty && (!value || value->empty()))
                return internals.string(internals.url.serialize());
            if (!value)
                return internals.string("");
            if (std::optional<net::Url> const resolved = net::parse_url(*value, &internals.base_url()))
                return internals.string(resolved->serialize());
            return internals.string(*value);
        },
        [attribute_name](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Element*> const element = element_of(interp, this_value);
            if (!element)
                return std::nullopt;
            std::optional<std::string> value = internals_of(interp).to_utf8(js::argument(args, 0));
            if (!value)
                return std::nullopt;
            set_attribute(internals_of(interp), **element, attribute_name, std::move(*value));
            return js::Value::undefined();
        });
}

void reflect_enum(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute,
    ReflectedEnum states)
{
    std::string const attribute_name(attribute);
    // The keywords are copied as strings: the generated caller passes views
    // of its own literals, but a realm outlives the call that built it.
    struct Keyword {
        std::string keyword;
        std::string canonical;
    };
    std::vector<Keyword> keywords;
    keywords.reserve(states.keywords.size());
    for (ReflectedKeyword const& keyword : states.keywords) {
        keywords.push_back(Keyword { ascii_lower(keyword.keyword),
            std::string(keyword.canonical.empty() ? keyword.keyword : keyword.canonical) });
    }
    std::optional<std::string> const missing
        = states.missing ? std::optional<std::string>(std::string(*states.missing)) : std::nullopt;
    std::optional<std::string> const invalid
        = states.invalid ? std::optional<std::string>(std::string(*states.invalid)) : std::nullopt;
    bool const nullable = states.nullable;

    define_getter(in, prototype, property,
        [attribute_name, keywords, missing, invalid, nullable](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Element*> const element = element_of(interp, this_value);
            if (!element)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            std::optional<std::string> const value = attribute_value(**element, attribute_name);
            std::optional<std::string> state = missing;
            if (value) {
                std::string const folded = ascii_lower(*value);
                state = invalid;
                for (Keyword const& keyword : keywords) {
                    if (keyword.keyword == folded) {
                        state = keyword.canonical;
                        break;
                    }
                }
            }
            // A state with no keyword of its own: the empty string, or null
            // where the IDL attribute is nullable.
            if (!state)
                return nullable ? js::Value::null() : internals.string("");
            return internals.string(*state);
        },
        [attribute_name, nullable](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Element*> const element = element_of(interp, this_value);
            if (!element)
                return std::nullopt;
            js::Value const& given = js::argument(args, 0);
            if (nullable && (given.is_null() || given.is_undefined())) {
                remove_attribute(internals_of(interp), **element, attribute_name);
                return js::Value::undefined();
            }
            std::optional<std::string> value = internals_of(interp).to_utf8(given);
            if (!value)
                return std::nullopt;
            set_attribute(internals_of(interp), **element, attribute_name, std::move(*value));
            return js::Value::undefined();
        });
}

void reflect_number(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute,
    ReflectedNumber kind, double fallback, double minimum, double maximum)
{
    std::string const attribute_name(attribute);
    define_getter(in, prototype, property,
        [attribute_name, kind, fallback, minimum, maximum](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Element*> const element = element_of(interp, this_value);
            if (!element)
                return std::nullopt;
            std::optional<std::string> const text = attribute_value(**element, attribute_name);
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
                return js::Value::number(std::min(std::max(*value, minimum), maximum));
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
        },
        [attribute_name, kind, fallback](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
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
            set_attribute(internals, **element, attribute_name, std::move(written));
            return js::Value::undefined();
        });
}

}
