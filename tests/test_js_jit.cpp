// The baseline compiler's machine code (js/jit, _plans/js-JIT-DESIGN.md §5):
// its templates and its calls to the interpreter's own handlers together
// must compute what T0 computes, test by test, whatever path the frame
// takes through the code — calls the run loop makes inline
// and their returns (the code left and entered again at the next
// instruction), generators and async bodies suspended and resumed at any
// instruction, a throw caught by a handler of the frame or of a caller, a
// recursion as deep as T0 allows. And the path is reached: blocks are given
// machine code in `eager` mode, the hot ones in `tiered`, none when the
// tier is off.

#include "JsTest.h"

#include "js/Bytecode.h"
#include "js/jit/Baseline.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace sashfold;

namespace {

constexpr std::string_view script = R"JS(
(function () {
    var out = [];
    function add(a, b) { return a + b * 2; }
    var s = 0;
    for (var i = 0; i < 2000; i++)
        s = add(s, i);
    out.push(s);
    // Calls the loop makes inline, nested, and their returns.
    function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
    out.push(fib(20));
    // A throw caught in the frame, and one caught by a caller two frames up.
    function thrower(depth) { if (depth === 0) throw new RangeError('deep'); return thrower(depth - 1); }
    try { thrower(5); } catch (e) { out.push(e.name + ':' + e.message); }
    function local() { try { null.x; } catch (e) { return 'caught ' + e.constructor.name; } finally { out.push('finally'); } }
    out.push(local());
    // Generators: resumed at the instruction after each yield.
    function* counter(n) { for (var k = 0; k < n; k++) { var got = yield k; if (got) k += got; } return 'done'; }
    var it = counter(10), seen = [];
    var step = it.next();
    while (!step.done) { seen.push(step.value); step = it.next(step.value === 2 ? 3 : 0); }
    out.push(seen.join('') + step.value);
    // Closures, arguments, this, getters, classes with super.
    function make(x) { return { get v() { return x; }, inc() { x++; return this; } }; }
    out.push(make(4).inc().inc().v);
    function args() { return Array.prototype.slice.call(arguments).reverse().join('-'); }
    out.push(args(1, 2, 3));
    class A { constructor(n) { this.n = n; } twice() { return this.n * 2; } }
    class B extends A { twice() { return super.twice() + 1; } }
    out.push(new B(10).twice());
    // Switch, labels, for-in, for-of, destructuring, spread, templates.
    var acc = '';
    outer: for (var p in { a: 1, b: 2, c: 3 }) {
        switch (p) { case 'b': continue outer; default: acc += p; }
    }
    for (var [k2, v2] of new Map([[1, 'x'], [2, 'y']])) acc += k2 + v2;
    out.push(acc + `${Math.max(...[3, 9, 4])}`);
    // A deep recursion ends in the RangeError T0 gives, and the engine goes on.
    function down(n) { return n === 0 ? 0 : 1 + down(n - 1); }
    try { down(1e6); out.push('no limit'); } catch (e) { out.push(e instanceof RangeError); }
    out.push(down(1000));
    return out.join(',');
})()
)JS";

