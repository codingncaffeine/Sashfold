#include "JsTest.h"

#include "js/Interpreter.h"

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

// The number-to-string paths through script: the Number library's own
// tests are in test_js_runtime; these are the conversion edges pages hit.
void test_number_strings()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "String(123456789)", "123456789");
    CHECK_JS_STRING(in, "String(0.1)", "0.1");
    CHECK_JS_STRING(in, "String(1e21)", "1e+21");
    CHECK_JS_STRING(in, "String(1e-7)", "1e-7");
    CHECK_JS_STRING(in, "String(123e-20)", "1.23e-18");
    CHECK_JS_STRING(in, "String(-1.5)", "-1.5");
    CHECK_JS_STRING(in, "String(2 ** 53)", "9007199254740992");
    CHECK_JS_STRING(in, "String(1 / 3)", "0.3333333333333333");
    CHECK_JS_STRING(in, "String(5e-324)", "5e-324");
    CHECK_JS_STRING(in, "String(1.7976931348623157e308)", "1.7976931348623157e+308");
    CHECK_JS_STRING(in, "(0.000001).toString()", "0.000001");
    CHECK_JS_STRING(in, "(100).toString(36)", "2s");
    CHECK_JS_STRING(in, "(-0.5).toString(2)", "-0.1");
    CHECK_JS_STRING(in, "(1e21).toString(16)", "3635c9adc5dea00000");
    CHECK_JS_STRING(in, "(12345.6789).toFixed(2)", "12345.68");
    CHECK_JS_STRING(in, "(0.5).toFixed(0)", "1");
    CHECK_JS_STRING(in, "(1.45).toFixed(1)", "1.4");
    CHECK_JS_STRING(in, "(-0).toFixed(2)", "0.00");
    CHECK_JS_STRING(in, "(1000000000000000128).toFixed(0)", "1000000000000000128");
    CHECK_JS_STRING(in, "(1.5e-10).toExponential(3)", "1.500e-10");
    CHECK_JS_STRING(in, "(0).toPrecision(3)", "0.00");
    CHECK_JS_STRING(in, "(123.456).toPrecision(2)", "1.2e+2");
    CHECK_JS_STRING(in, "(0.00001).toPrecision(1)", "0.00001");
    CHECK_JS_STRING(in, "(0.000001).toPrecision(1)", "0.000001");
    CHECK_JS_STRING(in, "(0.0000001).toPrecision(1)", "1e-7");
    CHECK_JS_TRUE(in, "Number('12.5e-1') === 1.25 && Number('  0x1F  ') === 31 && Number('-0x1F') !== Number('-0x1F') && Number('1e') !== Number('1e') && Number('.5') === 0.5 && Number('5.') === 5 && Number('Infinity') === Infinity && Number('infinity') !== Number('infinity')");
    CHECK_JS_TRUE(in, "parseInt('123abc') === 123 && parseInt('  0x10  ') === 16 && parseInt('0x10', 10) === 0 && parseInt('-0') === 0 && Object.is(parseInt('-0'), -0) && parseInt('12', 0) === 12 && parseInt('0b1') === 0 && parseInt('1e3') === 1");
    CHECK_JS_TRUE(in, "parseFloat('1e3x') === 1000 && parseFloat('0x10') === 0 && parseFloat('  .5.5') === 0.5 && parseFloat('-.5') === -0.5 && Object.is(parseFloat('-0'), -0) && parseFloat('Infinityx') === Infinity && parseFloat('1e') === 1 && parseFloat('1e+') === 1");
    CHECK_JS_TRUE(in, "(255).toString(16) === 'ff' && (255).toString(16).toUpperCase() === 'FF' && (0.1 + 0.2).toFixed(10) === '0.3000000000' && (25).toString(2) === '11001'");
    CHECK_JS_TRUE(in, "Number.MAX_SAFE_INTEGER + 2 === Number.MAX_SAFE_INTEGER + 1 && 0.1 * 3 !== 0.3 && 9007199254740993 === 9007199254740992");
}

} // namespace

int main()
{
    test_number_strings();
    return sashfold::test::report("js_number");
}
