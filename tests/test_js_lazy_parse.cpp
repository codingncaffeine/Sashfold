// Lazy parsing (the lazy plan's L8, js-LAZY-PARSE-DESIGN.md): the first
// parse checks a function's body and lets go of it, keeping a stub; the
// body is parsed again at the function's first call. What must hold:
//
// - once every stub is parsed again, the program is the one an eager parse
//   makes — the same scope tree, the same tree, every reference resolved
//   the same way (the global mark too, which the scope dump does not show);
// - every early error inside a body let go of is still reported by the
//   first parse, with the eager parse's message;
// - a script computes the same in both modes, and a body is parsed late
//   once, at its first call;
// - a function in parentheses, one called where it is written, and one
//   with a direct eval or a with in it keep their bodies; an arrow and a
//   class constructor are never let go of.
//
//   test_js_lazy_parse                  the cases below
//   test_js_lazy_parse --sweep <dir>    the first point over every .js under
//                                       dir (test262's layout: flags pick
//                                       module or strict)

#include "JsTest.h"

#include "js/Ast.h"
#include "js/Heap.h"
#include "js/Object.h"
#include "js/Parser.h"
#include "js/Strings.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace sashfold;

namespace {

// Atoms are permanent, so one heap serves every parse.
js::Heap& heap()
{
    static js::Heap* const the_heap = new js::Heap;
    return *the_heap;
}

struct Parsed {
    std::unique_ptr<js::Program> program;
    std::string error;
};

Parsed parse(std::string_view source, bool lazy, bool module = false, bool strict = false)
{
    js::ParseOptions options;
    options.module = module;
    options.strict = strict;
    options.record_references = true;
    options.lazy_functions = lazy;
    js::Parser parser(heap(), js::utf16_from_utf8(source), options);
    Parsed parsed;
    parsed.program = parser.parse_program("<test>");
    if (!parsed.program)
        parsed.error = parser.error() ? parser.error()->message : std::string("<failed without a message>");
    return parsed;
}

std::size_t stubs(js::Program const& program)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < program.function_count(); ++i)
        count += program.function_at(i)->lazy ? 1 : 0;
    return count;
}

// Every stub parsed again — the ones inside each as they appear — which is
// what calling every function once does.
bool complete(js::Program& program, std::string& error)
{
    for (std::size_t i = 0; i < program.function_count(); ++i) {
        js::FunctionNode& function = *program.function_at(i);
        if (!function.lazy)
            continue;
        js::ParseError parse_error;
        if (!js::Parser::parse_lazy_function(heap(), function, &parse_error, true)) {
            error = parse_error.message;
            return false;
        }
    }
    return true;
}

char const* resolution_name(js::Resolution resolution)
{
    switch (resolution) {
    case js::Resolution::Unresolved: return "unresolved";
    case js::Resolution::Local: return "local";
    case js::Resolution::Scoped: return "scoped";
    case js::Resolution::Dynamic: return "dynamic";
    }
    return "?";
}

template<typename Node>
std::string resolved(Node const& node)
{
    std::string line = std::string(" ") + resolution_name(node.resolution) + " hops " + std::to_string(node.hops) + " slot " + std::to_string(node.slot);
    if (node.scope)
        line += " in scope at " + std::to_string(node.scope->start) + " kind " + std::to_string(static_cast<int>(node.scope->kind));
    return line;
}

// Every reference the program recorded, one line each and sorted: where it
// stands, what it is, how it resolved, the declaring scope by its start and
// kind, and the global mark.
std::string resolutions(js::Program const& program)
{
    std::vector<std::string> lines;
    for (js::ScopeInfo const& info : program.scopes()) {
        for (js::Expression const* node : info.references) {
            std::string line = std::to_string(node->position.offset) + ' ';
            switch (node->type) {
            case js::NodeType::Identifier: {
                auto const& identifier = static_cast<js::Identifier const&>(*node);
                line += js::utf8_from_utf16(identifier.name->view()) + resolved(identifier) + (identifier.global ? " global" : "");
                break;
            }
            case js::NodeType::ThisExpression:
                line += "this" + resolved(static_cast<js::ThisExpression const&>(*node));
                break;
            case js::NodeType::NewTargetExpression:
                line += "new.target" + resolved(static_cast<js::NewTargetExpression const&>(*node));
                break;
            case js::NodeType::SuperMember:
                line += "super" + resolved(static_cast<js::SuperMember const&>(*node));
                break;
            default:
                line += "?";
                break;
            }
            lines.push_back(std::move(line));
        }
    }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (std::string const& line : lines)
        out += line + '\n';
    return out;
}

