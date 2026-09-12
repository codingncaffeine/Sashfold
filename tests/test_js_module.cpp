// Modules (§16.2): records over in-memory sources, linking, evaluation
// order, live bindings across modules, the namespace exotic object, the
// default export's shapes, cycles, and the errors of each phase. Every
// realm runs under heap stress.

#include "JsTest.h"

#include "js/Interpreter.h"
#include "js/Module.h"
#include "js/Object.h"
#include "js/Strings.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace sashfold;

namespace {

// A realm whose module loader reads from a map of key → source; a
// specifier is a key as written (no resolution beyond that), and every
// resolution is logged so that a test can see which referrer it was
// asked about.
struct ModuleRealm {
    js::Interpreter interpreter;
    std::map<std::string, std::string> files;
    std::string console; // what console.* printed, one line each
    std::string resolutions; // "<referrer>|<specifier>" per resolver call, one a line

    ModuleRealm()
    {
        interpreter.heap().set_stress(true);
        interpreter.on_console = [this](std::string_view, std::string_view message) {
            console += std::string(message) + "\n";
        };
        interpreter.set_module_hooks(
            [this](std::string_view referrer, std::string_view specifier, std::string& error) -> std::optional<std::string> {
                resolutions += std::string(referrer) + "|" + std::string(specifier) + "\n";
                if (!files.contains(std::string(specifier))) {
                    error = "no module named '" + std::string(specifier) + "'";
                    return std::nullopt;
                }
                return std::string(specifier);
            },
            [this](std::string_view key, std::string& error) -> std::optional<std::u16string> {
                auto const found = files.find(std::string(key));
                if (found == files.end()) {
                    error = "no source for '" + std::string(key) + "'";
                    return std::nullopt;
                }
                return js::utf16_from_utf8(found->second);
            });
    }

    // Parses, loads, links and evaluates `key`, drains the job queue, and
    // answers "" for a fulfilled evaluation, else "<phase>: <error>".
    std::string run(std::string const& key)
    {
        std::string const started = start(key);
        if (!evaluated)
            return started;
        return settle();
    }

    // The first half of run(): everything up to Evaluate, and the state
    // of the evaluation promise before a single job has run — "pending"
    // for a graph with a top-level await in it, "" for one without.
    std::string start(std::string const& key)
    {
        evaluated = false;
        auto const found = files.find(key);
        if (found == files.end())
            return "no such file";
        js::ModuleRecord* record = interpreter.parse_module(js::utf16_from_utf8(found->second), key);
        if (record == nullptr)
            return "parse: " + interpreter.describe(interpreter.take_exception());
        if (!interpreter.load_module(*record))
            return "load: " + interpreter.describe(interpreter.take_exception());
        if (!interpreter.link_module(*record))
            return "link: " + interpreter.describe(interpreter.take_exception());
        std::optional<js::Value> const promise = interpreter.evaluate_module(*record);
        if (!promise)
            return "evaluate: " + interpreter.describe(interpreter.take_exception());
        // Rooted for the realm's life: no Roots scope is open here, so the
        // push outlives every scope the engine opens later.
        interpreter.root(*promise);
        last_promise = *promise;
        evaluated = true;
        return state();
    }

    // The second half: the queue drained, then the state of the last
    // promise start() made.
    std::string settle()
    {
        interpreter.run_jobs([this](js::Value const& thrown) { console += "job threw " + interpreter.describe(thrown) + "\n"; });
        return state();
    }

    std::string state()
    {
        auto const* promise = static_cast<js::PromiseObject const*>(last_promise.as_object());
        if (promise->state() == js::PromiseObject::State::Rejected)
            return "rejected: " + interpreter.describe(promise->result());
        if (promise->state() == js::PromiseObject::State::Pending)
            return "pending";
        return "";
    }

    js::Value last_promise; // the evaluation promise start() made, rooted there
    bool evaluated = false; // start() got as far as Evaluate

