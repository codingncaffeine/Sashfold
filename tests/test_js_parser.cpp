#include "Test.h"

#include "js/Ast.h"
#include "js/Heap.h"
#include "js/Parser.h"
#include "js/Strings.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace sashfold;

namespace {

// Atoms are permanent, so one heap serves every parse.
js::Heap& heap()
{
    static js::Heap the_heap;
    return the_heap;
}

struct Parsed {
    std::unique_ptr<js::Program> program;
    std::optional<js::ParseError> error;
};

Parsed parse_source(std::string_view source, js::ParseOptions options = {})
{
    js::Parser parser(heap(), js::utf16_from_utf8(source), options);
    Parsed parsed;
    parsed.program = parser.parse_program("<test>");
    if (!parsed.program)
        parsed.error = parser.error();
    return parsed;
}

// The dump of a source that parses, or the message of the error that
// stopped it.
std::string parse(std::string_view source, js::ParseOptions options = {})
{
    Parsed parsed = parse_source(source, options);
    if (parsed.program)
        return js::dump_ast(*parsed.program);
    return parsed.error ? parsed.error->message : std::string("<failed without a message>");
}

std::string parse_strict(std::string_view source)
{
    js::ParseOptions options;
    options.strict = true;
    return parse(source, options);
}

// "line:column message" of the error, or "parsed".
std::string error_at(std::string_view source)
{
    Parsed parsed = parse_source(source);
    if (parsed.program)
        return "parsed";
    if (!parsed.error)
        return "<no error>";
    return std::to_string(parsed.error->position.line) + ":" + std::to_string(parsed.error->position.column) + " "
        + parsed.error->message;
}

std::string names(std::vector<js::JsString*> const& list)
{
    std::string out;
    for (js::JsString const* name : list) {
        if (!out.empty())
            out += ' ';
        out += name->to_utf8();
    }
    return out;
}

// The plain names of a parameter list ("a b"); a pattern prints as [].
std::string parameter_names(js::FunctionNode const& fn)
{
    std::string out;
    for (js::Parameter const& parameter : fn.parameters) {
        if (!out.empty())
            out += ' ';
        out += parameter.name ? parameter.name->to_utf8() : std::string("[]");
    }
    return out;
}

// "a const:b" for let a, const b.
std::string lexical_names(std::vector<std::pair<js::JsString*, bool>> const& list)
{
    std::string out;
    for (auto const& [name, is_const] : list) {
        if (!out.empty())
            out += ' ';
        if (is_const)
            out += "const:";
        out += name->to_utf8();
    }
    return out;
}

std::string function_names(std::vector<js::FunctionDeclaration const*> const& list)
{
    std::string out;
    for (js::FunctionDeclaration const* declaration : list) {
        if (!out.empty())
            out += ' ';
        out += declaration->function->name->to_utf8();
    }
    return out;
}

// The FunctionNode behind statement `index` of a body: a declaration,
// or the function/arrow expression of an expression statement, a var
// initializer, an assignment's value or a return's argument.
js::FunctionNode const* function_in(std::vector<js::Statement*> const& body, std::size_t index)
{
    if (index >= body.size())
        return nullptr;
    js::Statement const* statement = body[index];
    js::Expression const* expression = nullptr;
    switch (statement->type) {
    case js::NodeType::FunctionDeclaration:
        return static_cast<js::FunctionDeclaration const*>(statement)->function;
    case js::NodeType::ExpressionStatement:
        expression = static_cast<js::ExpressionStatement const*>(statement)->expression;
        break;
    case js::NodeType::ReturnStatement:
        expression = static_cast<js::ReturnStatement const*>(statement)->argument;
        break;
    case js::NodeType::VariableDeclaration: {
        auto const& declarators = static_cast<js::VariableDeclaration const*>(statement)->declarations;
        if (!declarators.empty())
            expression = declarators[0].init;
        break;
    }
    default:
        break;
    }
    while (expression) {
        if (expression->type == js::NodeType::FunctionExpression)
            return static_cast<js::FunctionExpression const*>(expression)->function;
        if (expression->type == js::NodeType::ArrowFunction)
            return static_cast<js::ArrowFunction const*>(expression)->function;
        if (expression->type == js::NodeType::AssignmentExpression)
            expression = static_cast<js::AssignmentExpression const*>(expression)->value;
        else
            break;
    }
    return nullptr;
}

std::string source_of(js::Program const& program, js::FunctionNode const& fn)
{
    return js::utf8_from_utf16(std::u16string_view(program.source).substr(fn.source_start, fn.source_end - fn.source_start));
}

std::string program_of(std::string_view statements)
{
    return "(program " + std::string(statements) + ")";
}

std::string expression_of(std::string_view expression)
{
    return "(program (expr " + std::string(expression) + "))";
}

void test_precedence()
{
    CHECK_EQ(parse("a + b * c"), expression_of("(binary + (id a) (binary * (id b) (id c)))"));
    CHECK_EQ(parse("a - b - c"), expression_of("(binary - (binary - (id a) (id b)) (id c))"));
    CHECK_EQ(parse("a * b / c % d"), expression_of("(binary % (binary / (binary * (id a) (id b)) (id c)) (id d))"));
    CHECK_EQ(parse("a ** b ** c"), expression_of("(binary ** (id a) (binary ** (id b) (id c)))"));
    CHECK_EQ(parse("a ** b * c"), expression_of("(binary * (binary ** (id a) (id b)) (id c))"));
    CHECK_EQ(parse("a * b ** c"), expression_of("(binary * (id a) (binary ** (id b) (id c)))"));
    CHECK_EQ(parse("a << b + c"), expression_of("(binary << (id a) (binary + (id b) (id c)))"));
    CHECK_EQ(parse("a >>> b >> c"), expression_of("(binary >> (binary >>> (id a) (id b)) (id c))"));
    CHECK_EQ(parse("a < b << c"), expression_of("(binary < (id a) (binary << (id b) (id c)))"));
    CHECK_EQ(parse("a >= b instanceof c"), expression_of("(binary instanceof (binary >= (id a) (id b)) (id c))"));
    CHECK_EQ(parse("a in b"), expression_of("(binary in (id a) (id b))"));
    CHECK_EQ(parse("a == b < c"), expression_of("(binary == (id a) (binary < (id b) (id c)))"));
    CHECK_EQ(parse("a !== b != c"), expression_of("(binary != (binary !== (id a) (id b)) (id c))"));
    CHECK_EQ(parse("a & b === c"), expression_of("(binary & (id a) (binary === (id b) (id c)))"));
    CHECK_EQ(parse("a ^ b & c"), expression_of("(binary ^ (id a) (binary & (id b) (id c)))"));
    CHECK_EQ(parse("a | b ^ c"), expression_of("(binary | (id a) (binary ^ (id b) (id c)))"));
    CHECK_EQ(parse("a && b | c"), expression_of("(logical && (id a) (binary | (id b) (id c)))"));
    CHECK_EQ(parse("a || b && c"), expression_of("(logical || (id a) (logical && (id b) (id c)))"));
    CHECK_EQ(parse("a ?? b ?? c"), expression_of("(logical ?? (logical ?? (id a) (id b)) (id c))"));
    CHECK_EQ(parse("a ?? b | c"), expression_of("(logical ?? (id a) (binary | (id b) (id c)))"));
    CHECK_EQ(parse("(a || b) ?? c"), expression_of("(logical ?? (logical || (id a) (id b)) (id c))"));
    CHECK_EQ(parse("a ?? (b && c)"), expression_of("(logical ?? (id a) (logical && (id b) (id c)))"));
    CHECK_EQ(parse("a || b ?? c"), "Mixing '?" "?' with '||' or '&&' without parentheses is not allowed");
    CHECK_EQ(parse("a ?? b && c"), "Mixing '?" "?' with '||' or '&&' without parentheses is not allowed");
    CHECK_EQ(parse("a ? b : c ? d : e"), expression_of("(cond (id a) (id b) (cond (id c) (id d) (id e)))"));
    CHECK_EQ(parse("a || b ? c : d"), expression_of("(cond (logical || (id a) (id b)) (id c) (id d))"));
    CHECK_EQ(parse("a = b = c"), expression_of("(assign = (id a) (assign = (id b) (id c)))"));
    CHECK_EQ(parse("a += b -= c"), expression_of("(assign += (id a) (assign -= (id b) (id c)))"));
    CHECK_EQ(parse("a **= 2"), expression_of("(assign **= (id a) (number 2))"));
    CHECK_EQ(parse("a ?" "?= b"), expression_of("(assign ??" "= (id a) (id b))"));
    CHECK_EQ(parse("a ||= b &&= c"), expression_of("(assign ||= (id a) (assign &&= (id b) (id c)))"));
    CHECK_EQ(parse("a >>>= 1; a <<= 1; a >>= 1; a &= 1; a |= 1; a ^= 1; a %= 1; a /= 1; a *= 1"),
        program_of("(expr (assign >>>= (id a) (number 1))) (expr (assign <<= (id a) (number 1))) "
                   "(expr (assign >>= (id a) (number 1))) (expr (assign &= (id a) (number 1))) "
                   "(expr (assign |= (id a) (number 1))) (expr (assign ^= (id a) (number 1))) "
                   "(expr (assign %= (id a) (number 1))) (expr (assign /= (id a) (number 1))) "
                   "(expr (assign *= (id a) (number 1)))"));
    CHECK_EQ(parse("a = b ? c : d"), expression_of("(assign = (id a) (cond (id b) (id c) (id d)))"));
    CHECK_EQ(parse("a, b, c"), expression_of("(seq (id a) (id b) (id c))"));
    CHECK_EQ(parse("a = b, c"), expression_of("(seq (assign = (id a) (id b)) (id c))"));
    CHECK_EQ(parse("1 = 2"), "Invalid left-hand side in assignment");
    CHECK_EQ(parse("a + b = c"), "Invalid left-hand side in assignment");
    CHECK_EQ(parse("f() = 1"), "Invalid left-hand side in assignment");
    CHECK_EQ(parse("(a) = 1"), expression_of("(assign = (id a) (number 1))"));
    CHECK_EQ(parse("a.b.c = 1"), expression_of("(assign = (member (member (id a) b) c) (number 1))"));
}

void test_unary()
{
    CHECK_EQ(parse("typeof a + b"), expression_of("(binary + (unary typeof (id a)) (id b))"));
    CHECK_EQ(parse("!a && b"), expression_of("(logical && (unary ! (id a)) (id b))"));
    CHECK_EQ(parse("- - a"), expression_of("(unary - (unary - (id a)))"));
    CHECK_EQ(parse("-a ** 2"), "Unary operator used immediately before exponentiation expression. Parenthesis must be used to disambiguate operator precedence");
    CHECK_EQ(parse("typeof a ** 2"), "Unary operator used immediately before exponentiation expression. Parenthesis must be used to disambiguate operator precedence");
    CHECK_EQ(parse("(-a) ** 2"), expression_of("(binary ** (unary - (id a)) (number 2))"));
    CHECK_EQ(parse("a ** -b"), expression_of("(binary ** (id a) (unary - (id b)))"));
    CHECK_EQ(parse("++a ** 2"), expression_of("(binary ** (update ++ prefix (id a)) (number 2))"));
    CHECK_EQ(parse("void 0"), expression_of("(unary void (number 0))"));
    CHECK_EQ(parse("~a"), expression_of("(unary ~ (id a))"));
    CHECK_EQ(parse("+a"), expression_of("(unary + (id a))"));
    CHECK_EQ(parse("delete a.b"), expression_of("(unary delete (member (id a) b))"));
    CHECK_EQ(parse("a++ + ++b"), expression_of("(binary + (update ++ postfix (id a)) (update ++ prefix (id b)))"));
    CHECK_EQ(parse("a-- - --b"), expression_of("(binary - (update -- postfix (id a)) (update -- prefix (id b)))"));
    CHECK_EQ(parse("++a.b"), expression_of("(update ++ prefix (member (id a) b))"));
    CHECK_EQ(parse("++a++"), "Invalid left-hand side expression in prefix operation");
    CHECK_EQ(parse("a + b++"), expression_of("(binary + (id a) (update ++ postfix (id b)))"));
    CHECK_EQ(parse("(a + b)++"), "Invalid left-hand side expression in postfix operation");
}

void test_asi()
{
    CHECK_EQ(parse("a\n++b"), program_of("(expr (id a)) (expr (update ++ prefix (id b)))"));
    CHECK_EQ(parse("a++\nb"), program_of("(expr (update ++ postfix (id a))) (expr (id b))"));
    CHECK_EQ(parse("function f() { return\nx }"), program_of("(function f () (return) (expr (id x)))"));
    CHECK_EQ(parse("x = y\n(1)"), expression_of("(assign = (id x) (call (id y) (number 1)))"));
    CHECK_EQ(parse("a\n[0]"), expression_of("(index (id a) (number 0))"));
    CHECK_EQ(parse("{}"), program_of("(block)"));
    CHECK_EQ(parse("{ a: 1 }"), program_of("(block (label a (expr (number 1))))"));
    CHECK_EQ(parse("({ a: 1 })"), expression_of("(object (init \"a\" (number 1)))"));
    CHECK_EQ(parse("throw\nx"), "Illegal newline after throw");
    CHECK_EQ(parse("a\nb"), program_of("(expr (id a)) (expr (id b))"));
    CHECK_EQ(parse("a b"), "Unexpected identifier 'b'");
    CHECK_EQ(parse("do x; while (y) z"), program_of("(do (expr (id x)) (id y)) (expr (id z))"));
    CHECK_EQ(parse("var a = 1\nvar b = 2"), program_of("(var (a (number 1))) (var (b (number 2)))"));
    CHECK_EQ(parse("if (a) b\nelse c"), program_of("(if (id a) (expr (id b)) (expr (id c)))"));
    CHECK_EQ(parse("a = b\n/c/g.test(d)"), expression_of("(assign = (id a) (binary / (binary / (id b) (id c)) (call (member (id g) test) (id d))))"));
    CHECK_EQ(parse("x\n/* multi\nline */ ++y"), program_of("(expr (id x)) (expr (update ++ prefix (id y)))"));
    CHECK_EQ(parse("for (;;) break\nx"), program_of("(for - - - (break)) (expr (id x))"));
    CHECK_EQ(parse("a: while (1) { continue a\nb }"), program_of("(label a (while (number 1) (block (continue a) (expr (id b)))))"));
    CHECK_EQ(parse("x = 1 <!-- comment\ny = 2"), program_of("(expr (assign = (id x) (number 1))) (expr (assign = (id y) (number 2)))"));
    CHECK_EQ(parse("x = 1\n--> comment\ny = 2"), program_of("(expr (assign = (id x) (number 1))) (expr (assign = (id y) (number 2)))"));
}

void test_regex_versus_division()
{
    CHECK_EQ(parse("a / b / c"), expression_of("(binary / (binary / (id a) (id b)) (id c))"));
    CHECK_EQ(parse("/a/g.test(x)"), expression_of("(call (member (regex /a/g) test) (id x))"));
    CHECK_EQ(parse("x = /=/"), expression_of("(assign = (id x) (regex /=/))"));
    CHECK_EQ(parse("if (a) /re/.test(b)"), program_of("(if (id a) (expr (call (member (regex /re/) test) (id b))))"));
    CHECK_EQ(parse("{} /re/"), program_of("(block) (expr (regex /re/))"));
    CHECK_EQ(parse("a = {} / 2"), expression_of("(assign = (id a) (binary / (object) (number 2)))"));
    CHECK_EQ(parse("f(/x/, /y/i)"), expression_of("(call (id f) (regex /x/) (regex /y/i))"));
    CHECK_EQ(parse("[/x/]"), expression_of("(array (regex /x/))"));
    CHECK_EQ(parse("typeof /x/"), expression_of("(unary typeof (regex /x/))"));
    CHECK_EQ(parse("a ? /x/ : /y/"), expression_of("(cond (id a) (regex /x/) (regex /y/))"));
    CHECK_EQ(parse("x = a++ / 2"), expression_of("(assign = (id x) (binary / (update ++ postfix (id a)) (number 2)))"));
    CHECK_EQ(parse("`t` / 2"), expression_of("(binary / (template (\"t\") ()) (number 2))"));
    CHECK_EQ(parse("x = function() {} / 2"), expression_of("(assign = (id x) (binary / (function ()) (number 2)))"));
    CHECK_EQ(parse("function f() {} /re/.test(x)"), program_of("(function f ()) (expr (call (member (regex /re/) test) (id x)))"));
    CHECK_EQ(parse("x = /[/]/"), expression_of("(assign = (id x) (regex /[/]/))"));
    CHECK_EQ(parse("x = /a\\/b/"), expression_of("(assign = (id x) (regex /a\\/b/))"));
    CHECK_EQ(parse("/a/gg"), "Invalid regular expression flags");
    CHECK_EQ(parse("x = /abc"), "Invalid regular expression: missing /");
    CHECK_EQ(parse("a /= 2"), expression_of("(assign /= (id a) (number 2))"));
    CHECK_EQ(parse("this / 2"), expression_of("(binary / this (number 2))"));
}

void test_arrows()
{
    CHECK_EQ(parse("x => x"), expression_of("(arrow (x) (id x))"));
    CHECK_EQ(parse("(a, b) => a + b"), expression_of("(arrow (a b) (binary + (id a) (id b)))"));
    CHECK_EQ(parse("() => 1"), expression_of("(arrow () (number 1))"));
    CHECK_EQ(parse("x => { return x }"), expression_of("(arrow (x) (return (id x)))"));
    CHECK_EQ(parse("(a) => ({})"), expression_of("(arrow (a) (object))"));
    CHECK_EQ(parse("(a, b)"), expression_of("(seq (id a) (id b))"));
    CHECK_EQ(parse("(a,) => a"), expression_of("(arrow (a) (id a))"));
    CHECK_EQ(parse("a => b => c"), expression_of("(arrow (a) (arrow (b) (id c)))"));
    CHECK_EQ(parse("x = y => y * 2"), expression_of("(assign = (id x) (arrow (y) (binary * (id y) (number 2))))"));
    CHECK_EQ(parse("f(x => x, 1)"), expression_of("(call (id f) (arrow (x) (id x)) (number 1))"));
    CHECK_EQ(parse("x => x ? 1 : 2"), expression_of("(arrow (x) (cond (id x) (number 1) (number 2)))"));
    CHECK_EQ(parse("(a = 1) => 1"), expression_of("(arrow ((= a (number 1))) (number 1))"));
    CHECK_EQ(parse("([a]) => 1"), expression_of("(arrow ((array-pattern (id a))) (number 1))"));
    CHECK_EQ(parse("(a, ...b) => b"), expression_of("(arrow (a (... b)) (id b))"));
    CHECK_EQ(parse("({a, b: [c] = []}, d = a) => d"), expression_of("(arrow ((object-pattern (\"a\" (id a)) (\"b\" (= (array-pattern (id c)) (array)))) (= d (id a))) (id d))"));
    // A template inside the head does not confuse the lookahead.
    CHECK_EQ(parse("(a = `${(1)}`) => a"), expression_of("(arrow ((= a (template (\"\" \"\") ((number 1))))) (id a))"));
    CHECK_EQ(parse("(a = `${1}`) => a"), expression_of("(arrow ((= a (template (\"\" \"\") ((number 1))))) (id a))"));
    CHECK_EQ(parse("(...a, b) => 1"), "Rest parameter must be last formal parameter");
    CHECK_EQ(parse("(...a = []) => 1"), "Rest parameter may not have a default initializer");
    CHECK_EQ(parse("(a = 1) => { 'use strict'; }"), "Illegal 'use strict' directive in function with non-simple parameter list");
    CHECK_EQ(parse("([a, a]) => 1"), "Duplicate parameter name not allowed in this context");
    CHECK_EQ(parse("(a, b)\n=> 1"), "Unexpected token '=>'");
    CHECK_EQ(parse("x\n=> 1"), "Unexpected token '=>'");
    CHECK_EQ(parse("()"), "Unexpected token ')'");
    CHECK_EQ(parse("(a, a) => 1"), "Duplicate parameter name not allowed in this context");
    CHECK_EQ(parse("async x => x"), "async functions are not supported yet");
    CHECK_EQ(parse("async (x) => x"), "async functions are not supported yet");
    CHECK_EQ(parse("async(x)"), expression_of("(call (id async) (id x))"));
    CHECK_EQ(parse("x => {}()"), "Unexpected token '('");
    // In a for-head an arrow's concise body inherits the `in` exclusion
    // (§15.3 ConciseBody[?In]), so the `in` belongs to the loop.
    CHECK_EQ(parse("for (var f = x => x in o);"), program_of("(for-in (var (f (arrow (x) (id x)))) (id o) (empty))"));
    CHECK_EQ(parse("for (var f = (x => x in o);;);"), program_of("(for (var (f (arrow (x) (binary in (id x) (id o))))) - - (empty))"));
    Parsed parsed = parse_source("function f() { return () => this }");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        js::FunctionNode const* outer = function_in(parsed.program->body, 0);
        js::FunctionNode const* arrow = outer ? function_in(outer->body, 0) : nullptr;
        CHECK(outer && outer->uses_this);
        CHECK(arrow && arrow->uses_this && arrow->is_arrow && !arrow->is_constructable);
    }
    parsed = parse_source("var f = () => arguments");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        js::FunctionNode const* arrow = function_in(parsed.program->body, 0);
        CHECK(arrow && arrow->uses_arguments);
        CHECK_EQ(source_of(*parsed.program, *arrow), "() => arguments");
    }
}

