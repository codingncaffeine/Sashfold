#include "JsTest.h"

#include "js/Interpreter.h"

#include <string>

using namespace sashfold;

namespace {

// Every realm here runs under heap stress, as the promise tests do: a
// suspended async frame is heap data the collector has to find.
js::Interpreter& fresh()
{
    static js::Interpreter* interpreter = nullptr;
    delete interpreter;
    interpreter = new js::Interpreter();
    interpreter->heap().set_stress(true);
    return *interpreter;
}

// Runs a script that fills a global `out` array, drains the job queue as
// a host's checkpoint would, and returns `out` joined — or the throw.
std::string settled(js::Interpreter& in, std::string_view source)
{
    test::JsRun const run = test::run_js(in, source);
    if (!run.ok)
        return "threw " + run.thrown;
    in.run_jobs([&in](js::Value const& thrown) {
        test::fail("a job threw: " + in.describe(thrown), __FILE__, __LINE__);
    });
    return test::eval_string(in, "out.join(' ')");
}

void test_calls_and_results()
{
    js::Interpreter& in = fresh();
    // The body runs to its first await inside the call; the rest runs
    // from the job queue, after the caller's own code.
    CHECK_EQ(settled(in, "var out = []; async function f() { out.push('a'); await null; out.push('c'); } f(); out.push('b');"), "a b c");
    // The call's value is a promise; return settles it, and a returned
    // promise is adopted first.
    CHECK_EQ(settled(in, "var out = []; async function f() { return 7; } var p = f(); out.push(p instanceof Promise); p.then(function (v) { out.push(v); });"), "true 7");
    CHECK_EQ(settled(in, "var out = []; async function f() { return Promise.resolve(8); } f().then(function (v) { out.push(v); });"), "8");
    CHECK_EQ(settled(in, "var out = []; async function f() {} f().then(function (v) { out.push(typeof v); });"), "undefined");
    // Async arrows and methods; an arrow keeps the enclosing this.
    CHECK_EQ(settled(in, "var out = []; var o = { async m() { return this === o; } }; o.m().then(function (v) { out.push(v); }); var a = async x => x * 2; a(21).then(function (v) { out.push(v); });"), "true 42");
    CHECK_EQ(settled(in, "var out = []; var o = { v: 3, m() { var a = async () => this.v; return a(); } }; o.m().then(function (v) { out.push(v); });"), "3");
    // An async function expression is a PrimaryExpression: it takes a
    // call or member tail, and it has no prototype property.
    CHECK_EQ(settled(in, "var out = []; out.push(async function () {}() instanceof Promise, typeof (async function () {}).prototype, 'prototype' in async function () {});"), "true undefined false");
    CHECK_JS_STRING(in, "Object.getPrototypeOf(async function () {})[Symbol.toStringTag]", "AsyncFunction");
    CHECK_JS_STRING(in, "(async function foo() {}).name + ':' + (async function () {}).length + ':' + (async function (a, b) {}).length", "foo:0:2");
    CHECK_JS_THROWS(in, "new (async function () {})()", "TypeError");
}

void test_await_ordering()
{
    js::Interpreter& in = fresh();
    // Each await costs one job, a native promise included (PromiseResolve
    // hands the promise itself back); a thenable is adopted through a job
    // of its own first.
    CHECK_EQ(settled(in, "var out = []; async function f() { await null; out.push('f1'); await null; out.push('f2'); } async function g() { out.push('g0'); await null; out.push('g1'); } f(); g(); Promise.resolve().then(function () { out.push('p'); });"), "g0 f1 g1 p f2");
    CHECK_EQ(settled(in, "var out = []; async function f() { await Promise.resolve(); out.push('x'); } f(); Promise.resolve().then(function () { out.push('y'); });"), "x y");
    CHECK_EQ(settled(in, "var out = []; var thenable = { then: function (res) { out.push('then'); res(5); } }; async function f() { var v = await thenable; out.push('v' + v); } f(); out.push('sync');"), "sync then v5");
    // await in the expression positions that matter: operands, a
    // computed key and its value, a condition, a template, an array.
    CHECK_EQ(settled(in, "var out = []; async function f() { var s = (await 1) + (await 2) * (await 3); var o = {}; o[await 'k'] = await 'v'; out.push(s, (await true) ? 'yes' : 'no', `${await 'a'}-${await 'b'}`, Object.keys(o)[0] + '=' + o.k, String(await [1, 2])); } f();"), "7 yes a-b k=v 1,2");
    // Closures made before an await see the bindings written after it.
    CHECK_EQ(settled(in, "var out = []; async function f() { var n = 1; var read = function () { return n; }; await null; n = 2; out.push(read()); } f();"), "2");
}

void test_throws_and_rejections()
{
    js::Interpreter& in = fresh();
    // A throw before the first await rejects the promise rather than
    // propagating; so does one after it.
    CHECK_EQ(settled(in, "var out = []; async function f() { throw new Error('boom'); } var p; try { p = f(); out.push('returned'); } catch (e) { out.push('threw'); } p.catch(function (e) { out.push(e.message); });"), "returned boom");
    CHECK_EQ(settled(in, "var out = []; async function f() { await null; throw 'late'; } f().catch(function (e) { out.push('caught ' + e); });"), "caught late");
    // A parameter default that throws rejects too (§15.8.4 step 3): the
    // capability exists before the arguments are bound.
    CHECK_EQ(settled(in, "var out = []; async function f(x = (function () { throw new Error('dflt'); })()) {} var p; try { p = f(); out.push('no sync throw'); } catch (e) { out.push('sync'); } p.catch(function (e) { out.push(e.message); });"), "no sync throw dflt");
    // A rejected operand throws at the await, where try/catch takes it;
    // finally runs on the way out either way, awaits inside it included.
    CHECK_EQ(settled(in, "var out = []; async function f() { try { await Promise.reject('r'); out.push('not here'); } catch (e) { out.push('c' + e); } finally { out.push('fin'); } return 'done'; } f().then(function (v) { out.push(v); });"), "cr fin done");
    CHECK_EQ(settled(in, "var out = []; async function f() { try { throw 't'; } finally { await null; out.push('in finally'); } } f().catch(function (e) { out.push('rejected ' + e); });"), "in finally rejected t");
    // An empty for await settles the call normally (its own tests follow).
    CHECK_EQ(settled(in, "var out = []; async function f() { for await (var x of []) {} return 'ok'; } f().then(function (v) { out.push(v); }, function (e) { out.push(e.name + ': ' + e.message); });"), "ok");
}

void test_own_name_and_scopes()
{
    js::Interpreter& in = fresh();
    // A named async function expression binds its own name, immutably:
    // a write is ignored in sloppy code and a TypeError in strict code.
    CHECK_EQ(settled(in, "var out = []; var f = async function g() { return g === f; }; f().then(function (v) { out.push(v); });"), "true");
    CHECK_EQ(settled(in, "var out = []; var f = async function g() { g = 1; return g === f; }; f().then(function (v) { out.push(v); });"), "true");
    CHECK_EQ(settled(in, "var out = []; var f = async function g() { 'use strict'; g = 1; }; f().catch(function (e) { out.push(e.name); });"), "TypeError");
    CHECK_EQ(settled(in, "var out = []; var probe; var f = async function g() { probe = function () { return g; }; }; var g = 'outside'; f(); out.push(g, probe() === f);"), "outside true");
    // arguments and this inside an async method.
    CHECK_EQ(settled(in, "var out = []; var o = { async m() { return arguments.length + ':' + (this === o); } }; o.m(1, 2).then(function (v) { out.push(v); });"), "2:true");
}

// for await (§14.7.5): the async iterator protocol, a sync iterable
// wrapped so that each value is awaited, the close awaited on every way
// out, and a throw winning over whatever the close does.
void test_for_await()
{
    js::Interpreter& in = fresh();
    CHECK_EQ(settled(in, "var out = []; async function f() { for await (const x of [1, Promise.resolve(2), 3]) out.push(x); out.push('end'); } f();"), "1 2 3 end");
    CHECK_EQ(settled(in, "var out = []; function make(n) { let i = 0; return { [Symbol.asyncIterator]() { return { next() { i += 1; return Promise.resolve({ value: i * 10, done: i > n }); }, return() { out.push('closed'); return Promise.resolve({ done: true }); } }; } }; } async function f() { for await (const v of make(3)) out.push(v); for await (const v of make(3)) { if (v === 20) break; out.push(v); } } f();"), "10 20 30 10 closed");
    CHECK_EQ(settled(in, "var out = []; function make() { return { [Symbol.asyncIterator]() { return { next() { return { value: 1, done: false }; }, return() { out.push('r'); return { done: true }; } }; } }; } async function f() { for await (const v of make()) return v; } async function g() { try { for await (const v of make()) throw new Error('boom'); } catch (e) { out.push(e.message); } } f().then(v => out.push('f' + v)); g();"), "r r boom f1");
    CHECK_EQ(settled(in, "var out = []; async function f() { try { for await (const v of { [Symbol.asyncIterator]() { return { next() { return Promise.reject(new Error('no')); } }; } }) out.push(v); } catch (e) { out.push(e.message); } try { for await (const v of { [Symbol.asyncIterator]() { return { next() { return 5; } }; } }) out.push(v); } catch (e) { out.push(e instanceof TypeError); } } f();"), "no true");
    CHECK_EQ(settled(in, "var out = []; async function f() { var sum = 0; for await (var { a } of [{ a: 1 }, { a: 2 }]) sum += a; let t; for await (t of ['x']) ; out.push(sum, t); const it = { [Symbol.iterator]() { return { next() { return { value: 1, done: false }; }, return() { out.push('sync-return'); return {}; } }; } }; for await (const v of it) break; } f();"), "3 x sync-return");
    CHECK_EQ(settled(in, "var out = []; async function f() { try { for await (const v of { [Symbol.iterator]() { return { next() { return 1; } }; } }) out.push(v); } catch (e) { out.push(e instanceof TypeError); } try { for await (const v of { [Symbol.iterator]() { return { next() { return { value: Promise.reject(new Error('rej')), done: false }; }, return() { out.push('closed'); return {}; } }; } }) out.push(v); } catch (e) { out.push(e.message); } } f();"), "true closed rej");
    // A close whose return() answers something that is not an object is a
    // TypeError on a normal exit; on a throw the throw still wins.
    CHECK_EQ(settled(in, "var out = []; function bad() { return { [Symbol.asyncIterator]() { return { next() { return { value: 1, done: false }; }, return() { return 7; } }; } }; } async function f() { try { for await (const v of bad()) break; } catch (e) { out.push(e instanceof TypeError); } try { for await (const v of bad()) throw new Error('mine'); } catch (e) { out.push(e.message); } } f();"), "true mine");
    CHECK_JS_THROWS(in, "function f() { for await (const x of []) {} }", "SyntaxError");
}

// Async generators (§27.6): requests queued and answered one at a time, a
// request that arrives while the body runs resuming it at once, yield and
// return awaiting their values, yield* over async and sync iterables, the
// prototypes, and the dynamic constructor.
void test_async_generators()
{
    js::Interpreter& in = fresh();
    CHECK_EQ(settled(in, "var out = []; async function* g() { yield 1; yield await Promise.resolve(2); return 3; } var it = g(); it.next().then(r => { out.push(r.value, r.done); return it.next(); }).then(r => { out.push(r.value, r.done); return it.next(); }).then(r => { out.push(r.value, r.done); return it.next(); }).then(r => out.push(String(r.value), r.done));"), "1 false 2 false 3 true undefined true");
    // yield awaits its value, so the body suspends before the caller's next
    // statement; the queued next() resumes it at once when the yield completes.
    CHECK_EQ(settled(in, "var out = []; async function* g() { out.push('a'); const x = yield 1; out.push('got ' + x); yield 2; } var it = g(); it.next('x').then(r => out.push(r.value)); it.next('y').then(r => out.push(r.value)); it.next('z').then(r => out.push(r.value + ':' + r.done)); out.push('sync');"), "a sync got y 1 2 undefined:true");
    // return before the body ran: the value awaited, no finally run.
    CHECK_EQ(settled(in, "var out = []; async function* g() { try { yield 1; } finally { out.push('fin'); } } var a = g(); a.return(Promise.resolve('early')).then(r => out.push(r.value, r.done));"), "early true");
    // return at a yield: through the finally, the value awaited.
    CHECK_EQ(settled(in, "var out = []; async function* g() { try { yield 1; } finally { out.push('fin'); } } var b = g(); b.next().then(() => b.return('later')).then(r => out.push(r.value, r.done));"), "fin later true");
    // throw before the body ran rejects; throw at a yield is caught inside.
    CHECK_EQ(settled(in, "var out = []; async function* g() { try { yield 1; } catch (e) { out.push('caught ' + e); yield 2; } } var c = g(); c.throw(new Error('boom')).catch(e => out.push(e.message)); var it = g(); it.next().then(() => it.throw('t')).then(r => out.push(r.value)).then(() => it.next()).then(r => out.push(r.done));"), "boom caught t 2 true");
    CHECK_EQ(settled(in, "var out = []; async function* inner() { yield 'i1'; yield 'i2'; return 'ret'; } async function* g() { const r = yield* inner(); out.push('r=' + r); yield* [Promise.resolve('s1'), 's2']; } (async () => { for await (const v of g()) out.push(v); })();"), "i1 i2 r=ret s1 s2");
    CHECK_EQ(settled(in, "var out = []; async function* g() { yield 1; yield 2; } (async () => { for await (const v of g()) out.push(v); var it2 = g(); out.push(it2[Symbol.asyncIterator]() === it2); })();"), "1 2 true");
    CHECK_EQ(settled(in, "var out = []; async function* g() { return Promise.resolve('awaited'); } g().next().then(r => out.push(r.value, r.done));"), "awaited true");
    // Once done: next is done, return awaits its value, throw rejects.
    CHECK_EQ(settled(in, "var out = []; async function* g() { yield 1; } var it = g(); it.next().then(() => it.next()).then(() => Promise.all([it.next(), it.return('x'), it.throw(new Error('e')).catch(e => e.message)])).then(rs => out.push(rs[0].done, rs[1].value, rs[2]));"), "true x e");
    // yield* forwards a throw to the inner iterator; a break out of for
    // await returns through the delegation.
    CHECK_EQ(settled(in, "var out = []; async function* inner() { try { yield 1; yield 2; } finally { out.push('inner-fin'); } } async function* g() { yield* inner(); } (async () => { for await (const v of g()) { out.push(v); break; } })();"), "1 inner-fin");
    CHECK_JS_TRUE(in, "typeof (async function* () {}).prototype === 'object' && Object.getPrototypeOf(async function* () {})[Symbol.toStringTag] === 'AsyncGeneratorFunction' && Object.getPrototypeOf(async function* () {}.prototype)[Symbol.toStringTag] === 'AsyncGenerator' && Object.getPrototypeOf(Object.getPrototypeOf(async function* () {}.prototype)) === Object.getPrototypeOf(Object.getPrototypeOf(Object.getPrototypeOf((async function* () {})())))");
    CHECK_JS_TRUE(in, "(async function* () {})().next() instanceof Promise && Object.getPrototypeOf(async function* () {}).constructor.name === 'AsyncGeneratorFunction' && typeof Object.getPrototypeOf(Object.getPrototypeOf(async function* () {}.prototype))[Symbol.asyncIterator] === 'function'");
    CHECK_EQ(settled(in, "var out = []; var AGF = Object.getPrototypeOf(async function* () {}).constructor; var g2 = new AGF('a', 'yield a * 2;'); g2(21).next().then(r => out.push(r.value));"), "42");
    CHECK_JS_THROWS(in, "new (async function* () {})()", "TypeError");
    CHECK_EQ(settled(in, "var out = []; var proto = Object.getPrototypeOf(async function* () {}.prototype); proto.next.call({}).catch(e => out.push(e instanceof TypeError));"), "true");
}

}

int main()
{
    test_calls_and_results();
    test_await_ordering();
    test_throws_and_rejections();
    test_own_name_and_scopes();
    test_for_await();
    test_async_generators();
    return sashfold::test::report("js_async");
}
