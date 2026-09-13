#include "JsTest.h"

#include "js/Runtime.h"

#include <cstddef>
#include <optional>
#include <string_view>

// Several realms in one heap. A function carries the realm it was made in:
// called from another realm, a script function makes what it creates and
// throws with its own realm's intrinsics, a native does the same, and a
// generator resumed through another realm's next() runs in its own; each
// call puts its caller's realm back however it ends. A constructor whose
// new.target has no object for a `prototype` takes the default from
// new.target's realm, through bound functions and proxies — Function and
// its kin too, while Array.from and Array.of construct `this`; another
// realm's %Array% as an array's constructor makes an array of this realm;
// and Symbol.for keeps one registry for every realm. A class constructor
// called without `new` throws its own realm's error, a derived
// constructor's result is judged in its caller's realm, and
// RegExp.prototype.compile refuses another realm's object. Each realm keeps
// its own module map, and a realm its host lets go is collected.

using namespace sashfold;

namespace {

js::Value global_of(js::Interpreter& in, js::RealmRecord const& realm, std::string_view name)
{
    return in.get(js::Value::object(realm.intrinsics.global), name).value_or(js::Value::undefined());
}

js::Object* prototype_of(js::Value const& value)
{
    return value.is_object() ? value.as_object()->prototype() : nullptr;
}

}

