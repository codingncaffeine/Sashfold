#include "JsTest.h"

#include "js/Interpreter.h"
#include "js/Object.h"

#include <cmath>
#include <string>
#include <string_view>

using namespace sashfold;

namespace {

// Every realm here runs under heap stress: a collection at every
// allocation, so an unrooted value fails the first time.
js::Interpreter& fresh()
{
    static js::Interpreter* interpreter = nullptr;
    delete interpreter;
    interpreter = new js::Interpreter();
    interpreter->heap().set_stress(true);
    return *interpreter;
}

bool is_nan(js::Interpreter& in, std::string_view source)
{
    test::JsRun const run = test::run_js(in, source);
    return run.ok && run.value.is_number() && std::isnan(run.value.as_number());
}

bool is_negative_zero(js::Interpreter& in, std::string_view source)
{
    test::JsRun const run = test::run_js(in, source);
    return run.ok && run.value.is_number() && run.value.as_number() == 0 && std::signbit(run.value.as_number());
}

bool is_undefined(js::Interpreter& in, std::string_view source)
{
    test::JsRun const run = test::run_js(in, source);
    return run.ok && run.value.is_undefined();
}

void test_arithmetic_and_coercion()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "1 + 2", 3);
    CHECK_JS_NUMBER(in, "7 - 10", -3);
    CHECK_JS_NUMBER(in, "6 * 7", 42);
    CHECK_JS_NUMBER(in, "1 / 4", 0.25);
    CHECK_JS_NUMBER(in, "7 % 3", 1);
    CHECK_JS_NUMBER(in, "-7 % 3", -1);
    CHECK_JS_NUMBER(in, "2 ** 10", 1024);
    CHECK_JS_NUMBER(in, "2 ** 3 ** 2", 512);
    CHECK_JS_NUMBER(in, "(-2) ** 2", 4);
    CHECK(is_nan(in, "1 ** Infinity"));
    CHECK(is_nan(in, "(-1) ** Infinity"));
    CHECK_JS_NUMBER(in, "NaN ** 0", 1);
    CHECK_JS_NUMBER(in, "'5' * '2'", 10);
    CHECK_JS_STRING(in, "'5' + 2", "52");
    CHECK_JS_NUMBER(in, "'5' - 2", 3);
    CHECK_JS_NUMBER(in, "true + true", 2);
    CHECK_JS_NUMBER(in, "1 + null", 1);
    CHECK(is_nan(in, "1 + undefined"));
    CHECK_JS_STRING(in, "[] + {}", "[object Object]");
    CHECK_JS_STRING(in, "[] + []", "");
    CHECK_JS_STRING(in, "[1, 2] + ''", "1,2");
    CHECK_JS_STRING(in, "1 + '' + 2", "12");
    CHECK(is_nan(in, "0 / 0"));
    CHECK_JS_NUMBER(in, "1 / 0", INFINITY);
    CHECK_JS_NUMBER(in, "-1 / 0", -INFINITY);
    CHECK(is_negative_zero(in, "-0"));
    CHECK(is_negative_zero(in, "0 * -1"));
    CHECK(is_negative_zero(in, "-7 % 7"));
    CHECK_JS_NUMBER(in, "+'  12  '", 12);
    CHECK_JS_NUMBER(in, "+'0x10'", 16);
    CHECK(is_nan(in, "+'1_0'"));
    CHECK_JS_NUMBER(in, "+''", 0);
    CHECK_JS_NUMBER(in, "+[]", 0);
    CHECK_JS_NUMBER(in, "+[5]", 5);
    CHECK(is_nan(in, "+[1, 2]"));
    CHECK_JS_NUMBER(in, "+true", 1);
    CHECK_JS_NUMBER(in, "-'3'", -3);
    CHECK_JS_NUMBER(in, "~5", -6);
    CHECK_JS_NUMBER(in, "~~3.7", 3);
    CHECK_JS_TRUE(in, "!0");
    CHECK_JS_FALSE(in, "!'a'");
    CHECK_JS_TRUE(in, "!!{}");
    CHECK_JS_NUMBER(in, "0.1 + 0.2", 0.30000000000000004);
    CHECK_JS_STRING(in, "typeof (1 + '1')", "string");
    CHECK_JS_STRING(in, "String(-0)", "0");
    CHECK_JS_STRING(in, "'' + 1e21", "1e+21");
    CHECK_JS_STRING(in, "'' + 123456789012345680000", "123456789012345680000");
    CHECK_JS_STRING(in, "'' + 0.000001", "0.000001");
    CHECK_JS_STRING(in, "'' + 1e-7", "1e-7");
}

void test_comparison_and_equality()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "1 == 1");
    CHECK_JS_TRUE(in, "'1' == 1");
    CHECK_JS_TRUE(in, "1 == '1'");
    CHECK_JS_FALSE(in, "'1' === 1");
    CHECK_JS_TRUE(in, "null == undefined");
    CHECK_JS_FALSE(in, "null === undefined");
    CHECK_JS_FALSE(in, "null == 0");
    CHECK_JS_FALSE(in, "null == false");
    CHECK_JS_FALSE(in, "undefined == 0");
    CHECK_JS_TRUE(in, "'' == 0");
    CHECK_JS_TRUE(in, "'0' == false");
    CHECK_JS_TRUE(in, "[] == false");
    CHECK_JS_TRUE(in, "[0] == false");
    CHECK_JS_TRUE(in, "[1] == 1");
    CHECK_JS_TRUE(in, "[1,2] == '1,2'");
    CHECK_JS_TRUE(in, "({}) == '[object Object]'");
    CHECK_JS_FALSE(in, "NaN == NaN");
    CHECK_JS_FALSE(in, "NaN === NaN");
    CHECK_JS_TRUE(in, "NaN != NaN");
    CHECK_JS_TRUE(in, "0 === -0");
    CHECK_JS_TRUE(in, "0 == -0");
    CHECK_JS_FALSE(in, "Symbol() == Symbol()");
    CHECK_JS_TRUE(in, "(function () { var s = Symbol(); return s == s; })()");
    CHECK_JS_TRUE(in, "new Number(5) == 5");
    CHECK_JS_FALSE(in, "new Number(5) === 5");
    CHECK_JS_FALSE(in, "new Number(5) == new Number(5)");
    CHECK_JS_TRUE(in, "(function () { var o = {}; return o == o && o === o; })()");
    CHECK_JS_FALSE(in, "({}) == ({})");
    CHECK_JS_TRUE(in, "'a' < 'b'");
    CHECK_JS_TRUE(in, "'10' < '9'");
    CHECK_JS_FALSE(in, "'10' < 9");
    CHECK_JS_TRUE(in, "null < 1");
    CHECK_JS_FALSE(in, "undefined < 1");
    CHECK_JS_FALSE(in, "undefined >= 1");
    CHECK_JS_FALSE(in, "({}) < ({})");
    CHECK_JS_FALSE(in, "[2] < [10]");
    CHECK_JS_TRUE(in, "[2] > [10]");
    CHECK_JS_TRUE(in, "2 <= 2");
    CHECK_JS_TRUE(in, "2 >= 2");
    CHECK_JS_FALSE(in, "NaN <= NaN");
    CHECK_JS_FALSE(in, "NaN >= 0");
    CHECK_JS_TRUE(in, "'B' < 'a'");
    CHECK_JS_TRUE(in, "'abc' < 'abd'");
    CHECK_JS_TRUE(in, "'ab' < 'abc'");
    // Coercion order: the left operand converts first, for > as well.
    CHECK_JS_STRING(in, "(function () { var log = ''; var a = { valueOf: function () { log += 'a'; return 1; } }; var b = { valueOf: function () { log += 'b'; return 2; } }; a < b; a > b; return log; })()", "abab");
}

