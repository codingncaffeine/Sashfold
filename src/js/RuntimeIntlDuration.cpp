#include "js/Runtime.h"

// Intl.DurationFormat (ECMA-402 section 13 of the 2025 edition): a duration
// record -- years down to nanoseconds -- written unit by unit with the
// NumberFormat's unit patterns, the clock-like units (numeric, 2-digit)
// run together with colons, and the pieces joined as a unit list.

#include "js/BigInteger.h"
#include "js/Intl.h"
#include "js/IntlData.h"
#include "js/Object.h"
#include "js/Strings.h"

#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

namespace {

constexpr std::size_t unit_count = 10;
constexpr std::string_view unit_names[unit_count] = { "years", "months", "weeks", "days", "hours", "minutes", "seconds",
    "milliseconds", "microseconds", "nanoseconds" };

struct UnitOptions {
    std::string style;
    std::string display;
};

struct DurationFormatData {
    static constexpr IntlKind kind = IntlKind::DurationFormat;
    std::string locale;
    std::string numbering_system = "latn";
    std::string style = "short";
    std::array<UnitOptions, unit_count> units;
    std::optional<int> fractional_digits;
};

std::optional<Value> duration_format_construct(Interpreter& in, Args args, Object* new_target)
{
    // Intl.DurationFormat (section 13.1.1).
    Interpreter::Roots const roots(in);
    in.root(Value::object(new_target));
    std::optional<IntlObjectOf<DurationFormatData>*> const object = allocate_intl<DurationFormatData>(in, new_target);
    if (!object)
        return std::nullopt;
    in.root(Value::object(*object));
    DurationFormatData& df = (*object)->data;
    std::optional<std::vector<std::string>> const requested = canonicalize_locale_list(in, argument(args, 0));
    if (!requested)
        return std::nullopt;
    std::optional<Object*> const options = get_options_object(in, argument(args, 1));
    if (!options)
        return std::nullopt;
    if (!read_locale_matcher(in, *options))
        return std::nullopt;
    std::optional<std::optional<std::string>> const numbering = get_string_option(in, *options, "numberingSystem");
    if (!numbering)
        return std::nullopt;
    if (*numbering && !is_unicode_type_sequence(**numbering))
        return in.throw_range_error("Invalid numberingSystem : " + **numbering);
    ResolvedLocale const resolved = resolve_locale(*requested, { { "nu", numbering_system_names(), *numbering } });
    df.locale = resolved.locale;
    df.numbering_system = resolved.value("nu");
    std::optional<std::optional<std::string>> const style = get_string_option(in, *options, "style", { "long", "short", "narrow", "digital" });
    if (!style)
        return std::nullopt;
    df.style = style->value_or("short");

    // GetDurationUnitOptions (section 13.5.6) for each unit, in order.
    std::string previous_style;
    for (std::size_t i = 0; i < unit_count; ++i) {
        std::string const unit(unit_names[i]);
        bool const clock = i >= 4 && i <= 6; // hours, minutes, seconds
        bool const subsecond = i >= 7;
        std::optional<std::optional<std::string>> const requested_style = clock
            ? get_string_option(in, *options, unit, { "long", "short", "narrow", "numeric", "2-digit" })
            : subsecond ? get_string_option(in, *options, unit, { "long", "short", "narrow", "numeric" })
                        : get_string_option(in, *options, unit, { "long", "short", "narrow" });
        if (!requested_style)
            return std::nullopt;
        std::string unit_style;
        std::string display_default = "always";
        if (*requested_style) {
            unit_style = **requested_style;
        } else if (df.style == "digital") {
            unit_style = i < 4 ? "short" : "numeric";
            if (!clock)
                display_default = "auto";
        } else if (previous_style == "fractional" || previous_style == "numeric" || previous_style == "2-digit") {
            unit_style = "numeric";
            if (unit != "minutes" && unit != "seconds")
                display_default = "auto";
        } else {
            unit_style = df.style;
            display_default = "auto";
        }
        if (unit_style == "numeric" && subsecond) {
            unit_style = "fractional";
            display_default = "auto";
        }
        std::optional<std::optional<std::string>> const display = get_string_option(in, *options, unit + "Display", { "auto", "always" });
        if (!display)
            return std::nullopt;
        std::string const unit_display = display->value_or(display_default);
        // ValidateDurationUnitStyle (section 13.5.7).
        if (unit_display == "always" && unit_style == "fractional")
            return in.throw_range_error(unit + "Display cannot be \"always\" with a fractional " + unit);
        if (previous_style == "fractional" && unit_style != "fractional")
            return in.throw_range_error(unit + " must be numeric after a fractional unit");
        if ((previous_style == "numeric" || previous_style == "2-digit") && unit_style != "fractional" && unit_style != "numeric" && unit_style != "2-digit")
            return in.throw_range_error(unit + " must be numeric after a numeric unit");
        if ((unit == "minutes" || unit == "seconds") && (previous_style == "numeric" || previous_style == "2-digit"))
            unit_style = "2-digit";
        df.units[i] = { unit_style, unit_display };
        if (clock || subsecond)
            previous_style = unit_style;
    }
    std::optional<std::optional<int>> const fractional = get_number_option(in, *options, "fractionalDigits", 0, 9);
    if (!fractional)
        return std::nullopt;
    df.fractional_digits = *fractional;
    return Value::object(*object);
}

// A duration record (section 13.5.1): the ten fields as integral Numbers.
using Duration = std::array<double, unit_count>;

std::optional<Duration> to_duration_record(Interpreter& in, Value const& input)
{
    // ToDurationRecord (section 13.5.3).
    if (!input.is_object()) {
        if (input.is_string())
            return in.throw_range_error("Invalid duration string");
        return in.throw_type_error("Duration must be an object");
    }
    Object& object = *input.as_object();
    Duration duration {};
    // The fields are read in alphabetical order.
    static constexpr std::size_t order[unit_count] = { 3, 4, 8, 7, 5, 1, 9, 6, 2, 0 };
    bool any = false;
    for (std::size_t index : order) {
        std::optional<Value> const value = in.get(object, in.key(unit_names[index]));
        if (!value)
            return std::nullopt;
        if (value->is_undefined())
            continue;
        any = true;
        std::optional<double> const number = in.to_number(*value);
        if (!number)
            return std::nullopt;
        if (!std::isfinite(*number) || std::trunc(*number) != *number)
            return in.throw_range_error("Duration fields must be integers");
        duration[index] = *number + 0.0;
    }
    if (!any)
        return in.throw_type_error("Duration has no fields");
    // IsValidDuration (section 7.5.4 of Temporal, as ECMA-402 cites it).
    int sign = 0;
    for (double v : duration) {
        int const s = v < 0 ? -1 : v > 0 ? 1 : 0;
        if (s != 0 && sign != 0 && s != sign)
            return in.throw_range_error("Duration fields must all have the same sign");
        if (s != 0)
            sign = s;
    }
    double const limit32 = 4294967296.0;
    for (std::size_t i = 0; i < 3; ++i)
        if (std::fabs(duration[i]) >= limit32)
            return in.throw_range_error("Duration years, months and weeks must be below 2^32");
    // The days and the time as nanoseconds must stay below 2^53 seconds.
    auto big = [](double v) { return *BigInteger::from_double(v); };
    BigInteger total = big(duration[3]) * BigInteger::from_int64(86400);
    total = total + big(duration[4]) * BigInteger::from_int64(3600);
    total = total + big(duration[5]) * BigInteger::from_int64(60);
    total = total + big(duration[6]);
    total = total * BigInteger::from_int64(1000000000);
    total = total + big(duration[7]) * BigInteger::from_int64(1000000);
    total = total + big(duration[8]) * BigInteger::from_int64(1000);
    total = total + big(duration[9]);
    BigInteger const bound = BigInteger::from_int64(9007199254740992LL) * BigInteger::from_int64(1000000000);
    BigInteger const magnitude = total.is_negative() ? total.negated() : total;
    if (compare(magnitude, bound) >= 0)
        return in.throw_range_error("Duration is out of range");
    return duration;
}

int duration_sign(Duration const& duration)
{
    for (double v : duration) {
        if (v < 0)
            return -1;
        if (v > 0)
            return 1;
    }
    return 0;
}

// A unit's value with the smaller units after it as a fraction, exactly:
// seconds with the milli-, micro- and nanoseconds (exponent 9), and so on.
Decimal fractional_value(Duration const& d, std::size_t unit)
{
    BigInteger total;
    int exponent = 0;
    auto big = [](double v) { return *BigInteger::from_double(v); };
    if (unit == 6) {
        total = big(d[6]) * BigInteger::from_int64(1000000000) + big(d[7]) * BigInteger::from_int64(1000000)
            + big(d[8]) * BigInteger::from_int64(1000) + big(d[9]);
        exponent = -9;
    } else if (unit == 7) {
        total = big(d[7]) * BigInteger::from_int64(1000000) + big(d[8]) * BigInteger::from_int64(1000) + big(d[9]);
        exponent = -6;
    } else {
        total = big(d[8]) * BigInteger::from_int64(1000) + big(d[9]);
        exponent = -3;
    }
    Decimal x;
    x.negative = total.is_negative();
    std::string digits = (total.is_negative() ? total.negated() : total).to_string();
    if (digits == "0")
        digits.clear();
    while (!digits.empty() && digits.back() == '0') {
        digits.pop_back();
        ++exponent;
    }
    x.digits = digits;
    x.exponent = digits.empty() ? 0 : exponent;
    return x;
}

// PartitionDurationFormatPattern (section 13.5.9): groups of parts, each group one
// element of the list, the numeric run of units one group.
std::vector<IntlPart> partition_duration(DurationFormatData const& df, Duration const& duration)
{
    std::vector<std::vector<IntlPart>> groups;
    bool need_separator = false;
    bool display_negative_sign = true;
    for (std::size_t i = 0; i < unit_count; ++i) {
        Decimal value = decimal_from_double(duration[i]);
        std::string const& style = df.units[i].style;
        std::string const& display = df.units[i].display;
        std::string_view const plural = unit_names[i];
        std::string const unit(plural.substr(0, plural.size() - 1));
        NumberFormatData nf;
        nf.locale = df.locale;
        nf.numbering_system = df.numbering_system;
        bool done = false;
        if ((i == 6 || i == 7 || i == 8) && df.units[i + 1].style == "fractional") {
            value = fractional_value(duration, i);
            nf.digits.maximum_fraction_digits = df.fractional_digits.value_or(9);
            nf.digits.minimum_fraction_digits = df.fractional_digits.value_or(0);
            nf.digits.rounding_mode = "trunc";
            done = true;
        }
        bool display_required = false;
        if (i == 5 && need_separator) {
            display_required = df.units[6].display == "always" || duration[6] != 0 || duration[7] != 0 || duration[8] != 0
                || duration[9] != 0;
        }
        if (!value.is_zero() || display != "auto" || display_required) {
            if (display_negative_sign) {
                display_negative_sign = false;
                if (value.is_zero() && duration_sign(duration) < 0)
                    value.negative = true;
            } else {
                nf.sign_display = "never";
            }
            bool const numeric = style == "numeric" || style == "2-digit";
            if (style == "2-digit")
                nf.digits.minimum_integer_digits = 2;
            if (!numeric) {
                nf.style = "unit";
                nf.unit = unit;
                nf.unit_display = style;
            } else {
                nf.use_grouping = "false";
            }
            std::vector<IntlPart>* list = nullptr;
            if (!need_separator) {
                groups.emplace_back();
                list = &groups.back();
            } else {
                list = &groups.back();
                list->push_back({ "literal", u":", {}, {} });
            }
            for (IntlPart part : partition_number_pattern(nf, value)) {
                part.extra_name = "unit";
                part.extra_value = unit;
                list->push_back(std::move(part));
            }
            if (!need_separator && numeric)
                need_separator = true;
        }
        if (done)
            break;
    }
    // The groups as a unit list (the digital style lists as short).
    std::size_t const width = df.style == "long" ? 0 : df.style == "narrow" ? 2 : 1;
    intl_data::ListPattern const pattern = intl_data::list_patterns[2][width];
    std::vector<IntlPart> result;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        if (g > 0) {
            std::string_view joiner = pattern.middle;
            if (groups.size() == 2)
                joiner = pattern.pair;
            else if (g == groups.size() - 1)
                joiner = pattern.end;
            result.push_back({ "literal", utf16_from_utf8(joiner), {}, {} });
        }
        result.insert(result.end(), groups[g].begin(), groups[g].end());
    }
    return result;
}

