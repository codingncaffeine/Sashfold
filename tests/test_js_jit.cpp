// The baseline compiler's machine code (js/jit, _plans/js-JIT-DESIGN.md §5):
// in this slice every instruction calls the interpreter's own handler, so
// what a script computes must be what T0 computes, test by test, whatever
// path the frame takes through the code — calls the run loop makes inline
// and their returns (the code left and entered again at the next
// instruction), generators and async bodies suspended and resumed at any
// instruction, a throw caught by a handler of the frame or of a caller, a
// recursion as deep as T0 allows. And the path is reached: blocks are given
// machine code in `eager` mode, none when the tier is off.

#include "JsTest.h"

#include "js/jit/Baseline.h"

#include <cstddef>
#include <string>
#include <string_view>

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

// An async body: awaited at several instructions and resumed from the job
// queue.
constexpr std::string_view async_script = R"JS(
var log = [];
async function inner(x) { await null; return x * 2; }
async function outer() { var a = await inner(1); var b = await inner(a); log.push(a, b); try { await Promise.reject(new Error('no')); } catch (e) { log.push(e.message); } return 'ok'; }
outer().then(function (v) { log.push(v); });
)JS";

std::string run(js::jit::Mode mode, std::size_t* blocks)
{
    js::jit::set_mode(mode);
    js::Interpreter in;
    in.heap().set_stress(true);
    std::string const result = test::eval_string(in, script);
    test::run_js(in, async_script);
    in.run_jobs([&in](js::Value const& thrown) {
        test::fail("a job threw: " + in.describe(thrown), __FILE__, __LINE__);
    });
    std::string const async_result = test::eval_string(in, "log.join(',')");
    *blocks = in.account().blocks_compiled_to_machine;
    return result + "|" + async_result;
}

}

int main()
{
    std::size_t blocks_off = 0;
    std::string const t0 = run(js::jit::Mode::Off, &blocks_off);
    CHECK_EQ(blocks_off, std::size_t { 0 });
    CHECK_EQ(t0, std::string("3998000,6765,RangeError:deep,finally,caught TypeError,0126789done,6,3-2-1,21,ac1x2y9,true,1000|2,4,no,ok"));
    if constexpr (js::jit::available) {
        std::size_t blocks_eager = 0;
        std::string const machine = run(js::jit::Mode::Eager, &blocks_eager);
        CHECK_EQ(machine, t0);
        CHECK(blocks_eager > 10); // the path is reached
    }
    js::jit::set_mode(js::jit::Mode::Off);
    return ::sashfold::test::report("test_js_jit");
}
