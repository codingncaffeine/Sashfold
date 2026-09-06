#include "JsTest.h"

// Generators (§27.5) on the bytecode tier: the protocol, the body's
// control flow around a yield, yield*, the intrinsics — every realm under
// heap stress, so an unrooted frame value fails at once.

#include <string>

using namespace sashfold;

namespace {

js::Interpreter& fresh()
{
    static js::Interpreter* interpreter = nullptr;
    delete interpreter;
    interpreter = new js::Interpreter;
    interpreter->heap().set_stress(true);
    return *interpreter;
}

// The values a generator produces, joined; a thrown value ends the list
// with "throw:" and its description.
std::string collect(js::Interpreter& in, std::string_view source)
{
    std::string program = "var out = []; (function () { var g = " + std::string(source)
        + "; try { for (var v of g) out.push(String(v)); } catch (e) { out.push('throw:' + e); } })(); out.join(' ')";
    return test::eval_string(in, program);
}

void test_protocol()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "function* g() { yield 1; yield 2; } var it = g(); JSON.stringify([it.next(), it.next(), it.next(), it.next()])",
        "[{\"value\":1,\"done\":false},{\"value\":2,\"done\":false},{\"done\":true},{\"done\":true}]");
    CHECK_JS_STRING(in, "function* g() { return 5; } var it = g(); JSON.stringify(it.next())", "{\"value\":5,\"done\":true}");
    CHECK_JS_STRING(in, "function* g() { yield 1; return 2; } var it = g(); JSON.stringify([it.next(), it.next(), it.next()])",
        "[{\"value\":1,\"done\":false},{\"value\":2,\"done\":true},{\"done\":true}]");
    // The value sent in is the yield's value; the first next()'s is dropped.
    CHECK_JS_STRING(in, "function* g() { var a = yield 'first'; var b = yield a + 1; return b * 2; } var it = g(); [it.next('x').value, it.next(10).value, it.next(7).value].join()", "first,11,14");
    // Nothing runs before the first next().
    CHECK_JS_STRING(in, "var log = []; function* g() { log.push('body'); yield; } var it = g(); log.push('made'); it.next(); log.join()", "made,body");
    CHECK_EQ(collect(in, "(function* () { yield 'a'; yield 'b'; yield 'c'; })()"), "a b c");
    CHECK_EQ(collect(in, "(function* () {})()"), "");
    // return() and throw() at each state.
    CHECK_JS_STRING(in, "function* g() { yield 1; } var it = g(); JSON.stringify([it.return(9), it.next()])", "[{\"value\":9,\"done\":true},{\"done\":true}]");
    CHECK_JS_STRING(in, "function* g() { yield 1; yield 2; } var it = g(); it.next(); JSON.stringify([it.return(9), it.next()])", "[{\"value\":9,\"done\":true},{\"done\":true}]");
    CHECK_JS_STRING(in, "function* g() { yield 1; } var it = g(); it.next(); it.next(); JSON.stringify(it.return(3))", "{\"value\":3,\"done\":true}");
    CHECK_JS_STRING(in, "function* g() { yield 1; } var it = g(); try { it.throw(new Error('e')); } catch (e) { var m = e.message; } m + ':' + JSON.stringify(it.next())", "e:{\"done\":true}");
    CHECK_JS_STRING(in, "function* g() { try { yield 1; } catch (e) { yield 'caught ' + e; } } var it = g(); it.next(); it.throw('x').value", "caught x");
    CHECK_JS_STRING(in, "function* g() { yield 1; } var it = g(); it.next(); it.next(); try { it.throw('late'); } catch (e) { var r = e; } r", "late");
    // Re-entry while running is a TypeError.
    CHECK_JS_THROWS(in, "var it; function* g() { it.next(); } it = g(); it.next()", "TypeError");
    // A generator is not a constructor; `this` is what the call gives.
    CHECK_JS_THROWS(in, "function* g() {} new g()", "TypeError");
    CHECK_JS_STRING(in, "var o = { *g() { yield this === o; } }; String(o.g().next().value)", "true");
    // arguments and closures inside the body.
    CHECK_JS_NUMBER(in, "function* g() { yield arguments.length; yield arguments[1]; } var it = g(1, 2, 3); it.next().value * 10 + it.next().value", 32);
    CHECK_JS_STRING(in, "function* g() { var fs = []; for (var i = 0; i < 3; i++) { let j = i; fs.push(function () { return j; }); yield i; } return fs.map(function (f) { return f(); }).join(); } var it = g(); it.next(); it.next(); it.next(); it.next().value", "0,1,2");
}