    // Runs `source` as a classic script under `name` — the referrer an
    // import() in it resolves against — and drains the job queue.
    std::string run_script(std::string const& source, std::string const& name)
    {
        js::Outcome const outcome = interpreter.run_script(source, name);
        if (!outcome.ok)
            return "threw: " + interpreter.describe(outcome.value);
        interpreter.run_jobs([this](js::Value const& thrown) { console += "job threw " + interpreter.describe(thrown) + "\n"; });
        return "";
    }
};

void test_exports_and_imports()
{
    ModuleRealm realm;
    realm.files["a"] = "export var v = 1; export let l = 2; export const c = 3; export function f() { return 4 }\n"
                       "export class K { m() { return 5 } } export default 6; var hidden = 7; export { hidden as shown };";
    realm.files["main"] = "import d, { v, l, c, f, K, shown } from 'a'; import * as ns from 'a';\n"
                          "globalThis.out = [v, l, c, f(), new K().m(), d, shown, ns.default, ns.v, typeof hidden].join();";
    CHECK_EQ(realm.run("main"), "");
    CHECK_JS_STRING(realm.interpreter, "out", "1,2,3,4,5,6,7,6,1,undefined");
    // Module scope is not the global scope, and the module ran once.
    CHECK_JS_STRING(realm.interpreter, "typeof v + typeof f + typeof hidden", "undefinedundefinedundefined");
    CHECK_EQ(realm.run("main"), "");
}

void test_live_bindings()
{
    ModuleRealm realm;
    realm.files["counter"] = "export let count = 0; export function increment() { count += 1 } export var boxed = { n: 0 };";
    realm.files["main"] = "import { count, increment, boxed } from 'counter'; import * as ns from 'counter';\n"
                          "var seen = [count, ns.count]; increment(); increment(); seen.push(count, ns.count);\n"
                          "boxed.n = 9; seen.push(ns.boxed.n);\n"
                          "var failures = 0; try { count = 5 } catch (e) { failures += e instanceof TypeError }\n"
                          "try { ns.count = 5 } catch (e) { failures += e instanceof TypeError }\n"
                          "try { count++ } catch (e) { failures += e instanceof TypeError }\n"
                          "globalThis.out = seen.join() + '|' + failures;";
    CHECK_EQ(realm.run("main"), "");
    CHECK_JS_STRING(realm.interpreter, "out", "0,0,2,2,9|3");
}

void test_namespace_object()
{
    ModuleRealm realm;
    realm.files["m"] = "export const b = 2, a = 1; export default 0; export function z() {} export * from 'n'; export * as inner from 'n';";
    realm.files["n"] = "export const y = 'y'; export default 'not re-exported';";
    realm.files["main"] = "import * as ns from 'm';\n"
                          "var out = [];\n"
                          "out.push(Object.keys(ns).join());\n"
                          "out.push(Object.getPrototypeOf(ns) === null, Object.isExtensible(ns), Object.isFrozen(ns), Object.isSealed(ns));\n"
                          "out.push(ns[Symbol.toStringTag], Object.prototype.toString.call(ns));\n"
                          "out.push('a' in ns, 'missing' in ns, ns.missing, Object.hasOwn(ns, 'default'));\n"
                          "var d = Object.getOwnPropertyDescriptor(ns, 'a'); out.push(d.value, d.writable, d.enumerable, d.configurable);\n"
                          "out.push(JSON.stringify(Object.getOwnPropertyDescriptor(ns, Symbol.toStringTag)));\n"
                          "out.push(ns.inner.y, ns.inner === ns.inner, typeof ns.z, ns.y);\n"
                          "var f = 0; try { ns.a = 1 } catch (e) { f += e instanceof TypeError } try { delete ns.a } catch (e) { f += e instanceof TypeError }\n"
                          "out.push(f, delete ns.missing, Reflect.deleteProperty(ns, 'a'), Reflect.set(ns, 'a', 3));\n"
                          "out.push(Reflect.defineProperty(ns, 'a', { value: 1 }), Reflect.defineProperty(ns, 'a', { value: 2 }), Reflect.defineProperty(ns, 'a', { configurable: true }));\n"
                          "out.push(Object.setPrototypeOf(ns, null) === ns, Reflect.setPrototypeOf(ns, {}), Reflect.preventExtensions(ns));\n"
                          "var conversion; try { String(ns) } catch (e) { conversion = e.name } out.push(conversion);\n"
                          "out.push(Object.entries(ns).map(function (e) { return e[0] + '=' + (typeof e[1] === 'object' ? 'ns' : String(e[1])) }).join(';'));\n"
                          "var spread = { ...ns }; out.push(Object.keys(spread).join());\n"
                          "var keys = []; for (var k in ns) keys.push(k); out.push(keys.join());\n"
                          "globalThis.out = out.join('|');";
    CHECK_EQ(realm.run("main"), "");
    // Every pushed value in order, `|` between them (an undefined joins as
    // nothing): the keys; null prototype, extensible, frozen, sealed; the
    // tag and toString; in, in, a missing read, hasOwn; the descriptor's
    // four fields; the tag's descriptor; the inner namespace's reads; the
    // two TypeErrors and the two Reflect refusals; the three defines; the
    // prototype and extensibility answers; String(ns); entries; the spread
    // and for-in keys.
    CHECK_JS_STRING(realm.interpreter, "out",
        "a,b,default,inner,y,z|true|false|false|true|Module|[object Module]|true|false||true|1|true|true|false"
        "|{\"value\":\"Module\",\"writable\":false,\"enumerable\":false,\"configurable\":false}|y|true|function|y"
        "|2|true|false|false|true|false|false|true|false|true|TypeError|a=1;b=2;default=0;inner=ns;y=y;z=function z() {}"
        "|a,b,default,inner,y,z|a,b,default,inner,y,z");
}

void test_indirect_and_star_exports()
{
    ModuleRealm realm;
    realm.files["leaf"] = "export const x = 'leaf-x', y = 'leaf-y'; export default 'leaf-default';";
    realm.files["middle"] = "export { x as renamed, default as dflt } from 'leaf'; export * from 'leaf'; export const own = 'middle';";
    realm.files["main"] = "import { renamed, dflt, y, own } from 'middle'; import * as ns from 'middle';\n"
                          "globalThis.out = [renamed, dflt, y, own, Object.keys(ns).join(), 'default' in ns, 'x' in ns].join('|');";
    CHECK_EQ(realm.run("main"), "");
    CHECK_JS_STRING(realm.interpreter, "out", "leaf-x|leaf-default|leaf-y|middle|dflt,own,renamed,x,y|false|true");
    // An imported name re-exported is an indirect export of the origin,
    // live through both hops.
    ModuleRealm hops;
    hops.files["origin"] = "export let n = 1; export function bump() { n++ }";
    hops.files["relay"] = "import { n, bump } from 'origin'; export { n, bump };";
    hops.files["main"] = "import { n, bump } from 'relay'; var before = n; bump(); globalThis.out = before + ',' + n;";
    CHECK_EQ(hops.run("main"), "");
    CHECK_JS_STRING(hops.interpreter, "out", "1,2");
}

void test_link_errors()
{
    ModuleRealm realm;
    realm.files["leaf"] = "export const x = 1;";
    realm.files["missing"] = "import { nope } from 'leaf';";
    CHECK_EQ(realm.run("missing"), "link: SyntaxError: The requested module does not provide an export named 'nope'");
    realm.files["indirect-missing"] = "export { nope } from 'leaf';";
    CHECK_EQ(realm.run("indirect-missing"), "link: SyntaxError: The requested module does not provide an export named 'nope'");
    realm.files["p"] = "export const same = 'p';";
    realm.files["q"] = "export const same = 'q';";
    realm.files["both"] = "export * from 'p'; export * from 'q';";
    realm.files["ambiguous"] = "import { same } from 'both';";
    CHECK_EQ(realm.run("ambiguous"), "link: SyntaxError: The requested module contains conflicting star exports for name 'same'");
    // An ambiguous name is simply absent from the namespace.
    realm.files["namespace-of-both"] = "import * as ns from 'both'; globalThis.out = Object.keys(ns).join() + '|' + ('same' in ns);";
    CHECK_EQ(realm.run("namespace-of-both"), "");
    CHECK_JS_STRING(realm.interpreter, "out", "|false");
    // The same binding through two star paths is one export, not a clash.
    realm.files["r"] = "export * from 'p';";
    realm.files["diamond"] = "export * from 'p'; export * from 'r';";
    realm.files["through-diamond"] = "import { same } from 'diamond'; globalThis.out = same;";
    CHECK_EQ(realm.run("through-diamond"), "");
    CHECK_JS_STRING(realm.interpreter, "out", "p");
    // A specifier the host cannot resolve, and a dependency that does not parse.
    realm.files["unresolvable"] = "import 'nowhere';";
    CHECK_EQ(realm.run("unresolvable"), "load: TypeError: no module named 'nowhere'");
    realm.files["broken"] = "export var = 1;";
    realm.files["imports-broken"] = "import 'broken';";
    CHECK(realm.run("imports-broken").starts_with("load: SyntaxError: Unexpected token '='"));
    CHECK(realm.run("broken").starts_with("parse: SyntaxError: Unexpected token '='"));
}

void test_evaluation_order_and_cycles()
{
    ModuleRealm realm;
    realm.files["log"] = "export const log = [];";
    realm.files["a"] = "import { log } from 'log'; import 'b'; log.push('a');";
    realm.files["b"] = "import { log } from 'log'; import 'c'; log.push('b');";
    realm.files["c"] = "import { log } from 'log'; log.push('c');";
    realm.files["main"] = "import { log } from 'log'; import 'a'; import 'b'; import 'c'; log.push('main'); globalThis.out = log.join();";
    CHECK_EQ(realm.run("main"), "");
    CHECK_JS_STRING(realm.interpreter, "out", "c,b,a,main");
    // A cycle: the module entered first runs last, its dependency first. A
    // function declared in the later one is callable from the earlier
    // one before it has run; a let of the later one is in its dead zone.
    ModuleRealm cycle;
    cycle.files["even"] = "import { isOdd, oddReady } from 'odd'; export function isEven(n) { return n === 0 || isOdd(n - 1) }\n"
                          "var tdz; try { tdz = oddReady } catch (e) { tdz = e.constructor.name + ':' + e.message }\n"
                          "export const evenSaw = tdz + ',' + isOdd(3);";
    cycle.files["odd"] = "import { isEven, evenSaw } from 'even'; export function isOdd(n) { return n !== 0 && isEven(n - 1) }\n"
                         "export let oddReady = true; export const oddResult = isOdd(7) + ',' + isEven(7) + ',' + evenSaw;";
    cycle.files["main"] = "import { oddResult, oddReady } from 'odd'; globalThis.out = oddResult + ',' + oddReady;";
    CHECK_EQ(cycle.run("main"), "");
    CHECK_JS_STRING(cycle.interpreter, "out", "true,false,ReferenceError:Cannot access 'oddReady' before initialization,true,true");
    // typeof an import in its dead zone throws too (§13.5.3 through GetValue).
    ModuleRealm tdz;
    tdz.files["first"] = "import { later } from 'second'; export const early = 1;";
    tdz.files["second"] = "import { early } from 'first'; var r; try { r = typeof early } catch (e) { r = e.name } export const later = r;";
    tdz.files["main"] = "import { later } from 'first'; globalThis.out = later;";
    tdz.files["first"] = "import { later } from 'second'; export const early = 1; export { later };";
    CHECK_EQ(tdz.run("main"), "");
    CHECK_JS_STRING(tdz.interpreter, "out", "ReferenceError");
}

void test_evaluation_errors()
{
    ModuleRealm realm;
    realm.files["thrower"] = "globalThis.ran = (globalThis.ran || 0) + 1; throw new RangeError('boom');";
    realm.files["importer"] = "import 'thrower'; globalThis.reached = true;";
    CHECK_EQ(realm.run("importer"), "rejected: RangeError: boom");
    CHECK_JS_TRUE(realm.interpreter, "globalThis.reached === undefined && ran === 1");
    // Every later request sees the same error, and the body never runs again.
    CHECK_EQ(realm.run("importer"), "rejected: RangeError: boom");
    CHECK_EQ(realm.run("thrower"), "rejected: RangeError: boom");
    CHECK_JS_NUMBER(realm.interpreter, "ran", 1);
    realm.files["other"] = "import 'thrower';";
    CHECK_EQ(realm.run("other"), "rejected: RangeError: boom");
    CHECK_JS_NUMBER(realm.interpreter, "ran", 1);
    // The evaluation promise settles through the job queue like any other.
    realm.files["fine"] = "export const ok = true;";
    CHECK_EQ(realm.run("fine"), "");
}

void test_top_level_await()
{
    // The body runs as an async function: nothing after the first await
    // has run when Evaluate returns, the promise is pending until the
    // jobs have run, and the exports are what the awaits produced. An
    // importer in the same graph runs after the whole body, and its own
    // promise settles after the dependency's.
    ModuleRealm realm;
    realm.files["tla"] = "globalThis.log = ['start']; export const a = await Promise.resolve(1); log.push('a=' + a);\n"
                         "export let b = await 2; log.push('b=' + b); export default await new Promise(function (r) { r('d') });\n"
                         "export function f() { return a + b } log.push('end');";
    realm.files["main"] = "import d, { a, b, f } from 'tla'; log.push('main:' + [a, b, d, f(), this].join('/'));";
    CHECK_EQ(realm.start("main"), "pending");
    CHECK_JS_STRING(realm.interpreter, "log.join()", "start");
    CHECK_EQ(realm.settle(), "");
    CHECK_JS_STRING(realm.interpreter, "log.join()", "start,a=1,b=2,end,main:1/2/d/3/");
    // Done is done: a later graph that imports it runs synchronously, and
    // the module's own promise is the one it always had.
    realm.files["later"] = "import { a } from 'tla'; log.push('later:' + a);";
    CHECK_EQ(realm.start("later"), "");
    CHECK_EQ(realm.run("tla"), "");
    CHECK_JS_STRING(realm.interpreter, "log.join()", "start,a=1,b=2,end,main:1/2/d/3/,later:1");
    CHECK_EQ(realm.console, "");

    // Await ticks interleave with promise reactions one for one, as in an
    // async function (§27.7.5.3: one job per await of a native promise).
    ModuleRealm ticks;
    ticks.files["main"] = "globalThis.actual = [];\n"
                          "Promise.resolve(0).then(function () { actual.push('tick 1') }).then(function () { actual.push('tick 2') })\n"
                          "  .then(function () { actual.push('tick 3') });\n"
                          "await 1; actual.push('await 1'); await 2; actual.push('await 2'); await 3; actual.push('await 3');";
    CHECK_EQ(ticks.run("main"), "");
    CHECK_JS_STRING(ticks.interpreter, "actual.join()", "tick 1,await 1,tick 2,await 2,tick 3,await 3");

    // A rejected await rejects the module, its importers never run, and
    // every later request answers the same error. A throw before the
    // first await is a rejection too, through the jobs, never a throw
    // out of Evaluate.
    ModuleRealm failing;
    failing.files["bad"] = "globalThis.ran = (globalThis.ran || 0) + 1; export const x = 1; await Promise.reject(new RangeError('async boom')); export const y = 2;";
    failing.files["importer"] = "import { x } from 'bad'; globalThis.reached = true;";
    CHECK_EQ(failing.run("importer"), "rejected: RangeError: async boom");
    CHECK_JS_TRUE(failing.interpreter, "globalThis.reached === undefined && ran === 1");
    CHECK_EQ(failing.run("importer"), "rejected: RangeError: async boom");
    CHECK_EQ(failing.run("bad"), "rejected: RangeError: async boom");
    failing.files["other"] = "import 'bad';";
    CHECK_EQ(failing.run("other"), "rejected: RangeError: async boom");
    CHECK_JS_NUMBER(failing.interpreter, "ran", 1);
    failing.files["early"] = "throw new TypeError('early'); await 1;";
    CHECK_EQ(failing.start("early"), "pending");
    CHECK_EQ(failing.settle(), "rejected: TypeError: early");
    // Each evaluation promise rejected with nobody handling it is reported
    // once, as a synchronous module's is; the second request for
    // `importer` answered the promise that already had been.
    CHECK_EQ(failing.console,
        "Uncaught (in promise) RangeError: async boom\nUncaught (in promise) RangeError: async boom\n"
        "Uncaught (in promise) RangeError: async boom\nUncaught (in promise) TypeError: early\n");

    // While one dependency waits, its siblings run; the importer runs when
    // the last pending dependency has finished, synchronously in that
    // dependency's continuation, and a chain of importers without awaits
    // of their own runs in the same job.
    ModuleRealm siblings;
    siblings.files["setup"] = "globalThis.order = []; globalThis.release = null;";
    siblings.files["slow"] = "import 'setup'; order.push('slow-start'); await new Promise(function (r) { globalThis.release = r }); order.push('slow-end');";
    siblings.files["quick"] = "import 'setup'; order.push('quick'); export const sawSlowEnd = order.indexOf('slow-end') >= 0;";
    siblings.files["mid"] = "import 'slow'; order.push('mid');";
    siblings.files["main"] = "import 'setup'; import 'slow'; import { sawSlowEnd } from 'quick'; import 'mid'; order.push('main:' + sawSlowEnd);";
    CHECK_EQ(siblings.start("main"), "pending");
    CHECK_JS_STRING(siblings.interpreter, "order.join()", "slow-start,quick");
    CHECK_EQ(siblings.settle(), "pending");
    CHECK_JS_STRING(siblings.interpreter, "order.join()", "slow-start,quick");
    CHECK_EQ(siblings.run_script("release();", "script"), "");
    CHECK_EQ(siblings.state(), "");
    CHECK_JS_STRING(siblings.interpreter, "order.join()", "slow-start,quick,slow-end,mid,main:false");

    // Two pending dependencies finish in the order the world settles
    // them, not the order they were imported; the importer runs after
    // the later one. Evaluate on a module already evaluating answers the
    // same pending promise.
    ModuleRealm order;
    order.files["setup"] = "globalThis.order = []; globalThis.releases = {};";
    order.files["first"] = "import 'setup'; order.push('first-start'); await new Promise(function (r) { releases.first = r }); order.push('first-end');";
    order.files["second"] = "import 'setup'; order.push('second-start'); await new Promise(function (r) { releases.second = r }); order.push('second-end');";
    order.files["main"] = "import 'setup'; import 'first'; import 'second'; order.push('main');";
    CHECK_EQ(order.start("main"), "pending");
    CHECK_EQ(order.start("main"), "pending");
    CHECK_EQ(order.run_script("releases.second();", "script"), "");
    CHECK_JS_STRING(order.interpreter, "order.join()", "first-start,second-start,second-end");
    CHECK_EQ(order.state(), "pending");
    CHECK_EQ(order.run_script("releases.first();", "script"), "");
    CHECK_EQ(order.state(), "");
    CHECK_JS_STRING(order.interpreter, "order.join()", "first-start,second-start,second-end,first-end,main");

    // A cycle with an await in it: the dependency entered second runs
    // first and finishes first, the other waits for it, and the root
    // waits for both — nothing hangs and nothing runs twice.
    ModuleRealm cycle;
    cycle.files["setup"] = "globalThis.order = [];";
    cycle.files["a"] = "import 'setup'; import 'b'; order.push('a'); await 0; order.push('a-after');";
    cycle.files["b"] = "import 'setup'; import 'a'; order.push('b'); await 0; order.push('b-after');";
    cycle.files["main"] = "import 'setup'; import 'a'; order.push('main');";
    CHECK_EQ(cycle.run("main"), "");
    CHECK_JS_STRING(cycle.interpreter, "order.join()", "b,b-after,a,a-after,main");
    CHECK_EQ(cycle.run("b"), "");
    CHECK_JS_STRING(cycle.interpreter, "order.join()", "b,b-after,a,a-after,main");

    // An importer without an await that throws once its dependency has
    // finished rejects its own graph and leaves the dependency evaluated.
    ModuleRealm late;
    late.files["top"] = "await 0; export const t = 't';";
    late.files["mid"] = "import { t } from 'top'; throw new Error('mid saw ' + t);";
    late.files["main"] = "import 'mid'; globalThis.reached = true;";
    CHECK_EQ(late.run("main"), "rejected: Error: mid saw t");
    CHECK_JS_TRUE(late.interpreter, "globalThis.reached === undefined");
    CHECK_EQ(late.run("top"), "");
    CHECK_EQ(late.run("mid"), "rejected: Error: mid saw t");

    // import() of a module still waiting resolves with its namespace when
    // it finishes, however many times it is asked; asked again once it
    // is done, at once with the same object.
    ModuleRealm dynamic;
    dynamic.files["waiting"] = "globalThis.started = (globalThis.started || 0) + 1; await new Promise(function (r) { globalThis.go = r }); export const v = 'v';";
    CHECK_EQ(dynamic.run_script("globalThis.out = []; var p1 = import('waiting'); p1.then(function (ns) { out.push('p1:' + ns.v) });", "script"), "");
    CHECK_EQ(dynamic.run_script("var p2 = import('waiting'); p2.then(function (ns) { out.push('p2:' + ns.v) }); out.push('started:' + started);", "script"), "");
    CHECK_JS_STRING(dynamic.interpreter, "out.join()", "started:1");
    CHECK_EQ(dynamic.run_script("go();", "script"), "");
    CHECK_JS_STRING(dynamic.interpreter, "out.join()", "started:1,p1:v,p2:v");
    CHECK_EQ(dynamic.run_script("import('waiting').then(function (ns) { out.push('again:' + (ns.v) + ':' + started) });", "script"), "");
    CHECK_JS_STRING(dynamic.interpreter, "out.join()", "started:1,p1:v,p2:v,again:v:1");

    // The body runs on the bytecode tier, so what only module code has
    // must work there: `this` undefined, import.meta the record's one
    // object, import() resolved against the module, and every export
    // form — the default's name "default" for an anonymous class, a
    // function or an arrow, and untouched for anything named.
    ModuleRealm forms;
    forms.files["dep"] = "export const fromDep = 'dep';";
    forms.files["d1"] = "await 0; export default class { static who() { return 'class' } }";
    forms.files["d2"] = "await 0; export default function () { return 'function' }";
    forms.files["d3"] = "await 0; export default () => 'arrow';";
    forms.files["d4"] = "await 0; var o = { named() { return 'named' } }; export default o.named;";
    forms.files["d5"] = "export const meta = [this === undefined, typeof import.meta, import.meta === import.meta].join('/');\n"
                        "export const ns = await import('dep'); export class K { m() { return 'k' } } export let x = 1; export { x as y };\n"
                        "export * from 'dep'; export function f() { return 'f' } export var v = await Promise.resolve('v'); x = 2;";
    forms.files["main"] = "import d1 from 'd1'; import d2 from 'd2'; import d3 from 'd3'; import d4 from 'd4';\n"
                          "import { meta, ns, K, x, y, fromDep, f, v } from 'd5';\n"
                          "globalThis.out = [d1.name, d1.who(), d2.name, d2(), d3.name, d3(), d4.name, d4(),\n"
                          "  meta, ns.fromDep, new K().m(), x, y, fromDep, f(), v].join();";
    CHECK_EQ(forms.run("main"), "");
    CHECK_JS_STRING(forms.interpreter, "out",
        "default,class,default,function,default,arrow,named,named,true/object/true,dep,k,2,2,dep,f,v");
    CHECK_EQ(forms.console, "");

    // `for await` at the top level: the module's body runs as an async
    // function, each step awaited, the module evaluated once it is done.
    ModuleRealm awaited;
    awaited.files["fa"] = "globalThis.out = []; for await (const x of [1, Promise.resolve(2), 3]) out.push(x); out.push('end');";
    CHECK_EQ(awaited.run("fa"), "");
    CHECK_JS_STRING(awaited.interpreter, "out.join()", "1,2,3,end");
}

void test_default_exports()
{
    ModuleRealm realm;
    realm.files["fn"] = "export default function () { return 'fn' }";
    realm.files["named-fn"] = "export default function named() { return named.name }";
    realm.files["gen"] = "export default function* () { yield 'gen' }";
    realm.files["async-fn"] = "export default async function () { return 'async' }";
    realm.files["cls"] = "export default class { who() { return 'cls' } }";
    realm.files["named-cls"] = "export default class Named { who() { return Named.name } }";
    realm.files["expr-fn"] = "export default (function () {});";
    realm.files["arrow"] = "export default () => 'arrow';";
    realm.files["expr-cls"] = "export default (class {}).name;";
    realm.files["value"] = "export default 'value';";
    realm.files["main"] = "import fn from 'fn'; import namedFn from 'named-fn'; import gen from 'gen'; import asyncFn from 'async-fn';\n"
                          "import Cls from 'cls'; import NamedCls from 'named-cls'; import exprFn from 'expr-fn'; import arrow from 'arrow';\n"
                          "import exprClsName from 'expr-cls'; import value from 'value'; import * as ns from 'fn';\n"
                          "globalThis.out = [fn(), fn.name, namedFn(), gen().next().value, gen.name, asyncFn.name, new Cls().who(), Cls.name,\n"
                          "  new NamedCls().who(), exprFn.name, arrow(), arrow.name, exprClsName, value, ns.default === fn, Object.keys(ns).join()].join('|');";
    CHECK_EQ(realm.run("main"), "");
    CHECK_JS_STRING(realm.interpreter, "out", "fn|default|named|gen|default|default|cls|default|Named|default|arrow|default||value|true|default");
    // A hoisted default function is callable before the module body ran
    // (a cycle), and a default expression is not.
    ModuleRealm hoist;
    hoist.files["a"] = "import b from 'b'; export default function () { return 'a' } export const fromB = b;";
    hoist.files["b"] = "import a from 'a'; export default a();";
    hoist.files["main"] = "import { fromB } from 'a'; globalThis.out = fromB;";
    CHECK_EQ(hoist.run("main"), "");
    CHECK_JS_STRING(hoist.interpreter, "out", "a");
}

void test_module_code_semantics()
{
    ModuleRealm realm;
    realm.files["main"] = "var top = this; function f() { return this } var g = function () { return typeof this }\n"
                          "export const arrow = (() => this)();\n"
                          "var strictness; try { undeclared = 1 } catch (e) { strictness = e.name }\n"
                          "globalThis.out = [top === undefined, f() === undefined, g(), arrow === undefined, strictness, typeof window].join();";
    CHECK_EQ(realm.run("main"), "");
    CHECK_JS_STRING(realm.interpreter, "out", "true,true,undefined,true,ReferenceError,undefined");
    // A module's top-level function and var do not reach the global object.
    CHECK_JS_STRING(realm.interpreter, "typeof f + typeof top + typeof arrow", "undefinedundefinedundefined");
    // Block-level functions in module code are strict: no Annex B hoisting.
    realm.files["annex"] = "{ function inner() {} } globalThis.out = typeof inner;";
    CHECK_EQ(realm.run("annex"), "");
    CHECK_JS_STRING(realm.interpreter, "out", "undefined");
    // A module's globals are the realm's; a script sees what it assigned.
    realm.files["writer"] = "globalThis.fromModule = 'yes'; export const x = 1;";
    CHECK_EQ(realm.run("writer"), "");
    CHECK_JS_STRING(realm.interpreter, "fromModule", "yes");
    // Evaluating a module twice hands back the same promise; the map keeps
    // one record per key.
    js::ModuleRecord* record = realm.interpreter.find_module("writer");
    CHECK(record != nullptr);
    CHECK(record && record->status() == js::ModuleRecord::Status::Evaluated);
    CHECK(realm.interpreter.parse_module(u"export const x = 2;", "writer") == record);
}

void test_import_and_export_of_index_names()
{
    ModuleRealm realm;
    realm.files["m"] = "const v = 'zero'; export { v as '0', v as 'a b' };";
    realm.files["main"] = "import * as ns from 'm'; import { '0' as zero, 'a b' as ab } from 'm';\n"
                          "globalThis.out = [zero, ab, ns[0], ns['a b'], Object.keys(ns).join(), 0 in ns, Object.getOwnPropertyNames(ns).join()].join('|');";
    CHECK_EQ(realm.run("main"), "");
    CHECK_JS_STRING(realm.interpreter, "out", "zero|zero|zero|zero|0,a b|true|0,a b");
}

void test_dynamic_import()
{
    ModuleRealm realm;
    realm.files["dep"] = "globalThis.evaluations = (globalThis.evaluations || 0) + 1; export const v = 1; export default 'd';";
    realm.files["main"] = "globalThis.out = [];\n"
                          "var p = import('dep');\n"
                          "out.push(p instanceof Promise, p.constructor === Promise, p !== import('dep'));\n"
                          "var first;\n"
                          "p.then(function (ns) {\n"
                          "  first = ns;\n"
                          "  out.push('v=' + ns.v, 'default=' + ns.default, Object.getPrototypeOf(ns) === null,\n"
                          "    ns[Symbol.toStringTag], evaluations);\n"
                          "  return import('dep');\n"
                          "}).then(function (again) { out.push(again === first, evaluations) });";
    CHECK_EQ(realm.run("main"), "");
    // A fresh promise of %Promise% each time; the namespace is the
    // module's own, with its live export and its default; and however
    // many times it is imported the body ran once.
    CHECK_JS_STRING(realm.interpreter, "out.join('|')", "true|true|true|v=1|default=d|true|Module|1|true|1");
    // Every call asked the host to resolve, even the ones the map answered.
    CHECK_EQ(realm.resolutions, "main|dep\nmain|dep\nmain|dep\n");
    CHECK_EQ(realm.console, "");
}

void test_dynamic_import_failures()
{
    ModuleRealm realm;
    realm.files["dep"] = "export const v = 1;";
    realm.files["broken"] = "export var = 1;";
    realm.files["thrower"] = "throw new RangeError('boom');";
    realm.files["main"] = "globalThis.out = [];\n"
                          "function watch(tag, p) {\n"
                          "  return p.then(function () { out.push(tag + ':resolved') }, function (e) { out.push(tag + ':' + e.name) });\n"
                          "}\n"
                          "var poison = { toString: function () { throw new TypeError('specifier says no') } };\n"
                          "watch('missing', import('nowhere'));\n"
                          "watch('tostring', import(poison));\n"
                          "watch('symbol', import(Symbol('s')));\n"
                          "watch('parse', import('broken'));\n"
                          "watch('evaluation', import('thrower'));\n"
                          "watch('options', import('dep', 5));\n"
                          "watch('with', import('dep', { with: 5 }));\n"
                          "watch('type', import('dep', { with: { type: 'json' } }));\n"
                          "watch('value', import('dep', { with: { other: 5 } }));\n"
                          "watch('unknown', import('dep', { with: { other: 'x' } }));\n"
                          "watch('both', import('dep', { with: { type: 'json', other: 'x' } }));\n"
                          "import('dep', { with: { type: 'json' } }).catch(function (e) { globalThis.typeMessage = e.message });\n"
                          "import('dep', { with: { other: 'x' } }).catch(function (e) { globalThis.otherMessage = e.message });\n"
                          "import('dep', { with: { type: 'json', other: 'x' } }).catch(function (e) { globalThis.bothMessage = e.message });";
    // Not one of them throws out of the expression: the module evaluates
    // to its end and every failure is a rejection. Sorted, because the
    // order the rejections settle in is not what this is about.
    CHECK_EQ(realm.run("main"), "");
    CHECK_JS_STRING(realm.interpreter, "out.slice().sort().join('|')",
        "both:TypeError|evaluation:RangeError|missing:TypeError|options:TypeError|parse:SyntaxError|symbol:TypeError"
        "|tostring:TypeError|type:TypeError|unknown:TypeError|value:TypeError|with:TypeError");
    // A module type is refused by name, the way a static import's is.
    CHECK_JS_STRING(realm.interpreter, "typeMessage",
        "Cannot import 'dep' as a module of type 'json': modules of that type are not supported yet");
    // An attribute key outside the host's supported set is not left to the
    // host to ignore: AllImportAttributesSupported is false for it, and
    // §13.3.10.1 rejects the capability with a TypeError of its own. The
    // set here holds `type` and nothing else.
    CHECK_JS_STRING(realm.interpreter, "otherMessage",
        "Cannot import 'dep' with the import attribute 'other': that attribute is not supported");
    // That check comes before the host's look at a `type` value, so a
    // request carrying both is refused for the unsupported key.
    CHECK_JS_STRING(realm.interpreter, "bothMessage",
        "Cannot import 'dep' with the import attribute 'other': that attribute is not supported");
    // What the loader was asked to resolve: only the three requests that
    // got that far. A bad options argument, a bad attribute value, a
    // module type and an unsupported attribute key are all refused before
    // the resolver, so none of them named a module to the host.
    CHECK_EQ(realm.resolutions, "main|nowhere\nmain|broken\nmain|thrower\n");
    // Every rejection had a handler, so nothing was reported unhandled.
    CHECK_EQ(realm.console, "");
}

void test_dynamic_import_referrers()
{
    ModuleRealm realm;
    realm.files["dep"] = "export const v = 'dep';";
    realm.files["helper"] = "export function load() { return import('dep') }";
    realm.files["caller"] = "import { load } from 'helper'; globalThis.out = []; load().then(function (ns) { out.push(ns.v) });";
    CHECK_EQ(realm.run("caller"), "");
    CHECK_JS_STRING(realm.interpreter, "out.join()", "dep");
    // The referrer is the module the function was written in, not the one
    // whose code called it.
    CHECK_EQ(realm.resolutions, "caller|helper\nhelper|dep\n");
    // A classic script resolves against the name the host ran it under.
    ModuleRealm script;
    script.files["dep"] = "export const v = 'dep';";
    CHECK_EQ(script.run_script("globalThis.out = []; import('dep').then(function (ns) { out.push(ns.v) });", "page.js"), "");
    CHECK_JS_STRING(script.interpreter, "out.join()", "dep");
    CHECK_EQ(script.resolutions, "page.js|dep\n");
    // import.meta is module code only, and the parser says so.
    CHECK(script.run_script("import.meta;", "page.js").starts_with("threw: SyntaxError"));
}

void test_dynamic_import_from_eval_code()
{
    // Eval code is not a script and not a module: PerformEval (§19.2.1.1)
    // gives the eval execution context the caller's [[ScriptOrModule]], so
    // GetActiveScriptOrModule in eval code answers the script or module the
    // eval was written in, and a relative specifier resolves against that.
    ModuleRealm realm;
    realm.files["dep"] = "export const v = 'dep';";
    realm.files["holder"] = "export function load() { return eval(\"import('dep')\") }";
    realm.files["main"] = "import { load } from 'holder';\n"
                          "globalThis.out = [];\n"
                          "function keep(tag) { return function (ns) { out.push(tag + ':' + ns.v) } }\n"
                          "eval(\"import('dep')\").then(keep('direct'));\n"
                          "eval(\"eval(\\\"import('dep')\\\")\").then(keep('nested'));\n"
                          "eval(\"(function () { return import('dep') })\")().then(keep('fn'));\n"
                          "function* gen() { yield eval(\"import('dep')\") }\n"
                          "gen().next().value.then(keep('vm'));\n"
                          "load().then(keep('holder'));";
    CHECK_EQ(realm.run("main"), "");
    CHECK_JS_STRING(realm.interpreter, "out.slice().sort().join('|')", "direct:dep|fn:dep|holder:dep|nested:dep|vm:dep");
    // Each resolved against a module, never against the eval: the four in
    // `main` — the plain one, an eval within that eval, the function an
    // eval made and left behind, and a generator body, which runs on the
    // bytecode tier — against `main`, and `holder`'s against `holder`.
    CHECK_EQ(realm.resolutions, "main|holder\nmain|dep\nmain|dep\nmain|dep\nmain|dep\nholder|dep\n");
    // A classic script is inherited the same way.
    ModuleRealm script;
    script.files["dep"] = "export const v = 'dep';";
    CHECK_EQ(script.run_script("globalThis.out = []; eval(\"import('dep')\").then(function (ns) { out.push(ns.v) });", "page.js"), "");
    CHECK_JS_STRING(script.interpreter, "out.join()", "dep");
    CHECK_EQ(script.resolutions, "page.js|dep\n");
    // The indirect form inherits nothing: it runs from the `eval` function
    // itself, whose execution context carries no script or module (§10.3.3),
    // so the host is given a referrer that names no module of the map
    // rather than the module the call was written in.
    ModuleRealm indirect;
    indirect.files["dep"] = "export const v = 'dep';";
    indirect.files["main"] = "globalThis.out = []; var run = eval;\n"
                             "run(\"import('dep')\").then(function (ns) { out.push(ns.v) });";
    CHECK_EQ(indirect.run("main"), "");
    CHECK_JS_STRING(indirect.interpreter, "out.join()", "dep");
    CHECK_EQ(indirect.resolutions, "eval|dep\n");
}

void test_dynamic_import_on_the_bytecode_tier()
{
    // An async function's body and a generator's compile to bytecode, so
    // these two go through the opcodes rather than the tree-walker.
    ModuleRealm realm;
    realm.files["dep"] = "export const v = 'vm';";
    realm.files["main"] = "globalThis.out = [];\n"
                          "async function viaAwait() { var ns = await import('dep'); return ns.v + ':' + (import.meta === mine) }\n"
                          "function* viaYield() { yield import('dep'); yield import.meta }\n"
                          "var mine = import.meta;\n"
                          "viaAwait().then(function (text) { out.push(text) });\n"
                          "var iterator = viaYield();\n"
                          "iterator.next().value.then(function (ns) { out.push('gen:' + ns.v) });\n"
                          "out.push('genMeta:' + (iterator.next().value === mine));";
    CHECK_EQ(realm.run("main"), "");
    CHECK_JS_STRING(realm.interpreter, "out.slice().sort().join('|')", "gen:vm|genMeta:true|vm:true");
    CHECK_EQ(realm.console, "");
}

void test_dynamic_import_of_a_failed_module()
{
    ModuleRealm realm;
    realm.files["thrower"] = "globalThis.ran = (globalThis.ran || 0) + 1; throw new RangeError('boom');";
    realm.files["main"] = "globalThis.out = [];\n"
                          "function note(tag) { return function (e) { out.push(tag + ':' + e.name + ':' + e.message + ':' + ran) } }\n"
                          "import('thrower').then(null, note('first'))\n"
                          "  .then(function () { return import('thrower') }).then(null, note('second'));";
    CHECK_EQ(realm.run("main"), "");
    // The body ran once and every later request is the same error.
    CHECK_JS_STRING(realm.interpreter, "out.join('|')", "first:RangeError:boom:1|second:RangeError:boom:1");
    CHECK_JS_NUMBER(realm.interpreter, "ran", 1);
    CHECK_EQ(realm.console, "");
}

void test_dynamic_import_of_the_running_module()
{
    // A module that imports itself while its own body is still running:
    // the record is linked already, so it must be neither linked nor
    // evaluated again — the environment its bindings live in has to
    // survive — and the promise settles with the namespace once the
    // evaluation under way has ended.
    ModuleRealm realm;
    realm.files["self"] = "globalThis.evaluations = (globalThis.evaluations || 0) + 1;\n"
                          "globalThis.out = [];\n"
                          "export let counter = 0;\n"
                          "export default class { value() { return 45 } }\n"
                          "import('self').then(function (ns) {\n"
                          "  out.push(new ns.default().value(), ns.default.name, ns.counter, evaluations);\n"
                          "  counter = 1;\n"
                          "  out.push(ns.counter);\n"
                          "});";
    CHECK_EQ(realm.run("self"), "");
    CHECK_JS_STRING(realm.interpreter, "out.join('|')", "45|default|0|1|1");
    CHECK_EQ(realm.console, "");
    // The record kept the environment it was linked with, so a module
    // that imports it afterwards still reads the live binding.
    realm.files["later"] = "import { counter } from 'self'; globalThis.seen = counter;";
    CHECK_EQ(realm.run("later"), "");
    CHECK_JS_NUMBER(realm.interpreter, "seen", 1);
}

void test_import_meta()
{
    ModuleRealm realm;
    realm.files["other"] = "export const meta = import.meta; export function getMeta() { return import.meta }";
    realm.files["main"] = "import { meta as otherMeta, getMeta } from 'other';\n"
                          "var mine = import.meta;\n"
                          "globalThis.out = [mine === import.meta, typeof mine, Object.getPrototypeOf(mine) === null,\n"
                          "  Object.keys(mine).length, mine === otherMeta, otherMeta === getMeta(),\n"
                          "  mine === (function () { return import.meta })(), Object.isExtensible(mine)].join('|');\n"
                          "mine.added = 1; globalThis.out += '|' + import.meta.added;";
    CHECK_EQ(realm.run("main"), "");
    // One object per module, the same on every read, ordinary and
    // extensible with no prototype and, with no host, no properties.
    CHECK_JS_STRING(realm.interpreter, "out", "true|object|true|0|false|true|true|true|1");
    // A host's properties are on it from the start.
    ModuleRealm hosted;
    hosted.interpreter.set_module_meta_hook([](js::Interpreter& in, js::ModuleRecord& record, js::Object& meta) {
        meta.put(in.key("url"), js::Value::string(in.string("file:///" + record.key())), js::default_attributes);
    });
    hosted.files["main"] = "globalThis.out = [import.meta.url, Object.keys(import.meta).join()].join('|');";
    CHECK_EQ(hosted.run("main"), "");
    CHECK_JS_STRING(hosted.interpreter, "out", "file:///main|url");
}

} // namespace

int main()
{
    test_exports_and_imports();
    test_live_bindings();
    test_namespace_object();
    test_indirect_and_star_exports();
    test_link_errors();
    test_evaluation_order_and_cycles();
    test_evaluation_errors();
    test_top_level_await();
    test_default_exports();
    test_module_code_semantics();
    test_import_and_export_of_index_names();
    test_dynamic_import();
    test_dynamic_import_failures();
    test_dynamic_import_referrers();
    test_dynamic_import_from_eval_code();
    test_dynamic_import_on_the_bytecode_tier();
    test_dynamic_import_of_a_failed_module();
    test_dynamic_import_of_the_running_module();
    test_import_meta();
    return sashfold::test::report("js_module");
}
