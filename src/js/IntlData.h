#pragma once

// The English locale data the Intl objects format with, written out by
// hand: the names of months, weekdays, eras and day periods; the currency
// symbols and names; ECMA-402's sanctioned units in their three widths;
// the relative-time phrases; the list patterns; the display names of a
// modest table of languages, regions, scripts and currencies; and the
// likely-subtags and alias tables the locale machinery needs. Nothing
// here is read from CLDR at build or run time. Strings are UTF-8.
//
// en and en-US share every string; en-GB differs where British usage
// does (day before month, a 24-hour clock, "am"/"pm", no serial comma),
// and those differences are spelled in the formatters, not here; the one
// exception is the unit names, whose British rows are units_gb below.

#include <cstdint>
#include <string_view>

namespace sashfold::js::intl_data {

inline constexpr std::string_view month_long[12] = { "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December" };
inline constexpr std::string_view month_short[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep",
    "Oct", "Nov", "Dec" };
inline constexpr std::string_view month_narrow[12] = { "J", "F", "M", "A", "M", "J", "J", "A", "S", "O", "N", "D" };
// Sunday first, as Date's week_day numbers them.
inline constexpr std::string_view weekday_long[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday" };
inline constexpr std::string_view weekday_short[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
inline constexpr std::string_view weekday_narrow[7] = { "S", "M", "T", "W", "T", "F", "S" };
// Before Christ, then Anno Domini.
inline constexpr std::string_view era_long[2] = { "Before Christ", "Anno Domini" };
inline constexpr std::string_view era_short[2] = { "BC", "AD" };
inline constexpr std::string_view era_narrow[2] = { "B", "A" };
inline constexpr std::string_view am_pm[2] = { "AM", "PM" };
inline constexpr std::string_view am_pm_gb[2] = { "am", "pm" };
inline constexpr std::string_view am_pm_narrow[2] = { "a", "p" };

// The flexible day periods (dayPeriod): from the hour they start at.
struct DayPeriod {
    int start_hour;
    std::string_view narrow_short;
    std::string_view long_name;
};
inline constexpr DayPeriod day_periods[] = {
    { 0, "at night", "at night" },
    { 6, "in the morning", "in the morning" },
    { 12, "noon", "noon" }, // only the exact minute; see the formatter
    { 12, "in the afternoon", "in the afternoon" },
    { 18, "in the evening", "in the evening" },
    { 21, "at night", "at night" },
};

// Zone names English writes with letters: the abbreviations a zone file
// gives that English keeps (the North American ones in en-US, the
// European ones in en-GB, GMT in both), with their long names. Any other
// zone is written by its offset, "GMT+5:30". The exceptions by zone
// are in the formatter.
struct ZoneName {
    std::string_view abbreviation;
    std::string_view long_name;
    bool american; // written short in en and en-US
    bool british; // written short in en-GB
};
inline constexpr ZoneName zone_names[] = {
    { "GMT", "Greenwich Mean Time", true, true },
    { "EST", "Eastern Standard Time", true, false },
    { "EDT", "Eastern Daylight Time", true, false },
    { "CST", "Central Standard Time", true, false },
    { "CDT", "Central Daylight Time", true, false },
    { "MST", "Mountain Standard Time", true, false },
    { "MDT", "Mountain Daylight Time", true, false },
    { "PST", "Pacific Standard Time", true, false },
    { "PDT", "Pacific Daylight Time", true, false },
    { "AKST", "Alaska Standard Time", true, false },
    { "AKDT", "Alaska Daylight Time", true, false },
    { "HST", "Hawaii-Aleutian Standard Time", true, false },
    { "HDT", "Hawaii-Aleutian Daylight Time", true, false },
    { "AST", "Atlantic Standard Time", true, false },
    { "ADT", "Atlantic Daylight Time", true, false },
    { "BST", "British Summer Time", false, true },
    { "CET", "Central European Standard Time", false, true },
    { "CEST", "Central European Summer Time", false, true },
    { "EET", "Eastern European Standard Time", false, true },
    { "EEST", "Eastern European Summer Time", false, true },
    { "WET", "Western European Standard Time", false, true },
    { "WEST", "Western European Summer Time", false, true },
    { "GST", "Gulf Standard Time", false, true },
};

// ------------------------------------------------------------- currencies

struct Currency {
    std::string_view code;
    std::string_view symbol;
    std::string_view narrow_symbol;
    std::string_view name; // the display name
    std::string_view one; // "{0} US dollar" style names, one and other
    std::string_view other;
    int digits;
};
inline constexpr Currency currencies[] = {
    { "AUD", "A$", "$", "Australian Dollar", "Australian dollar", "Australian dollars", 2 },
    { "BRL", "R$", "R$", "Brazilian Real", "Brazilian real", "Brazilian reals", 2 },
    { "CAD", "CA$", "$", "Canadian Dollar", "Canadian dollar", "Canadian dollars", 2 },
    { "CHF", "CHF", "CHF", "Swiss Franc", "Swiss franc", "Swiss francs", 2 },
    { "CNY", "CN\xC2\xA5", "\xC2\xA5", "Chinese Yuan", "Chinese yuan", "Chinese yuan", 2 },
    { "EUR", "\xE2\x82\xAC", "\xE2\x82\xAC", "Euro", "euro", "euros", 2 },
    { "GBP", "\xC2\xA3", "\xC2\xA3", "British Pound", "British pound", "British pounds", 2 },
    { "INR", "\xE2\x82\xB9", "\xE2\x82\xB9", "Indian Rupee", "Indian rupee", "Indian rupees", 2 },
    { "JPY", "\xC2\xA5", "\xC2\xA5", "Japanese Yen", "Japanese yen", "Japanese yen", 0 },
    { "KRW", "\xE2\x82\xA9", "\xE2\x82\xA9", "South Korean Won", "South Korean won", "South Korean won", 0 },
    { "MXN", "MX$", "$", "Mexican Peso", "Mexican peso", "Mexican pesos", 2 },
    { "USD", "$", "$", "US Dollar", "US dollar", "US dollars", 2 },
};
// ISO 4217's minor units for codes outside the table above whose count is
// not two (the rest take two, as section 15.5.1 CurrencyDigits allows).
struct CurrencyDigits {
    std::string_view code;
    int digits;
};
inline constexpr CurrencyDigits currency_digits[] = {
    { "BHD", 3 }, { "BIF", 0 }, { "CLF", 4 }, { "CLP", 0 }, { "DJF", 0 }, { "GNF", 0 }, { "IQD", 3 }, { "ISK", 0 },
    { "JOD", 3 }, { "KMF", 0 }, { "KWD", 3 }, { "LYD", 3 }, { "OMR", 3 }, { "PYG", 0 }, { "RWF", 0 }, { "TND", 3 },
    { "UGX", 0 }, { "UYI", 0 }, { "UYW", 4 }, { "VND", 0 }, { "VUV", 0 }, { "XAF", 0 }, { "XOF", 0 }, { "XPF", 0 },
};

// ------------------------------------------------------------------ units

// ECMA-402's sanctioned single units (Table 2), in order. Patterns put the
// number at {0}.
struct Unit {
    std::string_view name;
    std::string_view short_one;
    std::string_view short_other;
    std::string_view narrow_one;
    std::string_view narrow_other;
    std::string_view long_one;
    std::string_view long_other;
    std::string_view per_short; // the unit as a denominator: "{0}/h"
    std::string_view per_long; // "{0} per hour"
};
inline constexpr Unit units[] = {
    { "acre", "{0} ac", "{0} ac", "{0}ac", "{0}ac", "{0} acre", "{0} acres", "{0}/ac", "{0} per acre" },
    { "bit", "{0} bit", "{0} bit", "{0}bit", "{0}bit", "{0} bit", "{0} bits", "{0}/bit", "{0} per bit" },
    { "byte", "{0} byte", "{0} byte", "{0}B", "{0}B", "{0} byte", "{0} bytes", "{0}/byte", "{0} per byte" },
    { "celsius", "{0}\xC2\xB0" "C", "{0}\xC2\xB0" "C", "{0}\xC2\xB0" "C", "{0}\xC2\xB0" "C", "{0} degree Celsius",
        "{0} degrees Celsius", "{0}/\xC2\xB0" "C", "{0} per degree Celsius" },
    { "centimeter", "{0} cm", "{0} cm", "{0}cm", "{0}cm", "{0} centimeter", "{0} centimeters", "{0}/cm",
        "{0} per centimeter" },
    { "day", "{0} day", "{0} days", "{0}d", "{0}d", "{0} day", "{0} days", "{0}/d", "{0} per day" },
    { "degree", "{0} deg", "{0} deg", "{0}\xC2\xB0", "{0}\xC2\xB0", "{0} degree", "{0} degrees", "{0}/deg",
        "{0} per degree" },
    { "fahrenheit", "{0}\xC2\xB0" "F", "{0}\xC2\xB0" "F", "{0}\xC2\xB0", "{0}\xC2\xB0", "{0} degree Fahrenheit",
        "{0} degrees Fahrenheit", "{0}/\xC2\xB0" "F", "{0} per degree Fahrenheit" },
    { "fluid-ounce", "{0} fl oz", "{0} fl oz", "{0}fl oz", "{0}fl oz", "{0} fluid ounce", "{0} fluid ounces",
        "{0}/fl oz", "{0} per fluid ounce" },
    { "foot", "{0} ft", "{0} ft", "{0}\xE2\x80\xB2", "{0}\xE2\x80\xB2", "{0} foot", "{0} feet", "{0}/ft",
        "{0} per foot" },
    { "gallon", "{0} gal", "{0} gal", "{0}gal", "{0}gal", "{0} gallon", "{0} gallons", "{0}/gal US",
        "{0} per gallon" },
    { "gigabit", "{0} Gb", "{0} Gb", "{0}Gb", "{0}Gb", "{0} gigabit", "{0} gigabits", "{0}/Gb", "{0} per gigabit" },
    { "gigabyte", "{0} GB", "{0} GB", "{0}GB", "{0}GB", "{0} gigabyte", "{0} gigabytes", "{0}/GB",
        "{0} per gigabyte" },
    { "gram", "{0} g", "{0} g", "{0}g", "{0}g", "{0} gram", "{0} grams", "{0}/g", "{0} per gram" },
    { "hectare", "{0} ha", "{0} ha", "{0}ha", "{0}ha", "{0} hectare", "{0} hectares", "{0}/ha", "{0} per hectare" },
    { "hour", "{0} hr", "{0} hr", "{0}h", "{0}h", "{0} hour", "{0} hours", "{0}/h", "{0} per hour" },
    { "inch", "{0} in", "{0} in", "{0}\xE2\x80\xB3", "{0}\xE2\x80\xB3", "{0} inch", "{0} inches", "{0}/in",
        "{0} per inch" },
    { "kilobit", "{0} kb", "{0} kb", "{0}kb", "{0}kb", "{0} kilobit", "{0} kilobits", "{0}/kb", "{0} per kilobit" },
    { "kilobyte", "{0} kB", "{0} kB", "{0}kB", "{0}kB", "{0} kilobyte", "{0} kilobytes", "{0}/kB",
        "{0} per kilobyte" },
    { "kilogram", "{0} kg", "{0} kg", "{0}kg", "{0}kg", "{0} kilogram", "{0} kilograms", "{0}/kg",
        "{0} per kilogram" },
    { "kilometer", "{0} km", "{0} km", "{0}km", "{0}km", "{0} kilometer", "{0} kilometers", "{0}/km",
        "{0} per kilometer" },
    { "liter", "{0} L", "{0} L", "{0}L", "{0}L", "{0} liter", "{0} liters", "{0}/L", "{0} per liter" },
    { "megabit", "{0} Mb", "{0} Mb", "{0}Mb", "{0}Mb", "{0} megabit", "{0} megabits", "{0}/Mb", "{0} per megabit" },
    { "megabyte", "{0} MB", "{0} MB", "{0}MB", "{0}MB", "{0} megabyte", "{0} megabytes", "{0}/MB",
        "{0} per megabyte" },
    { "meter", "{0} m", "{0} m", "{0}m", "{0}m", "{0} meter", "{0} meters", "{0}/m", "{0} per meter" },
    { "microsecond", "{0} \xCE\xBCs", "{0} \xCE\xBCs", "{0}\xCE\xBCs", "{0}\xCE\xBCs", "{0} microsecond",
        "{0} microseconds", "{0}/\xCE\xBCs", "{0} per microsecond" },
    { "mile", "{0} mi", "{0} mi", "{0}mi", "{0}mi", "{0} mile", "{0} miles", "{0}/mi", "{0} per mile" },
    { "mile-scandinavian", "{0} smi", "{0} smi", "{0}smi", "{0}smi", "{0} mile-scandinavian",
        "{0} miles-scandinavian", "{0}/smi", "{0} per mile-scandinavian" },
    { "milliliter", "{0} mL", "{0} mL", "{0}mL", "{0}mL", "{0} milliliter", "{0} milliliters", "{0}/mL",
        "{0} per milliliter" },
    { "millimeter", "{0} mm", "{0} mm", "{0}mm", "{0}mm", "{0} millimeter", "{0} millimeters", "{0}/mm",
        "{0} per millimeter" },
    { "millisecond", "{0} ms", "{0} ms", "{0}ms", "{0}ms", "{0} millisecond", "{0} milliseconds", "{0}/ms",
        "{0} per millisecond" },
    { "minute", "{0} min", "{0} min", "{0}m", "{0}m", "{0} minute", "{0} minutes", "{0}/min", "{0} per minute" },
    { "month", "{0} mth", "{0} mths", "{0}m", "{0}m", "{0} month", "{0} months", "{0}/m", "{0} per month" },
    { "nanosecond", "{0} ns", "{0} ns", "{0}ns", "{0}ns", "{0} nanosecond", "{0} nanoseconds", "{0}/ns",
        "{0} per nanosecond" },
    { "ounce", "{0} oz", "{0} oz", "{0}oz", "{0}oz", "{0} ounce", "{0} ounces", "{0}/oz", "{0} per ounce" },
    { "percent", "{0}%", "{0}%", "{0}%", "{0}%", "{0} percent", "{0} percent", "{0}/%", "{0} per percent" },
    { "petabyte", "{0} PB", "{0} PB", "{0}PB", "{0}PB", "{0} petabyte", "{0} petabytes", "{0}/PB",
        "{0} per petabyte" },
    { "pound", "{0} lb", "{0} lb", "{0}#", "{0}#", "{0} pound", "{0} pounds", "{0}/lb", "{0} per pound" },
    { "second", "{0} sec", "{0} sec", "{0}s", "{0}s", "{0} second", "{0} seconds", "{0}/s", "{0} per second" },
    { "stone", "{0} st", "{0} st", "{0}st", "{0}st", "{0} stone", "{0} stones", "{0}/st", "{0} per stone" },
    { "terabit", "{0} Tb", "{0} Tb", "{0}Tb", "{0}Tb", "{0} terabit", "{0} terabits", "{0}/Tb", "{0} per terabit" },
    { "terabyte", "{0} TB", "{0} TB", "{0}TB", "{0}TB", "{0} terabyte", "{0} terabytes", "{0}/TB",
        "{0} per terabyte" },
    { "week", "{0} wk", "{0} wks", "{0}w", "{0}w", "{0} week", "{0} weeks", "{0}/w", "{0} per week" },
    { "yard", "{0} yd", "{0} yd", "{0}yd", "{0}yd", "{0} yard", "{0} yards", "{0}/yd", "{0} per yard" },
    { "year", "{0} yr", "{0} yrs", "{0}y", "{0}y", "{0} year", "{0} years", "{0}/y", "{0} per year" },
};

// en-GB's rows where British usage differs from the table above: the
// -metre and -litre spellings, "per cent", lower-case litre symbols, US
// qualifiers on the American volumes, plural abbreviations of time.
inline constexpr Unit units_gb[] = {
    { "centimeter", "{0} cm", "{0} cm", "{0}cm", "{0}cm", "{0} centimetre", "{0} centimetres", "{0}/cm",
        "{0} per centimetre" },
    { "fahrenheit", "{0}\xC2\xB0" "F", "{0}\xC2\xB0" "F", "{0}\xC2\xB0" "F", "{0}\xC2\xB0" "F",
        "{0} degree Fahrenheit", "{0} degrees Fahrenheit", "{0}/\xC2\xB0" "F", "{0} per degree Fahrenheit" },
    { "fluid-ounce", "{0} US fl oz", "{0} US fl oz", "{0}US fl oz", "{0}US fl oz", "{0} US fluid ounce",
        "{0} US fluid ounces", "{0}/US fl oz", "{0} per US fluid ounce" },
    { "gallon", "{0} US gal", "{0} US gal", "{0}USgal", "{0}USgal", "{0} US gallon", "{0} US gallons",
        "{0}/US gal", "{0} per US gallon" },
    { "hour", "{0} hr", "{0} hrs", "{0}h", "{0}h", "{0} hour", "{0} hours", "{0}/h", "{0} per hour" },
    { "kilometer", "{0} km", "{0} km", "{0}km", "{0}km", "{0} kilometre", "{0} kilometres", "{0}/km",
        "{0} per kilometre" },
    { "liter", "{0} l", "{0} l", "{0}l", "{0}l", "{0} litre", "{0} litres", "{0}/l", "{0} per litre" },
    { "meter", "{0} m", "{0} m", "{0}m", "{0}m", "{0} metre", "{0} metres", "{0}/m", "{0} per metre" },
    { "mile-scandinavian", "{0} smi", "{0} smi", "{0}smi", "{0}smi", "{0} Scandinavian mile",
        "{0} Scandinavian miles", "{0}/smi", "{0} per Scandinavian mile" },
    { "milliliter", "{0} ml", "{0} ml", "{0}ml", "{0}ml", "{0} millilitre", "{0} millilitres", "{0}/ml",
        "{0} per millilitre" },
    { "millimeter", "{0} mm", "{0} mm", "{0}mm", "{0}mm", "{0} millimetre", "{0} millimetres", "{0}/mm",
        "{0} per millimetre" },
    { "minute", "{0} min", "{0} mins", "{0}m", "{0}m", "{0} minute", "{0} minutes", "{0}/min", "{0} per minute" },
    { "percent", "{0}%", "{0}%", "{0}%", "{0}%", "{0} per cent", "{0} per cent", "{0}/%", "{0} per per cent" },
    { "pound", "{0} lb", "{0} lb", "{0}lb", "{0}lb", "{0} pound", "{0} pounds", "{0}/lb", "{0} per pound" },
    { "second", "{0} sec", "{0} secs", "{0}s", "{0}s", "{0} second", "{0} seconds", "{0}/s", "{0} per second" },
    { "stone", "{0} st", "{0} st", "{0}st", "{0}st", "{0} stone", "{0} stone", "{0}/st", "{0} per stone" },
};

// ---------------------------------------------------------- relative time

// Per unit and width: the future and past patterns (one, other), and the
// phrases numeric: "auto" uses for -1, 0 and 1 (empty = none).
struct RelativeUnit {
    std::string_view unit;
    std::string_view future_one, future_other, past_one, past_other;
    std::string_view last, current, next;
};
// long, short, narrow for each of second to year.
inline constexpr RelativeUnit relative_units[3][8] = {
    {
        { "second", "in {0} second", "in {0} seconds", "{0} second ago", "{0} seconds ago", "", "now", "" },
        { "minute", "in {0} minute", "in {0} minutes", "{0} minute ago", "{0} minutes ago", "", "this minute", "" },
        { "hour", "in {0} hour", "in {0} hours", "{0} hour ago", "{0} hours ago", "", "this hour", "" },
        { "day", "in {0} day", "in {0} days", "{0} day ago", "{0} days ago", "yesterday", "today", "tomorrow" },
        { "week", "in {0} week", "in {0} weeks", "{0} week ago", "{0} weeks ago", "last week", "this week",
            "next week" },
        { "month", "in {0} month", "in {0} months", "{0} month ago", "{0} months ago", "last month", "this month",
            "next month" },
        { "quarter", "in {0} quarter", "in {0} quarters", "{0} quarter ago", "{0} quarters ago", "last quarter",
            "this quarter", "next quarter" },
        { "year", "in {0} year", "in {0} years", "{0} year ago", "{0} years ago", "last year", "this year",
            "next year" },
    },
    {
        { "second", "in {0} sec.", "in {0} sec.", "{0} sec. ago", "{0} sec. ago", "", "now", "" },
        { "minute", "in {0} min.", "in {0} min.", "{0} min. ago", "{0} min. ago", "", "this minute", "" },
        { "hour", "in {0} hr.", "in {0} hr.", "{0} hr. ago", "{0} hr. ago", "", "this hour", "" },
        { "day", "in {0} day", "in {0} days", "{0} day ago", "{0} days ago", "yesterday", "today", "tomorrow" },
        { "week", "in {0} wk.", "in {0} wk.", "{0} wk. ago", "{0} wk. ago", "last wk.", "this wk.", "next wk." },
        { "month", "in {0} mo.", "in {0} mo.", "{0} mo. ago", "{0} mo. ago", "last mo.", "this mo.", "next mo." },
        { "quarter", "in {0} qtr.", "in {0} qtrs.", "{0} qtr. ago", "{0} qtrs. ago", "last qtr.", "this qtr.",
            "next qtr." },
        { "year", "in {0} yr.", "in {0} yr.", "{0} yr. ago", "{0} yr. ago", "last yr.", "this yr.", "next yr." },
    },
    {
        { "second", "in {0}s", "in {0}s", "{0}s ago", "{0}s ago", "", "now", "" },
        { "minute", "in {0}m", "in {0}m", "{0}m ago", "{0}m ago", "", "this minute", "" },
        { "hour", "in {0}h", "in {0}h", "{0}h ago", "{0}h ago", "", "this hour", "" },
        { "day", "in {0}d", "in {0}d", "{0}d ago", "{0}d ago", "yesterday", "today", "tomorrow" },
        { "week", "in {0}w", "in {0}w", "{0}w ago", "{0}w ago", "last wk.", "this wk.", "next wk." },
        { "month", "in {0}mo", "in {0}mo", "{0}mo ago", "{0}mo ago", "last mo.", "this mo.", "next mo." },
        { "quarter", "in {0}q", "in {0}q", "{0}q ago", "{0}q ago", "last qtr.", "this qtr.", "next qtr." },
        { "year", "in {0}y", "in {0}y", "{0}y ago", "{0}y ago", "last yr.", "this yr.", "next yr." },
    },
};

// ------------------------------------------------------------ list patterns

// For type conjunction, disjunction, unit and width long, short, narrow:
// the joiner between the last two of three or more, the joiner of a pair,
// and the joiner between the others.
struct ListPattern {
    std::string_view middle;
    std::string_view pair;
    std::string_view end;
};
inline constexpr ListPattern list_patterns[3][3] = {
    { { ", ", " and ", ", and " }, { ", ", " & ", ", & " }, { ", ", ", ", ", " } },
    { { ", ", " or ", ", or " }, { ", ", " or ", ", or " }, { ", ", " or ", ", or " } },
    { { ", ", ", ", ", " }, { ", ", ", ", ", " }, { " ", " ", " " } },
};

// ------------------------------------------------------------ display names

struct Name {
    std::string_view code;
    std::string_view name;
};
inline constexpr Name language_names[] = {
    { "af", "Afrikaans" }, { "am", "Amharic" }, { "ar", "Arabic" }, { "az", "Azerbaijani" }, { "be", "Belarusian" },
    { "bg", "Bulgarian" }, { "bn", "Bangla" }, { "bs", "Bosnian" }, { "ca", "Catalan" }, { "cs", "Czech" },
    { "cy", "Welsh" }, { "da", "Danish" }, { "de", "German" }, { "el", "Greek" }, { "en", "English" },
    { "eo", "Esperanto" }, { "es", "Spanish" }, { "et", "Estonian" }, { "eu", "Basque" }, { "fa", "Persian" },
    { "fi", "Finnish" }, { "fil", "Filipino" }, { "fr", "French" }, { "ga", "Irish" }, { "gl", "Galician" },
    { "gu", "Gujarati" }, { "ha", "Hausa" }, { "haw", "Hawaiian" }, { "he", "Hebrew" }, { "hi", "Hindi" },
    { "hr", "Croatian" }, { "hu", "Hungarian" }, { "hy", "Armenian" }, { "id", "Indonesian" }, { "ig", "Igbo" },
    { "is", "Icelandic" }, { "it", "Italian" }, { "ja", "Japanese" }, { "jv", "Javanese" }, { "ka", "Georgian" },
    { "kk", "Kazakh" }, { "km", "Khmer" }, { "kn", "Kannada" }, { "ko", "Korean" }, { "la", "Latin" },
    { "lo", "Lao" }, { "lt", "Lithuanian" }, { "lv", "Latvian" }, { "mk", "Macedonian" }, { "ml", "Malayalam" },
    { "mn", "Mongolian" }, { "mr", "Marathi" }, { "ms", "Malay" }, { "mt", "Maltese" }, { "my", "Burmese" },
    { "nb", "Norwegian Bokm\xC3\xA5l" }, { "ne", "Nepali" }, { "nl", "Dutch" }, { "nn", "Norwegian Nynorsk" },
    { "no", "Norwegian" }, { "pa", "Punjabi" }, { "pl", "Polish" }, { "ps", "Pashto" }, { "pt", "Portuguese" },
    { "ro", "Romanian" }, { "ru", "Russian" }, { "sk", "Slovak" }, { "sl", "Slovenian" }, { "so", "Somali" },
    { "sq", "Albanian" }, { "sr", "Serbian" }, { "sv", "Swedish" }, { "sw", "Swahili" }, { "ta", "Tamil" },
    { "te", "Telugu" }, { "th", "Thai" }, { "tr", "Turkish" }, { "uk", "Ukrainian" }, { "ur", "Urdu" },
    { "uz", "Uzbek" }, { "vi", "Vietnamese" }, { "yi", "Yiddish" }, { "yo", "Yoruba" }, { "zh", "Chinese" },
    { "zu", "Zulu" },
};
// The dialect names languageDisplay "dialect" gives a language and a
// region, or a language and a script, together.
inline constexpr Name dialect_names[] = {
    { "de-AT", "Austrian German" }, { "de-CH", "Swiss High German" }, { "en-AU", "Australian English" },
    { "en-CA", "Canadian English" }, { "en-GB", "British English" }, { "en-US", "American English" },
    { "es-419", "Latin American Spanish" }, { "es-ES", "European Spanish" }, { "es-MX", "Mexican Spanish" },
    { "fr-CA", "Canadian French" }, { "fr-CH", "Swiss French" }, { "nl-BE", "Flemish" },
    { "pt-BR", "Brazilian Portuguese" }, { "pt-PT", "European Portuguese" }, { "zh-Hans", "Simplified Chinese" },
    { "zh-Hant", "Traditional Chinese" },
};
inline constexpr Name region_names[] = {
    { "001", "world" }, { "150", "Europe" }, { "419", "Latin America" }, { "AE", "United Arab Emirates" },
    { "AR", "Argentina" }, { "AT", "Austria" }, { "AU", "Australia" }, { "BE", "Belgium" }, { "BR", "Brazil" },
    { "CA", "Canada" }, { "CH", "Switzerland" }, { "CL", "Chile" }, { "CN", "China" }, { "CO", "Colombia" },
    { "CZ", "Czechia" }, { "DE", "Germany" }, { "DK", "Denmark" }, { "EG", "Egypt" }, { "ES", "Spain" },
    { "FI", "Finland" }, { "FR", "France" }, { "GB", "United Kingdom" }, { "GR", "Greece" },
    { "HK", "Hong Kong SAR China" }, { "HU", "Hungary" }, { "ID", "Indonesia" }, { "IE", "Ireland" },
    { "IL", "Israel" }, { "IN", "India" }, { "IR", "Iran" }, { "IT", "Italy" }, { "JP", "Japan" }, { "KE", "Kenya" },
    { "KR", "South Korea" }, { "MX", "Mexico" }, { "MY", "Malaysia" }, { "NG", "Nigeria" }, { "NL", "Netherlands" },
    { "NO", "Norway" }, { "NZ", "New Zealand" }, { "PH", "Philippines" }, { "PK", "Pakistan" }, { "PL", "Poland" },
    { "PT", "Portugal" }, { "RO", "Romania" }, { "RS", "Serbia" }, { "RU", "Russia" }, { "SA", "Saudi Arabia" },
    { "SE", "Sweden" }, { "SG", "Singapore" }, { "TH", "Thailand" }, { "TR", "T\xC3\xBCrkiye" }, { "TW", "Taiwan" },
    { "UA", "Ukraine" }, { "US", "United States" }, { "VN", "Vietnam" }, { "ZA", "South Africa" },
};
inline constexpr Name region_short_names[] = {
    { "GB", "UK" }, { "HK", "Hong Kong" }, { "US", "US" },
};
inline constexpr Name script_names[] = {
    { "Arab", "Arabic" }, { "Armn", "Armenian" }, { "Beng", "Bangla" }, { "Cyrl", "Cyrillic" },
    { "Deva", "Devanagari" }, { "Ethi", "Ethiopic" }, { "Geor", "Georgian" }, { "Grek", "Greek" },
    { "Gujr", "Gujarati" }, { "Guru", "Gurmukhi" }, { "Hang", "Hangul" }, { "Hani", "Han" },
    { "Hans", "Simplified" }, { "Hant", "Traditional" }, { "Hebr", "Hebrew" }, { "Hira", "Hiragana" },
    { "Jpan", "Japanese" }, { "Kana", "Katakana" }, { "Khmr", "Khmer" }, { "Knda", "Kannada" }, { "Kore", "Korean" },
    { "Laoo", "Lao" }, { "Latn", "Latin" }, { "Mlym", "Malayalam" }, { "Mong", "Mongolian" }, { "Mymr", "Myanmar" },
    { "Shaw", "Shavian" }, { "Sinh", "Sinhala" }, { "Taml", "Tamil" }, { "Telu", "Telugu" }, { "Thaa", "Thaana" },
    { "Thai", "Thai" }, { "Tibt", "Tibetan" }, { "Zyyy", "Common" },
};
inline constexpr Name calendar_names[] = {
    { "gregory", "Gregorian Calendar" },
};
// dateTimeField names in long, short and narrow.
struct FieldName {
    std::string_view code;
    std::string_view long_name, short_name, narrow_name;
};
inline constexpr FieldName field_names[] = {
    { "era", "era", "era", "era" },
    { "year", "year", "yr.", "yr" },
    { "quarter", "quarter", "qtr.", "qtr" },
    { "month", "month", "mo.", "mo" },
    { "weekOfYear", "week", "wk.", "wk" },
    { "weekday", "day of the week", "day of wk.", "day of wk." },
    { "day", "day", "day", "day" },
    { "dayPeriod", "AM/PM", "AM/PM", "AM/PM" },
    { "hour", "hour", "hr.", "hr" },
    { "minute", "minute", "min.", "min" },
    { "second", "second", "sec.", "sec" },
    { "timeZoneName", "time zone", "zone", "zone" },
};

// -------------------------------------------------------- the locale tables

// Likely subtags (UTS #35 section 4.3): a language, language-script,
// language-region or und-... key and its maximal form.
inline constexpr Name likely_subtags[] = {
    { "aa", "aa-Latn-ET" }, { "aae", "aae-Latn-IT" }, { "jbo", "jbo-Latn-001" }, { "hak", "hak-Hans-CN" },
    { "hsn", "hsn-Hans-CN" }, { "hyw", "hyw-Armn-AM" }, { "pap", "pap-Latn-CW" }, { "und-CW", "pap-Latn-CW" },
    { "af", "af-Latn-ZA" }, { "am", "am-Ethi-ET" }, { "ar", "ar-Arab-EG" }, { "az", "az-Latn-AZ" },
    { "be", "be-Cyrl-BY" }, { "bg", "bg-Cyrl-BG" }, { "bn", "bn-Beng-BD" }, { "bs", "bs-Latn-BA" },
    { "ca", "ca-Latn-ES" }, { "cs", "cs-Latn-CZ" }, { "cy", "cy-Latn-GB" }, { "da", "da-Latn-DK" },
    { "de", "de-Latn-DE" }, { "el", "el-Grek-GR" }, { "en", "en-Latn-US" }, { "en-Shaw", "en-Shaw-GB" },
    { "es", "es-Latn-ES" }, { "et", "et-Latn-EE" }, { "eu", "eu-Latn-ES" }, { "fa", "fa-Arab-IR" },
    { "fi", "fi-Latn-FI" }, { "fil", "fil-Latn-PH" }, { "fr", "fr-Latn-FR" }, { "ga", "ga-Latn-IE" },
    { "gl", "gl-Latn-ES" }, { "gu", "gu-Gujr-IN" }, { "he", "he-Hebr-IL" }, { "hi", "hi-Deva-IN" },
    { "hr", "hr-Latn-HR" }, { "hu", "hu-Latn-HU" }, { "hy", "hy-Armn-AM" }, { "id", "id-Latn-ID" },
    { "is", "is-Latn-IS" }, { "it", "it-Latn-IT" }, { "ja", "ja-Jpan-JP" }, { "ka", "ka-Geor-GE" },
    { "kk", "kk-Cyrl-KZ" }, { "km", "km-Khmr-KH" }, { "kn", "kn-Knda-IN" }, { "ko", "ko-Kore-KR" },
    { "lo", "lo-Laoo-LA" }, { "lt", "lt-Latn-LT" }, { "lv", "lv-Latn-LV" }, { "mk", "mk-Cyrl-MK" },
    { "ml", "ml-Mlym-IN" }, { "mn", "mn-Cyrl-MN" }, { "mr", "mr-Deva-IN" }, { "ms", "ms-Latn-MY" },
    { "my", "my-Mymr-MM" }, { "nb", "nb-Latn-NO" }, { "ne", "ne-Deva-NP" }, { "nl", "nl-Latn-NL" },
    { "no", "no-Latn-NO" }, { "pa", "pa-Guru-IN" }, { "pa-PK", "pa-Arab-PK" }, { "pl", "pl-Latn-PL" },
    { "pt", "pt-Latn-BR" }, { "ro", "ro-Latn-RO" }, { "ru", "ru-Cyrl-RU" }, { "sk", "sk-Latn-SK" },
    { "sl", "sl-Latn-SI" }, { "sq", "sq-Latn-AL" }, { "sr", "sr-Cyrl-RS" }, { "sr-ME", "sr-Latn-ME" },
    { "sv", "sv-Latn-SE" }, { "sw", "sw-Latn-TZ" }, { "ta", "ta-Taml-IN" }, { "te", "te-Telu-IN" },
    { "th", "th-Thai-TH" }, { "tr", "tr-Latn-TR" }, { "uk", "uk-Cyrl-UA" }, { "ur", "ur-Arab-PK" },
    { "uz", "uz-Latn-UZ" }, { "vi", "vi-Latn-VN" }, { "yi", "yi-Hebr-UA" }, { "zh", "zh-Hans-CN" },
    { "zh-Hant", "zh-Hant-TW" }, { "zh-HK", "zh-Hant-HK" }, { "zh-MO", "zh-Hant-MO" }, { "zh-TW", "zh-Hant-TW" },
    { "und", "en-Latn-US" }, { "und-150", "en-Latn-150" }, { "und-419", "es-Latn-419" }, { "und-AT", "de-Latn-AT" },
    { "und-Arab", "ar-Arab-EG" }, { "und-Armn", "hy-Armn-AM" }, { "und-CH", "de-Latn-CH" }, { "und-CN", "zh-Hans-CN" },
    { "und-Cyrl", "ru-Cyrl-RU" }, { "und-Cyrl-RO", "bg-Cyrl-RO" }, { "und-DE", "de-Latn-DE" },
    { "und-Deva", "hi-Deva-IN" }, { "und-ES", "es-Latn-ES" }, { "und-FR", "fr-Latn-FR" },
    { "und-GB", "en-Latn-GB" }, { "und-Grek", "el-Grek-GR" }, { "und-Hans", "zh-Hans-CN" },
    { "und-Hant", "zh-Hant-TW" }, { "und-Hebr", "he-Hebr-IL" }, { "und-IN", "hi-Deva-IN" },
    { "und-IT", "it-Latn-IT" }, { "und-JP", "ja-Jpan-JP" }, { "und-Jpan", "ja-Jpan-JP" },
    { "und-KR", "ko-Kore-KR" }, { "und-Kore", "ko-Kore-KR" }, { "und-Latn", "en-Latn-US" },
    { "und-MX", "es-Latn-MX" }, { "und-RU", "ru-Cyrl-RU" }, { "und-Thai", "th-Thai-TH" },
    { "und-TW", "zh-Hant-TW" }, { "und-US", "en-Latn-US" },
};

// Language aliases (the parts of CLDR's alias data a tag canonicalizes
// through): a language subtag, or a language with a variant or a region
// (in the grandfathered forms), and what replaces it.
inline constexpr Name language_aliases[] = {
    { "aar", "aa" }, { "aju", "jrb" }, { "art-lojban", "jbo" }, { "cel-gaulish", "xtg" }, { "ces", "cs" },
    { "cmn", "zh" }, { "cnr", "sr-ME" }, { "deu", "de" }, { "eng", "en" }, { "fra", "fr" }, { "heb", "he" },
    { "hy-arevela", "hy" }, { "hy-arevmda", "hyw" }, { "in", "id" }, { "iw", "he" }, { "ji", "yi" },
    { "jw", "jv" }, { "mo", "ro" }, { "no-bok", "nb" }, { "no-nyn", "nn" }, { "sgn-GR", "gss" },
    { "sh", "sr-Latn" }, { "spa", "es" }, { "swc", "sw-CD" }, { "tl", "fil" }, { "zh-guoyu", "zh" },
    { "zh-hakka", "hak" }, { "zh-xiang", "hsn" }, { "zho", "zh" },
};
// Region aliases: a retired or numeric region and its replacement (the
// first of CLDR's list; the language's likely region picks among several,
// done for SU and 810 in the code).
inline constexpr Name region_aliases[] = {
    { "004", "AF" }, { "008", "AL" }, { "010", "AQ" }, { "012", "DZ" }, { "016", "AS" }, { "020", "AD" },
    { "024", "AO" }, { "028", "AG" }, { "031", "AZ" }, { "032", "AR" }, { "036", "AU" }, { "040", "AT" },
    { "056", "BE" }, { "076", "BR" }, { "124", "CA" }, { "156", "CN" }, { "250", "FR" }, { "276", "DE" },
    { "280", "DE" }, { "356", "IN" }, { "380", "IT" }, { "392", "JP" }, { "410", "KR" }, { "484", "MX" },
    { "528", "NL" }, { "643", "RU" }, { "724", "ES" }, { "756", "CH" }, { "826", "GB" }, { "840", "US" },
    { "886", "YE" }, { "152", "CL" }, { "158", "TW" }, { "170", "CO" }, { "203", "CZ" }, { "208", "DK" },
    { "246", "FI" }, { "300", "GR" }, { "344", "HK" }, { "348", "HU" }, { "360", "ID" }, { "364", "IR" },
    { "368", "IQ" }, { "372", "IE" }, { "376", "IL" }, { "404", "KE" }, { "458", "MY" }, { "504", "MA" },
    { "554", "NZ" }, { "566", "NG" }, { "578", "NO" }, { "586", "PK" }, { "604", "PE" }, { "608", "PH" },
    { "616", "PL" }, { "620", "PT" }, { "642", "RO" }, { "682", "SA" }, { "688", "RS" }, { "702", "SG" },
    { "704", "VN" }, { "710", "ZA" }, { "752", "SE" }, { "764", "TH" }, { "784", "AE" }, { "788", "TN" },
    { "792", "TR" }, { "804", "UA" }, { "818", "EG" }, { "862", "VE" }, { "BU", "MM" }, { "CS", "RS" }, { "CT", "KI" }, { "DD", "DE" }, { "DY", "BJ" },
    { "FQ", "AQ" }, { "FX", "FR" }, { "HV", "BF" }, { "JT", "UM" }, { "MI", "UM" }, { "NH", "VU" },
    { "NQ", "AQ" }, { "NT", "SA" }, { "PC", "FM" }, { "PU", "UM" }, { "PZ", "PA" }, { "QU", "EU" },
    { "RH", "ZW" }, { "TP", "TL" }, { "UK", "GB" }, { "VD", "VN" }, { "WK", "UM" }, { "YD", "YE" },
    { "YU", "RS" }, { "ZR", "CD" },
};
// The Soviet Union's successors, for SU and 810: the language's likely
// region when it is one of them, else the first.
inline constexpr std::string_view soviet_successors[] = { "RU", "AM", "AZ", "BY", "EE", "GE", "KZ", "KG", "LV",
    "LT", "MD", "TJ", "TM", "UA", "UZ" };
// Variant aliases.
inline constexpr Name variant_aliases[] = {
    { "heploc", "alalc97" },
};
// -u- keyword type aliases: key, alias, replacement.
struct KeywordAlias {
    std::string_view key;
    std::string_view alias;
    std::string_view replacement;
};
inline constexpr KeywordAlias keyword_aliases[] = {
    { "ca", "ethiopic-amete-alem", "ethioaa" }, { "ca", "islamicc", "islamic-civil" },
    { "ks", "primary", "level1" }, { "ks", "tertiary", "level3" }, { "ms", "imperial", "uksystem" },
    { "rg", "cn11", "cnbj" }, { "rg", "cz10a", "cz110" }, { "rg", "fra", "frges" }, { "rg", "frg", "frges" },
    { "rg", "lud", "lucl" }, { "rg", "no23", "no50" }, { "sd", "cn11", "cnbj" }, { "sd", "cz10a", "cz110" },
    { "sd", "fra", "frges" }, { "sd", "frg", "frges" }, { "sd", "lud", "lucl" }, { "sd", "no23", "no50" },
    { "tz", "cnckg", "cnsha" }, { "tz", "eire", "iedub" }, { "tz", "est", "papty" }, { "tz", "gmt0", "gmt" },
    { "tz", "uct", "utc" }, { "tz", "zulu", "utc" },
    { "m0", "names", "prprname" },
};
// The numbering systems with a simple digit mapping (UTS #35, CLDR's
// numberingSystems): each system's zero, the other nine following it in
// the code space, but for hanidec, whose digits are ideographs.
struct NumberingSystem {
    std::string_view name;
    char32_t zero;
};
inline constexpr NumberingSystem numbering_systems[] = {
    { "adlm", 0x1E950 }, { "ahom", 0x11730 }, { "arab", 0x0660 }, { "arabext", 0x06F0 }, { "bali", 0x1B50 },
    { "beng", 0x09E6 }, { "bhks", 0x11C50 }, { "brah", 0x11066 }, { "cakm", 0x11136 }, { "cham", 0xAA50 },
    { "deva", 0x0966 }, { "diak", 0x11950 }, { "fullwide", 0xFF10 }, { "gara", 0x10D40 }, { "gong", 0x11DA0 },
    { "gonm", 0x11D50 }, { "gujr", 0x0AE6 }, { "gukh", 0x16130 }, { "guru", 0x0A66 }, { "hanidec", 0 },
    { "hmng", 0x16B50 }, { "hmnp", 0x1E140 }, { "java", 0xA9D0 }, { "kali", 0xA900 }, { "kawi", 0x11F50 },
    { "khmr", 0x17E0 }, { "knda", 0x0CE6 }, { "krai", 0x16D70 }, { "lana", 0x1A80 }, { "lanatham", 0x1A90 },
    { "laoo", 0x0ED0 }, { "latn", 0x0030 }, { "lepc", 0x1C40 }, { "limb", 0x1946 }, { "mathbold", 0x1D7CE },
    { "mathdbl", 0x1D7D8 }, { "mathmono", 0x1D7F6 }, { "mathsanb", 0x1D7EC }, { "mathsans", 0x1D7E2 },
    { "mlym", 0x0D66 }, { "modi", 0x11650 }, { "mong", 0x1810 }, { "mroo", 0x16A60 }, { "mtei", 0xABF0 },
    { "mymr", 0x1040 }, { "mymrepka", 0x116DA }, { "mymrpao", 0x116D0 }, { "mymrshan", 0x1090 },
    { "mymrtlng", 0xA9F0 }, { "nagm", 0x1E4F0 }, { "newa", 0x11450 }, { "nkoo", 0x07C0 }, { "olck", 0x1C50 },
    { "onao", 0x1E5F1 }, { "orya", 0x0B66 }, { "osma", 0x104A0 }, { "outlined", 0x1CCF0 }, { "rohg", 0x10D30 },
    { "saur", 0xA8D0 }, { "segment", 0x1FBF0 }, { "shrd", 0x111D0 }, { "sind", 0x112F0 }, { "sinh", 0x0DE6 },
    { "sora", 0x110F0 }, { "sund", 0x1BB0 }, { "sunu", 0x11BF0 }, { "takr", 0x116C0 }, { "talu", 0x19D0 },
    { "tamldec", 0x0BE6 }, { "telu", 0x0C66 }, { "thai", 0x0E50 }, { "tibt", 0x0F20 }, { "tirh", 0x114D0 },
    { "tnsa", 0x16AC0 }, { "tols", 0x11DE0 }, { "vaii", 0xA620 }, { "wara", 0x118E0 }, { "wcho", 0x1E2F0 },
};
inline constexpr char32_t hanidec_digits[10] = { 0x3007, 0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94, 0x516D, 0x4E03,
    0x516B, 0x4E5D };

// Languages written right to left, for Locale.prototype.getTextInfo.
inline constexpr std::string_view rtl_scripts[] = { "Adlm", "Arab", "Hebr", "Nkoo", "Rohg", "Syrc", "Thaa" };

}
