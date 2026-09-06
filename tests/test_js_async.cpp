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
    // for await is not written yet: the first call rejects with the
    // SyntaxError that names it.
    CHECK_EQ(settled(in, "var out = []; async function f() { for await (var x of []) {} } f().catch(function (e) { out.push(e.name + ': ' + e.message); });"), "SyntaxError: for await is not supported yet");
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

}

int main()
{
    test_calls_and_results();
    test_await_ordering();
    test_throws_and_rejections();
    test_own_name_and_scopes();
    return sashfold::test::report("js_async");
}
