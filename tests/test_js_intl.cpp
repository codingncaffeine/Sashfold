#include "JsTest.h"

// ECMA-402: the Intl object and its constructors under heap stress --
// construction, resolvedOptions, the formatToParts shapes, en-US against
// en-GB, an offset time zone, the locale machinery, and the errors the
// specification names. Every expected string is what V8 with full ICU
// prints for the same call.

#include "js/Interpreter.h"
#include "js/Runtime.h"

#include <string>

using namespace sashfold;

namespace {

js::Interpreter& fresh()
{
    static js::Interpreter* interpreter = nullptr;
    delete interpreter;
    interpreter = new js::Interpreter();
    interpreter->heap().set_stress(true);
    return *interpreter;
}

void test_intl_object()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "Object.prototype.toString.call(Intl)", "[object Intl]");
    CHECK_JS_STRING(in, "Object.getOwnPropertyNames(Intl).sort().join()",
        "Collator,DateTimeFormat,DisplayNames,DurationFormat,ListFormat,Locale,NumberFormat,PluralRules,RelativeTimeFormat,Segmenter,getCanonicalLocales,supportedValuesOf");
    CHECK_JS_STRING(in, "[Intl.Collator, Intl.DateTimeFormat, Intl.NumberFormat, Intl.DisplayNames, Intl.Locale, Intl.getCanonicalLocales].map(f => f.name + f.length).join()",
        "Collator0,DateTimeFormat0,NumberFormat0,DisplayNames2,Locale1,getCanonicalLocales1");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(new Intl.NumberFormat()) === Intl.NumberFormat.prototype && Object.getPrototypeOf(Intl.NumberFormat.prototype) === Object.prototype");
    CHECK_JS_STRING(in, "Object.prototype.toString.call(new Intl.PluralRules())", "[object Intl.PluralRules]");
    CHECK_JS_TRUE(in, "Intl.getCanonicalLocales(['EN-us', 'cmn-hans-cn', 'sl-rozaj-biske-1994', 'ru-SU']).join() === 'en-US,zh-Hans-CN,sl-1994-biske-rozaj,ru-RU'");
    CHECK_JS_TRUE(in, "Intl.supportedValuesOf('calendar').join() === 'gregory' && Intl.supportedValuesOf('currency').includes('JPY')");
    CHECK_JS_TRUE(in, "Intl.NumberFormat.supportedLocalesOf(['en-GB', 'de', 'en-AU']).join() === 'en-GB,en-AU'");
    CHECK_JS_THROWS(in, "Intl.getCanonicalLocales('en_US')", "RangeError");
    CHECK_JS_THROWS(in, "Intl.getCanonicalLocales([5])", "TypeError");
    CHECK_JS_THROWS(in, "Intl.supportedValuesOf('colour')", "RangeError");
}

void test_locale()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "new Intl.Locale('en').maximize().toString()", "en-Latn-US");
    CHECK_JS_STRING(in, "new Intl.Locale('en-Latn-GB').minimize().toString()", "en-GB");
    CHECK_JS_STRING(in, "new Intl.Locale('en', { region: 'gb', hourCycle: 'h23', numeric: true }).toString()", "en-GB-u-hc-h23-kn");
    CHECK_JS_TRUE(in, "var l = new Intl.Locale('de-latn-de-u-ca-gregory-co-phonebk'); l.language === 'de' && l.script === 'Latn' && l.region === 'DE' && l.calendar === 'gregory' && l.collation === 'phonebk' && l.numeric === false && l.baseName === 'de-Latn-DE'");
    CHECK_JS_STRING(in, "JSON.stringify(new Intl.Locale('en-GB').getWeekInfo())", "{\"firstDay\":1,\"weekend\":[6,7]}");
    CHECK_JS_THROWS(in, "Intl.Locale('en')", "TypeError");
    CHECK_JS_THROWS(in, "new Intl.Locale('x-private')", "RangeError");
    CHECK_JS_THROWS(in, "new Intl.Locale(5)", "TypeError");
}

