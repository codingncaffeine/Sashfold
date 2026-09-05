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

void test_for_of_over_the_library()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "(function () { var out = ''; for (var x of [1, 2, 3]) out += x; return out; })()", "123");
    // A string is walked by code point: a surrogate pair is one step.
    CHECK_JS_STRING(in, "(function () { var out = []; for (const c of 'a\\uD834\\uDF06b') out.push(c.length); return out.join(); })()", "1,2,1");
    CHECK_JS_STRING(in, "(function () { var out = ''; for (var a of arguments) out += a; return out; })('x', 'y')", "xy");
    CHECK_JS_STRING(in, "(function () { 'use strict'; var out = ''; for (var a of arguments) out += a; return out; })('p', 'q')", "pq");
    // A hole reads as undefined, and the array is read again at every
    // step, so what the body appends is walked too.
    CHECK_JS_STRING(in, "(function () { var out = []; var a = [1, , 3]; for (var x of a) { out.push(x); if (out.length === 1) a.push(4); } return out.join(); })()", "1,,3,4");
    CHECK_JS_STRING(in, "(function () { var out = []; for (var k of ['a', 'b'].keys()) out.push(k); return out.join(); })()", "0,1");
    CHECK_JS_STRING(in, "(function () { var out = []; for (var e of ['a', 'b'].entries()) out.push(e[0] + e[1]); return out.join(); })()", "0a,1b");
    CHECK_JS_STRING(in, "(function () { var out = []; for (var v of Array.prototype.values.call({ length: 2, 0: 'p', 1: 'q' })) out.push(v); return out.join(); })()", "p,q");
    CHECK_JS_TRUE(in, "Array.prototype[Symbol.iterator] === Array.prototype.values && Array.prototype.values.name === 'values' && Array.prototype.values.length === 0");
    CHECK_JS_TRUE(in, "(function () { return arguments[Symbol.iterator] === Array.prototype.values; })()");
    CHECK_JS_TRUE(in, "(function () { 'use strict'; return arguments[Symbol.iterator] === Array.prototype.values; })()");
    CHECK_JS_TRUE(in, "typeof String.prototype[Symbol.iterator] === 'function' && String.prototype[Symbol.iterator].name === '[Symbol.iterator]'");
    // The chain: an iterator, %ArrayIteratorPrototype% with next and its
    // tag, then %IteratorPrototype%, whose @@iterator answers the receiver.
    CHECK_JS_TRUE(in, "(function () { var it = [][Symbol.iterator](); var proto = Object.getPrototypeOf(it); return proto[Symbol.toStringTag] === 'Array Iterator' && proto.hasOwnProperty('next') && !it.hasOwnProperty('next') && Object.getPrototypeOf(proto)[Symbol.iterator].call(it) === it && Object.getPrototypeOf(Object.getPrototypeOf(proto)) === Object.prototype; })()");
    CHECK_JS_TRUE(in, "Object.prototype.toString.call(''[Symbol.iterator]()) === '[object String Iterator]' && Object.prototype.toString.call([].keys()) === '[object Array Iterator]'");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(Object.getPrototypeOf(''[Symbol.iterator]())) === Object.getPrototypeOf(Object.getPrototypeOf([].keys()))");
    CHECK_JS_THROWS(in, "[][Symbol.iterator]().next.call({})", "TypeError");
    CHECK_JS_THROWS(in, "''[Symbol.iterator]().next.call([].keys())", "TypeError");
    CHECK_JS_THROWS(in, "String.prototype[Symbol.iterator].call(null)", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var it = [7].values(); var a = it.next(), b = it.next(), c = it.next(); return a.value === 7 && !a.done && b.value === undefined && b.done && c.done && Object.keys(a).join() === 'value,done'; })()");
    CHECK_JS_TRUE(in, "(function () { var it = 'ab'[Symbol.iterator](); var a = it.next(), b = it.next(), c = it.next(); return a.value === 'a' && b.value === 'b' && c.done && c.value === undefined; })()");
    // A lone surrogate is one step of its own.
    CHECK_JS_STRING(in, "(function () { var out = []; for (var c of '\\uD800x\\uDC00') out.push(c.charCodeAt(0).toString(16)); return out.join(); })()", "d800,78,dc00");
}

