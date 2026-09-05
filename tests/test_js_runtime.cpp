#include "JsTest.h"

#include "js/Interpreter.h"
#include "js/Object.h"

#include <cmath>
#include <string>
#include <string_view>

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

bool is_nan(js::Interpreter& in, std::string_view source)
{
    test::JsRun const run = test::run_js(in, source);
    return run.ok && run.value.is_number() && std::isnan(run.value.as_number());
}

void test_object_statics()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "typeof Object === 'function' && Object.length === 1 && Object.name === 'Object'");
    CHECK_JS_TRUE(in, "Object(1) instanceof Number && Object('s') instanceof String && typeof Object(true) === 'object'");
    CHECK_JS_TRUE(in, "(function () { var o = {}; return Object(o) === o && new Object(o) === o; })()");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(Object()) === Object.prototype && Object.getPrototypeOf(new Object(null)) === Object.prototype");
    CHECK_JS_TRUE(in, "(function () { var t = { a: 1 }; var r = Object.assign(t, { b: 2 }, null, undefined, { c: 3 }); return r === t && t.a === 1 && t.b === 2 && t.c === 3; })()");
    CHECK_JS_TRUE(in, "(function () { var s = Symbol(); var src = { get g() { return 'got'; } }; src[s] = 1; var t = Object.assign({}, src); return t.g === 'got' && t[s] === 1; })()");
    CHECK_JS_THROWS(in, "Object.assign(undefined, {})", "TypeError");
    CHECK_JS_TRUE(in, "Object.assign(1, { a: 1 }) instanceof Number");
    CHECK_JS_TRUE(in, "(function () { var t = Object.freeze({ a: 1 }); try { Object.assign(t, { a: 2 }); return false; } catch (e) { return e instanceof TypeError; } })()");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(Object.create(null)) === null");
    CHECK_JS_TRUE(in, "(function () { var p = {}; var o = Object.create(p, { x: { value: 1, enumerable: true } }); return Object.getPrototypeOf(o) === p && o.x === 1 && Object.keys(o)[0] === 'x'; })()");
    CHECK_JS_THROWS(in, "Object.create(1)", "TypeError");
    CHECK_JS_THROWS(in, "Object.create(undefined)", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var o = {}; Object.defineProperty(o, 'x', { value: 1 }); var d = Object.getOwnPropertyDescriptor(o, 'x'); return d.value === 1 && d.writable === false && d.enumerable === false && d.configurable === false; })()");
    CHECK_JS_TRUE(in, "(function () { var o = {}; var g = function () { return 1; }; Object.defineProperty(o, 'x', { get: g, configurable: true }); var d = Object.getOwnPropertyDescriptor(o, 'x'); return d.get === g && d.set === undefined && d.configurable && !d.enumerable && !('value' in d) && !('writable' in d); })()");
    CHECK_JS_THROWS(in, "Object.defineProperty({}, 'x', { get: 1 })", "TypeError");
    CHECK_JS_THROWS(in, "Object.defineProperty({}, 'x', { get: function () {}, value: 1 })", "TypeError");
    CHECK_JS_THROWS(in, "Object.defineProperty({}, 'x', 1)", "TypeError");
    CHECK_JS_THROWS(in, "Object.defineProperty(1, 'x', {})", "TypeError");
    CHECK_JS_THROWS(in, "(function () { var o = {}; Object.defineProperty(o, 'x', { value: 1 }); Object.defineProperty(o, 'x', { value: 2 }); })()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var o = {}; Object.defineProperty(o, 'x', { value: 1 }); Object.defineProperty(o, 'x', { value: 1 }); return o.x === 1; })()");
    CHECK_JS_TRUE(in, "(function () { var proto = { value: 5, enumerable: true }; var desc = Object.create(proto); var o = {}; Object.defineProperty(o, 'x', desc); return o.x === 5 && Object.keys(o).length === 1; })()");
    CHECK_JS_TRUE(in, "(function () { var o = Object.defineProperties({}, { a: { value: 1, enumerable: true }, b: { get: function () { return 2; } } }); return o.a === 1 && o.b === 2 && Object.keys(o).length === 1; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { b: 1, a: 2, 1: 3, 0: 4 }; var e = Object.entries(o); return e.length === 4 && e[0][0] === '0' && e[0][1] === 4 && e[3][0] === 'a'; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { a: 1, b: 2 }; Object.defineProperty(o, 'h', { value: 3 }); return Object.values(o).length === 2 && Object.values(o)[1] === 2; })()");
    CHECK_JS_TRUE(in, "(function () { var o = Object.freeze({ a: 1 }); return Object.isFrozen(o) && Object.isSealed(o) && !Object.isExtensible(o); })()");
    CHECK_JS_TRUE(in, "(function () { var o = Object.seal({ a: 1 }); o.a = 2; return Object.isSealed(o) && !Object.isFrozen(o) && o.a === 2 && delete o.a === false; })()");
    CHECK_JS_TRUE(in, "(function () { var o = Object.preventExtensions({ a: 1 }); o.b = 1; return !Object.isExtensible(o) && o.b === undefined && !Object.isSealed(o); })()");
    CHECK_JS_TRUE(in, "Object.isFrozen(1) && Object.isSealed('s') && !Object.isExtensible(null) && Object.freeze(1) === 1");
    CHECK_JS_TRUE(in, "Object.isFrozen(Object.preventExtensions({}))");
    CHECK_JS_TRUE(in, "(function () { var o = Object.fromEntries([['a', 1], ['b', 2]]); return o.a === 1 && o.b === 2; })()");
    CHECK_JS_THROWS(in, "Object.fromEntries([1])", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var o = { a: 1 }; var d = Object.getOwnPropertyDescriptors(o); return d.a.value === 1 && d.a.writable && d.a.enumerable && d.a.configurable; })()");
    CHECK_JS_TRUE(in, "Object.getOwnPropertyDescriptor('abc', 'length').value === 3 && Object.getOwnPropertyDescriptor('abc', 1).value === 'b'");
    CHECK_JS_TRUE(in, "Object.getOwnPropertyDescriptor({}, 'nope') === undefined");
    CHECK_JS_TRUE(in, "(function () { var o = { b: 1, 2: 1, a: 1, 1: 1 }; Object.defineProperty(o, 'h', { value: 1 }); var n = Object.getOwnPropertyNames(o); return n.length === 5 && n[0] === '1' && n[1] === '2' && n[2] === 'b' && n[3] === 'a' && n[4] === 'h'; })()");
    CHECK_JS_TRUE(in, "(function () { var s = Symbol('k'); var o = { a: 1 }; o[s] = 2; return Object.keys(o).length === 1 && Object.getOwnPropertyNames(o).length === 1 && Object.getOwnPropertySymbols(o).length === 1 && Object.getOwnPropertySymbols(o)[0] === s; })()");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(1) === Number.prototype && Object.getPrototypeOf('') === String.prototype");
    CHECK_JS_THROWS(in, "Object.getPrototypeOf(null)", "TypeError");
    CHECK_JS_TRUE(in, "Object.hasOwn({ a: 1 }, 'a') && !Object.hasOwn({}, 'toString')");
    CHECK_JS_TRUE(in, "Object.is(NaN, NaN) && !Object.is(0, -0) && Object.is(-0, -0) && Object.is('a', 'a') && !Object.is({}, {})");
    CHECK_JS_TRUE(in, "(function () { var o = {}; var p = { inherited: 1 }; return Object.setPrototypeOf(o, p) === o && o.inherited === 1; })()");
    CHECK_JS_THROWS(in, "(function () { var o = Object.preventExtensions({}); Object.setPrototypeOf(o, {}); })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { var a = {}; var b = Object.create(a); Object.setPrototypeOf(a, b); })()", "TypeError");
    CHECK_JS_THROWS(in, "Object.setPrototypeOf({}, 1)", "TypeError");
    CHECK_JS_THROWS(in, "Object.setPrototypeOf(null, {})", "TypeError");
    CHECK_JS_TRUE(in, "Object.setPrototypeOf(1, null) === 1");
    CHECK_JS_TRUE(in, "(function () { var o = Object.preventExtensions({}); return Object.setPrototypeOf(o, Object.prototype) === o; })()");
    CHECK_JS_TRUE(in, "Object.keys('ab').length === 2 && Object.keys('ab')[1] === '1'");
    CHECK_JS_THROWS(in, "Object.keys(null)", "TypeError");
}

