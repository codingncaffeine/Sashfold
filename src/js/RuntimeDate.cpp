#include "js/Runtime.h"

// Date (§21.4): the time-value arithmetic of §21.4.1 written out, the
// constructor and its parser, and the prototype. The clock and the local
// zone come from the C runtime, the only two places the library reads
// anything from outside; both are overridable for tests.

#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

namespace {

constexpr double ms_per_second = 1000;
constexpr double ms_per_minute = 60000;
constexpr double ms_per_hour = 3600000;
constexpr double ms_per_day = 86400000;
constexpr double time_clip_limit = 8.64e15;
double const nan = std::numeric_limits<double>::quiet_NaN();

double (*g_now)() = nullptr;
double (*g_offset)(double) = nullptr;

double positive_modulo(double x, double y)
{
    double const r = std::fmod(x, y);
    return r < 0 ? r + y : r;
}

// ---------------------------------------------------- §21.4.1 arithmetic

double day(double t) { return std::floor(t / ms_per_day); }
double time_within_day(double t) { return positive_modulo(t, ms_per_day); }

double days_in_year(double y)
{
    if (std::fmod(y, 4) != 0)
        return 365;
    if (std::fmod(y, 100) != 0)
        return 366;
    if (std::fmod(y, 400) != 0)
        return 365;
    return 366;
}

double day_from_year(double y)
{
    return 365 * (y - 1970) + std::floor((y - 1969) / 4) - std::floor((y - 1901) / 100) + std::floor((y - 1601) / 400);
}

double time_from_year(double y) { return ms_per_day * day_from_year(y); }

double year_from_time(double t)
{
    double y = std::floor(t / (ms_per_day * 365.2425)) + 1970;
    while (time_from_year(y) > t)
        --y;
    while (time_from_year(y + 1) <= t)
        ++y;
    return y;
}

bool in_leap_year(double t) { return days_in_year(year_from_time(t)) == 366; }
double day_within_year(double t) { return day(t) - day_from_year(year_from_time(t)); }

// The first day of each month within the year, for both year kinds.
constexpr int month_starts[2][13] = {
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
    { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 },
};

double month_from_time(double t)
{
    double const d = day_within_year(t);
    int const leap = in_leap_year(t) ? 1 : 0;
    for (int m = 0; m < 12; ++m) {
        if (d < month_starts[leap][m + 1])
            return m;
    }
    return 11;
}

double date_from_time(double t)
{
    double const d = day_within_year(t);
    int const leap = in_leap_year(t) ? 1 : 0;
    auto const m = static_cast<int>(month_from_time(t));
    return d - month_starts[leap][m] + 1;
}

double week_day(double t) { return positive_modulo(day(t) + 4, 7); }
double hour_from_time(double t) { return positive_modulo(std::floor(t / ms_per_hour), 24); }
double min_from_time(double t) { return positive_modulo(std::floor(t / ms_per_minute), 60); }
double sec_from_time(double t) { return positive_modulo(std::floor(t / ms_per_second), 60); }
double ms_from_time(double t) { return positive_modulo(t, ms_per_second); }

double make_time(double hour, double min, double sec, double ms)
{
    if (!std::isfinite(hour) || !std::isfinite(min) || !std::isfinite(sec) || !std::isfinite(ms))
        return nan;
    double const h = Interpreter::to_integer_or_infinity(hour);
    double const m = Interpreter::to_integer_or_infinity(min);
    double const s = Interpreter::to_integer_or_infinity(sec);
    double const milli = Interpreter::to_integer_or_infinity(ms);
    return h * ms_per_hour + m * ms_per_minute + s * ms_per_second + milli;
}

double make_day(double year, double month, double date)
{
    if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date))
        return nan;
    double const y = Interpreter::to_integer_or_infinity(year);
    double const m = Interpreter::to_integer_or_infinity(month);
    double const dt = Interpreter::to_integer_or_infinity(date);
    double const ym = y + std::floor(m / 12);
    if (!std::isfinite(ym) || std::abs(ym) > 400000)
        return nan;
    auto const mn = static_cast<int>(positive_modulo(m, 12));
    int const leap = days_in_year(ym) == 366 ? 1 : 0;
    double const day_of_month_start = day_from_year(ym) + month_starts[leap][mn];
    return day_of_month_start + dt - 1;
}