void test_bitwise_and_shifts()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "5 & 3", 1);
    CHECK_JS_NUMBER(in, "5 | 3", 7);
    CHECK_JS_NUMBER(in, "5 ^ 3", 6);
    CHECK_JS_NUMBER(in, "1 << 31", -2147483648.0);
    CHECK_JS_NUMBER(in, "1 << 32", 1);
    CHECK_JS_NUMBER(in, "-8 >> 1", -4);
    CHECK_JS_NUMBER(in, "-8 >>> 1", 2147483644);
    CHECK_JS_NUMBER(in, "-1 >>> 0", 4294967295.0);
    CHECK_JS_NUMBER(in, "2147483648 | 0", -2147483648.0);
    CHECK_JS_NUMBER(in, "4294967296.5 | 0", 0);
    CHECK_JS_NUMBER(in, "1e21 | 0", -559939584);
    CHECK_JS_NUMBER(in, "NaN | 0", 0);
    CHECK_JS_NUMBER(in, "'12' | 0", 12);
    CHECK_JS_NUMBER(in, "3.9 << 0", 3);
    CHECK_JS_NUMBER(in, "-3.9 >> 0", -3);
    CHECK_JS_NUMBER(in, "1 << -1", -2147483648.0);
}

void test_typeof_and_delete()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "typeof undefined", "undefined");
    CHECK_JS_STRING(in, "typeof null", "object");
    CHECK_JS_STRING(in, "typeof true", "boolean");
    CHECK_JS_STRING(in, "typeof 1", "number");
    CHECK_JS_STRING(in, "typeof 'x'", "string");
    CHECK_JS_STRING(in, "typeof Symbol()", "symbol");
    CHECK_JS_STRING(in, "typeof {}", "object");
    CHECK_JS_STRING(in, "typeof []", "object");
    CHECK_JS_STRING(in, "typeof function () {}", "function");
    CHECK_JS_STRING(in, "typeof (() => 1)", "function");
    CHECK_JS_STRING(in, "typeof Object", "function");
    CHECK_JS_STRING(in, "typeof notDeclaredAnywhere", "undefined");
    CHECK_JS_STRING(in, "typeof typeof 1", "string");
    CHECK_JS_THROWS(in, "notDeclaredAnywhere", "ReferenceError");
    CHECK_JS_TRUE(in, "(function () { var o = { a: 1 }; return delete o.a && !('a' in o); })()");
    CHECK_JS_TRUE(in, "(function () { var o = { a: 1 }; return delete o.b; })()");
    CHECK_JS_TRUE(in, "delete 1");
    CHECK_JS_TRUE(in, "delete notDeclaredAnywhere");
    CHECK_JS_FALSE(in, "(function () { var x = 1; return delete x; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { a: 1 }; var k = 'a'; return delete o[k] && o.a === undefined; })()");
    CHECK_JS_FALSE(in, "delete Object.prototype");
    CHECK_JS_THROWS(in, "'use strict'; delete Object.prototype", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; delete a[1]; return a.length === 3 && !(1 in a); })()");
    CHECK_JS_TRUE(in, "globalVar1 = 1; delete globalVar1");
    CHECK_JS_STRING(in, "typeof globalVar1", "undefined");
    CHECK_JS_TRUE(in, "var globalVar2 = 1; delete globalVar2 === false");
}

void test_closures_and_hoisting()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "(function () { function counter() { var n = 0; return function () { return ++n; }; } var c = counter(); c(); c(); return c(); })()", 3);
    CHECK_JS_NUMBER(in, "(function () { var c1 = (function () { var n = 0; return function () { return ++n; }; })(); var c2 = (function () { var n = 10; return function () { return ++n; }; })(); c1(); return c1() + c2(); })()", 13);
    CHECK_JS_STRING(in, "(function () { var fs = []; for (let i = 0; i < 3; i++) fs[i] = function () { return i; }; return fs[0]() + '' + fs[1]() + fs[2](); })()", "012");
    CHECK_JS_STRING(in, "(function () { var fs = []; for (var i = 0; i < 3; i++) fs[i] = function () { return i; }; return fs[0]() + '' + fs[1]() + fs[2](); })()", "333");
    CHECK_JS_NUMBER(in, "(function () { return hoisted(); function hoisted() { return 5; } })()", 5);
    CHECK_JS_TRUE(in, "(function () { var before = typeof x; var x = 1; return before === 'undefined'; })()");
    CHECK_JS_THROWS(in, "(function () { x; let x = 1; })()", "ReferenceError");
    CHECK_JS_THROWS(in, "(function () { typeof x; let x = 1; })()", "ReferenceError");
    CHECK_JS_THROWS(in, "(function () { const c = 1; c = 2; })()", "TypeError");
    CHECK_JS_THROWS(in, "const c = 1; c++", "TypeError");
    CHECK_JS_NUMBER(in, "(function () { let x = 1; { let x = 2; } return x; })()", 1);
    CHECK_JS_NUMBER(in, "(function () { let x = 1; { x = 2; } return x; })()", 2);
    CHECK_JS_NUMBER(in, "(function () { var x = 1; { var x = 2; } return x; })()", 2);
    CHECK_JS_THROWS(in, "(function () { { x = 1; let x; } })()", "ReferenceError");
    CHECK_JS_NUMBER(in, "(function () { function f() { return 1; } function f() { return 2; } return f(); })()", 2);
    CHECK_JS_NUMBER(in, "(function () { var f = 1; function f() {} return typeof f === 'number' ? 1 : 0; })()", 1);
    CHECK_JS_NUMBER(in, "(function () { let i = 0; for (let i = 5; i < 6; i++) {} return i; })()", 0);
    CHECK_JS_STRING(in, "(function () { var out = ''; for (let i = 0, j = 10; i < 2; i++, j--) out += i + ':' + j + ' '; return out; })()", "0:10 1:9 ");
    // The update expression runs in a fresh copy that saw the body's writes.
    CHECK_JS_STRING(in, "(function () { var fs = []; for (let i = 0; i < 3; i++) { fs[i] = function () { return i; }; i += 0; } return fs[0]() + '' + fs[2](); })()", "02");
    CHECK_JS_NUMBER(in, "(function () { var x = 10; function inner() { return x; } var x = 20; return inner(); })()", 20);
    CHECK_JS_NUMBER(in, "(function (a) { function a() {} return typeof a === 'function' ? 1 : 0; })(5)", 1);
    CHECK_JS_NUMBER(in, "(function (a) { var a; return a; })(5)", 5);
    CHECK_JS_NUMBER(in, "(function (a, a) { return a; })(1, 2)", 2);
}

