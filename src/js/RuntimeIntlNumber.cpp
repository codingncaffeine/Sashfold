#include "js/Runtime.h"

// Intl.NumberFormat (ECMA-402 section 15), Intl.PluralRules (section 16) and
// Intl.RelativeTimeFormat (section 17), and the locale-sensitive toLocaleString
// of Number and BigInt (section 20.1, section 20.2) that go through them.
//
// Every number is formatted from decimal digits: a Number's shortest
// round-trip digits (as V8 and SpiderMonkey, through ICU, format a double),
// a BigInt's integer exactly, a string's digits exactly as written.
// Rounding is decimal arithmetic on those digits.

#include "js/BigInteger.h"
#include "js/Intl.h"
#include "js/IntlData.h"
#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

// ---------------------------------------------------------------- Decimal

namespace {

void normalize(Decimal& d)
{
    std::size_t leading = 0;
    while (leading < d.digits.size() && d.digits[leading] == '0')
        ++leading;
    d.digits.erase(0, leading);
    while (!d.digits.empty() && d.digits.back() == '0') {
        d.digits.pop_back();
        ++d.exponent;
    }
    if (d.digits.empty())
        d.exponent = 0;
}

std::string repeat_zero(int count)
{
    return count > 0 ? std::string(static_cast<std::size_t>(count), '0') : std::string();
}

// Number::toString's output ("1.005", "1e+21", "1.23e-7") as a Decimal.
Decimal decimal_from_shortest(std::u16string const& text)
{
    Decimal d;
    std::string digits;
    int exponent = 0;
    std::size_t i = 0;
    for (; i < text.size() && text[i] != u'e'; ++i) {
        if (text[i] == u'.')
            continue;
        digits += static_cast<char>(text[i]);
    }
    std::size_t const point = text.find(u'.');
    std::size_t const mantissa_end = i;
    if (point != std::u16string::npos && point < mantissa_end)
        exponent -= static_cast<int>(mantissa_end - point - 1);
    if (i < text.size()) {
        ++i;
        bool const negative = i < text.size() && text[i] == u'-';
        if (i < text.size() && (text[i] == u'+' || text[i] == u'-'))
            ++i;
        int value = 0;
        for (; i < text.size(); ++i)
            value = value * 10 + (text[i] - u'0');
        exponent += negative ? -value : value;
    }
    d.digits = digits;
    d.exponent = exponent;
    normalize(d);
    return d;
}

} // namespace

double Decimal::to_double() const
{
    if (kind == Kind::NaN)
        return std::numeric_limits<double>::quiet_NaN();
    if (kind == Kind::Infinity)
        return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    if (digits.empty())
        return negative ? -0.0 : 0.0;
    std::string text = (negative ? "-" : "") + digits + "e" + std::to_string(exponent);
    return string_to_number(utf16_from_utf8(text));
}

int Decimal::compare(Decimal const& other) const
{
    auto sign_of = [](Decimal const& d) {
        if (d.kind == Kind::Infinity)
            return d.negative ? -2 : 2;
        if (d.digits.empty())
            return 0;
        return d.negative ? -1 : 1;
    };
    int const a = sign_of(*this);
    int const b = sign_of(other);
    if (a != b)
        return a < b ? -1 : 1;
    if (a == 0 || a == 2 || a == -2)
        return 0;
    // Same sign, both finite and non-zero: compare magnitudes.
    int result = 0;
    if (magnitude() != other.magnitude()) {
        result = magnitude() < other.magnitude() ? -1 : 1;
    } else {
        std::size_t const n = std::max(digits.size(), other.digits.size());
        for (std::size_t i = 0; i < n && result == 0; ++i) {
            char const x = i < digits.size() ? digits[i] : '0';
            char const y = i < other.digits.size() ? other.digits[i] : '0';
            if (x != y)
                result = x < y ? -1 : 1;
        }
    }
    return a < 0 ? -result : result;
}

Decimal decimal_from_double(double value)
{
    Decimal d;
    if (std::isnan(value)) {
        d.kind = Decimal::Kind::NaN;
        return d;
    }
    d.negative = std::signbit(value);
    if (std::isinf(value)) {
        d.kind = Decimal::Kind::Infinity;
        return d;
    }
    if (value == 0)
        return d;
    // The shortest digits that round-trip (Number::toString's), as ICU's
    // DecimalQuantity takes a double and so every shipping engine formats.
    Decimal shortest = decimal_from_shortest(number_to_string(std::fabs(value)));
    shortest.negative = d.negative;
    return shortest;
}

namespace {

// StringNumericLiteral (section 7.1.4.1.1) with its digits kept.
Decimal decimal_from_string(std::u16string_view source)
{
    Decimal nan;
    nan.kind = Decimal::Kind::NaN;
    std::string text = utf8_from_utf16(trim_string(source));
    Decimal d;
    if (text.empty())
        return d;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X' || text[1] == 'o' || text[1] == 'O' || text[1] == 'b' || text[1] == 'B')) {
        int const radix = (text[1] == 'x' || text[1] == 'X') ? 16 : (text[1] == 'o' || text[1] == 'O') ? 8 : 2;
        std::optional<BigInteger> const integer = BigInteger::parse_digits(std::string_view(text).substr(2), radix);
        if (!integer)
            return nan;
        d.digits = integer->to_string();
        normalize(d);
        return d;
    }
    std::size_t i = 0;
    if (text[i] == '+' || text[i] == '-') {
        d.negative = text[i] == '-';
        ++i;
    }
    if (text.substr(i) == "Infinity") {
        d.kind = Decimal::Kind::Infinity;
        return d;
    }
    std::string digits;
    int exponent = 0;
    bool any = false;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        digits += text[i++];
        any = true;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            digits += text[i++];
            --exponent;
            any = true;
        }
    }
    if (!any)
        return nan;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool negative_exponent = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            negative_exponent = text[i] == '-';
            ++i;
        }
        if (i >= text.size())
            return nan;
        long long value = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            if (value < 100000000)
                value = value * 10 + (text[i] - '0');
            ++i;
        }
        if (value > 1000000) {
            // Past any digit a format could show: infinitely large, or zero.
            if (negative_exponent) {
                d.digits.clear();
                return d;
            }
            if (digits.find_first_not_of('0') != std::string::npos) {
                d.kind = Decimal::Kind::Infinity;
                return d;
            }
        }
        exponent += static_cast<int>(negative_exponent ? -value : value);
    }
    if (i != text.size())
        return nan;
    d.digits = digits;
    d.exponent = exponent;
    normalize(d);
    return d;
}

// ---------------------------------------------------------------- rounding