void test_control_flow()
{
    js::Interpreter& in = fresh();
    // Loops, conditionals, switch, labels around yields.
    CHECK_EQ(collect(in, "(function* () { for (var i = 0; i < 3; i++) yield i; })()"), "0 1 2");
    CHECK_EQ(collect(in, "(function* () { var i = 0; while (i < 3) { if (i % 2) yield 'odd' + i; else yield 'even' + i; i++; } })()"), "even0 odd1 even2");
    CHECK_EQ(collect(in, "(function* () { var i = 0; do { yield i; } while (++i < 2); })()"), "0 1");
    CHECK_EQ(collect(in, "(function* () { for (var k in { a: 1, b: 2 }) yield k; })()"), "a b");
    CHECK_EQ(collect(in, "(function* () { for (var v of [1, 2]) yield v * 2; })()"), "2 4");
    CHECK_EQ(collect(in, "(function* () { for (let v of [1, 2]) yield (function () { return v; })(); })()"), "1 2");
    CHECK_EQ(collect(in, "(function* () { switch (2) { case 1: yield 'one'; case 2: yield 'two'; case 3: yield 'three'; break; default: yield 'd'; } })()"), "two three");
    CHECK_EQ(collect(in, "(function* () { outer: for (var i = 0; i < 3; i++) { for (var j = 0; j < 3; j++) { if (j == 1) continue outer; if (i == 2) break outer; yield i + '' + j; } } })()"), "00 10");
    CHECK_EQ(collect(in, "(function* () { var x = 1; { let x = 2; yield x; } yield x; })()"), "2 1");
    CHECK_EQ(collect(in, "(function* () { with ({ p: 3 }) { yield p; } })()"), "3");
    // Yields inside expressions: the operand stack survives the suspension.
    CHECK_JS_NUMBER(in, "function* g() { var r = 1 + (yield) * 10; return r; } var it = g(); it.next(); it.next(4).value", 41);
    CHECK_JS_STRING(in, "function* g() { return [yield 'a', yield 'b']; } var it = g(); it.next(); it.next(1); it.next(2).value.join()", "1,2");
    CHECK_JS_STRING(in, "function* g() { var o = { a: yield 1, b: yield 2 }; return JSON.stringify(o); } var it = g(); it.next(); it.next('x'); it.next('y').value", "{\"a\":\"x\",\"b\":\"y\"}");
    CHECK_JS_STRING(in, "function* g() { return f(yield 1, yield 2); } function f(a, b) { return a + b; } var it = g(); it.next(); it.next('p'); it.next('q').value", "pq");
    CHECK_JS_STRING(in, "function* g() { return `${yield 1}-${yield 2}`; } var it = g(); it.next(); it.next('p'); it.next('q').value", "p-q");
    CHECK_JS_STRING(in, "function* g() { var [a, b = yield 'dflt'] = [1]; return a + ':' + b; } var it = g(); it.next(); it.next('D').value", "1:D");
    CHECK_JS_STRING(in, "function* g() { var o = {}; o[yield 'k'] = yield 'v'; return JSON.stringify(o); } var it = g(); it.next(); it.next('key'); it.next('val').value", "{\"key\":\"val\"}");
    CHECK_JS_STRING(in, "function* g() { return (yield 1) ? 'yes' : 'no'; } var it = g(); it.next(); it.next(0).value", "no");
    CHECK_JS_STRING(in, "function* g() { return (yield 1) && (yield 2); } var it = g(); it.next(); JSON.stringify(it.next(false))", "{\"value\":false,\"done\":true}");
    // Classes, eval, this and closures inside a generator body.
    CHECK_JS_STRING(in, "function* g() { class A { m() { return 'm'; } } yield new A().m(); } g().next().value", "m");
    CHECK_JS_NUMBER(in, "function* g() { var x = 2; yield eval('x * 21'); } g().next().value", 42);
    CHECK_JS_STRING(in, "function* g() { var self = this; yield () => self === this; } String(g.call({}).next().value())", "true");
    // try/catch/finally with yields inside each.
    CHECK_EQ(collect(in, "(function* () { try { yield 'try'; throw 'x'; } catch (e) { yield 'catch ' + e; } finally { yield 'finally'; } yield 'after'; })()"), "try catch x finally after");
    CHECK_EQ(collect(in, "(function* () { try { yield 1; return 2; } finally { yield 3; } })()"), "1 3");
    CHECK_JS_STRING(in, "function* g() { try { yield 1; } finally { log.push('fin'); } } var log = []; var it = g(); it.next(); JSON.stringify(it.return(7)) + log.join()", "{\"value\":7,\"done\":true}fin");
    // return() runs the finally, which may yield again and even override.
    CHECK_JS_STRING(in, "function* g() { try { yield 1; } finally { yield 'cleanup'; } } var it = g(); it.next(); JSON.stringify([it.return(7), it.next()])", "[{\"value\":\"cleanup\",\"done\":false},{\"value\":7,\"done\":true}]");
    CHECK_JS_STRING(in, "function* g() { try { yield 1; } finally { return 'override'; } } var it = g(); it.next(); JSON.stringify(it.return(7))", "{\"value\":\"override\",\"done\":true}");
    CHECK_JS_STRING(in, "function* g() { try { yield 1; } finally { throw 'boom'; } } var it = g(); it.next(); try { it.return(7); } catch (e) { var r = e; } r", "boom");
    // A for-of over a generator that breaks early calls return(), which runs the finally.
    CHECK_JS_STRING(in, "var log = []; function* g() { try { yield 1; yield 2; } finally { log.push('closed'); } } for (var v of g()) { log.push(v); break; } log.join()", "1,closed");
    CHECK_JS_STRING(in, "var log = []; function* g() { try { yield 1; yield 2; } finally { log.push('closed'); } } (function () { for (var v of g()) { log.push(v); return; } })(); log.join()", "1,closed");
    // A throw inside the body propagates and finishes the generator.
    CHECK_JS_STRING(in, "function* g() { yield 1; throw new RangeError('r'); } var it = g(); it.next(); try { it.next(); } catch (e) { var n = e.name; } n + ':' + JSON.stringify(it.next())", "RangeError:{\"done\":true}");
    // Nested finally blocks unwind in order on return().
    CHECK_JS_STRING(in, "var log = []; function* g() { try { try { yield 1; } finally { log.push('inner'); } } finally { log.push('outer'); } } var it = g(); it.next(); it.return(); log.join()", "inner,outer");
    // break through a finally inside a loop, inside a generator.
    CHECK_EQ(collect(in, "(function* () { for (var i = 0; i < 3; i++) { try { if (i == 1) break; yield i; } finally { yield 'f' + i; } } yield 'end'; })()"), "0 f0 f1 end");
    // Destructuring of a generator's values closes it when the pattern ends first.
    CHECK_JS_STRING(in, "var log = []; function* g() { try { yield 1; yield 2; yield 3; } finally { log.push('closed'); } } var [a, b] = g(); a + b + log.join()", "3closed");
}