double make_date(double day_value, double time)
{
    if (!std::isfinite(day_value) || !std::isfinite(time))
        return nan;
    double const tv = day_value * ms_per_day + time;
    return std::isfinite(tv) ? tv : nan;
}

double time_clip(double time)
{
    if (!std::isfinite(time) || std::abs(time) > time_clip_limit)
        return nan;
    return Interpreter::to_integer_or_infinity(time);
}

double local_time(double t) { return t + local_time_zone_offset_minutes(t) * ms_per_minute; }

// UTC (§21.4.1.26): the offset at the local wall time, found by a
// second reading around a transition.
double utc_time(double t)
{
    double const guess = local_time_zone_offset_minutes(t);
    double const first = t - guess * ms_per_minute;
    double const refined = local_time_zone_offset_minutes(first);
    return t - refined * ms_per_minute;
}

// ---------------------------------------------------------- formatting

constexpr char const* day_names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
constexpr char const* month_names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

std::string padded(double value, int width)
{
    std::string digits = std::to_string(static_cast<long long>(std::abs(value)));
    while (static_cast<int>(digits.size()) < width)
        digits.insert(digits.begin(), '0');
    return digits;
}

std::string year_string(double year)
{
    return (year < 0 ? "-" : "") + padded(year, 4);
}

// DateString (§21.4.4.41.2).
std::string date_string(double tv)
{
    return std::string(day_names[static_cast<int>(week_day(tv))]) + " " + month_names[static_cast<int>(month_from_time(tv))] + " "
        + padded(date_from_time(tv), 2) + " " + year_string(year_from_time(tv));
}

// TimeString (§21.4.4.41.1).
std::string time_string(double tv)
{
    return padded(hour_from_time(tv), 2) + ":" + padded(min_from_time(tv), 2) + ":" + padded(sec_from_time(tv), 2) + " GMT";
}

// TimeZoneString (§21.4.4.41.3), with the implementation-defined name
// left empty.
std::string time_zone_string(double tv)
{
    double const offset = local_time_zone_offset_minutes(tv);
    double const absolute = std::abs(offset);
    return std::string(offset >= 0 ? "+" : "-") + padded(std::floor(absolute / 60), 2) + padded(std::fmod(absolute, 60), 2);
}

std::string to_date_string(double tv)
{
    if (std::isnan(tv))
        return "Invalid Date";
    double const t = local_time(tv);
    return date_string(t) + " " + time_string(t) + time_zone_string(tv);
}

std::string to_utc_string(double tv)
{
    return std::string(day_names[static_cast<int>(week_day(tv))]) + ", " + padded(date_from_time(tv), 2) + " " + month_names[static_cast<int>(month_from_time(tv))]
        + " " + year_string(year_from_time(tv)) + " " + time_string(tv);
}

std::string to_iso_string(double tv)
{
    double const year = year_from_time(tv);
    std::string year_text;
    if (year >= 0 && year <= 9999)
        year_text = padded(year, 4);
    else
        year_text = (year < 0 ? "-" : "+") + padded(year, 6);
    return year_text + "-" + padded(month_from_time(tv) + 1, 2) + "-" + padded(date_from_time(tv), 2) + "T" + padded(hour_from_time(tv), 2) + ":"
        + padded(min_from_time(tv), 2) + ":" + padded(sec_from_time(tv), 2) + "." + padded(ms_from_time(tv), 3) + "Z";
}

// ------------------------------------------------------------- parsing

struct Cursor {
    std::u16string_view text;
    std::size_t pos = 0;

    bool done() const { return pos >= text.size(); }
    char16_t peek() const { return done() ? u'\0' : text[pos]; }
    bool take(char16_t c)
    {
        if (peek() != c)
            return false;
        ++pos;
        return true;
    }
    bool digits(int count, double& out)
    {
        if (pos + static_cast<std::size_t>(count) > text.size())
            return false;
        double value = 0;
        for (int k = 0; k < count; ++k) {
            char16_t const c = text[pos + static_cast<std::size_t>(k)];
            if (c < u'0' || c > u'9')
                return false;
            value = value * 10 + (c - u'0');
        }
        pos += static_cast<std::size_t>(count);
        out = value;
        return true;
    }
    // One or more digits; the count taken is returned through `taken`.
    bool number(double& out, int& taken)
    {
        taken = 0;
        double value = 0;
        while (!done() && peek() >= u'0' && peek() <= u'9') {
            value = value * 10 + (peek() - u'0');
            ++pos;
            ++taken;
        }
        out = value;
        return taken > 0;
    }
};