// The value rounded to a multiple of increment * 10^magnitude, by the mode,
// the sign of the whole number (for ceil and floor) given.
Decimal round_decimal(Decimal const& x, int magnitude, int increment, std::string_view mode)
{
    if (x.digits.empty())
        return x;
    int const shift = x.exponent - magnitude;
    Decimal result;
    result.negative = x.negative;
    if (shift >= 0 && increment == 1)
        return x;
    // N = the integer part of |x| / 10^magnitude; the rest is a fraction
    // whose first digit and whether any follow decide a tie.
    std::string integer_digits;
    std::string fraction_digits;
    if (shift >= 0) {
        integer_digits = x.digits + repeat_zero(shift);
    } else {
        int const keep = static_cast<int>(x.digits.size()) + shift;
        if (keep > 0) {
            integer_digits = x.digits.substr(0, static_cast<std::size_t>(keep));
            fraction_digits = x.digits.substr(static_cast<std::size_t>(keep));
        } else {
            fraction_digits = repeat_zero(-keep) + x.digits;
        }
    }
    // Every increment divides 10^4, so the remainder by it, and the parity
    // of the quotient, are read from the last five digits: no arithmetic
    // on the whole number, however long a string made it.
    std::size_t const tail_size = std::min<std::size_t>(5, integer_digits.size());
    long const tail = integer_digits.empty() ? 0 : std::stol(integer_digits.substr(integer_digits.size() - tail_size));
    long const r = tail % increment;
    bool const q_odd = ((tail - r) / increment) % 2 != 0;
    bool const fraction_nonzero = fraction_digits.find_first_not_of('0') != std::string::npos;
    bool const exact = r == 0 && !fraction_nonzero;
    // Compare (r + f) against increment / 2.
    int half = 0; // -1 below, 0 a tie, 1 above
    {
        long const twice = r + r;
        if (twice > increment) {
            half = 1;
        } else if (twice == increment) {
            half = fraction_nonzero ? 1 : 0;
        } else {
            // 2r < inc: above only when 2r + 2f > inc, possible when inc = 2r + 1.
            if (twice + 1 == increment) {
                char const first = fraction_digits.empty() ? '0' : fraction_digits[0];
                bool const rest = fraction_digits.size() > 1 && fraction_digits.find_first_not_of('0', 1) != std::string::npos;
                if (first > '5' || (first == '5' && rest))
                    half = 1;
                else if (first == '5')
                    half = 0;
                else
                    half = -1;
            } else {
                half = -1;
            }
        }
    }
    bool expand = false;
    if (!exact) {
        bool const negative = x.negative;
        if (mode == "ceil")
            expand = !negative;
        else if (mode == "floor")
            expand = negative;
        else if (mode == "expand")
            expand = true;
        else if (mode == "trunc")
            expand = false;
        else if (half != 0)
            expand = half > 0;
        else if (mode == "halfCeil")
            expand = !negative;
        else if (mode == "halfFloor")
            expand = negative;
        else if (mode == "halfExpand")
            expand = true;
        else if (mode == "halfTrunc")
            expand = false;
        else // halfEven
            expand = q_odd;
    }
    // The result is N - r, plus one increment when rounding away: only the
    // last five digits change, with a carry past them.
    long low = tail - r + (expand ? increment : 0);
    std::string digits = integer_digits.substr(0, integer_digits.size() - tail_size);
    std::string low_text = std::to_string(low % 100000);
    if (!digits.empty() || low >= 100000)
        low_text = repeat_zero(5 - static_cast<int>(low_text.size())) + low_text;
    if (low >= 100000) {
        // Carry one into the leading digits.
        std::size_t i = digits.size();
        while (i > 0 && digits[i - 1] == '9') {
            digits[i - 1] = '0';
            --i;
        }
        if (i == 0)
            digits.insert(digits.begin(), '1');
        else
            ++digits[i - 1];
    }
    result.digits = digits + low_text;
    result.exponent = magnitude;
    normalize(result);
    return result;
}

// The digits of |x| laid out with `fraction` digits after the point.
std::string fixed_text(Decimal const& x, int fraction)
{
    std::string integer;
    std::string fraction_text;
    if (x.digits.empty()) {
        integer = "0";
    } else if (x.exponent >= 0) {
        integer = x.digits + repeat_zero(x.exponent);
    } else {
        int const point = static_cast<int>(x.digits.size()) + x.exponent;
        if (point > 0) {
            integer = x.digits.substr(0, static_cast<std::size_t>(point));
            fraction_text = x.digits.substr(static_cast<std::size_t>(point));
        } else {
            integer = "0";
            fraction_text = repeat_zero(-point) + x.digits;
        }
    }
    if (static_cast<int>(fraction_text.size()) < fraction)
        fraction_text += repeat_zero(fraction - static_cast<int>(fraction_text.size()));
    if (fraction_text.empty())
        return integer;
    return integer + "." + fraction_text;
}

struct RawResult {
    Decimal rounded;
    std::string text;
    int rounding_magnitude;
};

// ToRawPrecision (section 15.5.8).
RawResult to_raw_precision(Decimal const& x, int minimum, int maximum, std::string_view mode)
{
    RawResult result;
    if (x.digits.empty()) {
        result.rounded = x;
        result.text = fixed_text(x, minimum - 1);
        result.rounding_magnitude = 1 - maximum;
        return result;
    }
    int const e = x.magnitude();
    result.rounding_magnitude = e - maximum + 1;
    result.rounded = round_decimal(x, result.rounding_magnitude, 1, mode);
    int const e2 = result.rounded.digits.empty() ? 0 : result.rounded.magnitude();
    int const visible = std::max(0, std::max(minimum - 1 - e2, -result.rounded.exponent));
    result.text = fixed_text(result.rounded, visible);
    return result;
}

// ToRawFixed (section 15.5.9).
RawResult to_raw_fixed(Decimal const& x, int minimum, int maximum, int increment, std::string_view mode)
{
    RawResult result;
    result.rounding_magnitude = -maximum;
    result.rounded = round_decimal(x, -maximum, increment, mode);
    int const visible = std::max(minimum, result.rounded.digits.empty() ? 0 : std::max(0, -result.rounded.exponent));
    result.text = fixed_text(result.rounded, visible);
    return result;
}

bool is_integer(Decimal const& x)
{
    return x.digits.empty() || x.exponent >= 0;
}

} // namespace

RawNumber format_numeric_to_string(DigitOptions const& options, Decimal const& x)
{
    // section 15.5.3 on the magnitude; the sign is the caller's.
    Decimal value = x;
    RawResult result;
    switch (options.rounding_type) {
    case RoundingType::SignificantDigits:
        result = to_raw_precision(value, options.minimum_significant_digits, options.maximum_significant_digits, options.rounding_mode);
        break;
    case RoundingType::FractionDigits:
        result = to_raw_fixed(value, options.minimum_fraction_digits, options.maximum_fraction_digits,
            options.rounding_increment, options.rounding_mode);
        break;
    case RoundingType::MorePrecision:
    case RoundingType::LessPrecision: {
        RawResult const s = to_raw_precision(value, options.minimum_significant_digits, options.maximum_significant_digits, options.rounding_mode);
        RawResult const f = to_raw_fixed(value, options.minimum_fraction_digits, options.maximum_fraction_digits,
            options.rounding_increment, options.rounding_mode);
        bool const fixed_is_more_precise = s.rounding_magnitude > f.rounding_magnitude;
        if (options.rounding_type == RoundingType::MorePrecision)
            result = fixed_is_more_precise ? f : s;
        else
            result = fixed_is_more_precise ? s : f;
        break;
    }
    }
    std::string text = result.text;
    if (options.trailing_zero_display == "stripIfInteger" && is_integer(result.rounded)) {
        std::size_t const point = text.find('.');
        if (point != std::string::npos)
            text.erase(point);
    }
    std::size_t const point = text.find('.');
    int const integer_digits = static_cast<int>(point == std::string::npos ? text.size() : point);
    if (integer_digits < options.minimum_integer_digits)
        text = repeat_zero(options.minimum_integer_digits - integer_digits) + text;
    result.rounded.negative = x.negative;
    return { result.rounded, text };
}

std::optional<Decimal> to_intl_mathematical_value(Interpreter& in, Value const& value)
{
    std::optional<Value> const primitive = in.to_primitive(value, PreferredType::Number);
    if (!primitive)
        return std::nullopt;
    if (primitive->is_bigint()) {
        BigInteger const& integer = primitive->as_bigint()->value();
        Decimal d;
        d.negative = integer.is_negative();
        std::string text = integer.to_string();
        if (!text.empty() && text[0] == '-')
            text.erase(0, 1);
        d.digits = text;
        normalize(d);
        return d;
    }
    if (primitive->is_string())
        return decimal_from_string(primitive->as_string()->view());
    std::optional<double> const number = in.to_number(*primitive);
    if (!number)
        return std::nullopt;
    return decimal_from_double(*number);
}

