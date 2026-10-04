#include "JsTest.h"

#include "js/Runtime.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// What is made at first use and not up front. A built-in method or accessor
// is its property from the start — its key, its attributes, its place in
// the order — and its function object is made when something first asks: a
// script that reads it, a descriptor, a definition over it. It is then one
// function, the same at every later read, of the realm it was defined in
// whichever realm asked. Overwritten, redefined or deleted before anyone
// asked, it is never made. A function's own `length`, `name` and
// `prototype` are owed in the same way: there when looked for, in the
// standard's order however they were asked, gone when deleted, and a
// closure nobody inspects never gets a `prototype` object at all. With
// SASHFOLD_LAZY=0 (set_lazy_natives(false)) everything is made at once, and
// nothing a script can read differs between the two.

using namespace sashfold;

namespace {

bool is_lazy(js::Interpreter& in, js::Object* object, std::string_view name)
{
    js::Property const* const property = object->peek_own(in.key(name));
    return property != nullptr && property->lazy;
}

// What a script can see of the objects a realm is born with, and of each
// kind of function: every key list, joined.
constexpr std::string_view shape_script = R"JS(
(function () {
    function names(o) { return Object.getOwnPropertyNames(o).join(); }
    function f(a, b) {}
    var arrow = (a) => a;
    function* gen() {}
    async function later() {}
    class C { constructor(a) {} static s() {} m() {} }
    class D { static name() {} }
    var out = [];
    var objects = [Object, Object.prototype, Function.prototype, Array, Array.prototype, String.prototype, Number, Math, JSON, Reflect,
        Promise, Promise.prototype, Map.prototype, Set.prototype, Date.prototype, RegExp.prototype, Symbol, Error.prototype,
        ArrayBuffer.prototype, DataView.prototype, Intl, globalThis,
        f, arrow, gen, later, C, D, C.prototype.m, f.bind(null, 1), Math.max, Array.prototype.map,
        Object.getOwnPropertyDescriptor(Map.prototype, 'size').get];
    for (var i = 0; i < objects.length; i++)
        out.push(names(objects[i]));
    return out.join('|');
})()
)JS";

std::string shape_of(js::Interpreter& in)
{
    return test::eval_string(in, shape_script);
}

}