void test_templates()
{
    CHECK_EQ(parse("`abc`"), expression_of("(template (\"abc\") ())"));
    CHECK_EQ(parse("`a${b}c`"), expression_of("(template (\"a\" \"c\") ((id b)))"));
    CHECK_EQ(parse("`${a}${b}`"), expression_of("(template (\"\" \"\" \"\") ((id a) (id b)))"));
    CHECK_EQ(parse("`a${`b${c}d`}e`"), expression_of("(template (\"a\" \"e\") ((template (\"b\" \"d\") ((id c)))))"));
    CHECK_EQ(parse("`x${ {a: 1}.a }y`"), expression_of("(template (\"x\" \"y\") ((member (object (init \"a\" (number 1))) a)))"));
    CHECK_EQ(parse("`t\\tab \\\"q\\\"`"), expression_of("(template (\"t\tab \\\"q\\\"\") ())"));
    CHECK_EQ(parse("`${a + b}`"), expression_of("(template (\"\" \"\") ((binary + (id a) (id b))))"));
    CHECK_EQ(parse("`${f(1)}` + `${g}`"), expression_of("(binary + (template (\"\" \"\") ((call (id f) (number 1)))) (template (\"\" \"\") ((id g))))"));
    CHECK_EQ(parse("tag`x`"), "tagged templates are not supported yet");
    CHECK_EQ(parse("a.b`x`"), "tagged templates are not supported yet");
    CHECK_EQ(parse("`\\unicode`"), "Invalid escape sequence in template literal");
    CHECK_EQ(parse("`\\01`"), "Invalid escape sequence in template literal");
    CHECK_EQ(parse("`abc"), "Unterminated template literal");
    CHECK_EQ(parse("`${a`"), "Unterminated template literal");
    CHECK_EQ(parse("`${a b}`"), "Unexpected identifier 'b'");
    Parsed parsed = parse_source("`a\\n${x}`");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        auto const* statement = static_cast<js::ExpressionStatement const*>(parsed.program->body[0]);
        auto const* literal = static_cast<js::TemplateLiteral const*>(statement->expression);
        CHECK_EQ(literal->raw.size(), std::size_t(2));
        CHECK_EQ(literal->raw[0]->to_utf8(), "a\\n");
        CHECK_EQ(literal->cooked[0]->to_utf8(), "a\n");
    }
}