// The first point above for one source; false (with what differed) when it
// fails, so the sweep can count.
bool same_as_eager(std::string_view source, bool module, bool strict, std::string& why, std::size_t* let_go = nullptr, bool* rejected = nullptr)
{
    Parsed const eager = parse(source, false, module, strict);
    Parsed lazy = parse(source, true, module, strict);
    if (!eager.program || !lazy.program) {
        if (rejected)
            *rejected = true;
        if (eager.error != lazy.error) {
            why = "the parses disagree on the error: eager \"" + eager.error + "\", lazy \"" + lazy.error + "\"";
            return false;
        }
        return true;
    }
    if (let_go)
        *let_go = stubs(*lazy.program);
    std::string error;
    if (!complete(*lazy.program, error)) {
        why = "a stub did not parse again: " + error;
        return false;
    }
    if (std::string const a = js::dump_scopes(*eager.program), b = js::dump_scopes(*lazy.program); a != b) {
        why = "scopes differ\n--- eager\n" + a + "--- lazy\n" + b;
        return false;
    }
    if (std::string const a = js::dump_ast(*eager.program), b = js::dump_ast(*lazy.program); a != b) {
        why = "trees differ\n--- eager\n" + a + "\n--- lazy\n" + b;
        return false;
    }
    if (std::string const a = resolutions(*eager.program), b = resolutions(*lazy.program); a != b) {
        why = "resolutions differ\n--- eager\n" + a + "--- lazy\n" + b;
        return false;
    }
    return true;
}

void check_same(std::string_view name, std::string_view source, std::size_t at_least_let_go, bool module = false)
{
    std::string why;
    std::size_t let_go = 0;
    if (!same_as_eager(source, module, false, why, &let_go))
        test::fail(std::string(name) + ": " + why, __FILE__, __LINE__);
    else
        test::check(true, "same as eager", __FILE__, __LINE__);
    // The path is reached: the case lets go of what it means to.
    if (let_go < at_least_let_go)
        test::fail(std::string(name) + ": let go of " + std::to_string(let_go) + " bodies, expected at least " + std::to_string(at_least_let_go), __FILE__, __LINE__);
}