std::string plural_category(bool ordinal, Decimal const&, std::string_view formatted)
{
    // The operands of the formatted digits (UTS #35 section 5.1): i the integer
    // digits, v the count of visible fraction digits.
    std::size_t const point = formatted.find('.');
    std::string_view const integer = formatted.substr(0, point);
    std::string_view const fraction = point == std::string_view::npos ? std::string_view() : formatted.substr(point + 1);
    auto last_digits = [&](std::size_t count) {
        int value = 0;
        std::size_t const start = integer.size() > count ? integer.size() - count : 0;
        for (std::size_t i = start; i < integer.size(); ++i)
            value = value * 10 + (integer[i] - '0');
        return value;
    };
    bool const fraction_zero = fraction.find_first_not_of('0') == std::string_view::npos;
    if (!ordinal) {
        // one: i = 1 and v = 0.
        bool const one = fraction.empty() && integer.find_first_not_of('0') != std::string_view::npos
            && integer.substr(integer.find_first_not_of('0')) == "1";
        return one ? "one" : "other";
    }
    if (!fraction_zero)
        return "other";
    int const n10 = last_digits(1);
    int const n100 = last_digits(2);
    if (n10 == 1 && n100 != 11)
        return "one";
    if (n10 == 2 && n100 != 12)
        return "two";
    if (n10 == 3 && n100 != 13)
        return "few";
    return "other";
}

// ------------------------------------------------------- digit options

std::optional<bool> set_number_format_digit_options(Interpreter& in, DigitOptions& out, Object* options,
    int mnfd_default, int mxfd_default, std::string_view notation)
{
    // section 15.1.3.
    std::optional<std::optional<int>> const mnid = get_number_option(in, options, "minimumIntegerDigits", 1, 21);
    if (!mnid)
        return std::nullopt;
    auto read = [&](std::string_view name) -> std::optional<Value> {
        if (options == nullptr)
            return Value::undefined();
        return in.get(*options, in.key(name));
    };
    Interpreter::Roots const roots(in);
    std::optional<Value> const mnfd_value = read("minimumFractionDigits");
    if (!mnfd_value)
        return std::nullopt;
    in.root(*mnfd_value);
    std::optional<Value> const mxfd_value = read("maximumFractionDigits");
    if (!mxfd_value)
        return std::nullopt;
    in.root(*mxfd_value);
    std::optional<Value> const mnsd_value = read("minimumSignificantDigits");
    if (!mnsd_value)
        return std::nullopt;
    in.root(*mnsd_value);
    std::optional<Value> const mxsd_value = read("maximumSignificantDigits");
    if (!mxsd_value)
        return std::nullopt;
    in.root(*mxsd_value);
    out.minimum_integer_digits = mnid->value_or(1);
    std::optional<std::optional<int>> const increment = get_number_option(in, options, "roundingIncrement", 1, 5000);
    if (!increment)
        return std::nullopt;
    static constexpr int increments[] = { 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000 };
    int const rounding_increment = increment->value_or(1);
    if (std::find(std::begin(increments), std::end(increments), rounding_increment) == std::end(increments))
        return in.throw_range_error("roundingIncrement value is out of range.");
    std::optional<std::optional<std::string>> const mode = get_string_option(in, options, "roundingMode",
        { "ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc", "halfEven" });
    if (!mode)
        return std::nullopt;
    std::optional<std::optional<std::string>> const priority = get_string_option(in, options, "roundingPriority",
        { "auto", "morePrecision", "lessPrecision" });
    if (!priority)
        return std::nullopt;
    std::optional<std::optional<std::string>> const trailing = get_string_option(in, options, "trailingZeroDisplay",
        { "auto", "stripIfInteger" });
    if (!trailing)
        return std::nullopt;
    if (rounding_increment != 1)
        mxfd_default = mnfd_default;
    out.rounding_increment = rounding_increment;
    out.rounding_mode = mode->value_or("halfExpand");
    out.trailing_zero_display = trailing->value_or("auto");
    std::string const rounding_priority = priority->value_or("auto");
    bool const has_sd = !mnsd_value->is_undefined() || !mxsd_value->is_undefined();
    bool const has_fd = !mnfd_value->is_undefined() || !mxfd_value->is_undefined();
    bool need_sd = true;
    bool need_fd = true;
    if (rounding_priority == "auto") {
        need_sd = has_sd;
        if (need_sd || (!has_fd && notation == "compact"))
            need_fd = false;
    }
    if (need_sd) {
        if (has_sd) {
            std::optional<std::optional<int>> const mnsd = default_number_option(in, *mnsd_value, 1, 21, "minimumSignificantDigits");
            if (!mnsd)
                return std::nullopt;
            int const minimum = mnsd->value_or(1);
            std::optional<std::optional<int>> const mxsd = default_number_option(in, *mxsd_value, minimum, 21, "maximumSignificantDigits");
            if (!mxsd)
                return std::nullopt;
            out.minimum_significant_digits = minimum;
            out.maximum_significant_digits = mxsd->value_or(21);
        } else {
            out.minimum_significant_digits = 1;
            out.maximum_significant_digits = 21;
        }
    }
    if (need_fd) {
        if (has_fd) {
            std::optional<std::optional<int>> const mnfd = default_number_option(in, *mnfd_value, 0, 100, "minimumFractionDigits");
            if (!mnfd)
                return std::nullopt;
            std::optional<std::optional<int>> const mxfd = default_number_option(in, *mxfd_value, 0, 100, "maximumFractionDigits");
            if (!mxfd)
                return std::nullopt;
            int minimum = 0;
            int maximum = 0;
            if (!*mnfd) {
                maximum = **mxfd;
                minimum = std::min(mnfd_default, maximum);
            } else if (!*mxfd) {
                minimum = **mnfd;
                maximum = std::max(mxfd_default, minimum);
            } else {
                minimum = **mnfd;
                maximum = **mxfd;
                if (minimum > maximum)
                    return in.throw_range_error("maximumFractionDigits value is out of range.");
            }
            out.minimum_fraction_digits = minimum;
            out.maximum_fraction_digits = maximum;
        } else {
            out.minimum_fraction_digits = mnfd_default;
            out.maximum_fraction_digits = mxfd_default;
        }
    }
    if (!need_sd && !need_fd) {
        out.minimum_fraction_digits = 0;
        out.maximum_fraction_digits = 0;
        out.minimum_significant_digits = 1;
        out.maximum_significant_digits = 2;
        out.rounding_type = RoundingType::MorePrecision;
        out.computed_rounding_priority = "morePrecision";
    } else if (rounding_priority == "auto") {
        out.rounding_type = has_sd ? RoundingType::SignificantDigits : RoundingType::FractionDigits;
        out.computed_rounding_priority = "auto";
    } else if (rounding_priority == "morePrecision") {
        out.rounding_type = RoundingType::MorePrecision;
        out.computed_rounding_priority = "morePrecision";
    } else {
        out.rounding_type = RoundingType::LessPrecision;
        out.computed_rounding_priority = "lessPrecision";
    }
    if (rounding_increment != 1) {
        if (out.rounding_type != RoundingType::FractionDigits)
            return in.throw_type_error("roundingIncrement requires fraction digits rounding");
        if (out.maximum_fraction_digits != out.minimum_fraction_digits)
            return in.throw_range_error("maximumFractionDigits must equal minimumFractionDigits with a roundingIncrement");
    }
    return true;
}

void put_digit_counts(Interpreter& in, Object& result, DigitOptions const& options)
{
    result.put(in.key("minimumIntegerDigits"), Value::number(options.minimum_integer_digits));
    if (options.rounding_type != RoundingType::SignificantDigits) {
        result.put(in.key("minimumFractionDigits"), Value::number(options.minimum_fraction_digits));
        result.put(in.key("maximumFractionDigits"), Value::number(options.maximum_fraction_digits));
    }
    if (options.rounding_type != RoundingType::FractionDigits) {
        result.put(in.key("minimumSignificantDigits"), Value::number(options.minimum_significant_digits));
        result.put(in.key("maximumSignificantDigits"), Value::number(options.maximum_significant_digits));
    }
}