void test_the_protocol()
{
    js::Interpreter& in = fresh();
    // A hand-written iterable that logs what is called: return() runs for
    // break, return and throw, never for exhaustion or a plain continue.
    std::string const iterable = "var log = []; var it = { i: 0, next: function () { log.push('next'); return this.i < 3 ? { value: this.i++, done: false } : { value: undefined, done: true }; }, return: function () { log.push('return'); return {}; } }; var obj = {}; obj[Symbol.iterator] = function () { log.push('iter'); return it; }; ";
    CHECK_JS_STRING(in, iterable + "(function () { for (var x of obj) ; return log.join(); })()", "iter,next,next,next,next");
    CHECK_JS_STRING(in, iterable + "(function () { for (var x of obj) break; return log.join(); })()", "iter,next,return");
    CHECK_JS_STRING(in, iterable + "(function () { for (var x of obj) return 'r'; })() + ' ' + log.join()", "r iter,next,return");
    CHECK_JS_STRING(in, iterable + "(function () { try { for (var x of obj) throw new Error('body'); } catch (e) { return e.message + ' ' + log.join(); } })()", "body iter,next,return");
    CHECK_JS_STRING(in, iterable + "(function () { for (var x of obj) continue; return log.join(); })()", "iter,next,next,next,next");
    CHECK_JS_STRING(in, iterable + "(function () { outer: for (var k of [1]) { for (var x of obj) continue outer; } return log.join(); })()", "iter,next,return");
    CHECK_JS_STRING(in, iterable + "(function () { outer: for (var k of [1]) { for (var x of obj) break outer; } return log.join(); })()", "iter,next,return");
    // A throw from next() closes nothing; a throw from return() loses to
    // the body's own throw and wins over a break.
    CHECK_JS_STRING(in, "(function () { var log = []; var obj = {}; obj[Symbol.iterator] = function () { return { next: function () { throw new Error('next'); }, return: function () { log.push('return'); } }; }; try { for (var x of obj) ; } catch (e) { return e.message + log.length; } })()", "next0");
    CHECK_JS_STRING(in, "(function () { var obj = {}; obj[Symbol.iterator] = function () { return { next: function () { return { done: false }; }, return: function () { throw new Error('ret'); } }; }; try { for (var x of obj) throw new Error('body'); } catch (e) { return e.message; } })()", "body");
    CHECK_JS_STRING(in, "(function () { var obj = {}; obj[Symbol.iterator] = function () { return { next: function () { return { done: false }; }, return: function () { throw new Error('ret'); } }; }; try { for (var x of obj) break; } catch (e) { return e.message; } })()", "ret");
    // return() must answer an object, as must next(); the subject must be
    // iterable and its @@iterator must answer an object.
    CHECK_JS_THROWS(in, "(function () { var obj = {}; obj[Symbol.iterator] = function () { return { next: function () { return { done: false }; }, return: function () { return 1; } }; }; for (var x of obj) break; })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { var obj = {}; obj[Symbol.iterator] = function () { return { next: function () { return 1; } }; }; for (var x of obj) ; })()", "TypeError");
    CHECK_JS_THROWS(in, "for (var x of 5) ;", "TypeError");
    CHECK_JS_THROWS(in, "for (var x of undefined) ;", "TypeError");
    CHECK_JS_THROWS(in, "for (var x of {}) ;", "TypeError");
    CHECK_JS_THROWS(in, "(function () { var obj = {}; obj[Symbol.iterator] = function () { return 1; }; for (var x of obj) ; })()", "TypeError");
    // `done` is read before `value`, and a truthy done ends the loop
    // without reading the value.
    CHECK_JS_STRING(in, "(function () { var log = []; var obj = {}; obj[Symbol.iterator] = function () { return { next: function () { var r = {}; Object.defineProperty(r, 'done', { get: function () { log.push('done'); return true; } }); Object.defineProperty(r, 'value', { get: function () { log.push('value'); return 1; } }); return r; } }; }; for (var x of obj) ; return log.join(); })()", "done");
    // next is read once, when the loop starts.
    CHECK_JS_NUMBER(in, "(function () { var reads = 0; var obj = {}; obj[Symbol.iterator] = function () { var it = { i: 0 }; Object.defineProperty(it, 'next', { get: function () { reads++; return function () { return { value: this.i, done: this.i++ > 1 }; }; } }); return it; }; for (var x of obj) ; return reads; })()", 1);
}

