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
// specifier is a key as written (no resolution beyond that).
struct ModuleRealm {
    js::Interpreter interpreter;
    std::map<std::string, std::string> files;
    std::string console; // what console.* printed, one line each

    ModuleRealm()
    {
        interpreter.heap().set_stress(true);
        interpreter.on_console = [this](std::string_view, std::string_view message) {
            console += std::string(message) + "\n";
        };
        interpreter.set_module_hooks(
            [this](std::string_view, std::string_view specifier, std::string& error) -> std::optional<std::string> {
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
        js::Interpreter::Roots const roots(interpreter);
        interpreter.root(*promise);
        interpreter.run_jobs([this](js::Value const& thrown) { console += "job threw " + interpreter.describe(thrown) + "\n"; });
        auto const* state = static_cast<js::PromiseObject const*>(promise->as_object());
        if (state->state() == js::PromiseObject::State::Rejected)
            return "rejected: " + interpreter.describe(state->result());
        if (state->state() == js::PromiseObject::State::Pending)
            return "pending";
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
    // A module that awaits at its top level is refused by name, for now.
    realm.files["tla"] = "export const v = await 1;";
    CHECK_EQ(realm.run("tla"), "rejected: SyntaxError: top-level await is not supported yet");
    // The evaluation promise settles through the job queue like any other.
    realm.files["fine"] = "export const ok = true;";
    CHECK_EQ(realm.run("fine"), "");
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
    test_default_exports();
    test_module_code_semantics();
    test_import_and_export_of_index_names();
    return sashfold::test::report("js_module");
}
