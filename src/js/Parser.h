#pragma once

// The parser (§13–§16): recursive descent with precedence climbing,
// automatic semicolon insertion, the strict-mode directive and its early
// errors, and the declaration lists each scope's instantiation reads.
// Nesting is capped — a script is input-controlled data and must not be
// able to recurse the parser off the stack.

#include "js/Ast.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::js {

class Heap;

struct ParseError {
    SourcePosition position;
    std::string message;
};

struct ParseOptions {
    // The Module goal (§16.2.1): strict throughout, `await` a keyword with
    // top-level await allowed, import and export declarations at the top
    // level, `import.meta` an expression, no HTML-like comments, and the
    // record tables filled on the Program.
    bool module = false;
    bool strict = false; // the caller's strictness (a direct eval inherits it)
    bool in_function = false; // a direct eval inside a function: `new.target` and `arguments` are in scope
    bool allow_return = false; // for `new Function` bodies
    bool allow_super_property = false; // a direct eval inside a method: `super.x` is in scope
    bool allow_super_call = false; // … inside a derived constructor: `super()` too
    bool in_field_initializer = false; // … inside a class field initializer: `arguments` is an error
    std::vector<std::u16string> private_names; // … inside a class body: the private names in scope, # included
    // Eval code (direct or indirect): its program scope is an eval scope,
    // and nothing outside it is resolved.
    bool eval = false;
    // The wrapper parse_function_constructor builds: its function's name
    // `anonymous` binds nothing (§20.2.1.1.1 makes no own-name scope).
    bool function_constructor = false;
    // Keep every reference on its function's scope in source order, for
    // dump_scopes and the tests; a plain parse keeps only the resolutions
    // on the nodes.
    bool record_references = false;
};

class Parser {
public:
    // Names and literals are interned in the heap; the source is copied
    // into the Program, which outlives the parser.
    Parser(Heap&, std::u16string source, ParseOptions = {});
    ~Parser();
    Parser(Parser const&) = delete;
    Parser& operator=(Parser const&) = delete;

    // The program, or null with error() set.
    std::unique_ptr<Program> parse_program(std::string name = "");
    std::optional<ParseError> const& error() const { return m_error; }

    // `new Function(p1, …, body)` and its GeneratorFunction, AsyncFunction
    // and AsyncGeneratorFunction kin (§20.2.1.1.1): a program whose only
    // statement is an ExpressionStatement holding the function expression.
    static std::unique_ptr<Program> parse_function_constructor(Heap&, std::u16string_view parameters,
        std::u16string_view body, ParseError* error, DynamicFunctionKind kind = DynamicFunctionKind::Normal,
        bool record_references = false);

    static constexpr int max_nesting_depth = 1000;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::optional<ParseError> m_error;
};

// An S-expression rendering of a tree, for tests and the devtools:
// (program (var (x (number 1))) (expr (call (id f) (id x)))).
std::string dump_ast(Program const&);

// The scope tree the parser resolved, one line per scope, binding and
// reference, indented by nesting; siblings in source order:
//
//   program dynamic materializes env 1         function f(a) {
//     function f captured slot 0                   var b; b;
//     function f materializes env 1                return () => a + g;
//       parameter a captured slot 0            }
//       var b slot 0
//       reference b local slot 0
//       arrow
//         reference a scoped hops 0 slot 0
//         reference g dynamic
//
// A scope line is its kind (a function's with its name, `(anonymous)`, or
// `arrow`), then `dynamic`, `materializes` and `env <size>` as they apply.
// A binding line is `<kind> <name>`, `captured` when it is, and its slot:
// an environment index when captured, a register otherwise. A reference
// line (a function's or program's own references, in source order, when
// the parse recorded them) is `reference <name>` and its resolution:
// `local slot <r>`, `scoped hops <h> slot <s>` or `dynamic`. The implicit
// names print as this, new.target and super (the home object).
std::string dump_scopes(Program const&);

}