void test_bindings_and_syntax()
{
    js::Interpreter& in = fresh();
    // A fresh const or let per iteration, closed over.
    CHECK_JS_STRING(in, "(function () { var fs = []; for (const x of [1, 2, 3]) fs.push(function () { return x; }); return fs.map(function (f) { return f(); }).join(); })()", "1,2,3");
    CHECK_JS_STRING(in, "(function () { var fs = []; for (let x of [1, 2]) { fs.push(function () { return x; }); x = x * 10; } return fs.map(function (f) { return f(); }).join(); })()", "10,20");
    CHECK_JS_THROWS(in, "for (const x of [1]) x = 2;", "TypeError");
    // The head's expression sees the name in its dead zone.
    CHECK_JS_THROWS(in, "for (let x of x) ;", "ReferenceError");
    // An assignment target: a member, evaluated every step, and a var.
    CHECK_JS_STRING(in, "(function () { var o = {}, i = 0; for (o['k' + i++] of ['a', 'b']) ; return Object.keys(o).join() + o.k0 + o.k1; })()", "k0,k1ab");
    CHECK_JS_NUMBER(in, "(function () { var x; for (x of [3, 4]) ; return x; })()", 4);
    CHECK_JS_NUMBER(in, "(function () { for (var y of [9]) ; return y; })()", 9);
    // A target that throws closes the iterator.
    CHECK_JS_STRING(in, "(function () { var log = []; var obj = {}; obj[Symbol.iterator] = function () { return { next: function () { return { value: 1, done: false }; }, return: function () { log.push('return'); return {}; } }; }; try { for (null.x of obj) ; } catch (e) { return log.join(); } })()", "return");
    // The loop's completion value.
    CHECK_JS_NUMBER(in, "eval('for (var z of [1, 2]) z * 2')", 4);
    // A bare break keeps the value so far; one inside an `if` carries the
    // if statement's undefined (UpdateEmpty, §14.6.2).
    CHECK_JS_NUMBER(in, "eval('for (var z of [1, 2]) { z; break; }')", 1);
    CHECK_JS_TRUE(in, "eval('for (var z of [1, 2]) { if (z === 2) break; z; }') === undefined");
    CHECK_JS_TRUE(in, "eval('for (var z of []) 1') === undefined");
    // Syntax: no initializer, one binding, a simple target, no comma in
    // the iterable; `of` itself is an ordinary name.
    CHECK_JS_THROWS(in, "for (var x = 1 of []) ;", "SyntaxError");
    CHECK_JS_THROWS(in, "for (let x, y of []) ;", "SyntaxError");
    CHECK_JS_THROWS(in, "for (x + 1 of []) ;", "SyntaxError");
    CHECK_JS_THROWS(in, "for (async of []) ;", "SyntaxError");
    CHECK_JS_THROWS(in, "for (var x of [1], [2]) ;", "SyntaxError");
    CHECK_JS_NUMBER(in, "(function () { var of = 2; for (of of [5]) ; return of; })()", 5);
    CHECK_JS_NUMBER(in, "(function () { var of = [1]; var n = 0; for (var x of of) n += x; return n; })()", 1);
    CHECK_JS_NUMBER(in, "(function () { var n = 0; for ((async) of [2]) n = async; return n; })()", 2);
    CHECK_JS_NUMBER(in, "(function () { var n = 0; for (var x of [1, 2, 3]) { if (x === 2) continue; n += x; } return n; })()", 4);
    // The parser's rendering of the tree.
    CHECK_JS_TRUE(in, "(function () { var n = 0; label: for (var x of [1]) { for (var y of [2]) { n += x * y; break label; } } return n === 2; })()");
}

void test_the_library_over_iterables()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "Array.from('a\\uD834\\uDF06b').map(function (s) { return s.length; }).join()", "1,2,1");
    CHECK_JS_STRING(in, "Array.from([1, 2].keys()).join()", "0,1");
    CHECK_JS_STRING(in, "Array.from({ length: 2, 0: 'x', 1: 'y' }).join()", "x,y");
    CHECK_JS_STRING(in, "Array.from([1, 2], function (v, i) { return v * 10 + i; }).join()", "10,21");
    CHECK_JS_TRUE(in, "(function () { var a = Array.from([1, , 3]); return a.length === 3 && a.hasOwnProperty(1) && a[1] === undefined; })()");
    // A mapper that throws closes the iterator, and its throw is the outcome.
    CHECK_JS_STRING(in, "(function () { var log = []; var obj = {}; obj[Symbol.iterator] = function () { return { i: 0, next: function () { return { value: this.i, done: this.i++ > 1 }; }, return: function () { log.push('return'); return {}; } }; }; try { Array.from(obj, function () { throw new Error('map'); }); } catch (e) { return e.message + log.join(); } })()", "mapreturn");
    // The @@iterator method is read once and used even when the object
    // has a length of its own.
    CHECK_JS_STRING(in, "(function () { var reads = 0; var o = { length: 5 }; Object.defineProperty(o, Symbol.iterator, { get: function () { reads++; return function () { return [8, 9][Symbol.iterator](); }; } }); return Array.from(o).join() + reads; })()", "8,91");
    CHECK_JS_STRING(in, "(function () { var o = Object.fromEntries([['a', 1], ['b', 2]]); return Object.keys(o).join() + o.a + o.b; })()", "a,b12");
    CHECK_JS_STRING(in, "(function () { var pairs = {}; pairs[Symbol.iterator] = function () { var i = 0; return { next: function () { return i < 2 ? { value: ['k' + i, i++], done: false } : { done: true }; } }; }; var o = Object.fromEntries(pairs); return o.k0 + ',' + o.k1; })()", "0,1");
    CHECK_JS_STRING(in, "(function () { var log = []; var obj = {}; obj[Symbol.iterator] = function () { return { next: function () { return { value: 1, done: false }; }, return: function () { log.push('return'); return {}; } }; }; try { Object.fromEntries(obj); } catch (e) { return e.constructor.name + log.join(); } })()", "TypeErrorreturn");
    CHECK_JS_THROWS(in, "Object.fromEntries([1])", "TypeError");
    CHECK_JS_THROWS(in, "Object.fromEntries(5)", "TypeError");
    CHECK_JS_THROWS(in, "Object.fromEntries()", "TypeError");
    CHECK_JS_STRING(in, "Array.from(Array.prototype.entries.call('ab')).join(';')", "0,a;1,b");
}