void test_object_literals()
{
    CHECK_EQ(parse("({a: 1, 'b': 2, 3: c, [d]: e, f, g() {}, get h() { return 1 }, set h(v) {}, __proto__: p})"),
        expression_of("(object (init \"a\" (number 1)) (init \"b\" (number 2)) (init \"3\" (id c)) (computed (id d) (id e)) "
                      "(init \"f\" (id f)) (init \"g\" (function g ())) (get \"h\" (function h () (return (number 1)))) "
                      "(set \"h\" (function h (v))) (proto (id p)))"));
    CHECK_EQ(parse("({__proto__: a, __proto__: b})"), "Duplicate __proto__ fields are not allowed in object literals");
    CHECK_EQ(parse("({__proto__: a, \"__proto__\": b})"), "Duplicate __proto__ fields are not allowed in object literals");
    CHECK_EQ(parse("({__proto__})"), expression_of("(object (init \"__proto__\" (id __proto__)))"));
    CHECK_EQ(parse("({['__proto__']: x, __proto__: y})"), expression_of("(object (computed (string \"__proto__\") (id x)) (proto (id y)))"));
    CHECK_EQ(parse("({__proto__() {}, __proto__: y})"), expression_of("(object (init \"__proto__\" (function __proto__ ())) (proto (id y)))"));
    CHECK_EQ(parse("({get: 1, set: 2, get() {}, set() {}, async: 3})"),
        expression_of("(object (init \"get\" (number 1)) (init \"set\" (number 2)) (init \"get\" (function get ())) (init \"set\" (function set ())) (init \"async\" (number 3)))"));
    CHECK_EQ(parse("({get, set})"), expression_of("(object (init \"get\" (id get)) (init \"set\" (id set)))"));
    CHECK_EQ(parse("({get 'a'() {}, set 1(v) {}, get [k]() {}})"),
        expression_of("(object (get \"a\" (function a ())) (set \"1\" (function 1 (v))) (get computed (id k) (function ())))"));
    CHECK_EQ(parse("({get a(x) {}})"), "Getter must not have any formal parameters");
    CHECK_EQ(parse("({set a() {}})"), "Setter must have exactly one formal parameter");
    CHECK_EQ(parse("({set a(x, y) {}})"), "Setter must have exactly one formal parameter");
    CHECK_EQ(parse("({a = 1})"), "Invalid shorthand property initializer");
    CHECK_EQ(parse("f({a = 1})"), "Invalid shorthand property initializer");
    CHECK_EQ(parse("[{a = 1}]"), "Invalid shorthand property initializer");
    CHECK_EQ(parse("({a = 1}.b = 2)"), "Invalid shorthand property initializer");
    CHECK_EQ(parse("x = {a = 1}"), "Invalid shorthand property initializer");
    CHECK_EQ(parse("({__proto__: 1, __proto__: 2})"), "Duplicate __proto__ fields are not allowed in object literals");
    CHECK_EQ(parse("({__proto__: a, __proto__: b} = o)"), expression_of("(assign = (object-pattern (\"__proto__\" (id a)) (\"__proto__\" (id b))) (id o))"));
    CHECK_EQ(parse("({if: 1, class: 2, this: 3})"), expression_of("(object (init \"if\" (number 1)) (init \"class\" (number 2)) (init \"this\" (number 3)))"));
    CHECK_EQ(parse("({if})"), "Unexpected token '}'");
    CHECK_EQ(parse("({1.5: x, 0x10: y, 1e3: z, .5: w})"), expression_of("(object (init \"1.5\" (id x)) (init \"16\" (id y)) (init \"1000\" (id z)) (init \"0.5\" (id w)))"));
    CHECK_EQ(parse("({a: 1,})"), expression_of("(object (init \"a\" (number 1)))"));
    CHECK_EQ(parse("({a: 1, a: 2})"), expression_of("(object (init \"a\" (number 1)) (init \"a\" (number 2)))"));
    CHECK_EQ(parse("({...a})"), program_of("(expr (object (spread (id a))))"));
    CHECK_EQ(parse("({...a, b, ...c()})"), program_of("(expr (object (spread (id a)) (init \"b\" (id b)) (spread (call (id c)))))"));
    CHECK_EQ(parse("({*g() {}})"), "generators are not supported yet");
    CHECK_EQ(parse("({async m() {}})"), "async functions are not supported yet");
    CHECK_EQ(parse("({m(a, a) {}})"), "Duplicate parameter name not allowed in this context");
    Parsed parsed = parse_source("x = {a: function() {}, b: function n() {}, c: () => 1, d: 1, m() {}, get g() {}}");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        auto const* statement = static_cast<js::ExpressionStatement const*>(parsed.program->body[0]);
        auto const* assignment = static_cast<js::AssignmentExpression const*>(statement->expression);
        auto const* object = static_cast<js::ObjectLiteral const*>(assignment->value);
        CHECK_EQ(object->properties.size(), std::size_t(6));
        if (object->properties.size() == 6) {
            CHECK(object->properties[0].is_anonymous_function);
            CHECK(!object->properties[1].is_anonymous_function);
            CHECK(object->properties[2].is_anonymous_function);
            CHECK(!object->properties[3].is_anonymous_function);
            js::FunctionNode const* method = static_cast<js::FunctionExpression const*>(object->properties[4].value)->function;
            CHECK(!method->is_constructable && !method->is_getter && !method->is_setter);
            CHECK_EQ(source_of(*parsed.program, *method), "m() {}");
            js::FunctionNode const* getter = static_cast<js::FunctionExpression const*>(object->properties[5].value)->function;
            CHECK(getter->is_getter && !getter->is_constructable);
            CHECK_EQ(source_of(*parsed.program, *getter), "get g() {}");
        }
    }
}

void test_array_literals()
{
    CHECK_EQ(parse("[]"), expression_of("(array)"));
    CHECK_EQ(parse("[1, 2]"), expression_of("(array (number 1) (number 2))"));
    CHECK_EQ(parse("[1,]"), expression_of("(array (number 1))"));
    CHECK_EQ(parse("[1,,]"), expression_of("(array (number 1) hole)"));
    CHECK_EQ(parse("[,]"), expression_of("(array hole)"));
    CHECK_EQ(parse("[,,a]"), expression_of("(array hole hole (id a))"));
    CHECK_EQ(parse("[[a], {b: [c]}]"), expression_of("(array (array (id a)) (object (init \"b\" (array (id c)))))"));
    CHECK_EQ(parse("[...a]"), program_of("(expr (array (spread (id a))))"));
    CHECK_EQ(parse("[1, ...a, , ...b]"), program_of("(expr (array (number 1) (spread (id a)) hole (spread (id b))))"));
    CHECK_EQ(parse("new F(...a, 2)"), program_of("(expr (new (id F) (spread (id a)) (number 2)))"));
    CHECK_EQ(parse("...a"), "Unexpected token '...'");
    CHECK_EQ(parse("[...a, ]"), program_of("(expr (array (spread (id a))))"));
    CHECK_EQ(parse("[... a]"), program_of("(expr (array (spread (id a))))"));
    CHECK_EQ(parse("[a b]"), "Unexpected identifier 'b'");
}

