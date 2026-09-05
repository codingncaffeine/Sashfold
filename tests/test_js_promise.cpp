#include "JsTest.h"

#include "js/Interpreter.h"

#include <string>
#include <vector>

using namespace sashfold;

namespace {

// Every realm here runs under heap stress: a collection at every
// allocation, so an unrooted value fails the first time — and a job
// queue that holds a value across a collection is exactly what these
// tests exercise.
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

void test_construction_and_then()
{
    js::Interpreter& in = fresh();
    // Handlers run as jobs after the script, in order; a fulfilled promise
    // stays fulfilled; the second resolve is ignored.
    CHECK_EQ(settled(in, "var out = []; var p = new Promise(function (res, rej) { out.push('exec'); res(1); res(2); rej(3); }); p.then(function (v) { out.push('then ' + v); }); out.push('sync');"), "exec sync then 1");
    CHECK_EQ(settled(in, "var out = []; new Promise(function (res, rej) { rej('no'); }).then(function () { out.push('wrong'); }, function (e) { out.push('rejected ' + e); });"), "rejected no");
    // A throw in the executor rejects; a throw in a handler rejects the derived promise.
    CHECK_EQ(settled(in, "var out = []; new Promise(function () { throw new Error('boom'); }).catch(function (e) { out.push(e.message); });"), "boom");
    CHECK_EQ(settled(in, "var out = []; Promise.resolve(1).then(function () { throw 'x'; }).then(function () { out.push('wrong'); }).catch(function (e) { out.push('caught ' + e); });"), "caught x");
    // The chain: each then's return value is the next one's input, and
    // a returned promise is unwrapped first (one extra job for the
    // thenable, as every engine orders it).
    CHECK_EQ(settled(in, "var out = []; Promise.resolve(1).then(function (v) { return v + 1; }).then(function (v) { return Promise.resolve(v * 10); }).then(function (v) { out.push(v); });"), "20");
    CHECK_EQ(settled(in, "var out = []; Promise.resolve().then(function () { out.push('a1'); }).then(function () { out.push('a2'); }); Promise.resolve().then(function () { out.push('b1'); }).then(function () { out.push('b2'); });"), "a1 b1 a2 b2");
    // Non-callable handlers pass the value and the reason through; the
    // shorter chain (two links against three) settles first.
    CHECK_EQ(settled(in, "var out = []; Promise.resolve(7).then(5).then(null, 6).then(function (v) { out.push(v); }); Promise.reject(8).then(null).catch(function (e) { out.push(e); });"), "8 7");
    // A thenable is adopted through a job; a promise resolved with itself is a TypeError.
    CHECK_EQ(settled(in, "var out = []; Promise.resolve({ then: function (res) { out.push('then called'); res('adopted'); } }).then(function (v) { out.push(v); });"), "then called adopted");
    CHECK_EQ(settled(in, "var out = []; var res; var p = new Promise(function (r) { res = r; }); res(p); p.catch(function (e) { out.push(e.constructor.name + ':' + (e.message.indexOf('cycle') >= 0)); });"), "TypeError:true");
    // A thenable whose getter throws rejects; one whose then throws after
    // resolving is ignored.
    CHECK_EQ(settled(in, "var out = []; Promise.resolve(Object.defineProperty({}, 'then', { get: function () { throw 'getter'; } })).catch(function (e) { out.push(e); });"), "getter");
    CHECK_EQ(settled(in, "var out = []; Promise.resolve({ then: function (res) { res('ok'); throw 'late'; } }).then(function (v) { out.push(v); }, function (e) { out.push('wrong ' + e); });"), "ok");
    // The constructor's checks.
    CHECK_JS_THROWS(in, "Promise(function () {})", "TypeError");
    CHECK_JS_THROWS(in, "new Promise(1)", "TypeError");
    CHECK_JS_THROWS(in, "Promise.prototype.then.call({}, function () {})", "TypeError");
    CHECK_JS_TRUE(in, "Promise.length === 1 && Promise.name === 'Promise' && Promise.prototype.then.length === 2 && Promise.prototype.catch.length === 1 && Promise.prototype.finally.length === 1 && Promise[Symbol.species] === Promise && Object.prototype.toString.call(Promise.resolve()) === '[object Promise]'");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(Promise, 'prototype'); return !d.writable && !d.enumerable && !d.configurable; })()");
    // Promise.resolve returns a promise of its own constructor unchanged.
    CHECK_JS_TRUE(in, "(function () { var p = Promise.resolve(1); return Promise.resolve(p) === p && Promise.resolve(Promise.reject(2)) !== p; })()");
    // finally: the callback runs either way without the value, its result
    // is waited for, and the outcome passes through — unless it throws.
    CHECK_EQ(settled(in, "var out = []; Promise.resolve('v').finally(function () { out.push('fin ' + arguments.length); return 'ignored'; }).then(function (v) { out.push(v); }); Promise.reject('r').finally(function () { out.push('fin2'); }).catch(function (e) { out.push(e); });"), "fin 0 fin2 v r");
    CHECK_EQ(settled(in, "var out = []; Promise.resolve('v').finally(function () { throw 'oops'; }).catch(function (e) { out.push(e); }); Promise.resolve('w').finally(function () { return Promise.reject('rej'); }).catch(function (e) { out.push(e); });"), "oops rej");
    CHECK_EQ(settled(in, "var out = []; var order = []; Promise.resolve('v').finally(function () { return new Promise(function (r) { order.push('wait'); r(); }); }).then(function (v) { out.push(order.join('') + v); });"), "waitv");
    CHECK_EQ(settled(in, "var out = []; Promise.resolve(3).finally(5).then(function (v) { out.push(v); });"), "3");
    // Species: a subclass's then derives from the subclass.
    CHECK_EQ(settled(in, "var out = []; class P extends Promise {} var q = new P(function (r) { r(1); }).then(function (v) { return v; }); out.push(q instanceof P, q.constructor === P, Promise.resolve(1) instanceof P);"), "true true false");
    CHECK_EQ(settled(in, "var out = []; var p = Promise.resolve(1); p.constructor = { [Symbol.species]: function (executor) { out.push('species'); return new Promise(executor); } }; p.then(function (v) { out.push(v); });"), "species 1");
}