void test_object_prototype()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "Object.prototype.hasOwnProperty.call({ a: 1 }, 'a') && !({}).hasOwnProperty('toString')");
    CHECK_JS_TRUE(in, "(function () { var s = Symbol(); var o = {}; o[s] = 1; return o.hasOwnProperty(s); })()");
    CHECK_JS_TRUE(in, "'abc'.hasOwnProperty(1) && 'abc'.hasOwnProperty('length') && !'abc'.hasOwnProperty(3)");
    // ToPropertyKey happens before ToObject: the key's throw wins.
    CHECK_JS_STRING(in, "(function () { try { Object.prototype.hasOwnProperty.call(null, { toString() { throw new RangeError('key first'); } }); } catch (e) { return e.message; } })()", "key first");
    CHECK_JS_TRUE(in, "Object.prototype.isPrototypeOf({}) && !Object.prototype.isPrototypeOf(1) && Array.prototype.isPrototypeOf([]) === true");
    CHECK_JS_TRUE(in, "(function () { var o = { a: 1 }; Object.defineProperty(o, 'h', { value: 1 }); return o.propertyIsEnumerable('a') && !o.propertyIsEnumerable('h') && !o.propertyIsEnumerable('toString'); })()");
    CHECK_JS_TRUE(in, "'ab'.propertyIsEnumerable(0) && !'ab'.propertyIsEnumerable('length')");
    CHECK_JS_STRING(in, "Object.prototype.toString.call(null)", "[object Null]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call(undefined)", "[object Undefined]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call(1)", "[object Number]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call('s')", "[object String]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call(true)", "[object Boolean]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call([])", "[object Array]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call(function () {})", "[object Function]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call((function () {}).bind())", "[object Function]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call(new Error())", "[object Error]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call(Symbol())", "[object Symbol]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call(Math)", "[object Math]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call(Reflect)", "[object Reflect]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call({ [Symbol.toStringTag]: 'Custom' })", "[object Custom]");
    CHECK_JS_STRING(in, "Object.prototype.toString.call({ [Symbol.toStringTag]: 5 })", "[object Object]");
    CHECK_JS_STRING(in, "(function () { return Object.prototype.toString.call(arguments); })()", "[object Arguments]");
    CHECK_JS_STRING(in, "({}).toLocaleString()", "[object Object]");
    CHECK_JS_TRUE(in, "(function () { var o = {}; return o.valueOf() === o && typeof Object.prototype.valueOf.call(1) === 'object'; })()");
    CHECK_JS_TRUE(in, "(function () { var o = {}; return o.__proto__ === Object.prototype && (1).__proto__ === Number.prototype; })()");
    CHECK_JS_TRUE(in, "(function () { var o = {}; o.__proto__ = null; return Object.getPrototypeOf(o) === null && o.__proto__ === undefined; })()");
    CHECK_JS_TRUE(in, "(function () { var o = {}; o.__proto__ = 1; return Object.getPrototypeOf(o) === Object.prototype; })()");
    CHECK_JS_THROWS(in, "Object.prototype.__proto__ = Object.prototype", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(Object.prototype, '__proto__'); return typeof d.get === 'function' && typeof d.set === 'function' && !d.enumerable && d.configurable; })()");
    CHECK_JS_TRUE(in, "(function () { var o = {}; o.__defineGetter__('g', function () { return 7; }); o.__defineSetter__('s', function (v) { this.stored = v; }); o.s = 3; return o.g === 7 && o.stored === 3 && o.__lookupGetter__('g')() === 7 && typeof o.__lookupSetter__('s') === 'function' && o.__lookupGetter__('nope') === undefined; })()");
    CHECK_JS_THROWS(in, "({}).__defineGetter__('x', 1)", "TypeError");
}