// The Date Time String Format (§21.4.1.32): date-only forms are UTC,
// date-time forms without an offset are local time.
std::optional<double> parse_iso(std::u16string_view text)
{
    Cursor c { text };
    double year = 0;
    if (c.peek() == u'+' || c.peek() == u'-') {
        bool const negative = c.peek() == u'-';
        ++c.pos;
        if (!c.digits(6, year))
            return std::nullopt;
        if (negative && year == 0)
            return std::nullopt;
        if (negative)
            year = -year;
    } else if (!c.digits(4, year)) {
        return std::nullopt;
    }
    double month = 1;
    double date = 1;
    if (c.take(u'-')) {
        if (!c.digits(2, month) || month < 1 || month > 12)
            return std::nullopt;
        if (c.take(u'-')) {
            if (!c.digits(2, date) || date < 1 || date > 31)
                return std::nullopt;
        }
    }
    bool has_time = false;
    double hour = 0;
    double minute = 0;
    double second = 0;
    double millis = 0;
    std::optional<double> offset_minutes;
    if (c.take(u'T')) {
        has_time = true;
        if (!c.digits(2, hour) || !c.take(u':') || !c.digits(2, minute))
            return std::nullopt;
        if (c.take(u':')) {
            if (!c.digits(2, second))
                return std::nullopt;
            if (c.take(u'.')) {
                double fraction = 0;
                int taken = 0;
                if (!c.number(fraction, taken))
                    return std::nullopt;
                millis = std::floor(fraction / std::pow(10.0, taken - 3));
            }
        }
        if (hour > 24 || minute > 59 || second > 59)
            return std::nullopt;
        if (hour == 24 && (minute != 0 || second != 0 || millis != 0))
            return std::nullopt;
        if (c.take(u'Z')) {
            offset_minutes = 0;
        } else if (c.peek() == u'+' || c.peek() == u'-') {
            bool const negative = c.peek() == u'-';
            ++c.pos;
            double oh = 0;
            double om = 0;
            if (!c.digits(2, oh) || !c.take(u':') || !c.digits(2, om) || oh > 23 || om > 59)
                return std::nullopt;
            offset_minutes = (oh * 60 + om) * (negative ? -1 : 1);
        }
    }
    if (!c.done())
        return std::nullopt;
    double const day_value = make_day(year, month - 1, date);
    double const time = make_time(hour, minute, second, millis);
    double result = make_date(day_value, time);
    if (!has_time)
        return time_clip(result);
    if (offset_minutes)
        return time_clip(result - *offset_minutes * ms_per_minute);
    return time_clip(utc_time(result));
}

int month_index(std::u16string_view word)
{
    if (word.size() < 3)
        return -1;
    std::string lower;
    for (std::size_t k = 0; k < 3; ++k) {
        char16_t const c = word[k];
        lower += static_cast<char>((c >= u'A' && c <= u'Z') ? c + 32 : c);
    }
    constexpr char const* names[12] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    for (int m = 0; m < 12; ++m) {
        if (lower == names[m])
            return m;
    }
    return -1;
}

bool is_letter(char16_t c)
{
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}

bool is_digit(char16_t c)
{
    return c >= u'0' && c <= u'9';
}