void test_same_as_eager()
{
    check_same("closures", R"JS(
        var x = 1;
        function outer(a) { var b = 2; function inner(c) { return a + b + c + x + y; } return inner; }
        let y = outer(1)(2);
    )JS", 1);
    check_same("nested captures", R"JS(
        function one(a) {
            let b = a;
            function two(c) {
                { let d = c; try { throw d; } catch (e) {
                    for (let i = 0; i < 2; i++) { var three = function (f) { return a + b + c + d + e + f + i; }; }
                } }
                return three;
            }
            return two;
        }
    )JS", 1);
    // A lazy body whose own scopes materialize (arrows capture its
    // parameter and a block's let) naming its outer function's local: the
    // hops count those scopes.
    check_same("own scope materializes", R"JS(
        function outer() { var o = 1; function lazy(p) { var q = () => p; { let r = 2; var s = () => r + o; } return o + q() + s(); } return lazy; }
    )JS", 1);
    check_same("own name", "var f = function g(n) { return n ? g(n - 1) : 0; }; var h = function () { return h; };", 2);
    check_same("parameter scope", R"JS(
        function f(a = function () { return b + c; }, b = 2) { var c = 3; var b = 4; return a(); }
        function g(p, q = () => p) { function r() { return p + q(); } return r; }
    )JS", 2);
    check_same("with", R"JS(
        function outer(o) { with (o) { var h = function () { return x + h; }; } return h; }
        with ({}) { var k = function () { return k + zz; }; }
    )JS", 2);
    check_same("direct eval", R"JS(
        function outer() { eval("var q = 1"); function inner() { return q + r; } var r; return inner; }
        function around() { function inside() { return eval("1") + s; } var s; return inside; }
    )JS", 1);
    check_same("eval at the top", "eval('var z = 1'); function g() { return z + w; } var w;", 1);
    check_same("arrows inside", R"JS(
        function f() { var self = this; return () => [this, arguments, self, new.target, () => this]; }
        var o = { m() { return () => super.toString(); } };
    )JS", 2);
    check_same("methods and accessors", R"JS(
        var o = {
            m() { return super.toString(); },
            get p() { return this.q; },
            set p(v) { this.q = v; },
            async *gen() { yield await 1; },
            [Symbol.iterator]() { return this; },
            'quoted'(a, b) { return a + b; },
        };
    )JS", 6);
    check_same("classes", R"JS(
        class C extends Object {
            #x = 1;
            static #s() { return 2; }
            field = () => this.#x;
            static { var inBlock = function () { return C; }; }
            constructor() { super(); this.y = function () { return 3; }; }
            m() { return this.#x + C.#s.length; }
            get g() { return () => super.toString(); }
            static n(a) { class D { k() { return a + C; } } return D; }
        }
    )JS", 4);
    check_same("generators and async", R"JS(
        function* g(a) { yield a; yield* g(a - 1); }
        async function af(p) { return await p; }
        async function* ag() { for await (const v of []) yield v; }
        var e = async function () { await null; };
    )JS", 4);
    check_same("strictness", R"JS(
        function sloppy() { return function () { return this; }; }
        function strict() { "use strict"; return function () { return this; }; }
        function later(a, b) { "use strict"; return a + b; }
    )JS", 3);
    check_same("strict program", "'use strict'; function f(a) { return function g() { return a; }; }", 1);
    check_same("annex b", R"JS(
        function f() { { function g() { return 1; } } return g(); }
        function h() { if (true) function k() { return 2; } return k; }
        function m() { let n; { function n2() { return n; } } }
    )JS", 3);
    check_same("module", R"JS(
        import { a } from "x";
        export function f() { return a + b; }
        export default function () { return f(); }
        const b = 2;
        export const c = function () { return a; };
    )JS", 3, true);
    check_same("control flow", R"JS(
        function f(a) {
            outer: for (let i = 0; i < 3; i++) {
                switch (a) { case 1: continue outer; default: break outer; }
            }
            do { a--; } while (a > 0);
            for (var k in {}) {} for (const v of []) {}
            return a;
        }
    )JS", 1);
    check_same("patterns", R"JS(
        function f({ a, b: [c, ...d] }, ...rest) { var { x = a } = rest[0] || {}; [a, c] = [c, a]; return x + c + d.length; }
        function g([p] = [1], { q } = { q: p }) { return p + q; }
    )JS", 2);
    check_same("templates and patterns of text", R"JS(
        function f(t) { return tag`a${t}b` + `c${t}` + /x+/g.source + 0x1f + 1n; }
    )JS", 1);
    check_same("duplicate and shadowed names", R"JS(
        function f(a, a) { return a; }
        function g(arguments) { return arguments; }
        function h() { var arguments; return arguments; }
        function k() { return arguments.length; }
        function l(a) { arguments[0] = 2; return a; }
    )JS", 5);
    check_same("functions in blocks, catch and for heads", R"JS(
        try { throw 1; } catch (e) { var h = function () { return e; }; }
        for (let i = 0; i < 2; i++) { var fs = function () { return i; }; }
        { let blocked = 1; var bf = function () { return blocked; }; }
        switch (1) { case 1: let sw = 2; var sf = function () { return sw; }; }
    )JS", 4);
    check_same("many names", [] {
        // Enough bindings in one scope for the lookups to go by index.
        std::string source = "function big() {";
        for (int i = 0; i < 40; ++i)
            source += "var v" + std::to_string(i) + " = " + std::to_string(i) + ";";
        source += "return function () { return v0 + v17 + v39 + nothing; }; }";
        for (int i = 0; i < 40; ++i)
            source += "var g" + std::to_string(i) + ";";
        source += "function user() { return g3 + g38 + big; }";
        return source;
    }(), 2);
}

// The first parse reports what the eager one does, from inside a body it
// lets go of.
void test_early_errors()
{
    struct Case {
        char const* source;
        bool module;
    };
    Case const cases[] = {
        { "function f() { let a; let a; }", false },
        { "function f() { 'use strict'; with (o) {} }", false },
        { "function f() { break; }", false },
        { "function f(a, a) { 'use strict'; }", false },
        { "var o = { m() { super(); } };", false },
        { "class C { m() { return this.#x; } }", false },
        { "function f() { return 1 +; }", false },
        { "function f() { yield = 1; } 'use strict'", false },
        { "function f() { 'use strict'; var yield; }", false },
        { "function f() { await: 1; }", true },
        { "function f() { return function g() { const c; }; }", false },
        { "var o = { get p(a) { return a; } };", false },
        { "function f() { 'use strict'; return 010; }", false },
        { "function f() { new.target; } function g() { return () => { super.x; }; }", false },
        { "function f() { label: label: ; }", false },
        { "function f(...r = []) {}", false },
    };
    for (Case const& c : cases) {
        Parsed const eager = parse(c.source, false, c.module);
        Parsed const lazy = parse(c.source, true, c.module);
        CHECK_EQ(lazy.program == nullptr, eager.program == nullptr);
        CHECK_EQ(lazy.error, eager.error);
    }
    // And a sound program parses in both.
    CHECK(parse("function f() { yield = 1; }", true).program != nullptr);
}

// Which bodies are let go of: dump_ast shows a stub as (function name lazy).
void test_which_bodies()
{
    Parsed const parsed = parse(R"JS(
        (function wrapped() { function inner() { return 1; } })();
        f(function argument() { return 2; });
        !function banged() { var k; }();
        var called = function calledAtOnce() { return 3; }();
        var later = function notYet() { return 4; };
        (async function asyncWrapped() {})();
        function withEval() { eval(''); function underEval() {} }
        function withWith(o) { with (o) {} }
        var arrow = () => { return 5; };
        class K { constructor() { this.a = 1; } method() { return 6; } }
    )JS", true);
    CHECK(parsed.program != nullptr);
    if (!parsed.program)
        return;
    std::string const tree = js::dump_ast(*parsed.program);
    auto const has = [&](std::string_view text) { return tree.find(text) != std::string::npos; };
    CHECK(!has("(function wrapped lazy)"));
    CHECK(has("(function inner lazy)")); // inside a kept body, a stub all the same
    CHECK(has("(function argument lazy)"));
    CHECK(!has("(function banged lazy)"));
    CHECK(!has("(function calledAtOnce lazy)"));
    CHECK(has("(function notYet lazy)"));
    CHECK(!has("(async function asyncWrapped lazy)"));
    CHECK(!has("(function withEval lazy)"));
    CHECK(has("(function underEval lazy)"));
    CHECK(!has("(function withWith lazy)"));
    CHECK(!has("lazy) (arrow")); // an arrow keeps its body
    CHECK(has("(function method lazy)"));
    CHECK(!has("(function K lazy)")); // the constructor
}

// What a script computes, in both modes: closures over names of every kind,
// `this`, `arguments`, `super`, private names, generators, a function's
// own properties before and after its first call.
constexpr std::string_view behaviour_script = R"JS(
(function () {
    var out = [];
    var top = 10;
    function counter(start) { var n = start; return { up() { return ++n; }, get value() { return n; } }; }
    var c = counter(top);
    c.up(); c.up();
    out.push(c.value);
    function adder(a) { return function (b) { return function (c) { return a + b + c + top; }; }; }
    out.push(adder(1)(2)(3));
    function args() { return arguments.length + ':' + Array.prototype.join.call(arguments, '-'); }
    out.push(args(1, 2, 3));
    function mapped(a) { arguments[0] = 'changed'; return a; }
    out.push(mapped('kept'));
    function thisOf() { 'use strict'; return this; }
    out.push(String(thisOf()));
    var o = { name: 'o', who() { return () => this.name; }, toString() { return 'o:' + super.toString(); } };
    out.push(o.who()(), String(o));
    class A { #secret = 7; reveal() { return this.#secret; } static make() { return new A(); } }
    class B extends A { reveal() { return super.reveal() * 2; } }
    out.push(A.make().reveal(), new B().reveal());
    function* gen(n) { while (n > 0) yield n--; }
    out.push([...gen(3)].join(''));
    function shape(a, b = 2, ...c) { return a; }
    out.push(shape.length, shape.name, String(shape).length);
    shape(1);
    out.push(shape.length, shape.name, String(shape).length);
    function hoisted() { return typeof later; function later() {} }
    out.push(hoisted());
    function blockFn() { { function inBlock() { return 'b'; } } return inBlock(); }
    out.push(blockFn());
    function withEval() { var e = 1; return eval('e + 1'); }
    out.push(withEval());
    function reads(o2) { with (o2) { return function () { return x; }; } }
    out.push(reads({ x: 'with' })());
    function recursive(n) { return n <= 1 ? 1 : n * recursive(n - 1); }
    out.push(recursive(5));
    var named = function self(n) { return n ? self(n - 1) + 1 : 0; };
    out.push(named(4));
    return out.join(',');
})()
)JS";

void test_behaviour()
{
    js::set_lazy_natives(false);
    std::string eager_result;
    {
        js::Interpreter in;
        in.heap().set_stress(true);
        eager_result = test::eval_string(in, behaviour_script);
        CHECK_EQ(in.account().bodies_parsed_late, std::size_t { 0 });
    }
    js::set_lazy_natives(true);
    js::Interpreter in;
    in.heap().set_stress(true);
    std::string const lazy_result = test::eval_string(in, behaviour_script);
    CHECK_EQ(lazy_result, eager_result);
    CHECK_EQ(lazy_result, std::string("12,16,3:1-2-3,changed,undefined,o,o:[object Object],7,14,321,1,shape,44,1,shape,44,function,b,2,with,120,4"));
    CHECK(in.account().bodies_parsed_late > 10); // the path is reached

    // A body is parsed late once, at its first call, and not when its
    // closure is made or its source is read.
    std::size_t const before = in.account().bodies_parsed_late;
    test::run_js(in, "function once() { return 1; } var made = function () {}; String(once); once.length;");
    CHECK_EQ(in.account().bodies_parsed_late, before);
    test::run_js(in, "once(); once(); once();");
    CHECK_EQ(in.account().bodies_parsed_late, before + 1);
    // An exception thrown from a body parsed late is the body's own.
    CHECK_EQ(test::eval_string(in, "function thrower() { null.x; } try { thrower(); 'no' } catch (e) { e.constructor.name }"), std::string("TypeError"));
}

int sweep(char const* directory)
{
    std::size_t parsed = 0;
    std::size_t let_go = 0;
    std::size_t rejected_by_both = 0;
    int faults = 0;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(directory)) {
        std::string const path = entry.path().string();
        if (!entry.is_regular_file() || entry.path().extension() != ".js" || path.find("_FIXTURE") != std::string::npos)
            continue;
        std::ifstream file(path, std::ios::binary);
        std::string const text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        bool const module = text.find("flags: [module]") != std::string::npos || text.find("flags: [module,") != std::string::npos;
        bool const strict = text.find("onlyStrict") != std::string::npos;
        std::string why;
        std::size_t stubs_here = 0;
        bool rejected = false;
        ++parsed;
        if (!same_as_eager(text, module, strict, why, &stubs_here, &rejected)) {
            if (faults++ < 10)
                std::printf("%s: %s\n", path.c_str(), why.substr(0, 2000).c_str());
        }
        let_go += stubs_here;
        rejected_by_both += rejected ? 1 : 0;
    }
    std::printf("swept %zu files (%zu a syntax error in both modes alike), %zu bodies let go of and parsed again, %d differ\n", parsed, rejected_by_both, let_go, faults);
    return faults == 0 && let_go > 0 ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--sweep") == 0)
        return sweep(argv[2]);
    test_same_as_eager();
    test_early_errors();
    test_which_bodies();
    test_behaviour();
    return ::sashfold::test::report("test_js_lazy_parse");
}
