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

void test_parse()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "JSON.parse('1') === 1 && JSON.parse('\"s\"') === 's' && JSON.parse('true') === true && JSON.parse('null') === null && JSON.parse(' [1, 2] ')[1] === 2 && JSON.parse('-0.5e1') === -5");
    CHECK_JS_TRUE(in, "(function () { var o = JSON.parse('{\"a\": {\"b\": [1, {\"c\": \"d\"}]}}'); return o.a.b[1].c === 'd' && Object.keys(o).length === 1; })()");
    CHECK_JS_TRUE(in, "JSON.parse('\"\\\\u0041\\\\n\\\\\\\\\"') === 'A\\n\\\\' && JSON.parse('\"\\\\/\"') === '/' && JSON.parse('\"\\\\ud83d\\\\ude00\"').length === 2");
    CHECK_JS_TRUE(in, "Object.is(JSON.parse('-0'), -0) && JSON.parse('1e2') === 100 && JSON.parse('0') === 0 && JSON.parse('123456789012345678901234567890') === 1.2345678901234568e29");
    CHECK_JS_THROWS(in, "JSON.parse('')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('01')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('.5')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('1.')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('+1')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('[1,]')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('{\"a\":1,}')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse(\"{'a':1}\")", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('\"a\\tb\"')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('\"\\\\x41\"')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('undefined')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('NaN')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('[1] 2')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('\\u00a01')", "SyntaxError");
    CHECK_JS_THROWS(in, "JSON.parse('\"unterminated')", "SyntaxError");
    CHECK_JS_TRUE(in, "(function () { var o = JSON.parse('{\"a\": 1, \"a\": 2}'); return o.a === 2 && Object.keys(o).length === 1; })()");
    CHECK_JS_TRUE(in, "(function () { var o = JSON.parse('{\"__proto__\": 1}'); return Object.getPrototypeOf(o) === Object.prototype && Object.prototype.hasOwnProperty.call(o, '__proto__') && o.__proto__ === 1; })()");
    CHECK_JS_TRUE(in, "JSON.parse('[1, [2, [3]]]', function (k, v) { return typeof v === 'number' ? v * 2 : v; })[1][1][0] === 6");
    CHECK_JS_TRUE(in, "(function () { var o = JSON.parse('{\"a\": 1, \"b\": 2}', function (k, v) { return k === 'a' ? undefined : v; }); return !('a' in o) && o.b === 2; })()");
    CHECK_JS_TRUE(in, "(function () { var keys = []; JSON.parse('{\"a\": [1, {\"b\": 2}], \"c\": 3}', function (k, v) { keys.push(k); return v; }); return keys.join() === '0,b,1,a,c,'; })()");
    CHECK_JS_TRUE(in, "(function () { var holder; JSON.parse('1', function (k, v) { holder = this; return v; }); return typeof holder === 'object' && holder[''] === 1; })()");
    CHECK_JS_TRUE(in, "JSON.parse(1) === 1 && JSON.parse(true) === true && JSON.parse(null) === null && JSON.parse({ toString() { return '5'; } }) === 5");
    CHECK_JS_TRUE(in, "JSON.parse.length === 2 && JSON.stringify.length === 3 && JSON[Symbol.toStringTag] === 'JSON' && Object.prototype.toString.call(JSON) === '[object JSON]'");
    CHECK_JS_THROWS(in, "JSON.parse('[' .repeat(2000) + ']'.repeat(2000))", "RangeError");
}

