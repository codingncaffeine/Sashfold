// The scope tree the parser resolves: every binding with its kind, whether
// it is captured and its slot, and every reference as Local (a register),
// Scoped (hops out through materialized scopes, then a slot) or Dynamic (by
// name at run time). The dumps are exact; the invariants any tree must keep
// are checked on every parse here.
//
//   test_js_scopes                  the cases below
//   test_js_scopes --sweep <dir>    parse every .js under dir (test262's
//                                   layout: flags pick module or strict) and
//                                   check the invariants on each tree

#include "Test.h"

#include "js/Ast.h"
#include "js/Heap.h"
#include "js/Parser.h"
#include "js/Strings.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace sashfold;

namespace {

// Atoms are permanent, so one heap serves every parse; it runs under
// stress as every engine test does.
js::Heap& heap()
{
    static js::Heap* const the_heap = [] {
        auto* created = new js::Heap;
        created->set_stress(true);
        return created;
    }();
    return *the_heap;
}

std::unique_ptr<js::Program> parse_program(std::string_view source, js::ParseOptions options, std::string& error)
{
    options.record_references = true;
    js::Parser parser(heap(), js::utf16_from_utf8(source), options);
    std::unique_ptr<js::Program> program = parser.parse_program("<test>");
    if (!program)
        error = parser.error() ? parser.error()->message : std::string("<failed without a message>");
    return program;
}

// The resolution of one reference node, whatever its type.
struct Resolved {
    js::Resolution resolution = js::Resolution::Unresolved;
    std::uint32_t hops = 0;
    std::uint32_t slot = 0;
    js::ScopeInfo const* scope = nullptr;
};

Resolved resolution_of(js::Expression const& node)
{
    auto const read = [](auto const& resolved) {
        return Resolved { resolved.resolution, resolved.hops, resolved.slot, resolved.scope };
    };
    switch (node.type) {
    case js::NodeType::Identifier:
        return read(static_cast<js::Identifier const&>(node));
    case js::NodeType::ThisExpression:
        return read(static_cast<js::ThisExpression const&>(node));
    case js::NodeType::NewTargetExpression:
        return read(static_cast<js::NewTargetExpression const&>(node));
    case js::NodeType::SuperMember:
        return read(static_cast<js::SuperMember const&>(node));
    default:
        return {};
    }
}

// What every resolved tree keeps: each scope reachable from the program;
// captured bindings numbered 0.. in order and counted by environment_size;
// a scope materializing exactly when it is dynamic or has a captured
// binding; every reference resolved, a Local one to an uncaptured binding's
// register of its own function, a Scoped one to a captured binding's slot
// no more hops out than there are materialized scopes on the way, none
// static into a program scope, into any dynamic scope, or out of its
// function through one. Returns the number of faults.
int check_invariants(js::Program const& program, std::string const& where)
{
    int faults = 0;
    auto const fault = [&](std::string const& message) {
        if (faults++ < 5)
            ::sashfold::test::fail(where + ": " + message, __FILE__, __LINE__);
    };
    if (!program.scope)
        fault("no program scope");
    for (js::ScopeInfo const& info : program.scopes()) {
        if (!info.parent && &info != program.scope)
            fault("a scope with no parent");
        std::uint32_t environment = 0;
        for (js::ScopeInfo::Binding const& binding : info.bindings) {
            if (binding.captured && binding.slot != environment++)
                fault("captured slots out of order");
        }
        if (environment != info.environment_size)
            fault("environment_size is not the captured count");
        if (info.materializes != (info.dynamic || environment > 0))
            fault("materializes disagrees with dynamic and the captures");
        for (js::Expression const* node : info.references) {
            Resolved const resolved = resolution_of(*node);
            if (resolved.resolution == js::Resolution::Unresolved) {
                fault("an unresolved reference");
                continue;
            }
            if (resolved.resolution == js::Resolution::Dynamic)
                continue;
            if (!resolved.scope) {
                fault("a static reference without its declaring scope");
                continue;
            }
            js::ScopeInfo::Kind const declaring = resolved.scope->kind;
            if (declaring == js::ScopeInfo::Kind::Program || declaring == js::ScopeInfo::Kind::Module
                || declaring == js::ScopeInfo::Kind::Eval)
                fault("a program binding resolved statically");
            bool found = false;
            for (js::ScopeInfo::Binding const& binding : resolved.scope->bindings) {
                if (binding.slot != resolved.slot)
                    continue;
                if (resolved.resolution == js::Resolution::Local && !binding.captured)
                    found = true;
                if (resolved.resolution == js::Resolution::Scoped && binding.captured)
                    found = true;
            }
            if (!found)
                fault("a reference's slot names no binding of its kind");
            if (resolved.resolution == js::Resolution::Local && resolved.scope->function != info.function)
                fault("a Local reference into another function");
            if (resolved.scope->dynamic)
                fault("a static reference into a dynamic scope");
            if (resolved.scope->function != info.function) {
                for (js::ScopeInfo const* walk = &info; walk && walk != resolved.scope; walk = walk->parent) {
                    if (walk->dynamic)
                        fault("a static reference out through a dynamic scope");
                }
            }
            if (resolved.resolution == js::Resolution::Scoped) {
                std::uint32_t materialized = 0;
                for (js::ScopeInfo const* walk = &info; walk && walk != resolved.scope; walk = walk->parent)
                    materialized += walk->materializes ? 1 : 0;
                // The reference stands in this function's scope or one inside it.
                std::uint32_t inside = 0;
                for (js::ScopeInfo const& other : program.scopes()) {
                    if (other.function == info.function && other.materializes)
                        ++inside;
                }
                if (resolved.hops > materialized + inside)
                    fault("more hops than materialized scopes on the way");
            }
        }
    }
    return faults;
}

// The dump of a source that parses, its invariants checked, or the error.
std::string scopes(std::string_view source, js::ParseOptions options = {})
{
    std::string error;
    std::unique_ptr<js::Program> const program = parse_program(source, options, error);
    if (!program)
        return "syntax error: " + error;
    check_invariants(*program, std::string(source));
    return js::dump_scopes(*program);
}

std::string module_scopes(std::string_view source)
{
    js::ParseOptions options;
    options.module = true;
    return scopes(source, options);
}

void test_hoisting()
{
    // A var read before its declaration is the function's register.
    CHECK_EQ(scopes("function f() { x; var x; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    var x slot 0\n"
        "    reference x local slot 0\n");
    // A block's let captured by a function declared before it: the block
    // materializes; the block function also has the Annex B var.
    CHECK_EQ(scopes("function f() { { function g() { return y; } let y = 1; } }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    var g slot 0\n"
        "    block materializes env 1\n"
        "      function g slot 1\n"
        "      let y captured slot 0\n"
        "      function g\n"
        "        reference y scoped hops 0 slot 0\n");
    // Annex B: the block function reached from the function's top level is
    // the var; inside the block the name is the block's own binding.
    CHECK_EQ(scopes("function f() { { function g() {} g; } return g; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    var g slot 0\n"
        "    reference g local slot 1\n"
        "    reference g local slot 0\n"
        "    block\n"
        "      function g slot 1\n"
        "      function g\n");
    // Strict code hoists nothing out of the block.
    CHECK_EQ(scopes("'use strict'; function f() { { function g() {} } return g; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    reference g dynamic\n"
        "    block\n"
        "      function g slot 0\n"
        "      function g\n");
}

void test_captures()
{
    CHECK_EQ(scopes("function f(a) { return () => a; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f materializes env 1\n"
        "    parameter a captured slot 0\n"
        "    arrow\n"
        "      reference a scoped hops 0 slot 0\n");
    // Hops count only the scopes that materialize: a block whose let
    // nothing captures makes no environment ...
    CHECK_EQ(scopes("function f() { let x; { let y; { () => x; } } }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f materializes env 1\n"
        "    let x captured slot 0\n"
        "    block\n"
        "      let y slot 0\n"
        "      arrow\n"
        "        reference x scoped hops 0 slot 0\n");
    // ... and one whose let is captured does.
    CHECK_EQ(scopes("function f() { let x; { let y; () => y; { () => x; } } }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f materializes env 1\n"
        "    let x captured slot 0\n"
        "    block materializes env 1\n"
        "      let y captured slot 0\n"
        "      arrow\n"
        "        reference y scoped hops 0 slot 0\n"
        "      arrow\n"
        "        reference x scoped hops 1 slot 0\n");
}

void test_arguments()
{
    // A mapped arguments object aliases the parameters: captured.
    CHECK_EQ(scopes("function f(a, b) { return arguments; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f materializes env 2\n"
        "    parameter a captured slot 0\n"
        "    parameter b captured slot 1\n"
        "    arguments slot 0\n"
        "    reference arguments local slot 0\n");
    // A strict function's is unmapped: the parameters stay registers.
    CHECK_EQ(scopes("function f(a, b) { 'use strict'; return arguments; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    parameter a slot 0\n"
        "    parameter b slot 1\n"
        "    arguments slot 2\n"
        "    reference arguments local slot 2\n");
    // So is one with a default, and an arrow's `arguments` is its function's.
    CHECK_EQ(scopes("function f(a = 1) { return () => arguments; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f materializes env 1\n"
        "    parameter a slot 0\n"
        "    arguments captured slot 0\n"
        "    body\n"
        "      arrow\n"
        "        reference arguments scoped hops 0 slot 0\n");
}

void test_this()
{
    // `this` belongs to the nearest non-arrow function; arrows reach it
    // through their closures.
    CHECK_EQ(scopes("function f() { return () => { this; return () => this; }; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f materializes env 1\n"
        "    this captured slot 0\n"
        "    arrow\n"
        "      reference this scoped hops 0 slot 0\n"
        "      arrow\n"
        "        reference this scoped hops 0 slot 0\n");
    CHECK_EQ(scopes("function f() { return this; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    this slot 0\n"
        "    reference this local slot 0\n");
    // `super.m` from an arrow captures the home object and `this`. (The
    // class's own scope is program code's, so by name.)
    CHECK_EQ(scopes("class C { m() { return () => super.m(); } }"),
        "program dynamic materializes env 1\n"
        "  class C captured slot 0\n"
        "  class-name dynamic materializes env 1\n"
        "    class C captured slot 0\n"
        "    function m materializes env 2\n"
        "      this captured slot 0\n"
        "      home-object captured slot 1\n"
        "      arrow\n"
        "        reference super scoped hops 0 slot 1\n");
    // At the top level `this` is the program's: by name.
    CHECK_EQ(scopes("this; () => this;"),
        "program dynamic materializes\n"
        "  reference this dynamic\n"
        "  arrow\n"
        "    reference this dynamic\n");
}

void test_eval()
{
    // A direct eval in g makes g dynamic (every binding captured, every
    // reference in it by name, since the eval may declare a var there). The
    // eval may name any of f's bindings, so they are all captured, but it
    // cannot add one to f: f's own references and those of h, which has no
    // eval, stay static, and f is lent no this, new.target or arguments,
    // since g has its own.
    CHECK_EQ(scopes("function f() { var a; function g() { var b; eval(''); return b + a; }"
                    " function h() { var c; return c + a; } return a; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f materializes env 3\n"
        "    var a captured slot 0\n"
        "    function g captured slot 1\n"
        "    function h captured slot 2\n"
        "    reference a scoped hops 0 slot 0\n"
        "    function g dynamic materializes env 4\n"
        "      this captured slot 0\n"
        "      new.target captured slot 1\n"
        "      arguments captured slot 2\n"
        "      var b captured slot 3\n"
        "      reference eval dynamic\n"
        "      reference b dynamic\n"
        "      reference a dynamic\n"
        "    function h\n"
        "      var c slot 0\n"
        "      reference c local slot 0\n"
        "      reference a scoped hops 0 slot 0\n");
    // A function inside the one with the eval reaches outward through its
    // dynamic scope, so by name.
    CHECK_EQ(scopes("function f() { var a; function g() { eval(''); function h() { return a; } } }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f materializes env 2\n"
        "    var a captured slot 0\n"
        "    function g captured slot 1\n"
        "    function g dynamic materializes env 4\n"
        "      this captured slot 0\n"
        "      new.target captured slot 1\n"
        "      arguments captured slot 2\n"
        "      function h captured slot 3\n"
        "      reference eval dynamic\n"
        "      function h\n"
        "        reference a dynamic\n");
    // An eval in an arrow sees the enclosing function's this, new.target and
    // arguments, which are captured there; the function is not dynamic.
    CHECK_EQ(scopes("function f() { var a; () => eval(''); return a; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f materializes env 4\n"
        "    this captured slot 0\n"
        "    new.target captured slot 1\n"
        "    arguments captured slot 2\n"
        "    var a captured slot 3\n"
        "    reference a scoped hops 0 slot 3\n"
        "    arrow dynamic materializes\n"
        "      reference eval dynamic\n");
    // Only the scopes around the eval are captured: a sibling block keeps
    // its register.
    CHECK_EQ(scopes("function f() { { let p; p; } { let q; (function () { eval(''); }); } }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    reference p local slot 0\n"
        "    block\n"
        "      let p slot 0\n"
        "    block materializes env 1\n"
        "      let q captured slot 0\n"
        "      function (anonymous) dynamic materializes env 3\n"
        "        this captured slot 0\n"
        "        new.target captured slot 1\n"
        "        arguments captured slot 2\n"
        "        reference eval dynamic\n");
    // An eval in a parameter default counts.
    CHECK_EQ(scopes("function f(a = eval('')) { let b; b; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f dynamic materializes env 4\n"
        "    parameter a captured slot 0\n"
        "    this captured slot 1\n"
        "    new.target captured slot 2\n"
        "    arguments captured slot 3\n"
        "    reference eval dynamic\n"
        "    reference b dynamic\n"
        "    body dynamic materializes env 1\n"
        "      let b captured slot 0\n");
    // Eval code resolves nothing: its top scope and its blocks are by name.
    js::ParseOptions eval_code;
    eval_code.eval = true;
    CHECK_EQ(scopes("var a; let b; a; b; c; { let d; d; }", eval_code),
        "eval dynamic materializes env 2\n"
        "  var a captured slot 0\n"
        "  let b captured slot 1\n"
        "  reference a dynamic\n"
        "  reference b dynamic\n"
        "  reference c dynamic\n"
        "  reference d dynamic\n"
        "  block dynamic materializes env 1\n"
        "    let d captured slot 0\n");
}

void test_with()
{
    // A with makes its function dynamic as a direct eval does: the prologue
    // and every lookup stay by name, all its bindings captured.
    CHECK_EQ(scopes("function f(o) { var a; with (o) { a; b; let c; c; } return a; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f dynamic materializes env 2\n"
        "    parameter o captured slot 0\n"
        "    var a captured slot 1\n"
        "    reference o dynamic\n"
        "    reference a dynamic\n"
        "    reference b dynamic\n"
        "    reference c dynamic\n"
        "    reference a dynamic\n"
        "    with dynamic materializes\n"
        "      block dynamic materializes env 1\n"
        "        let c captured slot 0\n");
    // A with in an inner function captures the outer binding it may reach
    // by name, and leaves the outer function static.
    CHECK_EQ(scopes("function f() { let x; let y; function g(o) { with (o) x; } return y; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f materializes env 1\n"
        "    function g slot 0\n"
        "    let x captured slot 0\n"
        "    let y slot 1\n"
        "    reference y local slot 1\n"
        "    function g dynamic materializes env 1\n"
        "      parameter o captured slot 0\n"
        "      reference o dynamic\n"
        "      reference x dynamic\n"
        "      with dynamic materializes\n");
}

void test_block_scopes()
{
    // A catch parameter is a scope of its own, shadowing the var.
    CHECK_EQ(scopes("function f() { var e; try {} catch (e) { e; } return e; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    var e slot 0\n"
        "    reference e local slot 1\n"
        "    reference e local slot 0\n"
        "    catch\n"
        "      catch-parameter e slot 1\n");
    // A pattern's names and defaults resolve in the catch scope.
    CHECK_EQ(scopes("function f() { try {} catch ({ a = b, c }) { return () => a; } }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    reference a scoped hops 0 slot 0\n"
        "    reference b dynamic\n"
        "    reference c local slot 0\n"
        "    catch materializes env 1\n"
        "      catch-parameter a captured slot 0\n"
        "      catch-parameter c slot 0\n"
        "      arrow\n"
        "        reference a scoped hops 0 slot 0\n");
    // A for-let head is a scope, copied per iteration at run time: it
    // materializes when a closure in the body captures the variable ...
    CHECK_EQ(scopes("function f() { for (let i = 0; i < 3; i++) { () => i; } }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    reference i scoped hops 0 slot 0\n"
        "    reference i scoped hops 0 slot 0\n"
        "    for-head materializes env 1\n"
        "      let i captured slot 0\n"
        "      arrow\n"
        "        reference i scoped hops 0 slot 0\n");
    // ... and is a register otherwise.
    CHECK_EQ(scopes("function f() { for (let i = 0; i < 3; i++) { i; } }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    reference i local slot 0\n"
        "    reference i local slot 0\n"
        "    reference i local slot 0\n"
        "    for-head\n"
        "      let i slot 0\n");
    // A case block with a declaration is a block scope.
    CHECK_EQ(scopes("function f(v) { switch (v) { case 1: let q; return () => q; } }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    parameter v slot 0\n"
        "    reference v local slot 0\n"
        "    block materializes env 1\n"
        "      let q captured slot 0\n"
        "      arrow\n"
        "        reference q scoped hops 0 slot 0\n");
}

void test_own_names()
{
    // A class's name inside its body is the class scope's binding.
    CHECK_EQ(scopes("function f() { class C { m() { return C; } } }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    class C slot 0\n"
        "    class-name materializes env 1\n"
        "      class C captured slot 0\n"
        "      function m\n"
        "        reference C scoped hops 0 slot 0\n");
    // A named function expression reads its own name from the scope around it.
    CHECK_EQ(scopes("function f() { var g = function h() { return h; }; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    var g slot 0\n"
        "    function-name materializes env 1\n"
        "      function-name h captured slot 0\n"
        "      function h\n"
        "        reference h scoped hops 0 slot 0\n");
    // In program code both scopes are made by name, and read by name.
    CHECK_EQ(scopes("class C { m() { return C; } }"),
        "program dynamic materializes env 1\n"
        "  class C captured slot 0\n"
        "  class-name dynamic materializes env 1\n"
        "    class C captured slot 0\n"
        "    function m\n"
        "      reference C dynamic\n");
    CHECK_EQ(scopes("var g = function h() { return h; };"),
        "program dynamic materializes env 1\n"
        "  var g captured slot 0\n"
        "  function-name dynamic materializes env 1\n"
        "    function-name h captured slot 0\n"
        "    function h\n"
        "      reference h dynamic\n");
    // new Function's `anonymous` binds nothing, and nothing outside it resolves.
    js::ParseError error;
    std::unique_ptr<js::Program> const program = js::Parser::parse_function_constructor(
        heap(), u"a", u"return a + anonymous + b;", &error, js::DynamicFunctionKind::Normal, true);
    CHECK(program != nullptr);
    if (program) {
        check_invariants(*program, "new Function");
        CHECK_EQ(js::dump_scopes(*program),
            "program dynamic materializes\n"
            "  function anonymous\n"
            "    parameter a slot 0\n"
            "    reference a local slot 0\n"
            "    reference anonymous dynamic\n"
            "    reference b dynamic\n");
    }
}

void test_program_names()
{
    // Script top-level names are global: by name, every one.
    CHECK_EQ(scopes("var a; let b; function c() {} a; b; c; typeof d;"),
        "program dynamic materializes env 3\n"
        "  var a captured slot 0\n"
        "  function c captured slot 1\n"
        "  let b captured slot 2\n"
        "  reference a dynamic\n"
        "  reference b dynamic\n"
        "  reference c dynamic\n"
        "  reference d dynamic\n"
        "  function c\n");
    // So are a module's, imports included.
    CHECK_EQ(module_scopes("import { q } from './m.js'; var a; let b; export function c() { return a + b + q; }"),
        "module dynamic materializes env 4\n"
        "  var a captured slot 0\n"
        "  function c captured slot 1\n"
        "  import q captured slot 2\n"
        "  let b captured slot 3\n"
        "  function c\n"
        "    reference a dynamic\n"
        "    reference b dynamic\n"
        "    reference q dynamic\n");
    // typeof of an undeclared name inside a function.
    CHECK_EQ(scopes("function f() { return typeof nothing; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    reference nothing dynamic\n");
}

void test_parameter_scope()
{
    // Parameters with expressions are a scope apart from the body's vars:
    // the default reads the earlier parameter, the body's `a` is its var.
    CHECK_EQ(scopes("function f(a, b = a) { var a; return a + b; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f\n"
        "    parameter a slot 0\n"
        "    parameter b slot 1\n"
        "    reference a local slot 0\n"
        "    reference a local slot 2\n"
        "    reference b local slot 1\n"
        "    body\n"
        "      var a slot 2\n");
    // A closure in a default captures the parameter, not the body.
    CHECK_EQ(scopes("function f(a, g = () => a) { let a2; return g; }"),
        "program dynamic materializes env 1\n"
        "  function f captured slot 0\n"
        "  function f materializes env 1\n"
        "    parameter a captured slot 0\n"
        "    parameter g slot 0\n"
        "    reference g local slot 0\n"
        "    body\n"
        "      let a2 slot 1\n"
        "    arrow\n"
        "      reference a scoped hops 0 slot 0\n");
}

// The resolution on the nodes themselves, for the compiler to read.
void test_nodes()
{
    std::string error;
    std::unique_ptr<js::Program> const program
        = parse_program("function f(o) { let x = 1; { let y; () => y; x; } with (o) x; }", {}, error);
    CHECK(program != nullptr);
    if (!program)
        return;
    auto const* declaration = static_cast<js::FunctionDeclaration const*>(program->body[0]);
    js::FunctionNode const* f = declaration->function;
    CHECK(f->scope != nullptr);
    CHECK(f->dynamic); // a with inside
    CHECK_EQ(f->scope->kind, js::ScopeInfo::Kind::Function);
    auto const* block = static_cast<js::BlockStatement const*>(f->body[1]);
    CHECK(block->scope != nullptr);
    CHECK(block->scope->parent == f->scope);
    CHECK(block->scope->materializes);
    auto const* with = static_cast<js::WithStatement const*>(f->body[2]);
    CHECK(with->scope != nullptr && with->scope->kind == js::ScopeInfo::Kind::With);
    // The with makes the function dynamic: even outside it, by name.
    auto const* read_x = static_cast<js::Identifier const*>(static_cast<js::ExpressionStatement const*>(block->body[2])->expression);
    CHECK_EQ(read_x->resolution, js::Resolution::Dynamic);
    auto const* with_x = static_cast<js::Identifier const*>(static_cast<js::ExpressionStatement const*>(with->body)->expression);
    CHECK_EQ(with_x->resolution, js::Resolution::Dynamic);
    // Registers: none left once the with captured everything.
    CHECK_EQ(f->register_count, 0u);
    CHECK(f->body_scope == f->scope);
    CHECK_EQ(f->scopes.size(), 3u); // the function, the block, the with

    // Without the with: the block materializes for y, and x, which an arrow
    // also captures, is one materialized scope out.
    std::unique_ptr<js::Program> const plain
        = parse_program("function g(a = 0) { let x = 1; { let y; () => y; x; } () => x; }", {}, error);
    CHECK(plain != nullptr);
    if (!plain)
        return;
    js::FunctionNode const* g = static_cast<js::FunctionDeclaration const*>(plain->body[0])->function;
    CHECK(!g->dynamic);
    CHECK(g->body_scope != g->scope); // split by the default
    CHECK(g->body_scope->parent == g->scope);
    auto const* inner = static_cast<js::BlockStatement const*>(g->body[1]);
    CHECK(inner->scope->materializes);
    auto const* plain_x = static_cast<js::Identifier const*>(static_cast<js::ExpressionStatement const*>(inner->body[2])->expression);
    CHECK_EQ(plain_x->resolution, js::Resolution::Scoped);
    CHECK_EQ(plain_x->hops, 1u);
    CHECK(plain_x->scope == g->body_scope);
    CHECK_EQ(g->scopes.size(), 3u); // the function, its body, the block
    CHECK_EQ(g->register_count, 1u); // the parameter
}

int sweep(char const* directory)
{
    // A parse makes nothing but permanent atoms, so stress would only
    // collect them again at every allocation, quadratic in a file with
    // thousands of names.
    heap().set_stress(false);
    std::size_t parsed = 0;
    std::size_t skipped = 0;
    int faults = 0;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(directory)) {
        std::string const path = entry.path().string();
        if (!entry.is_regular_file() || entry.path().extension() != ".js" || path.find("_FIXTURE") != std::string::npos)
            continue;
        std::ifstream file(path, std::ios::binary);
        std::string const text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        js::ParseOptions options;
        options.module = text.find("flags: [module]") != std::string::npos || text.find("flags: [module,") != std::string::npos;
        options.strict = text.find("onlyStrict") != std::string::npos;
        std::string error;
        std::unique_ptr<js::Program> const program = parse_program(text, options, error);
        if (!program) {
            ++skipped;
            continue;
        }
        ++parsed;
        faults += check_invariants(*program, path);
    }
    std::printf("swept %zu parsed, %zu not parsed, %d faults\n", parsed, skipped, faults);
    return faults == 0 ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--sweep") == 0)
        return sweep(argv[2]);
    test_hoisting();
    test_captures();
    test_arguments();
    test_this();
    test_eval();
    test_with();
    test_block_scopes();
    test_own_names();
    test_program_names();
    test_parameter_scope();
    test_nodes();
    return ::sashfold::test::report("test_js_scopes");
}
