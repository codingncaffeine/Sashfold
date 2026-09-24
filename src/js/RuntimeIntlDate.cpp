#include "js/Runtime.h"

// Intl.DateTimeFormat (ECMA-402 section 11) and the locale-sensitive methods of
// Date (section 20.4): toLocaleString, toLocaleDateString, toLocaleTimeString.
//
// The Gregorian calendar and Latin digits only. Time zones: UTC and its
// aliases, the offset zones (+05:30), Etc/GMT+/-N, and the system's own zone
// by its IANA name (TZ, or the /etc/localtime link), whose offsets come
// from the platform as Date's do. Where the platform cannot name its zone
// (Windows), the system zone is reported, and formatted, as UTC. Any other
// IANA name is the RangeError section 11.1.2 gives an unsupported zone.
//
// A format is a list of fields and literals made once, at construction,
// from the resolved components the English way (en-US month first and a
// 12-hour clock, en-GB day first and a 24-hour clock).

#include "js/Intl.h"
#include "js/IntlData.h"
#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

namespace {

std::string read_system_time_zone_name()
{
#if defined(_WIN32)
    // The platform layer has no IANA name to give here.
    return "UTC";
#else
    auto from_path = [](std::string const& path) -> std::string {
        std::size_t const at = path.find("zoneinfo/");
        if (at == std::string::npos)
            return {};
        return path.substr(at + 9);
    };
    auto plausible = [](std::string const& name) {
        return !name.empty() && name.find('/') != std::string::npos && name.find("..") == std::string::npos;
    };
    if (char const* tz = std::getenv("TZ")) {
        std::string name = tz;
        if (!name.empty() && name[0] == ':')
            name.erase(0, 1);
        if (name.starts_with("/"))
            name = from_path(name);
        if (name == "UTC" || name == "Etc/UTC" || plausible(name))
            return name == "Etc/UTC" ? "UTC" : name;
        if (!name.empty())
            return "UTC";
    }
    std::error_code error;
    std::filesystem::path const target = std::filesystem::read_symlink("/etc/localtime", error);
    if (!error) {
        std::string const name = from_path(target.string());
        if (name == "UTC" || name == "Etc/UTC" || name == "Universal" || name == "Zulu")
            return "UTC";
        if (plausible(name))
            return name;
    }
    return "UTC";
#endif
}

} // namespace

// Read once: a format made in a loop (toLocaleString) must not touch the
// file system each time, and engines keep the zone until told otherwise.
std::string system_time_zone_name()
{
    static std::string const name = read_system_time_zone_name();
    return name;
}