void test_statements()
{
    CHECK_EQ(parse("var a, b = 1;"), program_of("(var (a) (b (number 1)))"));
    CHECK_EQ(parse("let a = 1, b;"), program_of("(let (a (number 1)) (b))"));
    CHECK_EQ(parse("const a = 1;"), program_of("(const (a (number 1)))"));
    CHECK_EQ(parse("const a;"), "Missing initializer in const declaration");
    CHECK_EQ(parse("function f(a, b) { return a }"), program_of("(function f (a b) (return (id a)))"));
    CHECK_EQ(parse("function () {}"), "Function statements require a function name");
    CHECK_EQ(parse("{ a; { b } }"), program_of("(block (expr (id a)) (block (expr (id b))))"));
    CHECK_EQ(parse("if (a) b; else c"), program_of("(if (id a) (expr (id b)) (expr (id c)))"));
    CHECK_EQ(parse("if (a) if (b) c; else d"), program_of("(if (id a) (if (id b) (expr (id c)) (expr (id d))))"));
    CHECK_EQ(parse("if (a) {} else if (b) {} else {}"), program_of("(if (id a) (block) (if (id b) (block) (block)))"));
    CHECK_EQ(parse("for (var i = 0; i < 3; i++) x"), program_of("(for (var (i (number 0))) (binary < (id i) (number 3)) (update ++ postfix (id i)) (expr (id x)))"));
    CHECK_EQ(parse("for (i = 0, j = 1; ; ) {}"), program_of("(for (expr (seq (assign = (id i) (number 0)) (assign = (id j) (number 1)))) - - (block))"));
    CHECK_EQ(parse("for (;;) {}"), program_of("(for - - - (block))"));
    CHECK_EQ(parse("for (let i = 0; i < 1; i++) {}"), program_of("(for (let (i (number 0))) (binary < (id i) (number 1)) (update ++ postfix (id i)) (block))"));
    CHECK_EQ(parse("for (const i = 0;;) {}"), program_of("(for (const (i (number 0))) - - (block))"));
    CHECK_EQ(parse("for (const i;;) {}"), "Missing initializer in const declaration");
    CHECK_EQ(parse("for (var k in o) x"), program_of("(for-in (var (k)) (id o) (expr (id x)))"));
    CHECK_EQ(parse("for (k in o) x"), program_of("(for-in (id k) (id o) (expr (id x)))"));
    CHECK_EQ(parse("for (a.b in o) x"), program_of("(for-in (member (id a) b) (id o) (expr (id x)))"));
    CHECK_EQ(parse("for (let k in o) x"), program_of("(for-in (let (k)) (id o) (expr (id x)))"));
    CHECK_EQ(parse("for (const k in o) x"), program_of("(for-in (const (k)) (id o) (expr (id x)))"));
    CHECK_EQ(parse("for (var k = 1 in o) x"), program_of("(for-in (var (k (number 1))) (id o) (expr (id x)))"));
    CHECK_EQ(parse_strict("for (var k = 1 in o) x"), "for-in loop variable declaration may not have an initializer");
    CHECK_EQ(parse("for (let k = 1 in o) x"), "for-in loop variable declaration may not have an initializer");
    CHECK_EQ(parse("for (var a, b in o) x"), "Invalid left-hand side in for-in loop: Must have a single binding");
    CHECK_EQ(parse("for (a = 1 in o) x"), "Invalid left-hand side in for-in loop");
    CHECK_EQ(parse("for (x of y) z"), program_of("(for-of (id x) (id y) (expr (id z)))"));
    CHECK_EQ(parse("for (const x of y) z"), program_of("(for-of (const (x)) (id y) (expr (id z)))"));
    CHECK_EQ(parse("for (var x = 1 of y) z"), "for-of loop variable declaration may not have an initializer");
    CHECK_EQ(parse("for (let a, b of o) x"), "Invalid left-hand side in for-of loop: Must have a single binding");
    CHECK_EQ(parse("for (a = 1 of o) x"), "Invalid left-hand side in for-of loop");
    // On one line `async of` reads as an async arrow's head, which the
    // parser declines by name; split by a line terminator it is a name
    // again, and the for-of rule is what refuses it.
    CHECK_EQ(parse("for (async of o) x"), "async functions are not supported yet");
    CHECK_EQ(parse("for (async\nof o) x"), "The left-hand side of a for-of loop may not be 'async'");
    CHECK_EQ(parse("for ((async) of o) x"), program_of("(for-of (id async) (id o) (expr (id x)))"));
    CHECK_EQ(parse("for (var x = (a in b);;) {}"), program_of("(for (var (x (binary in (id a) (id b)))) - - (block))"));
    CHECK_EQ(parse("for (let in o) x"), program_of("(for-in (id let) (id o) (expr (id x)))"));
    CHECK_EQ(parse("for (let;;) x"), program_of("(for (expr (id let)) - - (expr (id x)))"));
    CHECK_EQ(parse("while (a) b"), program_of("(while (id a) (expr (id b)))"));
    CHECK_EQ(parse("do b; while (a);"), program_of("(do (expr (id b)) (id a))"));
    CHECK_EQ(parse("do { } while (a)"), program_of("(do (block) (id a))"));
    CHECK_EQ(parse("function f() { return }"), program_of("(function f () (return))"));
    CHECK_EQ(parse("function f() { return 1; }"), program_of("(function f () (return (number 1)))"));
    CHECK_EQ(parse("while (1) { break; continue; }"), program_of("(while (number 1) (block (break) (continue)))"));
    CHECK_EQ(parse("throw new Error('x')"), program_of("(throw (new (id Error) (string \"x\")))"));
    CHECK_EQ(parse("try { a } catch (e) { b } finally { c }"), program_of("(try (block (expr (id a))) (catch e (block (expr (id b)))) (finally (block (expr (id c)))))"));
    CHECK_EQ(parse("try { a } catch { b }"), program_of("(try (block (expr (id a))) (catch (block (expr (id b)))))"));
    CHECK_EQ(parse("try { a } finally { c }"), program_of("(try (block (expr (id a))) (finally (block (expr (id c)))))"));
    CHECK_EQ(parse("try { a }"), "Missing catch or finally after try");
    CHECK_EQ(parse("try { } catch ([e]) { }"), program_of("(try (block) (catch (array-pattern (id e)) (block)))"));
    CHECK_EQ(parse("try { } catch ({message: m, code = 0}) { }"), program_of("(try (block) (catch (object-pattern (\"message\" (id m)) (\"code\" (= (id code) (number 0)))) (block)))"));
    CHECK_EQ(parse("try { } catch ([e, e]) { }"), "Identifier 'e' has already been declared");
    CHECK_EQ(parse("try { } catch ([e]) { var e; }"), "Identifier 'e' has already been declared");
    CHECK_EQ(parse("try { } catch ([e]) { let e; }"), "Identifier 'e' has already been declared");
    CHECK_EQ(parse("try { } catch (e) { var e; }"), program_of("(try (block) (catch e (block (var (e)))))"));
    CHECK_EQ(parse("switch (x) { case 1: a; b; case 2: default: c }"),
        program_of("(switch (id x) (case (number 1) (expr (id a)) (expr (id b))) (case (number 2)) (default (expr (id c))))"));
    CHECK_EQ(parse("switch (x) { default: a; default: b }"), "More than one default clause in switch statement");
    CHECK_EQ(parse("switch (x) { }"), program_of("(switch (id x))"));
    CHECK_EQ(parse("a: b: x"), program_of("(label a (label b (expr (id x))))"));
    CHECK_EQ(parse("with (o) x"), program_of("(with (id o) (expr (id x)))"));
    CHECK_EQ(parse(";"), program_of("(empty)"));
    CHECK_EQ(parse("debugger;"), program_of("(debugger)"));
    CHECK_EQ(parse("debugger\nx"), program_of("(debugger) (expr (id x))"));
    CHECK_EQ(parse(""), "(program)");
    CHECK_EQ(parse("   \n// only a comment\n"), "(program)");
    CHECK_EQ(parse("}"), "Unexpected token '}'");
    CHECK_EQ(parse("if (a) let x = 1"), "Lexical declaration cannot appear in a single-statement context");
    CHECK_EQ(parse("while (a) const x = 1"), "Lexical declaration cannot appear in a single-statement context");
    CHECK_EQ(parse("if (a) let\n[x] = y"), "Lexical declaration cannot appear in a single-statement context");
    CHECK_EQ(parse("if (a) let {x} = y"), "Lexical declaration cannot appear in a single-statement context");
    // `let` alone on its line in statement position is the identifier, ended by ASI (§14.5).
    CHECK_EQ(parse("if (a) let\nx = 1"), program_of("(if (id a) (expr (id let))) (expr (assign = (id x) (number 1)))"));
    CHECK_EQ(parse("while (a) let\n{}"), program_of("(while (id a) (expr (id let))) (block)"));
    CHECK_EQ(parse("if (a) let"), program_of("(if (id a) (expr (id let)))"));
    CHECK_EQ(parse("if (a) function f() {}"), program_of("(if (id a) (block (function f ())))"));
    CHECK_EQ(parse("if (a) x; else function f() {}"), program_of("(if (id a) (expr (id x)) (block (function f ())))"));
    CHECK_EQ(parse("while (a) function f() {}"), "In non-strict mode code, functions can only be declared at top level, inside a block, or as the body of an if statement");
    CHECK_EQ(parse("a: function f() {}"), program_of("(label a (function f ()))"));
    CHECK_EQ(parse("while (x) a: function f() {}"), "In non-strict mode code, functions can only be declared at top level, inside a block, or as the body of an if statement");
    CHECK_EQ(parse("let = 1"), expression_of("(assign = (id let) (number 1))"));
    CHECK_EQ(parse("let\nx = 1"), program_of("(let (x (number 1)))"));
    CHECK_EQ(parse("let.x"), expression_of("(member (id let) x)"));
    CHECK_EQ(parse("var let = 1"), program_of("(var (let (number 1)))"));
    CHECK_EQ(parse("let [a] = b"), program_of("(let ((array-pattern (id a)) (id b)))"));
    CHECK_EQ(parse("var {a} = b"), program_of("(var ((object-pattern (\"a\" (id a))) (id b)))"));
    CHECK_EQ(parse("const [a, , b = 1, ...c] = d"), program_of("(const ((array-pattern (id a) hole (= (id b) (number 1)) (... (id c))) (id d)))"));
    CHECK_EQ(parse("var {a: [b], 'c': d, 1: e, [f]: g = 2, ...h} = i"), program_of("(var ((object-pattern (\"a\" (array-pattern (id b))) (\"c\" (id d)) (\"1\" (id e)) ((computed (id f)) (= (id g) (number 2))) (... (id h))) (id i)))"));
    CHECK_EQ(parse("let [a];"), "Missing initializer in destructuring declaration");
    CHECK_EQ(parse("var [a];"), "Missing initializer in destructuring declaration");
    CHECK_EQ(parse("let [a, a] = b"), "Identifier 'a' has already been declared");
    CHECK_EQ(parse("let [let] = b"), "let is disallowed as a lexically bound name");
    CHECK_EQ(parse("var [...a, b] = c"), "Rest element must be last element");
    CHECK_EQ(parse("var [...a = 1] = c"), "Rest element may not have a default initializer");
    CHECK_EQ(parse("var {...a, b} = c"), "Rest element must be last element");
    CHECK_EQ(parse("var {...[a]} = c"), "Unexpected token '['");
    CHECK_EQ(parse("var {if} = c"), "Unexpected token 'if'");
    CHECK_EQ(parse("var {if: a} = c"), program_of("(var ((object-pattern (\"if\" (id a))) (id c)))"));
    CHECK_EQ(parse_strict("var [eval] = c"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse("for (let [a, b] of c) ;"), program_of("(for-of (let ((array-pattern (id a) (id b)))) (id c) (empty))"));
    CHECK_EQ(parse("for (var {a} in c) ;"), program_of("(for-in (var ((object-pattern (\"a\" (id a))))) (id c) (empty))"));
    CHECK_EQ(parse("for (var [a] = [] in c) ;"), "for-in loop variable declaration may not have an initializer");
    CHECK_EQ(parse("for (let [a] = [1]; ; ) break"), program_of("(for (let ((array-pattern (id a)) (array (number 1)))) - - (break))"));
    CHECK_EQ(parse("a.if + a.class + a.new"), expression_of("(binary + (binary + (member (id a) if) (member (id a) class)) (member (id a) new))"));
    CHECK_EQ(parse("\\u0069f (x) {}"), "Keyword must not contain escaped characters");
    CHECK_EQ(parse("var \\u0069f"), "Keyword must not contain escaped characters");
    CHECK_EQ(parse("a.\\u0069f"), expression_of("(member (id a) if)"));
    CHECK_EQ(parse("x = 0x10 + 1e3 + .5 + 08 + 017"), expression_of("(assign = (id x) (binary + (binary + (binary + (binary + (number 16) (number 1000)) (number 0.5)) (number 8)) (number 15)))"));
    CHECK_EQ(parse("'a\\\"b' + \"\\x41\" + 'c\\\\d'"), expression_of("(binary + (binary + (string \"a\\\"b\") (string \"A\")) (string \"c\\\\d\"))"));
    CHECK_EQ(parse("x = true, y = false, z = null, w = this"), expression_of("(seq (assign = (id x) true) (assign = (id y) false) (assign = (id z) null) (assign = (id w) this))"));
}

void test_labels()
{
    CHECK_EQ(parse("a: b: for (;;) { continue a; continue b; break a; break b; }"),
        program_of("(label a (label b (for - - - (block (continue a) (continue b) (break a) (break b)))))"));
    CHECK_EQ(parse("a: { break a; }"), program_of("(label a (block (break a)))"));
    CHECK_EQ(parse("a: { continue a; }"), "Illegal continue statement: 'a' does not denote an iteration statement");
    CHECK_EQ(parse("break x"), "Undefined label 'x'");
    CHECK_EQ(parse("while (1) { continue x }"), "Undefined label 'x'");
    CHECK_EQ(parse("a: a: x"), "Label 'a' has already been declared");
    CHECK_EQ(parse("a: { a: x }"), "Label 'a' has already been declared");
    CHECK_EQ(parse("a: x; a: y"), program_of("(label a (expr (id x))) (label a (expr (id y)))"));
    CHECK_EQ(parse("break"), "Illegal break statement");
    CHECK_EQ(parse("continue"), "Illegal continue statement: no surrounding iteration statement");
    CHECK_EQ(parse("switch (x) { case 1: continue }"), "Illegal continue statement: no surrounding iteration statement");
    CHECK_EQ(parse("switch (x) { case 1: break }"), program_of("(switch (id x) (case (number 1) (break)))"));
    CHECK_EQ(parse("while (1) switch (x) { case 1: continue }"), program_of("(while (number 1) (switch (id x) (case (number 1) (continue))))"));
    CHECK_EQ(parse("a: while (1) { function f() { break a } }"), "Undefined label 'a'");
    CHECK_EQ(parse("while (1) { function f() { break } }"), "Illegal break statement");
    CHECK_EQ(parse("a: while (1) { b: while (1) { continue a } }"), program_of("(label a (while (number 1) (block (label b (while (number 1) (block (continue a)))))))"));
    CHECK_EQ(parse("return"), "Illegal return statement");
    CHECK_EQ(parse("x => { return }"), expression_of("(arrow (x) (return))"));
    js::ParseOptions options;
    options.allow_return = true;
    CHECK_EQ(parse("return 1", options), program_of("(return (number 1))"));
    CHECK_EQ(parse("yield: 1"), program_of("(label yield (expr (number 1)))"));
}

void test_strict_mode()
{
    CHECK_EQ(parse("\"use strict\""), "(program strict (expr (string \"use strict\")))");
    CHECK_EQ(parse("'use strict';"), "(program strict (expr (string \"use strict\")))");
    CHECK_EQ(parse("\"use\\u0020strict\"; with (a) {}"), program_of("(expr (string \"use strict\")) (with (id a) (block))"));
    CHECK_EQ(parse("\"use strict\".length; with (a) {}"), program_of("(expr (member (string \"use strict\") length)) (with (id a) (block))"));
    CHECK_EQ(parse("(\"use strict\"); with (a) {}"), program_of("(expr (string \"use strict\")) (with (id a) (block))"));
    CHECK_EQ(parse("\"use strict\" + 1; with (a) {}"), program_of("(expr (binary + (string \"use strict\") (number 1))) (with (id a) (block))"));
    CHECK_EQ(parse("\"a\"; \"use strict\"; with (a) {}"), "Strict mode code may not include a with statement");
    CHECK_EQ(parse("x; \"use strict\"; with (a) {}"), program_of("(expr (id x)) (expr (string \"use strict\")) (with (id a) (block))"));
    CHECK_EQ(parse("\"use strict\"\nwith (a) {}"), "Strict mode code may not include a with statement");
    CHECK_EQ(parse("function f() { \"use strict\"; with (a) {} }"), "Strict mode code may not include a with statement");
    CHECK_EQ(parse("function f() { \"use strict\" } with (a) {}"), program_of("(function f () (expr (string \"use strict\"))) (with (id a) (block))"));
    CHECK_EQ(parse_strict("with (a) {}"), "Strict mode code may not include a with statement");
    CHECK_EQ(parse_strict("x = 010"), "Octal literals are not allowed in strict mode");
    CHECK_EQ(parse_strict("x = 08"), "Octal literals are not allowed in strict mode");
    CHECK_EQ(parse_strict("x = '\\01'"), "Octal escape sequences are not allowed in strict mode");
    CHECK_EQ(parse_strict("x = '\\8'"), "Octal escape sequences are not allowed in strict mode");
    CHECK_EQ(parse("'\\01'; 'use strict'"), "Octal escape sequences are not allowed in strict mode");
    // Not a prologue once a non-string statement precedes: sloppy, so the octal escape stands.
    CHECK_EQ(parse("x = '\\01'; 'use strict'"), program_of("(expr (assign = (id x) (string \"\x01\"))) (expr (string \"use strict\"))"));
    // \0 not followed by a digit is the NUL escape, fine in strict code.
    std::string nul_expected = "(program strict (expr (assign = (id x) (string \"";
    nul_expected.push_back('\0');
    nul_expected += "\"))))";
    CHECK_EQ(parse_strict("x = '\\0'"), nul_expected);
    CHECK_EQ(parse_strict("eval = 1"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse_strict("arguments++"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse_strict("--eval"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse_strict("var eval"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse_strict("let arguments"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse_strict("function eval() {}"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse_strict("function f(eval) {}"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse_strict("(function arguments() {})"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse_strict("try {} catch (eval) {}"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse_strict("x => { eval = 1 }"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse_strict("eval(x); arguments[0]; f(eval, arguments)"),
        "(program strict (expr (call (id eval) (id x))) (expr (index (id arguments) (number 0))) (expr (call (id f) (id eval) (id arguments))))");
    CHECK_EQ(parse("function eval() { 'use strict' }"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse("function f(arguments) { 'use strict' }"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse_strict("delete x"), "Delete of an unqualified identifier in strict mode");
    CHECK_EQ(parse_strict("delete (x)"), "Delete of an unqualified identifier in strict mode");
    CHECK_EQ(parse_strict("delete x.y; delete x[0]; delete (x, y)"), "(program strict (expr (unary delete (member (id x) y))) (expr (unary delete (index (id x) (number 0)))) (expr (unary delete (seq (id x) (id y)))))");
    CHECK_EQ(parse("delete x"), expression_of("(unary delete (id x))"));
    CHECK_EQ(parse_strict("function f(a, a) {}"), "Duplicate parameter name not allowed in this context");
    CHECK_EQ(parse("function f(a, a) { 'use strict' }"), "Duplicate parameter name not allowed in this context");
    CHECK_EQ(parse("function f(a, b, a) {}"), program_of("(function f (a b a))"));
    CHECK_EQ(parse_strict("var let"), "Unexpected strict mode reserved word");
    CHECK_EQ(parse_strict("let = 1"), "Unexpected strict mode reserved word");
    CHECK_EQ(parse_strict("var static"), "Unexpected strict mode reserved word");
    CHECK_EQ(parse_strict("yield"), "Unexpected strict mode reserved word");
    CHECK_EQ(parse_strict("implements + interface"), "Unexpected strict mode reserved word");
    CHECK_EQ(parse_strict("function package() {}"), "Unexpected strict mode reserved word");
    CHECK_EQ(parse_strict("function f(private) {}"), "Unexpected strict mode reserved word");
    CHECK_EQ(parse_strict("({protected})"), "Unexpected strict mode reserved word");
    CHECK_EQ(parse_strict("public: 1"), "Unexpected strict mode reserved word");
    CHECK_EQ(parse("var implements, interface, package, private, protected, public, static, yield"),
        program_of("(var (implements) (interface) (package) (private) (protected) (public) (static) (yield))"));
    CHECK_EQ(parse_strict("({static: 1, yield: 2}).let"), "(program strict (expr (member (object (init \"static\" (number 1)) (init \"yield\" (number 2))) let)))");
    CHECK_EQ(parse_strict("if (x) function f() {}"), "In strict mode code, functions can only be declared at top level or inside a block");
    CHECK_EQ(parse_strict("a: function f() {}"), "In strict mode code, functions can only be declared at top level or inside a block");
    CHECK_EQ(parse_strict("while (x) function f() {}"), "In strict mode code, functions can only be declared at top level or inside a block");
    CHECK_EQ(parse_strict("{ function f() {} }"), "(program strict (block (function f ())))");
    Parsed parsed = parse_source("'use strict'; function f() { function g() {} } var h = function() {}");
    CHECK(parsed.program && parsed.program->is_strict);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 1);
        js::FunctionNode const* g = f ? function_in(f->body, 0) : nullptr;
        js::FunctionNode const* h = function_in(parsed.program->body, 2);
        CHECK(f && f->is_strict);
        CHECK(g && g->is_strict);
        CHECK(h && h->is_strict);
    }
    parsed = parse_source("function f() { 'use strict'; function g() {} } function h() {}");
    CHECK(parsed.program && !parsed.program->is_strict);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 0);
        js::FunctionNode const* g = f ? function_in(f->body, 1) : nullptr;
        js::FunctionNode const* h = function_in(parsed.program->body, 1);
        CHECK(f && f->is_strict);
        CHECK(g && g->is_strict);
        CHECK(h && !h->is_strict);
    }
    js::ParseOptions options;
    options.strict = true;
    parsed = parse_source("function f() {}", options);
    CHECK(parsed.program && parsed.program->is_strict);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 0);
        CHECK(f && f->is_strict);
    }
}

void test_declarations()
{
    Parsed parsed = parse_source("var a; var b = 1, a; { var c; } for (var d;;); for (var e in o); function f() { var g; } function h() {} let i; const j = 1; var k = function m() {};");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        CHECK_EQ(names(parsed.program->declarations.vars), "a b c d e k");
        CHECK_EQ(function_names(parsed.program->declarations.functions), "f h");
        CHECK_EQ(lexical_names(parsed.program->declarations.lexicals), "i const:j");
        js::FunctionNode const* f = function_in(parsed.program->body, 5);
        CHECK(f && names(f->declarations.vars) == "g");
    }
    parsed = parse_source("{ let x; const y = 1; function z() {} var w; }");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        auto const* block = static_cast<js::BlockStatement const*>(parsed.program->body[0]);
        CHECK_EQ(lexical_names(block->declarations.lexicals), "x const:y");
        CHECK_EQ(function_names(block->declarations.functions), "z");
        CHECK_EQ(names(block->declarations.vars), "");
        CHECK_EQ(names(parsed.program->declarations.vars), "w z"); // z: Annex B.3.2 var-hoisting in sloppy code
        CHECK_EQ(lexical_names(parsed.program->declarations.lexicals), "");
        CHECK_EQ(function_names(parsed.program->declarations.functions), "");
    }
    parsed = parse_source("'use strict'; { function z() {} }");
    CHECK(parsed.program && names(parsed.program->declarations.vars) == "");
    parsed = parse_source("let z; { function z() {} }");
    CHECK(parsed.program && names(parsed.program->declarations.vars) == "");
    parsed = parse_source("{ function z() {} } let z;");
    CHECK(parsed.program && names(parsed.program->declarations.vars) == "");
    parsed = parse_source("{ let q; { function z() {} } } { function z() {} }");
    CHECK(parsed.program && names(parsed.program->declarations.vars) == "z");
    parsed = parse_source("function f(z) { { function z() {} } } function g() { { function z() {} } }");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 0);
        js::FunctionNode const* g = function_in(parsed.program->body, 1);
        CHECK(f && names(f->declarations.vars) == "");
        CHECK(g && names(g->declarations.vars) == "z");
    }
    parsed = parse_source("if (x) function z() {}");
    CHECK(parsed.program && names(parsed.program->declarations.vars) == "z");
    parsed = parse_source("switch (x) { case 1: let y; function z() {} var v; }");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        auto const* dispatch = static_cast<js::SwitchStatement const*>(parsed.program->body[0]);
        CHECK_EQ(lexical_names(dispatch->declarations.lexicals), "y");
        CHECK_EQ(function_names(dispatch->declarations.functions), "z");
        CHECK_EQ(names(parsed.program->declarations.vars), "v z");
    }
    parsed = parse_source("for (let i = 0, j = 1; i < 1; i++) { var k; }");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        auto const* loop = static_cast<js::ForStatement const*>(parsed.program->body[0]);
        CHECK_EQ(lexical_names(loop->declarations.lexicals), "i j");
        CHECK_EQ(names(parsed.program->declarations.vars), "k");
    }
    parsed = parse_source("function f(a, b) { var a; }");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 0);
        CHECK(f && names(f->declarations.vars) == "a");
        CHECK(f && parameter_names(*f) == "a b");
    }
    CHECK_EQ(parse("{ let a; var a; }"), "Identifier 'a' has already been declared");
    CHECK_EQ(parse("{ var a; } let a;"), "Identifier 'a' has already been declared");
    CHECK_EQ(parse("{ let a; { var a; } }"), "Identifier 'a' has already been declared");
    CHECK_EQ(parse("let a; var a;"), "Identifier 'a' has already been declared");
    CHECK_EQ(parse("var a; let a;"), "Identifier 'a' has already been declared");
    CHECK_EQ(parse("var a; { let a; }"), program_of("(var (a)) (block (let (a)))"));
    CHECK_EQ(parse("{ let a; } var a;"), program_of("(block (let (a))) (var (a))"));
    CHECK_EQ(parse("let a; { let a; }"), program_of("(let (a)) (block (let (a)))"));
    CHECK_EQ(parse("var a; var a;"), program_of("(var (a)) (var (a))"));
    CHECK_EQ(parse("let a; let a;"), "Identifier 'a' has already been declared");
    CHECK_EQ(parse("let a, a;"), "Identifier 'a' has already been declared");
    CHECK_EQ(parse("const a = 1; var a;"), "Identifier 'a' has already been declared");
    CHECK_EQ(parse("let f; function f() {}"), "Identifier 'f' has already been declared");
    CHECK_EQ(parse("function f() {} let f;"), "Identifier 'f' has already been declared");
    CHECK_EQ(parse("function f() {} var f;"), program_of("(function f ()) (var (f))"));
    CHECK_EQ(parse("function f() {} function f() {}"), program_of("(function f ()) (function f ())"));
    CHECK_EQ(parse("{ function f() {} function f() {} }"), program_of("(block (function f ()) (function f ()))"));
    CHECK_EQ(parse_strict("{ function f() {} function f() {} }"), "Identifier 'f' has already been declared");
    CHECK_EQ(parse("{ function f() {} let f; }"), "Identifier 'f' has already been declared");
    CHECK_EQ(parse("{ function f() {} var f; }"), "Identifier 'f' has already been declared");
    CHECK_EQ(parse("function f(a) { let a; }"), "Identifier 'a' has already been declared");
    CHECK_EQ(parse("function f(a) { { let a; } }"), program_of("(function f (a) (block (let (a))))"));
    CHECK_EQ(parse("x => { let x; }"), "Identifier 'x' has already been declared");
    CHECK_EQ(parse("let let = 1"), "let is disallowed as a lexically bound name");
    CHECK_EQ(parse("const let = 1"), "let is disallowed as a lexically bound name");
    CHECK_EQ(parse("for (let let;;);"), "let is disallowed as a lexically bound name");
    CHECK_EQ(parse("try {} catch (e) { var e; }"), program_of("(try (block) (catch e (block (var (e)))))"));
    CHECK_EQ(parse("try {} catch (e) { { var e; } }"), program_of("(try (block) (catch e (block (block (var (e))))))"));
    CHECK_EQ(parse("try {} catch (e) { let e; }"), "Identifier 'e' has already been declared");
    CHECK_EQ(parse("try {} catch (e) { function e() {} }"), "Identifier 'e' has already been declared");
    CHECK_EQ(parse("try {} catch (e) { { let e; } }"), program_of("(try (block) (catch e (block (block (let (e))))))"));
    CHECK_EQ(parse("for (let i;;) { var i; }"), "Identifier 'i' has already been declared");
    CHECK_EQ(parse("for (let i in o) { var i; }"), "Identifier 'i' has already been declared");
    CHECK_EQ(parse("for (var i;;) { let i; }"), program_of("(for (var (i)) - - (block (let (i))))"));
    CHECK_EQ(parse("for (let i;;); for (let i;;);"), program_of("(for (let (i)) - - (empty)) (for (let (i)) - - (empty))"));
    CHECK_EQ(parse("switch (x) { case 1: let a; case 2: let a; }"), "Identifier 'a' has already been declared");
    CHECK_EQ(parse("(function f() { let f; })"), expression_of("(function f () (let (f)))"));
    CHECK_EQ(parse("(function f(f) {})"), expression_of("(function f (f))"));
}

