#include "dom/Forms.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace sashfold::dom::forms {

namespace {

std::string lowercased(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return out;
}

}

std::string attribute_lower(dom::Element const& element, std::string_view name)
{
    dom::Attr const* attribute = element.find_attribute(name);
    return attribute ? lowercased(attribute->value) : std::string();
}

// An <input>'s type keyword: "text" when it is missing or unknown.
std::string input_type(dom::Element const& element)
{
    static constexpr std::string_view known[] = { "hidden", "text", "search", "tel", "url", "email", "password",
        "date", "month", "week", "time", "datetime-local", "number", "range", "color", "checkbox", "radio", "file",
        "submit", "image", "reset", "button" };
    std::string const type = attribute_lower(element, "type");
    for (std::string_view const keyword : known) {
        if (type == keyword)
            return type;
    }
    return "text";
}

// The types whose field takes text, and so readonly (§4.10.5.3.3).
bool takes_readonly(std::string const& type)
{
    return type == "text" || type == "search" || type == "url" || type == "tel" || type == "email" || type == "password"
        || type == "date" || type == "month" || type == "week" || type == "time" || type == "datetime-local" || type == "number";
}

bool takes_placeholder(std::string const& type)
{
    return type == "text" || type == "search" || type == "url" || type == "tel" || type == "email" || type == "password"
        || type == "number";
}

bool takes_required(std::string const& type)
{
    return type != "hidden" && type != "range" && type != "color" && type != "submit" && type != "image" && type != "reset"
        && type != "button";
}

dom::Element const* element_parent(dom::Element const& element)
{
    dom::Node const* parent = element.parent();
    return parent && parent->is_element() ? static_cast<dom::Element const*>(parent) : nullptr;
}

// A fieldset that is disabled disables what it holds, but what is in its
// first legend (§4.10.15).
bool disabled_by_fieldset(dom::Element const& element)
{
    dom::Node const* child = &element;
    for (dom::Element const* ancestor = element_parent(element); ancestor; child = ancestor, ancestor = element_parent(*ancestor)) {
        if (!ancestor->is_html("fieldset") || !ancestor->has_attribute("disabled"))
            continue;
        dom::Element const* first_legend = nullptr;
        for (dom::Node const* node : ancestor->children()) {
            if (node->is_element() && static_cast<dom::Element const*>(node)->is_html("legend")) {
                first_legend = static_cast<dom::Element const*>(node);
                break;
            }
        }
        if (child != first_legend)
            return true;
    }
    return false;
}

bool can_be_disabled(dom::Element const& element)
{
    return element.is_html("button") || element.is_html("input") || element.is_html("select") || element.is_html("textarea")
        || element.is_html("optgroup") || element.is_html("option") || element.is_html("fieldset");
}

bool actually_disabled(dom::Element const& element)
{
    if (element.is_html("optgroup"))
        return element.has_attribute("disabled");
    if (element.is_html("option")) {
        dom::Element const* parent = element_parent(element);
        return element.has_attribute("disabled") || (parent && parent->is_html("optgroup") && parent->has_attribute("disabled"));
    }
    return element.has_attribute("disabled") || disabled_by_fieldset(element);
}

// The select an option belongs to, through an optgroup.
dom::Element const* select_of(dom::Element const& option)
{
    dom::Element const* parent = element_parent(option);
    if (parent && parent->is_html("optgroup"))
        parent = element_parent(*parent);
    return parent && parent->is_html("select") ? parent : nullptr;
}

template<typename Each> void each_option(dom::Element const& select, Each const& each)
{
    for (dom::Node const* node : select.children()) {
        if (!node->is_element())
            continue;
        auto const& child = static_cast<dom::Element const&>(*node);
        if (child.is_html("option")) {
            if (!each(child))
                return;
        } else if (child.is_html("optgroup")) {
            for (dom::Node const* inner : child.children()) {
                if (inner->is_element() && static_cast<dom::Element const*>(inner)->is_html("option")
                    && !each(static_cast<dom::Element const&>(*inner)))
                    return;
            }
        }
    }
}

