#pragma once

// Named time zones from the platform's own zone database, for
// Intl.DateTimeFormat's timeZone option.
//
// On POSIX systems the IANA database is installed as one compiled TZif
// file per zone (RFC 8536) under $TZDIR or /usr/share/zoneinfo, as the
// fonts and the CA bundle are installed. A zone is read from its file:
// the transition instants, the local time types they switch to (UT
// offset, daylight flag, abbreviation), and the footer's POSIX TZ string,
// which gives the rule for every instant after the last transition.
// Windows has no such database, so there only UTC, the offset zones and
// the system zone are known.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::js {

// Local time at one instant in one zone.
struct TimeZoneLocal {
    std::int32_t offset_seconds = 0; // added to UT to give local time
    bool dst = false;
    std::string abbreviation; // "EST", "BST", "+0530"
};

// A POSIX TZ string as TZif footers carry it (RFC 8536 section 3.3):
// "EST5EDT,M3.2.0,M11.1.0", "<+0530>-5:30", "AEST-10AEDT,M10.1.0,M4.1.0/3".
class PosixTimeZone {
public:
    static std::optional<PosixTimeZone> parse(std::string_view text);
    TimeZoneLocal local_at(std::int64_t utc_seconds) const;

private:
    // One end of the daylight period: a day of the year by one of the
    // three forms, and the local time of day the change happens.
    struct Date {
        enum class Form : std::uint8_t { Julian, ZeroBased, MonthWeekDay } form = Form::MonthWeekDay;
        int day = 0; // Jn: 1-365; n: 0-365; Mm.w.d: d, 0 = Sunday
        int month = 0; // 1-12
        int week = 0; // 1-5, 5 = the last
        std::int32_t time = 7200; // seconds after local midnight
    };
    std::int64_t transition(std::int64_t year, Date const& date, std::int32_t offset) const;

    std::string m_std_name;
    std::string m_dst_name;
    std::int32_t m_std_offset = 0; // UT offset, east positive
    std::int32_t m_dst_offset = 0;
    bool m_has_dst = false;
    Date m_start;
    Date m_end;
};

// A parsed TZif file.
class TimeZoneData {
public:
    static std::optional<TimeZoneData> parse(std::span<unsigned char const> bytes);
    TimeZoneLocal local_at(std::int64_t utc_seconds) const;

private:
    struct Type {
        std::int32_t offset = 0;
        bool dst = false;
        std::string abbreviation;
    };
    std::vector<std::int64_t> m_transitions;
    std::vector<std::uint8_t> m_transition_types;
    std::vector<Type> m_types;
    std::optional<PosixTimeZone> m_footer;
};

// The database's name for a requested zone, matched without regard to
// case ("america/new_york" is "America/New_York"), or nothing when the
// name is malformed or the database has no such zone. Aliases keep their
// own names: ECMA-402 reports "Asia/Calcutta" as given.
std::optional<std::string> time_zone_database_name(std::string_view requested);

// The zone of that name, read once and kept; null when its file is not a
// zone the reader understands.
std::shared_ptr<TimeZoneData const> time_zone_database_load(std::string const& name);

// The database's primary zones (its Zone lines, not its Links), for
// Intl.supportedValuesOf; empty when the platform does not say.
std::vector<std::string> time_zone_database_primary_names();

} // namespace sashfold::js