void test_function_library()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "Function.length === 1 && Function.prototype.length === 0 && Function.prototype.name === ''");
    CHECK_JS_TRUE(in, "Function.prototype.constructor === Function && Object.getPrototypeOf(Function) === Function.prototype");
    CHECK_JS_NUMBER(in, "Function.prototype.apply.length + Function.prototype.call.length + Function.prototype.bind.length", 4);
    CHECK_JS_NUMBER(in, "(function (a, b) { return a + b; }).apply(null, { length: 2, 0: 1, 1: 2 })", 3);
    CHECK_JS_NUMBER(in, "(function () { return arguments.length; }).apply(null, undefined) + (function () { return arguments.length; }).apply(null, null)", 0);
    CHECK_JS_THROWS(in, "(function () {}).apply(null, 1)", "TypeError");
    CHECK_JS_THROWS(in, "Function.prototype.apply.call(1)", "TypeError");
    CHECK_JS_THROWS(in, "Function.prototype.call.call({})", "TypeError");
    CHECK_JS_NUMBER(in, "(function () { function f() { return arguments.length; } return f.apply(null, arguments); })(1, 2, 3)", 3);
    CHECK_JS_STRING(in, "(function () { var f = function () {}; Object.defineProperty(f, 'name', { value: 5 }); return f.bind().name; })()", "bound ");
    CHECK_JS_NUMBER(in, "(function () { var f = function () {}; Object.defineProperty(f, 'length', { value: 2.7 }); return f.bind().length; })()", 2);
    CHECK_JS_NUMBER(in, "(function () { var f = function () {}; Object.defineProperty(f, 'length', { value: Infinity }); return f.bind(null, 1).length; })()", INFINITY);
    CHECK_JS_NUMBER(in, "(function () { var f = function () {}; Object.defineProperty(f, 'length', { value: -Infinity }); return f.bind().length; })()", 0);
    CHECK_JS_NUMBER(in, "(function () { var f = function (a, b) {}; delete f.length; return f.bind().length; })()", 0);
    CHECK_JS_NUMBER(in, "(function () { var f = function (a, b) {}; Object.defineProperty(f, 'length', { value: 'x' }); return f.bind().length; })()", 0);
    CHECK_JS_TRUE(in, "(function () { function F() { return { r: 1 }; } var B = F.bind(); return new B().r === 1; })()");
    CHECK_JS_TRUE(in, "(function () { var b = (function () {}).bind(); return Object.getPrototypeOf(b) === Function.prototype && !('prototype' in b); })()");
    CHECK_JS_TRUE(in, "(function () { var b = (() => 1).bind(); try { new b(); return false; } catch (e) { return e instanceof TypeError; } })()");
    CHECK_JS_STRING(in, "(function () { function f() {} return typeof Function.prototype.toString.call(f); })()", "string");
    CHECK_JS_THROWS(in, "Function.prototype.toString.call({})", "TypeError");
    CHECK_JS_STRING(in, "Function.prototype.toString.call(Function.prototype)", "function () { [native code] }");
    CHECK_JS_STRING(in, "(function () { var d = Object.getOwnPropertyDescriptor(Function.prototype, 'caller'); return typeof d.get + (d.get === d.set) + d.configurable + d.enumerable; })()", "functiontruetruefalse");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(Function.prototype, Symbol.hasInstance); return !d.writable && !d.enumerable && !d.configurable; })()");
    CHECK_JS_TRUE(in, "(function () { var f = Function('a', 'b', 'return a * b'); return f(3, 4) === 12 && f.length === 2; })()");
    CHECK_JS_TRUE(in, "(function () { var f = new Function(); return f() === undefined && f.length === 0; })()");
    CHECK_JS_STRING(in, "new Function('a', 'return a').toString()", "function anonymous(a\n) {\nreturn a\n}");
    CHECK_JS_THROWS(in, "new Function('a', '}); (function () {')", "SyntaxError");
    CHECK_JS_THROWS(in, "new Function('/*', '*/){')", "SyntaxError");
    CHECK_JS_TRUE(in, "(function () { var x = 'outer'; var f = new Function('return typeof x'); return f() === 'undefined'; })()");
}

