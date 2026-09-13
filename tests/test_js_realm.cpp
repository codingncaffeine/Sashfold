#include "JsTest.h"

#include <cstddef>
#include <optional>
#include <string_view>

// Several realms in one heap. A function carries the realm it was made in:
// called from another realm, a script function makes what it creates and
// throws with its own realm's intrinsics, a native does the same, and a
// generator resumed through another realm's next() runs in its own; each
// call puts its caller's realm back however it ends. A constructor whose
// new.target has no object for a `prototype` takes the default from
// new.target's realm, through bound functions and proxies; another realm's
// %Array% as an array's constructor makes an array of this realm; and
// Symbol.for keeps one registry for every realm.

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

    return test::report("js realm");
}