// The two output formats (toString, toUTCString) and the informal
// forms pages write ("Sep 4, 2026", "2026/09/04 10:00", "4 Sep 2026
// 10:00:00 GMT+0100"): words name the month or the zone, numbers fill
// day/year/time in the order they appear.
std::optional<double> parse_informal(std::u16string_view text)
{
    Cursor c { text };
    int month = -1;
    std::vector<double> numbers;
    std::vector<int> number_widths;
    double hour = 0;
    double minute = 0;
    double second = 0;
    double millis = 0;
    bool has_time = false;
    std::optional<double> offset_minutes;
    bool pm = false;
    bool am = false;
    while (!c.done()) {
        char16_t const ch = c.peek();
        if (is_letter(ch)) {
            std::size_t const start = c.pos;
            while (!c.done() && is_letter(c.peek()))
                ++c.pos;
            std::u16string_view const word = text.substr(start, c.pos - start);
            std::u16string lowered;
            for (char16_t const w : word)
                lowered += (w >= u'A' && w <= u'Z') ? static_cast<char16_t>(w + 32) : w;
            if (lowered == u"gmt" || lowered == u"utc" || lowered == u"ut" || lowered == u"z") {
                if (!offset_minutes)
                    offset_minutes = 0;
                continue;
            }
            if (lowered == u"am") {
                am = true;
                continue;
            }
            if (lowered == u"pm") {
                pm = true;
                continue;
            }
            int const m = month_index(word);
            if (m >= 0 && month < 0) {
                month = m;
                continue;
            }
            // A weekday or a zone name in parentheses is skipped.
            continue;
        }
        if (is_digit(ch)) {
            double value = 0;
            int width = 0;
            c.number(value, width);
            if (c.peek() == u':') {
                // A time: hh:mm[:ss[.mmm]]
                has_time = true;
                hour = value;
                ++c.pos;
                int w = 0;
                if (!c.number(minute, w))
                    return std::nullopt;
                if (c.take(u':')) {
                    if (!c.number(second, w))
                        return std::nullopt;
                    if (c.take(u'.')) {
                        double fraction = 0;
                        if (!c.number(fraction, w))
                            return std::nullopt;
                        millis = std::floor(fraction / std::pow(10.0, w - 3));
                    }
                }
                continue;
            }
            numbers.push_back(value);
            number_widths.push_back(width);
            continue;
        }
        if ((ch == u'+' || ch == u'-') && (has_time || offset_minutes) && c.pos + 1 < text.size() && is_digit(text[c.pos + 1])) {
            bool const negative = ch == u'-';
            ++c.pos;
            double value = 0;
            int width = 0;
            c.number(value, width);
            double oh = 0;
            double om = 0;
            if (c.take(u':')) {
                oh = value;
                int w = 0;
                if (!c.number(om, w))
                    return std::nullopt;
            } else if (width <= 2) {
                oh = value;
            } else {
                oh = std::floor(value / 100);
                om = std::fmod(value, 100);
            }
            offset_minutes = (oh * 60 + om) * (negative ? -1 : 1);
            continue;
        }
        if (ch == u'(') {
            while (!c.done() && c.peek() != u')')
                ++c.pos;
            c.take(u')');
            continue;
        }
        ++c.pos; // separators: space, comma, slash, dash, dot
    }
    double year = nan;
    double date = 1;
    if (month >= 0) {
        // Month by name: the remaining numbers are day and year in the
        // order written, the year being the wider one.
        for (std::size_t k = 0; k < numbers.size(); ++k) {
            if (number_widths[k] >= 3 || numbers[k] > 31) {
                year = numbers[k];
            } else if (date == 1 && std::isnan(year) && k + 1 < numbers.size()) {
                date = numbers[k];
            } else if (date == 1) {
                date = numbers[k];
            }
        }
        if (numbers.size() == 1 && number_widths[0] <= 2)
            return std::nullopt;
    } else if (numbers.size() >= 3) {
        // Numeric: YYYY/MM/DD when the first is wide, else MM/DD/YYYY.
        if (number_widths[0] >= 3) {
            year = numbers[0];
            month = static_cast<int>(numbers[1]) - 1;
            date = numbers[2];
        } else {
            month = static_cast<int>(numbers[0]) - 1;
            date = numbers[1];
            year = numbers[2];
        }
    } else {
        return std::nullopt;
    }
    if (std::isnan(year) || month < 0 || month > 11 || date < 1 || date > 31)
        return std::nullopt;
    if (year >= 0 && year <= 49 && number_widths.size() > 0)
        year += 2000;
    else if (year >= 50 && year <= 99)
        year += 1900;
    if (pm && hour < 12)
        hour += 12;
    if (am && hour == 12)
        hour = 0;
    if (hour > 24 || minute > 59 || second > 59)
        return std::nullopt;
    double const result = make_date(make_day(year, month, date), make_time(hour, minute, second, millis));
    if (offset_minutes)
        return time_clip(result - *offset_minutes * ms_per_minute);
    return time_clip(utc_time(result));
}