void test_spread()
{
    js::Interpreter& in = fresh();
    // Array literals and argument lists spread any iterable, in place.
    CHECK_JS_STRING(in, "[...'ab', 0, ...[1, 2]].join()", "a,b,0,1,2");
    CHECK_JS_NUMBER(in, "Math.max(...[1, 5, 3])", 5);
    CHECK_JS_NUMBER(in, "(function () { return arguments.length; })(...[1, 2], 3, ...'xy')", 5);
    CHECK_JS_NUMBER(in, "new Date(...[2020, 0, 1]).getFullYear()", 2020);
    CHECK_JS_NUMBER(in, "(function () { var a = [3, 4]; return Math.min(...a, ...a); })()", 3);
    // A hole spreads as undefined and becomes a real element.
    CHECK_JS_TRUE(in, "(function () { var a = [...[1, , 3]]; return a.length === 3 && a.hasOwnProperty(1) && a[1] === undefined; })()");
    CHECK_JS_TRUE(in, "Array.isArray([...[]]) && [...[]].length === 0");
    // A custom iterable is walked by the protocol.
    CHECK_JS_STRING(in, "(function () { var it = {}; it[Symbol.iterator] = function () { var i = 0; return { next: function () { return i < 3 ? { value: i++ * 2, done: false } : { done: true }; } }; }; return [...it].join() + ' ' + String(...it); })()", "0,2,4 0");
    CHECK_JS_THROWS(in, "[...5]", "TypeError");
    CHECK_JS_THROWS(in, "Math.max(...{})", "TypeError");
    CHECK_JS_THROWS(in, "[...undefined]", "TypeError");
    // Object spread copies own enumerable properties, strings and symbols
    // alike, later ones winning; null and undefined copy nothing.
    CHECK_JS_STRING(in, "(function () { var o = { ...{ a: 1, b: 2 }, b: 3, ...null, ...undefined }; return Object.keys(o).join() + o.a + o.b; })()", "a,b13");
    CHECK_JS_TRUE(in, "(function () { var s = Symbol(); var src = { x: 1 }; src[s] = 2; Object.defineProperty(src, 'hidden', { value: 3, enumerable: false }); var proto = { inherited: 4 }; Object.setPrototypeOf(src, proto); var o = { ...src }; return o.x === 1 && o[s] === 2 && !('hidden' in o) && !('inherited' in o); })()");
    CHECK_JS_STRING(in, "(function () { var o = { ...'ab' }; return Object.keys(o).join() + o[0] + o[1]; })()", "0,1ab");
    CHECK_JS_TRUE(in, "(function () { var o = { ...[7, 8] }; return o[0] === 7 && o[1] === 8 && !('length' in o); })()");
    // A getter on the source runs once, and a copy is a data property
    // whatever the source's was.
    CHECK_JS_TRUE(in, "(function () { var reads = 0; var src = { get g() { reads++; return 9; } }; var o = { ...src }; var d = Object.getOwnPropertyDescriptor(o, 'g'); return reads === 1 && d.value === 9 && d.writable && d.enumerable; })()");
    CHECK_JS_STRING(in, "(function () { var log = []; var o = { ...{ get a() { log.push('a'); return 1; } }, b: (log.push('b'), 2), ...{ get c() { log.push('c'); return 3; } } }; return log.join() + Object.keys(o).join(); })()", "a,b,ca,b,c");
}

} // namespace

int main()
{
    test_for_of_over_the_library();
    test_the_protocol();
    test_bindings_and_syntax();
    test_the_library_over_iterables();
    test_spread();
    return sashfold::test::report("js_iterator");
}