void test_statics()
{
    js::Interpreter& in = fresh();
    // all: values in order, whatever the settlement order; one rejection ends it.
    CHECK_EQ(settled(in, "var out = []; var later; Promise.all([new Promise(function (r) { later = r; }), 2, Promise.resolve(3)]).then(function (v) { out.push(v.join()); }); later(1);"), "1,2,3");
    CHECK_EQ(settled(in, "var out = []; Promise.all([Promise.resolve(1), Promise.reject('bad'), Promise.reject('worse')]).then(function () { out.push('wrong'); }, function (e) { out.push(e); });"), "bad");
    CHECK_EQ(settled(in, "var out = []; Promise.all([]).then(function (v) { out.push('empty ' + v.length + Array.isArray(v)); });"), "empty 0true");
    CHECK_EQ(settled(in, "var out = []; Promise.all(5).catch(function (e) { out.push(e.constructor.name); });"), "TypeError");
    CHECK_EQ(settled(in, "var out = []; Promise.all(new Set([Promise.resolve('a'), 'b'])).then(function (v) { out.push(v.join('')); });"), "ab");
    // allSettled: every outcome described.
    CHECK_EQ(settled(in, "var out = []; Promise.allSettled([Promise.resolve(1), Promise.reject('e'), 3]).then(function (r) { out.push(r.map(function (x) { return x.status + ':' + (x.status === 'fulfilled' ? x.value : x.reason); }).join()); });"), "fulfilled:1,rejected:e,fulfilled:3");
    // any: the first fulfilment, or an AggregateError of every reason.
    CHECK_EQ(settled(in, "var out = []; Promise.any([Promise.reject(1), Promise.resolve(2), Promise.resolve(3)]).then(function (v) { out.push(v); });"), "2");
    CHECK_EQ(settled(in, "var out = []; Promise.any([Promise.reject(1), Promise.reject(2)]).catch(function (e) { out.push(e instanceof AggregateError, e.errors.join(), e.message); });"), "true 1,2 All promises were rejected");
    CHECK_EQ(settled(in, "var out = []; Promise.any([]).catch(function (e) { out.push(e.constructor.name + e.errors.length); });"), "AggregateError0");
    // race: the first to settle, either way; an empty race stays pending.
    CHECK_EQ(settled(in, "var out = []; Promise.race([new Promise(function () {}), Promise.resolve('fast'), Promise.reject('slow')]).then(function (v) { out.push(v); }, function (e) { out.push('wrong ' + e); });"), "fast");
    CHECK_EQ(settled(in, "var out = []; Promise.race([Promise.reject('first'), Promise.resolve('second')]).catch(function (e) { out.push(e); });"), "first");
    CHECK_EQ(settled(in, "var out = ['pending']; Promise.race([]).then(function () { out.push('wrong'); });"), "pending");
    // The combinators read `resolve` once from the constructor and close
    // the iterator when something other than the iterator throws.
    CHECK_EQ(settled(in, "var out = []; var log = []; var C = function (executor) { return new Promise(executor); }; C.resolve = function (v) { log.push('resolve'); return Promise.resolve(v); }; Promise.all.call(C, [1, 2]).then(function (v) { out.push(log.join() + ':' + v.join()); });"), "resolve,resolve:1,2");
    CHECK_EQ(settled(in, "var out = []; var closed = 0; var it = {}; it[Symbol.iterator] = function () { return { next: function () { return { value: 1, done: false }; }, return: function () { closed++; return {}; } }; }; var C = function (executor) { return new Promise(executor); }; C.resolve = function () { throw 'stop'; }; Promise.race.call(C, it).catch(function (e) { out.push(e + closed); });"), "stop1");
    // A constructor without `resolve` rejects the result (IfAbruptRejectPromise);
    // a non-constructor `this` throws synchronously.
    CHECK_EQ(settled(in, "var out = []; Promise.all.call(function (executor) { return new Promise(executor); }, [1]).then(function () { out.push('wrong'); }, function (e) { out.push('rejected ' + e.constructor.name); }); try { Promise.all.call(1, []); } catch (e) { out.push(e.constructor.name); }"), "TypeError rejected TypeError");
    // resolve / reject / withResolvers / try.
    CHECK_EQ(settled(in, "var out = []; var w = Promise.withResolvers(); w.promise.then(function (v) { out.push(v); }); w.resolve('w'); var x = Promise.withResolvers(); x.promise.catch(function (e) { out.push(e); }); x.reject('x');"), "w x");
    CHECK_EQ(settled(in, "var out = []; Promise.try(function (a, b) { return a + b; }, 1, 2).then(function (v) { out.push(v); }); Promise.try(function () { throw 'thrown'; }).catch(function (e) { out.push(e); }); Promise.try(function () { return Promise.resolve('inner'); }).then(function (v) { out.push(v); });"), "3 thrown inner");
    CHECK_JS_THROWS(in, "Promise.resolve.call(1, 2)", "TypeError");
    CHECK_JS_THROWS(in, "Promise.reject.call({}, 2)", "TypeError");
    CHECK_JS_TRUE(in, "Promise.all.length === 1 && Promise.race.length === 1 && Promise.any.length === 1 && Promise.allSettled.length === 1 && Promise.resolve.length === 1 && Promise.reject.length === 1 && Promise.withResolvers.length === 0 && Promise.try.length === 1");
    // AggregateError on its own.
    CHECK_JS_TRUE(in, "(function () { var e = new AggregateError([1, 2], 'm', { cause: 'c' }); return e instanceof Error && e.errors.join() === '1,2' && e.message === 'm' && e.cause === 'c' && Object.getPrototypeOf(AggregateError) === Error && AggregateError.length === 2 && AggregateError.prototype.name === 'AggregateError' && !e.hasOwnProperty('name') && e.toString() === 'AggregateError: m'; })()");
    CHECK_JS_TRUE(in, "(function () { var e = AggregateError(new Set([3])); return e instanceof AggregateError && e.errors[0] === 3 && !Object.getOwnPropertyDescriptor(e, 'errors').enumerable && e.message === ''; })()");
    CHECK_JS_THROWS(in, "new AggregateError(1)", "TypeError");
}