void test_this_binding()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "(function () { var o = { v: 3, m: function () { return this.v; } }; return o.m(); })()", 3);
    CHECK_JS_TRUE(in, "(function () { return this === globalThis; })()");
    CHECK_JS_TRUE(in, "(function () { 'use strict'; return this === undefined; })()");
    CHECK_JS_TRUE(in, "this === globalThis");
    CHECK_JS_NUMBER(in, "(function () { var o = { v: 4, m: function () { var f = () => this.v; return f(); } }; return o.m(); })()", 4);
    CHECK_JS_NUMBER(in, "(function () { var o = { v: 5, m: function () { function inner() { return this; } return inner() === globalThis ? 5 : 0; } }; return o.m(); })()", 5);
    CHECK_JS_NUMBER(in, "(function () { function f() { return this.v; } return f.call({ v: 6 }); })()", 6);
    CHECK_JS_NUMBER(in, "(function () { function f(a, b) { return this.v + a + b; } return f.apply({ v: 1 }, [2, 3]); })()", 6);
    CHECK_JS_NUMBER(in, "(function () { function f(a) { return this.v + a; } var g = f.bind({ v: 7 }, 1); return g(); })()", 8);
    CHECK_JS_NUMBER(in, "(function () { function F() { this.v = 9; } return new F().v; })()", 9);
    CHECK_JS_STRING(in, "(function () { function f() { return typeof this; } return f.call(5) + f.call('s'); })()", "objectobject");
    CHECK_JS_STRING(in, "(function () { 'use strict'; function f() { return typeof this; } return f.call(5) + f.call(null); })()", "numberobject");
    CHECK_JS_TRUE(in, "(function () { var o = { m() { return this; } }; return o.m() === o; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { m() { return this; } }; var m = o.m; return m() === globalThis; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { m() { return this; } }; return (o.m)() === o; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { m() { return this; } }; return (0, o.m)() === globalThis; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { a: { m() { return this; } } }; return o.a.m() === o.a; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { m() { return this; } }; return o['m']() === o; })()");
}

void test_constructors_and_prototypes()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "(function () { function F() {} return new F() instanceof F; })()");
    CHECK_JS_TRUE(in, "(function () { function F() {} return F.prototype.constructor === F; })()");
    CHECK_JS_TRUE(in, "(function () { function F() {} return Object.getPrototypeOf(new F()) === F.prototype; })()");
    CHECK_JS_NUMBER(in, "(function () { function F() {} F.prototype.m = function () { return 11; }; return new F().m(); })()", 11);
    CHECK_JS_TRUE(in, "(function () { function F() { return { other: true }; } return new F().other === true; })()");
    CHECK_JS_TRUE(in, "(function () { function F() { this.a = 1; return 5; } return new F().a === 1; })()");
    CHECK_JS_TRUE(in, "(function () { function F() {} var f = new F; return f instanceof F; })()");
    CHECK_JS_TRUE(in, "(function () { function A() {} function B() {} B.prototype = Object.create(A.prototype); var b = new B(); return b instanceof A && b instanceof B && !(new A() instanceof B); })()");
    CHECK_JS_THROWS(in, "new (() => 1)()", "TypeError");
    CHECK_JS_THROWS(in, "new ({ m() {} }).m()", "TypeError");
    CHECK_JS_THROWS(in, "new 5", "TypeError");
    CHECK_JS_THROWS(in, "new Math.max()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { function F() {} var g = F.bind(null); return new g() instanceof F; })()");
    CHECK_JS_TRUE(in, "(function () { var o = Object.create({ get x() { return this; } }); return o.x === o; })()");
    CHECK_JS_TRUE(in, "(function () { var p = { set x(v) { this._x = v * 2; } }; var o = Object.create(p); o.x = 2; return o._x === 4 && !Object.prototype.hasOwnProperty.call(o, 'x'); })()");
    CHECK_JS_TRUE(in, "(function () { var o = {}; o.__proto__ = { inherited: 1 }; return o.inherited === 1; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { __proto__: null }; return Object.getPrototypeOf(o) === null; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { __proto__: 5 }; return Object.getPrototypeOf(o) === Object.prototype; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { ['__proto__']: 1 }; return o.__proto__ === 1 && Object.getPrototypeOf(o) === Object.prototype; })()");
}

void test_object_literals()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "({ a: 1, b: 2 }).b", 2);
    CHECK_JS_NUMBER(in, "(function () { var x = 3; return ({ x }).x; })()", 3);
    CHECK_JS_NUMBER(in, "({ get v() { return 4; } }).v", 4);
    CHECK_JS_NUMBER(in, "(function () { var o = { set v(x) { this.w = x + 1; } }; o.v = 4; return o.w; })()", 5);
    CHECK_JS_NUMBER(in, "(function () { var o = { get v() { return this._v; }, set v(x) { this._v = x * 2; } }; o.v = 3; return o.v; })()", 6);
    CHECK_JS_NUMBER(in, "(function () { var k = 'dyn'; return ({ [k + 'amic']: 7 }).dynamic; })()", 7);
    CHECK_JS_NUMBER(in, "({ 1: 'a', 2: 'b' })[2] === 'b' ? 1 : 0", 1);
    CHECK_JS_STRING(in, "({ 0.5: 'half' })['0.5']", "half");
    CHECK_JS_STRING(in, "(function () { var o = { named: function () {}, arrow: () => 1, method() {}, [ 'comp' + 'uted' ]() {} }; return o.named.name + ',' + o.arrow.name + ',' + o.method.name + ',' + o.computed.name; })()", "named,arrow,method,computed");
    CHECK_JS_STRING(in, "(function () { var o = { get g() {}, set s(v) {} }; var d = Object.getOwnPropertyDescriptor(o, 'g'); var e = Object.getOwnPropertyDescriptor(o, 's'); return d.get.name + ',' + e.set.name; })()", "get g,set s");
    CHECK_JS_STRING(in, "(function () { var s = Symbol('tag'); var o = { [s]: function () {} }; return o[s].name; })()", "[tag]");
    CHECK_JS_STRING(in, "Object.keys({ b: 1, a: 2, 1: 3, 0: 4 })[0]", "0");
    CHECK_JS_TRUE(in, "(function () { var o = { toString() { return 'custom'; } }; return o + '' === 'custom'; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { valueOf() { return 42; } }; return o + 1 === 43 && o * 2 === 84 && o + '' === '42'; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { [Symbol.toPrimitive](hint) { return hint === 'number' ? 1 : hint === 'string' ? 's' : 'd'; } }; return +o === 1 && `${o}` === 's' && o + '' === 'd'; })()");
    CHECK_JS_THROWS(in, "(function () { var o = { toString() { return {}; }, valueOf() { return {}; } }; return o + ''; })()", "TypeError");
}