void test_error_library()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "(function () { var e = new Error('m'); return e.message === 'm' && e.name === 'Error' && e instanceof Error && Object.prototype.hasOwnProperty.call(e, 'message') && !e.propertyIsEnumerable('message'); })()");
    CHECK_JS_TRUE(in, "(function () { var e = Error('m'); return e instanceof Error && e.message === 'm'; })()");
    CHECK_JS_TRUE(in, "(function () { var e = new Error(); return !Object.prototype.hasOwnProperty.call(e, 'message') && e.message === ''; })()");
    CHECK_JS_TRUE(in, "(function () { var e = new Error(undefined); return !Object.prototype.hasOwnProperty.call(e, 'message'); })()");
    CHECK_JS_STRING(in, "new Error(42).message", "42");
    CHECK_JS_STRING(in, "String(new TypeError('t'))", "TypeError: t");
    CHECK_JS_STRING(in, "String(new Error())", "Error");
    CHECK_JS_STRING(in, "(function () { var e = new Error('m'); e.name = ''; return String(e); })()", "m");
    CHECK_JS_STRING(in, "(function () { var e = new Error(); e.name = 5; e.message = 6; return String(e); })()", "5: 6");
    CHECK_JS_STRING(in, "Error.prototype.toString.call({ name: undefined, message: 'x' })", "Error: x");
    CHECK_JS_THROWS(in, "Error.prototype.toString.call(1)", "TypeError");
    CHECK_JS_TRUE(in, "new TypeError() instanceof Error && new RangeError() instanceof RangeError && !(new RangeError() instanceof TypeError)");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(TypeError) === Error && Object.getPrototypeOf(TypeError.prototype) === Error.prototype");
    CHECK_JS_TRUE(in, "TypeError.prototype.name === 'TypeError' && TypeError.prototype.message === '' && !TypeError.prototype.hasOwnProperty('toString')");
    CHECK_JS_TRUE(in, "URIError.name === 'URIError' && EvalError.length === 1 && SyntaxError.prototype.constructor === SyntaxError && ReferenceError('r').message === 'r'");
    CHECK_JS_TRUE(in, "(function () { var e = new Error('m', { cause: 0 }); return e.cause === 0 && Object.prototype.hasOwnProperty.call(e, 'cause') && !e.propertyIsEnumerable('cause'); })()");
    CHECK_JS_TRUE(in, "(function () { var e = new Error('m', {}); return !('cause' in e); })()");
    CHECK_JS_TRUE(in, "(function () { var e = new Error('m', { get cause() { return 'g'; } }); return e.cause === 'g'; })()");
    CHECK_JS_TRUE(in, "typeof new Error('x').stack === 'string' && new RangeError('r').stack === 'RangeError: r'");
    CHECK_JS_TRUE(in, "(function () { function My() {} My.prototype = Object.create(Error.prototype); var e = Reflect.construct(Error, ['m'], My); return Object.getPrototypeOf(e) === My.prototype && e.message === 'm'; })()");
    CHECK_JS_TRUE(in, "Error.prototype.constructor === Error && Error.length === 1 && typeof Error.prototype.toString === 'function'");
    CHECK_JS_STRING(in, "(function () { try { null.f(); } catch (e) { return String(e).slice === undefined ? e.name : e.name; } })()", "TypeError");
}

void test_symbol_library()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "typeof Symbol() === 'symbol' && Symbol() !== Symbol() && Symbol('a').toString() === 'Symbol(a)' && Symbol().toString() === 'Symbol()'");
    CHECK_JS_TRUE(in, "Symbol('d').description === 'd' && Symbol().description === undefined && Symbol('').description === ''");
    CHECK_JS_THROWS(in, "new Symbol()", "TypeError");
    CHECK_JS_TRUE(in, "Symbol.for('a') === Symbol.for('a') && Symbol.for('a') !== Symbol('a') && Symbol.keyFor(Symbol.for('a')) === 'a' && Symbol.keyFor(Symbol('a')) === undefined");
    CHECK_JS_TRUE(in, "Symbol.for('x').description === 'x' && Symbol.for(1).description === '1'");
    CHECK_JS_THROWS(in, "Symbol.keyFor('a')", "TypeError");
    CHECK_JS_TRUE(in, "typeof Symbol.iterator === 'symbol' && typeof Symbol.hasInstance === 'symbol' && typeof Symbol.toPrimitive === 'symbol' && typeof Symbol.toStringTag === 'symbol' && typeof Symbol.isConcatSpreadable === 'symbol'");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(Symbol, 'iterator'); return !d.writable && !d.enumerable && !d.configurable; })()");
    CHECK_JS_TRUE(in, "Symbol.iterator.toString() === 'Symbol(Symbol.iterator)' && Symbol.keyFor(Symbol.iterator) === undefined");
    CHECK_JS_TRUE(in, "(function () { var s = Symbol('v'); var o = Object(s); return typeof o === 'object' && o.valueOf() === s && o == s && o !== s && Object.prototype.toString.call(o) === '[object Symbol]'; })()");
    CHECK_JS_TRUE(in, "(function () { var s = Symbol('v'); return s.valueOf() === s && Symbol.prototype.valueOf.call(Object(s)) === s; })()");
    CHECK_JS_THROWS(in, "Symbol.prototype.toString.call(1)", "TypeError");
    CHECK_JS_THROWS(in, "Symbol() + ''", "TypeError");
    CHECK_JS_THROWS(in, "+Symbol()", "TypeError");
    CHECK_JS_STRING(in, "String(Symbol('s'))", "Symbol(s)");
    CHECK_JS_TRUE(in, "Symbol.prototype[Symbol.toStringTag] === 'Symbol' && Symbol.prototype[Symbol.toPrimitive].call(Symbol.iterator) === Symbol.iterator");
    CHECK_JS_TRUE(in, "(function () { var s = Symbol(); var o = {}; o[s] = 1; return o[s] === 1 && Object.keys(o).length === 0 && JSON === JSON && s in o; })()");
    CHECK_JS_TRUE(in, "Symbol.length === 0 && Symbol.name === 'Symbol' && Symbol.for.length === 1");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(Symbol.prototype, 'description'); return typeof d.get === 'function' && d.set === undefined; })()");
}

