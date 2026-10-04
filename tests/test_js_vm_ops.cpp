#include "JsTest.h"

// The run loop's own member writes and calls (js-JIT-DESIGN.md 4.3): a
// member written with no reference made, a call to a script function that
// pushes the callee's frame and goes on in the same loop, and the
// interrupt's budget spent at loop back-edges and calls. The answers were
// read from Node 26 on 2026-10-03, case by case, except where a comment
// says the specification and V8 part.

#include "platform/ScriptThread.h"

#include <string>

using namespace sashfold;

namespace {

// Under heap stress, so a value the new paths fail to keep alive is
// collected at the next allocation and the answer reads it freed.
js::Interpreter& fresh()
{
    static js::Interpreter* interpreter = nullptr;
    delete interpreter;
    interpreter = new js::Interpreter;
    interpreter->heap().set_stress(true);
    return *interpreter;
}

void test_member_writes_on_every_receiver()
{
    js::Interpreter& in = fresh();
    // An ordinary object: assignment, compound, update, logical, chained.
    CHECK_JS_STRING(in,
        "var o = {}; o.a = 1; o['b'] = 2; o.a += 10; o['b'] *= 3; o.a++; ++o['b']; o.c ||= 5; o.c &&= 6; o.d ?\?= 7;"
        " o.e = o.f = 8; JSON.stringify(o)",
        "{\"a\":12,\"b\":7,\"c\":6,\"d\":7,\"f\":8,\"e\":8}");
    // An array at an index, past its end, by a computed key with an update
    // in it (evaluated once).
    CHECK_JS_STRING(in, "var a = [1, 2, 3]; a[0] = 9; a[1] += 5; a[2]++; a[5] = 1; var i = 0; a[i++] += 100; a.length + ':' + a.join() + ':' + i",
        "6:109,7,4,,,1:1");
    // A setter and a getter on the prototype: each write calls the setter
    // with the object as `this`, each compound reads once, and a logical
    // write that the value decides writes nothing.
    CHECK_JS_STRING(in,
        "var log = []; var proto = { set x(v) { log.push('set ' + v); }, get x() { log.push('get'); return 40; } };"
        " var obj = Object.create(proto); obj.x = 1; obj.x += 2; obj.x++; obj.x ||= 3; obj.x &&= 4; obj.x ?\?= 5;"
        " log.join(', ') + '|' + Object.keys(obj).length",
        "set 1, get, set 42, get, set 41, get, get, set 4, get|0");
    // A proxy: its traps in order.
    CHECK_JS_STRING(in,
        "var traps = []; var p = new Proxy({}, { set(t, k, v, r) { traps.push('set ' + String(k) + '=' + v); t[k] = v; return true; },"
        " get(t, k, r) { traps.push('get ' + String(k)); return t[k]; } });"
        " p.a = 1; p['b'] = 2; p.a += 1; p.b++; p.c ?\?= 9; p.c ||= 10; traps.join(', ')",
        "set a=1, set b=2, get a, set a=2, get b, set b=3, get c, set c=9, get c");
    // A trap that refuses: nothing from sloppy code, a TypeError from strict.
    CHECK_JS_STRING(in,
        "var refused = []; var r = new Proxy({}, { set(t, k, v) { refused.push(String(k)); return false; } }); r.a = 1; r['b'] += 1;"
        " var strict = (function () { 'use strict'; try { r.c = 1; return 'no'; } catch (e) { return e instanceof TypeError; } })();"
        " refused.join() + '|' + strict",
        "a,b,c|true");
    // A frozen object: sloppy code's writes are dropped...
    CHECK_JS_STRING(in, "(function () { var f = Object.freeze({ x: 1 }); f.x = 2; f['x'] += 1; f.x++; f.y = 3; f.x &&= 7; return f.x + ':' + f.y; })()",
        "1:undefined");
    // ...strict code's throw, and a logical write the value decides is no
    // write at all.
    CHECK_JS_STRING(in,
        "(function () { 'use strict'; var f = Object.freeze({ x: 1 }); var r = [];"
        " try { f.x = 2; r.push('no'); } catch (e) { r.push(e instanceof TypeError); }"
        " try { f['x'] += 1; r.push('no'); } catch (e) { r.push(e instanceof TypeError); }"
        " try { f.x++; r.push('no'); } catch (e) { r.push(e instanceof TypeError); }"
        " try { f.y = 1; r.push('no'); } catch (e) { r.push(e instanceof TypeError); }"
        " try { f.x ||= 1; r.push('skipped'); } catch (e) { r.push('threw'); } return r.join() + ':' + f.x; })()",
        "true,true,true,true,skipped:1");
    // A read-only own property, the same way.
    CHECK_JS_STRING(in,
        "(function () { var o = {}; Object.defineProperty(o, 'ro', { value: 1, writable: false }); o.ro = 2; o.ro += 1; o['ro']++; o.ro ||= 3; return String(o.ro); })()",
        "1");
    // A primitive receiver: sloppy code writes to a wrapper that is thrown
    // away; strict code throws.
    CHECK_JS_STRING(in, "(function () { var s = 'abc'; s.x = 1; s['y'] = 2; s.z += 1; s.w++; var n = 5; n.q = 1; return typeof s.x + typeof s.y + typeof n.q; })()",
        "undefinedundefinedundefined");
    CHECK_JS_STRING(in,
        "(function () { 'use strict'; var r = []; var s = 'abc';"
        " try { s.x = 1; r.push('no'); } catch (e) { r.push(e instanceof TypeError); }"
        " try { s['y'] += 1; r.push('no'); } catch (e) { r.push(e instanceof TypeError); }"
        " try { (5).x = 1; r.push('no'); } catch (e) { r.push(e instanceof TypeError); }"
        " try { true.x++; r.push('no'); } catch (e) { r.push(e instanceof TypeError); } return r.join(); })()",
        "true,true,true,true");
    // A setter on a primitive's prototype sees the primitive itself as
    // `this` (the receiver is the base, not its wrapper).
    CHECK_JS_STRING(in,
        "(function () { var seen = []; Object.defineProperty(String.prototype, 'probe', { set: function (v) { 'use strict'; seen.push(typeof this + '=' + v); },"
        " configurable: true }); 's'.probe = 1; (function () { 'use strict'; 't'.probe = 2; })(); delete String.prototype.probe; return seen.join(); })()",
        "string=1,string=2");
    // Null and undefined: a TypeError for every form, a logical one too.
    CHECK_JS_STRING(in,
        "(function () { var r = []; for (var b of [null, undefined]) {"
        " try { b.x = 1; r.push('no'); } catch (e) { r.push(e instanceof TypeError); }"
        " try { b['x'] += 1; r.push('no'); } catch (e) { r.push(e instanceof TypeError); }"
        " try { b.x ?\?= 1; r.push('no'); } catch (e) { r.push(e instanceof TypeError); } } return r.join(); })()",
        "true,true,true,true,true,true");
    // Keys of every kind: a symbol, numbers that are not indices, -0; a
    // typed array's elements (stored as their type converts, out of range
    // dropped).
    CHECK_JS_STRING(in,
        "var sym = Symbol('s'); var k = {}; k[sym] = 1; k[sym] += 1; k[sym]++; k[1.5] = 'a'; k[-0] = 'z'; k[1e21] = 'big'; k[sym] + ':' + Object.keys(k).sort().join(',')",
        "3:0,1.5,1e+21");
    CHECK_JS_STRING(in, "var ta = new Uint8Array(2); ta[0] = 300; ta[1] += 5; ta[1]++; ta[5] = 1; ta['1'] *= 2; Array.from(ta).join() + ':' + ta[5]",
        "44,12:undefined");
}

void test_evaluation_order()
{
    js::Interpreter& in = fresh();
    test::run_js(in,
        "var log = []; var target = {};"
        " function base() { log.push('base'); return target; }"
        " function key() { log.push('key'); return { toString() { log.push('toString'); return 'k'; } }; }"
        " function value() { log.push('value'); return 1; }");
    // base, then key, then value; the key made a property key once, at the
    // write (§13.15.2, §6.2.5.6).
    CHECK_JS_STRING(in, "log = []; base()[key()] = value(); log.join(', ') + '|' + target.k", "base, key, value, toString|1");
    // A null base: the TypeError comes at the write, after the value, and
    // before the key is converted (PutValue's ToObject first).
    CHECK_JS_STRING(in,
        "log = []; try { (function () { log.push('base'); return null; })()[key()] = value(); } catch (e) { log.push(e.constructor.name); } log.join(', ')",
        "base, key, value, TypeError");
    // A compound write reads once and converts once: GetValue writes the
    // property key back into the reference (§6.2.5.5 step 3.c), so PutValue
    // does not convert again. (V8 converts a second time, at the write: Node
    // 26 logs toString twice in each of the three cases below.)
    CHECK_JS_STRING(in,
        "log = []; target = { get k() { log.push('get'); return 1; }, set k(v) { log.push('set ' + v); } };"
        " base()[key()] += value(); log.join(', ')",
        "base, key, toString, get, value, set 2");
    CHECK_JS_STRING(in, "log = []; target[key()]++; log.join(', ')", "key, toString, get, set 2");
    CHECK_JS_STRING(in,
        "log = []; target = { get k() { log.push('get'); return 0; }, set k(v) { log.push('set ' + v); } };"
        " target[key()] ||= (log.push('rhs'), 5); target[key()] &&= (log.push('rhs2'), 6); log.join(', ')",
        "key, toString, get, rhs, set 5, key, toString, get");
    // A logical write's value is the expression's, written or not.
    CHECK_JS_STRING(in, "(function () { var o = {}; var a = (o.x ||= 1); var b = (o.x &&= 2); var c = (o.y ?\?= 3); var d = (o.x ||= 9); return [a, b, c, d, o.x].join(); })()",
        "1,2,3,2,2");
}

void test_destructuring_and_loop_heads()
{
    js::Interpreter& in = fresh();
    // Member targets of every pattern form: elements, properties, defaults,
    // rest elements and rest properties, nested.
    CHECK_JS_STRING(in,
        "(function () { var o = {}, rest; [o.a, o['b']] = [o.b = 1, 2]; ({ x: o.c = 3, ...rest } = { y: 4 }); o.r = rest.y;"
        " [o.d, ...o.e] = [5, 6, 7]; ({ ...o.f } = { g: 8 }); [[o.h], { i: o['j'] }] = [[9], { i: 10 }]; return JSON.stringify(o); })()",
        "{\"b\":2,\"a\":1,\"c\":3,\"r\":4,\"d\":5,\"e\":[6,7],\"f\":{\"g\":8},\"h\":9,\"j\":10}");
    // A target's base and key are evaluated before its value is taken: the
    // key converted at the write, after the iterator's step.
    CHECK_JS_STRING(in,
        "(function () { var log = []; var o = {};"
        " var it = { [Symbol.iterator]() { return { next() { log.push('next'); return { done: false, value: 7 }; }, return() { log.push('return'); return {}; } }; } };"
        " function base() { log.push('base'); return o; }"
        " function key() { log.push('key'); return { toString() { log.push('toString'); return 'k'; } }; }"
        " [base()[key()], base().n] = it; return log.join(', ') + '|' + o.k + o.n; })()",
        "base, key, next, toString, base, next, return|77");
    CHECK_JS_STRING(in,
        "(function () { var log = []; var o = {}; function base() { log.push('base'); return o; } function key() { log.push('key'); return 'k'; }"
        " var src = { get a() { log.push('get a'); return 1; }, get b() { log.push('get b'); return undefined; } };"
        " ({ a: base().x, b: base()[key()] = (log.push('default'), 2) } = src); return log.join(', ') + '|' + o.x + o.k; })()",
        "base, get a, base, key, get b, default|12");
    // A for-of and a for-in head assign after the value is had: the key is
    // evaluated each time round; a setter receives each value.
    CHECK_JS_STRING(in,
        "(function () { var log = []; var o = {}; var n = 0; function f() { log.push('f' + n); return 'k' + n++; }"
        " for (o[f()] of (log.push('iter'), [1, 2])) log.push(JSON.stringify(o)); return log.join(' '); })()",
        "iter f0 {\"k0\":1} f1 {\"k0\":1,\"k1\":2}");
    CHECK_JS_STRING(in,
        "(function () { var o = {}; var p = { set q(v) { this.seen = v; } }; for (o.a of [1, 2]); for (o['b'] in { z: 1, y: 2 }); var keys = [];"
        " for (p.q in { m: 1 }) keys.push(p.seen); return o.a + o.b + keys.join(); })()",
        "2ym");
}

void test_references_kept()
{
    js::Interpreter& in = fresh();
    // What still needs a reference: a super property, a private name, a
    // name inside `with`, delete.
    CHECK_JS_STRING(in,
        "(function () { class A { get x() { return this._x; } set x(v) { this._x = 'A' + v; } }"
        " class B extends A { m() { super.x = 1; super['x'] += 2; super.x ||= 0; return this._x; } } return new B().m(); })()",
        "AA12");
    CHECK_JS_STRING(in,
        "(function () { class C { #x = 1; m() { this.#x += 2; this.#x ||= 5; var old = this.#x++; this.#x ?\?= 0; return old + ':' + this.#x; } } return new C().m(); })()",
        "3:4");
    CHECK_JS_NUMBER(in, "(function () { var o = { x: 1 }; with (o) { x = 5; x += 1; x++; } return o.x; })()", 7);
    CHECK_JS_STRING(in, "(function () { var o = { x: 1, y: 2 }; return [delete o.x, delete o['y'], 'x' in o, Object.keys(o).length].join(); })()",
        "true,true,false,0");
    // Members of members.
    CHECK_JS_STRING(in, "(function () { var a = { b: { c: 1 } }, k = 'b', m = 'c'; a.b.c = 2; a[k][m] += 3; a.b['c']++; a.b.d = a.b.e = 7; return JSON.stringify(a); })()",
        "{\"b\":{\"c\":6,\"e\":7,\"d\":7}}");
}

void test_method_calls_keep_their_receiver()
{
    js::Interpreter& in = fresh();
    // The base is the call's `this`, evaluated once; a parenthesized comma
    // drops it; optional links, spreads and tagged templates keep it.
    CHECK_JS_STRING(in,
        "(function () { var o = { m() { return this; }, n: function () { 'use strict'; return this; } }; var calls = 0;"
        " function g() { calls++; return o; } return [o.m() === o, o['m']() === o, g().m() === o, calls, (0, o.n)() === undefined,"
        " o?.m() === o, o.m?.() === o].join(); })()",
        "true,true,true,1,true,true,true");
    CHECK_JS_STRING(in,
        "(function () { var o = { m() { return this === o ? Array.prototype.slice.call(arguments).join('+') : 'bad'; }, tag(s) { return this === o ? s[0] : 'bad'; } };"
        " var args = [1, 2, 3]; return [o.m(...args), o['m'](...args), o.nope?.(), o.tag`hi`, o['tag']`yo`].join(); })()",
        "1+2+3,1+2+3,,hi,yo");
    CHECK_JS_STRING(in, "['abc'.toUpperCase(), (5).toFixed(1), true.toString(), 'x'.concat.call('y', 'z')].join()", "ABC,5.0,true,yz");
}

void test_throws_across_inline_frames()
{
    js::Interpreter& in = fresh();
    // Thrown three calls deep, through a finally, caught by the outermost:
    // the frame that catches carries on with its operand stack as it was
    // (the 1 and 2 pushed before the call) and its locals.
    CHECK_JS_STRING(in,
        "(function () { var log = []; function c() { log.push('c'); throw new Error('boom'); }"
        " function b() { try { c(); } finally { log.push('finally b'); } log.push('after b'); }"
        " function a() { var keep = 5; b(); log.push('after a'); return keep; }"
        " function outer() { var x = 10; var r = [1, 2, (function () { try { return a(); } catch (e) { log.push('caught ' + e.message); return x + 1; } })(), 4]; return r.join(); }"
        " return outer() + '|' + log.join(', '); })()",
        "1,2,11,4|c, finally b, caught boom");
    // A finally on every one of fifty levels runs once.
    CHECK_JS_STRING(in,
        "(function () { function f(n) { if (n === 0) throw new RangeError('bottom'); try { return f(n - 1); } finally { f.unwound = (f.unwound || 0) + 1; } }"
        " try { f(50); } catch (e) { return e.message + ':' + f.unwound; } })()",
        "bottom:50");
    // Each throw gives back the calls it unwinds: thirty thousand throws from
    // three levels down, each caught, would pass the call-depth ceiling
    // (20,000) a few thousand throws in if one level leaked each time.
    CHECK_JS_NUMBER(in,
        "(function () { function c() { throw 1; } function b() { c(); } function a() { b(); } var caught = 0;"
        " for (var i = 0; i < 30000; i++) { try { a(); } catch (e) { caught += e; } } return caught; })()",
        30000);
    // And a recursion to the ceiling still reaches it afterwards.
    CHECK_JS_NUMBER(in, "(function () { function f(n) { return n ? 1 + f(n - 1) : 0; } return f(19000); })()", 19000);
}

void test_deep_recursion_on_a_small_budget()
{
    // A script's call to a script function pushes a frame and goes on in the
    // same run loop: no C++ frame a level. On a budget of 512 KB of C++
    // stack — a hundred and some levels when each call took the run loop's
    // frame again — a plain recursion reaches ten thousand; one through a
    // native, a C++ call a level, is stopped by the budget long before.
    int plain = 0;
    int through_native = 0;
    platform::ScriptThread thread([&] {
        js::Interpreter in;
        in.set_stack_budget(512u * 1024u);
        test::JsRun const a = test::run_js(in, "function f(n) { return n ? 1 + f(n - 1) : 0; } f(10000)");
        plain = a.ok && a.value.is_number() ? static_cast<int>(a.value.as_number()) : -1;
        test::JsRun const b = test::run_js(in, "var n = 0; function g() { ++n; [0].forEach(g); } try { g(); } catch (e) { e instanceof RangeError ? n : -1 }");
        through_native = b.ok && b.value.is_number() ? static_cast<int>(b.value.as_number()) : -1;
    });
    thread.join();
    CHECK_EQ(plain, 10000);
    CHECK(through_native > 5);
    CHECK(through_native < 1000);
}

void test_runaway_guard()
{
    // A loop's back-edge spends one step of the budget; each time the budget
    // runs out should_stop is asked, and a yes ends the script where no catch
    // sees it. Asked every thousand steps and saying yes the third time, a
    // loop counting once a round is stopped at three thousand.
    {
        js::Interpreter in;
        int polls = 0;
        in.set_interrupt([&polls] { return ++polls >= 3; }, 1000);
        test::JsRun const run = test::run_js(in, "var i = 0; try { for (; i < 1e7; i++) {} } catch (e) { 'caught' }");
        CHECK(!run.ok);
        CHECK(in.terminated());
        CHECK_EQ(polls, 3);
        in.clear_termination();
        CHECK_JS_NUMBER(in, "i", 3000);
    }
    // A call's entry spends one too: a recursion is stopped by the guard
    // long before the depth ceiling, whose RangeError the script could catch.
    // The two hundredth call spends the last step at its entry and is
    // stopped there, before its body counts it.
    {
        js::Interpreter in;
        int polls = 0;
        in.set_interrupt([&polls] { return ++polls >= 2; }, 100);
        test::JsRun const run = test::run_js(in, "var depth = 0; function f() { ++depth; f(); } try { f(); } catch (e) { 'caught' }");
        CHECK(!run.ok);
        CHECK(in.terminated());
        CHECK_EQ(polls, 2);
        in.clear_termination();
        CHECK_JS_NUMBER(in, "depth", 199);
    }
    // Every loop form has a back-edge that counts: each of these would run
    // its million rounds to the end if its back-edge spent nothing.
    for (char const* loop : { "var n = 0; do { n++; } while (n < 1e6)", "var n = 0; while (n < 1e6) n++;", "for (var n = 0; n < 1e6; n++) {}",
             "outer: for (var a = 0; a < 1e6; a++) { for (;;) { continue outer; } }" }) {
        js::Interpreter in;
        in.set_interrupt([] { return true; }, 50);
        test::JsRun const run = test::run_js(in, loop);
        CHECK(!run.ok);
        CHECK(in.terminated());
    }
}

}

int main()
{
    test_member_writes_on_every_receiver();
    test_evaluation_order();
    test_destructuring_and_loop_heads();
    test_references_kept();
    test_method_calls_keep_their_receiver();
    test_throws_across_inline_frames();
    test_deep_recursion_on_a_small_budget();
    test_runaway_guard();
    return sashfold::test::report("js_vm_ops");
}