void test_arrays_and_holes()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "[1, 2, 3].length", 3);
    CHECK_JS_NUMBER(in, "[1, , 3].length", 3);
    CHECK_JS_NUMBER(in, "[,].length", 1);
    CHECK_JS_NUMBER(in, "[1,].length", 1);
    CHECK_JS_NUMBER(in, "[1, , ].length", 2);
    CHECK_JS_FALSE(in, "1 in [1, , 3]");
    CHECK_JS_TRUE(in, "0 in [1, , 3]");
    CHECK_JS_NUMBER(in, "(function () { var a = []; a[5] = 1; return a.length; })()", 6);
    CHECK_JS_NUMBER(in, "(function () { var a = [1, 2, 3]; a.length = 1; return a.length + (a[1] === undefined ? 10 : 0); })()", 11);
    CHECK_JS_NUMBER(in, "[[1, 2], [3]][1][0]", 3);
    CHECK_JS_STRING(in, "typeof [].length", "number");
}

void test_arguments_object()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "(function () { return arguments.length; })(1, 2, 3)", 3);
    CHECK_JS_NUMBER(in, "(function (a) { return arguments[1]; })(1, 2, 3)", 2);
    CHECK_JS_NUMBER(in, "(function (a, b, c) { return arguments.length; })(1)", 1);
    CHECK_JS_TRUE(in, "(function f() { return arguments.callee === f; })()");
    CHECK_JS_THROWS(in, "(function () { 'use strict'; return arguments.callee; })()", "TypeError");
    CHECK_JS_NUMBER(in, "(function (a) { arguments[0] = 9; return a; })(1)", 9);
    CHECK_JS_NUMBER(in, "(function (a) { a = 9; return arguments[0]; })(1)", 9);
    CHECK_JS_NUMBER(in, "(function (a) { 'use strict'; arguments[0] = 9; return a; })(1)", 1);
    CHECK_JS_NUMBER(in, "(function (a) { 'use strict'; a = 9; return arguments[0]; })(1)", 1);
    CHECK_JS_NUMBER(in, "(function (a) { delete arguments[0]; arguments[0] = 5; return a; })(1)", 1);
    CHECK(is_undefined(in, "(function (a) { arguments[0] = 9; return a; })()"));
    CHECK_JS_NUMBER(in, "(function () { var f = () => arguments.length; return f(); })(1, 2)", 2);
    CHECK_JS_STRING(in, "Object.prototype.toString.call((function () { return arguments; })())", "[object Arguments]");
    CHECK_JS_TRUE(in, "(function () { return Object.getPrototypeOf(arguments) === Object.prototype; })()");
    CHECK_JS_NUMBER(in, "(function () { var arguments = 5; return arguments; })()", 5);
    CHECK_JS_NUMBER(in, "(function () { return Object.keys(arguments).length; })(1, 2)", 2);
    CHECK_JS_FALSE(in, "(function () { return Object.getOwnPropertyDescriptor(arguments, 'length').enumerable; })(1)");
    CHECK_JS_NUMBER(in, "(function (a, b) { arguments.length = 7; return arguments.length; })(1, 2)", 7);
}

void test_control_flow()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "(function () { var s = 0; for (var i = 0; i < 10; i++) { if (i % 2) continue; if (i > 6) break; s += i; } return s; })()", 12);
    CHECK_JS_NUMBER(in, "(function () { var i = 0; while (true) { if (++i === 5) break; } return i; })()", 5);
    CHECK_JS_NUMBER(in, "(function () { var i = 0; do { i++; } while (i < 3); return i; })()", 3);
    CHECK_JS_NUMBER(in, "(function () { var i = 0; do { i++; } while (false); return i; })()", 1);
    CHECK_JS_STRING(in, "(function () { var out = ''; outer: for (var i = 0; i < 3; i++) { for (var j = 0; j < 3; j++) { if (j === 1) continue outer; if (i === 2) break outer; out += i + '' + j; } } return out; })()", "0010");
    CHECK_JS_NUMBER(in, "(function () { block: { if (true) break block; return 1; } return 2; })()", 2);
    CHECK_JS_STRING(in, "(function (x) { switch (x) { case 1: return 'one'; case 2: case 3: return 'two-three'; default: return 'other'; } })(3)", "two-three");
    CHECK_JS_STRING(in, "(function (x) { var out = ''; switch (x) { case 1: out += 'a'; case 2: out += 'b'; break; case 3: out += 'c'; } return out; })(1)", "ab");
    CHECK_JS_STRING(in, "(function (x) { var out = ''; switch (x) { case 1: out += 'a'; default: out += 'd'; case 2: out += 'b'; } return out; })(5)", "db");
    CHECK_JS_STRING(in, "(function (x) { var out = ''; switch (x) { case 1: out += 'a'; default: out += 'd'; case 2: out += 'b'; } return out; })(2)", "b");
    CHECK_JS_STRING(in, "(function () { switch (1) { case '1': return 'loose'; case 1: return 'strict'; } })()", "strict");
    CHECK_JS_NUMBER(in, "(function () { var i = 0; switch (i) { case i++: case i++: return i; } })()", 1);
    CHECK_JS_THROWS(in, "(function () { switch (1) { case (function () { throw new RangeError('boom'); })(): } })()", "RangeError");
    CHECK_JS_NUMBER(in, "(function () { for (var i = 0; i < 5; i++) { switch (i) { case 2: continue; } if (i === 3) return i; } })()", 3);
    CHECK_JS_NUMBER(in, "(function () { var n = 0; for (;;) { if (n++ > 3) break; } return n; })()", 5);
    CHECK_JS_NUMBER(in, "(function () { var x = 0; if (x) return 1; else if (x === 0) return 2; else return 3; })()", 2);
    CHECK_JS_STRING(in, "(function () { var out = ''; a: b: for (var i = 0; i < 2; i++) { out += i; continue a; } return out; })()", "01");
}

