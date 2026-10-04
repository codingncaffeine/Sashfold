#include "JsTest.h"

#include "js/Heap.h"
#include "js/Object.h"
#include "js/Shape.h"

#include <cstddef>
#include <string>
#include <string_view>

// Shapes (js/Shape.h): what an object has is its shape, shared by every
// object that came by the same properties in the same order from the same
// prototype, and moved on by a transition when a property is added, takes
// other attributes, or the object stops being extensible. A delete, more
// than 64 properties, a function's statics, being a prototype and holding
// a property not yet made make an object a dictionary of its own. Nothing
// a script can see depends on which: every check of behaviour below is
// made in both kinds of object, and the shapes themselves are compared only
// to show which kind each object is in.

using namespace sashfold;

namespace {

js::Object* named(js::Interpreter& in, std::string_view name)
{
    js::PropertyRef const found = in.global()->find_own(in.key(name));
    return found && found->value.is_object() ? found->value.as_object() : nullptr;
}

js::Shape* shape_of(js::Interpreter& in, std::string_view name)
{
    js::Object* const object = named(in, name);
    return object != nullptr ? object->shape() : nullptr;
}

bool is_dictionary(js::Interpreter& in, std::string_view name)
{
    js::Shape const* const shape = shape_of(in, name);
    return shape != nullptr && shape->is_dictionary();
}

std::size_t shape_cells(js::Heap const& heap)
{
    return heap.cell_count_if([](js::Cell const& cell) { return dynamic_cast<js::Shape const*>(&cell) != nullptr; });
}

}