void test_stringify()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "JSON.stringify(1) === '1' && JSON.stringify('s') === '\"s\"' && JSON.stringify(true) === 'true' && JSON.stringify(null) === 'null' && JSON.stringify(undefined) === undefined && JSON.stringify(function () {}) === undefined && JSON.stringify(Symbol()) === undefined");
    CHECK_JS_TRUE(in, "JSON.stringify({ a: 1, b: 'x', c: [1, null, true], d: {} }) === '{\"a\":1,\"b\":\"x\",\"c\":[1,null,true],\"d\":{}}'");
    CHECK_JS_TRUE(in, "JSON.stringify({ u: undefined, f: function () {}, s: Symbol(), n: 1 }) === '{\"n\":1}' && JSON.stringify([undefined, function () {}, Symbol()]) === '[null,null,null]'");
    CHECK_JS_TRUE(in, "JSON.stringify(NaN) === 'null' && JSON.stringify(Infinity) === 'null' && JSON.stringify(-0) === '0' && JSON.stringify(1e21) === '1e+21'");
    CHECK_JS_TRUE(in, "JSON.stringify('\\n\\t\"\\\\\\u0001') === '\"\\\\n\\\\t\\\\\"\\\\\\\\\\\\u0001\"' && JSON.stringify('\\ud800') === '\"\\\\ud800\"' && JSON.stringify('\\ud83d\\ude00') === '\"\\ud83d\\ude00\"' && JSON.stringify('\\u007f\\u2028') === '\"\\u007f\\u2028\"'");
    CHECK_JS_TRUE(in, "JSON.stringify(new Number(1)) === '1' && JSON.stringify(new String('s')) === '\"s\"' && JSON.stringify(new Boolean(false)) === 'false' && JSON.stringify([new Number(2)]) === '[2]'");
    CHECK_JS_TRUE(in, "JSON.stringify({ toJSON() { return 'custom'; } }) === '\"custom\"' && JSON.stringify({ a: { toJSON(k) { return k + '!'; } } }) === '{\"a\":\"a!\"}' && JSON.stringify({ toJSON() { return undefined; } }) === undefined");
    CHECK_JS_TRUE(in, "JSON.stringify({ a: 1, b: 2 }, function (k, v) { return k === 'a' ? undefined : v; }) === '{\"b\":2}' && JSON.stringify({ a: 1 }, function (k, v) { return typeof v === 'number' ? v + 1 : v; }) === '{\"a\":2}'");
    CHECK_JS_TRUE(in, "JSON.stringify({ a: 1, b: 2, c: 3 }, ['c', 'a', 'c']) === '{\"c\":3,\"a\":1}' && JSON.stringify({ 1: 'x', a: 'y' }, [1, new String('a')]) === '{\"1\":\"x\",\"a\":\"y\"}' && JSON.stringify([1, 2], ['0']) === '[1,2]'");
    CHECK_JS_TRUE(in, "JSON.stringify({ a: [1, { b: 2 }] }, null, 2) === '{\\n  \"a\": [\\n    1,\\n    {\\n      \"b\": 2\\n    }\\n  ]\\n}'");
    CHECK_JS_TRUE(in, "JSON.stringify([1], null, '--') === '[\\n--1\\n]' && JSON.stringify({ a: 1 }, null, 20) === '{\\n          \"a\": 1\\n}' && JSON.stringify({ a: 1 }, null, 'abcdefghijklmnop') === '{\\nabcdefghij\"a\": 1\\n}' && JSON.stringify({}, null, 2) === '{}' && JSON.stringify([], null, 2) === '[]'");
    CHECK_JS_TRUE(in, "JSON.stringify({ a: 1 }, null, new Number(1)) === '{\\n \"a\": 1\\n}' && JSON.stringify({ a: 1 }, null, 0) === '{\"a\":1}' && JSON.stringify({ a: 1 }, null, '') === '{\"a\":1}'");
    CHECK_JS_THROWS(in, "(function () { var o = {}; o.self = o; JSON.stringify(o); })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { var a = []; a[0] = { a: a }; JSON.stringify(a); })()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var shared = { v: 1 }; return JSON.stringify([shared, shared]) === '[{\"v\":1},{\"v\":1}]'; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { b: 1, a: 2, 1: 3 }; Object.defineProperty(o, 'h', { value: 4 }); var s = Symbol(); o[s] = 5; return JSON.stringify(o) === '{\"1\":3,\"b\":1,\"a\":2}'; })()");
    CHECK_JS_TRUE(in, "JSON.stringify({ get g() { return 'got'; } }) === '{\"g\":\"got\"}' && JSON.stringify(Object.create({ inherited: 1 })) === '{}'");
    CHECK_JS_TRUE(in, "(function () { var order = []; JSON.stringify({ a: { b: 1 } }, function (k, v) { order.push(k); return v; }); return order.join() === ',a,b'; })()");
    CHECK_JS_TRUE(in, "(function () { var holder; JSON.stringify(1, function (k, v) { holder = this; return v; }); return holder[''] === 1; })()");
    CHECK_JS_TRUE(in, "JSON.stringify([, 1]) === '[null,1]' && JSON.stringify({ length: 1, 0: 'x' }) === '{\"0\":\"x\",\"length\":1}' && JSON.stringify(new Date(0)) === '\"1970-01-01T00:00:00.000Z\"'");
    CHECK_JS_TRUE(in, "JSON.parse(JSON.stringify({ a: [1, 'two', { three: null }], b: 'q\"uote' })).b === 'q\"uote'");
}

} // namespace

int main()
{
    test_parse();
    test_stringify();
    return sashfold::test::report("js_json");
}