void put_rounding_options(Interpreter& in, Object& result, DigitOptions const& options)
{
    result.put(in.key("roundingIncrement"), Value::number(options.rounding_increment));
    result.put(in.key("roundingMode"), intl_string(in, options.rounding_mode));
    result.put(in.key("roundingPriority"), intl_string(in, options.computed_rounding_priority));
    result.put(in.key("trailingZeroDisplay"), intl_string(in, options.trailing_zero_display));
}

// -------------------------------------------------------- NumberFormat

namespace {

intl_data::Currency const* find_currency(std::string_view code)
{
    for (auto const& currency : intl_data::currencies)
        if (currency.code == code)
            return &currency;
    return nullptr;
}

int currency_digits(std::string_view code)
{
    if (intl_data::Currency const* currency = find_currency(code))
        return currency->digits;
    for (auto const& entry : intl_data::currency_digits)
        if (entry.code == code)
            return entry.digits;
    return 2;
}

intl_data::Unit const* find_unit(std::string_view name)
{
    for (auto const& unit : intl_data::units)
        if (unit.name == name)
            return &unit;
    return nullptr;
}

bool is_well_formed_unit(std::string_view unit)
{
    if (find_unit(unit))
        return true;
    std::size_t const per = unit.find("-per-");
    if (per == std::string_view::npos)
        return false;
    return find_unit(unit.substr(0, per)) && find_unit(unit.substr(per + 5));
}

bool is_well_formed_currency(std::string_view code)
{
    return code.size() == 3 && std::all_of(code.begin(), code.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
}

std::string upper_ascii(std::string text)
{
    for (char& c : text)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return text;
}

int exponent_for_magnitude(NumberFormatData const& nf, int magnitude)
{
    if (nf.notation == "scientific")
        return magnitude;
    if (nf.notation == "engineering")
        return (magnitude >= 0 ? magnitude / 3 : -((-magnitude + 2) / 3)) * 3;
    // compact, English: thousands up to the trillions.
    if (magnitude < 3)
        return 0;
    return std::min(magnitude / 3 * 3, 12);
}

// ComputeExponent (section 15.5.5).
int compute_exponent(NumberFormatData const& nf, Decimal const& x)
{
    if (x.digits.empty())
        return 0;
    int const magnitude = x.magnitude();
    int const exponent = exponent_for_magnitude(nf, magnitude);
    Decimal scaled = x;
    scaled.exponent -= exponent;
    RawNumber const formatted = format_numeric_to_string(nf.digits, scaled);
    if (formatted.rounded.digits.empty())
        return exponent;
    if (formatted.rounded.magnitude() == magnitude - exponent)
        return exponent;
    return exponent_for_magnitude(nf, magnitude + 1);
}

std::u16string u16(std::string_view utf8)
{
    return utf16_from_utf8(utf8);
}

void push(std::vector<IntlPart>& parts, std::string type, std::string_view value)
{
    parts.push_back({ std::move(type), u16(value), {}, {} });
}

bool ends_with_letter(std::string_view text)
{
    if (text.empty())
        return false;
    char const last = text.back();
    return (last >= 'A' && last <= 'Z') || (last >= 'a' && last <= 'z');
}

// Splits a "{0} km" pattern into what goes before and after the number.
std::pair<std::string_view, std::string_view> split_pattern(std::string_view pattern)
{
    std::size_t const at = pattern.find("{0}");
    return { pattern.substr(0, at), pattern.substr(at + 3) };
}

// Text around the number as parts: the spaces at either end are literals,
// what they enclose one part of `type` ("kilometers per hour").
void push_affix(std::vector<IntlPart>& parts, std::string_view text, std::string_view type)
{
    std::size_t const first = text.find_first_not_of(' ');
    if (first == std::string_view::npos) {
        if (!text.empty())
            push(parts, "literal", text);
        return;
    }
    std::size_t const last = text.find_last_not_of(' ');
    if (first > 0)
        push(parts, "literal", text.substr(0, first));
    push(parts, std::string(type), text.substr(first, last + 1 - first));
    if (last + 1 < text.size())
        push(parts, "literal", text.substr(last + 1));
}

std::string_view unit_pattern(intl_data::Unit const& unit, std::string_view display, bool one)
{
    if (display == "long")
        return one ? unit.long_one : unit.long_other;
    if (display == "narrow")
        return one ? unit.narrow_one : unit.narrow_other;
    return one ? unit.short_one : unit.short_other;
}

} // namespace

std::vector<IntlPart> partition_number_pattern(NumberFormatData const& nf, Decimal x)
{
    // section 15.5.4.
    std::vector<IntlPart> number;
    Decimal rounded;
    std::string formatted_text;
    bool const negative = x.negative;
    int exponent = 0;
    if (x.kind == Decimal::Kind::NaN) {
        push(number, "nan", "NaN");
    } else if (x.kind == Decimal::Kind::Infinity) {
        push(number, "infinity", "\xE2\x88\x9E");
        rounded = x;
    } else {
        if (nf.style == "percent" && !x.digits.empty())
            x.exponent += 2;
        // The sign stays on: ceil and floor round a negative value the
        // other way (GetUnsignedRoundingMode).
        Decimal magnitude_value = x;
        if (nf.notation != "standard")
            exponent = compute_exponent(nf, magnitude_value);
        magnitude_value.exponent -= exponent;
        if (magnitude_value.digits.empty())
            magnitude_value.exponent = 0;
        RawNumber const raw = format_numeric_to_string(nf.digits, magnitude_value);
        rounded = raw.rounded;
        formatted_text = raw.text;
        std::size_t const point = formatted_text.find('.');
        std::string const integer = formatted_text.substr(0, point);
        // Grouping, by three from the right, when the setting allows it here.
        bool group = false;
        if (nf.use_grouping == "always" || nf.use_grouping == "auto")
            group = integer.size() >= 4;
        else if (nf.use_grouping == "min2")
            group = integer.size() >= 5;
        if (group) {
            std::size_t const first = integer.size() % 3 == 0 ? 3 : integer.size() % 3;
            push(number, "integer", integer.substr(0, first));
            for (std::size_t i = first; i < integer.size(); i += 3) {
                push(number, "group", ",");
                push(number, "integer", integer.substr(i, 3));
            }
        } else {
            push(number, "integer", integer);
        }
        if (point != std::string::npos) {
            push(number, "decimal", ".");
            push(number, "fraction", formatted_text.substr(point + 1));
        }
        if (nf.notation == "scientific" || nf.notation == "engineering") {
            push(number, "exponentSeparator", "E");
            if (exponent < 0)
                push(number, "exponentMinusSign", "-");
            push(number, "exponentInteger", std::to_string(exponent < 0 ? -exponent : exponent));
        } else if (nf.notation == "compact" && exponent > 0) {
            static constexpr std::string_view short_names[] = { "K", "M", "B", "T" };
            static constexpr std::string_view long_names[] = { "thousand", "million", "billion", "trillion" };
            std::size_t const index = static_cast<std::size_t>(exponent / 3 - 1);
            if (nf.compact_display == "long") {
                push(number, "literal", " ");
                push(number, "compact", long_names[index]);
            } else {
                push(number, "compact", short_names[index]);
            }
        }
    }

    // The sign (signDisplay, section 15.5.4's pattern choice).
    bool const is_zero = x.kind == Decimal::Kind::Finite && rounded.digits.empty();
    int sign = 0; // -1 minus, 1 plus
    if (x.kind != Decimal::Kind::NaN) {
        std::string const& display = nf.sign_display;
        if (display == "auto")
            sign = negative ? -1 : 0;
        else if (display == "always")
            sign = negative ? -1 : 1;
        else if (display == "exceptZero")
            sign = is_zero ? 0 : (negative ? -1 : 1);
        else if (display == "negative")
            sign = negative && !is_zero ? -1 : 0;
    } else if (nf.sign_display == "always" || nf.sign_display == "exceptZero") {
        sign = nf.sign_display == "always" ? 1 : 0;
    }
    bool const accounting = nf.style == "currency" && nf.currency_sign == "accounting" && sign < 0;
    std::vector<IntlPart> parts;
    auto push_sign = [&]() {
        if (accounting)
            return;
        if (sign < 0)
            push(parts, "minusSign", "-");
        else if (sign > 0)
            push(parts, "plusSign", "+");
    };
    auto append_number = [&]() { parts.insert(parts.end(), number.begin(), number.end()); };

    bool const one = plural_category(false, rounded, formatted_text) == "one" && exponent == 0;
    if (nf.style == "percent") {
        push_sign();
        append_number();
        push(parts, "percentSign", "%");
    } else if (nf.style == "currency") {
        intl_data::Currency const* currency = find_currency(nf.currency);
        if (accounting)
            push(parts, "literal", "(");
        if (nf.currency_display == "name") {
            push_sign();
            append_number();
            push(parts, "literal", " ");
            std::string const name = currency ? std::string(one ? currency->one : currency->other) : nf.currency;
            push(parts, "currency", name);
        } else {
            std::string symbol = nf.currency;
            if (currency && nf.currency_display == "symbol")
                symbol = std::string(currency->symbol);
            else if (currency && nf.currency_display == "narrowSymbol")
                symbol = std::string(currency->narrow_symbol);
            push_sign();
            push(parts, "currency", symbol);
            if (ends_with_letter(symbol))
                push(parts, "literal", "\xC2\xA0");
            append_number();
        }
        if (accounting)
            push(parts, "literal", ")");
    } else if (nf.style == "unit") {
        push_sign();
        std::string_view const unit = nf.unit;
        std::size_t const per = unit.find("-per-");
        if (per == std::string_view::npos) {
            intl_data::Unit const* u = find_unit(unit);
            auto const [before, after] = split_pattern(unit_pattern(*u, nf.unit_display, one));
            push_affix(parts, before, "unit");
            append_number();
            push_affix(parts, after, "unit");
        } else {
            intl_data::Unit const* numerator = find_unit(unit.substr(0, per));
            intl_data::Unit const* denominator = find_unit(unit.substr(per + 5));
            // The numerator's pattern with the denominator's "per" form
            // around its unit: "{0} km/h", "{0} kilometers per hour".
            std::string_view const pattern = unit_pattern(*numerator, nf.unit_display, one);
            auto const [before, after] = split_pattern(pattern);
            auto const [per_before, per_after] = split_pattern(nf.unit_display == "long" ? denominator->per_long : denominator->per_short);
            (void)per_before;
            std::string combined = std::string(after);
            // Short forms join tightly ("km/h"); long ones read "per hour".
            if (nf.unit_display == "long") {
                combined += std::string(per_after);
            } else {
                std::string_view denominator_symbol = per_after;
                if (!denominator_symbol.empty() && denominator_symbol[0] == '/')
                    denominator_symbol.remove_prefix(1);
                combined += "/" + std::string(denominator_symbol);
            }
            push_affix(parts, before, "unit");
            append_number();
            push_affix(parts, combined, "unit");
        }
    } else {
        push_sign();
        append_number();
    }
    if (nf.numbering_system != "latn") {
        for (IntlPart& part : parts) {
            if (part.type == "integer" || part.type == "fraction" || part.type == "exponentInteger")
                part.value = transliterate_digits(nf.numbering_system, part.value);
            else if (part.type == "decimal")
                part.value = decimal_separator(nf.numbering_system);
            else if (part.type == "group")
                part.value = group_separator(nf.numbering_system);
        }
    }
    return parts;
}

namespace {

// InitializeNumberFormat (section 15.1.2) on an allocated object.
std::optional<bool> initialize_number_format(Interpreter& in, NumberFormatData& nf, Value const& locales, Value const& options_value)
{
    std::optional<std::vector<std::string>> const requested = canonicalize_locale_list(in, locales);
    if (!requested)
        return std::nullopt;
    std::optional<Object*> const options = coerce_options_to_object(in, options_value);
    if (!options)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    if (*options)
        in.root(Value::object(*options));
    if (!read_locale_matcher(in, *options))
        return std::nullopt;
    std::optional<std::optional<std::string>> const numbering = get_string_option(in, *options, "numberingSystem");
    if (!numbering)
        return std::nullopt;
    if (*numbering && !is_unicode_type_sequence(**numbering))
        return in.throw_range_error("Invalid numberingSystem : " + **numbering);
    ResolvedLocale const resolved = resolve_locale(*requested, { { "nu", numbering_system_names(), *numbering } });
    nf.locale = resolved.locale;
    nf.data_locale = resolved.data_locale;
    nf.numbering_system = resolved.value("nu");

    // SetNumberFormatUnitOptions (section 15.1.4).
    std::optional<std::optional<std::string>> const style = get_string_option(in, *options, "style", { "decimal", "percent", "currency", "unit" });
    if (!style)
        return std::nullopt;
    nf.style = style->value_or("decimal");
    std::optional<std::optional<std::string>> const currency = get_string_option(in, *options, "currency");
    if (!currency)
        return std::nullopt;
    if (!*currency) {
        if (nf.style == "currency")
            return in.throw_type_error("Currency code is required with currency style.");
    } else if (!is_well_formed_currency(**currency)) {
        return in.throw_range_error("Invalid currency code : " + **currency);
    }
    std::optional<std::optional<std::string>> const currency_display = get_string_option(in, *options, "currencyDisplay",
        { "code", "symbol", "narrowSymbol", "name" });
    if (!currency_display)
        return std::nullopt;
    std::optional<std::optional<std::string>> const currency_sign = get_string_option(in, *options, "currencySign", { "standard", "accounting" });
    if (!currency_sign)
        return std::nullopt;
    std::optional<std::optional<std::string>> const unit = get_string_option(in, *options, "unit");
    if (!unit)
        return std::nullopt;
    if (!*unit) {
        if (nf.style == "unit")
            return in.throw_type_error("Unit is required with unit style.");
    } else if (!is_well_formed_unit(**unit)) {
        return in.throw_range_error("Invalid unit argument for Intl.NumberFormat() '" + **unit + "'");
    }
    std::optional<std::optional<std::string>> const unit_display = get_string_option(in, *options, "unitDisplay", { "short", "narrow", "long" });
    if (!unit_display)
        return std::nullopt;
    if (nf.style == "currency") {
        nf.currency = upper_ascii(**currency);
        nf.currency_display = currency_display->value_or("symbol");
        nf.currency_sign = currency_sign->value_or("standard");
    }
    if (nf.style == "unit") {
        nf.unit = **unit;
        nf.unit_display = unit_display->value_or("short");
    }

    std::optional<std::optional<std::string>> const notation = get_string_option(in, *options, "notation",
        { "standard", "scientific", "engineering", "compact" });
    if (!notation)
        return std::nullopt;
    nf.notation = notation->value_or("standard");
    int mnfd_default = 0;
    int mxfd_default = nf.style == "percent" ? 0 : 3;
    if (nf.style == "currency" && nf.notation == "standard") {
        mnfd_default = currency_digits(nf.currency);
        mxfd_default = mnfd_default;
    }
    if (!set_number_format_digit_options(in, nf.digits, *options, mnfd_default, mxfd_default, nf.notation))
        return std::nullopt;
    std::optional<std::optional<std::string>> const compact = get_string_option(in, *options, "compactDisplay", { "short", "long" });
    if (!compact)
        return std::nullopt;
    nf.compact_display = compact->value_or("short");
    std::string const default_grouping = nf.notation == "compact" ? "min2" : "auto";
    // GetBooleanOrStringNumberFormatOption (section 15.1.18).
    {
        std::optional<Value> const grouping = *options ? in.get(**options, in.key("useGrouping")) : std::optional<Value>(Value::undefined());
        if (!grouping)
            return std::nullopt;
        if (grouping->is_undefined()) {
            nf.use_grouping = default_grouping;
        } else if (grouping->is_boolean() && grouping->as_boolean()) {
            nf.use_grouping = "always";
        } else if (!Interpreter::to_boolean(*grouping)) {
            nf.use_grouping = "false";
        } else {
            std::optional<JsString*> const text = in.to_string(*grouping);
            if (!text)
                return std::nullopt;
            std::string const value = (*text)->to_utf8();
            if (value == "true" || value == "false")
                nf.use_grouping = default_grouping;
            else if (value == "min2" || value == "auto" || value == "always")
                nf.use_grouping = value;
            else
                return in.throw_range_error("Value " + value + " out of range for Intl.NumberFormat options property useGrouping");
        }
    }
    std::optional<std::optional<std::string>> const sign = get_string_option(in, *options, "signDisplay",
        { "auto", "never", "always", "exceptZero", "negative" });
    if (!sign)
        return std::nullopt;
    nf.sign_display = sign->value_or("auto");
    return true;
}

std::optional<Value> number_format_construct(Interpreter& in, Args args, Object* new_target)
{
    Interpreter::Roots const roots(in);
    in.root(Value::object(new_target));
    std::optional<IntlObjectOf<NumberFormatData>*> const object = allocate_intl<NumberFormatData>(in, new_target);
    if (!object)
        return std::nullopt;
    in.root(Value::object(*object));
    if (!initialize_number_format(in, (*object)->data, argument(args, 0), argument(args, 1)))
        return std::nullopt;
    return Value::object(*object);
}

// UnwrapNumberFormat (section 15.5.2): the object itself, or the one a legacy
// call chained behind the fallback symbol.
std::optional<IntlObjectOf<NumberFormatData>*> unwrap_number_format(Interpreter& in, Value const& this_value, std::string_view method)
{
    if (!this_value.is_object())
        return intl_this<NumberFormatData>(in, this_value, method);
    if (auto* nf = intl_cast<NumberFormatData>(this_value))
        return nf;
    Function* constructor = in.intrinsics().intl_constructors[static_cast<std::size_t>(IntlKind::NumberFormat)];
    std::optional<bool> const inherits = in.ordinary_has_instance(Value::object(constructor), this_value);
    if (!inherits)
        return std::nullopt;
    if (*inherits) {
        std::optional<Value> const inner = in.get(*this_value.as_object(), PropertyKey::symbol(in.intrinsics().intl_fallback_symbol));
        if (!inner)
            return std::nullopt;
        return intl_this<NumberFormatData>(in, *inner, method);
    }
    return intl_this<NumberFormatData>(in, this_value, method);
}

std::optional<Value> format_to_string(Interpreter& in, NumberFormatData const& nf, Value const& value)
{
    std::optional<Decimal> const x = to_intl_mathematical_value(in, value);
    if (!x)
        return std::nullopt;
    return intl_string(in, join_parts(partition_number_pattern(nf, *x)));
}

// PartitionNumberRangePattern (section 15.5.21).
std::optional<std::vector<IntlPart>> partition_number_range(Interpreter& in, NumberFormatData const& nf, Value const& start, Value const& end)
{
    if (start.is_undefined() || end.is_undefined())
        return in.throw_type_error("start or end is undefined");
    Interpreter::Roots const roots(in);
    in.root(end);
    std::optional<Decimal> const x = to_intl_mathematical_value(in, start);
    if (!x)
        return std::nullopt;
    std::optional<Decimal> const y = to_intl_mathematical_value(in, end);
    if (!y)
        return std::nullopt;
    if (x->kind == Decimal::Kind::NaN || y->kind == Decimal::Kind::NaN)
        return in.throw_range_error("Invalid number range: a bound is NaN");
    std::vector<IntlPart> xs = partition_number_pattern(nf, *x);
    std::vector<IntlPart> ys = partition_number_pattern(nf, *y);
    std::vector<IntlPart> result;
    if (join_parts(xs) == join_parts(ys)) {
        // FormatApproximately: the approximately sign before the number.
        result.push_back({ "approximatelySign", u"~", "source", "shared" });
        for (IntlPart part : xs) {
            part.extra_name = "source";
            part.extra_value = "shared";
            result.push_back(part);
        }
        return result;
    }
    auto plain = [](std::vector<IntlPart> const& parts) {
        return std::all_of(parts.begin(), parts.end(), [](IntlPart const& p) {
            return p.type == "integer" || p.type == "group" || p.type == "decimal" || p.type == "fraction";
        });
    };
    for (IntlPart part : xs) {
        part.extra_name = "source";
        part.extra_value = "startRange";
        result.push_back(part);
    }
    // An en dash (U+2013), spaced unless both ends are bare digits.
    std::u16string const dash = plain(xs) && plain(ys) ? std::u16string { 0x2013 } : std::u16string { u' ', 0x2013, u' ' };
    result.push_back({ "literal", dash, "source", "shared" });
    for (IntlPart part : ys) {
        part.extra_name = "source";
        part.extra_value = "endRange";
        result.push_back(part);
    }
    return result;
}

void install_number_format(Interpreter& in, Object& intl)
{
    NativeFunction* constructor = define_intl_constructor(in, intl, IntlKind::NumberFormat, 0,
        [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            // Called as a function (section 15.1.1 with no new.target): the active
            // function is new.target, and ChainNumberFormat hides the
            // result on a `this` that inherits from the prototype.
            Function* self = interp.intrinsics().intl_constructors[static_cast<std::size_t>(IntlKind::NumberFormat)];
            std::optional<Value> const made = number_format_construct(interp, args, self);
            if (!made)
                return std::nullopt;
            if (this_value.is_object()) {
                std::optional<bool> const inherits = interp.ordinary_has_instance(Value::object(self), this_value);
                if (!inherits)
                    return std::nullopt;
                if (*inherits) {
                    PropertyDescriptor const descriptor = PropertyDescriptor::data(*made, 0);
                    if (!interp.define_property_or_throw(*this_value.as_object(), PropertyKey::symbol(interp.intrinsics().intl_fallback_symbol), descriptor))
                        return std::nullopt;
                    return this_value;
                }
            }
            return made;
        },
        number_format_construct);
    Object& prototype = *intl_prototype(in, IntlKind::NumberFormat);
    (void)constructor;
    Heap::NoCollect const guard(in.heap());

    define_accessor(in, prototype, "format", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<NumberFormatData>*> const nf = unwrap_number_format(interp, this_value, "format");
        if (!nf)
            return std::nullopt;
        if ((*nf)->slot(0).is_undefined()) {
            ClosureFunction* bound = interp.new_closure("", 1, { Value::object(*nf) },
                [](Interpreter& in2, ClosureFunction& self, Value const&, Args args) -> std::optional<Value> {
                    auto* object = static_cast<IntlObjectOf<NumberFormatData>*>(self.slot(0).as_object());
                    return format_to_string(in2, object->data, argument(args, 0));
                });
            (*nf)->set_slot(0, Value::object(bound));
        }
        return (*nf)->slot(0);
    });
    define_method(in, prototype, "formatToParts", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<NumberFormatData>*> const nf = intl_this<NumberFormatData>(interp, this_value, "formatToParts");
        if (!nf)
            return std::nullopt;
        std::optional<Decimal> const x = to_intl_mathematical_value(interp, argument(args, 0));
        if (!x)
            return std::nullopt;
        return parts_to_array(interp, partition_number_pattern((*nf)->data, *x));
    });
    define_method(in, prototype, "formatRange", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<NumberFormatData>*> const nf = intl_this<NumberFormatData>(interp, this_value, "formatRange");
        if (!nf)
            return std::nullopt;
        std::optional<std::vector<IntlPart>> const parts = partition_number_range(interp, (*nf)->data, argument(args, 0), argument(args, 1));
        if (!parts)
            return std::nullopt;
        return intl_string(interp, join_parts(*parts));
    });
    define_method(in, prototype, "formatRangeToParts", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<NumberFormatData>*> const nf = intl_this<NumberFormatData>(interp, this_value, "formatRangeToParts");
        if (!nf)
            return std::nullopt;
        std::optional<std::vector<IntlPart>> const parts = partition_number_range(interp, (*nf)->data, argument(args, 0), argument(args, 1));
        if (!parts)
            return std::nullopt;
        return parts_to_array(interp, *parts);
    });
    define_method(in, prototype, "resolvedOptions", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<NumberFormatData>*> const found = unwrap_number_format(interp, this_value, "resolvedOptions");
        if (!found)
            return std::nullopt;
        NumberFormatData const& nf = (*found)->data;
        Heap::NoCollect const no_collect(interp.heap());
        Object* result = interp.new_object();
        auto put = [&](char const* name, std::string const& value) { result->put(interp.key(name), intl_string(interp, value)); };
        put("locale", nf.locale);
        put("numberingSystem", nf.numbering_system);
        put("style", nf.style);
        if (nf.style == "currency") {
            put("currency", nf.currency);
            put("currencyDisplay", nf.currency_display);
            put("currencySign", nf.currency_sign);
        }
        if (nf.style == "unit") {
            put("unit", nf.unit);
            put("unitDisplay", nf.unit_display);
        }
        put_digit_counts(interp, *result, nf.digits);
        if (nf.use_grouping == "false")
            result->put(interp.key("useGrouping"), Value::boolean(false));
        else
            put("useGrouping", nf.use_grouping);
        put("notation", nf.notation);
        if (nf.notation == "compact")
            put("compactDisplay", nf.compact_display);
        put("signDisplay", nf.sign_display);
        put_rounding_options(interp, *result, nf.digits);
        return Value::object(result);
    });
}