std::optional<std::vector<IntlPart>> format_duration(Interpreter& in, Value const& this_value, Value const& input, std::string_view method)
{
    std::optional<IntlObjectOf<DurationFormatData>*> const df = intl_this<DurationFormatData>(in, this_value, method);
    if (!df)
        return std::nullopt;
    std::optional<Duration> const duration = to_duration_record(in, input);
    if (!duration)
        return std::nullopt;
    return partition_duration((*df)->data, *duration);
}

} // namespace

void install_intl_duration(Interpreter& in, Object& intl)
{
    define_intl_constructor(in, intl, IntlKind::DurationFormat, 0, {}, duration_format_construct);
    Object& prototype = *intl_prototype(in, IntlKind::DurationFormat);
    Heap::NoCollect const guard(in.heap());
    define_method(in, prototype, "format", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<std::vector<IntlPart>> const parts = format_duration(interp, this_value, argument(args, 0), "format");
        if (!parts)
            return std::nullopt;
        return intl_string(interp, join_parts(*parts));
    });
    define_method(in, prototype, "formatToParts", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<std::vector<IntlPart>> const parts = format_duration(interp, this_value, argument(args, 0), "formatToParts");
        if (!parts)
            return std::nullopt;
        return parts_to_array(interp, *parts);
    });
    define_method(in, prototype, "resolvedOptions", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<DurationFormatData>*> const found = intl_this<DurationFormatData>(interp, this_value, "resolvedOptions");
        if (!found)
            return std::nullopt;
        DurationFormatData const& df = (*found)->data;
        Heap::NoCollect const no_collect(interp.heap());
        Object* result = interp.new_object();
        result->put(interp.key("locale"), intl_string(interp, df.locale));
        result->put(interp.key("numberingSystem"), intl_string(interp, df.numbering_system));
        result->put(interp.key("style"), intl_string(interp, df.style));
        for (std::size_t i = 0; i < unit_count; ++i) {
            std::string const unit(unit_names[i]);
            // A fractional unit reads as numeric (section 13.4.5 step 5).
            std::string const style = df.units[i].style == "fractional" ? std::string("numeric") : df.units[i].style;
            result->put(interp.key(unit), intl_string(interp, style));
            result->put(interp.key(unit + "Display"), intl_string(interp, df.units[i].display));
        }
        if (df.fractional_digits)
            result->put(interp.key("fractionalDigits"), Value::number(*df.fractional_digits));
        return Value::object(result);
    });
}

}