// An option's selectedness: its selected attribute, or — in a select that
// shows one option and has none selected — being the first that is not
// disabled (§4.10.7's selectedness setting algorithm).
bool option_selected(dom::Element const& option)
{
    if (option.has_attribute("selected"))
        return true;
    dom::Element const* select = select_of(option);
    if (!select || select->has_attribute("multiple"))
        return false;
    bool any_selected = false;
    dom::Element const* first = nullptr;
    each_option(*select, [&](dom::Element const& candidate) {
        if (candidate.has_attribute("selected"))
            any_selected = true;
        if (!first && !actually_disabled(candidate))
            first = &candidate;
        return true;
    });
    return !any_selected && first == &option;
}

bool checkedness(dom::Element const& element)
{
    if (element.is_html("option"))
        return option_selected(element);
    if (!element.is_html("input"))
        return false;
    std::string const type = input_type(element);
    if (type != "checkbox" && type != "radio")
        return false;
    if (element.document().live_checked) {
        if (std::optional<bool> const live = element.document().live_checked(element))
            return *live;
    }
    return element.has_attribute("checked");
}

std::string control_value(dom::Element const& element)
{
    if (element.document().live_value) {
        if (std::optional<std::string> live = element.document().live_value(element))
            return *live;
    }
    if (element.is_html("textarea")) {
        std::string text;
        for (dom::Node const* child : element.children()) {
            if (child->is_text())
                text += static_cast<dom::Text const*>(child)->data;
        }
        return text;
    }
    if (element.is_html("select")) {
        std::string value;
        each_option(element, [&](dom::Element const& option) {
            if (!option_selected(option))
                return true;
            dom::Attr const* attribute = option.find_attribute("value");
            if (attribute) {
                value = attribute->value;
            } else {
                for (dom::Node const* child : option.children()) {
                    if (child->is_text())
                        value += static_cast<dom::Text const*>(child)->data;
                }
            }
            return false;
        });
        return value;
    }
    dom::Attr const* attribute = element.find_attribute("value");
    return attribute ? attribute->value : std::string();
}

// The radios of one group: the same name, the same form (§4.10.5.1.18).
bool editing_host_editable(dom::Element const& element)
{
    for (dom::Element const* ancestor = &element; ancestor; ancestor = element_parent(*ancestor)) {
        dom::Attr const* attribute = ancestor->find_attribute("contenteditable");
        if (!attribute)
            continue;
        std::string const value = lowercased(attribute->value);
        if (value.empty() || value == "true" || value == "plaintext-only")
            return true;
        if (value == "false")
            return false;
    }
    return false;
}

bool read_write(dom::Element const& element)
{
    if (element.is_html("input"))
        return takes_readonly(input_type(element)) && !element.has_attribute("readonly") && !actually_disabled(element);
    if (element.is_html("textarea"))
        return !element.has_attribute("readonly") && !actually_disabled(element);
    return editing_host_editable(element);
}

// A valid floating-point number (§2.3.4.3): an optional minus, digits with
// an optional fraction (or a fraction alone), an optional exponent; no
// white space, no plus, no infinity. Its value must be finite.
bool parse_number(std::string_view text, double& out)
{
    std::size_t i = 0;
    if (i < text.size() && text[i] == '-')
        ++i;
    std::size_t digits_seen = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        ++i;
        ++digits_seen;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        std::size_t fraction = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
            ++fraction;
        }
        if (fraction == 0)
            return false;
        digits_seen += fraction;
    }
    if (digits_seen == 0)
        return false;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-'))
            ++i;
        std::size_t exponent = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
            ++exponent;
        }
        if (exponent == 0)
            return false;
    }
    if (i != text.size())
        return false;
    std::string const copy(text);
    out = std::strtod(copy.c_str(), nullptr);
    return std::isfinite(out);
}