// ------------------------------------------------------------ PluralRules

struct PluralRulesData {
    static constexpr IntlKind kind = IntlKind::PluralRules;
    std::string locale;
    std::string type = "cardinal";
    std::string notation = "standard";
    std::string compact_display = "short";
    DigitOptions digits;
};

std::string resolve_plural(PluralRulesData const& pr, double n)
{
    if (!std::isfinite(n))
        return "other";
    Decimal x = decimal_from_double(n);
    x.negative = false;
    RawNumber const raw = format_numeric_to_string(pr.digits, x);
    return plural_category(pr.type == "ordinal", raw.rounded, raw.text);
}

void install_plural_rules(Interpreter& in, Object& intl)
{
    define_intl_constructor(in, intl, IntlKind::PluralRules, 0, {},
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            // section 16.1.1.
            Interpreter::Roots const roots(interp);
            interp.root(Value::object(new_target));
            std::optional<IntlObjectOf<PluralRulesData>*> const object = allocate_intl<PluralRulesData>(interp, new_target);
            if (!object)
                return std::nullopt;
            interp.root(Value::object(*object));
            PluralRulesData& pr = (*object)->data;
            std::optional<std::vector<std::string>> const requested = canonicalize_locale_list(interp, argument(args, 0));
            if (!requested)
                return std::nullopt;
            std::optional<Object*> const options = coerce_options_to_object(interp, argument(args, 1));
            if (!options)
                return std::nullopt;
            if (*options)
                interp.root(Value::object(*options));
            if (!read_locale_matcher(interp, *options))
                return std::nullopt;
            std::optional<std::optional<std::string>> const type = get_string_option(interp, *options, "type", { "cardinal", "ordinal" });
            if (!type)
                return std::nullopt;
            pr.type = type->value_or("cardinal");
            std::optional<std::optional<std::string>> const notation = get_string_option(interp, *options, "notation",
                { "standard", "scientific", "engineering", "compact" });
            if (!notation)
                return std::nullopt;
            pr.notation = notation->value_or("standard");
            std::optional<std::optional<std::string>> const compact = get_string_option(interp, *options, "compactDisplay", { "short", "long" });
            if (!compact)
                return std::nullopt;
            pr.compact_display = compact->value_or("short");
            if (!set_number_format_digit_options(interp, pr.digits, *options, 0, 3, pr.notation))
                return std::nullopt;
            pr.locale = resolve_locale(*requested, {}).locale;
            return Value::object(*object);
        });
    Object& prototype = *intl_prototype(in, IntlKind::PluralRules);
    Heap::NoCollect const guard(in.heap());
    define_method(in, prototype, "select", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<PluralRulesData>*> const pr = intl_this<PluralRulesData>(interp, this_value, "select");
        if (!pr)
            return std::nullopt;
        std::optional<double> const n = interp.to_number(argument(args, 0));
        if (!n)
            return std::nullopt;
        return intl_string(interp, resolve_plural((*pr)->data, *n));
    });
    define_method(in, prototype, "selectRange", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<PluralRulesData>*> const pr = intl_this<PluralRulesData>(interp, this_value, "selectRange");
        if (!pr)
            return std::nullopt;
        if (argument(args, 0).is_undefined() || argument(args, 1).is_undefined())
            return interp.throw_type_error("start or end is undefined");
        std::optional<double> const x = interp.to_number(argument(args, 0));
        if (!x)
            return std::nullopt;
        std::optional<double> const y = interp.to_number(argument(args, 1));
        if (!y)
            return std::nullopt;
        if (std::isnan(*x) || std::isnan(*y))
            return interp.throw_range_error("Invalid range: a bound is NaN");
        // English's plural ranges all end in "other".
        return intl_string(interp, "other");
    });
    define_method(in, prototype, "resolvedOptions", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<PluralRulesData>*> const found = intl_this<PluralRulesData>(interp, this_value, "resolvedOptions");
        if (!found)
            return std::nullopt;
        PluralRulesData const& pr = (*found)->data;
        Heap::NoCollect const no_collect(interp.heap());
        Object* result = interp.new_object();
        result->put(interp.key("locale"), intl_string(interp, pr.locale));
        result->put(interp.key("type"), intl_string(interp, pr.type));
        result->put(interp.key("notation"), intl_string(interp, pr.notation));
        if (pr.notation == "compact")
            result->put(interp.key("compactDisplay"), intl_string(interp, pr.compact_display));
        put_digit_counts(interp, *result, pr.digits);
        std::vector<std::string> const categories = pr.type == "ordinal"
            ? std::vector<std::string> { "one", "two", "few", "other" }
            : std::vector<std::string> { "one", "other" };
        result->put(interp.key("pluralCategories"), intl_string_array(interp, categories));
        put_rounding_options(interp, *result, pr.digits);
        return Value::object(result);
    });
}

