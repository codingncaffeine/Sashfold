#include "JsTest.h"

#include "js/Interpreter.h"
#include "js/Runtime.h"

#include <string>

using namespace sashfold;

namespace {

// The tests pin the clock and the zone: a fixed instant, and a zone one
// hour east of UTC that observes no daylight time, so every expected
// string is the same on every machine.
double fixed_now()
{
    return 1788000000000.0; // 2026-08-29T15:33:20.000Z
}

double fixed_offset(double)
{
    return 60;
}

js::Interpreter& fresh()
{
    static js::Interpreter* interpreter = nullptr;
    delete interpreter;
    js::set_time_source(fixed_now, fixed_offset);
    interpreter = new js::Interpreter();
    interpreter->heap().set_stress(true);
    return *interpreter;
}

void test_construction_and_parsing()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "Date.now() === 1788000000000 && new Date().getTime() === 1788000000000 && typeof Date() === 'string' && Date(1, 2, 3).length > 10");
    CHECK_JS_TRUE(in, "new Date(0).getTime() === 0 && new Date(1.9).getTime() === 1 && new Date(-1).getTime() === -1 && Object.is(new Date(-0).getTime(), 0)");
    CHECK_JS_TRUE(in, "Number.isNaN(new Date(NaN).getTime()) && Number.isNaN(new Date(8.64e15 + 1).getTime()) && new Date(8.64e15).getTime() === 8.64e15 && Number.isNaN(new Date('garbage').getTime())");
    CHECK_JS_TRUE(in, "new Date('2026-09-04').getTime() === 1788480000000 && new Date('2026-09-04T00:00:00Z').getTime() === 1788480000000 && new Date('2026-09-04T00:00:00').getTime() === 1788480000000 - 3600000");
    CHECK_JS_TRUE(in, "new Date('2026-09-04T01:02:03.456+02:00').getTime() === 1788480000000 + 3723456 - 7200000 && new Date('2026-09').getTime() === 1788220800000 && new Date('2026').getTime() === 1767225600000");
    CHECK_JS_TRUE(in, "new Date('+002026-09-04T00:00:00.000Z').getTime() === 1788480000000 && new Date('-000001-01-01T00:00:00Z').getUTCFullYear() === -1 && Number.isNaN(Date.parse('-000000-01-01T00:00:00Z'))");
    CHECK_JS_TRUE(in, "Number.isNaN(Date.parse('2026-13-01')) && Number.isNaN(Date.parse('2026-09-04T25:00')) && Date.parse('2026-09-04T24:00:00Z') === 1788566400000 && Number.isNaN(Date.parse('2026-09-04T24:00:01Z'))");
    CHECK_JS_TRUE(in, "Date.parse('Fri Sep 04 2026 10:00:00 GMT+0100') === 1788512400000 && Date.parse('Fri, 04 Sep 2026 09:00:00 GMT') === 1788512400000 && Date.parse('Sep 4, 2026') === 1788480000000 - 3600000 && Date.parse('4 Sep 2026 09:00:00 UTC') === 1788512400000");
    CHECK_JS_TRUE(in, "Date.parse('2026/09/04') === 1788476400000 && Date.parse('09/04/2026 10:00') === 1788512400000 && Date.parse('September 4, 2026 1:00 PM') === 1788523200000");
    CHECK_JS_TRUE(in, "(function () { var d = new Date(2026, 8, 4); return d.getFullYear() === 2026 && d.getMonth() === 8 && d.getDate() === 4 && d.getHours() === 0 && d.getTime() === 1788480000000 - 3600000; })()");
    CHECK_JS_TRUE(in, "new Date(2026, 8, 4, 10, 20, 30, 400).getTime() === 1788480000000 - 3600000 + 37230400 && new Date(99, 0).getFullYear() === 1999 && new Date(100, 0).getFullYear() === 100 && new Date(2026, 12, 1).getMonth() === 0");
    CHECK_JS_TRUE(in, "Date.UTC(2026, 8, 4) === 1788480000000 && Date.UTC(2026) === 1767225600000 && Date.UTC(70, 0) === 0 && Number.isNaN(Date.UTC()) && Date.UTC(2026, 8, 4, 10, 20, 30, 400) === 1788480000000 + 37230400");
    CHECK_JS_TRUE(in, "(function () { var d = new Date(1000); return new Date(d).getTime() === 1000 && new Date(new Date(NaN)).getTime() !== new Date(new Date(NaN)).getTime(); })()");
    CHECK_JS_TRUE(in, "new Date({ valueOf() { return 5; } }).getTime() === 5 && new Date({ toString() { return '1970-01-01T00:00:00.007Z'; }, valueOf: undefined }).getTime() === 7 && new Date(true).getTime() === 1");
    CHECK_JS_TRUE(in, "Object.prototype.toString.call(new Date()) === '[object Date]' && typeof new Date() === 'object' && new Date() instanceof Date && Date.prototype.constructor === Date && Date.length === 7");
    CHECK_JS_THROWS(in, "Date.prototype.getTime.call({})", "TypeError");
    CHECK_JS_THROWS(in, "Date.prototype.valueOf.call(1)", "TypeError");
}