void test_try_catch_finally()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "(function () { try { throw new Error('x'); } catch (e) { return e.message; } })()", "x");
    CHECK_JS_STRING(in, "(function () { var log = ''; try { log += 't'; return log; } finally { log += 'f'; } })()", "t");
    CHECK_JS_STRING(in, "(function () { var log = ''; function f() { try { log += 't'; return 'r'; } finally { log += 'f'; } } f(); return log; })()", "tf");
    CHECK_JS_STRING(in, "(function () { try { try { throw 1; } finally { return 'finally wins'; } } catch (e) { return 'caught'; } })()", "finally wins");
    CHECK_JS_THROWS(in, "(function () { try { throw 1; } catch (e) { throw 2; } finally { } })()", "2");
    CHECK_JS_THROWS(in, "(function () { try { throw 1; } finally { throw 2; } })()", "2");
    CHECK_JS_NUMBER(in, "(function () { var e = 1; try { throw 2; } catch (e) { e = 3; } return e; })()", 1);
    CHECK_JS_NUMBER(in, "(function () { try { throw 2; } catch { return 5; } })()", 5);
    CHECK_JS_NUMBER(in, "(function () { for (var i = 0; i < 3; i++) { try { continue; } finally { if (i === 1) return i; } } })()", 1);
    CHECK_JS_NUMBER(in, "(function () { for (var i = 0; i < 3; i++) { try { break; } finally { i = 10; } } return i; })()", 10);
    CHECK_JS_STRING(in, "(function () { try { null.x; } catch (e) { return e instanceof TypeError ? 'TypeError' : 'other'; } })()", "TypeError");
    CHECK_JS_STRING(in, "(function () { try { undefinedName; } catch (e) { return e.name + ':' + e.message; } })()", "ReferenceError:undefinedName is not defined");
    CHECK_JS_STRING(in, "(function () { try { null.prop; } catch (e) { return e.message; } })()", "Cannot read properties of null (reading 'prop')");
    CHECK_JS_STRING(in, "(function () { try { var o = {}; o.f(); } catch (e) { return e.message; } })()", "o.f is not a function");
    CHECK_JS_STRING(in, "(function () { try { (void 0)(); } catch (e) { return e.constructor.name; } })()", "TypeError");
    CHECK_JS_STRING(in, "(function () { try { throw 'plain'; } catch (e) { return typeof e + e; } })()", "stringplain");
    CHECK_JS_TRUE(in, "(function () { var caught; try { throw undefined; } catch (e) { caught = e === undefined; } return caught; })()");
    CHECK_JS_STRING(in, "(function () { try { return 'a'; } finally { } })()", "a");
    CHECK_JS_STRING(in, "(function () { var log = ''; try { try { throw 'inner'; } finally { log += 'f1'; } } catch (e) { log += e; } return log; })()", "f1inner");
}

void test_for_in()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "(function () { var out = ''; for (var k in { b: 1, a: 2, 1: 3, 0: 4 }) out += k; return out; })()", "01ba");
    CHECK_JS_STRING(in, "(function () { var out = ''; var p = { inherited: 1 }; var o = Object.create(p); o.own = 2; for (var k in o) out += k + ','; return out; })()", "own,inherited,");
    CHECK_JS_STRING(in, "(function () { var out = ''; var p = { x: 1 }; var o = Object.create(p); Object.defineProperty(o, 'x', { value: 2, enumerable: false }); for (var k in o) out += k; return out; })()", "");
    CHECK_JS_STRING(in, "(function () { var out = ''; var o = { a: 1, b: 2, c: 3 }; for (var k in o) { out += k; delete o.c; } return out; })()", "ab");
    CHECK_JS_STRING(in, "(function () { var out = ''; var o = { a: 1 }; for (var k in o) { out += k; o.z = 1; } return out; })()", "a");
    CHECK_JS_STRING(in, "(function () { var out = ''; for (var k in null) out += k; for (var k in undefined) out += k; return out + 'ok'; })()", "ok");
    CHECK_JS_STRING(in, "(function () { var out = ''; for (var k in 'ab') out += k; return out; })()", "01");
    CHECK_JS_STRING(in, "(function () { var out = ''; for (var k in [7, 8]) out += k + typeof k; return out; })()", "0string1string");
    CHECK_JS_STRING(in, "(function () { var out = ''; for (let k in { a: 1, b: 2 }) out += k; return out; })()", "ab");
    CHECK_JS_STRING(in, "(function () { var fs = []; for (let k in { a: 1, b: 2 }) fs[fs.length] = () => k; return fs[0]() + fs[1](); })()", "ab");
    CHECK_JS_STRING(in, "(function () { var o = {}; for (o.k in { z: 1 }) {} return o.k; })()", "z");
    CHECK_JS_STRING(in, "(function () { var out = ''; for (var k = 'init' in { a: 1 }) out += k; return out; })()", "a");
    CHECK_JS_THROWS(in, "(function () { for (let x in x) {} })()", "ReferenceError");
    CHECK_JS_STRING(in, "(function () { var out = ''; var o = { a: 1 }; var s = Symbol(); o[s] = 2; for (var k in o) out += typeof k; return out; })()", "string");
}

void test_with_and_eval()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "(function () { var o = { x: 1 }; with (o) { x = 2; } return o.x; })()", 2);
    CHECK_JS_NUMBER(in, "(function () { var o = { x: 1 }; var y = 0; with (o) { y = 5; } return y + (o.y === undefined ? 0 : 100); })()", 5);
    CHECK_JS_NUMBER(in, "(function () { var o = { x: 1, m: function () { return this.x; } }; with (o) { return m(); } })()", 1);
    CHECK_JS_NUMBER(in, "(function () { var x = 10; with ({}) { return x; } })()", 10);
    CHECK_JS_NUMBER(in, "(function () { var a = 1; return eval('a + 1'); })()", 2);
    CHECK_JS_NUMBER(in, "(function () { eval('var leaked = 3'); return leaked; })()", 3);
    CHECK_JS_STRING(in, "(function () { 'use strict'; eval('var kept = 3'); return typeof kept; })()", "undefined");
    CHECK_JS_STRING(in, "(function () { eval('\"use strict\"; var kept2 = 3'); return typeof kept2; })()", "undefined");
    CHECK_JS_NUMBER(in, "(function () { var a = 1; var f = eval; return f('typeof a') === 'undefined' ? 1 : 0; })()", 1);
    CHECK_JS_NUMBER(in, "(0, eval)('var indirectGlobal = 4'); indirectGlobal", 4);
    CHECK_JS_NUMBER(in, "eval('1; 2; 3')", 3);
    CHECK(is_undefined(in, "eval('1; if (true) {}')"));
    CHECK_JS_NUMBER(in, "eval('1; ;')", 1);
    CHECK_JS_NUMBER(in, "eval('1; { 2; }')", 2);
    CHECK_JS_NUMBER(in, "eval('var v = 1; v')", 1);
    CHECK_JS_TRUE(in, "eval(5) === 5");
    CHECK_JS_TRUE(in, "eval() === undefined");
    CHECK_JS_THROWS(in, "eval('1 +')", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { let x = 1; eval('var x = 2'); })()", "SyntaxError");
    CHECK_JS_NUMBER(in, "(function () { let x = 1; eval('x = 2'); return x; })()", 2);
    CHECK_JS_NUMBER(in, "(function () { return eval('(function () { return 7; })')(); })()", 7);
    CHECK_JS_TRUE(in, "(function () { var t = eval('this'); return t === globalThis; })()");
    CHECK_JS_TRUE(in, "(function () { return eval('this') === this; }).call({})");
    CHECK_JS_NUMBER(in, "(function (a) { eval('a = 2'); return a; })(1)", 2);
    CHECK_JS_NUMBER(in, "(function () { var f = new Function('a', 'b', 'return a + b'); return f(2, 3); })()", 5);
    CHECK_JS_STRING(in, "new Function('return 1').name", "anonymous");
    CHECK_JS_TRUE(in, "new Function('return this')() === globalThis");
    CHECK_JS_THROWS(in, "new Function('return }')", "SyntaxError");
    CHECK_JS_NUMBER(in, "Function('a', 'return a * 2')(4)", 8);
    CHECK_JS_NUMBER(in, "(function () { var deleted = eval('var ev = 1; delete ev'); return deleted ? 1 : 0; })()", 1);
}