void test_function_nodes()
{
    Parsed parsed = parse_source("function f(a, b) { arguments; this; eval(\"x\"); }");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 0);
        CHECK(f != nullptr);
        if (f) {
            CHECK_EQ(f->name->to_utf8(), "f");
            CHECK_EQ(parameter_names(*f), "a b");
            CHECK(f->uses_arguments && f->uses_this && f->has_direct_eval);
            CHECK(!f->is_arrow && f->is_constructable && !f->is_getter && !f->is_setter && !f->is_strict && !f->has_duplicate_parameters);
            CHECK_EQ(source_of(*parsed.program, *f), "function f(a, b) { arguments; this; eval(\"x\"); }");
            CHECK_EQ(f->position.column, std::uint32_t(1));
            auto const* call_statement = static_cast<js::ExpressionStatement const*>(f->body[2]);
            auto const* call = static_cast<js::CallExpression const*>(call_statement->expression);
            CHECK(call->is_direct_eval);
        }
    }
    parsed = parse_source("function f() { function g() { this; arguments; } }");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 0);
        js::FunctionNode const* g = f ? function_in(f->body, 0) : nullptr;
        CHECK(f && !f->uses_this && !f->uses_arguments);
        CHECK(g && g->uses_this && g->uses_arguments);
    }
    parsed = parse_source("function f() { return () => () => this }");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 0);
        js::FunctionNode const* outer = f ? function_in(f->body, 0) : nullptr;
        js::FunctionNode const* inner = outer && outer->expression_body ? static_cast<js::ArrowFunction const*>(outer->expression_body)->function : nullptr;
        CHECK(f && f->uses_this);
        CHECK(outer && outer->uses_this);
        CHECK(inner && inner->uses_this);
    }
    parsed = parse_source("function f() { function g() { eval(1) } } function h() { eval?.(1); (0, eval)(2); x.eval(3) } function k() { (eval)(4) }");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 0);
        js::FunctionNode const* g = f ? function_in(f->body, 0) : nullptr;
        js::FunctionNode const* h = function_in(parsed.program->body, 1);
        js::FunctionNode const* k = function_in(parsed.program->body, 2);
        CHECK(f && f->has_direct_eval);
        CHECK(g && g->has_direct_eval);
        CHECK(h && !h->has_direct_eval);
        CHECK(k && k->has_direct_eval);
    }
    parsed = parse_source("var f = function foo(a) { return a };\nvar g = x => x + 1;\nvar h = function() {};");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 0);
        js::FunctionNode const* g = function_in(parsed.program->body, 1);
        js::FunctionNode const* h = function_in(parsed.program->body, 2);
        CHECK(f && source_of(*parsed.program, *f) == "function foo(a) { return a }");
        CHECK(f && f->name && f->name->to_utf8() == "foo");
        CHECK(g && source_of(*parsed.program, *g) == "x => x + 1");
        CHECK(g && g->position.line == 2 && g->position.column == 9);
        CHECK(h && h->name == nullptr && h->is_constructable);
    }
    parsed = parse_source("function f(a, b, a) {}");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 0);
        CHECK(f && f->has_duplicate_parameters);
    }
    // Positions and end offsets of nested nodes.
    parsed = parse_source("  a + b;\nif (x) { y }");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        auto const* statement = static_cast<js::ExpressionStatement const*>(parsed.program->body[0]);
        CHECK_EQ(statement->position.offset, std::uint32_t(2));
        CHECK_EQ(statement->end_offset, std::uint32_t(8));
        auto const* binary = static_cast<js::BinaryExpression const*>(statement->expression);
        CHECK_EQ(binary->position.column, std::uint32_t(3));
        CHECK_EQ(binary->end_offset, std::uint32_t(7));
        CHECK_EQ(binary->right->position.offset, std::uint32_t(6));
        CHECK_EQ(binary->right->end_offset, std::uint32_t(7));
        js::Statement const* branch = parsed.program->body[1];
        CHECK_EQ(branch->position.line, std::uint32_t(2));
        CHECK_EQ(branch->position.column, std::uint32_t(1));
        CHECK_EQ(branch->end_offset, std::uint32_t(parsed.program->source.size()));
    }
}