// A string shaped like the ISO format — a year, then a dash, a T or the
// end — is the ISO format or nothing: the informal parser must not
// rescue "2026-13-01" or "-000000-01-01".
bool has_iso_shape(std::u16string_view text)
{
    std::size_t pos = 0;
    if (pos < text.size() && (text[pos] == u'+' || text[pos] == u'-'))
        ++pos;
    std::size_t digits = 0;
    while (pos < text.size() && is_digit(text[pos])) {
        ++pos;
        ++digits;
    }
    if (digits < 4)
        return false;
    return pos == text.size() || text[pos] == u'-' || text[pos] == u'T';
}

double parse_date(std::u16string_view text)
{
    std::u16string_view const trimmed = trim_string(text);
    if (std::optional<double> const iso = parse_iso(trimmed))
        return *iso;
    if (has_iso_shape(trimmed))
        return nan;
    if (std::optional<double> const informal = parse_informal(trimmed))
        return *informal;
    return nan;
}

// ------------------------------------------------------------ the class

std::optional<double> this_time_value(Interpreter& in, Value const& this_value)
{
    if (!this_value.is_object() || this_value.as_object()->class_id() != Object::Class::Date)
        return in.throw_type_error("this is not a Date object.");
    return static_cast<DateObject*>(this_value.as_object())->time_value();
}

DateObject& this_date(Value const& this_value)
{
    return *static_cast<DateObject*>(this_value.as_object());
}

// The field getters (§21.4.4.2–.19 and B.2.3.1), local or UTC.
void define_getter(Interpreter& in, Object& prototype, std::string_view name, bool local, double (*field)(double))
{
    define_method(in, prototype, name, 0, [local, field](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<double> const t = this_time_value(interp, this_value);
        if (!t)
            return std::nullopt;
        if (std::isnan(*t))
            return Value::number(nan);
        return Value::number(field(local ? local_time(*t) : *t));
    });
}

// The argument coercions of the setters, in order, stopping at a throw.
std::optional<bool> coerce(Interpreter& in, Args args, std::size_t count, std::vector<double>& out)
{
    for (std::size_t k = 0; k < std::min(count, args.size()); ++k) {
        std::optional<double> const number = in.to_number(args[k]);
        if (!number)
            return std::nullopt;
        out.push_back(*number);
    }
    return true;
}

double with_default(std::vector<double> const& values, std::size_t index, double fallback)
{
    return index < values.size() ? values[index] : fallback;
}

// Date(...) as a function or a constructor (§21.4.2.1).
std::optional<Value> construct_date(Interpreter& in, Args args, Object* new_target)
{
    Interpreter::Roots const roots(in);
    for (Value const& arg : args)
        in.root(arg);
    double tv = nan;
    if (args.empty()) {
        tv = current_time_ms();
    } else if (args.size() == 1) {
        Value const value = args[0];
        if (value.is_object() && value.as_object()->class_id() == Object::Class::Date) {
            tv = static_cast<DateObject*>(value.as_object())->time_value();
        } else {
            std::optional<Value> const primitive = in.to_primitive(value);
            if (!primitive)
                return std::nullopt;
            in.root(*primitive);
            if (primitive->is_string()) {
                tv = parse_date(primitive->as_string()->view());
            } else {
                std::optional<double> const number = in.to_number(*primitive);
                if (!number)
                    return std::nullopt;
                tv = time_clip(*number);
            }
        }
    } else {
        std::vector<double> fields;
        if (!coerce(in, args, 7, fields))
            return std::nullopt;
        double year = fields[0];
        if (!std::isnan(year)) {
            double const integer = Interpreter::to_integer_or_infinity(year);
            if (integer >= 0 && integer <= 99)
                year = 1900 + integer;
        }
        double const final_date = make_date(make_day(year, fields[1], with_default(fields, 2, 1)),
            make_time(with_default(fields, 3, 0), with_default(fields, 4, 0), with_default(fields, 5, 0), with_default(fields, 6, 0)));
        tv = time_clip(utc_time(final_date));
    }
    std::optional<Object*> const prototype = in.get_prototype_from_constructor(new_target, in.intrinsics().date_prototype);
    if (!prototype)
        return std::nullopt;
    return Value::object(in.heap().allocate<DateObject>(*prototype, tv));
}

} // namespace