void test_reflect()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "Reflect.apply(function (a, b) { return this.v + a + b; }, { v: 1 }, [2, 3])", 6);
    CHECK_JS_THROWS(in, "Reflect.apply(1, null, [])", "TypeError");
    CHECK_JS_THROWS(in, "Reflect.apply(function () {}, null, 1)", "TypeError");
    CHECK_JS_TRUE(in, "(function () { function F(a) { this.a = a; } var o = Reflect.construct(F, [5]); return o.a === 5 && o instanceof F; })()");
    CHECK_JS_TRUE(in, "(function () { function F() {} function G() {} var o = Reflect.construct(F, [], G); return Object.getPrototypeOf(o) === G.prototype; })()");
    CHECK_JS_THROWS(in, "Reflect.construct(function () {}, [], 1)", "TypeError");
    CHECK_JS_THROWS(in, "Reflect.construct(() => 1, [])", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var o = {}; return Reflect.defineProperty(o, 'x', { value: 1 }) && o.x === 1 && !Reflect.defineProperty(o, 'x', { value: 2 }); })()");
    CHECK_JS_THROWS(in, "Reflect.defineProperty(1, 'x', {})", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var o = { a: 1 }; return Reflect.deleteProperty(o, 'a') && !('a' in o) && !Reflect.deleteProperty(Object.freeze({ b: 1 }), 'b'); })()");
    CHECK_JS_TRUE(in, "(function () { var o = { get g() { return this; } }; var r = {}; return Reflect.get(o, 'g', r) === r && Reflect.get(o, 'g') === o && Reflect.get({ a: 1 }, 'a') === 1; })()");
    CHECK_JS_THROWS(in, "Reflect.get(1, 'a')", "TypeError");
    CHECK_JS_TRUE(in, "Reflect.getOwnPropertyDescriptor({ a: 1 }, 'a').value === 1 && Reflect.getOwnPropertyDescriptor({}, 'a') === undefined");
    CHECK_JS_TRUE(in, "Reflect.getPrototypeOf([]) === Array.prototype && Reflect.getPrototypeOf(Object.create(null)) === null");
    CHECK_JS_TRUE(in, "Reflect.has({ a: 1 }, 'a') && Reflect.has({}, 'toString') && !Reflect.has({}, 'nope')");
    CHECK_JS_TRUE(in, "Reflect.isExtensible({}) && !Reflect.isExtensible(Object.freeze({}))");
    CHECK_JS_TRUE(in, "(function () { var s = Symbol(); var o = { b: 1, 1: 1, a: 1 }; o[s] = 1; var k = Reflect.ownKeys(o); return k.length === 4 && k[0] === '1' && k[1] === 'b' && k[2] === 'a' && k[3] === s; })()");
    CHECK_JS_TRUE(in, "(function () { var o = {}; return Reflect.preventExtensions(o) && !Object.isExtensible(o); })()");
    CHECK_JS_TRUE(in, "(function () { var o = {}; var r = {}; return Reflect.set(o, 'a', 1) && o.a === 1 && Reflect.set(o, 'b', 2, r) && r.b === 2 && !('b' in o); })()");
    CHECK_JS_TRUE(in, "(function () { var o = { set s(v) { this.got = v; } }; var r = {}; Reflect.set(o, 's', 3, r); return r.got === 3 && !('got' in o); })()");
    CHECK_JS_TRUE(in, "!Reflect.set(Object.freeze({ a: 1 }), 'a', 2)");
    CHECK_JS_TRUE(in, "(function () { var o = {}; return Reflect.setPrototypeOf(o, null) && Object.getPrototypeOf(o) === null && !Reflect.setPrototypeOf(Object.preventExtensions({}), {}); })()");
    CHECK_JS_THROWS(in, "Reflect.setPrototypeOf({}, 1)", "TypeError");
    CHECK_JS_TRUE(in, "Reflect[Symbol.toStringTag] === 'Reflect' && Reflect.apply.length === 3 && Reflect.construct.length === 2 && Reflect.set.length === 3");
}