void test_number_format()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "new Intl.NumberFormat('en-US', { style: 'currency', currency: 'USD' }).format(1234.5)", "$1,234.50");
    CHECK_JS_STRING(in, "JSON.stringify(new Intl.NumberFormat('en', { style: 'currency', currency: 'EUR' }).formatToParts(-1234.5))",
        "[{\"type\":\"minusSign\",\"value\":\"-\"},{\"type\":\"currency\",\"value\":\"\xE2\x82\xAC\"},{\"type\":\"integer\",\"value\":\"1\"},{\"type\":\"group\",\"value\":\",\"},{\"type\":\"integer\",\"value\":\"234\"},{\"type\":\"decimal\",\"value\":\".\"},{\"type\":\"fraction\",\"value\":\"50\"}]");
    CHECK_JS_STRING(in, "new Intl.NumberFormat('en', { notation: 'compact' }).format(999999) + '|' + new Intl.NumberFormat('en', { notation: 'compact', compactDisplay: 'long' }).format(1234567)", "1M|1.2 million");
    CHECK_JS_STRING(in, "new Intl.NumberFormat('en', { notation: 'scientific' }).format(-0.00123) + '|' + new Intl.NumberFormat('en', { notation: 'engineering' }).format(12345)", "-1.23E-3|12.345E3");
    CHECK_JS_STRING(in, "new Intl.NumberFormat('en', { style: 'unit', unit: 'kilometer-per-hour', unitDisplay: 'long' }).format(2) + '|' + new Intl.NumberFormat('en', { style: 'percent' }).format(-0.25)", "2 kilometers per hour|-25%");
    // A Number formats from its shortest digits, a string from its own, exactly.
    CHECK_JS_STRING(in, "var two = new Intl.NumberFormat('en', { maximumFractionDigits: 2 }); two.format(1.005) + '|' + two.format('1.00499999999999999999') + '|' + new Intl.NumberFormat('en', { minimumSignificantDigits: 5 }).format(123.456)", "1.01|1|123.456");
    CHECK_JS_STRING(in, "new Intl.NumberFormat('en', { style: 'currency', currency: 'USD', currencySign: 'accounting' }).format(-1) + '|' + new Intl.NumberFormat().format(-0)", "($1.00)|-0");
    CHECK_JS_STRING(in, "new Intl.NumberFormat('en').formatRange(3, 5) + '|' + new Intl.NumberFormat('en').formatRange(3, 3)", "3\xE2\x80\x93" "5|~3");
    CHECK_JS_STRING(in, "(1234567.891).toLocaleString() + '|' + (12345678901234567890n).toLocaleString('en')", "1,234,567.891|12,345,678,901,234,567,890");
    CHECK_JS_STRING(in, "var o = new Intl.NumberFormat('de').resolvedOptions(); [o.locale, o.numberingSystem, o.style, o.minimumFractionDigits, o.maximumFractionDigits, o.useGrouping, o.roundingMode].join()", "en-US,latn,decimal,0,3,auto,halfExpand");
    CHECK_JS_TRUE(in, "var nf = new Intl.NumberFormat(); nf.format === nf.format && nf.format.length === 1 && nf.format.name === '' && [1, 2].map(nf.format).join() === '1,2'");
    CHECK_JS_THROWS(in, "new Intl.NumberFormat('en', { style: 'currency' })", "TypeError");
    CHECK_JS_THROWS(in, "new Intl.NumberFormat('en', { style: 'currency', currency: 'US$' })", "RangeError");
    CHECK_JS_THROWS(in, "new Intl.NumberFormat('en', { maximumFractionDigits: 101 })", "RangeError");
    CHECK_JS_THROWS(in, "Intl.NumberFormat.prototype.formatToParts.call({}, 1)", "TypeError");
}

void test_plural_rules()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "var p = new Intl.PluralRules('en'); [0, 1, 1.5, 2].map(n => p.select(n)).join()", "other,one,other,other");
    CHECK_JS_STRING(in, "var o = new Intl.PluralRules('en', { type: 'ordinal' }); [1, 2, 3, 4, 11, 22, 103, 113].map(n => o.select(n)).join()", "one,two,few,other,other,two,few,other");
    CHECK_JS_STRING(in, "new Intl.PluralRules('en', { type: 'ordinal' }).resolvedOptions().pluralCategories.join()", "one,two,few,other");
    CHECK_JS_STRING(in, "new Intl.PluralRules('en').selectRange(1, 2)", "other");
    CHECK_JS_THROWS(in, "new Intl.PluralRules('en').selectRange(NaN, 2)", "RangeError");
    CHECK_JS_THROWS(in, "Intl.PluralRules()", "TypeError");
}