void test_new_and_calls()
{
    CHECK_EQ(parse("new X"), expression_of("(new (id X))"));
    CHECK_EQ(parse("new X()"), expression_of("(new (id X))"));
    CHECK_EQ(parse("new X(1, 2)"), expression_of("(new (id X) (number 1) (number 2))"));
    CHECK_EQ(parse("new a.b.c()"), expression_of("(new (member (member (id a) b) c))"));
    CHECK_EQ(parse("new a.b.c"), expression_of("(new (member (member (id a) b) c))"));
    CHECK_EQ(parse("new a[b]()"), expression_of("(new (index (id a) (id b)))"));
    CHECK_EQ(parse("new new X()()"), expression_of("(new (new (id X)))"));
    CHECK_EQ(parse("new new X"), expression_of("(new (new (id X)))"));
    CHECK_EQ(parse("new X().y"), expression_of("(member (new (id X)) y)"));
    CHECK_EQ(parse("new X()()"), expression_of("(call (new (id X)))"));
    CHECK_EQ(parse("new X.y()"), expression_of("(new (member (id X) y))"));
    CHECK_EQ(parse("new (f())"), expression_of("(new (call (id f)))"));
    CHECK_EQ(parse("new f()(1).g"), expression_of("(member (call (new (id f)) (number 1)) g)"));
    CHECK_EQ(parse("new a?.b()"), "Invalid optional chain from new expression");
    CHECK_EQ(parse("new.target"), "new.target expression is not allowed here");
    CHECK_EQ(parse("f()()"), expression_of("(call (call (id f)))"));
    CHECK_EQ(parse("f(a)(b, c)"), expression_of("(call (call (id f) (id a)) (id b) (id c))"));
    CHECK_EQ(parse("f(a,)"), expression_of("(call (id f) (id a))"));
    CHECK_EQ(parse("a.b(c)[d].e"), expression_of("(member (index (call (member (id a) b) (id c)) (id d)) e)"));
    CHECK_EQ(parse("f(...a)"), program_of("(expr (call (id f) (spread (id a))))"));
    CHECK_EQ(parse("f(a b)"), "Unexpected identifier 'b'");
}

