// The Proxy exotic object (§10.5) and the Proxy constructor (§28.2): every
// trap, the fall-through when a handler has none, revocation, the invariant
// checks that hold a trap to what its target has committed to, and the
// places in the rest of the engine that have to see a proxy for what it is.
// Each realm runs under heap stress, so a trap's answer that nothing else
// holds on to fails here the first time.

#include "JsTest.h"

#include "js/Interpreter.h"
#include "js/Object.h"

#include <string>

using namespace sashfold;

namespace {

// A realm with a `log` array on the global: a trap pushes its name, and the
// test asserts the exact sequence, so a check cannot pass by a trap never
// having run at all.
struct ProxyRealm {
    js::Interpreter interpreter;

    ProxyRealm()
    {
        interpreter.heap().set_stress(true);
        test::run_js(interpreter, "globalThis.log = [];");
    }
};

void test_every_trap_runs_and_is_handed_the_right_arguments()
{
    ProxyRealm realm;
    js::Interpreter& in = realm.interpreter;
    // One handler recording every trap, each still doing the honest thing
    // through Reflect so that the invariant checks are satisfied.
    test::run_js(in, R"JS(
        globalThis.target = { a: 1 };
        globalThis.handler = {
            get(t, k, r) { log.push('get:' + String(k)); return Reflect.get(t, k, r); },
            set(t, k, v, r) { log.push('set:' + String(k)); return Reflect.set(t, k, v, r); },
            has(t, k) { log.push('has:' + String(k)); return Reflect.has(t, k); },
            deleteProperty(t, k) { log.push('delete:' + String(k)); return Reflect.deleteProperty(t, k); },
            ownKeys(t) { log.push('ownKeys'); return Reflect.ownKeys(t); },
            getOwnPropertyDescriptor(t, k) { log.push('gopd:' + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
            defineProperty(t, k, d) { log.push('define:' + String(k)); return Reflect.defineProperty(t, k, d); },
            getPrototypeOf(t) { log.push('getProto'); return Reflect.getPrototypeOf(t); },
            setPrototypeOf(t, p) { log.push('setProto'); return Reflect.setPrototypeOf(t, p); },
            isExtensible(t) { log.push('isExtensible'); return Reflect.isExtensible(t); },
            preventExtensions(t) { log.push('preventExtensions'); return Reflect.preventExtensions(t); },
        };
        globalThis.p = new Proxy(target, handler);
    )JS");

    CHECK_JS_NUMBER(in, "p.a", 1);
    CHECK_JS_STRING(in, "log.join()", "get:a");

    test::run_js(in, "log.length = 0;");
    CHECK_JS_TRUE(in, "'a' in p");
    CHECK_JS_STRING(in, "log.join()", "has:a");

    test::run_js(in, "log.length = 0;");
    CHECK_JS_NUMBER(in, "(p.b = 7, target.b)", 7);
    // OrdinarySet on the target's behalf reads the receiver's own
    // descriptor and then defines it there: both are the proxy's traps.
    CHECK_JS_STRING(in, "log.join()", "set:b,gopd:b,define:b");

    test::run_js(in, "log.length = 0;");
    CHECK_JS_TRUE(in, "delete p.b");
    CHECK_JS_STRING(in, "log.join()", "delete:b");

    // GetOwnPropertyKeys (§20.1.2.11) asks [[OwnPropertyKeys]] and filters
    // the answer by key type: it reads no descriptors at all.
    test::run_js(in, "log.length = 0;");
    CHECK_JS_STRING(in, "Object.getOwnPropertyNames(p).join()", "a");
    CHECK_JS_STRING(in, "log.join()", "ownKeys");
    // EnumerableOwnProperties (§7.3.24) reads each key's descriptor to find
    // out whether it is enumerable, and for values and entries the value too.
    test::run_js(in, "log.length = 0;");
    CHECK_JS_STRING(in, "Object.keys(p).join()", "a");
    CHECK_JS_STRING(in, "log.join()", "ownKeys,gopd:a");
    test::run_js(in, "log.length = 0;");
    CHECK_JS_STRING(in, "Object.values(p).join()", "1");
    CHECK_JS_STRING(in, "log.join()", "ownKeys,gopd:a,get:a");

    test::run_js(in, "log.length = 0;");
    CHECK_JS_NUMBER(in, "Object.getOwnPropertyDescriptor(p, 'a').value", 1);
    CHECK_JS_STRING(in, "log.join()", "gopd:a");

    test::run_js(in, "log.length = 0;");
    CHECK_JS_NUMBER(in, "(Object.defineProperty(p, 'c', { value: 3, configurable: true }), target.c)", 3);
    CHECK_JS_STRING(in, "log.join()", "define:c");

    test::run_js(in, "log.length = 0;");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(p) === Object.prototype");
    CHECK_JS_STRING(in, "log.join()", "getProto");

    test::run_js(in, "log.length = 0;");
    CHECK_JS_TRUE(in, "(Object.setPrototypeOf(p, null), Object.getPrototypeOf(target) === null)");
    CHECK_JS_STRING(in, "log.join()", "setProto");

    test::run_js(in, "log.length = 0;");
    CHECK_JS_TRUE(in, "Object.isExtensible(p)");
    CHECK_JS_STRING(in, "log.join()", "isExtensible");

    // The invariant check behind preventExtensions interrogates the target
    // itself, not the proxy, so only the one trap runs here; asking the
    // proxy afterwards is what runs the other.
    test::run_js(in, "log.length = 0;");
    CHECK_JS_TRUE(in, "(Object.preventExtensions(p), !Object.isExtensible(target))");
    CHECK_JS_STRING(in, "log.join()", "preventExtensions");
    test::run_js(in, "log.length = 0;");
    CHECK_JS_FALSE(in, "Object.isExtensible(p)");
    CHECK_JS_STRING(in, "log.join()", "isExtensible");

    // The apply and construct traps, on a callable target.
    test::run_js(in, R"JS(
        log.length = 0;
        globalThis.fn = function (x) { return x * 2; };
        globalThis.callable = new Proxy(fn, {
            apply(t, self, args) { log.push('apply:' + args.join()); return Reflect.apply(t, self, args); },
            construct(t, args, nt) { log.push('construct:' + args.join()); return Reflect.construct(t, args, nt); },
        });
    )JS");
    CHECK_JS_NUMBER(in, "callable(21)", 42);
    CHECK_JS_STRING(in, "log.join()", "apply:21");
    test::run_js(in, "log.length = 0;");
    CHECK_JS_TRUE(in, "typeof new callable(1) === 'object'");
    CHECK_JS_STRING(in, "log.join()", "construct:1");
}

void test_absent_traps_fall_through_to_the_target()
{
    ProxyRealm realm;
    js::Interpreter& in = realm.interpreter;
    // An empty handler: every operation must behave as it does on the
    // target itself. The handler's own absent traps are read with
    // GetMethod, so null is as good as missing.
    test::run_js(in, R"JS(
        globalThis.target = { a: 1, b: 2 };
        Object.defineProperty(target, 'hidden', { value: 3, enumerable: false, configurable: true });
        globalThis.bare = new Proxy(target, {});
        globalThis.nulled = new Proxy(target, { get: null, has: undefined });
    )JS");
    CHECK_JS_NUMBER(in, "bare.a", 1);
    CHECK_JS_NUMBER(in, "nulled.a", 1);
    CHECK_JS_TRUE(in, "'b' in bare && 'b' in nulled");
    CHECK_JS_FALSE(in, "'zz' in bare");
    CHECK_JS_STRING(in, "Object.keys(bare).join()", "a,b");
    CHECK_JS_STRING(in, "Object.getOwnPropertyNames(bare).join()", "a,b,hidden");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(bare) === Object.prototype");
    CHECK_JS_TRUE(in, "Object.isExtensible(bare)");
    CHECK_JS_NUMBER(in, "(bare.c = 9, target.c)", 9);
    CHECK_JS_TRUE(in, "delete bare.c");
    CHECK_JS_FALSE(in, "'c' in target");
    CHECK_JS_NUMBER(in, "Object.getOwnPropertyDescriptor(bare, 'a').value", 1);
    CHECK_JS_STRING(in, "JSON.stringify(bare)", "{\"a\":1,\"b\":2}");
    // A callable target with no apply trap is still callable, and a
    // constructor target with no construct trap still constructs.
    CHECK_JS_NUMBER(in, "new Proxy(x => x + 1, {})(1)", 2);
    CHECK_JS_NUMBER(in, "new (new Proxy(class { constructor() { this.v = 5; } }, {}))().v", 5);
}

void test_revocation()
{
    ProxyRealm realm;
    js::Interpreter& in = realm.interpreter;
    test::run_js(in, R"JS(
        globalThis.r = Proxy.revocable({ a: 1 }, {});
        globalThis.p = r.proxy;
    )JS");
    CHECK_JS_NUMBER(in, "p.a", 1);
    CHECK_JS_TRUE(in, "typeof r.revoke === 'function'");
    CHECK_JS_TRUE(in, "r.revoke() === undefined");
    // Every operation after revocation is a TypeError, and a second
    // revocation is a no-op rather than a throw.
    CHECK_JS_TRUE(in, "r.revoke() === undefined");
    CHECK_JS_THROWS(in, "p.a", "TypeError");
    CHECK_JS_THROWS(in, "p.a = 2", "TypeError");
    CHECK_JS_THROWS(in, "'a' in p", "TypeError");
    CHECK_JS_THROWS(in, "delete p.a", "TypeError");
    CHECK_JS_THROWS(in, "Object.keys(p)", "TypeError");
    CHECK_JS_THROWS(in, "Object.getPrototypeOf(p)", "TypeError");
    CHECK_JS_THROWS(in, "Object.setPrototypeOf(p, null)", "TypeError");
    CHECK_JS_THROWS(in, "Object.isExtensible(p)", "TypeError");
    CHECK_JS_THROWS(in, "Object.preventExtensions(p)", "TypeError");
    CHECK_JS_THROWS(in, "Object.getOwnPropertyDescriptor(p, 'a')", "TypeError");
    CHECK_JS_THROWS(in, "Object.defineProperty(p, 'x', { value: 1 })", "TypeError");
    CHECK_JS_THROWS(in, "Array.isArray(p)", "TypeError");
    CHECK_JS_THROWS(in, "for (const k in p) {}", "TypeError");
    // A revoked callable proxy is still callable — and throws when called.
    test::run_js(in, "globalThis.c = Proxy.revocable(function () {}, {}); c.revoke();");
    CHECK_JS_STRING(in, "typeof c.proxy", "function");
    CHECK_JS_THROWS(in, "c.proxy()", "TypeError");
    CHECK_JS_THROWS(in, "new c.proxy()", "TypeError");
    // The target of a revoked proxy is let go, so nothing the proxy holds
    // keeps it alive; what matters observably is that it reports nothing.
    CHECK_JS_THROWS(in, "Reflect.ownKeys(p)", "TypeError");
}

void test_invariant_violations_are_type_errors()
{
    ProxyRealm realm;
    js::Interpreter& in = realm.interpreter;
    // A frozen target: one non-configurable, non-writable data property,
    // and no room for new ones. Every trap below lies about it.
    test::run_js(in, "globalThis.frozen = Object.freeze({ a: 1 });");

    // 1. getOwnPropertyDescriptor may not report a non-configurable
    //    property of the target as missing.
    CHECK_JS_THROWS(in,
        "Object.getOwnPropertyDescriptor(new Proxy(frozen, { getOwnPropertyDescriptor() { return undefined; } }), 'a')",
        "TypeError");
    // 2. …nor invent one on a non-extensible target.
    CHECK_JS_THROWS(in,
        "Object.getOwnPropertyDescriptor(new Proxy(frozen, "
        "{ getOwnPropertyDescriptor() { return { value: 1, configurable: true }; } }), 'zz')",
        "TypeError");
    // 3. get may not report a value other than a read-only,
    //    non-configurable property's own.
    CHECK_JS_THROWS(in, "(new Proxy(frozen, { get() { return 2; } })).a", "TypeError");
    // 4. set may not claim to have written one.
    CHECK_JS_THROWS(in, "'use strict'; (new Proxy(frozen, { set() { return true; } })).a = 2", "TypeError");
    // 5. has may not disown a non-configurable property.
    CHECK_JS_THROWS(in, "'a' in new Proxy(frozen, { has() { return false; } })", "TypeError");
    // 6. deleteProperty may not claim to have deleted one.
    CHECK_JS_THROWS(in, "delete (new Proxy(frozen, { deleteProperty() { return true; } })).a", "TypeError");
    // 7. ownKeys must name every non-configurable key…
    CHECK_JS_THROWS(in, "Object.getOwnPropertyNames(new Proxy(frozen, { ownKeys() { return []; } }))", "TypeError");
    // 8. …must not invent keys on a non-extensible target…
    CHECK_JS_THROWS(in,
        "Object.getOwnPropertyNames(new Proxy(frozen, { ownKeys() { return ['a', 'zz']; } }))", "TypeError");
    // 9. …and must be free of duplicates, whatever the target.
    CHECK_JS_THROWS(in,
        "Object.getOwnPropertyNames(new Proxy({}, { ownKeys() { return ['x', 'x']; } }))", "TypeError");
    // 10. getPrototypeOf must agree with a non-extensible target.
    CHECK_JS_THROWS(in,
        "Object.getPrototypeOf(new Proxy(frozen, { getPrototypeOf() { return null; } }))", "TypeError");
    // 11. …and must answer an object or null at all.
    CHECK_JS_THROWS(in, "Object.getPrototypeOf(new Proxy({}, { getPrototypeOf() { return 1; } }))", "TypeError");
    // 12. setPrototypeOf may not claim a change on a non-extensible target.
    CHECK_JS_THROWS(in,
        "Object.setPrototypeOf(new Proxy(frozen, { setPrototypeOf() { return true; } }), null)", "TypeError");
    // 13. isExtensible may not disagree with the target, either way round.
    CHECK_JS_THROWS(in, "Object.isExtensible(new Proxy(frozen, { isExtensible() { return true; } }))", "TypeError");
    CHECK_JS_THROWS(in, "Object.isExtensible(new Proxy({}, { isExtensible() { return false; } }))", "TypeError");
    // 14. preventExtensions may not claim success while the target stays
    //     extensible.
    CHECK_JS_THROWS(in,
        "Object.preventExtensions(new Proxy({}, { preventExtensions() { return true; } }))", "TypeError");
    // 15. defineProperty may not add to a non-extensible target…
    CHECK_JS_THROWS(in,
        "Object.defineProperty(new Proxy(frozen, { defineProperty() { return true; } }), 'zz', { value: 1 })",
        "TypeError");
    // 16. …nor report a non-configurable definition the target does not hold.
    CHECK_JS_THROWS(in,
        "Object.defineProperty(new Proxy({}, { defineProperty() { return true; } }), 'x', "
        "{ value: 1, configurable: false })",
        "TypeError");
    // 17. construct must answer an object.
    CHECK_JS_THROWS(in, "new (new Proxy(function () {}, { construct() { return 1; } }))()", "TypeError");
    // 18. A trap that is present but not callable is a TypeError.
    CHECK_JS_THROWS(in, "(new Proxy({}, { get: 1 })).a", "TypeError");
    // 19. Neither target nor handler may be a non-object.
    CHECK_JS_THROWS(in, "new Proxy(1, {})", "TypeError");
    CHECK_JS_THROWS(in, "new Proxy({}, 1)", "TypeError");
    CHECK_JS_THROWS(in, "Proxy.revocable({}, 1)", "TypeError");
    // 20. Proxy is not callable without `new`, and has no prototype object.
    CHECK_JS_THROWS(in, "Proxy({}, {})", "TypeError");
    CHECK_JS_TRUE(in, "Proxy.prototype === undefined");

    // The positive controls: the same shapes, told the truth, all pass.
    CHECK_JS_NUMBER(in, "(new Proxy(frozen, { get(t, k) { return t[k]; } })).a", 1);
    CHECK_JS_TRUE(in, "'a' in new Proxy(frozen, { has() { return true; } })");
    CHECK_JS_FALSE(in, "delete (new Proxy(frozen, { deleteProperty() { return false; } })).a");
    CHECK_JS_STRING(in, "Object.getOwnPropertyNames(new Proxy(frozen, { ownKeys() { return ['a']; } })).join()", "a");
    CHECK_JS_TRUE(in,
        "Object.getPrototypeOf(new Proxy(frozen, { getPrototypeOf() { return Object.prototype; } })) === Object.prototype");
    CHECK_JS_FALSE(in, "Object.isExtensible(new Proxy(frozen, { isExtensible() { return false; } }))");
    CHECK_JS_TRUE(in, "Object.isExtensible(new Proxy({}, { isExtensible() { return true; } }))");
}

void test_a_proxy_of_an_array()
{
    ProxyRealm realm;
    js::Interpreter& in = realm.interpreter;
    test::run_js(in, R"JS(
        globalThis.array = [1, 2, 3];
        globalThis.p = new Proxy(array, {});
        globalThis.nested = new Proxy(new Proxy(array, {}), {});
    )JS");
    // IsArray sees through any depth of proxy.
    CHECK_JS_TRUE(in, "Array.isArray(p)");
    CHECK_JS_TRUE(in, "Array.isArray(nested)");
    CHECK_JS_FALSE(in, "Array.isArray(new Proxy({}, {}))");
    CHECK_JS_STRING(in, "Object.prototype.toString.call(p)", "[object Array]");
    CHECK_JS_STRING(in, "JSON.stringify(p)", "[1,2,3]");
    CHECK_JS_STRING(in, "JSON.stringify({ x: nested })", "{\"x\":[1,2,3]}");
    CHECK_JS_NUMBER(in, "p.length", 3);
    CHECK_JS_STRING(in, "[].concat(p).join()", "1,2,3"); // IsConcatSpreadable
    CHECK_JS_STRING(in, "Array.prototype.map.call(p, x => x * 2).join()", "2,4,6");
    CHECK_JS_STRING(in, "[...p].join()", "1,2,3");
    CHECK_JS_STRING(in, "Object.keys(p).join()", "0,1,2");
    // A write through the proxy reaches the array's own length.
    CHECK_JS_NUMBER(in, "(p[3] = 4, array.length)", 4);
    CHECK_JS_NUMBER(in, "(p.length = 1, array.length)", 1);
}

void test_a_proxy_as_a_prototype()
{
    ProxyRealm realm;
    js::Interpreter& in = realm.interpreter;
    // The chain walks of [[Get]], [[Set]] and [[HasProperty]] have to hand
    // over to a proxy they meet above the object they started from.
    test::run_js(in, R"JS(
        globalThis.log = [];
        globalThis.proto = new Proxy({ inherited: 'yes' }, {
            get(t, k, r) { log.push('get:' + String(k)); return Reflect.get(t, k, r); },
            has(t, k) { log.push('has:' + String(k)); return Reflect.has(t, k); },
            set(t, k, v, r) { log.push('set:' + String(k)); return Reflect.set(t, k, v, r); },
        });
        globalThis.child = Object.create(proto);
        // This write already goes through the proxy's set trap: the child has
        // no own property of that name, so OrdinarySet walks up to the
        // prototype and hands over to it with the child still the receiver.
        child.own = 'mine';
        globalThis.setupLog = log.join();
        log.length = 0;
    )JS");
    CHECK_JS_STRING(in, "setupLog", "set:own");
    CHECK_JS_TRUE(in, "Object.getOwnPropertyNames(child).includes('own')");
    CHECK_JS_STRING(in, "child.own", "mine");
    CHECK_JS_STRING(in, "log.join()", ""); // an own property never reaches the proxy
    CHECK_JS_STRING(in, "child.inherited", "yes");
    CHECK_JS_STRING(in, "log.join()", "get:inherited");
    test::run_js(in, "log.length = 0;");
    CHECK_JS_TRUE(in, "'inherited' in child");
    CHECK_JS_STRING(in, "log.join()", "has:inherited");
    test::run_js(in, "log.length = 0;");
    CHECK_JS_FALSE(in, "'nothing' in child");
    CHECK_JS_STRING(in, "log.join()", "has:nothing");
    // A write through the chain is the proxy's [[Set]], with the child
    // still the receiver, so the property lands on the child.
    test::run_js(in, "log.length = 0;");
    CHECK_JS_STRING(in, "(child.fresh = 'v', log.join())", "set:fresh");
    CHECK_JS_TRUE(in, "Object.getOwnPropertyNames(child).includes('fresh')");
    CHECK_JS_FALSE(in, "Object.getOwnPropertyNames(proto).includes('fresh')");
    // for-in walks the chain through [[GetPrototypeOf]] and [[OwnPropertyKeys]].
    CHECK_JS_TRUE(in, "(() => { const k = []; for (const n in child) k.push(n); return k.includes('inherited'); })()");
    // instanceof walks it too.
    test::run_js(in, R"JS(
        globalThis.Base = function () {};
        globalThis.shadow = {};
        Base.prototype = new Proxy(shadow, {});
        globalThis.instance = Object.create(shadow);
    )JS");
    CHECK_JS_TRUE(in, "Object.create(Base.prototype) instanceof Base");
    CHECK_JS_FALSE(in, "({}) instanceof Base");
    // A getPrototypeOf trap decides what instanceof sees.
    CHECK_JS_TRUE(in,
        "(function () { const q = new Proxy({}, { getPrototypeOf() { return Base.prototype; } });"
        " return q instanceof Base; })()");
}

void test_a_callable_and_a_constructible_proxy()
{
    ProxyRealm realm;
    js::Interpreter& in = realm.interpreter;
    test::run_js(in, R"JS(
        globalThis.plain = new Proxy({}, {});
        globalThis.fn = new Proxy(function (a, b) { return a + b; }, {});
        globalThis.arrow = new Proxy(x => x, {});
        globalThis.klass = new Proxy(class { constructor(v) { this.v = v; } }, {});
    )JS");
    // typeof answers for the target's callability…
    CHECK_JS_STRING(in, "typeof plain", "object");
    CHECK_JS_STRING(in, "typeof fn", "function");
    CHECK_JS_STRING(in, "typeof arrow", "function");
    CHECK_JS_STRING(in, "typeof klass", "function");
    // …and so does every place a call can be made.
    CHECK_JS_NUMBER(in, "fn(1, 2)", 3);
    CHECK_JS_NUMBER(in, "fn.call(null, 1, 2)", 3);
    CHECK_JS_NUMBER(in, "fn.apply(null, [1, 2])", 3);
    CHECK_JS_NUMBER(in, "Reflect.apply(fn, null, [1, 2])", 3);
    CHECK_JS_NUMBER(in, "[1, 2].map(new Proxy(x => x * 3, {})).join(''), [3].map(x => x)[0]", 3);
    CHECK_JS_NUMBER(in, "fn.bind(null, 1)(2)", 3);
    // BoundFunctionCreate (§10.4.1.3 step 1) takes the bound function's
    // [[Prototype]] from the target's [[GetPrototypeOf]] — a proxy's trap,
    // or its target's prototype — so a bound proxy inherits from
    // Function.prototype like any other bound function.
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(fn.bind(null)) === Function.prototype");
    CHECK_JS_STRING(in, "typeof fn.bind(null).call", "function");
    CHECK_JS_NUMBER(in, "fn.bind(null, 1).call(null, 2)", 3);
    CHECK_JS_NUMBER(in, "fn.bind(null).length", 2);
    CHECK_JS_TRUE(in,
        "(function () { const marker = { mine: true };"
        " const q = new Proxy(function () {}, { getPrototypeOf() { return marker; } });"
        " return Object.getPrototypeOf(q.bind(null)) === marker; })()");
    CHECK_JS_THROWS(in,
        "(new Proxy(function () {}, { getPrototypeOf() { throw new RangeError('no'); } })).bind(null)", "RangeError");
    CHECK_JS_NUMBER(in, "new klass(4).v", 4);
    CHECK_JS_NUMBER(in, "Reflect.construct(klass, [5]).v", 5);
    // A proxy of an arrow function is callable but not constructible, and a
    // proxy of a plain object is neither.
    CHECK_JS_THROWS(in, "new arrow()", "TypeError");
    CHECK_JS_THROWS(in, "plain()", "TypeError");
    CHECK_JS_THROWS(in, "new plain()", "TypeError");
    // Callability is fixed when the proxy is made, so a revoked one still
    // answers "function" — and throws.
    CHECK_JS_FALSE(in, "Reflect.has(plain, 'call')");
    // An apply trap on a non-callable target can never be reached.
    CHECK_JS_THROWS(in, "new Proxy({}, { apply() { return 1; } })()", "TypeError");
}

void test_both_execution_tiers_agree()
{
    ProxyRealm realm;
    js::Interpreter& in = realm.interpreter;
    // A generator's body compiles to bytecode and runs on the VM, while a
    // plain function's runs on the tree-walker. The same operations over
    // the same proxy go through each, and the two answers must match.
    test::run_js(in, R"JS(
        globalThis.log = [];
        globalThis.make = () => new Proxy({ a: 1, b: 2 }, {
            get(t, k, r) { log.push('get:' + String(k)); return Reflect.get(t, k, r); },
            has(t, k) { log.push('has:' + String(k)); return Reflect.has(t, k); },
            set(t, k, v, r) { log.push('set:' + String(k)); return Reflect.set(t, k, v, r); },
            deleteProperty(t, k) { log.push('delete:' + String(k)); return Reflect.deleteProperty(t, k); },
            ownKeys(t) { log.push('ownKeys'); return Reflect.ownKeys(t); },
        });
        // Every proxy-touching operation, written once.
        globalThis.body = function (p) {
            const out = [];
            out.push(p.a);
            out.push('b' in p);
            out.push(delete p.b);
            p.c = 3;
            out.push(p.c);
            const keys = [];
            for (const k in p) keys.push(k);
            out.push(keys.join('+'));
            out.push(Object.keys(p).join('+'));
            out.push(JSON.stringify(p));
            const { a, ...rest } = p;
            out.push(a + ':' + JSON.stringify(rest));
            out.push([...Object.values(p)].join('+'));
            return out.join('|');
        };
        // The tree-walker: an ordinary call.
        globalThis.walker = (function () { log.length = 0; const r = body(make()); return [r, log.join()]; })();
        // The bytecode tier: the identical statements inside a generator
        // body, which the compiler takes at its first call.
        globalThis.gen = function* (p) {
            const out = [];
            out.push(p.a);
            out.push('b' in p);
            out.push(delete p.b);
            p.c = 3;
            out.push(p.c);
            const keys = [];
            for (const k in p) keys.push(k);
            out.push(keys.join('+'));
            out.push(Object.keys(p).join('+'));
            out.push(JSON.stringify(p));
            const { a, ...rest } = p;
            out.push(a + ':' + JSON.stringify(rest));
            out.push([...Object.values(p)].join('+'));
            yield out.join('|');
        };
        globalThis.vm = (function () { log.length = 0; const r = gen(make()).next().value; return [r, log.join()]; })();
    )JS");

    // The answer itself, spelled out: if either tier drifts, this says how.
    CHECK_JS_STRING(in, "walker[0]",
        "1|true|true|3|a+c|a+c|{\"a\":1,\"c\":3}|1:{\"c\":3}|1+3");
    CHECK_JS_STRING(in, "vm[0] === walker[0] ? 'same' : 'VM ' + vm[0] + ' WALKER ' + walker[0]", "same");
    // And the trap sequence, so neither tier reached a fast path that
    // skipped the handler altogether.
    CHECK_JS_STRING(in, "vm[1] === walker[1] ? 'same' : 'VM ' + vm[1] + ' WALKER ' + walker[1]", "same");
    CHECK_JS_TRUE(in, "walker[1].split(',').length > 10");
    CHECK_JS_TRUE(in, "walker[1].startsWith('get:a,has:b,delete:b,set:c')");

    // A throw out of a trap unwinds the VM's frame the same way.
    test::run_js(in, R"JS(
        globalThis.boom = new Proxy({}, { get() { throw new RangeError('boom'); } });
        globalThis.caught = function* () { try { yield boom.x; } catch (e) { yield e.constructor.name; } };
    )JS");
    CHECK_JS_STRING(in, "caught().next().value", "RangeError");
    CHECK_JS_THROWS(in, "(function* () { yield boom.x; })().next()", "RangeError");
}

void test_a_proxy_in_the_places_that_switch_on_object_kind()
{
    ProxyRealm realm;
    js::Interpreter& in = realm.interpreter;
    test::run_js(in, "globalThis.p = new Proxy({ a: 1, b: 2 }, {});");
    // The reflective built-ins, the ones that ask for keys and descriptors.
    CHECK_JS_STRING(in, "Object.keys(p).join()", "a,b");
    CHECK_JS_STRING(in, "Object.values(p).join()", "1,2");
    CHECK_JS_STRING(in, "JSON.stringify(Object.entries(p))", "[[\"a\",1],[\"b\",2]]");
    CHECK_JS_STRING(in, "JSON.stringify(Object.assign({}, p))", "{\"a\":1,\"b\":2}");
    CHECK_JS_STRING(in, "Object.getOwnPropertyNames(Object.getOwnPropertyDescriptors(p)).join()", "a,b");
    CHECK_JS_TRUE(in, "Object.prototype.hasOwnProperty.call(p, 'a')");
    CHECK_JS_TRUE(in, "Object.prototype.propertyIsEnumerable.call(p, 'a')");
    CHECK_JS_TRUE(in, "Reflect.ownKeys(p).join() === 'a,b'");
    // Spread and destructuring, object and array forms.
    CHECK_JS_STRING(in, "JSON.stringify({ ...p })", "{\"a\":1,\"b\":2}");
    CHECK_JS_NUMBER(in, "(({ a }) => a)(p)", 1);
    CHECK_JS_STRING(in, "(({ a, ...r }) => JSON.stringify(r))(p)", "{\"b\":2}");
    // freeze and seal go through every one of them at once.
    test::run_js(in, "globalThis.q = new Proxy({ x: 1 }, {});");
    CHECK_JS_TRUE(in, "(Object.freeze(q), Object.isFrozen(q))");
    CHECK_JS_TRUE(in, "Object.isSealed(q)");
    CHECK_JS_FALSE(in, "Object.isExtensible(q)");
    CHECK_JS_FALSE(in, "Object.isFrozen(new Proxy({ y: 1 }, {}))");
    // A symbol key a trap invents survives being handed back: the keys are
    // read, then used across allocations, under heap stress.
    CHECK_JS_NUMBER(in,
        "(function () { const s = Symbol('fresh');"
        " const r = new Proxy({}, { ownKeys() { return [s]; },"
        " getOwnPropertyDescriptor() { return { value: 1, enumerable: true, configurable: true }; } });"
        " return Object.getOwnPropertySymbols(r).length; })()",
        1);
    // A `with` over a proxy consults the has trap for every name it is
    // asked about, and a throw from it is the statement's outcome.
    CHECK_JS_STRING(in,
        "(function () { const seen = [];"
        " const w = new Proxy({ inside: 'found' }, { has(t, k) { seen.push(String(k)); return k in t; } });"
        " let got; with (w) { got = inside; } return got + ':' + seen.includes('inside'); })()",
        "found:true");
    CHECK_JS_THROWS(in,
        "(function () { const w = new Proxy({}, { has() { throw new RangeError('no'); } });"
        " with (w) { return anything; } })()",
        "RangeError");
    // InstallErrorCause (§20.5.8.1) asks HasProperty of the options object,
    // which over a proxy is the has trap — for AggregateError as much as
    // for the other errors.
    CHECK_JS_NUMBER(in, "new Error('m', new Proxy({}, { has: () => true, get: () => 42 })).cause", 42);
    CHECK_JS_NUMBER(in, "new RangeError('m', new Proxy({}, { has: () => true, get: () => 42 })).cause", 42);
    CHECK_JS_NUMBER(in, "new AggregateError([], 'm', new Proxy({}, { has: () => true, get: () => 42 })).cause", 42);
    CHECK_JS_THROWS(in,
        "new AggregateError([], 'm', new Proxy({}, { has() { throw new RangeError('no'); } }))", "RangeError");
    // The three reasons a new prototype is refused, each named for what it
    // is: the ordinary object's two (§10.1.2.1 steps 3 and 8), and a
    // proxy's, which is its trap's answer and neither of the others.
    CHECK_JS_THROWS(in, "(function () { const o = {}; const p = Object.create(o); Object.setPrototypeOf(o, p); })()",
        "TypeError: Cyclic __proto__ value");
    CHECK_JS_THROWS(in, "(function () { const o = {}; const p = Object.create(o); o.__proto__ = p; })()",
        "TypeError: Cyclic __proto__ value");
    CHECK_JS_THROWS(in, "Object.setPrototypeOf(Object.preventExtensions({}), {})",
        "TypeError: #<Object> is not extensible");
    CHECK_JS_THROWS(in, "(function () { 'use strict'; Object.preventExtensions(globalThis.locked = {}); locked.__proto__ = {}; })()",
        "TypeError: #<Object> is not extensible");
    CHECK_JS_THROWS(in, "Object.setPrototypeOf(new Proxy({}, { setPrototypeOf() { return false; } }), null)",
        "TypeError: 'setPrototypeOf' on proxy: trap returned falsish");
}

void test_a_trap_invented_prototype_survives_the_walk()
{
    ProxyRealm realm;
    js::Interpreter& in = realm.interpreter;
    // for-in walks the chain through [[GetPrototypeOf]] (§14.7.5.9), which
    // over a proxy is a trap free to answer with an object nothing else
    // holds. The walk keeps that object across the loop body — which
    // allocates — so it has to be rooted where the walk stores it, not
    // merely where the loop started. Both tiers run the same loop here:
    // the VM's enumerator is a traced cell, and the tree-walker's has to
    // be one too.
    test::run_js(in, R"JS(
        globalThis.make = () => new Proxy({}, {
            ownKeys() { return []; },
            getPrototypeOf() {
                return Object.create(null, {
                    z: { value: 1, enumerable: true },
                    y: { value: 2, enumerable: true },
                    x: { value: 3, enumerable: true },
                });
            },
        });
        globalThis.walk = function (p) { const keys = []; for (const k in p) keys.push(k); return keys.join(); };
        globalThis.spin = function* (p) { const keys = []; for (const k in p) keys.push(k); yield keys.join(); };
    )JS");
    // The keys are the observable: the enumeration takes them from the
    // object the trap answered with, while reading p[k] would go through
    // the proxy's [[Get]] — the target's own chain, which that object is
    // not part of. Pushing each key allocates, which is what sweeps an
    // enumerator that is not traced.
    CHECK_JS_STRING(in, "walk(make())", "z,y,x");
    CHECK_JS_STRING(in, "spin(make()).next().value", "z,y,x");
    // A fresh object at every level of a chain the trap builds, so the
    // walk holds one trap answer while asking for the next.
    test::run_js(in, R"JS(
        globalThis.chain = (depth) => {
            let made = 0;
            const step = () => {
                const level = made++;
                if (level >= depth)
                    return null;
                const proto = Object.create(null, { ['k' + level]: { value: level, enumerable: true } });
                return new Proxy(proto, { getPrototypeOf() { return step(); } });
            };
            return step();
        };
    )JS");
    CHECK_JS_STRING(in, "walk(chain(4))", "k0,k1,k2,k3");
    CHECK_JS_STRING(in, "spin(chain(4)).next().value", "k0,k1,k2,k3");
}

void test_the_array_generics_ask_the_has_trap()
{
    ProxyRealm realm;
    js::Interpreter& in = realm.interpreter;
    // §23.1.3: the generics that skip holes ask HasProperty(O, Pk), which
    // over a proxy is the has trap. A trap that disowns an index hides the
    // element from every one of them, and a trap that throws ends the
    // method — neither can be answered from the target behind the proxy.
    test::run_js(in, R"JS(
        globalThis.seen = [];
        globalThis.hidden = new Proxy({ 0: 'x', 1: 'y', length: 2 }, {
            has(t, k) { seen.push(String(k)); return false; },
            get(t, k) { return t[k]; },
        });
        globalThis.boom = (length) => new Proxy({ 0: 'x', 1: 'y', length },
            { has() { throw new RangeError('no'); }, get(t, k) { return t[k]; } });
    )JS");
    CHECK_JS_STRING(in,
        "(function () { const out = []; Array.prototype.forEach.call(hidden, v => out.push(v)); return out.join(); })()", "");
    CHECK_JS_STRING(in, "seen.join()", "0,1");
    CHECK_JS_NUMBER(in, "Array.prototype.indexOf.call(hidden, 'x')", -1);
    CHECK_JS_NUMBER(in, "Array.prototype.lastIndexOf.call(hidden, 'y')", -1);
    CHECK_JS_STRING(in, "Array.prototype.filter.call(hidden, () => true).join()", "");
    CHECK_JS_TRUE(in, "Array.prototype.every.call(hidden, () => false)");
    CHECK_JS_FALSE(in, "Array.prototype.some.call(hidden, () => true)");
    CHECK_JS_STRING(in, "Object.keys(Array.prototype.map.call(hidden, v => v)).join()", "");
    CHECK_JS_NUMBER(in, "Array.prototype.map.call(hidden, v => v).length", 2);
    // slice and concat step their destination index whether the key was
    // there or not (§23.1.3.25 step 8.e, §23.1.3.1 step 5.c.iv.4), so a
    // disowned index leaves a hole rather than closing the gap: two holes
    // join as one comma, and the array has no own index keys at all.
    CHECK_JS_STRING(in, "Array.prototype.slice.call(hidden, 0).join()", ",");
    CHECK_JS_STRING(in, "Object.keys(Array.prototype.slice.call(hidden, 0)).join()", "");
    CHECK_JS_STRING(in, "[].concat(new Proxy([1, 2], { has: () => false })).join()", ",");
    CHECK_JS_NUMBER(in, "[].concat(new Proxy([1, 2], { has: () => false })).length", 2);
    CHECK_JS_STRING(in, "Object.keys([].concat(new Proxy([1, 2], { has: () => false }))).join()", "");
    // A throw out of the trap is the method's outcome, at every site that
    // asks — the visitors, the reducers, the searches and the movers.
    CHECK_JS_THROWS(in, "Array.prototype.forEach.call(boom(2), () => {})", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.reduce.call(boom(2), (a, b) => a)", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.reduce.call(boom(2), (a, b) => a, 0)", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.indexOf.call(boom(2), 'x')", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.lastIndexOf.call(boom(2), 'x')", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.sort.call(boom(2))", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.reverse.call(boom(2))", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.shift.call(boom(2))", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.unshift.call(boom(2), 'z')", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.splice.call(boom(2), 0, 1)", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.splice.call(boom(2), 0, 0, 'z')", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.copyWithin.call(boom(2), 0, 1)", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.slice.call(boom(2), 0)", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.flat.call(boom(2))", "RangeError");
    CHECK_JS_THROWS(in, "Array.prototype.flatMap.call(boom(2), v => v)", "RangeError");
    CHECK_JS_THROWS(in, "[].concat(new Proxy([1, 2], { has() { throw new RangeError('no'); } }))", "RangeError");
}

void test_a_deep_chain_of_proxies_is_a_range_error()
{
    // How deeply proxies nest is the script's to choose, and a handler
    // with no trap answers by asking its target the same question one C++
    // frame deeper. So the fall-through is recursion over script-built
    // data, and it is held to the same stack budget as any other: a
    // RangeError, never a crashed process. No heap stress here — building
    // the chain is the point, not what it allocates.
    js::Interpreter in;
    test::run_js(in, R"JS(
        globalThis.nest = (n) => { let p = { x: 1 }; for (let i = 0; i < n; i++) p = new Proxy(p, {}); return p; };
        globalThis.shallow = nest(20);
        globalThis.deep = nest(30000);
    )JS");
    // The positive control: the same operations over a chain that fits.
    CHECK_JS_NUMBER(in, "shallow.x", 1);
    CHECK_JS_TRUE(in, "'x' in shallow");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(shallow) === Object.prototype");
    CHECK_JS_STRING(in, "Object.keys(shallow).join()", "x");
    CHECK_JS_THROWS(in, "deep.x", "RangeError");
    CHECK_JS_THROWS(in, "'x' in deep", "RangeError");
    CHECK_JS_THROWS(in, "Object.getPrototypeOf(deep)", "RangeError");
    CHECK_JS_THROWS(in, "Object.keys(deep)", "RangeError");
    CHECK_JS_THROWS(in, "deep.x = 2", "RangeError");
    CHECK_JS_THROWS(in, "delete deep.x", "RangeError");
    // And the realm is still usable afterwards: the limit unwound rather
    // than left the interpreter counting a depth it never returned from.
    CHECK_JS_NUMBER(in, "shallow.x", 1);
}

} // namespace

int main()
{
    test_every_trap_runs_and_is_handed_the_right_arguments();
    test_absent_traps_fall_through_to_the_target();
    test_revocation();
    test_invariant_violations_are_type_errors();
    test_a_proxy_of_an_array();
    test_a_proxy_as_a_prototype();
    test_a_callable_and_a_constructible_proxy();
    test_both_execution_tiers_agree();
    test_a_proxy_in_the_places_that_switch_on_object_kind();
    test_a_trap_invented_prototype_survives_the_walk();
    test_the_array_generics_ask_the_has_trap();
    test_a_deep_chain_of_proxies_is_a_range_error();
    return sashfold::test::report("js_proxy");
}