void test_date_time_format()
{
    js::Interpreter& in = fresh();
    sashfold::test::run_js(in, "var d = new Date(Date.UTC(2024, 0, 5, 15, 4, 9, 87));");
    // en-US writes the month first and a 12-hour clock; en-GB the day first and a 24-hour one.
    CHECK_JS_STRING(in, "new Intl.DateTimeFormat('en-US', { timeZone: 'UTC' }).format(d) + '|' + new Intl.DateTimeFormat('en-GB', { timeZone: 'UTC' }).format(d)", "1/5/2024|05/01/2024");
    CHECK_JS_STRING(in, "new Intl.DateTimeFormat('en-US', { dateStyle: 'full', timeStyle: 'short', timeZone: 'UTC' }).format(d)", "Friday, January 5, 2024 at 3:04 PM");
    CHECK_JS_STRING(in, "new Intl.DateTimeFormat('en-GB', { dateStyle: 'medium', timeStyle: 'medium', timeZone: 'UTC' }).format(d)", "5 Jan 2024, 15:04:09");
    // An offset zone moves the fields and names itself by its offset.
    CHECK_JS_STRING(in, "new Intl.DateTimeFormat('en-GB', { timeZone: '+05:30', dateStyle: 'full', timeStyle: 'long' }).format(d)", "Friday, 5 January 2024 at 20:34:09 GMT+5:30");
    CHECK_JS_STRING(in, "JSON.stringify(new Intl.DateTimeFormat('en-US', { timeZone: '-08:00', hour: 'numeric', minute: '2-digit', timeZoneName: 'short' }).formatToParts(d))",
        "[{\"type\":\"hour\",\"value\":\"7\"},{\"type\":\"literal\",\"value\":\":\"},{\"type\":\"minute\",\"value\":\"04\"},{\"type\":\"literal\",\"value\":\" \"},{\"type\":\"dayPeriod\",\"value\":\"AM\"},{\"type\":\"literal\",\"value\":\" \"},{\"type\":\"timeZoneName\",\"value\":\"GMT-8\"}]");
    CHECK_JS_STRING(in, "var o = new Intl.DateTimeFormat('en-GB', { hour: 'numeric', timeZone: '+0530' }).resolvedOptions(); [o.locale, o.calendar, o.timeZone, o.hourCycle, o.hour12, o.hour].join()", "en-GB,gregory,+05:30,h23,false,2-digit");
    CHECK_JS_STRING(in, "new Intl.DateTimeFormat('en', { hour: 'numeric', minute: 'numeric', timeZone: 'UTC' }).formatRange(d, new Date(Date.UTC(2024, 0, 5, 17)))", "3:04\xE2\x80\x89\xE2\x80\x93\xE2\x80\x89" "5:00 PM");
    CHECK_JS_STRING(in, "new Intl.DateTimeFormat('en', { timeZone: 'UTC', year: 'numeric', era: 'short' }).format(new Date(Date.UTC(-100, 0, 1)))", "101 BC");
    CHECK_JS_STRING(in, "new Date(0).toLocaleString('en-US', { timeZone: '+01:00' }) + '|' + new Date(0).toLocaleDateString('en-GB', { timeZone: 'UTC' }) + '|' + new Date(0).toLocaleTimeString('en-US', { timeZone: '+01:00' })",
        "1/1/1970, 1:00:00 AM|01/01/1970|1:00:00 AM");
    CHECK_JS_STRING(in, "new Date(NaN).toLocaleString()", "Invalid Date");
    CHECK_JS_THROWS(in, "new Intl.DateTimeFormat('en', { timeZone: 'Mars/Olympus_Mons' })", "RangeError");
    CHECK_JS_THROWS(in, "new Intl.DateTimeFormat('en', { timeZone: '+24:00' })", "RangeError");
    CHECK_JS_THROWS(in, "new Intl.DateTimeFormat('en', { dateStyle: 'short', hour: 'numeric' })", "TypeError");
    CHECK_JS_THROWS(in, "new Intl.DateTimeFormat().format(NaN)", "RangeError");
    CHECK_JS_THROWS(in, "new Date(0).toLocaleDateString('en', { timeStyle: 'short' })", "TypeError");
}

void test_relative_time_and_lists()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "var r = new Intl.RelativeTimeFormat('en', { numeric: 'auto' }); [r.format(-1, 'day'), r.format(0, 'second'), r.format(2, 'weeks'), r.format(-3, 'quarter')].join('|')", "yesterday|now|in 2 weeks|3 quarters ago");
    CHECK_JS_STRING(in, "JSON.stringify(new Intl.RelativeTimeFormat('en').formatToParts(1000, 'day'))",
        "[{\"type\":\"literal\",\"value\":\"in \"},{\"type\":\"integer\",\"value\":\"1\",\"unit\":\"day\"},{\"type\":\"group\",\"value\":\",\",\"unit\":\"day\"},{\"type\":\"integer\",\"value\":\"000\",\"unit\":\"day\"},{\"type\":\"literal\",\"value\":\" days\"}]");
    CHECK_JS_THROWS(in, "new Intl.RelativeTimeFormat('en').format(1, 'fortnight')", "RangeError");
    CHECK_JS_STRING(in, "new Intl.ListFormat('en-US').format(['A', 'B', 'C']) + '|' + new Intl.ListFormat('en-GB').format(['A', 'B', 'C']) + '|' + new Intl.ListFormat('en', { type: 'unit', style: 'narrow' }).format(['A', 'B'])", "A, B, and C|A, B and C|A B");
    CHECK_JS_STRING(in, "JSON.stringify(new Intl.ListFormat('en', { type: 'disjunction' }).formatToParts(['a', 'b', 'c']))",
        "[{\"type\":\"element\",\"value\":\"a\"},{\"type\":\"literal\",\"value\":\", \"},{\"type\":\"element\",\"value\":\"b\"},{\"type\":\"literal\",\"value\":\", or \"},{\"type\":\"element\",\"value\":\"c\"}]");
    CHECK_JS_THROWS(in, "new Intl.ListFormat('en').format(['a', 1])", "TypeError");
    CHECK_JS_STRING(in, "new Intl.DurationFormat('en', { style: 'digital' }).format({ hours: 1, minutes: 2, seconds: 3, milliseconds: 40 }) + '|' + new Intl.DurationFormat('en', { style: 'long' }).format({ years: 1, days: 2, hours: 3 })", "1:02:03.04|1 year, 2 days, 3 hours");
    CHECK_JS_THROWS(in, "new Intl.DurationFormat().format({ hours: 1, minutes: -1 })", "RangeError");
}