// Whether a control falls outside its min and max (§4.10.5.3.7): number
// fields; a range field is held within them and never does.
std::optional<bool> out_of_range(dom::Element const& element)
{
    if (!element.is_html("input"))
        return std::nullopt;
    std::string const type = input_type(element);
    if (type != "number" && type != "range")
        return std::nullopt;
    dom::Attr const* min = element.find_attribute("min");
    dom::Attr const* max = element.find_attribute("max");
    if (!min && !max)
        return std::nullopt;
    if (type == "range")
        return false;
    double value = 0;
    if (!parse_number(control_value(element), value))
        return false;
    double limit = 0;
    if (min && parse_number(min->value, limit) && value < limit)
        return true;
    if (max && parse_number(max->value, limit) && value > limit)
        return true;
    return false;
}

// A control that takes part in constraint validation (§4.10.20.1): not
// disabled, not read-only, not a button that is no submit button.
bool validation_candidate(dom::Element const& element)
{
    if (element.is_html("input")) {
        std::string const type = input_type(element);
        if (type == "hidden" || type == "reset" || type == "button")
            return false;
        if (takes_readonly(type) && element.has_attribute("readonly"))
            return false;
        return !actually_disabled(element);
    }
    if (element.is_html("textarea"))
        return !element.has_attribute("readonly") && !actually_disabled(element);
    if (element.is_html("select"))
        return !actually_disabled(element);
    if (element.is_html("button"))
        return attribute_lower(element, "type") != "button" && attribute_lower(element, "type") != "reset" && !actually_disabled(element);
    // A form-associated custom element that has said its validity through
    // ElementInternals, unless it is disabled.
    if (element.document().internals_validity.contains(&element))
        return !element.has_attribute("disabled");
    return false;
}

bool plausible_email(std::string const& value)
{
    // §4.10.5.1.5's valid e-mail address: a local part of the permitted
    // characters, an @, and labels of letters, digits and hyphens, none
    // longer than 63 and none starting or ending with a hyphen.
    std::size_t const at = value.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= value.size())
        return false;
    constexpr std::string_view local_extra = ".!#$%&'*+/=?^_`{|}~-";
    for (std::size_t i = 0; i < at; ++i) {
        char const c = value[i];
        bool const alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && local_extra.find(c) == std::string_view::npos)
            return false;
    }
    std::string_view domain = std::string_view(value).substr(at + 1);
    while (!domain.empty()) {
        std::size_t const dot = domain.find('.');
        std::string_view const label = domain.substr(0, dot);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
            return false;
        for (char const c : label) {
            bool const alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            if (!alnum && c != '-')
                return false;
        }
        if (dot == std::string_view::npos)
            break;
        domain.remove_prefix(dot + 1);
        if (domain.empty())
            return false;
    }
    return true;
}

namespace {

std::string trimmed_spaces(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r' || text.front() == '\f'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r' || text.back() == '\f'))
        text.remove_suffix(1);
    return std::string(text);
}