// Each template at its edges (js/jit/Baseline.cpp): int32 arithmetic that
// overflows or makes -0, remainders and shifts, comparisons taken both ways
// and fused with their jumps, ToBoolean of every kind, the dead zone and a
// const in a register and in a closure's environment, `this` before super(),
// arguments past the last, element reads off holes and the end, a named
// property across shapes, a dictionary, a getter and a frozen object, and a
// loop taken into machine code at a back-edge. The answers are node's.
constexpr std::string_view edges_script = R"JS(
(function () {
    var out = [];
    function show(v) { return Object.is(v, -0) ? '-0' : String(v); }
    // int32 arithmetic at its edges: overflow to doubles, -0, remainders,
    // shifts past 31, every comparison taken and not.
    function arith(a, b) {
        return [a + b, a - b, a * b, a % b, a & b, a | b, a ^ b, a << b, a >> b, a < b, a <= b, a > b, a >= b, a == b, a === b, a != b, a !== b].map(show).join(' ');
    }
    var pairs = [[1, 2], [2147483647, 1], [-2147483648, 1], [-2147483648, -1], [65536, 65536], [0, -5], [-7, 3], [7, -3], [7, 0], [-8, 4], [5, 33], [-1, 31], [3, 3], [1.5, 2], [0, 0]];
    for (var p = 0; p < pairs.length; p++) out.push(arith(pairs[p][0], pairs[p][1]));
    // ++ and -- at the ends of the int32 range, and on non-numbers.
    var big = 2147483647, small = -2147483648, s = '5', u;
    big++; small--; s++; u++;
    out.push([big, small, s, u].map(show).join(' '));
    // Fused compare-and-jump in both directions, and ToBoolean of every kind.
    var taken = '';
    var values = [0, 1, -1, 2147483647, -0, 0.5, NaN, '', 'x', null, undefined, true, false, {}, []];
    for (var i = 0; i < values.length; i++) {
        var v = values[i];
        if (v) taken += 'T'; else taken += 'F';
        taken += v && 'a' || 'b';
        taken += (v ?? 'n') === 'n' ? 'N' : '-';
        if (i < 3) taken += '<'; else taken += '>';
        if (i >= 7 === false) taken += 'e';
    }
    out.push(taken);
    // Locals in their dead zone, a const written, closures over let.
    try { (function () { x; let x = 1; })(); } catch (e) { out.push(e.name); }
    try { (function () { const c = 1; c = 2; })(); } catch (e) { out.push(e.name); }
    try { (function () { var f = function () { return y; }; f(); let y; })(); } catch (e) { out.push('scoped ' + e.name); }
    try { (function () { const k = 1; var f = function () { k = 3; }; f(); })(); } catch (e) { out.push('scoped ' + e.name); }
    var counter = (function () { let n = 0; return function () { n = n + 1; return n; }; })();
    counter(); counter();
    out.push(counter());
    // this: a derived constructor before super(), a method, a plain call.
    class Base { constructor() { this.k = 1; } }
    class Derived extends Base { constructor() { try { this.k; } catch (e) { out.push('this ' + e.name); } super(); } }
    new Derived();
    out.push((function () { return this; }).call(7) + '');
    // Arguments past the last, and the arguments object beside them.
    function args(a, b, c) { return [a, b, c, arguments.length].map(show).join(','); }
    out.push(args(1) + ' ' + args(1, 2, 3, 4));
    // Element reads: holes, past the end, negative, strings and objects.
    var arr = [1, , 3];
    var reads = [];
    for (var j = -1; j < 5; j++) reads.push(show(arr[j]));
    Array.prototype[1] = 'proto';
    reads.push(arr[1]);
    delete Array.prototype[1];
    reads.push('abc'[1], ({ 0: 'o' })[0], new Uint8Array([9])[0]);
    out.push(reads.join(','));
    // A named property's cache across shapes, a dictionary, a getter, a
    // frozen object written, a prototype's property shadowed.
    function getx(o) { return o.x; }
    function setx(o, v) { o.x = v; return o.x; }
    var shapes = [{ x: 1 }, { y: 20, x: 2 }, Object.create({ x: 3 }), { get x() { return 4; } }];
    var dict = { x: 5 }; for (var d = 0; d < 100; d++) dict['k' + d] = d; delete dict.k0;
    shapes.push(dict);
    var got = [];
    for (var r = 0; r < 3; r++) for (var q = 0; q < shapes.length; q++) got.push(getx(shapes[q]));
    var frozen = Object.freeze({ x: 1 });
    got.push(setx({ x: 1 }, 2), setx(frozen, 9), setx(shapes[2], 6), Object.getPrototypeOf(shapes[2]).x);
    try { (function () { 'use strict'; frozen.x = 3; })(); } catch (e) { got.push(e.name); }
    out.push(got.join(','));
    // A long loop entered mid-way in machine code (the tier-up at a
    // back-edge), with a call in it.
    var total = 0;
    function addOne(t) { return t + 1; }
    for (var w = 0; w < 5000; w++) { total = addOne(total); if (w % 1000 === 999) total = total * 2 % 1000003; }
    out.push(total);
    return out.join('\n');
})()
)JS";