void test_boolean_and_number()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "Boolean(1) === true && Boolean('') === false && Boolean() === false && Boolean([]) === true");
    CHECK_JS_TRUE(in, "(function () { var b = new Boolean(false); return typeof b === 'object' && b.valueOf() === false && !!b === true && b.toString() === 'false'; })()");
    CHECK_JS_THROWS(in, "Boolean.prototype.valueOf.call(1)", "TypeError");
    CHECK_JS_TRUE(in, "Boolean.prototype.valueOf() === false && Boolean.prototype.toString() === 'false'");
    CHECK_JS_TRUE(in, "Number() === 0 && Number('12') === 12 && Number('') === 0 && Number(' 12 ') === 12 && Number(null) === 0 && Number([5]) === 5 && Number(true) === 1");
    CHECK(is_nan(in, "Number(undefined)"));
    CHECK(is_nan(in, "Number([1, 2])"));
    CHECK(is_nan(in, "Number('12px')"));
    CHECK_JS_TRUE(in, "Number('0x10') === 16 && Number('0b11') === 3 && Number('0o17') === 15 && Number('1e3') === 1000 && Number('-Infinity') === -Infinity");
    CHECK_JS_TRUE(in, "(function () { var n = new Number(5); return typeof n === 'object' && n.valueOf() === 5 && n + 1 === 6; })()");
    CHECK_JS_TRUE(in, "Number.MAX_SAFE_INTEGER === 9007199254740991 && Number.MIN_SAFE_INTEGER === -9007199254740991 && Number.EPSILON === Math.pow(2, -52) && Number.MIN_VALUE === 5e-324 && Number.MAX_VALUE === 1.7976931348623157e308");
    CHECK_JS_TRUE(in, "Number.isNaN(NaN) && !Number.isNaN('NaN') && isNaN('NaN') && !Number.isFinite('1') && isFinite('1') && Number.isInteger(5) && !Number.isInteger(5.5) && Number.isSafeInteger(2 ** 53 - 1) && !Number.isSafeInteger(2 ** 53)");
    CHECK_JS_TRUE(in, "Number.parseInt === parseInt && Number.parseFloat === parseFloat");
    CHECK_JS_TRUE(in, "parseInt('12px') === 12 && parseInt('0x1f') === 31 && parseInt('11', 2) === 3 && parseInt('  -7  ') === -7 && parseInt('z', 36) === 35");
    CHECK(is_nan(in, "parseInt('')"));
    CHECK(is_nan(in, "parseInt('10', 1)"));
    CHECK(is_nan(in, "parseInt('10', 37)"));
    CHECK_JS_TRUE(in, "parseFloat('3.14abc') === 3.14 && parseFloat('.5') === 0.5 && parseFloat('-Infinityx') === -Infinity && parseFloat('1e2') === 100");
    CHECK(is_nan(in, "parseFloat('abc')"));
    CHECK_JS_STRING(in, "(255).toString(16)", "ff");
    CHECK_JS_STRING(in, "(255).toString(2)", "11111111");
    CHECK_JS_STRING(in, "(-255).toString(36)", "-73");
    CHECK_JS_STRING(in, "(0.5).toString(2)", "0.1");
    CHECK_JS_THROWS(in, "(1).toString(1)", "RangeError");
    CHECK_JS_THROWS(in, "(1).toString(37)", "RangeError");
    CHECK_JS_THROWS(in, "Number.prototype.toString.call('1')", "TypeError");
    CHECK_JS_STRING(in, "(1.005).toFixed(2)", "1.00");
    CHECK_JS_STRING(in, "(1.5).toFixed(0)", "2");
    CHECK_JS_STRING(in, "(2.5).toFixed(0)", "3");
    CHECK_JS_STRING(in, "(-1.5).toFixed(0)", "-2");
    CHECK_JS_STRING(in, "(0).toFixed(2)", "0.00");
    CHECK_JS_STRING(in, "(1e21).toFixed(2)", "1e+21");
    CHECK_JS_STRING(in, "(NaN).toFixed(2)", "NaN");
    CHECK_JS_THROWS(in, "(1).toFixed(101)", "RangeError");
    CHECK_JS_THROWS(in, "(1).toFixed(-1)", "RangeError");
    CHECK_JS_STRING(in, "(123.456).toExponential(2)", "1.23e+2");
    CHECK_JS_STRING(in, "(0).toExponential()", "0e+0");
    CHECK_JS_STRING(in, "(123456).toExponential()", "1.23456e+5");
    CHECK_JS_STRING(in, "(Infinity).toExponential(200)", "Infinity");
    CHECK_JS_THROWS(in, "(1).toExponential(101)", "RangeError");
    CHECK_JS_STRING(in, "(123.456).toPrecision(4)", "123.5");
    CHECK_JS_STRING(in, "(0.000123).toPrecision(2)", "0.00012");
    CHECK_JS_STRING(in, "(123456).toPrecision(2)", "1.2e+5");
    CHECK_JS_STRING(in, "(1).toPrecision()", "1");
    CHECK_JS_THROWS(in, "(1).toPrecision(0)", "RangeError");
    CHECK_JS_TRUE(in, "(5).valueOf() === 5 && new Number(5).valueOf() === 5 && Number.prototype.valueOf() === 0");
    CHECK_JS_STRING(in, "(12.5).toLocaleString()", "12.5");
    CHECK_JS_TRUE(in, "Number.prototype.toString.length === 1 && Number.prototype.toFixed.length === 1 && Number.length === 1");
}