int main()
{
    js::Interpreter in;
    in.heap().set_stress(true);
    js::RealmRecord* const a = in.current_realm();
    js::RealmRecord* const b = in.create_realm();
    CHECK(b != nullptr && b != a);
    CHECK(in.current_realm() == a); // making a realm does not enter it
    CHECK(b->intrinsics.array_prototype != a->intrinsics.array_prototype);
    CHECK(b->intrinsics.global != a->intrinsics.global);

    {
        js::Interpreter::RealmScope const inside(in, b);
        CHECK(in.current_realm() == b);
        test::JsRun const defined = test::run_js(in,
            "function makeArray() { return []; }"
            "function makeError() { return new TypeError('made'); }"
            "function throwIt() { return null.x; }"
            "function* yieldArray() { yield []; }"
            "var generatorB = yieldArray();");
        CHECK(defined.ok);
    }
    CHECK(in.current_realm() == a);
    CHECK_JS_TRUE(in, "typeof makeArray === 'undefined'"); // B's globals are B's

    js::Interpreter::Roots const roots(in);
    std::size_t const type_error = static_cast<std::size_t>(js::ErrorType::TypeError);

    // A script function of B, called from A.
    js::Value const made
        = in.root(in.call(global_of(in, *b, "makeArray"), js::Value::undefined(), {}).value_or(js::Value::undefined()));
    CHECK(prototype_of(made) == b->intrinsics.array_prototype);
    CHECK(in.current_realm() == a);
    js::Value const error
        = in.root(in.call(global_of(in, *b, "makeError"), js::Value::undefined(), {}).value_or(js::Value::undefined()));
    CHECK(prototype_of(error) == b->intrinsics.error_prototypes[type_error]);
    // What an operation inside it throws is B's too, and A is current again
    // after the throw.
    CHECK(!in.call(global_of(in, *b, "throwIt"), js::Value::undefined(), {}).has_value());
    js::Value const thrown = in.root(in.take_exception());
    CHECK(prototype_of(thrown) == b->intrinsics.error_prototypes[type_error]);
    CHECK(in.current_realm() == a);

    // A native of B: B's Array, called from A.
    js::Value const three[1] = { js::Value::number(3) };
    js::Value const native_made = in.root(
        in.call(js::Value::object(b->intrinsics.array_constructor), js::Value::undefined(), three).value_or(js::Value::undefined()));
    CHECK(prototype_of(native_made) == b->intrinsics.array_prototype);
    CHECK(in.current_realm() == a);

    // A generator of B resumed through A's next(): the body runs in B.
    test::JsRun const next_a = test::run_js(in, "Object.getPrototypeOf(Object.getPrototypeOf((function* () {})())).next");
    CHECK(next_a.ok);
    js::Value const next_function = in.root(next_a.value);
    js::Value const step
        = in.root(in.call(next_function, global_of(in, *b, "generatorB"), {}).value_or(js::Value::undefined()));
    CHECK(step.is_object());
    js::Value const yielded = in.root(step.is_object() ? in.get(step, "value").value_or(js::Value::undefined()) : js::Value::undefined());
    CHECK(prototype_of(yielded) == b->intrinsics.array_prototype);
    CHECK(in.current_realm() == a);

    // Script in A reaching B's global as `other`.
    a->intrinsics.global->put(in.key("other"), js::Value::object(b->intrinsics.global));
    CHECK_JS_TRUE(in, "var C = new other.Function(); C.prototype = null;"
                      "Object.getPrototypeOf(Reflect.construct(Array, [], C)) === other.Array.prototype");
    CHECK_JS_TRUE(in, "var D = new other.Function(); D.prototype = null;"
                      "Object.getPrototypeOf(Reflect.construct(Array, [], D.bind())) === other.Array.prototype");
    CHECK_JS_TRUE(in, "var E = new other.Function(); E.prototype = null;"
                      "Object.getPrototypeOf(Reflect.construct(Array, [], new Proxy(E, {}))) === other.Array.prototype");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(Array.prototype.map.call(new other.Array(1, 2), function (x) { return x; })) === Array.prototype");
    CHECK_JS_TRUE(in, "other.Symbol.for('shared') === Symbol.for('shared') && other.Symbol.keyFor(Symbol.for('shared')) === 'shared'");
    CHECK(in.current_realm() == a);

    // A class constructor called without `new` throws its own realm's
    // TypeError; a derived constructor's result is judged back in the
    // caller's context, so those errors are the caller's realm's.
    CHECK_JS_TRUE(in, "var OC = other.eval('(class {})');"
                      "(function () { try { OC(); } catch (e) { return e instanceof other.TypeError; } })()");
    CHECK_JS_TRUE(in, "var DR = other.eval('(class extends Object { constructor() { return null; } })');"
                      "(function () { try { new DR(); } catch (e) { return e instanceof TypeError; } })()");
    CHECK_JS_TRUE(in, "var DU = other.eval('(class extends Object { constructor() {} })');"
                      "(function () { try { new DU(); } catch (e) { return e instanceof ReferenceError; } })()");
    // Function and its kin take the [[Prototype]] from new.target's realm;
    // the function made is the constructor's realm's.
    CHECK_JS_TRUE(in, "var F = new other.Function(); F.prototype = null;"
                      "var made = Reflect.construct(Function, ['return []'], F);"
                      "Object.getPrototypeOf(made) === other.Function.prototype && Object.getPrototypeOf(made()) === Array.prototype");
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(Reflect.construct(Object.getPrototypeOf(function* () {}).constructor, [], F))"
                      " === Object.getPrototypeOf(other.eval('(function* () {})'))");
    // Array.from and Array.of construct `this`.
    CHECK_JS_TRUE(in, "Object.getPrototypeOf(Array.from.call(F, [1])) === other.Object.prototype"
                      " && Object.getPrototypeOf(Array.of.call(F, 1)) === other.Object.prototype");
    CHECK_JS_TRUE(in, "function Pack() {} var packed = Array.of.call(Pack, 'a', 'b');"
                      "packed instanceof Pack && packed.length === 2 && packed[1] === 'b'");
    // RegExp.prototype.compile refuses another realm's object and a
    // subclass's instance.
    CHECK_JS_TRUE(in, "var otherRegExp = new other.RegExp('a');"
                      "(function () { try { RegExp.prototype.compile.call(otherRegExp); } catch (e) { return e instanceof TypeError; } })()"
                      " && otherRegExp.compile('b') === otherRegExp && otherRegExp.source === 'b'");
    CHECK_JS_TRUE(in, "(function () { try { new (class extends RegExp {})('a').compile(); } catch (e) { return e instanceof TypeError; } })()");
    CHECK(in.current_realm() == a);

    // The module map is the realm's: a module parsed with B current is not
    // in A's map.
    {
        js::ModuleRecord* in_b = nullptr;
        {
            js::Interpreter::RealmScope const inside(in, b);
            in_b = in.parse_module(u"export let x = 1;", "realm-module");
            CHECK(in_b != nullptr && in.find_module("realm-module") == in_b);
        }
        CHECK(in_b != nullptr && in.find_module("realm-module") == nullptr);
    }

    // A realm its host lets go is collected once nothing refers to it.
    {
        in.heap().collect();
        std::size_t const before = in.heap().cell_count();
        js::RealmRecord* const released = in.create_realm();
        std::size_t const with_realm = in.heap().cell_count();
        CHECK(released != nullptr && with_realm > before + 100);
        in.release_realm(released);
        in.heap().collect();
        CHECK(in.heap().cell_count() + 100 < with_realm);
    }
    CHECK(in.current_realm() == a);

    // The incumbent realm, the realm of the script code running: a native of B
    // called from A's script sees A, through B's Function.prototype.call too;
    // called from B's script function, B's generator, or B's async function
    // after an await, it sees B.
    {
        js::RealmRecord const* const realm_a = a;
        js::RealmRecord const* const realm_b = b;
        {
            js::Interpreter::RealmScope const inside(in, b);
            js::define_method(in, *b->intrinsics.global, "whoCalls", 0,
                [realm_a, realm_b](js::Interpreter& interp, js::Value const&, std::span<js::Value const>) -> std::optional<js::Value> {
                    js::RealmRecord const* const incumbent = interp.incumbent_realm();
                    std::string_view const name = incumbent == realm_a ? "a" : incumbent == realm_b ? "b" : "?";
                    return js::Value::string(interp.string(name));
                });
        }
        CHECK_JS_TRUE(in, "other.whoCalls() === 'a'");
        CHECK_JS_TRUE(in, "other.whoCalls.call() === 'a'");
        CHECK_JS_TRUE(in, "other.eval('(function () { return whoCalls(); })')() === 'b'");
        CHECK_JS_TRUE(in, "other.eval('(function* () { yield whoCalls(); })')().next().value === 'b'");
        CHECK_JS_TRUE(in, "other.eval('var afterAwait; (async function () { await 0; afterAwait = whoCalls(); })')(); true");
        in.run_jobs([](js::Value const&) {});
        CHECK_JS_TRUE(in, "other.afterAwait === 'b'");
        CHECK(in.incumbent_realm() == a);
    }

    // A realm whose host gives it a [[GlobalThisValue]] of its own: `this` in
    // its global code, in its sloppy functions called with none — from
    // another realm too — and through an indirect eval, and `globalThis`, are
    // that object, while the names global code declares stay on the global
    // object.
    {
        js::RealmRecord* const hosted = in.create_realm();
        js::Object* const stands_for = in.new_object(hosted->intrinsics.object_prototype);
        in.root(js::Value::object(stands_for));
        in.set_global_this(*hosted, stands_for);
        stands_for->put(in.key("marker"), js::Value::number(1));
        a->intrinsics.global->put(in.key("hosted"), js::Value::object(hosted->intrinsics.global));
        {
            js::Interpreter::RealmScope const inside(in, hosted);
            CHECK_JS_TRUE(in, "this.marker === 1");
            CHECK_JS_TRUE(in, "globalThis === this");
            CHECK_JS_TRUE(in, "(function () { return this; })() === globalThis");
            CHECK_JS_TRUE(in, "(0, eval)('this') === globalThis && Function('return this')() === globalThis");
            CHECK_JS_TRUE(in, "(function () { 'use strict'; return this; })() === undefined");
            CHECK_JS_TRUE(in, "var declared = 2; function sloppyThis() { return this; } declared === 2 && globalThis.declared === undefined");
        }
        CHECK(in.current_realm() == a);
        CHECK_JS_TRUE(in, "var calledBare = hosted.sloppyThis; calledBare().marker === 1 && hosted.sloppyThis.call(undefined) === hosted.globalThis");
        CHECK_JS_TRUE(in, "hosted.declared === 2 && globalThis === this && this.hosted === hosted");
    }

    return test::report("js realm");
}