constexpr std::string_view edges_expected = R"(
3 -1 2 1 0 3 3 4 0 true true false false false false true true
2147483648 2147483646 2147483647 0 1 2147483647 2147483646 -2 1073741823 false false true true false false true true
-2147483647 -2147483649 -2147483648 -0 0 -2147483647 -2147483647 0 -1073741824 true true false false false false true true
-2147483649 -2147483647 2147483648 -0 -2147483648 -1 2147483647 0 -1 true true false false false false true true
131072 0 4294967296 0 65536 65536 0 65536 65536 false true false true true true false false
-5 5 -0 0 0 -5 -5 0 0 false false true true false false true true
-4 -10 -21 -1 1 -5 -6 -56 -1 true true false false false false true true
4 10 -21 1 5 -1 -6 -536870912 0 false false true true false false true true
7 7 0 NaN 0 7 7 7 7 false false true true false false true true
-4 -12 -32 -0 0 -4 -4 -128 -1 true true false false false false true true
38 -28 165 5 1 37 36 10 2 true true false false false false true true
30 -32 -31 -1 31 -1 -32 -2147483648 -1 true true false false false false true true
6 0 9 0 3 3 0 24 0 false true false true true true false false
3.5 -0.5 3 1.5 0 3 3 4 0 true true false false false false true true
0 0 0 NaN 0 0 0 0 0 false true false true true true false false
2147483648 -2147483649 6 NaN
Fb-<eTa-<eTa-<eTa->eFb->eTa->eFb->eFb->Ta->FbN>FbN>Ta->Fb->Ta->Ta->
ReferenceError
TypeError
scoped ReferenceError
scoped TypeError
3
this ReferenceError
7
1,undefined,undefined,1 1,2,3,4
undefined,1,undefined,3,undefined,undefined,proto,b,o,9
1,2,3,4,5,1,2,3,4,5,1,2,3,4,5,2,1,6,3,TypeError
62000
)";

// An async body: awaited at several instructions and resumed from the job
// queue.
constexpr std::string_view async_script = R"JS(
var log = [];
async function inner(x) { await null; return x * 2; }
async function outer() { var a = await inner(1); var b = await inner(a); log.push(a, b); try { await Promise.reject(new Error('no')); } catch (e) { log.push(e.message); } return 'ok'; }
outer().then(function (v) { log.push(v); });
)JS";


struct Run {
    std::string main;
    std::string edges;
    std::size_t blocks = 0;
};

Run run(js::jit::Mode mode)
{
    js::jit::set_mode(mode);
    js::Interpreter in;
    in.heap().set_stress(true);
    Run ran;
    ran.main = test::eval_string(in, script);
    test::run_js(in, async_script);
    in.run_jobs([&in](js::Value const& thrown) {
        test::fail("a job threw: " + in.describe(thrown), __FILE__, __LINE__);
    });
    ran.main += "|" + test::eval_string(in, "log.join(',')");
    ran.edges = test::eval_string(in, edges_script);
    ran.blocks = in.account().blocks_compiled_to_machine;
    return ran;
}

