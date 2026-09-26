// Bindings resolved to registers and environment slots: what a function
// compiles to (a register read for an uncaptured local, an environment
// only for a scope with a captured binding) and that every behaviour the
// names used to give stays the same: dead zones, const, delete, typeof,
// the mapped arguments object, `this` through arrows and before super(),
// generators and async functions across a suspension, eval and with, the
// own names of function expressions and classes, a second realm, and the
// messages, word for word (Node's are the oracle).

#include "JsTest.h"

#include "js/Bytecode.h"
#include "js/Evaluator.h"
#include "js/Interpreter.h"
#include "js/Object.h"

#include <string>
#include <string_view>

using namespace sashfold;

namespace {

// Every realm here runs under heap stress: a collection at every
// allocation, so a value the machine forgot to root fails the first time.
js::Interpreter& fresh()
{
    static js::Interpreter* interpreter = nullptr;
    delete interpreter;
    interpreter = new js::Interpreter();
    interpreter->heap().set_stress(true);
    return *interpreter;
}

void drain(js::Interpreter& in)
{
    in.run_jobs([](js::Value const&) {});
}

// The listing of the compiled body of the function named `name`, once it
// has run.
std::string listing(js::Interpreter& in, std::string_view name)
{
    for (auto const& [node, code] : in.impl().code_blocks) {
        if (node->name && node->name->to_utf8() == name)
            return js::disassemble(*code);
    }
    return "<no body named " + std::string(name) + ">";
}

bool contains(std::string const& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

void test_registers()
{
    js::Interpreter& in = fresh();
    // A local read and written in a loop lives in a register: no name is
    // looked up and no environment is made for the call.
    CHECK_JS_NUMBER(in,
        "function sum(n) { let total = 0; for (let i = 0; i < n; i++) total += i; return total; }"
        "sum(10)",
        45);
    std::string const code = listing(in, "sum");
    CHECK(contains(code, "GetLocal r0 n"));
    CHECK(contains(code, "SetLocal r1 total"));
    CHECK(contains(code, "GetLocal r2 i"));
    CHECK(!contains(code, "GetName"));
    CHECK(!contains(code, "PushEnv"));
    CHECK(!contains(code, "CopyIterationEnv"));
    // A captured binding makes its scope an environment, pushed once and
    // read by slot.
    CHECK_JS_NUMBER(in, "function captures(a) { let b = a * 2; return () => a + b; } captures(3)()", 9);
    std::string const captured = listing(in, "captures");
    CHECK(contains(captured, "PushEnv 0 size=2 function"));
    CHECK(contains(captured, "InitScoped hops=0 slot=0"));
    CHECK(contains(captured, "InitScoped hops=0 slot=1"));
    // A const is flagged where it is written, and the hole is its dead zone.
    CHECK_JS_NUMBER(in, "function konst() { const k = 4; return k * k; } konst()", 16);
    CHECK(contains(listing(in, "konst"), "PushEmpty"));
}

void test_closures_and_blocks()
{
    js::Interpreter& in = fresh();
    // A captured let read by a closure before and after its block ends.
    CHECK_JS_STRING(in,
        "function f() { let read; { let x = 1; read = () => x; var before = read(); x = 2; }"
        "  return before + ',' + read(); } f()",
        "1,2");
    // The per-iteration copy: each closure keeps its iteration's value.
    CHECK_JS_STRING(in,
        "function f() { const fs = []; for (let i of [0, 1, 2]) fs.push(() => i); return fs.map(g => g()).join(); } f()",
        "0,1,2");
    CHECK_JS_STRING(in,
        "function f() { const fs = []; for (let i = 0; i < 3; i++) fs.push(() => i); return fs.map(g => g()).join(); } f()",
        "0,1,2");
    CHECK_JS_STRING(in,
        "function f() { const fs = []; for (let k in { a: 1, b: 2 }) fs.push(() => k); return fs.map(g => g()).join(); } f()",
        "a,b");
    // An update in the loop's step lands on the copy the next body sees.
    CHECK_JS_STRING(in,
        "function f() { const fs = []; for (let i = 0; i < 3; i += 1) { fs.push(() => i); i += 0; } return fs.map(g => g()).join(); } f()",
        "0,1,2");
    // Hops count only the scopes that materialize: the middle block's let is
    // read only where it is declared, so it stays in a register and the block
    // makes no environment. The closure reaches `outer` one hop out, past the
    // inner block alone.
    CHECK_JS_NUMBER(in,
        "function hops() { let outer = 5; { let middle = 1; { let inner = 2 + middle;"
        "  return function readBoth() { return outer + inner; }; } } } hops()()",
        8);
    std::string const code = listing(in, "hops");
    CHECK(contains(code, "PushEnv 0 size=1 function"));
    CHECK(contains(code, "PushEnv 1 size=1\n"));
    CHECK(!contains(code, "PushEnv 2"));
    CHECK(contains(code, "GetLocal r0 middle"));
    std::string const reader = listing(in, "readBoth");
    CHECK(contains(reader, "GetScoped hops=1 slot=0"));
    CHECK(contains(reader, "GetScoped hops=0 slot=0"));
    CHECK(!contains(reader, "hops=2"));
    // A block entered again starts its lets in their dead zone again.
    CHECK_JS_STRING(in,
        "function f() { const out = []; for (let n = 0; n < 2; n++) { try { out.push(v); } catch (e) { out.push(e.name); } let v = n; }"
        "  return out.join(); } f()",
        "ReferenceError,ReferenceError");
    // A switch's case block is a block scope.
    CHECK_JS_NUMBER(in, "function f(v) { switch (v) { case 1: let q = 10; return (() => q)(); default: return 0; } } f(1)", 10);
    // Annex B: a block function is also the function's var.
    CHECK_JS_STRING(in, "function f() { { function g() { return 'g'; } } return g(); } f()", "g");
}

void test_dead_zone_and_const()
{
    js::Interpreter& in = fresh();
    CHECK_EQ(test::eval_throws(in, "function f() { x; let x = 1; } f()"), "ReferenceError: Cannot access 'x' before initialization");
    CHECK_EQ(test::eval_throws(in, "function f() { x = 2; let x = 1; } f()"), "ReferenceError: Cannot access 'x' before initialization");
    CHECK_EQ(test::eval_throws(in, "function f() { const read = () => y; read(); let y; } f()"),
        "ReferenceError: Cannot access 'y' before initialization");
    CHECK_EQ(test::eval_throws(in, "function f() { const c = 1; c = 2; } f()"), "TypeError: Assignment to constant variable.");
    CHECK_EQ(test::eval_throws(in, "function f() { const c = 1; c += 1; } f()"), "TypeError: Assignment to constant variable.");
    CHECK_EQ(test::eval_throws(in, "function f() { const c = 1; (() => { c++; })(); } f()"), "TypeError: Assignment to constant variable.");
    // typeof of a let in its dead zone throws, as a read does.
    CHECK_EQ(test::eval_throws(in, "function f() { return typeof y; let y; } f()"), "ReferenceError: Cannot access 'y' before initialization");
    CHECK_JS_STRING(in, "function f() { let y = 1; return typeof y + typeof nothing; } f()", "numberundefined");
    // delete of a declared binding is false and leaves it.
    CHECK_JS_STRING(in, "function f() { var x = 1; let y = 2; const d = [delete x, delete y]; return d + ',' + x + y; } f()",
        "false,false,12");
    CHECK_JS_TRUE(in, "function f() { var x = 1; return (() => delete x)() === false && x === 1; } f()");
    // A parameter list that is not simple has its own dead zone.
    CHECK_EQ(test::eval_throws(in, "function f(a = b, b) {} f()"), "ReferenceError: Cannot access 'b' before initialization");
    CHECK_JS_NUMBER(in, "function f(a, b = a + 1) { var a; return a + b; } f(1)", 3);
    CHECK_JS_NUMBER(in, "function f(a, b = () => a) { var a = 5; return b(); } f(1)", 1);
    // A class's own name in its heritage is in its dead zone.
    CHECK_EQ(test::eval_throws(in, "function f() { class C extends C {} } f()"), "ReferenceError: Cannot access 'C' before initialization");
    CHECK_EQ(test::eval_throws(in, "function f() { return nope; } f()"), "ReferenceError: nope is not defined");
}

void test_arguments()
{
    js::Interpreter& in = fresh();
    // Mapped: the parameter and the index are one binding, both ways.
    CHECK_JS_STRING(in, "function f(x) { x = 2; const a = arguments[0]; arguments[0] = 3; return a + ',' + x; } f(1)", "2,3");
    CHECK_JS_NUMBER(in, "function f(x) { return (() => { arguments[0] = 7; return x; })(); } f(1)", 7);
    // Unmapped in strict code and with a default: independent.
    CHECK_JS_STRING(in, "function f(x) { 'use strict'; x = 2; const a = arguments[0]; arguments[0] = 3; return a + ',' + x; } f(1)", "1,2");
    CHECK_JS_STRING(in, "function f(x = 0) { x = 2; const a = arguments[0]; arguments[0] = 3; return a + ',' + x; } f(1)", "1,2");
    // A name given twice: its last parameter is the one the arguments
    // object aliases, both ways; the earlier index is a plain value.
    CHECK_JS_NUMBER(in, "function d(a, a) { a = 9; return arguments[1]; } d(1, 2)", 9);
    CHECK_JS_STRING(in, "function d(a, a) { arguments[0] = 5; arguments[1] = 6; return a + ',' + arguments[0]; } d(1, 2)", "6,5");
    CHECK_JS_NUMBER(in, "function d(a, a) { a = 9; return arguments[0]; } d(1, 2)", 1);
    // Only the arguments given are mapped; a deleted index is unmapped.
    CHECK_JS_STRING(in, "function f(x, y) { y = 5; return arguments.length + ',' + arguments[1]; } f(1)", "1,undefined");
    CHECK_JS_STRING(in, "function f(x) { delete arguments[0]; arguments[0] = 9; return x + ',' + arguments[0]; } f(1)", "1,9");
    CHECK_JS_TRUE(in, "function f() { return arguments.callee === f; } f()");
}

void test_this_and_super()
{
    js::Interpreter& in = fresh();
    // An arrow reading `this` two levels down.
    CHECK_JS_NUMBER(in, "var o = { v: 4, m() { return () => () => this.v; } }; o.m()()()", 4);
    CHECK_JS_NUMBER(in, "var o = { v: 4, m() { return this.v; } }; o.m()", 4);
    CHECK_JS_TRUE(in, "function f() { return this; } f() === globalThis");
    CHECK_JS_TRUE(in, "function f() { 'use strict'; return this; } f() === undefined");
    CHECK_JS_STRING(in, "function f() { return typeof this; } f.call(1)", "object");
    // A derived constructor's `this` before and after super(). (Each case
    // in a function of its own, so the classes are that function's.)
    CHECK_EQ(test::eval_throws(in, "(() => { class A {} class B extends A { constructor() { this.x = 1; super(); } } new B(); })()"),
        "ReferenceError: Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
    CHECK_JS_NUMBER(in, "(() => { class A { constructor() { this.a = 1; } } class B extends A { constructor() { super(); this.b = 2; } }"
                        "  const b = new B(); return b.a + b.b; })()", 3);
    // super() from an arrow binds the constructor's `this`.
    CHECK_JS_NUMBER(in, "(() => { class A {} class B extends A { constructor() { const s = () => super(); s(); this.c = 3; } } return new B().c; })()", 3);
    CHECK_EQ(test::eval_throws(in, "(() => { class A {} class B extends A { constructor() { super(); super(); } } new B(); })()"),
        "ReferenceError: Super constructor may only be called once");
    CHECK_EQ(test::eval_throws(in, "(() => { class A {} class B extends A { constructor() {} } new B(); })()"),
        "ReferenceError: Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
    // super.x in the method itself and from an arrow inside it.
    CHECK_JS_STRING(in, "(() => { class A { m() { return 'A'; } } class B extends A { m() { return super.m() + (() => super.m())(); } }"
                        "  return new B().m(); })()",
        "AA");
    // new.target, own and through an arrow.
    CHECK_JS_TRUE(in, "function F() { return new.target === F; } new F() instanceof F");
    CHECK_JS_TRUE(in, "function F() { this.t = (() => new.target)(); } new F().t === F");
    CHECK_JS_TRUE(in, "function F() { return new.target; } F() === undefined");
    // Fields see their instance.
    CHECK_JS_NUMBER(in, "class P { x = 1; y = this.x + 1; } new P().y", 2);
}

void test_generators_and_async()
{
    js::Interpreter& in = fresh();
    // A generator resumed with a captured and an uncaptured local intact.
    CHECK_JS_STRING(in,
        "function* g(n) { let plain = n; let kept = n * 10; const read = () => kept; yield plain; plain += 1; kept += 1;"
        "  yield plain + ':' + read(); }"
        "var it = g(1); [it.next().value, it.next().value, it.next().done].join()",
        "1,2:11,true");
    // The prologue runs at the call: a default's effect comes before next().
    CHECK_JS_STRING(in,
        "var order = []; function* h(a = order.push('default')) { order.push('body'); }"
        "var gen = h(); order.push('made'); gen.next(); order.join()",
        "default,made,body");
    CHECK_EQ(test::eval_throws(in, "function* t(a = boom) {} t()"), "ReferenceError: boom is not defined");
    // An async function across an await, likewise.
    test::JsRun const started = test::run_js(in,
        "var result = 'pending';"
        "async function a(n) { let plain = n; let kept = n + 1; const read = () => kept; await null; plain *= 2;"
        "  await Promise.resolve(); result = plain + ':' + read(); }"
        "a(3);");
    CHECK(started.ok);
    drain(in);
    CHECK_JS_STRING(in, "result", "6:4");
    // An async generator: prologue at the call, locals across both kinds of suspension.
    test::JsRun const async_generator = test::run_js(in,
        "var got = [];"
        "async function* ag(n) { let local = n; await null; yield local; local += 1; yield local; }"
        "(async () => { for await (const v of ag(5)) got.push(v); })();");
    CHECK(async_generator.ok);
    drain(in);
    CHECK_JS_STRING(in, "got.join()", "5,6");
}

void test_by_name_paths()
{
    js::Interpreter& in = fresh();
    // A function with a direct eval keeps its bindings by name: the eval's
    // var is seen, and the eval sees the function's bindings.
    CHECK_JS_NUMBER(in, "function f() { eval('var added = 40'); return added + 2; } f()", 42);
    CHECK_JS_NUMBER(in, "function f() { let local = 3; return eval('local * 2'); } f()", 6);
    // An eval in an arrow sees the enclosing function's locals and this.
    CHECK_JS_NUMBER(in, "function f() { let k = 5; return (() => eval('k + this.v'))(); } f.call({ v: 1 })", 6);
    // A with inside a function.
    CHECK_JS_STRING(in, "function f(o) { var a = 'outer'; with (o) { return a + b; } } f({ b: '!' })", "outer!");
    CHECK_JS_STRING(in, "function f(o) { var a = 'outer'; with (o) { return a; } } f({ a: 'inner' })", "inner");
    // A closure in a function with a with, reading that function's binding.
    CHECK_JS_NUMBER(in, "function f() { let x = 7; with ({}) {} return (() => x)(); } f()", 7);
}

void test_own_names()
{
    js::Interpreter& in = fresh();
    // A named function expression reads its own name; assigning it is
    // ignored in sloppy code and a TypeError in strict code.
    CHECK_JS_TRUE(in, "function outer() { var g = function h() { return h; }; return g() === g; } outer()");
    CHECK_JS_TRUE(in, "function outer() { var g = function h() { h = 1; return h; }; return g() === g; } outer()");
    CHECK_EQ(test::eval_throws(in, "function outer() { var g = function h() { 'use strict'; h = 1; }; g(); } outer()"),
        "TypeError: Assignment to constant variable.");
    // A function expression whose name nothing reads makes no environment.
    CHECK_JS_STRING(in, "function holder() { return (function unread() { return 'x'; })(); } holder()", "x");
    CHECK(contains(listing(in, "holder"), "MakeClosure #0 - flags=1"));
    // A catch parameter shadows a var of the same name.
    CHECK_JS_STRING(in, "function f() { var e = 'var'; try { throw 'caught'; } catch (e) { var inside = e; } return e + ',' + inside; } f()",
        "var,caught");
    CHECK_JS_STRING(in, "function f() { try { throw { a: 1 }; } catch ({ a, b = a + 1 }) { return a + ',' + b; } } f()", "1,2");
    CHECK_EQ(test::eval_throws(in, "function f() { try { throw {}; } catch ({ a = b, b }) {} } f()"),
        "ReferenceError: Cannot access 'b' before initialization");
    // A class's name read inside its body, and by its methods.
    CHECK_JS_TRUE(in, "function f() { class C { static self() { return C; } m() { return C; } } return C.self() === C && new C().m() === C; } f()");
    CHECK_JS_STRING(in, "function f() { class C { static k = 'k'; static [C_key()] = 1; } function C_key() { return 'z'; } return C.k + Object.keys(C); } f()",
        "kk,z");
    CHECK_EQ(test::eval_throws(in, "function f() { class C { static m() { C = 1; } } C.m(); } f()"), "TypeError: Assignment to constant variable.");
}

void test_second_realm()
{
    js::Interpreter& in = fresh();
    js::RealmRecord* const other = in.create_realm();
    {
        js::Interpreter::RealmScope const inside(in, other);
        test::JsRun const defined = test::run_js(in,
            "function counter(start) { let n = start; const bump = () => ++n; bump(); return bump() + (this === globalThis ? 100 : 0); }");
        CHECK(defined.ok);
    }
    js::Interpreter::Roots const roots(in);
    js::Value const counter = in.root(in.get(js::Value::object(other->intrinsics.global), in.key("counter")).value_or(js::Value::undefined()));
    js::Value const argument[1] = { js::Value::number(1) };
    std::optional<js::Value> const result = in.call(counter, js::Value::undefined(), argument);
    CHECK(result.has_value());
    // Sloppy `this` for an undefined receiver is the callee's realm's global.
    CHECK(result && result->is_number() && result->as_number() == 103);
}

}

int main()
{
    test_registers();
    test_closures_and_blocks();
    test_dead_zone_and_const();
    test_arguments();
    test_this_and_super();
    test_generators_and_async();
    test_by_name_paths();
    test_own_names();
    test_second_realm();
    return ::sashfold::test::report("test_js_slots");
}