// An absolute URL as far as a url input asks (§4.10.5.1.4): a scheme — a
// letter, then letters, digits, +, - or . — and a colon.
bool plausible_url(std::string const& value)
{
    std::size_t const colon = value.find(':');
    if (colon == std::string::npos || colon == 0)
        return false;
    for (std::size_t i = 0; i < colon; ++i) {
        char const c = value[i];
        bool const letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (i == 0 ? !letter : !(letter || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
            return false;
    }
    return value.find_first_of(" \t\n\r\f") == std::string::npos;
}

bool takes_pattern(std::string const& type)
{
    return type == "text" || type == "search" || type == "url" || type == "tel" || type == "email" || type == "password";
}

// --- The date and time microsyntaxes (§2.3.5) ---------------------------------

std::optional<int> digits(std::string_view text, std::size_t& at, std::size_t count, bool at_least = false)
{
    std::size_t const start = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9' && (at_least || at - start < count))
        ++at;
    if (at - start < count || (!at_least && at - start != count))
        return std::nullopt;
    if (at - start > 9)
        return std::nullopt;
    int value = 0;
    for (std::size_t i = start; i < at; ++i)
        value = value * 10 + (text[i] - '0');
    return value;
}

bool leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month)
{
    static constexpr int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return month == 2 && leap(year) ? 29 : days[month - 1];
}

// Days from 1970-01-01 to the date (proleptic Gregorian).
double days_from_civil(int year, int month, int day)
{
    int const y = month <= 2 ? year - 1 : year;
    int const era = (y >= 0 ? y : y - 399) / 400;
    int const yoe = y - era * 400;
    int const doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<double>(era) * 146097.0 + static_cast<double>(doe) - 719468.0;
}

// A year (four digits or more, above zero) and a month.
bool parse_year_month(std::string_view text, std::size_t& at, int& year, int& month)
{
    std::optional<int> const y = digits(text, at, 4, true);
    if (!y || *y <= 0 || at >= text.size() || text[at] != '-')
        return false;
    ++at;
    std::optional<int> const m = digits(text, at, 2);
    if (!m || *m < 1 || *m > 12)
        return false;
    year = *y;
    month = *m;
    return true;
}

std::optional<double> parse_date_at(std::string_view text, std::size_t& at)
{
    int year = 0;
    int month = 0;
    if (!parse_year_month(text, at, year, month) || at >= text.size() || text[at] != '-')
        return std::nullopt;
    ++at;
    std::optional<int> const day = digits(text, at, 2);
    if (!day || *day < 1 || *day > days_in_month(year, month))
        return std::nullopt;
    return days_from_civil(year, month, *day);
}

// Milliseconds since midnight: HH:MM, then :SS and a fraction if given.
std::optional<double> parse_time_at(std::string_view text, std::size_t& at)
{
    std::optional<int> const hour = digits(text, at, 2);
    if (!hour || *hour > 23 || at >= text.size() || text[at] != ':')
        return std::nullopt;
    ++at;
    std::optional<int> const minute = digits(text, at, 2);
    if (!minute || *minute > 59)
        return std::nullopt;
    double ms = (static_cast<double>(*hour) * 60 + *minute) * 60000;
    if (at < text.size() && text[at] == ':') {
        ++at;
        std::optional<int> const second = digits(text, at, 2);
        if (!second || *second > 59)
            return std::nullopt;
        ms += *second * 1000.0;
        if (at < text.size() && text[at] == '.') {
            ++at;
            std::size_t const start = at;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9')
                ++at;
            if (at == start || at - start > 3)
                return std::nullopt;
            double fraction = 0;
            double scale = 100;
            for (std::size_t i = start; i < at; ++i, scale /= 10)
                fraction += (text[i] - '0') * scale;
            ms += fraction;
        }
    }
    return ms;
}

// What a date-like input's value means as a number (§4.10.5.1.7–.11): days
// for a date, months for a month, milliseconds for a week, a time and a
// date-time; nothing for a string that is not a valid one of its kind.
std::optional<double> date_like_number(std::string const& type, std::string_view text)
{
    std::size_t at = 0;
    std::optional<double> result;
    if (type == "date") {
        if (std::optional<double> const days = parse_date_at(text, at))
            result = *days * 86400000.0;
    } else if (type == "month") {
        int year = 0;
        int month = 0;
        if (parse_year_month(text, at, year, month))
            result = (static_cast<double>(year) - 1970) * 12 + (month - 1);
    } else if (type == "week") {
        std::optional<int> const year = digits(text, at, 4, true);
        if (year && *year > 0 && at + 1 < text.size() && text[at] == '-' && text[at + 1] == 'W') {
            at += 2;
            std::optional<int> const week = digits(text, at, 2);
            // A year has 53 weeks when it starts on a Thursday, or on a
            // Wednesday in a leap year (ISO 8601).
            double const jan1 = days_from_civil(*year, 1, 1);
            int const weekday = static_cast<int>(std::fmod(std::fmod(jan1 + 3, 7) + 7, 7)); // 0 = Monday
            int const weeks = weekday == 3 || (weekday == 2 && leap(*year)) ? 53 : 52;
            if (week && *week >= 1 && *week <= weeks) {
                // The Monday of week 1: the week holding January 4th.
                double const jan4 = days_from_civil(*year, 1, 4);
                int const jan4_weekday = static_cast<int>(std::fmod(std::fmod(jan4 + 3, 7) + 7, 7));
                result = (jan4 - jan4_weekday + (*week - 1) * 7) * 86400000.0;
            }
        }
    } else if (type == "time") {
        result = parse_time_at(text, at);
    } else if (type == "datetime-local") {
        if (std::optional<double> const days = parse_date_at(text, at); days && at < text.size() && (text[at] == 'T' || text[at] == ' ')) {
            ++at;
            if (std::optional<double> const ms = parse_time_at(text, at))
                result = *days * 86400000.0 + *ms;
        }
    }
    if (!result || at != text.size())
        return std::nullopt;
    return result;
}

bool date_like(std::string const& type)
{
    return type == "date" || type == "month" || type == "week" || type == "time" || type == "datetime-local";
}

// The step scale factor and default step and base of each type (§4.10.5.4).
struct StepRules {
    double scale = 1;
    double default_step = 1;
    double default_base = 0;
};

StepRules step_rules(std::string const& type)
{
    if (type == "date")
        return { 86400000.0, 1, 0 };
    if (type == "month")
        return { 1, 1, 0 };
    if (type == "week")
        return { 604800000.0, 1, -259200000.0 };
    if (type == "time" || type == "datetime-local")
        return { 1000, 60, 0 };
    return { 1, 1, 0 };
}

// Whether a pattern compiles under the v flag (§4.10.5.3.6), as far as what
// v refuses that u takes: in a character class, an unescaped ( ) [ { } / or |,
// and the doubled punctuators reserved for set operations.
bool v_flag_acceptable(std::string_view pattern)
{
    bool in_class = false;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        char const c = pattern[i];
        if (c == '\\') {
            ++i;
            continue;
        }
        if (!in_class) {
            if (c == '[')
                in_class = true;
            continue;
        }
        if (c == ']') {
            in_class = false;
            continue;
        }
        if (std::string_view("()[{}/|").find(c) != std::string_view::npos)
            return false;
        if (i + 1 < pattern.size() && pattern[i + 1] == c && std::string_view("&!#$%*+,.:;<=>?@^`~").find(c) != std::string_view::npos)
            return false;
    }
    return true;
}

}