void test_math()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "Math.PI === 3.141592653589793 && Math.E === 2.718281828459045 && Math.SQRT2 === 1.4142135623730951 && Math.LN2 === 0.6931471805599453");
    CHECK_JS_TRUE(in, "Math.abs(-5) === 5 && Math.abs('-2') === 2 && Math.floor(-1.5) === -2 && Math.ceil(-1.5) === -1 && Math.trunc(-1.7) === -1");
    CHECK_JS_TRUE(in, "Math.round(0.5) === 1 && Math.round(-0.5) === 0 && Object.is(Math.round(-0.5), -0) && Math.round(2.5) === 3 && Math.round(-2.5) === -2 && Math.round(0.49999999999999994) === 0");
    CHECK_JS_TRUE(in, "Math.max() === -Infinity && Math.min() === Infinity && Math.max(1, 3, 2) === 3 && Math.min(1, '0', 2) === 0 && Object.is(Math.max(-0, 0), 0) && Object.is(Math.min(0, -0), -0)");
    CHECK(is_nan(in, "Math.max(1, NaN, 2)"));
    CHECK(is_nan(in, "Math.min(NaN, 1)"));
    CHECK_JS_STRING(in, "(function () { var log = ''; Math.max({ valueOf() { log += 'a'; return NaN; } }, { valueOf() { log += 'b'; return 1; } }); return log; })()", "ab");
    CHECK_JS_TRUE(in, "Math.pow(2, 10) === 1024 && Number.isNaN(Math.pow(1, Infinity)) && Math.pow(NaN, 0) === 1 && Math.sqrt(16) === 4 && Math.cbrt(27) === 3");
    CHECK_JS_TRUE(in, "Math.sign(-3) === -1 && Math.sign(3) === 1 && Object.is(Math.sign(-0), -0) && Number.isNaN(Math.sign(NaN))");
    CHECK_JS_TRUE(in, "Math.hypot() === 0 && Math.hypot(3, 4) === 5 && Math.hypot(NaN, Infinity) === Infinity && Number.isNaN(Math.hypot(NaN, 1)) && Math.hypot(1e200, 1e200) > 1e200");
    CHECK_JS_TRUE(in, "Math.clz32(1) === 31 && Math.clz32(0) === 32 && Math.clz32(-1) === 0 && Math.imul(3, 4) === 12 && Math.imul(0xffffffff, 5) === -5 && Math.imul(2147483647, 2) === -2");
    CHECK_JS_TRUE(in, "Math.fround(5.5) === 5.5 && Math.fround(5.05) !== 5.05 && Math.fround(1e40) === Infinity");
    CHECK_JS_TRUE(in, "Math.atan2(1, 1) === Math.PI / 4 && Math.log(Math.E) === 1 && Math.log2(8) === 3 && Math.log10(1000) === 3 && Math.exp(0) === 1 && Math.expm1(0) === 0 && Math.log1p(0) === 0");
    CHECK_JS_TRUE(in, "Math.sin(0) === 0 && Math.cos(0) === 1 && Math.tan(0) === 0 && Math.asin(0) === 0 && Math.acos(1) === 0 && Math.atan(0) === 0 && Math.sinh(0) === 0 && Math.cosh(0) === 1 && Math.tanh(0) === 0 && Math.asinh(0) === 0 && Math.acosh(1) === 0 && Math.atanh(0) === 0");
    CHECK_JS_TRUE(in, "(function () { for (var i = 0; i < 100; i++) { var r = Math.random(); if (!(r >= 0 && r < 1)) return false; } return Math.random() !== Math.random(); })()");
    CHECK_JS_TRUE(in, "Math[Symbol.toStringTag] === 'Math' && Object.prototype.toString.call(Math) === '[object Math]' && typeof Math === 'object' && Math.max.length === 2 && Math.hypot.length === 2 && Math.abs.length === 1");
    CHECK_JS_THROWS(in, "new Math.abs(1)", "TypeError");
    CHECK_JS_THROWS(in, "Math()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(Math, 'PI'); return !d.writable && !d.enumerable && !d.configurable; })()");
}

void test_global_functions()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "globalThis === this && globalThis.globalThis === globalThis && typeof globalThis === 'object'");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(globalThis, 'NaN'); return !d.writable && !d.enumerable && !d.configurable && Number.isNaN(d.value); })()");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(globalThis, 'undefined'); return d.value === undefined && !d.writable; })()");
    CHECK_JS_TRUE(in, "Object.getOwnPropertyDescriptor(globalThis, 'Infinity').value === Infinity");
    CHECK_JS_TRUE(in, "Object.getOwnPropertyDescriptor(globalThis, 'Object').writable && !Object.getOwnPropertyDescriptor(globalThis, 'Object').enumerable");
    CHECK_JS_TRUE(in, "typeof eval === 'function' && eval.length === 1 && eval.name === 'eval'");
    CHECK_JS_TRUE(in, "isNaN(NaN) && isNaN(undefined) && !isNaN(null) && isNaN({}) && !isNaN('12') && isFinite(1) && !isFinite(Infinity) && !isFinite(NaN) && isFinite(null)");
    CHECK_JS_STRING(in, "encodeURIComponent('a b&c/d?e=f#g')", "a%20b%26c%2Fd%3Fe%3Df%23g");
    CHECK_JS_STRING(in, "encodeURI('http://x.y/a b?q=1&r=[2]#h')", "http://x.y/a%20b?q=1&r=%5B2%5D#h");
    CHECK_JS_STRING(in, "encodeURIComponent('\\u00e9\\u4e2d\\ud83d\\ude00')", "%C3%A9%E4%B8%AD%F0%9F%98%80");
    CHECK_JS_STRING(in, "encodeURIComponent(\"-_.!~*'()\")", "-_.!~*'()");
    CHECK_JS_THROWS(in, "encodeURIComponent('\\ud800')", "URIError");
    CHECK_JS_THROWS(in, "encodeURI('\\udc00x')", "URIError");
    CHECK_JS_STRING(in, "decodeURIComponent('a%20b%26c%2Fd')", "a b&c/d");
    CHECK_JS_STRING(in, "decodeURI('a%20b%26c%2Fd%23e')", "a b%26c%2Fd%23e");
    CHECK_JS_STRING(in, "decodeURIComponent('%C3%A9%E4%B8%AD%F0%9F%98%80')", "\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80");
    CHECK_JS_THROWS(in, "decodeURIComponent('%')", "URIError");
    CHECK_JS_THROWS(in, "decodeURIComponent('%zz')", "URIError");
    CHECK_JS_THROWS(in, "decodeURIComponent('%C3')", "URIError");
    CHECK_JS_THROWS(in, "decodeURIComponent('%C0%80')", "URIError");
    CHECK_JS_THROWS(in, "decodeURIComponent('%ED%A0%80')", "URIError");
    CHECK_JS_THROWS(in, "decodeURIComponent('%80')", "URIError");
    CHECK_JS_STRING(in, "decodeURIComponent('no escapes')", "no escapes");
    CHECK_JS_STRING(in, "escape('a b+c\\u00e9\\u4e2d@*_+-./')", "a%20b+c%E9%u4E2D@*_+-./");
    CHECK_JS_STRING(in, "unescape('a%20b%E9%u4E2D%zz%u12')", "a b\xC3\xA9\xE4\xB8\xAD%zz%u12");
    CHECK_JS_TRUE(in, "encodeURI.length === 1 && decodeURIComponent.name === 'decodeURIComponent' && escape.length === 1");
    CHECK_JS_TRUE(in, "(function () { var log = ''; console.log('a', 1, { toString() { return 'o'; } }); return typeof console.log === 'function' && typeof console.error === 'function'; })()");
    {
        std::string captured;
        in.on_console = [&captured](std::string_view level, std::string_view message) { captured += std::string(level) + ":" + std::string(message) + "|"; };
        test::run_js(in, "console.log('a', 1, null, undefined, [1, 2], { toString() { return 'o'; } }); console.warn(Symbol('s')); console.error(new TypeError('t'))");
        CHECK_EQ(captured, std::string("log:a 1 null undefined 1,2 o|warn:Symbol(s)|error:TypeError: t|"));
        in.on_console = nullptr;
    }
}

