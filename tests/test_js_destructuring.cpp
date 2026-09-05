#include "JsTest.h"

#include "js/Interpreter.h"

#include <string>

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

// An iterable of three values that logs every call the protocol makes,
// and one that yields undefined for ever.
std::string const logging_iterable = "var log = []; var obj = {}; obj[Symbol.iterator] = function () { log.push('iter'); var i = 0; return { next: function () { log.push('next'); return i < 3 ? { value: i++, done: false } : { value: undefined, done: true }; }, return: function () { log.push('return'); return {}; } }; }; ";
std::string const undefined_iterable = "var log = []; var obj = {}; obj[Symbol.iterator] = function () { log.push('iter'); return { next: function () { log.push('next'); return { value: undefined, done: false }; }, return: function () { log.push('return'); return {}; } }; }; ";

void test_array_bindings()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "(function () { var [a, b, c] = [1, 2]; return typeof a + b + c; })()", "number2undefined");
    CHECK_JS_STRING(in, "(function () { let [a, , b] = 'xyz'; return a + b; })()", "xz");
    CHECK_JS_NUMBER(in, "(function () { const [a, [b, c], d] = [1, [2, 3], 4]; return a + b + c + d; })()", 10);
    CHECK_JS_STRING(in, "(function () { var [a, ...rest] = [1, 2, 3]; return a + ':' + rest.join(); })()", "1:2,3");
    CHECK_JS_TRUE(in, "(function () { var [...r] = []; return Array.isArray(r) && r.length === 0; })()");
    CHECK_JS_STRING(in, "(function () { var [...[a, b]] = [1, 2, 3]; return a + ',' + b; })()", "1,2");
    CHECK_JS_STRING(in, "(function () { var [a = 1, b = a * 2, c = 'c'] = [undefined, undefined, null]; return a + ',' + b + ',' + c; })()", "1,2,null");
    // An anonymous function default takes the name it lands in.
    CHECK_JS_STRING(in, "(function () { var [f = function () {}, g = () => 1, h = function named() {}] = []; return f.name + ',' + g.name + ',' + h.name; })()", "f,g,named");
    // A hole reads as undefined and takes the default; a present value
    // leaves its default unevaluated; values past the pattern are ignored.
    CHECK_JS_STRING(in, "(function () { var log = []; var [a = log.push('a'), b = log.push('b')] = [1, , 3]; return a + ',' + b + ',' + log.join(); })()", "1,1,b");
    // Strings are walked by code point.
    CHECK_JS_STRING(in, "(function () { var [a, b, c] = 'x\\uD834\\uDF06y'; return a + b.length + c; })()", "x2y");
    // The protocol: the iterator is closed when the pattern ends first,
    // and not when it ran out.
    CHECK_JS_STRING(in, logging_iterable + "(function () { var [a] = obj; return a + log.join(); })()", "0iter,next,return");
    CHECK_JS_STRING(in, logging_iterable + "(function () { var [a, b, c, d] = obj; return log.join(); })()", "iter,next,next,next,next");
    CHECK_JS_STRING(in, logging_iterable + "(function () { var [] = obj; return log.join(); })()", "iter,return");
    CHECK_JS_STRING(in, logging_iterable + "(function () { var [...r] = obj; return r.join() + ' ' + log.join(); })()", "0,1,2 iter,next,next,next,next");
    CHECK_JS_STRING(in, logging_iterable + "(function () { var [, , ] = obj; return log.join(); })()", "iter,next,next,return");
    // A default that throws closes the iterator; a throw from next() is
    // the iterator's own and closes nothing.
    CHECK_JS_STRING(in, undefined_iterable + "(function () { try { var [a = (function () { throw new Error('dflt'); })()] = obj; } catch (e) { return e.message + ' ' + log.join(); } })()", "dflt iter,next,return");
    CHECK_JS_STRING(in, undefined_iterable + "(function () { var [a = 'd', b] = obj; return a + b + ' ' + log.join(); })()", "dundefined iter,next,next,return");
    CHECK_JS_STRING(in, "(function () { var log = []; var obj = {}; obj[Symbol.iterator] = function () { return { next: function () { throw new Error('nx'); }, return: function () { log.push('return'); return {}; } }; }; try { var [a] = obj; } catch (e) { return e.message + log.length; } })()", "nx0");
    // A return() that throws is the outcome of a normal exit, and loses
    // to a throw from inside the pattern.
    CHECK_JS_STRING(in, "(function () { var obj = {}; obj[Symbol.iterator] = function () { return { next: function () { return { value: 1, done: false }; }, return: function () { throw new Error('ret'); } }; }; try { var [a] = obj; } catch (e) { return e.message; } })()", "ret");
    // Not iterable.
    CHECK_JS_THROWS(in, "var [a] = 5;", "TypeError");
    CHECK_JS_THROWS(in, "var [a] = {};", "TypeError");
    CHECK_JS_THROWS(in, "var [a] = null;", "TypeError");
    CHECK_JS_THROWS(in, "(function () { let [a] = undefined; })()", "TypeError");
    // The dead zone covers the pattern's own names.
    CHECK_JS_THROWS(in, "(function () { let [a = b, b] = []; })()", "ReferenceError");
    CHECK_JS_THROWS(in, "(function () { let [a = a] = []; })()", "ReferenceError");
    CHECK_JS_THROWS(in, "let [tdz1 = tdz2, tdz2] = [];", "ReferenceError");
    CHECK_JS_NUMBER(in, "(function () { let [a, b = a] = [4]; return b; })()", 4);
    CHECK_JS_THROWS(in, "(function () { const [a] = [1]; a = 2; })()", "TypeError");
    // A var pattern assigns through references: a `with` object catches it.
    CHECK_JS_NUMBER(in, "(function () { var o = { a: 0 }; with (o) { var [a] = [3]; } return o.a; })()", 3);
    // Nested patterns each take an iterator of their own.
    CHECK_JS_STRING(in, "(function () { var [[a, b], [c]] = ['xy', 'z']; return a + b + c; })()", "xyz");
    // A for head's let/const pattern is fresh per iteration.
    CHECK_JS_STRING(in, "(function () { var fs = []; for (let [i] = [0]; i < 3; i++) fs.push(function () { return i; }); return fs.map(function (f) { return f(); }).join(); })()", "0,1,2");
}

