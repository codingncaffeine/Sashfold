#include "js/TimeZone.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace sashfold::js {

namespace {

constexpr std::int64_t seconds_per_day = 86400;

std::int64_t floor_div(std::int64_t a, std::int64_t b)
{
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        --q;
    return q;
}

// Days from 1970-01-01 to a proleptic Gregorian date (month 1-12).
std::int64_t days_from_civil(std::int64_t y, int m, int d)
{
    y -= m <= 2 ? 1 : 0;
    std::int64_t const era = floor_div(y, 400);
    std::int64_t const yoe = y - era * 400;
    std::int64_t const mp = (m + 9) % 12;
    std::int64_t const doy = (153 * mp + 2) / 5 + d - 1;
    std::int64_t const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

std::int64_t year_from_days(std::int64_t days)
{
    std::int64_t const z = days + 719468;
    std::int64_t const era = floor_div(z, 146097);
    std::int64_t const doe = z - era * 146097;
    std::int64_t const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    std::int64_t const mp = (5 * doy + 2) / 153;
    std::int64_t const m = mp < 10 ? mp + 3 : mp - 9;
    return yoe + era * 400 + (m <= 2 ? 1 : 0);
}

bool leap_year(std::int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int days_in_month(std::int64_t y, int m)
{
    static constexpr int lengths[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leap_year(y) ? 29 : lengths[m - 1];
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// A decimal number of at most max_digits digits.
std::optional<int> parse_number(std::string_view& s, std::size_t max_digits)
{
    std::size_t n = 0;
    int value = 0;
    while (n < s.size() && n < max_digits && is_digit(s[n])) {
        value = value * 10 + (s[n] - '0');
        ++n;
    }
    if (n == 0)
        return std::nullopt;
    s.remove_prefix(n);
    return value;
}

// A zone designation: three or more letters, or anything of letters,
// digits, '+' and '-' between angle brackets.
std::optional<std::string> parse_designation(std::string_view& s)
{
    std::string name;
    if (!s.empty() && s[0] == '<') {
        std::size_t const close = s.find('>');
        if (close == std::string_view::npos)
            return std::nullopt;
        name = std::string(s.substr(1, close - 1));
        s.remove_prefix(close + 1);
        for (char c : name)
            if (!is_alpha(c) && !is_digit(c) && c != '+' && c != '-')
                return std::nullopt;
    } else {
        std::size_t n = 0;
        while (n < s.size() && is_alpha(s[n]))
            ++n;
        name = std::string(s.substr(0, n));
        s.remove_prefix(n);
    }
    if (name.size() < 3)
        return std::nullopt;
    return name;
}

// [+-]hh[:mm[:ss]], in seconds as written (POSIX offsets count west as
// positive; rule times are simply signed). Hours up to max_hours.
std::optional<std::int32_t> parse_time(std::string_view& s, int max_hours)
{
    int sign = 1;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        sign = s[0] == '-' ? -1 : 1;
        s.remove_prefix(1);
    }
    std::optional<int> const hours = parse_number(s, 3);
    if (!hours || *hours > max_hours)
        return std::nullopt;
    int minutes = 0;
    int seconds = 0;
    if (!s.empty() && s[0] == ':') {
        s.remove_prefix(1);
        std::optional<int> const m = parse_number(s, 2);
        if (!m || *m > 59)
            return std::nullopt;
        minutes = *m;
        if (!s.empty() && s[0] == ':') {
            s.remove_prefix(1);
            std::optional<int> const sec = parse_number(s, 2);
            if (!sec || *sec > 59)
                return std::nullopt;
            seconds = *sec;
        }
    }
    return sign * (*hours * 3600 + minutes * 60 + seconds);
}

} // namespace

// ------------------------------------------------------------ POSIX TZ

std::optional<PosixTimeZone> PosixTimeZone::parse(std::string_view s)
{
    PosixTimeZone zone;
    std::optional<std::string> std_name = parse_designation(s);
    if (!std_name)
        return std::nullopt;
    zone.m_std_name = std::move(*std_name);
    std::optional<std::int32_t> const std_offset = parse_time(s, 24);
    if (!std_offset)
        return std::nullopt;
    zone.m_std_offset = -*std_offset;
    if (s.empty()) {
        zone.m_dst_name = zone.m_std_name;
        zone.m_dst_offset = zone.m_std_offset;
        return zone;
    }
    std::optional<std::string> dst_name = parse_designation(s);
    if (!dst_name)
        return std::nullopt;
    zone.m_dst_name = std::move(*dst_name);
    zone.m_has_dst = true;
    zone.m_dst_offset = zone.m_std_offset + 3600;
    if (!s.empty() && s[0] != ',') {
        std::optional<std::int32_t> const dst_offset = parse_time(s, 24);
        if (!dst_offset)
            return std::nullopt;
        zone.m_dst_offset = -*dst_offset;
    }
    // With no rule, the United States' rule, as the C libraries assume.
    zone.m_start = { Date::Form::MonthWeekDay, 0, 3, 2, 7200 };
    zone.m_end = { Date::Form::MonthWeekDay, 0, 11, 1, 7200 };
    if (s.empty())
        return zone;
    auto parse_date = [&s](Date& date) -> bool {
        if (s.empty() || s[0] != ',')
            return false;
        s.remove_prefix(1);
        if (!s.empty() && s[0] == 'M') {
            s.remove_prefix(1);
            std::optional<int> const month = parse_number(s, 2);
            if (!month || *month < 1 || *month > 12 || s.empty() || s[0] != '.')
                return false;
            s.remove_prefix(1);
            std::optional<int> const week = parse_number(s, 1);
            if (!week || *week < 1 || *week > 5 || s.empty() || s[0] != '.')
                return false;
            s.remove_prefix(1);
            std::optional<int> const day = parse_number(s, 1);
            if (!day || *day > 6)
                return false;
            date.form = Date::Form::MonthWeekDay;
            date.month = *month;
            date.week = *week;
            date.day = *day;
        } else if (!s.empty() && s[0] == 'J') {
            s.remove_prefix(1);
            std::optional<int> const day = parse_number(s, 3);
            if (!day || *day < 1 || *day > 365)
                return false;
            date.form = Date::Form::Julian;
            date.day = *day;
        } else {
            std::optional<int> const day = parse_number(s, 3);
            if (!day || *day > 365)
                return false;
            date.form = Date::Form::ZeroBased;
            date.day = *day;
        }
        date.time = 7200;
        if (!s.empty() && s[0] == '/') {
            s.remove_prefix(1);
            // RFC 8536 section 3.3.1 extends the hours to -167..167.
            std::optional<std::int32_t> const time = parse_time(s, 167);
            if (!time)
                return false;
            date.time = *time;
        }
        return true;
    };
    if (!parse_date(zone.m_start) || !parse_date(zone.m_end) || !s.empty())
        return std::nullopt;
    return zone;
}

std::int64_t PosixTimeZone::transition(std::int64_t year, Date const& date, std::int32_t offset) const
{
    std::int64_t day = 0;
    switch (date.form) {
    case Date::Form::Julian:
        // Jn counts 1-365 and never counts February 29.
        day = days_from_civil(year, 1, 1) + date.day - 1 + (leap_year(year) && date.day >= 60 ? 1 : 0);
        break;
    case Date::Form::ZeroBased:
        day = days_from_civil(year, 1, 1) + date.day;
        break;
    case Date::Form::MonthWeekDay: {
        std::int64_t const first = days_from_civil(year, date.month, 1);
        int const first_weekday = static_cast<int>(((first + 4) % 7 + 7) % 7);
        int offset_days = (date.day - first_weekday + 7) % 7 + (date.week - 1) * 7;
        int const length = days_in_month(year, date.month);
        while (offset_days >= length)
            offset_days -= 7;
        day = first + offset_days;
        break;
    }
    }
    return day * seconds_per_day + date.time - offset;
}

TimeZoneLocal PosixTimeZone::local_at(std::int64_t utc_seconds) const
{
    TimeZoneLocal standard { m_std_offset, false, m_std_name };
    if (!m_has_dst)
        return standard;
    std::int64_t const year = year_from_days(floor_div(utc_seconds + m_std_offset, seconds_per_day));
    // The start is written in standard time, the end in daylight time.
    std::int64_t const start = transition(year, m_start, m_std_offset);
    std::int64_t const end = transition(year, m_end, m_dst_offset);
    bool const dst = start < end ? (utc_seconds >= start && utc_seconds < end) : !(utc_seconds >= end && utc_seconds < start);
    if (!dst)
        return standard;
    return { m_dst_offset, true, m_dst_name };
}

// ----------------------------------------------------------------- TZif

namespace {

class Reader {
public:
    explicit Reader(std::span<unsigned char const> bytes)
        : m_bytes(bytes)
    {
    }
    bool has(std::uint64_t n) const { return m_pos <= m_bytes.size() && n <= m_bytes.size() - m_pos; }
    std::size_t position() const { return m_pos; }
    void skip(std::size_t n) { m_pos += n; }
    std::uint8_t u8() { return m_bytes[m_pos++]; }
    std::uint32_t be32()
    {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v = (v << 8) | m_bytes[m_pos++];
        return v;
    }
    std::uint64_t be64()
    {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | m_bytes[m_pos++];
        return v;
    }
    std::span<unsigned char const> rest() const { return m_bytes.subspan(std::min(m_pos, m_bytes.size())); }

private:
    std::span<unsigned char const> m_bytes;
    std::size_t m_pos = 0;
};

struct Header {
    char version = 0;
    std::uint32_t isutcnt = 0, isstdcnt = 0, leapcnt = 0, timecnt = 0, typecnt = 0, charcnt = 0;

    std::uint64_t block_size(std::uint64_t time_size) const
    {
        return std::uint64_t { timecnt } * time_size + timecnt + std::uint64_t { typecnt } * 6 + charcnt
            + std::uint64_t { leapcnt } * (time_size + 4) + isstdcnt + isutcnt;
    }
};

std::optional<Header> read_header(Reader& r)
{
    if (!r.has(44))
        return std::nullopt;
    Header h;
    if (r.u8() != 'T' || r.u8() != 'Z' || r.u8() != 'i' || r.u8() != 'f')
        return std::nullopt;
    h.version = static_cast<char>(r.u8());
    r.skip(15);
    h.isutcnt = r.be32();
    h.isstdcnt = r.be32();
    h.leapcnt = r.be32();
    h.timecnt = r.be32();
    h.typecnt = r.be32();
    h.charcnt = r.be32();
    // RFC 8536 section 3.1: at least one type and one designation byte,
    // and the indicator counts are zero or the type count.
    if (h.typecnt == 0 || h.typecnt > 256 || h.charcnt == 0 || h.timecnt > (1u << 20) || h.leapcnt > (1u << 16))
        return std::nullopt;
    if ((h.isutcnt != 0 && h.isutcnt != h.typecnt) || (h.isstdcnt != 0 && h.isstdcnt != h.typecnt))
        return std::nullopt;
    return h;
}

} // namespace

std::optional<TimeZoneData> TimeZoneData::parse(std::span<unsigned char const> bytes)
{
    Reader r(bytes);
    std::optional<Header> header = read_header(r);
    if (!header)
        return std::nullopt;
    std::uint64_t time_size = 4;
    if (header->version != 0) {
        // Version 2 and later repeat the data with 64-bit times after the
        // version 1 block; the first block is for old readers only.
        std::uint64_t const skip = header->block_size(4);
        if (!r.has(skip))
            return std::nullopt;
        r.skip(static_cast<std::size_t>(skip));
        header = read_header(r);
        if (!header)
            return std::nullopt;
        time_size = 8;
    }
    Header const& h = *header;
    if (!r.has(h.block_size(time_size)))
        return std::nullopt;
    TimeZoneData data;
    data.m_transitions.reserve(h.timecnt);
    for (std::uint32_t i = 0; i < h.timecnt; ++i) {
        std::int64_t const t = time_size == 8 ? static_cast<std::int64_t>(r.be64()) : static_cast<std::int32_t>(r.be32());
        if (!data.m_transitions.empty() && t <= data.m_transitions.back())
            return std::nullopt;
        data.m_transitions.push_back(t);
    }
    data.m_transition_types.reserve(h.timecnt);
    for (std::uint32_t i = 0; i < h.timecnt; ++i) {
        std::uint8_t const type = r.u8();
        if (type >= h.typecnt)
            return std::nullopt;
        data.m_transition_types.push_back(type);
    }
    struct RawType {
        std::int32_t offset;
        bool dst;
        std::uint8_t designation;
    };
    std::vector<RawType> raw;
    for (std::uint32_t i = 0; i < h.typecnt; ++i) {
        auto const offset = static_cast<std::int32_t>(r.be32());
        std::uint8_t const dst = r.u8();
        std::uint8_t const designation = r.u8();
        if (offset == std::numeric_limits<std::int32_t>::min() || dst > 1 || designation >= h.charcnt)
            return std::nullopt;
        raw.push_back({ offset, dst == 1, designation });
    }
    std::size_t const chars_at = r.position();
    std::span<unsigned char const> const chars = bytes.subspan(chars_at, h.charcnt);
    for (RawType const& t : raw) {
        auto const begin = chars.begin() + t.designation;
        auto const nul = std::find(begin, chars.end(), '\0');
        if (nul == chars.end())
            return std::nullopt;
        data.m_types.push_back({ t.offset, t.dst, std::string(begin, nul) });
    }
    r.skip(h.charcnt + static_cast<std::size_t>(std::uint64_t { h.leapcnt } * (time_size + 4)) + h.isstdcnt + h.isutcnt);
    if (h.version != 0) {
        // The footer: a newline, a POSIX TZ string, a newline. An empty
        // or unreadable one leaves the last transition's type in force.
        std::span<unsigned char const> const rest = r.rest();
        if (rest.size() >= 2 && rest[0] == '\n') {
            auto const end = std::find(rest.begin() + 1, rest.end(), '\n');
            if (end != rest.end() && end != rest.begin() + 1)
                data.m_footer = PosixTimeZone::parse(std::string(rest.begin() + 1, end));
        }
    }
    return data;
}

TimeZoneLocal TimeZoneData::local_at(std::int64_t utc_seconds) const
{
    auto from_type = [this](std::size_t index) {
        Type const& t = m_types[index];
        return TimeZoneLocal { t.offset, t.dst, t.abbreviation };
    };
    if (m_transitions.empty())
        return m_footer ? m_footer->local_at(utc_seconds) : from_type(0);
    // Before the first transition, type 0 (RFC 8536 section 3.2); after
    // the last, the footer's rule.
    if (utc_seconds < m_transitions.front())
        return from_type(0);
    if (utc_seconds >= m_transitions.back() && m_footer)
        return m_footer->local_at(utc_seconds);
    auto const after = std::upper_bound(m_transitions.begin(), m_transitions.end(), utc_seconds);
    auto const index = static_cast<std::size_t>(std::distance(m_transitions.begin(), after) - 1);
    return from_type(m_transition_types[index]);
}

// ------------------------------------------------------------- database

namespace {

// [A-Za-z0-9_+-]+ separated by single slashes: no dots, so no way out of
// the database's directory.
bool valid_zone_name(std::string_view name)
{
    if (name.empty() || name.size() > 128 || name.front() == '/' || name.back() == '/')
        return false;
    char previous = '/';
    for (char c : name) {
        bool const word = is_alpha(c) || is_digit(c) || c == '_' || c == '+' || c == '-';
        if (!word && c != '/')
            return false;
        if (c == '/' && previous == '/')
            return false;
        previous = c;
    }
    return true;
}

std::string lower_ascii(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::filesystem::path database_directory()
{
    if (char const* dir = std::getenv("TZDIR"); dir && *dir)
        return dir;
    return "/usr/share/zoneinfo";
}

struct DatabaseIndex {
    std::unordered_map<std::string, std::string> by_lower_name;
    std::vector<std::string> primary;
};

DatabaseIndex build_index()
{
    DatabaseIndex index;
#if !defined(_WIN32)
    std::filesystem::path const root = database_directory();
    std::error_code error;
    // The database's files that are not zones: the fallback rules, the
    // placeholder zone, the leap second table, and the notes.
    static constexpr std::string_view not_zones[] = { "posixrules", "Factory", "localtime", "SECURITY", "leapseconds" };
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error);
    for (; !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        std::filesystem::directory_entry const& entry = *it;
        std::string const name = entry.path().lexically_relative(root).generic_string();
        std::error_code status_error;
        if (entry.is_directory(status_error)) {
            // posix/ repeats the database and right/ counts leap seconds.
            if (it.depth() == 0 && (name == "posix" || name == "right"))
                it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(status_error) || !valid_zone_name(name))
            continue;
        if (std::find(std::begin(not_zones), std::end(not_zones), name) != std::end(not_zones))
            continue;
        index.by_lower_name.emplace(lower_ascii(name), name);
    }
    // tzdata.zi, the database in zic's input form, tells a Zone from a
    // Link: its "Z" lines name the primary zones.
    std::ifstream zi(root / "tzdata.zi");
    std::string line;
    while (std::getline(zi, line)) {
        if (!line.starts_with("Z "))
            continue;
        std::size_t const end = line.find_first_of(" \t", 2);
        std::string const name = line.substr(2, end == std::string::npos ? std::string::npos : end - 2);
        auto const found = index.by_lower_name.find(lower_ascii(name));
        if (found != index.by_lower_name.end() && found->second == name)
            index.primary.push_back(name);
    }
#endif
    return index;
}

DatabaseIndex const& database_index()
{
    static DatabaseIndex const index = build_index();
    return index;
}

std::shared_ptr<TimeZoneData const> read_zone_file(std::string const& name)
{
    std::ifstream file(database_directory() / name, std::ios::binary);
    if (!file)
        return nullptr;
    std::vector<unsigned char> bytes;
    char buffer[4096];
    while (file.read(buffer, sizeof buffer) || file.gcount() > 0) {
        bytes.insert(bytes.end(), buffer, buffer + file.gcount());
        if (bytes.size() > (1u << 20))
            return nullptr;
    }
    std::optional<TimeZoneData> data = TimeZoneData::parse(bytes);
    if (!data)
        return nullptr;
    return std::make_shared<TimeZoneData const>(std::move(*data));
}

} // namespace

std::optional<std::string> time_zone_database_name(std::string_view requested)
{
    if (!valid_zone_name(requested))
        return std::nullopt;
    DatabaseIndex const& index = database_index();
    auto const found = index.by_lower_name.find(lower_ascii(requested));
    if (found == index.by_lower_name.end())
        return std::nullopt;
    return found->second;
}

std::shared_ptr<TimeZoneData const> time_zone_database_load(std::string const& name)
{
    // Only a name the database lists, exactly as it lists it.
    std::optional<std::string> const listed = time_zone_database_name(name);
    if (!listed || *listed != name)
        return nullptr;
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<TimeZoneData const>> cache;
    std::lock_guard const lock(mutex);
    auto const found = cache.find(name);
    if (found != cache.end())
        return found->second;
    std::shared_ptr<TimeZoneData const> data = read_zone_file(name);
    cache.emplace(name, data);
    return data;
}

std::vector<std::string> time_zone_database_primary_names()
{
    return database_index().primary;
}

} // namespace sashfold::js