void test_conversions_through_script()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "String(Symbol('q'))", "Symbol(q)");
    CHECK_JS_STRING(in, "String(null) + String(undefined) + String(true)", "nullundefinedtrue");
    CHECK_JS_NUMBER(in, "Number('  \\n 12 \\u2028 ')", 12);
    CHECK(is_nan(in, "Number('0b')"));
    CHECK(is_nan(in, "Number('1_0')"));
    CHECK_JS_NUMBER(in, "Number({ valueOf() { return '7'; } })", 7);
    CHECK_JS_NUMBER(in, "Number({ valueOf: 1, toString() { return '8'; } })", 8);
    CHECK_JS_TRUE(in, "(function () { var o = { [Symbol.toPrimitive]() { return {}; } }; try { +o; return false; } catch (e) { return e instanceof TypeError; } })()");
    CHECK_JS_TRUE(in, "(function () { var d = { [Symbol.toPrimitive](h) { return h; } }; return (d + '') === 'default' && `${d}` === 'string' && (d * 1 !== d * 1); })()");
    CHECK_JS_TRUE(in, "(function () { var o = { toString() { throw new RangeError('ts'); } }; try { String(o); } catch (e) { return e.message === 'ts'; } })()");
    CHECK_JS_TRUE(in, "(function () { var o = {}; return o == o && !({} == {}) && ({}) != ({}); })()");
    CHECK_JS_TRUE(in, "'1' == 1 && 1 == '1' && '' == 0 && ' \\t' == 0 && 'a' != 0 && null == null && undefined == null");
    CHECK_JS_TRUE(in, "(function () { var d = { toString() { return '5'; } }; return d == 5 && d == '5' && !(d === 5); })()");
    CHECK_JS_TRUE(in, "(function () { var s = Symbol(); return Object(s) == s && !(Object(s) === s); })()");
    CHECK_JS_TRUE(in, "typeof (2 ** 31 | 0) === 'number' && (2 ** 31 | 0) === -(2 ** 31) && (-(2 ** 31) - 1 | 0) === 2 ** 31 - 1 && (4294967296.5 | 0) === 0 && (-1 >>> 0) === 4294967295");
    CHECK_JS_TRUE(in, "(function () { var a = []; a.length = 2 ** 32 - 1; return a.length === 4294967295; })()");
    CHECK_JS_THROWS(in, "(function () { var a = []; a.length = 2 ** 32; })()", "RangeError");
    CHECK_JS_THROWS(in, "(function () { var a = []; a.length = -1; })()", "RangeError");
    CHECK_JS_THROWS(in, "(function () { var a = []; a.length = 1.5; })()", "RangeError");
    CHECK_JS_THROWS(in, "(function () { var a = []; a.length = 'x'; })()", "RangeError");
    CHECK_JS_TRUE(in, "(function () { var a = []; a.length = '3'; return a.length === 3; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; a.length = { valueOf() { return 1; } }; return a.length === 1; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { get length() { return 3; } }; Object.setPrototypeOf(o, Array.prototype); return Array.prototype.isPrototypeOf(o); })()");
}

} // namespace

int main()
{
    test_object_statics();
    test_object_prototype();
    test_function_library();
    test_error_library();
    test_symbol_library();
    test_reflect();
    test_boolean_and_number();
    test_math();
    test_global_functions();
    test_conversions_through_script();
    return sashfold::test::report("js_runtime");
}