#if defined(_WIN32)
// Win64 unwinds a frame of the code by the data written beside it: from an
// address in the body, over a frame laid out as the prologue lays it out —
// the shadow space and `next` under the five pushes, then rbp and the
// return address — the system finds the caller's return address and stack
// pointer and every register the prologue saved. (The code is never run:
// its layout of the engine is left at zero.)
void check_unwind_data()
{
    js::CodeBlock block;
    block.code.push_back(js::Instruction { js::Opcode::Nop });
    block.code.push_back(js::Instruction { js::Opcode::Return });
    std::unique_ptr<js::jit::Code> const code = js::jit::compile(block, nullptr, js::jit::Layout {}, js::jit::Helpers {});
    CHECK(code != nullptr);
    if (code == nullptr)
        return;
    auto const body = reinterpret_cast<DWORD64>(code->table[0]);
    DWORD64 image = 0;
    PRUNTIME_FUNCTION const function = RtlLookupFunctionEntry(body, &image, nullptr);
    CHECK(function != nullptr);
    if (function == nullptr)
        return;
    DWORD64 stack[16] = {};
    stack[5] = 0x1515; // r15
    stack[6] = 0x1414; // r14
    stack[7] = 0x1313; // r13
    stack[8] = 0x1212; // r12
    stack[9] = 0xb0b0; // rbx
    stack[10] = 0xbbbb; // rbp
    stack[11] = 0x401234; // the return address
    CONTEXT context = {};
    context.Rip = body;
    context.Rsp = reinterpret_cast<DWORD64>(&stack[0]);
    PVOID handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, body, function, &context, &handler_data, &establisher, nullptr);
    CHECK_EQ(static_cast<std::uint64_t>(context.Rip), std::uint64_t { 0x401234 });
    CHECK_EQ(static_cast<std::uint64_t>(context.Rsp), static_cast<std::uint64_t>(reinterpret_cast<DWORD64>(&stack[12])));
    CHECK_EQ(static_cast<std::uint64_t>(context.R15), std::uint64_t { 0x1515 });
    CHECK_EQ(static_cast<std::uint64_t>(context.R14), std::uint64_t { 0x1414 });
    CHECK_EQ(static_cast<std::uint64_t>(context.R13), std::uint64_t { 0x1313 });
    CHECK_EQ(static_cast<std::uint64_t>(context.R12), std::uint64_t { 0x1212 });
    CHECK_EQ(static_cast<std::uint64_t>(context.Rbx), std::uint64_t { 0xb0b0 });
    CHECK_EQ(static_cast<std::uint64_t>(context.Rbp), std::uint64_t { 0xbbbb });
}
#endif

// A loop with no end but the host's: each back-edge spends a step of the
// interrupt budget in machine code as in T0, so asked every thousand steps
// and saying yes the third time, the loop is stopped at 3,000.
void check_interrupt(js::jit::Mode mode)
{
    js::jit::set_mode(mode);
    js::Interpreter in;
    int polls = 0;
    in.set_interrupt([&polls] { return ++polls >= 3; }, 1000);
    test::JsRun const ran = test::run_js(in, "var i = 0; try { for (; i < 10000000; i++) {} } catch (e) { 'caught' }");
    CHECK(!ran.ok);
    CHECK(in.terminated());
    CHECK_EQ(polls, 3);
    in.clear_termination();
    CHECK_JS_NUMBER(in, "i", 3000);
}

}

int main()
{
    Run const t0 = run(js::jit::Mode::Off);
    CHECK_EQ(t0.blocks, std::size_t { 0 });
    CHECK_EQ(t0.main, std::string("3998000,6765,RangeError:deep,finally,caught TypeError,0126789done,6,3-2-1,21,ac1x2y9,true,1000|2,4,no,ok"));
    CHECK_EQ("\n" + t0.edges + "\n", std::string(edges_expected));
    check_interrupt(js::jit::Mode::Off);
    if constexpr (js::jit::available) {
        // Every block compiled at its first run; then compiled once hot,
        // at a threshold low enough that loops are taken in mid-run.
        Run const eager = run(js::jit::Mode::Eager);
        CHECK_EQ(eager.main, t0.main);
        CHECK_EQ(eager.edges, t0.edges);
        CHECK(eager.blocks > 10); // the path is reached
        js::jit::set_threshold(2);
        Run const tiered = run(js::jit::Mode::Tiered);
        CHECK_EQ(tiered.main, t0.main);
        CHECK_EQ(tiered.edges, t0.edges);
        CHECK(tiered.blocks > 10);
        CHECK(tiered.blocks < eager.blocks); // a block run once is never compiled
        check_interrupt(js::jit::Mode::Tiered);
        js::jit::set_threshold(0);
        check_interrupt(js::jit::Mode::Eager);
#if defined(_WIN32)
        check_unwind_data();
#endif
    }
    js::jit::set_mode(js::jit::Mode::Off);
    return ::sashfold::test::report("test_js_jit");
}