// ---------------------------------------------------- RelativeTimeFormat

struct RelativeTimeFormatData {
    static constexpr IntlKind kind = IntlKind::RelativeTimeFormat;
    std::string locale;
    std::string numbering_system = "latn";
    std::string style = "long";
    std::string numeric = "always";
    NumberFormatData number_format;
};

std::optional<std::vector<IntlPart>> partition_relative_time(Interpreter& in, RelativeTimeFormatData const& rtf,
    Value const& value_argument, Value const& unit_argument)
{
    // section 17.5.2.
    Interpreter::Roots const roots(in);
    in.root(unit_argument);
    std::optional<double> const value = in.to_number(value_argument);
    if (!value)
        return std::nullopt;
    std::optional<JsString*> const unit_string = in.to_string(unit_argument);
    if (!unit_string)
        return std::nullopt;
    if (!std::isfinite(*value))
        return in.throw_range_error("Invalid time value");
    std::string unit = (*unit_string)->to_utf8();
    std::string const original = unit;
    static constexpr std::string_view names[] = { "second", "minute", "hour", "day", "week", "month", "quarter", "year" };
    std::size_t index = 8;
    for (std::size_t i = 0; i < 8; ++i)
        if (unit == names[i] || unit == std::string(names[i]) + "s")
            index = i;
    if (index == 8)
        return in.throw_range_error("Invalid unit argument for format() '" + original + "'");
    unit = std::string(names[index]);
    std::size_t const width = rtf.style == "short" ? 1 : rtf.style == "narrow" ? 2 : 0;
    intl_data::RelativeUnit const& data = intl_data::relative_units[width][index];
    std::vector<IntlPart> parts;
    if (rtf.numeric == "auto") {
        std::string_view phrase;
        if (*value == -1)
            phrase = data.last;
        else if (*value == 0)
            phrase = data.current;
        else if (*value == 1)
            phrase = data.next;
        if (!phrase.empty()) {
            parts.push_back({ "literal", utf16_from_utf8(phrase), {}, {} });
            return parts;
        }
    }
    bool const past = std::signbit(*value);
    Decimal x = decimal_from_double(*value);
    x.negative = false;
    std::vector<IntlPart> number = partition_number_pattern(rtf.number_format, x);
    RawNumber const raw = format_numeric_to_string(rtf.number_format.digits, x);
    bool const one = plural_category(false, raw.rounded, raw.text) == "one";
    std::string_view const pattern = past ? (one ? data.past_one : data.past_other) : (one ? data.future_one : data.future_other);
    std::size_t const at = pattern.find("{0}");
    if (at > 0)
        parts.push_back({ "literal", utf16_from_utf8(pattern.substr(0, at)), {}, {} });
    for (IntlPart part : number) {
        part.extra_name = "unit";
        part.extra_value = unit;
        parts.push_back(part);
    }
    if (at + 3 < pattern.size())
        parts.push_back({ "literal", utf16_from_utf8(pattern.substr(at + 3)), {}, {} });
    return parts;
}