void test_object_bindings()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "(function () { var {a, b: c, d = 4, e: {f} = {f: 6}, ...rest} = {a: 1, b: 2, g: 7, h: 8}; return [a, c, d, f, Object.keys(rest).join('+'), rest.g + rest.h].join(); })()", "1,2,4,6,g+h,15");
    CHECK_JS_STRING(in, "(function () { var k = 'x'; var {[k]: v, [k + 'y']: w = 'w'} = {x: 1}; return v + w; })()", "1w");
    CHECK_JS_NUMBER(in, "(function () { var {length} = 'abc'; return length; })()", 3);
    CHECK_JS_STRING(in, "(function () { let {toFixed} = 1; return typeof toFixed; })()", "function");
    CHECK_JS_STRING(in, "(function () { const {0: a, 1: b} = 'pq'; return b + a; })()", "qp");
    CHECK_JS_THROWS(in, "var {a} = null;", "TypeError");
    CHECK_JS_THROWS(in, "var {} = undefined;", "TypeError");
    CHECK_JS_NUMBER(in, "(function () { var {} = 0; return 1; })()", 1);
    // The rest property is a fresh object of the own enumerable properties
    // not named before it — string and symbol keys alike, computed ones
    // included — and a getter on the source is read once.
    CHECK_JS_TRUE(in, "(function () { var s = Symbol(); var reads = 0; var src = { a: 1, b: 2 }; src[s] = 3; Object.defineProperty(src, 'g', { get: function () { reads++; return 4; }, enumerable: true }); Object.defineProperty(src, 'hidden', { value: 5 }); var k = 'b'; var { a, [k]: bee, ...rest } = src; var d = Object.getOwnPropertyDescriptor(rest, 'g'); return a === 1 && bee === 2 && rest[s] === 3 && d.value === 4 && d.writable && reads === 1 && !('a' in rest) && !('b' in rest) && !('hidden' in rest) && Object.getPrototypeOf(rest) === Object.prototype; })()");
    // Order: each key is evaluated, then its value read, in source order.
    CHECK_JS_STRING(in, "(function () { var log = []; var src = { get a() { log.push('get a'); return 1; }, get b() { log.push('get b'); return 2; } }; var { [(log.push('key a'), 'a')]: a, [(log.push('key b'), 'b')]: b } = src; return log.join(); })()", "key a,get a,key b,get b");
    // Defaults and their names.
    CHECK_JS_STRING(in, "(function () { var {f = function () {}, g: h = () => 1, i = function j() {}} = {}; return f.name + ',' + h.name + ',' + i.name; })()", "f,h,j");
    CHECK_JS_STRING(in, "(function () { var log = []; var { a = log.push('a'), b = log.push('b') } = { a: null }; return a + ',' + b + ',' + log.join(); })()", "null,1,b");
    // Two __proto__ keys are two ordinary reads in a pattern.
    CHECK_JS_TRUE(in, "(function () { var {__proto__: a, __proto__: b} = {}; return a === Object.prototype && b === Object.prototype; })()");
    // Nested through arrays and back.
    CHECK_JS_STRING(in, "(function () { var { p: [ { q } , r ] } = { p: [ { q: 'Q' }, 'R' ] }; return q + r; })()", "QR");
    // The dead zone.
    CHECK_JS_THROWS(in, "(function () { let {a = b, b} = {}; })()", "ReferenceError");
    CHECK_JS_THROWS(in, "let {a: {b}} = {};", "TypeError");
    // Inherited properties are read; only own enumerable ones are copied
    // into the rest.
    CHECK_JS_TRUE(in, "(function () { var proto = { inherited: 1 }; var src = Object.create(proto); src.own = 2; var { inherited, ...rest } = src; return inherited === 1 && rest.own === 2 && !('inherited' in rest); })()");
}