void test_templates_and_optional_chaining()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "`a${1 + 1}b${'c'}`", "a2bc");
    CHECK_JS_STRING(in, "`${[1, 2]}`", "1,2");
    CHECK_JS_STRING(in, "`${{ toString() { return 'T'; } }}`", "T");
    CHECK_JS_STRING(in, "`line1\nline2`", "line1\nline2");
    CHECK_JS_THROWS(in, "`${Symbol()}`", "TypeError");
    CHECK(is_undefined(in, "(function () { var a = null; return a?.b; })()"));
    CHECK(is_undefined(in, "(function () { var a = null; return a?.b.c.d; })()"));
    CHECK(is_undefined(in, "(function () { var a = undefined; return a?.(); })()"));
    CHECK(is_undefined(in, "(function () { var a = null; return a?.[0]; })()"));
    CHECK(is_undefined(in, "(function () { var o = {}; return o.m?.(); })()"));
    CHECK_JS_NUMBER(in, "(function () { var o = { m() { return 3; } }; return o.m?.(); })()", 3);
    CHECK_JS_NUMBER(in, "(function () { var o = { a: { b: 4 } }; return o?.a?.b; })()", 4);
    CHECK_JS_THROWS(in, "(function () { var a = null; return (a?.b).c; })()", "TypeError");
    CHECK_JS_NUMBER(in, "(function () { var o = { v: 5, m() { return this.v; } }; return o?.m(); })()", 5);
    CHECK_JS_NUMBER(in, "(function () { var o = { v: 6, m() { return this.v; } }; return o.m?.(); })()", 6);
    CHECK_JS_TRUE(in, "(function () { var o = { a: 1 }; return delete o?.a && !('a' in o); })()");
    CHECK_JS_TRUE(in, "(function () { var a = null; return delete a?.b; })()");
    CHECK_JS_NUMBER(in, "(function () { var a = null; var b = 0; return a ?? b; })()", 0);
    CHECK_JS_NUMBER(in, "(function () { var a = 0; return a ?? 5; })()", 0);
    CHECK_JS_NUMBER(in, "(function () { var a = 0; return a || 5; })()", 5);
    CHECK_JS_NUMBER(in, "(function () { var a = null; a ?" "?= 3; a ?" "?= 4; return a; })()", 3);
    CHECK_JS_NUMBER(in, "(function () { var a = 1; a &&= 2; return a; })()", 2);
    CHECK_JS_NUMBER(in, "(function () { var a = 0; a &&= 2; return a; })()", 0);
    CHECK_JS_NUMBER(in, "(function () { var a = 0; a ||= 9; return a; })()", 9);
    CHECK_JS_NUMBER(in, "(function () { var calls = 0; var a = 1; a ||= (calls++, 2); return calls; })()", 0);
    CHECK_JS_STRING(in, "(function () { var f; f ?" "?= function () {}; return f.name; })()", "f");
    CHECK_JS_NUMBER(in, "(1, 2, 3)", 3);
    CHECK(is_undefined(in, "void 1"));
    CHECK_JS_NUMBER(in, "(function () { var o = { a: 1 }; return o?.['a']; })()", 1);
}

void test_assignment_semantics()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "(function () { var n = 0; var o = {}; function key() { n++; return 'k'; } o[key()] = 1; o[key()] += 1; return n + o.k * 10; })()", 22);
    CHECK_JS_NUMBER(in, "(function () { var x = 1; x = x + 1; return x; })()", 2);
    CHECK_JS_STRING(in, "(function () { var x = '5'; ++x; return typeof x + x; })()", "number6");
    CHECK_JS_NUMBER(in, "(function () { var x = 5; var y = x++; return y * 10 + x; })()", 56);
    CHECK_JS_NUMBER(in, "(function () { var x = 5; var y = --x; return y * 10 + x; })()", 44);
    CHECK_JS_NUMBER(in, "(function () { var o = { p: 1 }; o.p++; ++o.p; o.p += 2; return o.p; })()", 5);
    CHECK_JS_NUMBER(in, "(function () { var a = [1]; a[0] **= 3; a[0] -= 1; return a[0]; })()", 0);
    CHECK_JS_NUMBER(in, "(function () { var a, b, c; a = b = c = 7; return a + b + c; })()", 21);
    CHECK_JS_NUMBER(in, "(function () { var x = 1; x <<= 3; x |= 1; x ^= 3; x >>= 1; x >>>= 0; x &= 7; x %= 4; x /= 1; x *= 3; return x; })()", 3);
    CHECK_JS_THROWS(in, "(function () { 'use strict'; undeclaredStrict = 1; })()", "ReferenceError");
    CHECK_JS_NUMBER(in, "(function () { undeclaredSloppy = 4; return globalThis.undeclaredSloppy; })()", 4);
    CHECK_JS_TRUE(in, "(function () { undefined = 1; return undefined === void 0; })()");
    CHECK_JS_THROWS(in, "(function () { 'use strict'; undefined = 1; })()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { NaN = 1; return isNaN(NaN); })()");
    CHECK_JS_STRING(in, "(function () { var log = ''; var o = { get x() { log += 'g'; return 1; }, set x(v) { log += 's' + v; } }; o.x += 1; return log; })()", "gs2");
    CHECK_JS_STRING(in, "(function () { var f = function () {}; var g = () => {}; var h = function named() {}; return f.name + ',' + g.name + ',' + h.name; })()", "f,g,named");
    CHECK_JS_STRING(in, "(function () { var o = {}; o.f = function () {}; return o.f.name; })()", "");
    CHECK_JS_STRING(in, "(function () { let a = () => {}; const b = function () {}; return a.name + b.name; })()", "ab");
    // PutValue comes after the right-hand side: the TypeError for a
    // nullish base is thrown only once the value exists.
    CHECK_JS_NUMBER(in, "(function () { var order = ''; try { null.x = (order += 'r', 1); } catch (e) { order += 'c'; } return order === 'rc' ? 1 : 0; })()", 1);
    CHECK_JS_NUMBER(in, "(function () { var f = function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); }; return f(5); })()", 120);
    CHECK_JS_STRING(in, "(function () { var f = function named() { named = 1; return typeof named; }; return f(); })()", "function");
    CHECK_JS_THROWS(in, "(function () { 'use strict'; var f = function named() { named = 1; }; f(); })()", "TypeError");
    CHECK_JS_STRING(in, "(function () { var s = 'abc'; s.x = 1; return typeof s.x; })()", "undefined");
    CHECK_JS_THROWS(in, "(function () { 'use strict'; var s = 'abc'; s.x = 1; })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { 'use strict'; var o = Object.freeze({ a: 1 }); o.a = 2; })()", "TypeError");
    CHECK_JS_NUMBER(in, "(function () { var o = Object.freeze({ a: 1 }); o.a = 2; return o.a; })()", 1);
}