void test_yield_star()
{
    js::Interpreter& in = fresh();
    CHECK_EQ(collect(in, "(function* () { yield 0; yield* [1, 2]; yield* (function* () { yield 3; yield 4; })(); yield 5; })()"), "0 1 2 3 4 5");
    // The inner generator's return value is the yield* expression's value.
    CHECK_JS_NUMBER(in, "function* inner() { yield 1; return 10; } function* outer() { var r = yield* inner(); yield r; } var it = outer(); it.next(); it.next().value", 10);
    // Sent values reach the inner generator.
    CHECK_JS_STRING(in, "function* inner() { var a = yield 'i1'; var b = yield 'i2'; return a + b; } function* outer() { return yield* inner(); } var it = outer(); it.next(); it.next('x'); JSON.stringify(it.next('y'))", "{\"value\":\"xy\",\"done\":true}");
    // throw() is forwarded to the inner iterator's throw method.
    CHECK_JS_STRING(in, "function* inner() { try { yield 1; } catch (e) { yield 'inner caught ' + e; } } function* outer() { yield* inner(); } var it = outer(); it.next(); it.throw('t').value", "inner caught t");
    // return() is forwarded too, and the inner finally runs.
    CHECK_JS_STRING(in, "var log = []; function* inner() { try { yield 1; } finally { log.push('inner fin'); } } function* outer() { try { yield* inner(); } finally { log.push('outer fin'); } } var it = outer(); it.next(); JSON.stringify(it.return(5)) + log.join()", "{\"value\":5,\"done\":true}inner fin,outer fin");
    // An iterator without a throw method is closed and a TypeError raised.
    CHECK_JS_STRING(in, "var closed = false; var iterable = { [Symbol.iterator]() { return { next() { return { value: 1, done: false }; }, return() { closed = true; return {}; } }; } }; function* g() { yield* iterable; } var it = g(); it.next(); try { it.throw('x'); } catch (e) { var n = e.name; } n + ':' + closed", "TypeError:true");
    // A string is iterable by code points; the result objects pass through untouched.
    CHECK_EQ(collect(in, "(function* () { yield* 'ab'; })()"), "a b");
    CHECK_JS_STRING(in, "var inner = { [Symbol.iterator]() { return { next() { return { value: 'v', done: false, extra: 'kept' }; } }; } }; function* g() { yield* inner; } JSON.stringify(g().next())", "{\"value\":\"v\",\"done\":false,\"extra\":\"kept\"}");
}