void test_assignment_patterns()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "(function () { var a = 1, b = 2; [a, b] = [b, a]; return a + ',' + b; })()", "2,1");
    CHECK_JS_STRING(in, "(function () { var o = {}; [o.x, o['y'], o.z = 3] = [1, 2]; return o.x + ',' + o.y + ',' + o.z; })()", "1,2,3");
    CHECK_JS_TRUE(in, "(function () { var a; var src = [5]; var r = ([a] = src); return r === src && a === 5; })()");
    CHECK_JS_STRING(in, "(function () { var a, c, rest; ({a, b: c = 2, ...rest} = {a: 1, d: 4}); return a + ',' + c + ',' + Object.keys(rest).join() + rest.d; })()", "1,2,d4");
    CHECK_JS_STRING(in, "(function () { var a, b, c; [a, [b, [c]]] = [1, [2, [3]]]; return '' + a + b + c; })()", "123");
    CHECK_JS_STRING(in, "(function () { var a, b; [a] = [b] = [7]; return a + ',' + b; })()", "7,7");
    CHECK_JS_STRING(in, "(function () { var a, b; [a = 1, b = 2] = [undefined, null]; return a + ',' + b; })()", "1,null");
    CHECK_JS_STRING(in, "(function () { var f, g; [f = function () {}] = []; ({ g = () => 1 } = {}); return f.name + ',' + g.name; })()", "f,g");
    CHECK_JS_STRING(in, "(function () { var a, b, out = []; for ([a, b] of [[1, 2], [3, 4]]) out.push(a + b); return out.join(); })()", "3,7");
    CHECK_JS_STRING(in, "(function () { var out = []; for (const [k, v] of [['a', 1], ['b', 2]]) out.push(k + v); return out.join(); })()", "a1,b2");
    CHECK_JS_STRING(in, "(function () { var n, out = []; for ({length: n} of ['ab', 'c']) out.push(n); return out.join(); })()", "2,1");
    CHECK_JS_STRING(in, "(function () { var out = []; for (var {x, y = 9} of [{x: 1}, {x: 2, y: 3}]) out.push(x + ':' + y); return out.join(); })()", "1:9,2:3");
    CHECK_JS_STRING(in, "(function () { var c, out = []; for ([c] in {ab: 1, cd: 2}) out.push(c); return out.join(); })()", "a,c");
    CHECK_JS_STRING(in, "(function () { var out = []; for (let [c, d] in {ab: 1}) out.push(d + c); return out.join(); })()", "ba");
    // The target reference is taken before the value is read (§13.15.5.5):
    // for an object pattern after its key, for an array pattern before
    // the iterator steps.
    CHECK_JS_STRING(in, "(function () { var log = []; var tgt = { set p(v) { log.push('set ' + v); } }; var src = {}; Object.defineProperty(src, 'a', { get: function () { log.push('get a'); return 1; } }); ({ [(log.push('key'), 'a')]: tgt[(log.push('ref'), 'p')] } = src); return log.join(); })()", "key,ref,get a,set 1");
    CHECK_JS_STRING(in, logging_iterable + "(function () { var tgt = { set p(v) { log.push('set ' + v); } }; [tgt[(log.push('ref'), 'p')]] = obj; return log.join(); })()", "iter,ref,next,set 0,return");
    // A target that cannot be written closes the iterator, with the
    // write's error as the outcome.
    CHECK_JS_STRING(in, logging_iterable + "(function () { try { [null.x] = obj; } catch (e) { return e.constructor.name + ' ' + log.join(); } })()", "TypeError iter,next,return");
    CHECK_JS_THROWS(in, "[] = 5;", "TypeError");
    CHECK_JS_THROWS(in, "({} = null);", "TypeError");
    CHECK_JS_NUMBER(in, "({} = 1)", 1);
    CHECK_JS_STRING(in, "(function () { var a, b; [a, ...b] = 'xyz'; return a + b.join(); })()", "xy,z");
    CHECK_JS_STRING(in, "(function () { var o = {}; [...o.rest] = [1, 2]; return o.rest.join(); })()", "1,2");
    CHECK_JS_STRING(in, "(function () { var o = {}; ({...o.rest} = {a: 1}); return Object.keys(o.rest).join(); })()", "a");
    // Strict code refuses an unresolvable name, sloppy code makes a global.
    CHECK_JS_THROWS(in, "'use strict'; [noSuchName] = [1];", "ReferenceError");
    CHECK_JS_NUMBER(in, "[fromPattern] = [6]; fromPattern", 6);
    // The completion value of an expression statement is the right-hand side.
    CHECK_JS_STRING(in, "(function () { var a; return eval('[a] = [\"v\"]').join(); })()", "v");
}