void test_global_scope()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "var gv = 1; gv", 1);
    CHECK_JS_TRUE(in, "Object.getOwnPropertyDescriptor(globalThis, 'gv').configurable === false");
    CHECK_JS_NUMBER(in, "let gl = 2; gl", 2);
    CHECK_JS_STRING(in, "typeof globalThis.gl", "undefined");
    CHECK_JS_NUMBER(in, "gl", 2);
    CHECK_JS_THROWS(in, "let gl = 3;", "SyntaxError");
    CHECK_JS_THROWS(in, "var gl = 3;", "SyntaxError");
    CHECK_JS_THROWS(in, "let gv = 3;", "SyntaxError");
    CHECK_JS_THROWS(in, "let undefined = 1;", "SyntaxError");
    CHECK_JS_NUMBER(in, "function gf() { return 4; } gf()", 4);
    CHECK_JS_TRUE(in, "Object.getOwnPropertyDescriptor(globalThis, 'gf').enumerable === true");
    CHECK_JS_NUMBER(in, "globalThis.shadowed = 1; let shadowed = 2; shadowed", 2);
    CHECK_JS_NUMBER(in, "globalThis.shadowed", 1);
    CHECK_JS_NUMBER(in, "eval('var evalVar = 5'); evalVar", 5);
    CHECK_JS_TRUE(in, "delete globalThis.evalVar");
    CHECK(is_undefined(in, "1; if (true) {}"));
    CHECK_JS_NUMBER(in, "2; ;", 2);
    CHECK_JS_NUMBER(in, "3; var unused;", 3);
    CHECK_JS_NUMBER(in, "4; function unusedFn() {}", 4);
    CHECK(is_undefined(in, "var x1;"));
    CHECK_JS_NUMBER(in, "5; { 6; }", 6);
    CHECK_JS_NUMBER(in, "7; try { 8; } finally { 9; }", 8);
    CHECK_JS_NUMBER(in, "10; do { 11; } while (false)", 11);
    CHECK_JS_NUMBER(in, "12; for (var i = 0; i < 2; i++) { i; }", 1);
    CHECK(is_undefined(in, "13; while (false) {}"));
    CHECK_JS_NUMBER(in, "{ let blockLet = 1; } typeof blockLet === 'undefined' ? 14 : 0", 14);
    CHECK_JS_NUMBER(in, "{ function blockFn() { return 15; } } blockFn()", 15);
    CHECK_JS_NUMBER(in, "if (true) function ifFn() { return 16; } ifFn()", 16);
}

void test_limits_and_termination()
{
    js::Interpreter& in = fresh();
    CHECK_JS_THROWS(in, "(function f() { return f(); })()", "RangeError");
    CHECK_JS_NUMBER(in, "(function f(n) { return n === 0 ? 0 : 1 + f(n - 1); })(300)", 300);
    // A runaway loop is stopped by the interrupt; the finally inside it
    // does not get to run, and the stop is reported as a RangeError.
    {
        js::Interpreter& stopped = fresh();
        int polls = 0;
        stopped.set_interrupt([&polls] { return ++polls >= 3; }, 1000);
        test::JsRun const run = test::run_js(stopped, "var ran = false; try { while (true) {} } finally { ran = true; } 'after'");
        CHECK(!run.ok);
        CHECK(stopped.terminated());
        CHECK(run.thrown.starts_with("RangeError"));
        test::JsRun const check = test::run_js(stopped, "ran");
        CHECK(check.ok && check.value.is_boolean() && !check.value.as_boolean());
    }
    {
        js::Interpreter& deep = fresh();
        deep.set_interrupt([] { return true; }, 100);
        test::JsRun const run = test::run_js(deep, "function f(n) { try { return n ? f(n - 1) : g(); } catch (e) { return 'caught'; } } function g() { while (true) {} } f(20)");
        CHECK(!run.ok);
        CHECK(deep.terminated());
    }
    // Two hundred thousand short strings: the collector keeps the heap
    // bounded and nothing in use is swept.
    {
        js::Interpreter& gc = fresh();
        gc.heap().set_stress(false);
        test::JsRun const run = test::run_js(gc, "var keep = []; for (var i = 0; i < 200000; i++) { var s = 'str' + i; if (i % 1000 === 0) keep[keep.length] = s; } keep.length + ':' + keep[3]");
        CHECK(run.ok);
        CHECK_EQ(run.value.is_string() ? run.value.as_string()->to_utf8() : std::string("?"), std::string("200:str3000"));
        CHECK(gc.heap().cell_count() < 100000);
        CHECK(gc.heap().collections() > 0);
    }
    {
        js::Interpreter& stress = fresh();
        test::JsRun const run = test::run_js(stress, "var fs = []; for (var i = 0; i < 2000; i++) { fs[i] = (function (k) { return function () { return k + i; }; })(i); } fs[1999]()");
        CHECK(run.ok);
        CHECK(run.value.is_number() && run.value.as_number() == 3999);
    }
}

