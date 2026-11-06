#include "JsTest.h"

// The IANA links the engine carries itself: a system zone database with
// a zone's file and none of its old names (Debian's and Ubuntu's tzdata
// without tzdata-legacy) still answers to them — Asia/Calcutta is read
// from Asia/Kolkata and keeps its own name, as ECMA-402 reports it. The
// database here is a folder holding Asia/Kolkata alone, copied from the
// system's; where the system keeps none (Windows) there is nothing to do.

#include "js/Interpreter.h"
#include "js/TimeZone.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace sashfold;

int main()
{
#if defined(_WIN32)
    return sashfold::test::report("js_tz_links");
#else
    std::filesystem::path const system = "/usr/share/zoneinfo/Asia/Kolkata";
    std::error_code error;
    if (!std::filesystem::exists(system, error))
        return sashfold::test::report("js_tz_links");
    std::filesystem::path const root
        = std::filesystem::temp_directory_path() / ("sashfold-tz-links-" + std::to_string(::getpid()));
    std::filesystem::create_directories(root / "Asia", error);
    std::filesystem::copy_file(system, root / "Asia" / "Kolkata", std::filesystem::copy_options::overwrite_existing, error);
    CHECK(!error);
    // Read once, at the first question: set before any.
    ::setenv("TZDIR", root.c_str(), 1);

    CHECK(js::time_zone_database_name("Asia/Kolkata") == std::string("Asia/Kolkata"));
    CHECK(js::time_zone_database_name("asia/calcutta") == std::string("Asia/Calcutta"));
    CHECK(js::time_zone_database_load("Asia/Calcutta") != nullptr);
    // A link whose zone the database lacks is no zone.
    CHECK(!js::time_zone_database_name("US/Eastern"));

    js::Interpreter in;
    CHECK_JS_STRING(in, "new Intl.DateTimeFormat('en', { timeZone: 'Asia/Calcutta' }).resolvedOptions().timeZone", "Asia/Calcutta");
    CHECK_JS_STRING(in, "new Date(Date.UTC(2024, 6, 15, 12)).toLocaleString('en-GB', { timeZone: 'Asia/Calcutta' })", "15/07/2024, 17:30:00");
    CHECK_JS_THROWS(in, "new Intl.DateTimeFormat('en', { timeZone: 'US/Eastern' })", "RangeError");

    std::filesystem::remove_all(root, error);
    return sashfold::test::report("js_tz_links");
#endif
}