void test_optional_chaining()
{
    CHECK_EQ(parse("a?.b"), expression_of("(member? (id a) b)"));
    CHECK_EQ(parse("a?.[b]"), expression_of("(index? (id a) (id b))"));
    CHECK_EQ(parse("a?.(b)"), expression_of("(call? (id a) (id b))"));
    CHECK_EQ(parse("a?.b.c"), expression_of("(member (member? (id a) b) c)"));
    CHECK_EQ(parse("a.b?.c()"), expression_of("(call (member? (member (id a) b) c))"));
    CHECK_EQ(parse("a?.b?.()"), expression_of("(call? (member? (id a) b))"));
    CHECK_EQ(parse("a?.new"), expression_of("(member? (id a) new)"));
    CHECK_EQ(parse("a?.5:b"), expression_of("(cond (id a) (number 0.5) (id b))"));
    CHECK_EQ(parse("a?.b = 1"), "Invalid left-hand side in assignment");
    CHECK_EQ(parse("a?.b.c = 1"), "Invalid left-hand side in assignment");
    CHECK_EQ(parse("a?.b++"), "Invalid left-hand side expression in postfix operation");
    CHECK_EQ(parse("for (a?.b in o);"), "Invalid left-hand side in for-in loop");
    CHECK_EQ(parse("a?.`x`"), "tagged templates are not supported yet");
    Parsed parsed = parse_source("eval?.(x)");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        auto const* statement = static_cast<js::ExpressionStatement const*>(parsed.program->body[0]);
        auto const* call = static_cast<js::CallExpression const*>(statement->expression);
        CHECK(call->optional && !call->is_direct_eval);
    }
}

void test_error_positions()
{
    CHECK_EQ(error_at("var x = ;"), "1:9 Unexpected token ';'");
    CHECK_EQ(error_at("\n\nfoo("), "3:5 Unexpected end of input");
    CHECK_EQ(error_at("a = 1;\n  b = )"), "2:7 Unexpected token ')'");
    CHECK_EQ(error_at("var x = 1 @ 2"), "1:11 Unexpected character '@'");
    CHECK_EQ(error_at("x = 'abc"), "1:5 Unterminated string literal");
    CHECK_EQ(error_at("/* open"), "1:1 Unterminated comment");
    CHECK_EQ(error_at("let a;\nlet a;"), "2:5 Identifier 'a' has already been declared");
    CHECK_EQ(error_at("x = 1\r\ny = {"), "2:6 Unexpected end of input");
    CHECK_EQ(error_at("x = 2 +\n  * 3"), "2:3 Unexpected token '*'");
}

void test_nesting_cap()
{
    std::string deep = std::string(2000, '(') + "x" + std::string(2000, ')');
    CHECK_EQ(parse(deep), "too much nesting");
    std::string fine = std::string(400, '(') + "x" + std::string(400, ')');
    CHECK_EQ(parse(fine), expression_of("(id x)"));
    std::string blocks = std::string(2000, '{') + std::string(2000, '}');
    CHECK_EQ(parse(blocks), "too much nesting");
    std::string arrays = std::string(2000, '[') + std::string(2000, ']');
    CHECK_EQ(parse(arrays), "too much nesting");
    std::string calls;
    for (int i = 0; i < 2000; ++i)
        calls += "f(";
    calls += "x";
    calls += std::string(2000, ')');
    CHECK_EQ(parse(calls), "too much nesting");
    std::string powers;
    for (int i = 0; i < 3000; ++i)
        powers += "2 ** ";
    powers += "2";
    CHECK_EQ(parse(powers), "too much nesting");
    std::string unary = std::string(3000, '!') + "x";
    CHECK_EQ(parse(unary), "too much nesting");
    std::string functions;
    for (int i = 0; i < 2000; ++i)
        functions += "function f() {";
    functions += std::string(2000, '}');
    CHECK_EQ(parse(functions), "too much nesting");
    std::string ifs;
    for (int i = 0; i < 2000; ++i)
        ifs += "if (a) ";
    ifs += "x";
    CHECK_EQ(parse(ifs), "too much nesting");
    // Just under the cap on every recursion path: the cap must fit the
    // stack, so these parse rather than crash. Each level of these costs
    // one or two counts (see enter() in the parser).
    auto repeated = [](std::string const& open, int count, std::string const& middle, std::string const& close) {
        std::string source;
        for (int i = 0; i < count; ++i)
            source += open;
        source += middle;
        for (int i = 0; i < count; ++i)
            source += close;
        return source;
    };
    CHECK(parse_source(repeated("(", 499, "x", ")")).program != nullptr);
    CHECK(parse_source(repeated("f(", 499, "x", ")")).program != nullptr);
    CHECK(parse_source(repeated("a[", 499, "x", "]")).program != nullptr);
    CHECK(parse_source(repeated("[", 499, "", "]")).program != nullptr);
    CHECK(parse_source("x = " + repeated("{a:", 498, "1", "}")).program != nullptr);
    CHECK(parse_source(repeated("{", 499, "", "}")).program != nullptr);
    CHECK(parse_source(repeated("if (a) ", 998, "x", "")).program != nullptr);
    CHECK(parse_source(repeated("!", 998, "x", "")).program != nullptr);
    CHECK(parse_source(repeated("2 ** ", 998, "2", "")).program != nullptr);
    CHECK(parse_source(repeated("a => ", 498, "x", "")).program != nullptr);
    CHECK(parse_source(repeated("function f() {", 998, "", "}")).program != nullptr);
    CHECK(parse_source(repeated("`${", 498, "x", "}`")).program != nullptr);
    CHECK(parse_source(repeated("new ", 998, "X", "")).program != nullptr);
    CHECK(parse_source(repeated("a ? b : ", 498, "c", "")).program != nullptr);
    CHECK(parse_source(repeated("x = ", 998, "1", "")).program != nullptr);
    CHECK(parse_source(repeated("while (a) ", 998, "x", "")).program != nullptr);
    CHECK(parse_source(repeated("try {", 499, "", "} finally {}")).program != nullptr);
    // A long flat chain is not nesting: it parses, and the dump survives it.
    std::string chain = "x";
    for (int i = 0; i < 3000; ++i)
        chain += " + x";
    Parsed parsed = parse_source(chain);
    CHECK(parsed.program != nullptr);
    if (parsed.program)
        CHECK(!js::dump_ast(*parsed.program).empty());
    std::string members = "a";
    for (int i = 0; i < 3000; ++i)
        members += ".b";
    CHECK(parse_source(members).program != nullptr);
}

// Destructuring: parameters, and the cover grammar's assignment patterns.
void test_patterns()
{
    // Parameters: defaults, rest, patterns, and what they do to the list.
    CHECK_EQ(parse("function f(a = 1) {}"), program_of("(function f ((= a (number 1))))"));
    CHECK_EQ(parse("function f(...r) {}"), program_of("(function f ((... r)))"));
    CHECK_EQ(parse("function f([a], {b}) {}"), program_of("(function f ((array-pattern (id a)) (object-pattern (\"b\" (id b)))))"));
    CHECK_EQ(parse("function f(a, b = a, ...[c, d]) {}"), program_of("(function f (a (= b (id a)) (... (array-pattern (id c) (id d)))))"));
    CHECK_EQ(parse("function f(a = 1) { 'use strict'; }"), "Illegal 'use strict' directive in function with non-simple parameter list");
    CHECK_EQ(parse("function f(a, a = 1) {}"), "Duplicate parameter name not allowed in this context");
    CHECK_EQ(parse("function f(a, [a]) {}"), "Duplicate parameter name not allowed in this context");
    CHECK_EQ(parse("function f(a, a) {}"), program_of("(function f (a a))"));
    CHECK_EQ(parse("function f(...a, b) {}"), "Rest parameter must be last formal parameter");
    CHECK_EQ(parse("function f(...a,) {}"), "Rest parameter must be last formal parameter");
    CHECK_EQ(parse("function f(...a = []) {}"), "Rest parameter may not have a default initializer");
    CHECK_EQ(parse("function f([a]) { let a; }"), "Identifier 'a' has already been declared");
    CHECK_EQ(parse("({ set x(v = 1) {} })"), expression_of("(object (set \"x\" (function x ((= v (number 1))))))"));
    CHECK_EQ(parse("({ set x(...v) {} })"), "Setter must have exactly one formal parameter");
    CHECK_EQ(parse("({ get x([a]) {} })"), "Getter must not have any formal parameters");
    CHECK_EQ(parse_strict("function f([eval]) {}"), "Unexpected eval or arguments in strict mode");
    Parsed parsed = parse_source("function f(a, b = 1, c) {} function g(...r) {} function h([a], b) {} function k(a, b) {}");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 0);
        js::FunctionNode const* g = function_in(parsed.program->body, 1);
        js::FunctionNode const* h = function_in(parsed.program->body, 2);
        js::FunctionNode const* k = function_in(parsed.program->body, 3);
        CHECK(f && f->expected_argument_count == 1 && !f->has_simple_parameter_list && f->has_parameter_expressions);
        CHECK(g && g->expected_argument_count == 0 && !g->has_simple_parameter_list && !g->has_parameter_expressions);
        CHECK(h && h->expected_argument_count == 2 && !h->has_simple_parameter_list && !h->has_parameter_expressions);
        CHECK(k && k->expected_argument_count == 2 && k->has_simple_parameter_list && !k->has_parameter_expressions);
    }
    parsed = parse_source("function f([a = 1]) {} function g({[k]: v}) {}");
    CHECK(parsed.program != nullptr);
    if (parsed.program) {
        js::FunctionNode const* f = function_in(parsed.program->body, 0);
        js::FunctionNode const* g = function_in(parsed.program->body, 1);
        CHECK(f && f->has_parameter_expressions);
        CHECK(g && g->has_parameter_expressions);
    }

    // Assignment patterns, converted from the literal that covered them.
    CHECK_EQ(parse("[a, b] = c"), expression_of("(assign = (array-pattern (id a) (id b)) (id c))"));
    CHECK_EQ(parse("[a, , b = 1, ...c] = d"), expression_of("(assign = (array-pattern (id a) hole (= (id b) (number 1)) (... (id c))) (id d))"));
    CHECK_EQ(parse("[o.x, o[y], [z]] = d"), expression_of("(assign = (array-pattern (member (id o) x) (index (id o) (id y)) (array-pattern (id z))) (id d))"));
    CHECK_EQ(parse("({a} = b)"), expression_of("(assign = (object-pattern (\"a\" (id a))) (id b))"));
    CHECK_EQ(parse("({a = 1} = b)"), expression_of("(assign = (object-pattern (\"a\" (= (id a) (number 1)))) (id b))"));
    CHECK_EQ(parse("({a: b.c = 1, [d]: e, ...f} = g)"), expression_of("(assign = (object-pattern (\"a\" (= (member (id b) c) (number 1))) ((computed (id d)) (id e)) (... (id f))) (id g))"));
    CHECK_EQ(parse("[[a] = [1]] = b"), expression_of("(assign = (array-pattern (= (array-pattern (id a)) (array (number 1)))) (id b))"));
    CHECK_EQ(parse("[(a)] = b"), expression_of("(assign = (array-pattern (id a)) (id b))"));
    CHECK_EQ(parse("[(a.b)] = c"), expression_of("(assign = (array-pattern (member (id a) b)) (id c))"));
    CHECK_EQ(parse("x = [a] = b"), expression_of("(assign = (id x) (assign = (array-pattern (id a)) (id b)))"));
    CHECK_EQ(parse("for ([a, b] of c) ;"), program_of("(for-of (array-pattern (id a) (id b)) (id c) (empty))"));
    CHECK_EQ(parse("for ({a} in c) ;"), program_of("(for-in (object-pattern (\"a\" (id a))) (id c) (empty))"));
    CHECK_EQ(parse("[a + 1] = b"), "Invalid destructuring assignment target");
    CHECK_EQ(parse("[f()] = b"), "Invalid destructuring assignment target");
    CHECK_EQ(parse("[([a])] = b"), "Invalid destructuring assignment target");
    CHECK_EQ(parse("[...a, b] = c"), "Rest element must be last element");
    CHECK_EQ(parse("[...a,] = c"), "Rest element must be last element");
    CHECK_EQ(parse("[...a = 1] = c"), "Rest element may not have a default initializer");
    CHECK_EQ(parse("[...[a]] = c"), expression_of("(assign = (array-pattern (... (array-pattern (id a)))) (id c))"));
    CHECK_EQ(parse("({...a,} = c)"), "Rest element must be last element");
    CHECK_EQ(parse("({...{a}} = c)"), "`...` must be followed by an assignable reference in assignment contexts");
    CHECK_EQ(parse("({...a.b} = c)"), expression_of("(assign = (object-pattern (... (member (id a) b))) (id c))"));
    CHECK_EQ(parse("({get a() {}} = b)"), "Invalid destructuring assignment target");
    CHECK_EQ(parse("({a() {}} = b)"), "Invalid destructuring assignment target");
    CHECK_EQ(parse("({a: 1} = b)"), "Invalid destructuring assignment target");
    CHECK_EQ(parse("[a] += b"), "Invalid left-hand side in assignment");
    CHECK_EQ(parse("([a]) = b"), "Invalid left-hand side in assignment");
    CHECK_EQ(parse("[a?.b] = c"), "Invalid left-hand side in assignment");
    CHECK_EQ(parse_strict("[eval] = c"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse_strict("({a: arguments} = c)"), "Unexpected eval or arguments in strict mode");
    CHECK_EQ(parse("[{a = 1}.b] = c"), "Invalid shorthand property initializer");
    CHECK_EQ(parse("[a = {b = 1}] = c"), "Invalid shorthand property initializer");
    CHECK_EQ(parse("for ([a] = [1] of c) ;"), "Invalid left-hand side in for-of loop");
    CHECK_EQ(parse("for ({a = 1}; ; ) ;"), "Invalid shorthand property initializer");
    CHECK_EQ(parse("for ([a], b in c) ;"), "Invalid left-hand side in for-in loop");
}