namespace {

constexpr double ms_per_day = 86400000.0;

// ---------------------------------------------------------------- zones

enum class ZoneKind : std::uint8_t { Utc, Offset, System };

struct Zone {
    ZoneKind kind = ZoneKind::Utc;
    std::string name; // the canonical identifier resolvedOptions reports
    int offset_minutes = 0; // for an offset zone
};

std::string lower_ascii(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool all_digits(std::string_view s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// IsTimeZoneOffsetString for an Intl zone: +/-HH, +/-HHMM or +/-HH:MM, hours
// below 24 and minutes below 60.
std::optional<int> parse_offset_zone(std::string_view text)
{
    if (text.size() < 3 || (text[0] != '+' && text[0] != '-'))
        return std::nullopt;
    int const sign = text[0] == '-' ? -1 : 1;
    std::string_view rest = text.substr(1);
    std::string_view hours;
    std::string_view minutes = "00";
    if (rest.size() == 2) {
        hours = rest;
    } else if (rest.size() == 4) {
        hours = rest.substr(0, 2);
        minutes = rest.substr(2);
    } else if (rest.size() == 5 && rest[2] == ':') {
        hours = rest.substr(0, 2);
        minutes = rest.substr(3);
    } else {
        return std::nullopt;
    }
    if (!all_digits(hours) || !all_digits(minutes))
        return std::nullopt;
    int const h = (hours[0] - '0') * 10 + (hours[1] - '0');
    int const m = (minutes[0] - '0') * 10 + (minutes[1] - '0');
    if (h > 23 || m > 59)
        return std::nullopt;
    return sign * (h * 60 + m);
}

std::string format_offset(int minutes, bool colon = true)
{
    std::string out = minutes < 0 ? "-" : "+";
    int const a = std::abs(minutes);
    int const h = a / 60;
    int const m = a % 60;
    out += static_cast<char>('0' + h / 10);
    out += static_cast<char>('0' + h % 10);
    if (colon)
        out += ':';
    out += static_cast<char>('0' + m / 10);
    out += static_cast<char>('0' + m % 10);
    return out;
}

std::optional<Zone> resolve_zone(std::string const& requested)
{
    if (std::optional<int> const offset = parse_offset_zone(requested)) {
        Zone zone;
        zone.kind = ZoneKind::Offset;
        zone.offset_minutes = *offset;
        zone.name = format_offset(*offset);
        return zone;
    }
    std::string const lower = lower_ascii(requested);
    // The names of UTC, kept as written but for case (ECMA-402 no longer
    // folds a zone's links into their primary name).
    static constexpr std::string_view utc_names[] = { "UTC", "Etc/UTC", "Etc/GMT", "GMT", "Etc/UCT", "UCT", "Etc/Universal",
        "Universal", "Etc/Zulu", "Zulu", "Etc/Greenwich", "Greenwich", "Etc/GMT0", "GMT0", "GMT+0", "GMT-0",
        "Etc/GMT+0", "Etc/GMT-0" };
    for (std::string_view name : utc_names) {
        if (lower == lower_ascii(name)) {
            Zone zone;
            zone.name = std::string(name);
            return zone;
        }
    }
    // Etc/GMT+N is N hours west of Greenwich; Etc/GMT-N east.
    if (lower.starts_with("etc/gmt") && lower.size() > 8 && (lower[7] == '+' || lower[7] == '-') && all_digits(lower.substr(8))
        && lower.size() <= 10 && !(lower.size() == 10 && lower[8] == '0')) {
        int const n = std::stoi(lower.substr(8));
        bool const west = lower[7] == '+';
        if ((west && n <= 12) || (!west && n <= 14)) {
            Zone zone;
            zone.kind = ZoneKind::Offset;
            zone.offset_minutes = (west ? -n : n) * 60;
            zone.name = std::string("Etc/GMT") + lower[7] + std::to_string(n);
            return zone;
        }
    }
    std::string const system = system_time_zone_name();
    if (system != "UTC" && lower == lower_ascii(system)) {
        Zone zone;
        zone.kind = ZoneKind::System;
        zone.name = system;
        return zone;
    }
    return std::nullopt;
}

Zone default_zone()
{
    std::string const system = system_time_zone_name();
    Zone zone;
    if (system != "UTC") {
        zone.kind = ZoneKind::System;
        zone.name = system;
    } else {
        zone.name = "UTC";
    }
    return zone;
}

int zone_offset_at(Zone const& zone, double utc_ms)
{
    switch (zone.kind) {
    case ZoneKind::Utc: return 0;
    case ZoneKind::Offset: return zone.offset_minutes;
    case ZoneKind::System: return static_cast<int>(local_time_zone_offset_minutes(utc_ms));
    }
    return 0;
}

// ---------------------------------------------------------------- dates

struct Fields {
    double year = 0; // proleptic, astronomical (1 BC = 0)
    int month = 0; // 0-11
    int day = 1;
    int weekday = 0; // 0 = Sunday
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millisecond = 0;
    int offset_minutes = 0;
};

Fields fields_of(double utc_ms, Zone const& zone)
{
    Fields f;
    f.offset_minutes = zone_offset_at(zone, utc_ms);
    double const t = utc_ms + f.offset_minutes * 60000.0;
    double const days = std::floor(t / ms_per_day);
    double ms_in_day = t - days * ms_per_day;
    f.weekday = static_cast<int>(std::fmod(std::fmod(days + 4, 7) + 7, 7));
    // Civil from days (the proleptic Gregorian calendar).
    double const z = days + 719468;
    double const era = std::floor(z / 146097);
    double const doe = z - era * 146097;
    double const yoe = std::floor((doe - std::floor(doe / 1460) + std::floor(doe / 36524) - std::floor(doe / 146096)) / 365);
    double const doy = doe - (365 * yoe + std::floor(yoe / 4) - std::floor(yoe / 100));
    double const mp = std::floor((5 * doy + 2) / 153);
    double const d = doy - std::floor((153 * mp + 2) / 5) + 1;
    double const m = mp < 10 ? mp + 3 : mp - 9;
    f.year = yoe + era * 400 + (m <= 2 ? 1 : 0);
    f.month = static_cast<int>(m) - 1;
    f.day = static_cast<int>(d);
    f.hour = static_cast<int>(std::floor(ms_in_day / 3600000.0));
    ms_in_day -= f.hour * 3600000.0;
    f.minute = static_cast<int>(std::floor(ms_in_day / 60000.0));
    ms_in_day -= f.minute * 60000.0;
    f.second = static_cast<int>(std::floor(ms_in_day / 1000.0));
    f.millisecond = static_cast<int>(ms_in_day - f.second * 1000.0);
    return f;
}

// --------------------------------------------------------------- formats

enum class Field : std::uint8_t { Literal, Weekday, Era, Year, Month, Day, AmPm, DayPeriod, Hour, Minute, Second, FractionalSecond, TimeZoneName };

struct Token {
    Field field = Field::Literal;
    std::string text; // a literal's text, or the field's width
};

struct DateTimeFormatData {
    static constexpr IntlKind kind = IntlKind::DateTimeFormat;
    std::string locale;
    std::string calendar = "gregory";
    std::string numbering_system = "latn";
    Zone zone;
    bool british = false;
    std::string hour_cycle; // "" when the format shows no hour
    std::string weekday, era, year, month, day, day_period, hour, minute, second, time_zone_name;
    int fractional_second_digits = 0;
    std::string date_style, time_style;
    std::vector<Token> pattern;
};

void literal(std::vector<Token>& out, std::string_view text)
{
    if (!out.empty() && out.back().field == Field::Literal)
        out.back().text += text;
    else
        out.push_back({ Field::Literal, std::string(text) });
}

void field(std::vector<Token>& out, Field f, std::string const& width)
{
    out.push_back({ f, width });
}

bool is_text_month(std::string const& month)
{
    return month == "short" || month == "long" || month == "narrow";
}

std::vector<Token> date_pattern(DateTimeFormatData const& d)
{
    std::vector<Token> out;
    bool const w = !d.weekday.empty();
    bool const y = !d.year.empty();
    bool const m = !d.month.empty();
    bool const dd = !d.day.empty();
    if (m && is_text_month(d.month)) {
        if (d.british) {
            if (w) {
                field(out, Field::Weekday, d.weekday);
                literal(out, y ? ", " : " ");
            }
            if (dd) {
                field(out, Field::Day, d.day);
                literal(out, " ");
            }
            field(out, Field::Month, d.month);
            if (y) {
                literal(out, " ");
                field(out, Field::Year, d.year);
            }
        } else {
            if (w) {
                field(out, Field::Weekday, d.weekday);
                literal(out, ", ");
            }
            field(out, Field::Month, d.month);
            if (dd) {
                literal(out, " ");
                field(out, Field::Day, d.day);
            }
            if (y) {
                literal(out, dd ? ", " : " ");
                field(out, Field::Year, d.year);
            }
        }
    } else if (m) {
        if (w) {
            field(out, Field::Weekday, d.weekday);
            literal(out, ", ");
        }
        if (d.british) {
            if (dd) {
                field(out, Field::Day, d.day);
                literal(out, "/");
            }
            field(out, Field::Month, d.month);
        } else {
            field(out, Field::Month, d.month);
            if (dd) {
                literal(out, "/");
                field(out, Field::Day, d.day);
            }
        }
        if (y) {
            literal(out, "/");
            field(out, Field::Year, d.year);
        }
    } else {
        // No month: a year, a day, a weekday, alone or together.
        if (w && dd) {
            if (d.british) {
                field(out, Field::Weekday, d.weekday);
                literal(out, " ");
                field(out, Field::Day, d.day);
            } else {
                field(out, Field::Day, d.day);
                literal(out, " ");
                field(out, Field::Weekday, d.weekday);
            }
        } else if (w) {
            field(out, Field::Weekday, d.weekday);
        } else if (dd) {
            field(out, Field::Day, d.day);
        }
        if (y) {
            if (!out.empty())
                literal(out, " ");
            field(out, Field::Year, d.year);
        }
    }
    if (!d.era.empty()) {
        if (!out.empty())
            literal(out, " ");
        field(out, Field::Era, d.era);
    }
    return out;
}

std::vector<Token> time_pattern(DateTimeFormatData const& d)
{
    std::vector<Token> out;
    bool const twelve = d.hour_cycle == "h11" || d.hour_cycle == "h12";
    if (!d.hour.empty()) {
        field(out, Field::Hour, d.hour);
        if (!d.minute.empty()) {
            literal(out, ":");
            field(out, Field::Minute, d.minute);
        }
        if (!d.second.empty()) {
            literal(out, ":");
            field(out, Field::Second, d.second);
        }
        if (d.fractional_second_digits > 0) {
            literal(out, utf8_from_utf16(decimal_separator(d.numbering_system)));
            field(out, Field::FractionalSecond, std::to_string(d.fractional_second_digits));
        }
        if (twelve) {
            literal(out, " ");
            if (!d.day_period.empty())
                field(out, Field::DayPeriod, d.day_period);
            else
                field(out, Field::AmPm, "short");
        }
    } else {
        if (!d.minute.empty()) {
            field(out, Field::Minute, d.minute);
            if (!d.second.empty())
                literal(out, ":");
        }
        if (!d.second.empty())
            field(out, Field::Second, d.second);
        if (d.fractional_second_digits > 0) {
            if (!out.empty())
                literal(out, utf8_from_utf16(decimal_separator(d.numbering_system)));
            field(out, Field::FractionalSecond, std::to_string(d.fractional_second_digits));
        }
        if (!d.day_period.empty()) {
            if (!out.empty())
                literal(out, " ");
            field(out, Field::DayPeriod, d.day_period);
        }
    }
    return out;
}

std::vector<Token> make_pattern(DateTimeFormatData const& d, std::string_view join)
{
    std::vector<Token> date = date_pattern(d);
    std::vector<Token> const time = time_pattern(d);
    std::vector<Token> out = date;
    if (!date.empty() && !time.empty())
        literal(out, join);
    for (Token const& token : time) {
        if (token.field == Field::Literal)
            literal(out, token.text);
        else
            out.push_back(token);
    }
    if (!d.time_zone_name.empty()) {
        literal(out, time.empty() && !date.empty() ? ", " : " ");
        field(out, Field::TimeZoneName, d.time_zone_name);
    }
    return out;
}

std::string two_digits(int value)
{
    std::string out = std::to_string(value % 100);
    if (out.size() < 2)
        out.insert(out.begin(), '0');
    return out;
}

std::string number_text(double value)
{
    return number_to_utf8(value);
}

std::string time_zone_display(Zone const& zone, std::string const& style, int offset_minutes)
{
    if (zone.kind == ZoneKind::Utc && (style == "short" || style == "long"))
        return style == "short" ? "UTC" : "Coordinated Universal Time";
    bool const long_form = style == "long" || style == "longOffset" || style == "longGeneric";
    if (long_form)
        return "GMT" + format_offset(offset_minutes);
    std::string out = "GMT";
    out += offset_minutes < 0 ? "-" : "+";
    int const a = std::abs(offset_minutes);
    out += std::to_string(a / 60);
    if (a % 60 != 0)
        out += ":" + two_digits(a % 60);
    return out;
}

std::string_view flexible_day_period(Fields const& f, std::string const& width)
{
    if (f.hour == 12 && f.minute == 0 && f.second == 0 && f.millisecond == 0)
        return width == "narrow" ? "n" : "noon";
    if (f.hour >= 6 && f.hour < 12)
        return "in the morning";
    if (f.hour >= 12 && f.hour < 18)
        return "in the afternoon";
    if (f.hour >= 18 && f.hour < 21)
        return "in the evening";
    return "at night";
}

std::vector<IntlPart> format_fields(DateTimeFormatData const& d, Fields const& f)
{
    std::vector<IntlPart> parts;
    auto push = [&](char const* type, std::string_view value) {
        std::string_view const t = type;
        bool const numeric = t == "year" || t == "month" || t == "day" || t == "hour" || t == "minute" || t == "second"
            || t == "fractionalSecond";
        std::u16string text = utf16_from_utf8(value);
        if (numeric && d.numbering_system != "latn")
            text = transliterate_digits(d.numbering_system, text);
        parts.push_back({ type, std::move(text), {}, {} });
    };
    for (Token const& token : d.pattern) {
        switch (token.field) {
        case Field::Literal:
            push("literal", token.text);
            break;
        case Field::Weekday:
            push("weekday", token.text == "long" ? intl_data::weekday_long[f.weekday]
                    : token.text == "short"      ? intl_data::weekday_short[f.weekday]
                                                 : intl_data::weekday_narrow[f.weekday]);
            break;
        case Field::Era: {
            std::size_t const era = f.year > 0 ? 1 : 0;
            push("era", token.text == "long" ? intl_data::era_long[era]
                    : token.text == "short"  ? intl_data::era_short[era]
                                             : intl_data::era_narrow[era]);
            break;
        }
        case Field::Year: {
            // The year of the era: 1 BC is year 0.
            double const year = f.year > 0 ? f.year : 1 - f.year;
            if (token.text == "2-digit")
                push("year", two_digits(static_cast<int>(std::fmod(year, 100))));
            else
                push("year", number_text(year));
            break;
        }
        case Field::Month:
            if (token.text == "long")
                push("month", intl_data::month_long[f.month]);
            else if (token.text == "short")
                push("month", intl_data::month_short[f.month]);
            else if (token.text == "narrow")
                push("month", intl_data::month_narrow[f.month]);
            else if (token.text == "2-digit")
                push("month", two_digits(f.month + 1));
            else
                push("month", std::to_string(f.month + 1));
            break;
        case Field::Day:
            push("day", token.text == "2-digit" ? two_digits(f.day) : std::to_string(f.day));
            break;
        case Field::AmPm:
            push("dayPeriod", d.british ? intl_data::am_pm_gb[f.hour >= 12 ? 1 : 0] : intl_data::am_pm[f.hour >= 12 ? 1 : 0]);
            break;
        case Field::DayPeriod:
            push("dayPeriod", flexible_day_period(f, token.text));
            break;
        case Field::Hour: {
            int hour = f.hour;
            if (d.hour_cycle == "h11")
                hour = f.hour % 12;
            else if (d.hour_cycle == "h12")
                hour = f.hour % 12 == 0 ? 12 : f.hour % 12;
            else if (d.hour_cycle == "h24")
                hour = f.hour == 0 ? 24 : f.hour;
            push("hour", token.text == "2-digit" ? two_digits(hour) : std::to_string(hour));
            break;
        }
        case Field::Minute:
            push("minute", token.text == "2-digit" ? two_digits(f.minute) : std::to_string(f.minute));
            break;
        case Field::Second:
            push("second", token.text == "2-digit" ? two_digits(f.second) : std::to_string(f.second));
            break;
        case Field::FractionalSecond: {
            std::string digits = std::to_string(f.millisecond);
            while (digits.size() < 3)
                digits.insert(digits.begin(), '0');
            push("fractionalSecond", digits.substr(0, static_cast<std::size_t>(token.text[0] - '0')));
            break;
        }
        case Field::TimeZoneName:
            push("timeZoneName", time_zone_display(d.zone, token.text, f.offset_minutes));
            break;
        }
    }
    return parts;
}

// ---------------------------------------------------- construction

enum class Required : std::uint8_t { Date, Time, Any };
enum class Defaults : std::uint8_t { Date, Time, All };

std::optional<bool> create_date_time_format(Interpreter& in, DateTimeFormatData& d, Value const& locales,
    Value const& options_value, Required required, Defaults defaults)
{
    // CreateDateTimeFormat (section 11.1.2).
    std::optional<std::vector<std::string>> const requested = canonicalize_locale_list(in, locales);
    if (!requested)
        return std::nullopt;
    std::optional<Object*> const options_object = coerce_options_to_object(in, options_value);
    if (!options_object)
        return std::nullopt;
    Object* const options = *options_object;
    Interpreter::Roots const roots(in);
    if (options)
        in.root(Value::object(options));
    if (!read_locale_matcher(in, options))
        return std::nullopt;
    std::optional<std::optional<std::string>> const calendar = get_string_option(in, options, "calendar");
    if (!calendar)
        return std::nullopt;
    if (*calendar && !is_unicode_type_sequence(**calendar))
        return in.throw_range_error("Invalid calendar : " + **calendar);
    std::optional<std::optional<std::string>> const numbering = get_string_option(in, options, "numberingSystem");
    if (!numbering)
        return std::nullopt;
    if (*numbering && !is_unicode_type_sequence(**numbering))
        return in.throw_range_error("Invalid numberingSystem : " + **numbering);
    std::optional<std::optional<bool>> const hour12 = get_boolean_option(in, options, "hour12");
    if (!hour12)
        return std::nullopt;
    std::optional<std::optional<std::string>> hour_cycle = get_string_option(in, options, "hourCycle", { "h11", "h12", "h23", "h24" });
    if (!hour_cycle)
        return std::nullopt;
    // hour12 makes the option null, which overrides a -u-hc- keyword (""
    // is null among the values below).
    if (*hour12)
        *hour_cycle = std::string();
    std::optional<std::string> calendar_option;
    if (*calendar)
        calendar_option = lower_ascii(**calendar);
    std::optional<std::string> numbering_option;
    if (*numbering)
        numbering_option = lower_ascii(**numbering);
    ResolvedLocale const resolved = resolve_locale(*requested, {
                                                                   { "ca", { "gregory" }, calendar_option },
                                                                   { "hc", { "", "h11", "h12", "h23", "h24" }, *hour_cycle },
                                                                   { "nu", numbering_system_names(), numbering_option },
                                                               });
    d.locale = resolved.locale;
    d.calendar = resolved.value("ca");
    d.numbering_system = resolved.value("nu");
    d.british = resolved.data_locale == "en-GB";
    std::string const resolved_hc = resolved.value("hc");

    // The time zone.
    std::optional<Value> const zone_value = options ? in.get(*options, in.key("timeZone")) : std::optional<Value>(Value::undefined());
    if (!zone_value)
        return std::nullopt;
    if (zone_value->is_undefined()) {
        d.zone = default_zone();
    } else {
        std::optional<JsString*> const zone_string = in.to_string(*zone_value);
        if (!zone_string)
            return std::nullopt;
        std::string const name = (*zone_string)->to_utf8();
        std::optional<Zone> const zone = resolve_zone(name);
        if (!zone)
            return in.throw_range_error("Invalid time zone specified: " + name);
        d.zone = *zone;
    }

    // The components (Table 16), in order.
    bool explicit_components = false;
    auto read_component = [&](char const* name, std::string& out, std::initializer_list<std::string_view> values) -> bool {
        std::optional<std::optional<std::string>> const value = get_string_option(in, options, name, values);
        if (!value)
            return false;
        if (*value) {
            out = **value;
            explicit_components = true;
        }
        return true;
    };
    if (!read_component("weekday", d.weekday, { "narrow", "short", "long" }))
        return std::nullopt;
    if (!read_component("era", d.era, { "narrow", "short", "long" }))
        return std::nullopt;
    if (!read_component("year", d.year, { "2-digit", "numeric" }))
        return std::nullopt;
    if (!read_component("month", d.month, { "2-digit", "numeric", "narrow", "short", "long" }))
        return std::nullopt;
    if (!read_component("day", d.day, { "2-digit", "numeric" }))
        return std::nullopt;
    if (!read_component("dayPeriod", d.day_period, { "narrow", "short", "long" }))
        return std::nullopt;
    if (!read_component("hour", d.hour, { "2-digit", "numeric" }))
        return std::nullopt;
    if (!read_component("minute", d.minute, { "2-digit", "numeric" }))
        return std::nullopt;
    if (!read_component("second", d.second, { "2-digit", "numeric" }))
        return std::nullopt;
    std::optional<std::optional<int>> const fsd = get_number_option(in, options, "fractionalSecondDigits", 1, 3);
    if (!fsd)
        return std::nullopt;
    if (*fsd) {
        d.fractional_second_digits = **fsd;
        explicit_components = true;
    }
    if (!read_component("timeZoneName", d.time_zone_name, { "short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric" }))
        return std::nullopt;
    std::optional<std::optional<std::string>> const matcher = get_string_option(in, options, "formatMatcher", { "basic", "best fit" });
    if (!matcher)
        return std::nullopt;
    std::optional<std::optional<std::string>> const date_style = get_string_option(in, options, "dateStyle", { "full", "long", "medium", "short" });
    if (!date_style)
        return std::nullopt;
    std::optional<std::optional<std::string>> const time_style = get_string_option(in, options, "timeStyle", { "full", "long", "medium", "short" });
    if (!time_style)
        return std::nullopt;

    // The hour cycle the format would use if it shows an hour.
    std::string hc;
    if (*hour12)
        hc = **hour12 ? "h12" : "h23";
    else if (!resolved_hc.empty())
        hc = resolved_hc;
    else
        hc = d.british ? "h23" : "h12";

    std::string join = ", ";
    if (*date_style || *time_style) {
        if (explicit_components)
            return in.throw_type_error("Can't set option " + std::string(*date_style ? "dateStyle" : "timeStyle") + " when other date or time components are set");
        if (required == Required::Date && *time_style)
            return in.throw_type_error("Invalid option : timeStyle");
        if (required == Required::Time && *date_style)
            return in.throw_type_error("Invalid option : dateStyle");
        d.date_style = date_style->value_or("");
        d.time_style = time_style->value_or("");
        // DateTimeStyleFormat: the fields each style stands for.
        DateTimeFormatData shape = d;
        if (*date_style) {
            std::string const& s = **date_style;
            if (s == "full") {
                shape.weekday = "long";
                shape.month = "long";
                shape.day = "numeric";
                shape.year = "numeric";
            } else if (s == "long" || s == "medium") {
                shape.month = s == "long" ? "long" : "short";
                shape.day = "numeric";
                shape.year = "numeric";
            } else if (d.british) {
                shape.day = "2-digit";
                shape.month = "2-digit";
                shape.year = "numeric";
            } else {
                shape.day = "numeric";
                shape.month = "numeric";
                shape.year = "2-digit";
            }
            join = s == "full" || s == "long" ? " at " : ", ";
        }
        if (*time_style) {
            std::string const& s = **time_style;
            shape.hour_cycle = hc;
            shape.hour = hc == "h23" || hc == "h24" ? "2-digit" : "numeric";
            shape.minute = "2-digit";
            if (s != "short")
                shape.second = "2-digit";
            if (s == "full")
                shape.time_zone_name = "long";
            else if (s == "long")
                shape.time_zone_name = "short";
            d.hour_cycle = hc;
        }
        d.pattern = make_pattern(shape, join);
        return true;
    }

    // ToDateTimeOptions's defaults (section 11.1.2 steps 36-39).
    bool need_defaults = true;
    if (required == Required::Date || required == Required::Any)
        if (!d.weekday.empty() || !d.year.empty() || !d.month.empty() || !d.day.empty())
            need_defaults = false;
    if (required == Required::Time || required == Required::Any)
        if (!d.day_period.empty() || !d.hour.empty() || !d.minute.empty() || !d.second.empty() || d.fractional_second_digits > 0)
            need_defaults = false;
    if (need_defaults && (defaults == Defaults::Date || defaults == Defaults::All)) {
        d.year = "numeric";
        d.month = "numeric";
        d.day = "numeric";
    }
    if (need_defaults && (defaults == Defaults::Time || defaults == Defaults::All)) {
        d.hour = "numeric";
        d.minute = "numeric";
        d.second = "numeric";
    }

    // The best fit, the English way: an era alone shows the date, the
    // numeric fields beside others take two digits where the pattern does.
    if (!d.era.empty() && d.year.empty() && d.month.empty() && d.day.empty() && d.weekday.empty()) {
        d.year = "numeric";
        d.month = "numeric";
        d.day = "numeric";
    }
    if (!d.hour.empty()) {
        d.hour_cycle = hc;
        if (hc == "h23" || hc == "h24")
            d.hour = "2-digit";
        if (!d.minute.empty())
            d.minute = "2-digit";
        if (!d.second.empty())
            d.second = "2-digit";
        if (hc == "h23" || hc == "h24")
            d.day_period.clear();
    } else if (!d.minute.empty() && !d.second.empty()) {
        d.minute = "2-digit";
        d.second = "2-digit";
    }
    if (d.british && !d.month.empty() && !is_text_month(d.month)) {
        d.month = "2-digit";
        if (!d.day.empty())
            d.day = "2-digit";
    }
    if (d.month == "long" && (!d.hour.empty() || !d.minute.empty() || !d.second.empty()))
        join = " at ";
    else if (d.month.empty() && d.day.empty() && d.year.empty() && !d.weekday.empty() && !d.british)
        join = " ";
    d.pattern = make_pattern(d, join);
    return true;
}

std::optional<Value> date_time_format_construct(Interpreter& in, Args args, Object* new_target)
{
    Interpreter::Roots const roots(in);
    in.root(Value::object(new_target));
    std::optional<IntlObjectOf<DateTimeFormatData>*> const object = allocate_intl<DateTimeFormatData>(in, new_target);
    if (!object)
        return std::nullopt;
    in.root(Value::object(*object));
    if (!create_date_time_format(in, (*object)->data, argument(args, 0), argument(args, 1), Required::Any, Defaults::Date))
        return std::nullopt;
    return Value::object(*object);
}

// UnwrapDateTimeFormat (section 11.5.1).
std::optional<IntlObjectOf<DateTimeFormatData>*> unwrap_date_time_format(Interpreter& in, Value const& this_value, std::string_view method)
{
    if (auto* dtf = intl_cast<DateTimeFormatData>(this_value))
        return dtf;
    if (this_value.is_object()) {
        Function* constructor = in.intrinsics().intl_constructors[static_cast<std::size_t>(IntlKind::DateTimeFormat)];
        std::optional<bool> const inherits = in.ordinary_has_instance(Value::object(constructor), this_value);
        if (!inherits)
            return std::nullopt;
        if (*inherits) {
            std::optional<Value> const inner = in.get(*this_value.as_object(), PropertyKey::symbol(in.intrinsics().intl_fallback_symbol));
            if (!inner)
                return std::nullopt;
            return intl_this<DateTimeFormatData>(in, *inner, method);
        }
    }
    return intl_this<DateTimeFormatData>(in, this_value, method);
}

// The time value a format method is given: now, or ToNumber, clipped.
std::optional<double> date_value(Interpreter& in, Value const& date)
{
    double x = 0;
    if (date.is_undefined()) {
        x = current_time_ms();
    } else {
        std::optional<double> const number = in.to_number(date);
        if (!number)
            return std::nullopt;
        x = *number;
    }
    if (!std::isfinite(x) || std::fabs(x) > 8.64e15)
        return in.throw_range_error("Invalid time value");
    return std::trunc(x) + 0.0;
}

std::optional<std::vector<IntlPart>> format_parts(Interpreter& in, DateTimeFormatData const& d, Value const& date)
{
    std::optional<double> const x = date_value(in, date);
    if (!x)
        return std::nullopt;
    return format_fields(d, fields_of(*x, d.zone));
}

bool is_date_type(std::string const& type)
{
    return type == "weekday" || type == "era" || type == "year" || type == "month" || type == "day";
}

std::optional<std::vector<IntlPart>> format_range_parts(Interpreter& in, DateTimeFormatData const& d, Value const& start, Value const& end)
{
    // PartitionDateTimeRangePattern (section 11.5.9).
    if (start.is_undefined() || end.is_undefined())
        return in.throw_type_error("startDate or endDate is undefined");
    Interpreter::Roots const roots(in);
    in.root(end);
    std::optional<double> const x = date_value(in, start);
    if (!x)
        return std::nullopt;
    std::optional<double> const y = date_value(in, end);
    if (!y)
        return std::nullopt;
    std::vector<IntlPart> a = format_fields(d, fields_of(*x, d.zone));
    std::vector<IntlPart> b = format_fields(d, fields_of(*y, d.zone));
    auto tag = [](std::vector<IntlPart>& parts, char const* source) {
        for (IntlPart& part : parts) {
            part.extra_name = "source";
            part.extra_value = source;
        }
    };
    bool same = a.size() == b.size();
    bool date_differs = false;
    for (std::size_t i = 0; same && i < a.size(); ++i) {
        if (a[i].value != b[i].value) {
            same = false;
        }
    }
    if (same) {
        tag(a, "shared");
        return a;
    }
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        if (is_date_type(a[i].type) && a[i].value != b[i].value)
            date_differs = true;
    // The collapsed forms: parts both ends share at the front or the back
    // are written once ("January 5 - 9, 2024", "3:04 - 5:00 PM").
    std::size_t prefix = 0;
    std::size_t suffix = 0;
    bool const text_month = std::any_of(d.pattern.begin(), d.pattern.end(), [](Token const& token) {
        return token.field == Field::Month && is_text_month(token.text);
    });
    bool const has_time = std::any_of(a.begin(), a.end(), [](IntlPart const& p) {
        return p.type == "hour" || p.type == "minute" || p.type == "second";
    });
    auto shareable_suffix = [&](std::string const& type) {
        if (type == "literal")
            return true;
        if (!date_differs)
            return type == "dayPeriod" || type == "timeZoneName";
        return text_month && !has_time && (type == "year" || type == "era");
    };
    while (suffix < a.size() && suffix < b.size()) {
        IntlPart const& p = a[a.size() - 1 - suffix];
        IntlPart const& q = b[b.size() - 1 - suffix];
        if (p.type != q.type || p.value != q.value || !shareable_suffix(p.type))
            break;
        ++suffix;
    }
    while (suffix > 0 && a[a.size() - suffix].type != "literal")
        --suffix;
    if (!date_differs) {
        while (prefix < a.size() && prefix < b.size() && a[prefix].type == b[prefix].type && a[prefix].value == b[prefix].value
            && (is_date_type(a[prefix].type) || a[prefix].type == "literal"))
            ++prefix;
    } else if (text_month && suffix > 0) {
        bool const year_shared = std::any_of(a.end() - static_cast<std::ptrdiff_t>(suffix), a.end(), [](IntlPart const& p) { return p.type == "year"; });
        if (year_shared) {
            while (prefix < a.size() && prefix < b.size() && a[prefix].type == b[prefix].type && a[prefix].value == b[prefix].value
                && (a[prefix].type == "month" || a[prefix].type == "literal" || a[prefix].type == "weekday"))
                ++prefix;
        }
    }
    while (prefix > 0 && a[prefix - 1].type != "literal")
        --prefix;
    if (prefix + suffix >= a.size() || prefix + suffix >= b.size()) {
        prefix = 0;
        suffix = 0;
    }
    std::vector<IntlPart> result;
    for (std::size_t i = 0; i < prefix; ++i) {
        result.push_back(a[i]);
        result.back().extra_name = "source";
        result.back().extra_value = "shared";
    }
    for (std::size_t i = prefix; i < a.size() - suffix; ++i) {
        result.push_back(a[i]);
        result.back().extra_name = "source";
        result.back().extra_value = "startRange";
    }
    // An en dash between thin spaces (U+2009 U+2013 U+2009).
    result.push_back({ "literal", std::u16string { 0x2009, 0x2013, 0x2009 }, "source", "shared" });
    for (std::size_t i = prefix; i < b.size() - suffix; ++i) {
        result.push_back(b[i]);
        result.back().extra_name = "source";
        result.back().extra_value = "endRange";
    }
    for (std::size_t i = b.size() - suffix; i < b.size(); ++i) {
        result.push_back(b[i]);
        result.back().extra_name = "source";
        result.back().extra_value = "shared";
    }
    // Adjacent literals read as one.
    std::vector<IntlPart> merged;
    for (IntlPart const& part : result) {
        if (!merged.empty() && merged.back().type == "literal" && part.type == "literal" && merged.back().extra_value == part.extra_value)
            merged.back().value += part.value;
        else
            merged.push_back(part);
    }
    return merged;
}

std::optional<Value> locale_date_string(Interpreter& in, Value const& this_value, Args args, Required required, Defaults defaults, std::string_view method)
{
    // Date.prototype.toLocale{,Date,Time}String (section 20.4.1-3).
    if (!this_value.is_object() || this_value.as_object()->class_id() != Object::Class::Date)
        return in.throw_type_error("this is not a Date object.");
    double const t = static_cast<DateObject*>(this_value.as_object())->time_value();
    (void)method;
    if (std::isnan(t))
        return intl_string(in, "Invalid Date");
    Interpreter::Roots const roots(in);
    auto* object = in.heap().allocate<IntlObjectOf<DateTimeFormatData>>(intl_prototype(in, IntlKind::DateTimeFormat));
    in.root(Value::object(object));
    if (!create_date_time_format(in, object->data, argument(args, 0), argument(args, 1), required, defaults))
        return std::nullopt;
    return intl_string(in, join_parts(format_fields(object->data, fields_of(t, object->data.zone))));
}

} // namespace

void install_intl_date_time_format(Interpreter& in, Object& intl)
{
    define_intl_constructor(in, intl, IntlKind::DateTimeFormat, 0,
        [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            Function* self = interp.intrinsics().intl_constructors[static_cast<std::size_t>(IntlKind::DateTimeFormat)];
            std::optional<Value> const made = date_time_format_construct(interp, args, self);
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
        date_time_format_construct);
    Object& prototype = *intl_prototype(in, IntlKind::DateTimeFormat);
    Heap::NoCollect const guard(in.heap());
    define_accessor(in, prototype, "format", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<DateTimeFormatData>*> const dtf = unwrap_date_time_format(interp, this_value, "format");
        if (!dtf)
            return std::nullopt;
        if ((*dtf)->slot(0).is_undefined()) {
            ClosureFunction* bound = interp.new_closure("", 1, { Value::object(*dtf) },
                [](Interpreter& in2, ClosureFunction& self, Value const&, Args args) -> std::optional<Value> {
                    auto* object = static_cast<IntlObjectOf<DateTimeFormatData>*>(self.slot(0).as_object());
                    std::optional<std::vector<IntlPart>> const parts = format_parts(in2, object->data, argument(args, 0));
                    if (!parts)
                        return std::nullopt;
                    return intl_string(in2, join_parts(*parts));
                });
            (*dtf)->set_slot(0, Value::object(bound));
        }
        return (*dtf)->slot(0);
    });
    define_method(in, prototype, "formatToParts", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<DateTimeFormatData>*> const dtf = intl_this<DateTimeFormatData>(interp, this_value, "formatToParts");
        if (!dtf)
            return std::nullopt;
        std::optional<std::vector<IntlPart>> const parts = format_parts(interp, (*dtf)->data, argument(args, 0));
        if (!parts)
            return std::nullopt;
        return parts_to_array(interp, *parts);
    });
    define_method(in, prototype, "formatRange", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<DateTimeFormatData>*> const dtf = intl_this<DateTimeFormatData>(interp, this_value, "formatRange");
        if (!dtf)
            return std::nullopt;
        std::optional<std::vector<IntlPart>> const parts = format_range_parts(interp, (*dtf)->data, argument(args, 0), argument(args, 1));
        if (!parts)
            return std::nullopt;
        return intl_string(interp, join_parts(*parts));
    });
    define_method(in, prototype, "formatRangeToParts", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<IntlObjectOf<DateTimeFormatData>*> const dtf = intl_this<DateTimeFormatData>(interp, this_value, "formatRangeToParts");
        if (!dtf)
            return std::nullopt;
        std::optional<std::vector<IntlPart>> const parts = format_range_parts(interp, (*dtf)->data, argument(args, 0), argument(args, 1));
        if (!parts)
            return std::nullopt;
        return parts_to_array(interp, *parts);
    });
    define_method(in, prototype, "resolvedOptions", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<IntlObjectOf<DateTimeFormatData>*> const found = unwrap_date_time_format(interp, this_value, "resolvedOptions");
        if (!found)
            return std::nullopt;
        DateTimeFormatData const& d = (*found)->data;
        Heap::NoCollect const no_collect(interp.heap());
        Object* result = interp.new_object();
        auto put = [&](char const* name, std::string const& value) {
            if (!value.empty())
                result->put(interp.key(name), intl_string(interp, value));
        };
        put("locale", d.locale);
        put("calendar", d.calendar);
        put("numberingSystem", d.numbering_system);
        put("timeZone", d.zone.name);
        if (!d.hour_cycle.empty()) {
            put("hourCycle", d.hour_cycle);
            result->put(interp.key("hour12"), Value::boolean(d.hour_cycle == "h11" || d.hour_cycle == "h12"));
        }
        if (d.date_style.empty() && d.time_style.empty()) {
            put("weekday", d.weekday);
            put("era", d.era);
            put("year", d.year);
            put("month", d.month);
            put("day", d.day);
            put("dayPeriod", d.day_period);
            put("hour", d.hour);
            put("minute", d.minute);
            put("second", d.second);
            if (d.fractional_second_digits > 0)
                result->put(interp.key("fractionalSecondDigits"), Value::number(d.fractional_second_digits));
            put("timeZoneName", d.time_zone_name);
        }
        put("dateStyle", d.date_style);
        put("timeStyle", d.time_style);
        return Value::object(result);
    });
}

void install_intl_date_methods(Interpreter& in)
{
    Heap::NoCollect const guard(in.heap());
    Object& date_prototype = *in.intrinsics().date_prototype;
    define_method(in, date_prototype, "toLocaleString", 0, [](Interpreter& interp, Value const& this_value, Args args) {
        return locale_date_string(interp, this_value, args, Required::Any, Defaults::All, "toLocaleString");
    });
    define_method(in, date_prototype, "toLocaleDateString", 0, [](Interpreter& interp, Value const& this_value, Args args) {
        return locale_date_string(interp, this_value, args, Required::Date, Defaults::Date, "toLocaleDateString");
    });
    define_method(in, date_prototype, "toLocaleTimeString", 0, [](Interpreter& interp, Value const& this_value, Args args) {
        return locale_date_string(interp, this_value, args, Required::Time, Defaults::Time, "toLocaleTimeString");
    });
}

}