void test_intrinsics()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "function* g() {} Object.prototype.toString.call(g())", "[object Generator]");
    CHECK_JS_STRING(in, "function* g() {} Object.prototype.toString.call(g)", "[object GeneratorFunction]");
    CHECK_JS_TRUE(in, "function* g() {} Object.getPrototypeOf(g()) === g.prototype");
    CHECK_JS_TRUE(in, "function* g() {} Object.getPrototypeOf(g.prototype) === Object.getPrototypeOf(function* () {}).prototype");
    CHECK_JS_TRUE(in, "function* g() {} var GeneratorFunction = Object.getPrototypeOf(g).constructor; GeneratorFunction.prototype === Object.getPrototypeOf(g)");
    CHECK_JS_FALSE(in, "function* g() {} g.prototype.hasOwnProperty('constructor')");
    CHECK_JS_STRING(in, "function* g() {} JSON.stringify(Object.getOwnPropertyDescriptor(g, 'prototype'))", "{\"value\":{},\"writable\":true,\"enumerable\":false,\"configurable\":false}");
    CHECK_JS_FALSE(in, "function* g() {} g.hasOwnProperty('caller') || g.hasOwnProperty('arguments')");
    CHECK_JS_TRUE(in, "function* g() {} var it = g(); it[Symbol.iterator]() === it");
    CHECK_JS_TRUE(in, "function* g() {} var p = Object.getPrototypeOf(g.prototype); typeof p.next === 'function' && typeof p.return === 'function' && typeof p.throw === 'function'");
    CHECK_JS_STRING(in, "function* g() {} Object.getPrototypeOf(g.prototype)[Symbol.toStringTag]", "Generator");
    CHECK_JS_STRING(in, "function* g(a, b) { yield a; } g.name + g.length", "g2");
    CHECK_JS_STRING(in, "function* g() { yield 1 }; g.toString()", "function* g() { yield 1 }");
    CHECK_JS_STRING(in, "var o = { async *m() {}, *n() {} }; typeof o.n().next", "function");
    // A generator's `prototype` property can be replaced, and a non-object one falls back.
    CHECK_JS_TRUE(in, "function* g() {} var p = { custom: true }; g.prototype = p; Object.getPrototypeOf(g()) === p");
    CHECK_JS_TRUE(in, "function* g() {} g.prototype = 5; Object.getPrototypeOf(g()) === Object.getPrototypeOf(function* () {}).prototype");
    // The GeneratorFunction constructor.
    CHECK_JS_STRING(in, "var GeneratorFunction = Object.getPrototypeOf(function* () {}).constructor; var g = new GeneratorFunction('a', 'yield a; yield a * 2;'); var it = g(21); it.next().value + ',' + it.next().value", "21,42");
    CHECK_JS_STRING(in, "var GeneratorFunction = Object.getPrototypeOf(function* () {}).constructor; GeneratorFunction('yield 1').toString()", "function* anonymous(\n) {\nyield 1\n}");
    CHECK_JS_STRING(in, "var GeneratorFunction = Object.getPrototypeOf(function* () {}).constructor; GeneratorFunction.name + GeneratorFunction.length + (Object.getPrototypeOf(GeneratorFunction) === Function)", "GeneratorFunction1true");
    // next/return/throw on a non-generator receiver.
    CHECK_JS_THROWS(in, "function* g() {} g.prototype.next.call({})", "TypeError");
    // Generator methods in classes and objects, static too.
    CHECK_JS_STRING(in, "class A { *m() { yield 'a'; } static *s() { yield 's'; } } A.s().next().value + new A().m().next().value", "sa");
    CHECK_JS_STRING(in, "(function () { class B { *[Symbol.iterator]() { yield 1; yield 2; } } return [...new B()].join(); })()", "1,2");
    CHECK_JS_STRING(in, "var o = { *[('comp' + 'uted')]() { yield 7; } }; o.computed().next().value + o.computed.name", "7computed");
    // Spread and Array.from drive the protocol.
    CHECK_JS_STRING(in, "function* g() { yield 1; yield 2; yield 3; } [...g()].join() + Array.from(g()).length", "1,2,33");
    // A generator function declared inside a block, hoisted like a function.
    CHECK_JS_NUMBER(in, "(function () { { function* g() { yield 4; } return g().next().value; } })()", 4);
}

} // namespace