int main()
{
    // Whatever the environment says: each part below names its mode.
    js::set_lazy_natives(true);

    // --- A method is its property until something asks for the function.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        js::Object* const array_prototype = in.intrinsics().array_prototype;
        js::Heap::LazyCensus const& census = in.heap().lazy_census();
        CHECK(is_lazy(in, array_prototype, "map"));
        CHECK(census.natives_described > 300); // the path is reached: most built-ins are descriptions
        std::uint64_t const made_before = census.natives_made_later;
        // The property is there before the function is.
        CHECK_JS_TRUE(in, "Array.prototype.hasOwnProperty('map')");
        CHECK_JS_TRUE(in, "'map' in Array.prototype");
        CHECK(is_lazy(in, array_prototype, "map") || census.natives_made_later > made_before); // hasOwnProperty may make it
        // Read, it is made once and is the same function ever after.
        CHECK_JS_TRUE(in, "Array.prototype.map === Array.prototype.map");
        CHECK(!is_lazy(in, array_prototype, "map"));
        std::uint64_t const made_after = census.natives_made_later;
        CHECK(made_after > made_before);
        CHECK_JS_TRUE(in, "[1, 2].map(function (x) { return x * 2; })[1] === 4");
        CHECK_JS_TRUE(in, "var kept = Array.prototype.map; kept === [].map");
        CHECK_JS_STRING(in, "Array.prototype.map.name", "map");
        CHECK_JS_NUMBER(in, "Array.prototype.map.length", 1);
        CHECK_JS_STRING(in, "Object.getOwnPropertyNames(Array.prototype.map).join()", "length,name");
        CHECK_JS_STRING(in, "typeof Array.prototype.filter", "function");
        // A neighbour nobody asked for is still a description.
        CHECK(is_lazy(in, array_prototype, "findLastIndex"));

        // An accessor's functions are made together and stay the same.
        CHECK(is_lazy(in, in.intrinsics().map_prototype, "size"));
        CHECK_JS_TRUE(in, "Object.getOwnPropertyDescriptor(Map.prototype, 'size').get === Object.getOwnPropertyDescriptor(Map.prototype, 'size').get");
        CHECK_JS_STRING(in, "Object.getOwnPropertyDescriptor(Map.prototype, 'size').get.name", "get size");
        CHECK_JS_NUMBER(in, "Object.getOwnPropertyDescriptor(Map.prototype, 'size').get.length", 0);
        CHECK_JS_TRUE(in, "Object.getOwnPropertyDescriptor(Map.prototype, 'size').set === undefined");
        CHECK_JS_NUMBER(in, "new Map([[1, 2]]).size", 1);

        // One function under two keys is one function.
        CHECK_JS_TRUE(in, "Array.prototype.values === Array.prototype[Symbol.iterator]");
        CHECK_JS_TRUE(in, "Number.parseFloat === parseFloat && Number.parseInt === parseInt");
        CHECK_JS_TRUE(in, "String.prototype.trimRight === String.prototype.trimEnd");
        CHECK_JS_TRUE(in, "Set.prototype.keys === Set.prototype.values");

        // Overwritten, redefined or deleted before anyone had asked.
        CHECK(is_lazy(in, array_prototype, "fill"));
        CHECK_JS_TRUE(in, "Array.prototype.fill = 1; Array.prototype.fill === 1");
        CHECK(!is_lazy(in, array_prototype, "fill"));
        CHECK(is_lazy(in, array_prototype, "findLast"));
        CHECK_JS_TRUE(in, "delete Array.prototype.findLast");
        CHECK_JS_FALSE(in, "'findLast' in Array.prototype");
        CHECK_JS_TRUE(in, "[].findLast === undefined");
        CHECK(is_lazy(in, array_prototype, "copyWithin"));
        CHECK_JS_TRUE(in, "Object.defineProperty(Array.prototype, 'copyWithin', { value: 2 }); Array.prototype.copyWithin === 2");
        // The attributes it was defined with are the ones it keeps.
        CHECK_JS_STRING(in, "JSON.stringify(Object.getOwnPropertyDescriptor(Array.prototype, 'copyWithin'))",
            "{\"value\":2,\"writable\":true,\"enumerable\":false,\"configurable\":true}");
        CHECK_JS_STRING(in, "(function () { var d = Object.getOwnPropertyDescriptor(Array.prototype, 'flat');"
                            " return [typeof d.value, d.writable, d.enumerable, d.configurable].join(); })()",
            "function,true,false,true");
        // Put over by the engine itself, a description is dropped unmade.
        {
            std::uint64_t const before_put = census.natives_made_later;
            CHECK(is_lazy(in, array_prototype, "lastIndexOf"));
            array_prototype->put(in.key("lastIndexOf"), js::Value::number(5), js::builtin_attributes);
            CHECK(!is_lazy(in, array_prototype, "lastIndexOf"));
            CHECK(is_lazy(in, array_prototype, "reduceRight"));
            CHECK(array_prototype->remove_own(in.key("reduceRight")));
            CHECK(array_prototype->peek_own(in.key("reduceRight")) == nullptr);
            CHECK_EQ(census.natives_made_later, before_put);
            CHECK_JS_TRUE(in, "Array.prototype.lastIndexOf === 5 && !('reduceRight' in Array.prototype)");
        }

        // A collection between the definition and the first use.
        in.heap().collect();
        CHECK(is_lazy(in, array_prototype, "toSorted"));
        CHECK_JS_TRUE(in, "[3, 1, 2].toSorted().join() === '1,2,3'");
    }

    // --- A function made at first touch is its object's realm's.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        js::RealmRecord* const a = in.current_realm();
        js::RealmRecord* const b = in.create_realm();
        js::Object* const b_array_prototype = b->intrinsics.array_prototype;
        CHECK(is_lazy(in, b_array_prototype, "includes"));
        CHECK(in.current_realm() == a);
        // Asked for from realm A, with A current.
        js::Property const* const made = b_array_prototype->find_own(in.key("includes"));
        CHECK(made != nullptr && made->value.is_object());
        if (made != nullptr && made->value.is_object()) {
            auto* const function = static_cast<js::Function*>(made->value.as_object());
            CHECK(function->realm() == b);
            CHECK(function->prototype() == b->intrinsics.function_prototype);
            CHECK(function->prototype() != a->intrinsics.function_prototype);
        }
        // An accessor likewise.
        js::Property const* const size = b->intrinsics.set_prototype->find_own(in.key("size"));
        CHECK(size != nullptr && size->accessor && size->getter != nullptr);
        if (size != nullptr && size->getter != nullptr)
            CHECK(static_cast<js::Function*>(size->getter)->realm() == b);

        // An object that outlives its realm's hold still makes its
        // functions: what is described keeps its realm alive.
        js::RealmRecord* const c = in.create_realm();
        js::Interpreter::Roots const roots(in);
        js::Object* const c_string_prototype = c->intrinsics.string_prototype;
        in.root(js::Value::object(c_string_prototype));
        in.release_realm(c);
        in.heap().collect();
        CHECK(is_lazy(in, c_string_prototype, "padStart"));
        js::Property const* const pad = c_string_prototype->find_own(in.key("padStart"));
        CHECK(pad != nullptr && pad->value.is_object());
        if (pad != nullptr && pad->value.is_object()) {
            js::Value const function = in.root(pad->value);
            js::Value const star = in.root(js::Value::string(in.string(std::string_view("*"))));
            js::Value const receiver = in.root(js::Value::string(in.string(std::string_view("a"))));
            js::Value const arguments[] = { js::Value::number(3), star };
            std::optional<js::Value> const padded = in.call(function, receiver, arguments);
            CHECK(padded.has_value() && padded->is_string() && padded->as_string()->to_utf8() == "**a");
            // It is the released realm's own function all the same.
            CHECK(static_cast<js::Function*>(function.as_object())->realm() == c);
        }
    }

    // --- A function's own length, name and prototype.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        js::Heap::LazyCensus const& census = in.heap().lazy_census();
        // The order is the standard's whichever is asked first.
        CHECK_JS_STRING(in, "function f1(a, b) {} Object.getOwnPropertyNames(f1).join()", "length,name,prototype");
        CHECK_JS_STRING(in, "function f2(a, b) {} f2.prototype; Object.getOwnPropertyNames(f2).join()", "length,name,prototype");
        CHECK_JS_STRING(in, "function f3(a, b) {} f3.name; f3.prototype; f3.length; Object.getOwnPropertyNames(f3).join()", "length,name,prototype");
        CHECK_JS_STRING(in, "function f4(a, b) {} f4.extra = 1; f4.prototype; Object.getOwnPropertyNames(f4).join()", "length,name,prototype,extra");
        CHECK_JS_STRING(in, "function f5(a, b) {} f5.extra = 1; f5.name; Object.keys(f5).join() + ':' + Object.getOwnPropertyNames(f5).join()",
            "extra:length,name,prototype,extra");
        CHECK_JS_STRING(in, "Reflect.ownKeys(function (a) {}).join()", "length,name,prototype");
        // Their values and attributes.
        CHECK_JS_NUMBER(in, "f1.length", 2);
        CHECK_JS_STRING(in, "f1.name", "f1");
        CHECK_JS_STRING(in, "(function () {}).name", "");
        CHECK_JS_STRING(in, "JSON.stringify(Object.getOwnPropertyDescriptor(f1, 'length'))",
            "{\"value\":2,\"writable\":false,\"enumerable\":false,\"configurable\":true}");
        CHECK_JS_STRING(in, "JSON.stringify(Object.getOwnPropertyDescriptor(f1, 'name'))",
            "{\"value\":\"f1\",\"writable\":false,\"enumerable\":false,\"configurable\":true}");
        CHECK_JS_STRING(in, "(function () { var d = Object.getOwnPropertyDescriptor(f1, 'prototype');"
                            " return [typeof d.value, d.writable, d.enumerable, d.configurable].join(); })()",
            "object,true,false,false");
        CHECK_JS_TRUE(in, "f1.hasOwnProperty('prototype') && f1.hasOwnProperty('name') && f1.hasOwnProperty('length')");
        CHECK_JS_TRUE(in, "f1.prototype.constructor === f1 && f1.prototype === f1.prototype");
        CHECK_JS_TRUE(in, "Object.getPrototypeOf(f1.prototype) === Object.prototype");
        // `new` reads the same object a script would.
        CHECK_JS_TRUE(in, "function g1() {} var made = new g1(); Object.getPrototypeOf(made) === g1.prototype && made instanceof g1");
        CHECK_JS_TRUE(in, "function g2() {} var other = { tag: 1 }; g2.prototype = other; Object.getPrototypeOf(new g2()) === other");
        CHECK_JS_TRUE(in, "function g3() {} g3.prototype.m = function () { return 7; }; new g3().m() === 7");
        // Deleted before anyone asked, they are gone and do not come back.
        CHECK_JS_STRING(in, "function d1(a) {} delete d1.name; Object.getOwnPropertyNames(d1).join()", "length,prototype");
        CHECK_JS_TRUE(in, "d1.hasOwnProperty('name') === false && d1.name === ''");
        CHECK_JS_TRUE(in, "function d2(a, b, c) {} delete d2.length && d2.length === 0 && !d2.hasOwnProperty('length')");
        CHECK_JS_TRUE(in, "function d3() {} delete d3.prototype === false && typeof d3.prototype === 'object'");
        CHECK_JS_THROWS(in, "'use strict'; function d4() {} delete d4.prototype;", "TypeError");
        // Added again, a deleted one goes to the end like any other.
        CHECK_JS_STRING(in, "function d5(a) {} delete d5.name; Object.defineProperty(d5, 'name', { value: 'again', configurable: true });"
                            " Object.getOwnPropertyNames(d5).join()",
            "length,prototype,name");
        // Redefined before anyone asked: in its place, with the new value.
        CHECK_JS_STRING(in, "function r1(a) {} Object.defineProperty(r1, 'name', { value: 'renamed' }); r1.name + ':' + Object.getOwnPropertyNames(r1).join()",
            "renamed:length,name,prototype");
        // Assignment to a read-only one is refused as before.
        CHECK_JS_TRUE(in, "function r2(a) {} r2.name = 'no'; r2.length = 9; r2.name === 'r2' && r2.length === 1");
        // The kinds of function.
        CHECK_JS_STRING(in, "Object.getOwnPropertyNames((a) => a).join()", "length,name");
        CHECK_JS_STRING(in, "Object.getOwnPropertyNames({ m(a, b) {} }.m).join()", "length,name");
        CHECK_JS_STRING(in, "Object.getOwnPropertyNames(async function later() {}).join()", "length,name");
        CHECK_JS_STRING(in, "function* gen() {} Object.getOwnPropertyNames(gen).join()", "length,name,prototype");
        CHECK_JS_TRUE(in, "Object.getOwnPropertyNames(gen.prototype).length === 0"
                          " && Object.getPrototypeOf(gen.prototype) === Object.getPrototypeOf(function* () {}).prototype");
        CHECK_JS_STRING(in, "(function () { var d = Object.getOwnPropertyDescriptor(gen, 'prototype');"
                            " return [d.writable, d.enumerable, d.configurable].join(); })()",
            "true,false,false");
        CHECK_JS_STRING(in, "class C1 { constructor(a) {} static s() {} } Object.getOwnPropertyNames(C1).join()", "length,name,prototype,s");
        CHECK_JS_STRING(in, "class C2 { static name() { return 1; } } Object.getOwnPropertyNames(C2).join() + ':' + typeof C2.name", "length,name,prototype:function");
        CHECK_JS_STRING(in, "(function () { var d = Object.getOwnPropertyDescriptor(C1, 'prototype');"
                            " return [d.writable, d.enumerable, d.configurable].join(); })()",
            "false,false,false");
        CHECK_JS_STRING(in, "function b1(a, b, c) {} var bound = b1.bind(null, 1); bound.name + ':' + bound.length", "bound b1:2");
        CHECK_JS_STRING(in, "Object.getOwnPropertyNames(Math.max).join() + ':' + Math.max.name + ':' + Math.max.length", "length,name:max:2");
        CHECK_JS_STRING(in, "Object.getOwnPropertyNames(Promise.resolve).join()", "length,name");
        CHECK_JS_STRING(in, "var settle; new Promise(function (resolve) { settle = resolve; }); Object.getOwnPropertyNames(settle).join() + ':' + settle.length",
            "length,name:1");
        CHECK_JS_TRUE(in, "function t1(a) { return a; } t1.toString() === 'function t1(a) { return a; }'");

        // The path is reached: closures that are only called are never
        // asked, and get no prototype object.
        std::uint64_t const functions_before = census.script_functions;
        std::uint64_t const prototypes_before = census.prototypes_asked;
        std::uint64_t const names_before = census.names_or_lengths_asked;
        CHECK_JS_NUMBER(in, "var total = 0; for (var i = 0; i < 1000; i++) { total += (function (x) { return x + 1; })(i); } total", 500500);
        CHECK(census.script_functions - functions_before >= 1000);
        CHECK_EQ(census.prototypes_asked - prototypes_before, std::uint64_t { 0 });
        CHECK_EQ(census.names_or_lengths_asked - names_before, std::uint64_t { 0 });
        // And one that is constructed is asked once.
        CHECK_JS_TRUE(in, "function Once() {} new Once() instanceof Once");
        CHECK(census.prototypes_asked - prototypes_before >= 1);
    }

    // --- Nothing a script can read differs between the two modes.
    {
        std::string lazy_shape;
        std::string eager_shape;
        {
            js::Interpreter in;
            in.heap().set_stress(true);
            // Asked for out of order first, so that an order that followed
            // the asking would show.
            CHECK_JS_TRUE(in, "typeof Array.prototype.sort === 'function' && typeof Array.prototype.at === 'function'"
                              " && typeof Math.trunc === 'function' && typeof Object.values === 'function'"
                              " && typeof Promise.prototype.finally === 'function' && typeof JSON.stringify === 'function'");
            lazy_shape = shape_of(in);
            CHECK(in.heap().lazy_census().natives_described > 300);
        }
        js::set_lazy_natives(false);
        {
            js::Interpreter in;
            in.heap().set_stress(true);
            // The eager mode makes everything as it is defined.
            CHECK(!is_lazy(in, in.intrinsics().array_prototype, "map"));
            CHECK_EQ(in.heap().lazy_census().natives_described, std::uint64_t { 0 });
            eager_shape = shape_of(in);
            CHECK_JS_STRING(in, "function e1(a, b) {} Object.getOwnPropertyNames(e1).join()", "length,name,prototype");
            CHECK_JS_TRUE(in, "Array.prototype.map === Array.prototype.map && [1].map(function (x) { return x; })[0] === 1");
        }
        js::set_lazy_natives(true);
        CHECK(!lazy_shape.empty());
        CHECK_EQ(lazy_shape, eager_shape);
    }

    return test::report("js lazy");
}