void test_fields()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "(function () { var d = new Date(1788480000000 + 37230400); return d.getUTCFullYear() === 2026 && d.getUTCMonth() === 8 && d.getUTCDate() === 4 && d.getUTCDay() === 5 && d.getUTCHours() === 10 && d.getUTCMinutes() === 20 && d.getUTCSeconds() === 30 && d.getUTCMilliseconds() === 400; })()");
    CHECK_JS_TRUE(in, "(function () { var d = new Date(1788480000000 + 37230400); return d.getFullYear() === 2026 && d.getMonth() === 8 && d.getDate() === 4 && d.getDay() === 5 && d.getHours() === 11 && d.getMinutes() === 20 && d.getSeconds() === 30 && d.getMilliseconds() === 400 && d.getTimezoneOffset() === -60 && d.getYear() === 126; })()");
    CHECK_JS_TRUE(in, "(function () { var d = new Date(NaN); return Number.isNaN(d.getFullYear()) && Number.isNaN(d.getDay()) && Number.isNaN(d.getTimezoneOffset()) && Number.isNaN(d.valueOf()); })()");
    CHECK_JS_TRUE(in, "new Date(-1).getUTCFullYear() === 1969 && new Date(-1).getUTCMonth() === 11 && new Date(-1).getUTCDate() === 31 && new Date(-1).getUTCHours() === 23 && new Date(-1).getUTCMilliseconds() === 999");
    CHECK_JS_TRUE(in, "new Date(Date.UTC(2000, 1, 29)).getUTCDate() === 29 && new Date(Date.UTC(1900, 1, 29)).getUTCMonth() === 2 && new Date(Date.UTC(2024, 1, 29)).getUTCDate() === 29 && new Date(Date.UTC(-100, 0, 1)).getUTCFullYear() === -100");
    CHECK_JS_TRUE(in, "(function () { var d = new Date(0); d.setUTCFullYear(2026, 8, 4); return d.getTime() === 1788480000000 && d.setUTCHours(10, 20, 30, 400) === 1788480000000 + 37230400; })()");
    CHECK_JS_TRUE(in, "(function () { var d = new Date(0); d.setFullYear(2026, 8, 4); return d.getTime() === 1788480000000 && d.setHours(1) === 1788480000000 && d.setMinutes(30) === 1788480000000 + 1800000 && d.setSeconds(10, 5) === 1788480000000 + 1810005 && d.setMilliseconds(7) === 1788480000000 + 1810007; })()");
    CHECK_JS_TRUE(in, "(function () { var d = new Date(1788480000000); d.setDate(0); return d.getUTCDate() === 31 && d.getUTCMonth() === 7 && (d.setMonth(0) , d.getUTCMonth() === 0); })()");
    CHECK_JS_TRUE(in, "(function () { var d = new Date(NaN); return Number.isNaN(d.setDate(1)) && Number.isNaN(d.setMonth(1)) && Number.isNaN(d.setHours(1)) && d.setFullYear(2026) === 1767225600000 - 3600000 && d.getFullYear() === 2026; })()");
    CHECK_JS_TRUE(in, "(function () { var d = new Date(NaN); d.setUTCFullYear(2026); return d.getTime() === 1767225600000; })()");
    CHECK_JS_TRUE(in, "(function () { var d = new Date(0); return d.setTime(5) === 5 && d.getTime() === 5 && Number.isNaN(d.setTime('x')) && Number.isNaN(d.getTime()); })()");
    CHECK_JS_TRUE(in, "(function () { var d = new Date(0); d.setYear(99); return d.getUTCFullYear() === 1999 && (d.setYear(2026), d.getUTCFullYear() === 2026) && Number.isNaN(d.setYear(NaN)); })()");
    CHECK_JS_TRUE(in, "(function () { var log = ''; var d = new Date(NaN); d.setHours({ valueOf() { log += 'h'; return 1; } }, { valueOf() { log += 'm'; return 1; } }); return log === 'hm'; })()");
    CHECK_JS_TRUE(in, "(function () { var d = new Date(0); return Number.isNaN(d.setDate()) && Number.isNaN(d.getTime()); })()");
    CHECK_JS_TRUE(in, "Date.prototype.setUTCHours.length === 4 && Date.prototype.setSeconds.length === 2 && Date.prototype.setDate.length === 1 && Date.prototype.setFullYear.length === 3");
}