// A named generator expression binds its own name, immutably, in a scope
// of its own (§15.5.5): the parameters and the body see the function, a
// write is ignored in sloppy code and a TypeError in strict code, and
// the enclosing scope never sees the name.
void test_own_name()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "var f = function* g() { return g; }; f().next().value === f");
    CHECK_JS_TRUE(in, "var f = function* g(p = function () { return g; }) { return p; }; f().next().value() === f");
    CHECK_JS_TRUE(in, "var f = function* g() { g = 1; return g; }; f().next().value === f");
    CHECK_JS_THROWS(in, "var f = function* g() { 'use strict'; g = 1; }; f().next()", "TypeError");
    CHECK_JS_STRING(in, "var probe; var f = function* g() { probe = function () { return g; }; }; var g = 'outside'; f().next(); g + ':' + (probe() === f)", "outside:true");
}

// Code after a throw or a return still compiles: the depth model stays
// dead across the labels such code binds, so a yield, a short-circuit
// or an optional chain there cannot underflow it (test262
// GeneratorPrototype/throw/try-finally-nested-try-catch-within-finally).
void test_dead_code()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "function* g() { return 1; yield 2; } g().next().value", 1);
    CHECK_JS_NUMBER(in, "function* g() { throw 1; x = a && b; f(a || b); a?.b.c; x = { [yield 2]: 3 }; } try { g().next(); } catch (e) { var c = e; } c", 1);
    CHECK_JS_STRING(in, "var log = []; function* g() { try { yield 1; throw new Error('e'); try { yield 2; } catch (e) { yield e; } yield 3; } finally { yield 4; log.push('fin'); } yield 5; } var it = g(); it.next(); log.push(it.next().value); try { it.throw(new RangeError('r')); } catch (e) { log.push(e.name); } log.push(it.next().done); log.join()", "4,RangeError,true");
}

int main()
{
    test_protocol();
    test_control_flow();
    test_yield_star();
    test_intrinsics();
    test_own_name();
    test_dead_code();
    return sashfold::test::report("js_generator");
}
