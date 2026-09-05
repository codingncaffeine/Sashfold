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

namespace sashfold::js {

class Heap;

struct ParseError {
    SourcePosition position;
    std::string message;
};

struct ParseOptions {
    bool strict = false; // the caller's strictness (a direct eval inherits it)
    bool in_function = false; // a direct eval inside a function: `new.target` and `arguments` are in scope
    bool allow_return = false; // for `new Function` bodies
    bool allow_super_property = false; // a direct eval inside a method: `super.x` is in scope
    bool allow_super_call = false; // … inside a derived constructor: `super()` too
    bool in_field_initializer = false; // … inside a class field initializer: `arguments` is an error
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

    // `new Function(p1, …, body)` (§20.2.1.1.1): a program whose only
    // statement is an ExpressionStatement holding the FunctionExpression.
    static std::unique_ptr<Program> parse_function_constructor(Heap&, std::u16string_view parameters,
        std::u16string_view body, ParseError* error);

    static constexpr int max_nesting_depth = 1000;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::optional<ParseError> m_error;
};

// An S-expression rendering of a tree, for tests and the devtools:
// (program (var (x (number 1))) (expr (call (id f) (id x)))).
std::string dump_ast(Program const&);

}