void test_jobs_and_rejections()
{
    js::Interpreter& in = fresh();
    std::vector<std::string> console;
    in.on_console = [&console](std::string_view level, std::string_view message) {
        console.push_back(std::string(level) + ": " + std::string(message));
    };
    // Microtasks and reactions share one queue, in order.
    {
        test::JsRun const run = test::run_js(in, "var out = []; Promise.resolve().then(function () { out.push('reaction'); });");
        CHECK(run.ok);
        CHECK(in.has_pending_jobs());
        js::Interpreter::Roots const roots(in);
        js::Value const callback = in.root(js::Value::object(in.new_native("cb", 0, [](js::Interpreter& interp, js::Value const&, std::span<js::Value const>) -> std::optional<js::Value> {
            return interp.run_script("out.push('microtask')", "cb").value;
        })));
        in.enqueue_microtask(callback, {});
        in.run_jobs({});
        CHECK(!in.has_pending_jobs());
        CHECK_JS_STRING(in, "out.join(' ')", "reaction microtask");
    }
    // A job queued by a job runs in the same drain, and a handler's own
    // reaction is queued before the handler's return settles the derived
    // promise (the 1 3 2 every engine prints); one at a time when asked.
    CHECK_EQ(settled(in, "var out = []; Promise.resolve().then(function () { out.push(1); Promise.resolve().then(function () { out.push(3); }); }).then(function () { out.push(2); });"), "1 3 2");
    {
        test::JsRun const run = test::run_js(in, "var out = []; Promise.resolve().then(function () { out.push('a'); }); Promise.resolve().then(function () { out.push('b'); });");
        CHECK(run.ok);
        js::Value thrown = js::Value::empty();
        CHECK(in.run_next_job(&thrown));
        CHECK_JS_STRING(in, "out.join('')", "a");
        CHECK(in.run_next_job(&thrown));
        CHECK(!in.run_next_job(&thrown));
        CHECK_JS_STRING(in, "out.join('')", "ab");
        CHECK(thrown.is_empty());
    }
    // A callback job's throw is reported to the drain's callback.
    {
        test::JsRun const run = test::run_js(in, "queueMicrotaskLike = function () {}");
        CHECK(run.ok);
        js::Interpreter::Roots const roots(in);
        js::Value const thrower = in.root(js::Value::object(in.new_native("thrower", 0, [](js::Interpreter& interp, js::Value const&, std::span<js::Value const>) -> std::optional<js::Value> {
            return interp.throw_type_error("from a microtask");
        })));
        in.enqueue_microtask(thrower, {});
        std::vector<std::string> reported;
        in.run_jobs([&](js::Value const& thrown) { reported.push_back(in.describe(thrown)); });
        CHECK_EQ(reported.size(), std::size_t(1));
        CHECK(!reported.empty() && reported[0] == "TypeError: from a microtask");
    }
    // Unhandled rejections are reported once, at the end of the drain, and
    // only when nothing handled them by then.
    console.clear();
    CHECK_EQ(settled(in, "var out = ['x']; Promise.reject(new Error('nobody')); Promise.reject('handled').catch(function () {}); Promise.resolve().then(function () { throw 'late'; });"), "x");
    CHECK_EQ(console.size(), std::size_t(2));
    CHECK(console.size() == 2 && console[0] == "error: Uncaught (in promise) Error: nobody" && console[1] == "error: Uncaught (in promise) late");
    console.clear();
    // A handler attached in a later job, before the drain ends, is in time.
    CHECK_EQ(settled(in, "var out = ['y']; var p = Promise.reject('soon'); Promise.resolve().then(function () { p.catch(function (e) { out.push(e); }); });"), "y soon");
    CHECK_EQ(console.size(), std::size_t(0));
    // Values queued on the jobs survive collections: many pending
    // reactions, then a drain under stress.
    CHECK_EQ(settled(in, "var out = []; var ps = []; for (var i = 0; i < 50; i++) ps.push(new Promise(function (r) { r({ i: i }); }).then(function (o) { return o.i * 2; })); Promise.all(ps).then(function (v) { out.push(v.length, v[49]); });"), "50 98");
    // clear_jobs drops what is pending.
    {
        test::JsRun const run = test::run_js(in, "var out = []; Promise.resolve().then(function () { out.push('dropped'); });");
        CHECK(run.ok && in.has_pending_jobs());
        in.clear_jobs();
        CHECK(!in.has_pending_jobs());
        in.run_jobs({});
        CHECK_JS_STRING(in, "out.join('')", "");
    }
}

} // namespace

int main()
{
    test_construction_and_then();
    test_statics();
    test_jobs_and_rejections();
    return sashfold::test::report("js_promise");
}
