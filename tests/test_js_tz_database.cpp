#include "JsTest.h"

// The time zone database the engine carries (TimeZoneData.cpp): every IANA
// zone and link the same on every machine — with the system's zoneinfo
// pointed at an empty folder, on Windows (which has none), on a
// distribution without the old names (Ubuntu's tzdata without
// tzdata-legacy, which reddened CI twice). Links keep their own names, as
// ECMA-402 reports them.

#include "js/Interpreter.h"
#include "js/TimeZone.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

using namespace sashfold;

int main()
{
    // Whatever the machine has is not asked: an empty folder changes nothing.
    std::error_code error;
    std::filesystem::path const empty = std::filesystem::temp_directory_path() / "sashfold-no-zoneinfo";
    std::filesystem::create_directories(empty, error);
#if defined(_WIN32)
    _putenv_s("TZDIR", empty.string().c_str());
#else
    ::setenv("TZDIR", empty.c_str(), 1);
#endif

    CHECK(std::string(js::time_zone_database_version()) >= "2026a");
    CHECK(js::embedded_zones().size() > 300);
    CHECK(js::time_zone_database_name("asia/kolkata") == std::string("Asia/Kolkata"));
    CHECK(js::time_zone_database_name("asia/calcutta") == std::string("Asia/Calcutta"));
    CHECK(js::time_zone_database_name("US/Eastern") == std::string("US/Eastern"));
    CHECK(js::time_zone_database_name("America/Coyhaique") == std::string("America/Coyhaique"));
    CHECK(js::time_zone_database_load("Asia/Calcutta") != nullptr);
    CHECK(!js::time_zone_database_name("Factory"));
    CHECK(!js::time_zone_database_name("Not/AZone"));

    js::Interpreter in;
    CHECK_JS_STRING(in, "new Intl.DateTimeFormat('en', { timeZone: 'Asia/Calcutta' }).resolvedOptions().timeZone", "Asia/Calcutta");
    CHECK_JS_STRING(in, "new Date(Date.UTC(2024, 6, 15, 12)).toLocaleString('en-GB', { timeZone: 'Asia/Calcutta' })", "15/07/2024, 17:30:00");
    CHECK_JS_STRING(in, "new Date(Date.UTC(2024, 0, 15, 12)).toLocaleString('en-US', { timeZone: 'US/Eastern' })", "1/15/2024, 7:00:00 AM");
    CHECK_JS_TRUE(in, "Intl.supportedValuesOf('timeZone').includes('Europe/Kyiv')");
    CHECK_JS_TRUE(in, "!Intl.supportedValuesOf('timeZone').includes('US/Eastern')");

    std::filesystem::remove(empty, error);
    return sashfold::test::report("js_tz_database");
}