void install_relative_time_format(Interpreter& in, Object& intl)
{
    define_intl_constructor(in, intl, IntlKind::RelativeTimeFormat, 0, {},
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            // section 17.1.1.
            Interpreter::Roots const roots(interp);
            interp.root(Value::object(new_target));
            std::optional<IntlObjectOf<RelativeTimeFormatData>*> const object = allocate_intl<RelativeTimeFormatData>(interp, new_target);
            if (!object)
                return std::nullopt;
            interp.root(Value::object(*object));
            RelativeTimeFormatData& rtf = (*object)->data;
            std::optional<std::vector<std::string>> const requested = canonicalize_locale_list(interp, argument(args, 0));
            if (!requested)
                return std::nullopt;
            std::optional<Object*> const options = coerce_options_to_object(interp, argument(args, 1));
            if (!options)
                return std::nullopt;
            if (*options)
                interp.root(Value::object(*options));
            if (!read_locale_matcher(interp, *options))
                return std::nullopt;
            std::optional<std::optional<std::string>> const numbering = get_string_option(interp, *options, "numberingSystem");
            if (!numbering)
                return std::nullopt;
            if (*numbering && !is_unicode_type_sequence(**numbering))
                return interp.throw_range_error("Invalid numberingSystem : " + **numbering);
            ResolvedLocale const resolved = resolve_locale(*requested, { { "nu", numbering_system_names(), *numbering } });
            rtf.locale = resolved.locale;
            rtf.numbering_system = resolved.value("nu");
            std::optional<std::optional<std::string>> const style = get_string_option(interp, *options, "style", { "long", "short", "narrow" });
            if (!style)
                return std::nullopt;
            rtf.style = style->value_or("long");
            std::optional<std::optional<std::string>> const numeric = get_string_option(interp, *options, "numeric", { "always", "auto" });
            if (!numeric)
                return std::nullopt;
            rtf.numeric = numeric->value_or("always");
            rtf.number_format.locale = rtf.locale;
            rtf.number_format.numbering_system = rtf.numbering_system;
            return Value::object(*object);
        });
    Object& prototype = *intl_prototype(in, IntlKind::RelativeTimeFormat);
    Heap::NoCollect const guard(in.heap());
    define_method(in, prototype, "format", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<RelativeTimeFormatData>*> const rtf = intl_this<RelativeTimeFormatData>(interp, this_value, "format");
        if (!rtf)
            return std::nullopt;
        std::optional<std::vector<IntlPart>> const parts = partition_relative_time(interp, (*rtf)->data, argument(args, 0), argument(args, 1));
        if (!parts)
            return std::nullopt;
        return intl_string(interp, join_parts(*parts));
    });
    define_method(in, prototype, "formatToParts", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<RelativeTimeFormatData>*> const rtf = intl_this<RelativeTimeFormatData>(interp, this_value, "formatToParts");
        if (!rtf)
            return std::nullopt;
        std::optional<std::vector<IntlPart>> const parts = partition_relative_time(interp, (*rtf)->data, argument(args, 0), argument(args, 1));
        if (!parts)
            return std::nullopt;
        return parts_to_array(interp, *parts);
    });
    define_method(in, prototype, "resolvedOptions", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<RelativeTimeFormatData>*> const found = intl_this<RelativeTimeFormatData>(interp, this_value, "resolvedOptions");
        if (!found)
            return std::nullopt;
        RelativeTimeFormatData const& rtf = (*found)->data;
        Heap::NoCollect const no_collect(interp.heap());
        Object* result = interp.new_object();
        result->put(interp.key("locale"), intl_string(interp, rtf.locale));
        result->put(interp.key("style"), intl_string(interp, rtf.style));
        result->put(interp.key("numeric"), intl_string(interp, rtf.numeric));
        result->put(interp.key("numberingSystem"), intl_string(interp, rtf.numbering_system));
        return Value::object(result);
    });
}

} // namespace