void test_text_services()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "['b', 'A', 'a', '\\u00e1', 'B', '_', '10', '2'].sort(new Intl.Collator('en').compare).join('|')", "_|10|2|a|A|\xC3\xA1|b|B");
    CHECK_JS_STRING(in, "['10', '2', 'a', 'A'].sort(new Intl.Collator('en', { numeric: true, caseFirst: 'upper' }).compare).join('|')", "2|10|A|a");
    CHECK_JS_TRUE(in, "new Intl.Collator('en', { sensitivity: 'base' }).compare('a', '\\u00c1') === 0 && new Intl.Collator('en', { sensitivity: 'accent' }).compare('a', '\\u00e1') === -1 && 'a'.localeCompare('B') === -1 && '\\u00e4\\u0323'.localeCompare('a\\u0323\\u0308') === 0");
    CHECK_JS_STRING(in, "var o = new Intl.Collator('en-u-kn').resolvedOptions(); [o.locale, o.usage, o.sensitivity, o.numeric, o.caseFirst].join()", "en-u-kn,sort,variant,true,false");
    CHECK_JS_STRING(in, "var n = new Intl.DisplayNames('en', { type: 'language' }); [n.of('en-US'), n.of('zh-Hant'), new Intl.DisplayNames('en', { type: 'region' }).of('gb'), new Intl.DisplayNames('en', { type: 'currency' }).of('jpy')].join('|')",
        "American English|Traditional Chinese|United Kingdom|Japanese Yen");
    CHECK_JS_TRUE(in, "new Intl.DisplayNames('en', { type: 'region', fallback: 'none' }).of('QQ') === undefined && new Intl.DisplayNames('en', { type: 'region' }).of('QQ') === 'QQ'");
    CHECK_JS_THROWS(in, "new Intl.DisplayNames('en')", "TypeError");
    CHECK_JS_THROWS(in, "new Intl.DisplayNames('en', { type: 'region' }).of('1')", "RangeError");
    CHECK_JS_STRING(in, "[...new Intl.Segmenter('en', { granularity: 'word' }).segment('Hello, world! 3.14')].map(s => s.segment + (s.isWordLike ? '*' : '')).join('|')", "Hello*|,| |world*|!| |3.14*");
    CHECK_JS_NUMBER(in, "[...new Intl.Segmenter().segment('e\\u0301\\u{1F1FA}\\u{1F1F8}\\u{1F468}\\u200D\\u{1F469}x')].length", 4);
    CHECK_JS_STRING(in, "[...new Intl.Segmenter('en', { granularity: 'sentence' }).segment('Hi there. It is 3.5 now! ok.')].map(s => s.segment).join('|')", "Hi there. |It is 3.5 now! |ok.");
    CHECK_JS_STRING(in, "var s = new Intl.Segmenter('en', { granularity: 'word' }).segment('ab cd').containing(4); s.segment + s.index + s.input", "cd3ab cd");
    CHECK_JS_STRING(in, "'istanbul'.toLocaleUpperCase('tr') + '|' + 'I'.toLocaleLowerCase('tr') + '|' + 'I'.toLocaleLowerCase('en')", "\xC4\xB0STANBUL|\xC4\xB1|i");
    CHECK_JS_STRING(in, "[1234.5, new Date(0)].toLocaleString('en-GB', { timeZone: 'UTC' })", "1,234.5,01/01/1970, 00:00:00");
}

} // namespace

int main()
{
    test_intl_object();
    test_locale();
    test_number_format();
    test_plural_rules();
    test_date_time_format();
    test_relative_time_and_lists();
    test_text_services();
    return sashfold::test::report("js_intl");
}