void test_formatting()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "new Date(1788480000000 + 37230400).toISOString()", "2026-09-04T10:20:30.400Z");
    CHECK_JS_STRING(in, "new Date(-62198755200000).toISOString()", "-000001-01-01T00:00:00.000Z");
    CHECK_JS_STRING(in, "new Date(253402300800000).toISOString()", "+010000-01-01T00:00:00.000Z");
    CHECK_JS_THROWS(in, "new Date(NaN).toISOString()", "RangeError");
    CHECK_JS_STRING(in, "new Date(1788480000000 + 37230400).toString()", "Fri Sep 04 2026 11:20:30 GMT+0100");
    CHECK_JS_STRING(in, "new Date(1788480000000 + 37230400).toUTCString()", "Fri, 04 Sep 2026 10:20:30 GMT");
    CHECK_JS_STRING(in, "new Date(1788480000000 + 37230400).toDateString()", "Fri Sep 04 2026");
    CHECK_JS_STRING(in, "new Date(1788480000000 + 37230400).toTimeString()", "11:20:30 GMT+0100");
    CHECK_JS_STRING(in, "new Date(NaN).toString()", "Invalid Date");
    CHECK_JS_STRING(in, "new Date(NaN).toUTCString()", "Invalid Date");
    CHECK_JS_STRING(in, "String(new Date(NaN))", "Invalid Date");
    CHECK_JS_TRUE(in, "Date.prototype.toGMTString === Date.prototype.toUTCString && new Date(0).toLocaleString() === new Date(0).toString() && new Date(0).toLocaleDateString() === 'Thu Jan 01 1970' && new Date(0).toLocaleTimeString() === '01:00:00 GMT+0100'");
    CHECK_JS_TRUE(in, "new Date(5).toJSON() === '1970-01-01T00:00:00.005Z' && new Date(NaN).toJSON() === null && Date.prototype.toJSON.call({ toISOString() { return 'custom'; }, valueOf() { return 1; } }) === 'custom'");
    CHECK_JS_TRUE(in, "new Date(0) + '' === 'Thu Jan 01 1970 01:00:00 GMT+0100' && new Date(0) - 0 === 0 && new Date(0) < new Date(1) && Number(new Date(7)) === 7 && `${new Date(0)}`.startsWith('Thu')");
    CHECK_JS_TRUE(in, "new Date(0)[Symbol.toPrimitive]('number') === 0 && typeof new Date(0)[Symbol.toPrimitive]('default') === 'string' && Date.prototype[Symbol.toPrimitive].length === 1");
    CHECK_JS_THROWS(in, "new Date(0)[Symbol.toPrimitive]('x')", "TypeError");
    CHECK_JS_THROWS(in, "Date.prototype[Symbol.toPrimitive].call(1, 'number')", "TypeError");
    CHECK_JS_TRUE(in, "Date.parse(new Date(1788480000000).toString()) === 1788480000000 && Date.parse(new Date(1788480000000).toUTCString()) === 1788480000000 && Date.parse(new Date(1788480000000).toISOString()) === 1788480000000");
    CHECK_JS_TRUE(in, "new Date(-62198755200000).toString().startsWith('Fri Jan 01 -0001') && new Date(-62198755200000).toUTCString() === 'Fri, 01 Jan -0001 00:00:00 GMT'");
    CHECK_JS_TRUE(in, "new Date(1788480000000).getTimezoneOffset() === -60 && new Date(-9e14).getTimezoneOffset() === -60 && new Date(9e14).getTimezoneOffset() === -60");
}

} // namespace

int main()
{
    test_construction_and_parsing();
    test_fields();
    test_formatting();
    return sashfold::test::report("js_date");
}