// ------------------------------------------------------------- the clock

double current_time_ms()
{
    if (g_now)
        return g_now();
    auto const since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return std::floor(static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count()));
}

double local_time_zone_offset_minutes(double utc_ms)
{
    if (g_offset)
        return g_offset(utc_ms);
    if (!std::isfinite(utc_ms))
        return 0;
    // The runtime converts a bounded range of instants; outside it, the
    // same date in a year of the same leap kind stands in, which is what
    // §21.4.1.25's note allows for years the zone database does not cover.
    double year = year_from_time(utc_ms);
    double probe = utc_ms;
    if (year < 1971 || year > 2036) {
        double const equivalent = days_in_year(year) == 366 ? 2024 : 2023;
        probe = make_date(make_day(equivalent, month_from_time(utc_ms), date_from_time(utc_ms)), time_within_day(utc_ms));
    }
    auto const seconds = static_cast<std::time_t>(std::floor(probe / 1000));
    std::tm local {};
#ifdef _WIN32
    if (localtime_s(&local, &seconds) != 0)
        return 0;
    std::time_t const as_utc = _mkgmtime(&local);
#else
    if (localtime_r(&seconds, &local) == nullptr)
        return 0;
    std::time_t const as_utc = timegm(&local);
#endif
    if (as_utc == static_cast<std::time_t>(-1))
        return 0;
    return static_cast<double>(as_utc - seconds) / 60.0;
}

void set_time_source(double (*now)(), double (*offset)(double))
{
    g_now = now;
    g_offset = offset;
}

// ------------------------------------------------------------- install