void test_parameters()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "(function (a, b = 2, c = a + b) { return a + b + c; })(1)", 6);
    CHECK_JS_NUMBER(in, "(function (a, b = 2, c = a + b) { return a + b + c; })(1, 3)", 8);
    CHECK_JS_NUMBER(in, "(function (a, b = 2, c = a + b) { return a + b + c; })(1, undefined, 0)", 3);
    CHECK_JS_TRUE(in, "(function (a = 1) { return a; })(null) === null");
    // length counts the parameters before the first default or the rest.
    CHECK_JS_NUMBER(in, "(function (a, b = 1, c) {}).length", 1);
    CHECK_JS_NUMBER(in, "(function (...r) {}).length", 0);
    CHECK_JS_NUMBER(in, "(function ([a], {b}, c) {}).length", 3);
    CHECK_JS_NUMBER(in, "((a, b) => 1).length", 2);
    CHECK_JS_NUMBER(in, "((a = 1) => 1).length", 0);
    CHECK_JS_NUMBER(in, "(x => 1).length", 1);
    // Rest.
    CHECK_JS_STRING(in, "(function (a, ...r) { return r.length + ':' + r.join() + ':' + Array.isArray(r); })(1, 2, 3)", "2:2,3:true");
    CHECK_JS_STRING(in, "(function (a, ...r) { return r.length + ':' + r.join(); })()", "0:");
    CHECK_JS_STRING(in, "((...r) => r.join())(1, 2)", "1,2");
    CHECK_JS_STRING(in, "(function (...[a, b = 'B']) { return a + b; })('A')", "AB");
    // Patterns.
    CHECK_JS_NUMBER(in, "(function ([a, b], {c}) { return a + b + c; })([1, 2], {c: 3})", 6);
    CHECK_JS_NUMBER(in, "(function ({a = 1, b} = {}) { return a + (b === undefined); })()", 2);
    CHECK_JS_NUMBER(in, "(function ({a = 1, b} = {}) { return a + (b === undefined); })({a: 5, b: 0})", 5);
    CHECK_JS_THROWS(in, "(function ({a}) {})()", "TypeError");
    CHECK_JS_THROWS(in, "(function ([a]) {})({})", "TypeError");
    // The dead zone runs left to right over the list.
    CHECK_JS_THROWS(in, "(function (a = b, b) {})()", "ReferenceError");
    CHECK_JS_THROWS(in, "(function (a = a) {})()", "ReferenceError");
    CHECK_JS_NUMBER(in, "(function (a, b = a) { return b; })(1)", 1);
    // A non-simple list means an unmapped arguments object, available to
    // the defaults.
    CHECK_JS_NUMBER(in, "(function (a = 0) { arguments[0] = 9; return a; })(1)", 1);
    CHECK_JS_NUMBER(in, "(function (a = 0) { a = 9; return arguments[0]; })(1)", 1);
    CHECK_JS_NUMBER(in, "(function (a) { arguments[0] = 9; return a; })(1)", 9);
    CHECK_JS_NUMBER(in, "(function (a = arguments.length) { return a; })(undefined, 2, 3)", 3);
    CHECK_JS_NUMBER(in, "(function (a, b = () => arguments) { return b()[0]; })(4)", 4);
    CHECK_JS_THROWS(in, "(function (a = 0) { return arguments.callee; })()", "TypeError");
    // Closures made in the defaults see the parameters and not the body's
    // declarations; a body var of a parameter's name starts as a copy.
    CHECK_JS_STRING(in, "(function (a, g = () => typeof x) { var x = 1; return g(); })()", "undefined");
    CHECK_JS_STRING(in, "(function (g = () => typeof h) { function h() {} return g(); })()", "undefined");
    CHECK_JS_NUMBER(in, "(function (a, g = () => a) { a = 2; return g(); })(1)", 2);
    CHECK_JS_NUMBER(in, "(function (a, g = () => a) { var a = 2; return g(); })(1)", 1);
    CHECK_JS_NUMBER(in, "(function (a, g = () => a) { var a; return a; })(3)", 3);
    CHECK_JS_STRING(in, "(function (a = this) { return typeof a; }).call(7)", "object");
    CHECK_JS_NUMBER(in, "'use strict'; (function (a = this) { return a; }).call(7)", 7);
    CHECK_JS_NUMBER(in, "'use strict'; (function (a = 1) { return a; })()", 1);
    CHECK_JS_THROWS(in, "(function (a = 1) { 'use strict'; })", "SyntaxError");
    CHECK_JS_THROWS(in, "(function (a, a = 1) {})", "SyntaxError");
    // Defaults are evaluated once per call, in order.
    CHECK_JS_STRING(in, "(function () { var log = []; function f(a = log.push('a'), b = log.push('b')) {} f(); f(1); return log.join(); })()", "a,b,b");
    // The Function constructor takes the same parameter grammar.
    CHECK_JS_NUMBER(in, "new Function('a = 1', '[b]', 'return a + b')(undefined, [2])", 3);
    CHECK_JS_NUMBER(in, "new Function('...r', 'return r.length')(1, 2)", 2);
    CHECK_JS_NUMBER(in, "new Function('a = 1', '[b]', 'return a + b').length", 0);
    // Arrows, methods and accessors.
    CHECK_JS_NUMBER(in, "((a, {b}, [c] = [3], ...d) => a + b + c + d.length)(1, {b: 2})", 6);
    CHECK_JS_NUMBER(in, "(([a]) => a)([4])", 4);
    CHECK_JS_NUMBER(in, "((a = 1, b = a) => a + b)()", 2);
    CHECK_JS_NUMBER(in, "((a = 1, b = a) => a + b)(5)", 10);
    CHECK_JS_NUMBER(in, "(function () { var o = { set x(v = 5) { this.y = v; } }; o.x = undefined; return o.y; })()", 5);
    CHECK_JS_NUMBER(in, "({ m([a]) { return a; } }).m([8])", 8);
    CHECK_JS_STRING(in, "(function (a = 1) { return this.k; }).call({ k: 'K' })", "K");
    CHECK_JS_NUMBER(in, "new (function (a = 4) { this.v = a; })().v", 4);
    // Arguments past the rest are gone; a default is not counted.
    CHECK_JS_NUMBER(in, "(function (a, ...r) { return arguments.length; })(1, 2, 3)", 3);
}

