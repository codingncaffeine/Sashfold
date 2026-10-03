#include "JsTest.h"

// The machine's frames on its stacks (js-JIT-DESIGN.md 4.2): a running
// frame's registers and operand stack on the value stack, its environments
// on the environment stack, and a suspended body's frame copied out and
// back. Every realm under heap stress, so a value the stacks fail to keep
// alive is collected at the next allocation and the check reads it freed.

#include <string>

using namespace sashfold;

namespace {

js::Interpreter& fresh()
{
    static js::Interpreter* interpreter = nullptr;
    delete interpreter;
    interpreter = new js::Interpreter;
    interpreter->heap().set_stress(true);
    return *interpreter;
}

// Runs a script that fills a global `out` array, drains the job queue, and
// answers the array joined.
std::string with_jobs(js::Interpreter& in, std::string_view source)
{
    test::JsRun const run = test::run_js(in, std::string("var out = [];\n") + std::string(source));
    if (!run.ok)
        return "threw " + run.thrown;
    in.run_jobs([&in](js::Value const& thrown) {
        test::fail("a job threw: " + in.describe(thrown), __FILE__, __LINE__);
    });
    return test::eval_string(in, "out.join(' ')");
}

void test_recursion_to_the_limit()
{
    js::Interpreter& in = fresh();
    test::run_js(in, "function f(n) { return n ? 1 + f(n - 1) : 0; }");
    // (A depth every build's C++ stack holds while each script call still
    // recurses through the run loop: under the address sanitizer, 1,000
    // levels pass the 4 MB budget, before this change as after it.)
    CHECK_JS_NUMBER(in, "f(100)", 100);
    // Past every limit: the RangeError, caught by the script, and the
    // stacks are whole again after it.
    CHECK_JS_THROWS(in, "f(1e7)", "RangeError");
    CHECK_JS_TRUE(in, "(function () { try { f(1e7); return false; } catch (e) { return e instanceof RangeError; } })()");
    CHECK_JS_NUMBER(in, "f(100)", 100);
    CHECK_JS_NUMBER(in, "(function () { var x = 41; try { f(1e7); } catch (e) {} return x + 1; })()", 42);
}

void test_values_only_on_the_stacks()
{
    js::Interpreter& in = fresh();
    // A string that lives in one frame's registers while every frame above
    // it allocates.
    CHECK_JS_STRING(in, "function deep(n, s) { if (n == 0) return s; var t = s + 'x'; return deep(n - 1, t) + n; } deep(5, 'a')",
        "axxxxx12345");
    // An object on the operand stack, mid-expression, across a call that
    // allocates.
    CHECK_JS_STRING(in, "function garbage() { for (var i = 0; i < 50; i++) ({ i: i }); return 'g'; }"
                        " JSON.stringify([{ a: 1 }, garbage(), { b: 2 }])",
        "[{\"a\":1},\"g\",{\"b\":2}]");
    // The arguments a native was handed, read from the caller's operand
    // stack, while the callback's frames above them allocate.
    CHECK_JS_STRING(in, "[1, 2, 3].map(function (x) { var big = []; for (var i = 0; i < 10; i++) big.push({ i: i }); return x * big.length; }).join()",
        "10,20,30");
    // A native calling back into script calling a native calling back.
    CHECK_JS_STRING(in, "[[1, 2], [3]].map(function (row) { return row.map(function (x) { return { v: x * 2 }; }).map(function (o) { return o.v; }).join('+'); }).join('|')",
        "2+4|6");
}

void test_throws_across_frames()
{
    js::Interpreter& in = fresh();
    test::run_js(in, "function a(n) { if (n == 0) throw new Error('deep'); var keep = { n: n }; return a(n - 1) + keep.n; }");
    CHECK_JS_STRING(in, "(function () { try { return String(a(5)); } catch (e) { return e.message; } })()", "deep");
    // A finally on every level, and the frame that catches carries on.
    CHECK_JS_STRING(in,
        "var seen = [];"
        "function b(n) { try { if (n == 0) throw new TypeError('t'); return b(n - 1); } finally { seen.push(n); } }"
        "(function () { var x = 'kept'; try { b(3); } catch (e) { seen.push(e.name); } return x + ':' + seen.join(); })()",
        "kept:0,1,2,3,TypeError");
}

void test_environments_across_frames()
{
    js::Interpreter& in = fresh();
    // Block scopes in each frame of a recursion.
    CHECK_JS_NUMBER(in, "function f(n) { { let a = n; if (n > 0) { let b = f(n - 1); return a + b; } return a; } } f(10)", 55);
    // A closure made in an inner frame's block keeps its environment after
    // the frame is gone.
    CHECK_JS_STRING(in, "function mk(n) { { let v = 'v' + n; return function () { return v; }; } } var fs = [mk(1), mk(2)]; fs[0]() + fs[1]()", "v1v2");
    // A class with no scope of its own pushes its outer environment again.
    CHECK_JS_NUMBER(in, "function k(n) { class A { m() { return n; } } return n > 0 ? new A().m() + k(n - 1) : 0; } k(4)", 10);
    // with: an object environment on the environment stack.
    CHECK_JS_NUMBER(in, "function w(o) { with (o) { return x + (function () { return x; })(); } } w({ x: 21 })", 42);
}

void test_suspension()
{
    js::Interpreter& in = fresh();
    // A generator suspended in the middle of an array literal: what was
    // already evaluated sits on its operand stack across the suspension and
    // the allocations between.
    CHECK_JS_STRING(in,
        "function* g() { var r = [{ x: 1 }, yield 'a', { y: 2 }]; return JSON.stringify(r); }"
        " var it = g(); it.next(); for (var i = 0; i < 50; i++) ({ i: i }); it.next(5).value",
        "[{\"x\":1},5,{\"y\":2}]");
    // Suspended inside a class's computed key: the class under construction
    // and its scope are kept with the frame.
    CHECK_JS_NUMBER(in, "function* c() { class C { [yield 'k']() { return 7; } } return new C().m(); } var ic = c(); ic.next(); ic.next('m').value", 7);
    // A generator resumed from a frame deeper than the one that made it.
    CHECK_JS_STRING(in,
        "function* counter() { var n = 0; while (true) n += yield n; }"
        " var gen = counter(); gen.next();"
        " function deeper(k) { return k ? deeper(k - 1) : gen.next(5).value; } deeper(20) + ',' + deeper(3)",
        "5,10");
    // Async functions awaiting inside nested calls, their values kept.
    CHECK_EQ(with_jobs(in,
                 "async function inner(v) { var a = [v]; await null; a.push(v + 1); return a; }"
                 " async function outer() { var o = { k: 'kept' }; var r = await inner(1); return o.k + r.join(); }"
                 " outer().then(function (v) { out.push(v); });"),
        std::string("kept1,2"));
    // An async generator, its requests queued while it waits.
    CHECK_EQ(with_jobs(in,
                 "async function* ag() { var keep = { z: 'z' }; yield 1; await null; yield keep.z; }"
                 " var g2 = ag(); g2.next().then(function (r) { out.push(r.value); }); g2.next().then(function (r) { out.push(r.value); });"
                 " g2.next().then(function (r) { out.push(String(r.done)); });"),
        std::string("1 z true"));
}

void test_stack_traces_walk_the_frame_stack()
{
    js::Interpreter& in = fresh();
    std::string const stack = test::eval_string(in, "function a() { return new Error('x').stack; } function b() { return a(); } b()");
    std::size_t const at_a = stack.find("at a");
    std::size_t const at_b = stack.find("at b");
    CHECK(at_a != std::string::npos);
    CHECK(at_b != std::string::npos);
    CHECK(at_a < at_b);
}

}

int main()
{
    test_recursion_to_the_limit();
    test_values_only_on_the_stacks();
    test_throws_across_frames();
    test_environments_across_frames();
    test_suspension();
    test_stack_traces_walk_the_frame_stack();
    return sashfold::test::report("js_vm_frames");
}