void test_describe_and_errors()
{
    js::Interpreter& in = fresh();
    CHECK_EQ(test::eval_throws(in, "throw 5"), std::string("5"));
    CHECK_EQ(test::eval_throws(in, "throw 'text'"), std::string("text"));
    CHECK_EQ(test::eval_throws(in, "throw new TypeError('bad')"), std::string("TypeError: bad"));
    CHECK_EQ(test::eval_throws(in, "throw new Error()"), std::string("Error"));
    CHECK_EQ(test::eval_throws(in, "throw { get message() { throw 1; } }"), std::string("[object Object]"));
    CHECK_EQ(test::eval_throws(in, "throw null"), std::string("null"));
    CHECK_EQ(test::eval_throws(in, "var e = new RangeError('r'); e.name = 'Custom'; throw e"), std::string("Custom: r"));
    CHECK_EQ(test::eval_throws(in, "throw Symbol('s')"), std::string("Symbol(s)"));
    CHECK(test::eval_throws(in, "(").starts_with("SyntaxError"));
    CHECK(test::eval_throws(in, "async function f() {}").find("not supported") != std::string::npos);
    CHECK_EQ(test::eval_throws(in, "[].x.y"), std::string("TypeError: Cannot read properties of undefined (reading 'y')"));
    CHECK_EQ(test::eval_throws(in, "var u; u.p = 1"), std::string("TypeError: Cannot set properties of undefined (setting 'p')"));
    CHECK_EQ(test::eval_throws(in, "(function () { let z; z(); })()"), std::string("TypeError: z is not a function"));
    CHECK_EQ(test::eval_throws(in, "new (function () {}).x()"), std::string("TypeError: (function () {}).x is not a constructor"));
    CHECK_EQ(test::eval_throws(in, "'x' in 5"), std::string("TypeError: Cannot use 'in' operator to search for 'x' in 5"));
    CHECK_EQ(test::eval_throws(in, "1 instanceof 2"), std::string("TypeError: Right-hand side of 'instanceof' is not an object"));
    CHECK_EQ(test::eval_throws(in, "1 instanceof {}"), std::string("TypeError: Right-hand side of 'instanceof' is not callable"));
    CHECK_JS_TRUE(in, "(function () { try { throw new Error('m', { cause: 'c' }); } catch (e) { return e.cause === 'c' && e.stack === 'Error: m'; } })()");
}

void test_function_properties()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "(function (a, b, c) {}).length", 3);
    CHECK_JS_NUMBER(in, "((a) => 1).length", 1);
    CHECK_JS_STRING(in, "(function foo() {}).name", "foo");
    CHECK_JS_STRING(in, "Object.getOwnPropertyNames(function f(a) {})[0]", "length");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(function () {}, 'length'); return !d.writable && !d.enumerable && d.configurable; })()");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(function () {}, 'prototype'); return d.writable && !d.enumerable && !d.configurable; })()");
    CHECK_JS_TRUE(in, "(() => 1).prototype === undefined");
    CHECK_JS_TRUE(in, "({ m() {} }).m.prototype === undefined");
    CHECK_JS_STRING(in, "(function add(a, b) { return a + b; }).toString()", "function add(a, b) { return a + b; }");
    CHECK_JS_STRING(in, "(x => x * 2).toString()", "x => x * 2");
    CHECK_JS_STRING(in, "({ m(a) { return a; } }).m.toString()", "m(a) { return a; }");
    CHECK_JS_STRING(in, "Object.getOwnPropertyDescriptor({ get g() { return 1; } }, 'g').get.toString()", "get g() { return 1; }");
    CHECK_JS_STRING(in, "Math.max.toString()", "function max() { [native code] }");
    CHECK_JS_STRING(in, "(function () {}).bind().toString()", "function () { [native code] }");
    CHECK_JS_STRING(in, "(function named() {}).bind().name", "bound named");
    CHECK_JS_NUMBER(in, "(function (a, b, c) {}).bind(null, 1).length", 2);
    CHECK_JS_NUMBER(in, "(function (a) {}).bind(null, 1, 2, 3).length", 0);
    CHECK_JS_NUMBER(in, "(function () { function f(a, b) { return [this, a, b]; } var g = f.bind(1, 2); var r = g(3); return r[1] * 10 + r[2]; })()", 23);
    CHECK_JS_TRUE(in, "(function () { function F(a) { this.a = a; } var G = F.bind({ ignored: true }, 5); var g = new G(); return g.a === 5 && g instanceof F && g instanceof G; })()");
    CHECK_JS_STRING(in, "(function () { function f() {} var g = f.bind().bind(); return g.name; })()", "bound bound f");
    CHECK_JS_THROWS(in, "(function () { 'use strict'; return (function () {}).caller; })()", "TypeError");
    CHECK_JS_TRUE(in, "(function () {}).arguments === null && (function () {}).caller === null");
    CHECK_JS_THROWS(in, "(function () { 'use strict'; }).arguments", "TypeError");
    CHECK_JS_THROWS(in, "(() => 1).caller", "TypeError");
    CHECK_JS_TRUE(in, "Function.prototype() === undefined");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(function () {}) === Function.prototype");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(Function.prototype) === Object.prototype");
    CHECK_JS_TRUE(in, "Function.prototype[Symbol.hasInstance].call(Object, {})");
    CHECK_JS_TRUE(in, "(function () { var o = {}; Object.defineProperty(o, Symbol.hasInstance, { value: function () { return true; } }); return 1 instanceof o; })()");
}

void test_strict_mode_shapes()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "(function () { 'use strict'; return (function () { return this; })() === undefined; })()");
    CHECK_JS_THROWS(in, "'use strict'; with ({}) {}", "SyntaxError");
    CHECK_JS_THROWS(in, "'use strict'; var eval = 1;", "SyntaxError");
    CHECK_JS_THROWS(in, "'use strict'; 010", "SyntaxError");
    CHECK_JS_THROWS(in, "function f(a, a) { 'use strict'; }", "SyntaxError");
    CHECK_JS_THROWS(in, "'use strict'; delete x;", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { 'use strict'; var o = {}; Object.defineProperty(o, 'ro', { value: 1 }); o.ro = 2; })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { 'use strict'; var o = { get g() { return 1; } }; o.g = 2; })()", "TypeError");
    CHECK_JS_NUMBER(in, "(function () { var o = { get g() { return 1; } }; o.g = 2; return o.g; })()", 1);
    CHECK_JS_THROWS(in, "(function () { 'use strict'; var o = Object.preventExtensions({}); o.n = 1; })()", "TypeError");
    CHECK_JS_NUMBER(in, "(function () { var o = Object.preventExtensions({}); o.n = 1; return o.n === undefined ? 1 : 0; })()", 1);
}

} // namespace

int main()
{
    test_arithmetic_and_coercion();
    test_comparison_and_equality();
    test_bitwise_and_shifts();
    test_typeof_and_delete();
    test_closures_and_hoisting();
    test_this_binding();
    test_constructors_and_prototypes();
    test_object_literals();
    test_arrays_and_holes();
    test_arguments_object();
    test_control_flow();
    test_try_catch_finally();
    test_for_in();
    test_with_and_eval();
    test_templates_and_optional_chaining();
    test_assignment_semantics();
    test_global_scope();
    test_limits_and_termination();
    test_describe_and_errors();
    test_function_properties();
    test_strict_mode_shapes();
    return sashfold::test::report("js_interpreter");
}