int main()
{
    // --- One constructor's objects share a shape; the same properties in
    // another order, or from another prototype, are another shape.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        CHECK_JS_TRUE(in, "function P(x, y) { this.x = x; this.y = y; } var p1 = new P(1, 2), p2 = new P(3, 4);"
                          " var q = {}; q.y = 1; q.x = 2; var r = { x: 5, y: 6 }; var l1 = { a: 1, b: 2 }, l2 = { a: 'x', b: null }; true");
        CHECK(shape_of(in, "p1") != nullptr && shape_of(in, "p1") == shape_of(in, "p2"));
        CHECK(shape_of(in, "q") != shape_of(in, "r"));
        CHECK(shape_of(in, "p1") != shape_of(in, "r"));
        CHECK(shape_of(in, "l1") == shape_of(in, "l2"));
        CHECK(!is_dictionary(in, "p1") && !is_dictionary(in, "l1"));
        CHECK_JS_STRING(in, "[p1.x, p1.y, p2.x, p2.y, l2.a, l2.b, r.x, q.y].join()", "1,2,3,4,x,,5,1");
        // An object's slots are its own, whoever shares its shape.
        CHECK_JS_STRING(in, "p1.x = 10; [p1.x, p2.x].join()", "10,3");
        // The prototype an object was made with is its shape's.
        CHECK(shape_of(in, "p1")->prototype() == named(in, "p1")->prototype());
    }

    // --- Attribute changes, freeze, seal and preventExtensions are
    // transitions: a read-only and a writable property never share a shape,
    // and two objects changed the same way do.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        CHECK_JS_TRUE(in, "var w1 = { v: 1 }, w2 = { v: 2 }, w3 = { v: 3 };"
                          " Object.defineProperty(w2, 'v', { writable: false }); Object.defineProperty(w3, 'v', { writable: false }); true");
        CHECK(shape_of(in, "w1") != shape_of(in, "w2"));
        CHECK(shape_of(in, "w2") == shape_of(in, "w3"));
        CHECK(!is_dictionary(in, "w2"));
        CHECK_JS_STRING(in, "w2.v = 9; w1.v = 9; [w1.v, w2.v, w3.v].join()", "9,2,3");
        CHECK_JS_THROWS(in, "'use strict'; w2.v = 5;", "TypeError");
        CHECK_JS_STRING(in, "JSON.stringify(Object.getOwnPropertyDescriptor(w2, 'v'))",
            "{\"value\":2,\"writable\":false,\"enumerable\":true,\"configurable\":true}");
        // Two different changes from one shape are two shapes, and a
        // property added read-only is not the shape of one added writable.
        CHECK_JS_TRUE(in, "var r1 = { v: 1 }, r2 = { v: 1 }; Object.defineProperty(r1, 'v', { writable: false });"
                          " Object.defineProperty(r2, 'v', { enumerable: false }); r1.v = 5; r2.v = 5;"
                          " r1.v === 1 && r2.v === 5 && Object.keys(r1).join() === 'v' && Object.keys(r2).join() === ''");
        CHECK(shape_of(in, "r1") != shape_of(in, "r2"));
        CHECK_JS_TRUE(in, "var a1 = {}; Object.defineProperty(a1, 'k', { value: 1, writable: false, enumerable: true, configurable: true });"
                          " var a2 = {}; a2.k = 1; a2.k = 2; a1.k = 2; a1.k === 1 && a2.k === 2");
        CHECK(shape_of(in, "a1") != shape_of(in, "a2"));
        // Back to writable: the shape of an object never made read-only is
        // not the one it returns to (a transition is not undone), and the
        // property behaves as written.
        CHECK_JS_TRUE(in, "Object.defineProperty(w3, 'v', { writable: true }); w3.v = 4; w3.v === 4");
        // Enumerable and configurable likewise.
        CHECK_JS_TRUE(in, "var e1 = { a: 1, b: 2 }, e2 = { a: 1, b: 2 }; Object.defineProperty(e1, 'a', { enumerable: false });"
                          " Object.defineProperty(e2, 'a', { enumerable: false }); Object.keys(e1).join() === 'b' && Object.keys(e2).join() === 'b'");
        CHECK(shape_of(in, "e1") == shape_of(in, "e2"));
        // A data property made an accessor and back.
        CHECK_JS_TRUE(in, "var g = { a: 1, b: 2 }; Object.defineProperty(g, 'a', { get: function () { return 7; }, configurable: true });"
                          " var once = g.a; Object.defineProperty(g, 'a', { value: 8, writable: true }); once === 7 && g.a === 8 && Object.keys(g).join() === 'a,b'");
        // Freeze, seal and preventExtensions.
        CHECK_JS_TRUE(in, "var f1 = Object.freeze({ a: 1, b: 2 }), f2 = Object.freeze({ a: 3, b: 4 }), f3 = { a: 1, b: 2 }; true");
        CHECK(shape_of(in, "f1") == shape_of(in, "f2"));
        CHECK(shape_of(in, "f1") != shape_of(in, "f3"));
        CHECK(!shape_of(in, "f1")->is_extensible() && shape_of(in, "f3")->is_extensible());
        CHECK_JS_TRUE(in, "f1.a = 9; f1.c = 1; Object.isFrozen(f1) && !Object.isFrozen(f3) && f1.a === 1 && f1.c === undefined");
        CHECK_JS_THROWS(in, "'use strict'; f1.a = 2;", "TypeError");
        CHECK_JS_THROWS(in, "Object.defineProperty(f1, 'a', { value: 5 });", "TypeError");
        CHECK_JS_TRUE(in, "var s1 = Object.seal({ a: 1 }), s2 = Object.seal({ a: 2 }); s1.a = 5; delete s1.a; s1.z = 1;"
                          " Object.isSealed(s1) && !Object.isFrozen(s1) && s1.a === 5 && !('z' in s1)");
        CHECK(shape_of(in, "s1") == shape_of(in, "s2"));
        CHECK_JS_TRUE(in, "var n1 = Object.preventExtensions({ a: 1 }), n2 = Object.preventExtensions({ a: 2 }); n1.b = 1;"
                          " !Object.isExtensible(n1) && !('b' in n1) && delete n1.a && !('a' in n1)");
        CHECK_JS_THROWS(in, "'use strict'; n2.b = 1;", "TypeError");
    }

    // --- A delete makes the object a dictionary of its own, and nothing it
    // does changes: its order, its values, a property added again (at the
    // end), accessors, attributes, freezing, many adds and deletes.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        CHECK_JS_TRUE(in, "var d = { a: 1, b: 2, c: 3 }, e = { a: 1, b: 2, c: 3 }; delete d.b; true");
        CHECK(is_dictionary(in, "d"));
        CHECK(!is_dictionary(in, "e"));
        CHECK_JS_STRING(in, "Object.keys(d).join() + ':' + d.a + d.c + ':' + d.b + ':' + ('b' in d)", "a,c:13:undefined:false");
        CHECK_JS_STRING(in, "Object.keys(e).join() + ':' + e.b", "a,b,c:2");
        CHECK_JS_STRING(in, "d.b = 7; JSON.stringify(d)", "{\"a\":1,\"c\":3,\"b\":7}");
        CHECK_JS_STRING(in, "delete d.a; d.a = 0; var out = []; for (var k in d) out.push(k); out.join()", "c,b,a");
        CHECK_JS_TRUE(in, "var acc = 0; Object.defineProperty(d, 'g', { get: function () { return this.c * 2; }, set: function (v) { acc = v; }, enumerable: true, configurable: true });"
                          " d.g = 11; d.g === 6 && acc === 11");
        CHECK_JS_THROWS(in, "'use strict'; Object.defineProperty(d, 'c', { writable: false }); d.c = 1;", "TypeError");
        CHECK_JS_TRUE(in, "Object.freeze(d); d.z = 1; Object.isFrozen(d) && !('z' in d) && d.c === 3");
        // A dictionary used as a table: slots taken back and given out again.
        CHECK_JS_NUMBER(in, "var t = { seed: 0 }; delete t.seed; var sum = 0; for (var i = 0; i < 2000; i++) { t['k' + i] = i; if (i >= 3) delete t['k' + (i - 3)]; }"
                            " for (var k in t) sum += t[k]; sum + Object.keys(t).length * 100000",
            300000 + 1997 + 1998 + 1999);
        CHECK(is_dictionary(in, "t"));
        CHECK_JS_STRING(in, "Object.keys(t).join()", "k1997,k1998,k1999");
        // Deleting a property no object has is fine, and changes nothing.
        CHECK_JS_TRUE(in, "var u = { a: 1 }; delete u.zz && Object.keys(u).join() === 'a'");
        CHECK(!is_dictionary(in, "u"));
    }

    // --- Key order: indices ascending, then names, then symbols, each in
    // creation order, whatever the object's kind.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        CHECK_JS_STRING(in, "var s1 = Symbol('s1'), s2 = Symbol('s2'); var o = {}; o.b = 1; o[s1] = 1; o[2] = 1; o.a = 1; o[0] = 1; o[s2] = 1; o[1] = 1;"
                            " Reflect.ownKeys(o).map(String).join()",
            "0,1,2,b,a,Symbol(s1),Symbol(s2)");
        CHECK(!is_dictionary(in, "o"));
        CHECK_JS_STRING(in, "delete o.b; o.b = 1; o[10] = 1; Reflect.ownKeys(o).map(String).join()", "0,1,2,10,a,b,Symbol(s1),Symbol(s2)");
        CHECK(is_dictionary(in, "o"));
        CHECK_JS_STRING(in, "var k = { x: 1, y: 2, z: 3 }; Object.defineProperty(k, 'x', { enumerable: false }); Reflect.ownKeys(k).join() + ':' + Object.keys(k).join()",
            "x,y,z:y,z");
        // Past 64 properties an object is a dictionary, in the same order.
        CHECK_JS_TRUE(in, "var wide = {}; for (var i = 0; i < 70; i++) wide['p' + i] = i; var narrow = {}; for (var j = 0; j < 64; j++) narrow['p' + j] = j;"
                          " Object.keys(wide).length === 70 && Object.keys(wide)[69] === 'p69' && Object.keys(wide)[0] === 'p0' && wide.p33 === 33");
        CHECK(is_dictionary(in, "wide"));
        CHECK(!is_dictionary(in, "narrow"));
        CHECK_JS_TRUE(in, "var wide2 = {}; for (var i = 0; i < 70; i++) wide2['p' + i] = i; Object.keys(wide2).join() === Object.keys(wide).join()");
    }

    // --- Functions: those of a kind share their shapes; one given a static
    // is a dictionary; a prototype is a dictionary.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        CHECK_JS_TRUE(in, "function f1(a) {} function f2(b, c) {} f1.length; f2.length; f1.name; f2.name; true");
        CHECK(shape_of(in, "f1") == shape_of(in, "f2"));
        CHECK_JS_STRING(in, "Object.getOwnPropertyNames(f1).join() + ':' + f1.length + f2.length + f1.name + f2.name", "length,name,prototype:12f1f2");
        CHECK_JS_TRUE(in, "function g1() {} g1.helper = 1; class K { static s() { return 3; } } var KK = K; g1.helper === 1 && K.s() === 3");
        CHECK(is_dictionary(in, "g1"));
        CHECK(is_dictionary(in, "KK"));
        CHECK_JS_STRING(in, "Object.getOwnPropertyNames(g1).join() + ':' + Object.getOwnPropertyNames(K).join()", "length,name,prototype,helper:length,name,prototype,s");
        CHECK_JS_TRUE(in, "function Made() {} var m = new Made(); true");
        js::Object* const made = named(in, "m");
        CHECK(made != nullptr && made->prototype() != nullptr && made->prototype()->shape()->is_dictionary());
    }

    // --- A new prototype replays the shape: objects with the same
    // properties moved to one prototype share a shape there too.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        CHECK_JS_TRUE(in, "var base = { hello: function () { return 'hi ' + this.n; } }; var a1 = { n: 1, m: 2 }, a2 = { n: 3, m: 4 };"
                          " Object.setPrototypeOf(a1, base); Object.setPrototypeOf(a2, base); a1.hello() === 'hi 1' && a2.hello() === 'hi 3'");
        CHECK(shape_of(in, "a1") == shape_of(in, "a2"));
        CHECK(!is_dictionary(in, "a1"));
        CHECK(shape_of(in, "a1")->prototype() == named(in, "base"));
        CHECK(is_dictionary(in, "base"));
        CHECK_JS_STRING(in, "Object.keys(a1).join() + a1.n + a1.m", "n,m12");
        CHECK_JS_TRUE(in, "var lit = { __proto__: base, n: 9 }; lit.hello() === 'hi 9'");
        CHECK_JS_TRUE(in, "Object.setPrototypeOf(a1, null); Object.getPrototypeOf(a1) === null && a1.hello === undefined && a1.n === 1");
    }

    // --- The collector: a prototype's root goes with it, a root no object
    // uses goes while the prototype stays, a transition whose shape died
    // goes and is made again.
    {
        js::Interpreter in;
        js::Heap& heap = in.heap();
        js::PropertyKey const x = in.key("x");
        js::PropertyKey const y = in.key("y");
        heap.collect();
        std::size_t const before = shape_cells(heap);
        js::Persistent proto_root(heap);
        js::Persistent child_root(heap);
        {
            js::Heap::NoCollect const guard(heap);
            js::Object* const proto = heap.allocate<js::Object>(nullptr);
            proto_root.set(js::Value::object(proto));
            js::Object* const child = heap.allocate<js::Object>(proto);
            child_root.set(js::Value::object(child));
            child->put(x, js::Value::number(1));
        }
        heap.collect();
        // The prototype is a dictionary, its own (no cell); its children's
        // root and the shape with x are cells.
        CHECK(proto_root.value().as_object()->shape()->is_dictionary());
        CHECK_EQ(shape_cells(heap), before + 2);
        // A root nobody uses goes, with what was made from it.
        child_root.set(js::Value::undefined());
        heap.collect();
        CHECK_EQ(shape_cells(heap), before);
        // Made again when another child is made, and gone with the prototype.
        {
            js::Heap::NoCollect const guard(heap);
            js::Object* const again = heap.allocate<js::Object>(proto_root.value().as_object());
            child_root.set(js::Value::object(again));
            again->put(x, js::Value::number(2));
            again->put(y, js::Value::number(3));
        }
        heap.collect();
        CHECK_EQ(shape_cells(heap), before + 3);
        child_root.set(js::Value::undefined());
        proto_root.set(js::Value::undefined());
        heap.collect();
        CHECK_EQ(shape_cells(heap), before);

        // A transition whose shape died: an object of {x} stays, one of
        // {x, y} goes; {x, y} is made again, as a new shape.
        js::Persistent keep(heap);
        js::Persistent passing(heap);
        {
            js::Heap::NoCollect const guard(heap);
            js::Object* const kept = heap.allocate<js::Object>(nullptr);
            kept->put(x, js::Value::number(1));
            keep.set(js::Value::object(kept));
            js::Object* const gone = heap.allocate<js::Object>(nullptr);
            gone->put(x, js::Value::number(1));
            gone->put(y, js::Value::number(1));
            passing.set(js::Value::object(gone));
        }
        heap.collect();
        std::size_t const both = shape_cells(heap);
        passing.set(js::Value::undefined());
        heap.collect();
        CHECK_EQ(shape_cells(heap), both - 1);
        {
            js::Heap::NoCollect const guard(heap);
            js::Object* const fresh = heap.allocate<js::Object>(nullptr);
            fresh->put(x, js::Value::number(4));
            fresh->put(y, js::Value::number(5));
            passing.set(js::Value::object(fresh));
        }
        heap.collect();
        CHECK_EQ(shape_cells(heap), both);
        CHECK(keep.value().as_object()->shape() == keep.value().as_object()->shape());
        CHECK(passing.value().as_object()->find_own(y)->value == js::Value::number(5));
    }

    // --- Memory: ten thousand objects of one shape add no shapes, and each
    // costs its header and its values.
    {
        js::Interpreter in;
        js::Heap& heap = in.heap();
        // (push is made at its first use: used here, before the count.)
        CHECK_JS_TRUE(in, "function M(i) { this.a = i; this.b = i; } var warm = new M(0); var many = []; many.push(warm); many.length = 0; true");
        heap.collect();
        auto const objects = [&heap] {
            return heap.cell_count_if([](js::Cell const& cell) { return dynamic_cast<js::Object const*>(&cell) != nullptr; });
        };
        std::size_t const shapes_before = shape_cells(heap);
        std::size_t const objects_before = objects();
        CHECK_JS_TRUE(in, "for (var i = 0; i < 10000; i++) many.push(new M(i)); many[9999].b === 9999");
        heap.collect();
        CHECK_EQ(shape_cells(heap), shapes_before);
        CHECK_EQ(objects(), objects_before + 10000);
        // Two values in the object's own room: nothing outside the object.
        js::Object* const one = named(in, "warm");
        CHECK(one != nullptr && one->size_in_bytes() == sizeof(js::Object));
        CHECK(sizeof(js::Object) <= 96);
    }

    // --- Slots past an object's inline room move to a block of their own,
    // values whole, in both kinds of object.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        CHECK_JS_NUMBER(in, "var big = {}; for (var i = 0; i < 40; i++) big['q' + i] = i * 3; var s = 0; for (var k in big) s += big[k]; s", 2340);
        CHECK_JS_TRUE(in, "var four = { a: 1, b: 2, c: 3, d: 4 }, five = { a: 1, b: 2, c: 3, d: 4, e: 5 }; four.d + five.e === 9 && JSON.stringify(five) === '{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5}'");
        CHECK_JS_NUMBER(in, "var bigd = {}; for (var i = 0; i < 40; i++) bigd['q' + i] = i; delete bigd.q0; for (var j = 40; j < 80; j++) bigd['q' + j] = j; var t = 0; for (var k in bigd) t += bigd[k]; t", 3160);
        CHECK(is_dictionary(in, "bigd"));
    }

    // --- Exotic objects' shapes say their own properties are not all their
    // storage's; an ordinary object's do not.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        CHECK_JS_TRUE(in, "var plain = {}, list = [], wrapper = new String('ab'), proxy = new Proxy({}, {}), args = (function () { return arguments; })(1);"
                          " var typed = new Uint8Array(2); true");
        CHECK(!shape_of(in, "plain")->is_uncacheable());
        for (std::string_view const name : { "list", "wrapper", "proxy", "args", "typed" })
            CHECK(shape_of(in, name) != nullptr && shape_of(in, name)->is_uncacheable());
    }

    return test::report("js shapes");
}