void test_catch_patterns()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "(function () { try { throw { message: 'm', code: 3 }; } catch ({ message, code = 0 }) { return message + code; } })()", "m3");
    CHECK_JS_NUMBER(in, "(function () { try { throw [1, 2]; } catch ([a, b]) { return a + b; } })()", 3);
    CHECK_JS_THROWS(in, "try { throw null; } catch ({ x }) { }", "TypeError");
    CHECK_JS_STRING(in, "(function () { var log = []; try { try { throw null; } catch ({ x }) { log.push('c'); } finally { log.push('f'); } } catch (e) { log.push(e.constructor.name); } return log.join(); })()", "f,TypeError");
    CHECK_JS_STRING(in, "(function () { try { throw new Error('E'); } catch ({ message: m, name }) { return name + ':' + m; } })()", "Error:E");
    CHECK_JS_NUMBER(in, "(function () { try { throw [1]; } catch ([a]) { let b = a + 1; return b; } })()", 2);
}

void test_syntax()
{
    js::Interpreter& in = fresh();
    CHECK_JS_THROWS(in, "({a = 1})", "SyntaxError");
    CHECK_JS_THROWS(in, "let [a, a] = [];", "SyntaxError");
    CHECK_JS_THROWS(in, "[...a,] = [];", "SyntaxError");
    CHECK_JS_THROWS(in, "var [a];", "SyntaxError");
    CHECK_JS_THROWS(in, "[a + 1] = [];", "SyntaxError");
    CHECK_JS_NUMBER(in, "(function () { var a; [(a)] = [1]; return a; })()", 1);
}

} // namespace

int main()
{
    test_array_bindings();
    test_object_bindings();
    test_assignment_patterns();
    test_parameters();
    test_catch_patterns();
    test_syntax();
    return sashfold::test::report("js_destructuring");
}