// Classes, super and new.target.
void test_classes()
{
    CHECK_EQ(parse("class A {}"), program_of("(class A)"));
    CHECK_EQ(parse("x = class {}"), expression_of("(assign = (id x) (class))"));
    CHECK_EQ(parse("class A extends B { constructor(x) { super(x); } m() { return super.m(); } static s() {} get g() {} set g(v) {} }"),
        program_of("(class A (extends (id B)) (constructor (function A (x) (expr (super-call (id x))))) (method \"m\" (function m () (return (call (super-member m))))) (static method \"s\" (function s ())) (get \"g\" (function g ())) (set \"g\" (function g (v))))"));
    CHECK_EQ(parse("class A { x = 1; y; static z = 2; static { z; } [k] = 3; 'q'() {} static static() {} }"),
        program_of("(class A (field \"x\" (function x () (number 1))) (field \"y\") (static field \"z\" (function z () (number 2))) (static block (function () (expr (id z)))) (field (computed (id k)) (function () (number 3))) (method \"q\" (function q ())) (static method \"static\" (function static ())))"));
    CHECK_EQ(parse("class A { m() { new.target; super['x']; } }"), program_of("(class A (method \"m\" (function m () (expr new.target) (expr (super-index (string \"x\"))))))"));
    CHECK_EQ(parse("class A { if() {} }"), program_of("(class A (method \"if\" (function if ())))"));
    CHECK_EQ(parse("class A extends (B, C) {}"), program_of("(class A (extends (seq (id B) (id C))))"));
    CHECK_EQ(parse("class {}"), "Unexpected token '{'");
    CHECK_EQ(parse("class A { constructor() {} constructor() {} }"), "A class may only have one constructor");
    CHECK_EQ(parse("class A { get constructor() {} }"), "Class constructor may not be an accessor");
    CHECK_EQ(parse("class A { constructor = 1 }"), "Classes may not have a field named 'constructor'");
    CHECK_EQ(parse("class A { static prototype() {} }"), "Classes may not have a static property named 'prototype'");
    CHECK_EQ(parse("class A { static prototype = 1 }"), "Classes may not have a static property named 'prototype'");
    CHECK_EQ(parse("class A { m() { super(); } }"), "'super' keyword unexpected here");
    CHECK_EQ(parse("class A { constructor() { super(); } }"), "'super' keyword unexpected here");
    CHECK_EQ(parse("class A extends B { constructor() { function f() { super(); } } }"), "'super' keyword unexpected here");
    CHECK_EQ(parse("class A extends B { constructor() { var f = () => super(); } }"), program_of("(class A (extends (id B)) (constructor (function A () (var (f (arrow () (super-call)))))))"));
    CHECK_EQ(parse("function f() { super.x; }"), "'super' keyword unexpected here");
    CHECK_EQ(parse("({ m() { super.x; } })"), expression_of("(object (init \"m\" (function m () (expr (super-member x)))))"));
    CHECK_EQ(parse("({ m: function () { super.x; } })"), "'super' keyword unexpected here");
    CHECK_EQ(parse("super.x"), "'super' keyword unexpected here");
    CHECK_EQ(parse("class A { m() { super; } }"), "'super' keyword unexpected here");
    CHECK_EQ(parse("new.target"), "new.target expression is not allowed here");
    CHECK_EQ(parse("function f() { return new.target; }"), program_of("(function f () (return new.target))"));
    CHECK_EQ(parse("function f() { return () => new.target; }"), program_of("(function f () (return (arrow () new.target)))"));
    CHECK_EQ(parse("new.other"), "Unexpected identifier 'other'");
    CHECK_EQ(parse("class A { x = arguments; }"), "'arguments' is not allowed in class field initializer or static initialization block");
    CHECK_EQ(parse("class A { static { return; } }"), "Illegal return statement");
    CHECK_EQ(parse("class A { static { function f() { return 1; } } }"), program_of("(class A (static block (function () (function f () (return (number 1))))))"));
    CHECK_EQ(parse("class A { m() { with (x) {} } }"), "Strict mode code may not include a with statement");
    CHECK_EQ(parse("class A extends function () { with (x) {} } {}"), "Strict mode code may not include a with statement");
    CHECK_EQ(parse("class A {} with (x) {}"), program_of("(class A) (with (id x) (block))"));
    CHECK_EQ(parse("class yield {}"), "Unexpected strict mode reserved word");
    CHECK_EQ(parse("class A { #p = 1; }"), "private class members are not supported yet");
    CHECK_EQ(parse("if (x) class A {}"), "Lexical declaration cannot appear in a single-statement context");
    CHECK_EQ(parse("class A {} let A;"), "Identifier 'A' has already been declared");
    CHECK_EQ(parse("class A { *g() {} }"), "generators are not supported yet");
    CHECK_EQ(parse("class A { async m() {} }"), "async functions are not supported yet");
}

void test_unsupported_features()
{
    CHECK_EQ(parse("f(...a)"), program_of("(expr (call (id f) (spread (id a))))"));
    CHECK_EQ(parse("async function f() {}"), "async functions are not supported yet");
    CHECK_EQ(parse("await x"), "async functions are not supported yet");
    CHECK_EQ(parse("for await (x of y);"), "async functions are not supported yet");
    CHECK_EQ(parse("function* g() {}"), "generators are not supported yet");
    CHECK_EQ(parse("(function* () {})"), "generators are not supported yet");
    CHECK_EQ(parse("yield x"), "generators are not supported yet");
    CHECK_EQ(parse("import x from 'y'"), "modules are not supported yet");
    CHECK_EQ(parse("export var x"), "modules are not supported yet");
    CHECK_EQ(parse("x = 10n"), "BigInt literals are not supported");
    CHECK_EQ(parse("({a: 1}) = b"), "Invalid left-hand side in assignment");
    CHECK_EQ(parse("async (a = 1) => 1"), "async functions are not supported yet");
    CHECK_EQ(parse("async ((a)) => 1"), "async functions are not supported yet");
    CHECK_EQ(parse("async function f() {}"), "async functions are not supported yet");
    CHECK_EQ(parse("(a,)"), "Unexpected token ')'");
    CHECK_EQ(parse("(a, b,)"), "Unexpected token ')'");
    CHECK_EQ(parse("await"), expression_of("(id await)"));
    CHECK_EQ(parse("await(x)"), expression_of("(call (id await) (id x))"));
    CHECK_EQ(parse("x = enum"), "Unexpected token 'enum'");
}

void test_function_constructor()
{
    js::ParseError error;
    std::unique_ptr<js::Program> program = js::Parser::parse_function_constructor(heap(), u"a, b", u"return a + b", &error);
    CHECK(program != nullptr);
    if (program) {
        CHECK_EQ(js::dump_ast(*program), expression_of("(function anonymous (a b) (return (binary + (id a) (id b))))"));
        js::FunctionNode const* fn = function_in(program->body, 0);
        CHECK(fn && source_of(*program, *fn) == "function anonymous(a, b\n) {\nreturn a + b\n}");
        CHECK(fn && fn->is_constructable && !fn->is_strict);
    }
    program = js::Parser::parse_function_constructor(heap(), u"", u"", &error);
    CHECK(program != nullptr);
    program = js::Parser::parse_function_constructor(heap(), u"", u"'use strict'; return this", &error);
    CHECK(program != nullptr);
    if (program) {
        js::FunctionNode const* fn = function_in(program->body, 0);
        CHECK(fn && fn->is_strict && fn->uses_this);
    }
    program = js::Parser::parse_function_constructor(heap(), u"a b", u"", &error);
    CHECK(program == nullptr);
    CHECK_EQ(error.message, "Unexpected identifier 'b'");
    program = js::Parser::parse_function_constructor(heap(), u"", u"return", &error);
    CHECK(program != nullptr);
    program = js::Parser::parse_function_constructor(heap(), u"", u"}); evil(); (function() {", &error);
    CHECK(program == nullptr);
    program = js::Parser::parse_function_constructor(heap(), u"a", u"return a +", &error);
    CHECK(program == nullptr);
    CHECK_EQ(error.message, "Unexpected token '}'");
}

} // namespace

int main()
{
    test_precedence();
    test_unary();
    test_asi();
    test_regex_versus_division();
    test_arrows();
    test_templates();
    test_object_literals();
    test_array_literals();
    test_statements();
    test_labels();
    test_strict_mode();
    test_declarations();
    test_function_nodes();
    test_new_and_calls();
    test_optional_chaining();
    test_error_positions();
    test_nesting_cap();
    test_unsupported_features();
    test_patterns();
    test_classes();
    test_function_constructor();
    return sashfold::test::report("js_parser");
}