void install_date(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    Heap::NoCollect const guard(in.heap());
    Object& prototype = *i.date_prototype;
    NativeFunction* constructor = in.new_native(
        "Date", 7,
        [](Interpreter& interp, Value const&, Args) -> std::optional<Value> {
            // Called as a function: the current time as a string, whatever
            // the arguments (§21.4.2.1 step 1).
            return Value::string(interp.string(std::string_view(to_date_string(current_time_ms()))));
        },
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> { return construct_date(interp, args, new_target); });
    i.date_constructor = constructor;
    constructor->put(PropertyKey::atom(in.atoms().prototype), Value::object(&prototype), frozen_attributes);
    prototype.put(PropertyKey::atom(in.atoms().constructor), Value::object(constructor), builtin_attributes);
    in.global()->put(in.key("Date"), Value::object(constructor), builtin_attributes);

    define_method(in, *constructor, "now", 0, [](Interpreter&, Value const&, Args) -> std::optional<Value> {
        return Value::number(current_time_ms());
    });
    define_method(in, *constructor, "parse", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<JsString*> const text = interp.to_string(argument(args, 0));
        if (!text)
            return std::nullopt;
        return Value::number(parse_date((*text)->view()));
    });
    define_method(in, *constructor, "UTC", 7, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        // §21.4.3.4.
        std::vector<double> fields;
        if (!coerce(interp, args, 7, fields))
            return std::nullopt;
        double year = with_default(fields, 0, nan);
        if (!std::isnan(year)) {
            double const integer = Interpreter::to_integer_or_infinity(year);
            if (integer >= 0 && integer <= 99)
                year = 1900 + integer;
        }
        return Value::number(time_clip(make_date(make_day(year, with_default(fields, 1, 0), with_default(fields, 2, 1)),
            make_time(with_default(fields, 3, 0), with_default(fields, 4, 0), with_default(fields, 5, 0), with_default(fields, 6, 0)))));
    });

    define_getter(in, prototype, "getDate", true, date_from_time);
    define_getter(in, prototype, "getDay", true, week_day);
    define_getter(in, prototype, "getFullYear", true, year_from_time);
    define_getter(in, prototype, "getHours", true, hour_from_time);
    define_getter(in, prototype, "getMilliseconds", true, ms_from_time);
    define_getter(in, prototype, "getMinutes", true, min_from_time);
    define_getter(in, prototype, "getMonth", true, month_from_time);
    define_getter(in, prototype, "getSeconds", true, sec_from_time);
    define_getter(in, prototype, "getUTCDate", false, date_from_time);
    define_getter(in, prototype, "getUTCDay", false, week_day);
    define_getter(in, prototype, "getUTCFullYear", false, year_from_time);
    define_getter(in, prototype, "getUTCHours", false, hour_from_time);
    define_getter(in, prototype, "getUTCMilliseconds", false, ms_from_time);
    define_getter(in, prototype, "getUTCMinutes", false, min_from_time);
    define_getter(in, prototype, "getUTCMonth", false, month_from_time);
    define_getter(in, prototype, "getUTCSeconds", false, sec_from_time);
    define_getter(in, prototype, "getYear", true, [](double t) { return year_from_time(t) - 1900; });
    define_method(in, prototype, "getTime", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<double> const t = this_time_value(interp, this_value);
        if (!t)
            return std::nullopt;
        return Value::number(*t);
    });
    define_method(in, prototype, "getTimezoneOffset", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<double> const t = this_time_value(interp, this_value);
        if (!t)
            return std::nullopt;
        if (std::isnan(*t))
            return Value::number(nan);
        return Value::number(-local_time_zone_offset_minutes(*t));
    });

    // The setters share one shape (§21.4.4.20–35): the arguments coerce
    // in order, a NaN date stays NaN (setFullYear excepted), and the new
    // fields rebuild the time value.
    struct Setter {
        std::string_view name;
        int length;
        bool local;
        int first_field; // 0 year, 1 month, 2 date, 3 hours, 4 minutes, 5 seconds, 6 ms
        int field_count;
    };
    constexpr Setter setters[] = {
        { "setDate", 1, true, 2, 1 }, { "setFullYear", 3, true, 0, 3 }, { "setHours", 4, true, 3, 4 },
        { "setMilliseconds", 1, true, 6, 1 }, { "setMinutes", 3, true, 4, 3 }, { "setMonth", 2, true, 1, 2 },
        { "setSeconds", 2, true, 5, 2 }, { "setUTCDate", 1, false, 2, 1 }, { "setUTCFullYear", 3, false, 0, 3 },
        { "setUTCHours", 4, false, 3, 4 }, { "setUTCMilliseconds", 1, false, 6, 1 }, { "setUTCMinutes", 3, false, 4, 3 },
        { "setUTCMonth", 2, false, 1, 2 }, { "setUTCSeconds", 2, false, 5, 2 },
    };
    for (Setter const& setter : setters) {
        bool const local = setter.local;
        int const first = setter.first_field;
        int const count = setter.field_count;
        define_method(in, prototype, setter.name, setter.length, [local, first, count](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            std::optional<double> const tv = this_time_value(interp, this_value);
            if (!tv)
                return std::nullopt;
            std::vector<double> given;
            if (!coerce(interp, args, static_cast<std::size_t>(count), given))
                return std::nullopt;
            double t = *tv;
            if (std::isnan(t)) {
                if (first != 0) {
                    this_date(this_value).set_time_value(nan);
                    return Value::number(nan);
                }
                t = 0;
            } else if (local) {
                t = local_time(t);
            }
            double fields[7] = { year_from_time(t), month_from_time(t), date_from_time(t), hour_from_time(t), min_from_time(t), sec_from_time(t), ms_from_time(t) };
            if (given.empty())
                fields[first] = nan;
            for (std::size_t k = 0; k < given.size(); ++k)
                fields[static_cast<std::size_t>(first) + k] = given[k];
            double const rebuilt = make_date(make_day(fields[0], fields[1], fields[2]), make_time(fields[3], fields[4], fields[5], fields[6]));
            double const result = time_clip(local ? utc_time(rebuilt) : rebuilt);
            this_date(this_value).set_time_value(result);
            return Value::number(result);
        });
    }
    define_method(in, prototype, "setTime", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        if (!this_time_value(interp, this_value))
            return std::nullopt;
        std::optional<double> const t = interp.to_number(argument(args, 0));
        if (!t)
            return std::nullopt;
        double const clipped = time_clip(*t);
        this_date(this_value).set_time_value(clipped);
        return Value::number(clipped);
    });
    define_method(in, prototype, "setYear", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // B.2.3.2.
        std::optional<double> const tv = this_time_value(interp, this_value);
        if (!tv)
            return std::nullopt;
        std::optional<double> const y = interp.to_number(argument(args, 0));
        if (!y)
            return std::nullopt;
        double const t = std::isnan(*tv) ? 0 : local_time(*tv);
        if (std::isnan(*y)) {
            this_date(this_value).set_time_value(nan);
            return Value::number(nan);
        }
        double const yi = Interpreter::to_integer_or_infinity(*y);
        double const yyyy = (yi >= 0 && yi <= 99) ? yi + 1900 : *y;
        double const result = time_clip(utc_time(make_date(make_day(yyyy, month_from_time(t), date_from_time(t)), time_within_day(t))));
        this_date(this_value).set_time_value(result);
        return Value::number(result);
    });

    auto const define_text = [&](std::string_view name, std::string (*format)(double), bool invalid_throws) {
        define_method(in, prototype, name, 0, [format, invalid_throws](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
            std::optional<double> const t = this_time_value(interp, this_value);
            if (!t)
                return std::nullopt;
            if (std::isnan(*t)) {
                if (invalid_throws)
                    return interp.throw_range_error("Invalid time value");
                return Value::string(interp.string(std::string_view("Invalid Date")));
            }
            return Value::string(interp.string(std::string_view(format(*t))));
        });
    };
    define_text("toDateString", [](double t) { return date_string(local_time(t)); }, false);
    define_text("toISOString", to_iso_string, true);
    define_text("toLocaleDateString", [](double t) { return date_string(local_time(t)); }, false);
    define_text("toLocaleString", to_date_string, false);
    define_text("toLocaleTimeString", [](double t) { return time_string(local_time(t)) + time_zone_string(t); }, false);
    define_text("toString", to_date_string, false);
    define_text("toTimeString", [](double t) { return time_string(local_time(t)) + time_zone_string(t); }, false);
    NativeFunction* utc_string = define_method(in, prototype, "toUTCString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<double> const t = this_time_value(interp, this_value);
        if (!t)
            return std::nullopt;
        if (std::isnan(*t))
            return Value::string(interp.string(std::string_view("Invalid Date")));
        return Value::string(interp.string(std::string_view(to_utc_string(*t))));
    });
    prototype.put(in.key("toGMTString"), Value::object(utc_string), builtin_attributes); // B.2.3.3: the same function
    define_method(in, prototype, "toJSON", 1, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §21.4.4.37: null for a non-finite time, else toISOString.
        Interpreter::Roots const roots(interp);
        std::optional<Object*> const object = interp.to_object(this_value);
        if (!object)
            return std::nullopt;
        interp.root(Value::object(*object));
        std::optional<Value> const tv = interp.to_primitive(Value::object(*object), PreferredType::Number);
        if (!tv)
            return std::nullopt;
        if (tv->is_number() && !std::isfinite(tv->as_number()))
            return Value::null();
        return interp.invoke(Value::object(*object), interp.key("toISOString"), {});
    });
    define_method(in, prototype, "valueOf", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<double> const t = this_time_value(interp, this_value);
        if (!t)
            return std::nullopt;
        return Value::number(*t);
    });
    {
        // §21.4.4.45: Date's @@toPrimitive prefers a string for "default".
        NativeFunction* to_primitive = in.new_native("[Symbol.toPrimitive]", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            if (!this_value.is_object())
                return interp.throw_type_error("Date.prototype[Symbol.toPrimitive] called on non-object");
            Value const hint = argument(args, 0);
            if (!hint.is_string())
                return interp.throw_type_error("Invalid hint: " + interp.describe(hint));
            std::u16string_view const text = hint.as_string()->view();
            PreferredType preferred;
            if (text == u"string" || text == u"default")
                preferred = PreferredType::String;
            else if (text == u"number")
                preferred = PreferredType::Number;
            else
                return interp.throw_type_error("Invalid hint: " + hint.as_string()->to_utf8());
            return interp.ordinary_to_primitive(*this_value.as_object(), preferred);
        });
        prototype.put(PropertyKey::symbol(in.atoms().symbol_to_primitive), Value::object(to_primitive), Configurable);
    }
}

}