std::optional<IntlObjectOf<NumberFormatData>*> create_number_format(Interpreter& in, Value const& locales, Value const& options)
{
    Interpreter::Roots const roots(in);
    in.root(locales);
    in.root(options);
    auto* object = in.heap().allocate<IntlObjectOf<NumberFormatData>>(intl_prototype(in, IntlKind::NumberFormat));
    in.root(Value::object(object));
    if (!initialize_number_format(in, object->data, locales, options))
        return std::nullopt;
    return object;
}

void install_intl_number_format(Interpreter& in, Object& intl)
{
    install_number_format(in, intl);
}

void install_intl_plural_rules(Interpreter& in, Object& intl)
{
    install_plural_rules(in, intl);
}

void install_intl_relative_time_format(Interpreter& in, Object& intl)
{
    install_relative_time_format(in, intl);
}

void install_intl_number_methods(Interpreter& in)
{
    // Number.prototype.toLocaleString (section 20.1.1) and
    // BigInt.prototype.toLocaleString (section 20.2.1): a NumberFormat of the
    // arguments, formatting the value.
    Heap::NoCollect const guard(in.heap());
    define_method(in, *in.intrinsics().number_prototype, "toLocaleString", 0,
        [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            std::optional<double> const value = this_number_value(interp, this_value, "toLocaleString");
            if (!value)
                return std::nullopt;
            std::optional<IntlObjectOf<NumberFormatData>*> const nf = create_number_format(interp, argument(args, 0), argument(args, 1));
            if (!nf)
                return std::nullopt;
            return intl_string(interp, join_parts(partition_number_pattern((*nf)->data, decimal_from_double(*value))));
        });
    define_method(in, *in.intrinsics().bigint_prototype, "toLocaleString", 0,
        [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            Value bigint = this_value;
            if (this_value.is_object() && this_value.as_object()->class_id() == Object::Class::BigInt)
                bigint = static_cast<PrimitiveObject*>(this_value.as_object())->primitive();
            if (!bigint.is_bigint())
                return interp.throw_type_error("BigInt.prototype.toLocaleString requires that 'this' be a BigInt");
            Interpreter::Roots const roots(interp);
            interp.root(bigint);
            std::optional<IntlObjectOf<NumberFormatData>*> const nf = create_number_format(interp, argument(args, 0), argument(args, 1));
            if (!nf)
                return std::nullopt;
            std::optional<Decimal> const x = to_intl_mathematical_value(interp, bigint);
            if (!x)
                return std::nullopt;
            return intl_string(interp, join_parts(partition_number_pattern((*nf)->data, *x)));
        });
}

}
