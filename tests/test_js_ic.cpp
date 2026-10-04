#include "JsTest.h"

#include "js/Heap.h"
#include "js/Object.h"
#include "js/Shape.h"

#include <cstddef>
#include <cstdint>
#include <string>

// The inline caches (js/Feedback.h): a named read or write, and a global
// name, remember what they found for each shape, and a later run of the
// same instruction over an object of that shape takes the answer without a
// lookup. Every check below runs a site past the point it has cached, then
// changes what the answer was made from — a prototype's properties, the
// receiver's, the global's — and reads again: the answer must be the one a
// lookup gives. The counters say the path was reached: an answer was taken
// (a hit) where nothing had changed.

using namespace sashfold;

namespace {

std::size_t shape_cells(js::Heap const& heap)
{
    return heap.cell_count_if([](js::Cell const& cell) { return dynamic_cast<js::Shape const*>(&cell) != nullptr; });
}

}

int main()
{
    // --- The caches are reached: a loop over one shape is all hits after
    // its first round, for a read, a write and a global.
    {
        js::Interpreter in;
        CHECK_JS_NUMBER(in, "var o = { x: 1, y: 2 }; function rd(p) { return p.x + p.y; } function wr(p, v) { p.x = v; }"
                            " function loop() { var s = 0; for (var i = 0; i < 100; i++) { wr(o, i); s += rd(o); } return s; } loop()", 5150);
        std::uint64_t const hits = in.cache_hits();
        std::uint64_t const misses = in.cache_misses();
        CHECK_JS_NUMBER(in, "loop()", 5150);
        // 100 rounds of five sites (wr, o, rd, o, and the three members
        // inside), and one miss: this script's own name for loop.
        CHECK(in.cache_hits() - hits >= 500);
        CHECK_EQ(in.cache_misses() - misses, std::uint64_t { 1 });
    }

    // --- A prototype changed after a site cached an answer from it.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        // A getter added to a prototype after the site cached the property
        // as absent, and then as the prototype's data.
        CHECK_JS_TRUE(in, "function P() {} var p = new P(); function rd(o) { return o.v; } rd(p); rd(p) === undefined");
        CHECK_JS_TRUE(in, "Object.defineProperty(P.prototype, 'v', { get: function () { return 'got'; }, configurable: true }); rd(p) === 'got' && rd(p) === 'got'");
        CHECK_JS_TRUE(in, "Object.defineProperty(P.prototype, 'v', { value: 'data', writable: true, configurable: true }); rd(p) === 'data'");
        CHECK_JS_TRUE(in, "P.prototype.v = 'changed'; rd(p) === 'changed'");
        // Deleted from the prototype: absent again.
        CHECK_JS_TRUE(in, "delete P.prototype.v; rd(p) === undefined && rd(p) === undefined");
        // Added higher up the chain, then shadowed lower down.
        CHECK_JS_TRUE(in, "Object.prototype.v = 'top'; var r1 = rd(p); P.prototype.v = 'mid'; var r2 = rd(p); p.v = 'own'; var r3 = rd(p);"
                          " delete Object.prototype.v; r1 === 'top' && r2 === 'mid' && r3 === 'own'");
        // A prototype changed under an object: its chain is another.
        CHECK_JS_TRUE(in, "function Q() {} Q.prototype.w = 1; var q = new Q(); function rw(o) { return o.w; } rw(q); rw(q);"
                          " Object.setPrototypeOf(Q.prototype, { w: 2, z: 3 }); delete Q.prototype.w; rw(q) === 2");
        CHECK_JS_TRUE(in, "Object.setPrototypeOf(q, { w: 9 }); rw(q) === 9");
        // A method replaced on the prototype a call site's load cached.
        CHECK_JS_TRUE(in, "function M() {} M.prototype.f = function () { return 1; }; var m = new M(); function call(o) { return o.f(); }"
                          " call(m) + call(m) === 2 && (M.prototype.f = function () { return 5; }, call(m) === 5)");
    }

    // --- A setter installed up the chain after a write site cached an
    // add-transition: the next write calls it, and adds nothing.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        // (The objects the write made are kept: an answer goes with the
        // last object of its shape.)
        CHECK_JS_TRUE(in, "function A() {} function add(o, v) { o.k = v; return o; } var kept_a = [add(new A(), 1), add(new A(), 2)]; var seen = [];"
                          " Object.defineProperty(A.prototype, 'k', { set: function (v) { seen.push(v); }, configurable: true });"
                          " var a = add(new A(), 3); seen.join() === '3' && !a.hasOwnProperty('k')");
        // A read-only property up the chain likewise refuses the write.
        CHECK_JS_TRUE(in, "function B() {} function put(o, v) { o.r = v; return o; } var kept_b = [put(new B(), 1), put(new B(), 2)];"
                          " Object.defineProperty(B.prototype, 'r', { value: 'ro', writable: false, configurable: true });"
                          " var b = put(new B(), 3); b.r === 'ro' && !b.hasOwnProperty('r')");
        CHECK_JS_THROWS(in, "'use strict'; function sput(o) { o.r = 4; } sput(new B());", "TypeError");
        // An own write to a property made read-only, or to a frozen object.
        CHECK_JS_TRUE(in, "function set(o, v) { o.x = v; } var w = { x: 1 }; set(w, 2); set(w, 3); Object.defineProperty(w, 'x', { writable: false });"
                          " set(w, 4); w.x === 3");
        CHECK_JS_TRUE(in, "var f1 = { x: 1 }; set(f1, 5); Object.freeze(f1); set(f1, 6); f1.x === 5");
        CHECK_JS_TRUE(in, "function grow(o) { o.fresh = 1; } grow({ a: 1 }); grow({ a: 1 }); var n = Object.preventExtensions({ a: 1 }); grow(n); !('fresh' in n)");
        CHECK_JS_TRUE(in, "var sealed = Object.seal({ a: 1 }); grow(sealed); !('fresh' in sealed)");
    }

    // --- One site over many shapes: monomorphic, polymorphic, megamorphic,
    // every answer right at every stage.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        CHECK_JS_TRUE(in, "function rd(o) { return o.a; } var objs = []; for (var i = 0; i < 12; i++) { var o = {}; o['p' + i] = i; o.a = i * 10; objs.push(o); }"
                          " var ok = true; for (var round = 0; round < 3; round++) for (var j = 0; j < 12; j++) ok = ok && rd(objs[j]) === j * 10; ok");
        CHECK_JS_TRUE(in, "function wr(o, v) { o.a = v; } for (var j = 0; j < 12; j++) wr(objs[j], -j); var ok2 = true;"
                          " for (var j = 0; j < 12; j++) ok2 = ok2 && rd(objs[j]) === -j; ok2");
        // A megamorphic site meets a change to a prototype.
        CHECK_JS_TRUE(in, "function miss(o) { return o.zz; } for (var j = 0; j < 12; j++) miss(objs[j]); Object.prototype.zz = 7;"
                          " var all = true; for (var j = 0; j < 12; j++) all = all && miss(objs[j]) === 7; delete Object.prototype.zz; all && miss(objs[3]) === undefined");
    }

    // --- Dictionaries: a receiver whose own properties change in place.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        CHECK_JS_TRUE(in, "var d = { a: 1, b: 2, c: 3 }; delete d.c; function ra(o) { return o.a; } function rb(o) { return o.b; } ra(d); rb(d);"
                          " delete d.a; d.e = 'e'; ra(d) === undefined && rb(d) === 2");
        CHECK_JS_TRUE(in, "delete d.b; d.f = 'f'; rb(d) === undefined && d.f === 'f'");
        CHECK_JS_TRUE(in, "function wb(o, v) { o.b = v; } d.b = 0; wb(d, 1); wb(d, 2); Object.defineProperty(d, 'b', { get: function () { return 'g'; }, configurable: true });"
                          " wb(d, 3); rb(d) === 'g'");
        // A built-in namespace, a dictionary, read through a cache.
        CHECK_JS_TRUE(in, "function fl(x) { return Math.floor(x); } fl(1.5); fl(2.5); var saved = Math.floor; Math.floor = function () { return 'mine'; };"
                          " var r = fl(3.5); Math.floor = saved; r === 'mine' && fl(4.5) === 4");
    }

    // --- Globals: a cached global read meets a global added, deleted,
    // shadowed by a script's let, and made an accessor.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        CHECK_JS_TRUE(in, "function g() { return typeof later === 'undefined' ? 'none' : later; } g(); g() === 'none'");
        CHECK_JS_TRUE(in, "var later = 'here'; g() === 'here' && g() === 'here'");
        CHECK_JS_TRUE(in, "globalThis.made = 1; function rm() { try { return made; } catch (e) { return 'gone'; } } rm(); rm(); delete globalThis.made; rm() === 'gone'");
        CHECK_JS_TRUE(in, "globalThis.shade = 'object'; function rs() { return shade; } rs(); rs() === 'object'");
        CHECK_JS_TRUE(in, "globalThis.lexical = 'property'; function rl() { return lexical; } rl(); rl() === 'property'");
        CHECK_JS_TRUE(in, "let lexical = 'binding'; rl() === 'binding'");
        CHECK_JS_TRUE(in, "Object.defineProperty(globalThis, 'shade', { get: function () { return 'getter'; }, configurable: true }); rs() === 'getter'");
        // Writes through a cached global: a var, a deleted one, a read-only one.
        CHECK_JS_TRUE(in, "var counter = 0; function bump() { counter = counter + 1; } bump(); bump(); bump(); counter === 3");
        CHECK_JS_TRUE(in, "globalThis.loose = 0; function wl(v) { loose = v; } wl(1); wl(2); delete globalThis.loose; wl(3); loose === 3");
        CHECK_JS_THROWS(in, "'use strict'; globalThis.strictly = 0; function ws(v) { strictly = v; } ws(1); ws(2); delete globalThis.strictly; ws(3);", "ReferenceError");
        CHECK_JS_TRUE(in, "globalThis.fixed = 1; function wf(v) { fixed = v; } wf(2); Object.defineProperty(globalThis, 'fixed', { writable: false }); wf(3); fixed === 2");
        // A let written through a cached reference; a const refuses.
        CHECK_JS_TRUE(in, "let tally = 0; function tick() { tally = tally + 2; } tick(); tick(); tick(); tally === 6");
        CHECK_JS_THROWS(in, "const frozen = 1; function wc() { frozen = 2; } try { wc(); } catch (e) {} wc();", "TypeError");
        // The dead zone of a let a cache knows.
        CHECK_JS_TRUE(in, "globalThis.yy = 'prop'; function rr() { return yy; } rr(); rr() === 'prop'");
        CHECK_JS_THROWS(in, "rr(); let yy = 1;", "ReferenceError");
    }

    // --- One site over objects of two realms.
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        CHECK_JS_TRUE(in, "function rd(o) { return o.tag; } function call(o) { return o.toString(); } Object.prototype.tag = 'A'; rd({}) === 'A'");
        js::RealmRecord* const a = in.current_realm();
        js::RealmRecord* const b = in.create_realm();
        {
            js::Interpreter::RealmScope const inside(in, b);
            CHECK_JS_TRUE(in, "Object.prototype.tag = 'B'; globalThis.made_in_b = { own: 1 }; true");
        }
        CHECK(in.current_realm() == a);
        js::PropertyRef const in_b = b->intrinsics.global->find_own(in.key("made_in_b"));
        CHECK(in_b && in_b->value.is_object());
        if (in_b && in_b->value.is_object()) {
            in.global()->put(in.key("other"), in_b->value);
            CHECK_JS_TRUE(in, "var r = []; for (var i = 0; i < 6; i++) { r.push(rd(i % 2 ? other : {})); } r.join() === 'A,B,A,B,A,B'");
            CHECK_JS_TRUE(in, "call({}) === '[object Object]' && call(other) === '[object Object]'");
        }
    }

    // --- The collector: a shape no object uses goes, cached or not, and an
    // answer over a live shape survives a collection.
    {
        js::Interpreter in;
        js::Heap& heap = in.heap();
        CHECK_JS_TRUE(in, "function rd(o) { return o.dying; } var keep = { kept: 1 }; function rk(o) { return o.kept; } function probe() { return rk(keep); } probe(); probe(); true");
        heap.collect();
        std::size_t const before = shape_cells(heap);
        // A fresh shape, cached at a site, then let go.
        CHECK_JS_TRUE(in, "(function () { var t = { tmp: 1 }; t.dying = 2; return rd(t) + rd(t); })() === 4");
        heap.collect();
        CHECK_EQ(shape_cells(heap), before);
        // Another shape at the same site reads right after the first died.
        CHECK_JS_TRUE(in, "rd({ other: 1, dying: 3 }) === 3 && rd({ dying: 5 }) === 5");
        // An answer over a live shape is still taken after a collection.
        std::uint64_t const misses = in.cache_misses();
        heap.collect();
        CHECK_JS_TRUE(in, "probe() === 1");
        // The new script's name for probe is its one miss; probe's own
        // sites (rk, keep, o.kept) still answer.
        CHECK_EQ(in.cache_misses() - misses, std::uint64_t { 1 });
        // Under stress: a collection at every allocation between reads.
        heap.set_stress(true);
        CHECK_JS_TRUE(in, "var ok = true; for (var i = 0; i < 20; i++) { var o = {}; o['f' + (i % 3)] = i; o.dying = i; ok = ok && rd(o) === i; } ok");
    }

    return test::report("js ic");
}