std::uint16_t Validity::bits() const
{
    bool const flags[10] = { value_missing, type_mismatch, pattern_mismatch, too_long, too_short, range_underflow, range_overflow,
        step_mismatch, bad_input, custom_error };
    std::uint16_t out = 0;
    for (int i = 0; i < 10; ++i)
        out = static_cast<std::uint16_t>(out | (flags[i] ? 1u << i : 0u));
    return out;
}

Validity Validity::from_bits(std::uint16_t bits)
{
    Validity v;
    bool* const flags[10] = { &v.value_missing, &v.type_mismatch, &v.pattern_mismatch, &v.too_long, &v.too_short, &v.range_underflow,
        &v.range_overflow, &v.step_mismatch, &v.bad_input, &v.custom_error };
    for (int i = 0; i < 10; ++i)
        *flags[i] = (bits & (1u << i)) != 0;
    return v;
}

namespace {

// Whether an element has constraints to report at all (the submittable
// elements, and a custom element that set its validity): a disabled one
// still reports its flags, it is only not a candidate (§4.10.20).
bool has_constraints(dom::Element const& element)
{
    return element.is_html("input") || element.is_html("select") || element.is_html("textarea") || element.is_html("button")
        || element.document().internals_validity.contains(&element);
}

}

Validity validity_state(dom::Element const& element)
{
    Validity v;
    if (!has_constraints(element))
        return v;
    dom::Document const& document = element.document();
    if (auto const it = document.internals_validity.find(&element); it != document.internals_validity.end())
        return Validity::from_bits(it->second.flags);
    if (auto const it = document.custom_validity.find(&element); it != document.custom_validity.end() && !it->second.empty())
        v.custom_error = true;
    bool const required = element.has_attribute("required");
    if (element.is_html("input")) {
        std::string const type = input_type(element);
        if (type == "checkbox") {
            v.value_missing = required && !checkedness(element);
            return v;
        }
        if (type == "radio") {
            bool group_required = false;
            bool group_checked = false;
            each_in_radio_group(element, [&](dom::Element const& radio) {
                group_required = group_required || radio.has_attribute("required");
                group_checked = group_checked || checkedness(radio);
            });
            v.value_missing = group_required && !group_checked;
            return v;
        }
        std::string value = control_value(element);
        // email and url values are sanitized: their surrounding white space
        // goes (and an email list's, around each address, below).
        if (type == "email" || type == "url")
            value = trimmed_spaces(value);
        // required applies to a mutable control alone: one disabled or
        // read-only is not missing its value (§4.10.5.3.4).
        bool const mutable_control = !takes_readonly(type) || (!actually_disabled(element) && !element.has_attribute("readonly"));
        // A date-like value that is not a valid one of its kind is no value
        // (the value sanitization algorithm).
        std::optional<double> date_value;
        if (date_like(type) && !value.empty()) {
            date_value = date_like_number(type, value);
            if (!date_value)
                value.clear();
        }
        // So is a number that is not a valid floating-point number (what a user
        // types is badInput; a value set is sanitized to nothing).
        if (type == "number" && !value.empty()) {
            double parsed = 0;
            if (!parse_number(value, parsed))
                value.clear();
        }
        if (required && mutable_control && takes_required(type) && value.empty())
            v.value_missing = true;
        if (value.empty())
            return v;
        bool const multiple = element.has_attribute("multiple");
        std::vector<std::string> values;
        if (type == "email" && multiple) {
            std::size_t start = 0;
            for (;;) {
                std::size_t const comma = value.find(',', start);
                values.push_back(trimmed_spaces(std::string_view(value).substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
        } else {
            values.push_back(value);
        }
        if (type == "email") {
            for (std::string const& one : values)
                v.type_mismatch = v.type_mismatch || !plausible_email(one);
        }
        if (type == "url")
            v.type_mismatch = !plausible_url(value);
        if (takes_pattern(type) && document.pattern_matches) {
            if (dom::Attr const* const pattern = element.find_attribute("pattern"); pattern && v_flag_acceptable(pattern->value)) {
                for (std::string const& one : values) {
                    std::optional<bool> const matched = document.pattern_matches(pattern->value, one);
                    v.pattern_mismatch = v.pattern_mismatch || (matched && !*matched);
                }
            }
        }
        std::optional<double> number;
        if (type == "number" || type == "range") {
            double parsed = 0;
            if (!parse_number(value, parsed))
                return v; // sanitized away above
            number = parsed;
        } else if (date_value) {
            number = date_value;
        }
        if (!number)
            return v;
        // min and max, each when it parses as its type does.
        auto const bound = [&](std::string_view name) -> std::optional<double> {
            dom::Attr const* const attribute = element.find_attribute(name);
            if (!attribute)
                return std::nullopt;
            if (date_value)
                return date_like_number(type, attribute->value);
            double parsed = 0;
            return parse_number(attribute->value, parsed) ? std::optional<double>(parsed) : std::nullopt;
        };
        std::optional<double> const min = bound("min");
        std::optional<double> const max = bound("max");
        if (type != "range") {
            if (type == "time" && min && max && *min > *max) {
                // A reversed range (§4.10.5.1.10): a time outside it is before
                // the start and after the end at once.
                bool const outside = *number > *max && *number < *min;
                v.range_underflow = outside;
                v.range_overflow = outside;
            } else {
                v.range_underflow = min && *number < *min;
                v.range_overflow = max && *number > *max;
            }
        }
        // The step (§4.10.5.4): the type's default when absent or invalid,
        // none for "any", counted from min, else the value attribute, else the
        // type's default base.
        StepRules const rules = step_rules(type);
        std::string const step_text = attribute_lower(element, "step");
        if (step_text != "any") {
            double step = rules.default_step;
            if (!step_text.empty()) {
                double written = 0;
                if (parse_number(step_text, written) && written > 0)
                    step = type == "month" ? std::max(1.0, std::round(written)) : written;
            }
            step *= rules.scale;
            double base = rules.default_base;
            if (min) {
                base = *min;
            } else if (dom::Attr const* const initial = element.find_attribute("value")) {
                if (date_value) {
                    if (std::optional<double> const parsed = date_like_number(type, initial->value))
                        base = *parsed;
                } else {
                    double parsed = 0;
                    if (parse_number(initial->value, parsed))
                        base = parsed;
                }
            }
            double const steps = (*number - base) / step;
            double const nearest = std::round(steps);
            v.step_mismatch = std::fabs(steps - nearest) * step > std::fabs(step) * 1e-12 * std::max(1.0, std::fabs(nearest));
        }
        return v;
    }
    if (element.is_html("textarea") || element.is_html("select")) {
        bool const mutable_control = !element.is_html("textarea") || (!actually_disabled(element) && !element.has_attribute("readonly"));
        v.value_missing = required && mutable_control && control_value(element).empty();
    }
    return v;
}

std::string validation_message(dom::Element const& element)
{
    Validity const v = validity_state(element);
    if (v.valid())
        return std::string();
    dom::Document const& document = element.document();
    if (auto const it = document.internals_validity.find(&element); it != document.internals_validity.end())
        return it->second.message;
    if (v.custom_error)
        return document.custom_validity.at(&element);
    std::string const type = element.is_html("input") ? input_type(element) : std::string();
    if (v.value_missing) {
        if (type == "checkbox")
            return "Please check this box if you want to proceed.";
        if (type == "radio")
            return "Please select one of these options.";
        if (type == "file")
            return "Please select a file.";
        if (element.is_html("select"))
            return "Please select an item in the list.";
        return "Please fill out this field.";
    }
    if (v.type_mismatch)
        return type == "email" ? "Please enter an email address." : "Please enter a URL.";
    if (v.pattern_mismatch) {
        dom::Attr const* const title = element.find_attribute("title");
        return title && !title->value.empty() ? "Please match the requested format: " + title->value + "." : "Please match the requested format.";
    }
    if (v.bad_input)
        return "Please enter a number.";
    if (v.range_underflow)
        return "Please select a value that is no less than " + element.find_attribute("min")->value + ".";
    if (v.range_overflow)
        return "Please select a value that is no more than " + element.find_attribute("max")->value + ".";
    if (v.step_mismatch)
        return "Please select a valid value.";
    return std::string();
}

namespace {

// A form or fieldset is invalid when a control in it is.
bool holds_invalid(dom::Element const& element)
{
    for (dom::Node const* child : element.children()) {
        if (!child->is_element())
            continue;
        auto const& control = static_cast<dom::Element const&>(*child);
        if (!validity_state(control).valid() || holds_invalid(control))
            return true;
    }
    return false;
}

}

std::optional<bool> validity(dom::Element const& element)
{
    if (element.is_html("form") || element.is_html("fieldset"))
        return !holds_invalid(element);
    if (!validation_candidate(element))
        return std::nullopt;
    return validity_state(element).valid();
}

}
