#pragma once

// The syntax tree the parser builds and the evaluator walks. A Program
// owns every node of its tree (an arena of unique_ptrs) and its own source
// text, and lives as long as the realm that ran it, since functions keep
// pointing into it. Names and string literals are heap atoms — permanent,
// so the tree holds them without rooting.
//
// Coverage: ES5 whole, plus the pieces of later editions a tree-walker
// takes cheaply and real pages use everywhere — let/const with block
// scoping, arrow functions, template literals, `**`, `??`, `?.`, optional
// catch binding, shorthand and computed property names. Classes,
// generators, destructuring, spread, modules and async are parse errors
// that name themselves.

#include "js/Value.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::js {

class JsString;
class Program;

struct SourcePosition {
    std::uint32_t offset = 0; // code units from the start
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

enum class NodeType : std::uint8_t {
    // expressions
    Identifier,
    NumberLiteral,
    StringLiteral,
    BooleanLiteral,
    NullLiteral,
    ThisExpression,
    RegExpLiteral,
    TemplateLiteral,
    ArrayLiteral,
    ObjectLiteral,
    FunctionExpression,
    ArrowFunction,
    UnaryExpression,
    UpdateExpression,
    BinaryExpression,
    LogicalExpression,
    AssignmentExpression,
    ConditionalExpression,
    CallExpression,
    NewExpression,
    MemberExpression,
    SequenceExpression,
    // statements
    VariableDeclaration,
    FunctionDeclaration,
    ExpressionStatement,
    BlockStatement,
    EmptyStatement,
    IfStatement,
    ForStatement,
    ForInStatement,
    ForOfStatement,
    WhileStatement,
    DoWhileStatement,
    ReturnStatement,
    BreakStatement,
    ContinueStatement,
    ThrowStatement,
    TryStatement,
    SwitchStatement,
    LabeledStatement,
    WithStatement,
    DebuggerStatement,
};

struct Node {
    NodeType type;
    SourcePosition position;
    std::uint32_t end_offset = 0;

    virtual ~Node() = default;

protected:
    explicit Node(NodeType t)
        : type(t)
    {
    }
};

struct Expression : Node {
    // Written inside parentheses. The tree keeps no node for them, but an
    // optional chain ends at a parenthesised expression (§13.3.9), and a
    // few early errors in the parser look through them.
    bool parenthesized = false;

protected:
    using Node::Node;
};

struct Statement : Node {
protected:
    using Node::Node;
};

struct FunctionDeclaration;

// What a scope declares, gathered while parsing so that instantiating it
// (§16.1.7, §10.2.11, §14.2.3) walks lists rather than the tree.
struct Declarations {
    // `var` names, parameters excluded, first occurrence only.
    std::vector<JsString*> vars;
    // Hoisted function declarations in source order; for a name declared
    // twice the last one wins.
    std::vector<FunctionDeclaration const*> functions;
    // let/const at this level: the name and whether it is const.
    std::vector<std::pair<JsString*, bool>> lexicals;
};

struct Identifier : Expression {
    Identifier()
        : Expression(NodeType::Identifier)
    {
    }
    JsString* name = nullptr;
};

struct NumberLiteral : Expression {
    NumberLiteral()
        : Expression(NodeType::NumberLiteral)
    {
    }
    double value = 0;
};

struct StringLiteral : Expression {
    StringLiteral()
        : Expression(NodeType::StringLiteral)
    {
    }
    JsString* value = nullptr;
};

struct BooleanLiteral : Expression {
    BooleanLiteral()
        : Expression(NodeType::BooleanLiteral)
    {
    }
    bool value = false;
};

struct NullLiteral : Expression {
    NullLiteral()
        : Expression(NodeType::NullLiteral)
    {
    }
};

struct ThisExpression : Expression {
    ThisExpression()
        : Expression(NodeType::ThisExpression)
    {
    }
};

struct RegExpLiteral : Expression {
    RegExpLiteral()
        : Expression(NodeType::RegExpLiteral)
    {
    }
    JsString* pattern = nullptr;
    JsString* flags = nullptr;
};

// `a${b}c`: quasis.size() == expressions.size() + 1.
struct TemplateLiteral : Expression {
    TemplateLiteral()
        : Expression(NodeType::TemplateLiteral)
    {
    }
    std::vector<JsString*> cooked;
    std::vector<JsString*> raw;
    std::vector<Expression*> expressions;
};

struct ArrayLiteral : Expression {
    ArrayLiteral()
        : Expression(NodeType::ArrayLiteral)
    {
    }
    std::vector<Expression*> elements; // nullptr = a hole
};

struct PropertyDefinition {
    enum class Kind : std::uint8_t { Init, Get, Set };
    Kind kind = Kind::Init;
    JsString* key = nullptr; // a numeric key is stored as its canonical string
    Expression* computed_key = nullptr; // `[expr]: value`; key is null then
    Expression* value = nullptr; // a FunctionExpression for Get/Set
    bool is_proto = false; // `__proto__: value` sets the prototype (§13.2.5.5)
    bool is_anonymous_function = false; // the value takes the key as its name
};

struct ObjectLiteral : Expression {
    ObjectLiteral()
        : Expression(NodeType::ObjectLiteral)
    {
    }
    std::vector<PropertyDefinition> properties;
};

struct FunctionNode {
    JsString* name = nullptr; // null when anonymous
    std::vector<JsString*> parameters;
    std::vector<Statement*> body; // empty when expression_body is set
    Expression* expression_body = nullptr; // an arrow's concise body
    Declarations declarations; // the function's own var scope
    SourcePosition position;
    Program const* program = nullptr; // the tree this node belongs to; its source holds the text
    std::uint32_t source_start = 0; // for Function.prototype.toString, into program->source
    std::uint32_t source_end = 0;
    bool is_arrow = false;
    bool is_strict = false;
    bool is_constructable = true; // false for arrows, getters, setters
    bool is_getter = false;
    bool is_setter = false;
    bool uses_arguments = false; // `arguments` appears in the body (not in a nested non-arrow function)
    bool uses_this = false;
    bool has_direct_eval = false; // a direct `eval(...)` call inside; everything must stay in scope
    bool has_duplicate_parameters = false;
};

struct FunctionExpression : Expression {
    FunctionExpression()
        : Expression(NodeType::FunctionExpression)
    {
    }
    FunctionNode* function = nullptr;
};

struct ArrowFunction : Expression {
    ArrowFunction()
        : Expression(NodeType::ArrowFunction)
    {
    }
    FunctionNode* function = nullptr;
};

enum class UnaryOp : std::uint8_t { Minus, Plus, Not, BitwiseNot, Typeof, Void, Delete };

struct UnaryExpression : Expression {
    UnaryExpression()
        : Expression(NodeType::UnaryExpression)
    {
    }
    UnaryOp op = UnaryOp::Minus;
    Expression* operand = nullptr;
};

struct UpdateExpression : Expression {
    UpdateExpression()
        : Expression(NodeType::UpdateExpression)
    {
    }
    bool increment = true;
    bool prefix = true;
    Expression* target = nullptr; // Identifier or MemberExpression
};

enum class BinaryOp : std::uint8_t {
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Exponent,
    LeftShift,
    RightShift,
    UnsignedRightShift,
    BitwiseAnd,
    BitwiseOr,
    BitwiseXor,
    Equal,
    NotEqual,
    StrictEqual,
    StrictNotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    In,
    Instanceof,
};

struct BinaryExpression : Expression {
    BinaryExpression()
        : Expression(NodeType::BinaryExpression)
    {
    }
    BinaryOp op = BinaryOp::Add;
    Expression* left = nullptr;
    Expression* right = nullptr;
};

enum class LogicalOp : std::uint8_t { And, Or, Nullish };

struct LogicalExpression : Expression {
    LogicalExpression()
        : Expression(NodeType::LogicalExpression)
    {
    }
    LogicalOp op = LogicalOp::And;
    Expression* left = nullptr;
    Expression* right = nullptr;
};

enum class AssignmentOp : std::uint8_t {
    Assign,
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Exponent,
    LeftShift,
    RightShift,
    UnsignedRightShift,
    BitwiseAnd,
    BitwiseOr,
    BitwiseXor,
    LogicalAnd,
    LogicalOr,
    Nullish,
};

struct AssignmentExpression : Expression {
    AssignmentExpression()
        : Expression(NodeType::AssignmentExpression)
    {
    }
    AssignmentOp op = AssignmentOp::Assign;
    Expression* target = nullptr; // Identifier or MemberExpression
    Expression* value = nullptr;
};

struct ConditionalExpression : Expression {
    ConditionalExpression()
        : Expression(NodeType::ConditionalExpression)
    {
    }
    Expression* test = nullptr;
    Expression* consequent = nullptr;
    Expression* alternate = nullptr;
};

struct CallExpression : Expression {
    CallExpression()
        : Expression(NodeType::CallExpression)
    {
    }
    Expression* callee = nullptr;
    std::vector<Expression*> arguments;
    bool is_direct_eval = false; // the callee is the plain identifier `eval`
    bool optional = false; // `a?.()`
};

struct NewExpression : Expression {
    NewExpression()
        : Expression(NodeType::NewExpression)
    {
    }
    Expression* callee = nullptr;
    std::vector<Expression*> arguments;
};

struct MemberExpression : Expression {
    MemberExpression()
        : Expression(NodeType::MemberExpression)
    {
    }
    Expression* object = nullptr;
    JsString* name = nullptr; // `a.b`
    Expression* property = nullptr; // `a[b]`; name is null then
    bool optional = false; // `a?.b`
};

struct SequenceExpression : Expression {
    SequenceExpression()
        : Expression(NodeType::SequenceExpression)
    {
    }
    std::vector<Expression*> expressions;
};

struct VariableDeclarator {
    JsString* name = nullptr;
    Expression* init = nullptr;
};

struct VariableDeclaration : Statement {
    VariableDeclaration()
        : Statement(NodeType::VariableDeclaration)
    {
    }
    enum class Kind : std::uint8_t { Var, Let, Const };
    Kind kind = Kind::Var;
    std::vector<VariableDeclarator> declarations;
};

struct FunctionDeclaration : Statement {
    FunctionDeclaration()
        : Statement(NodeType::FunctionDeclaration)
    {
    }
    FunctionNode* function = nullptr;
    // B.3.2.1: a block-level declaration in sloppy code that the parser
    // also hoisted as a var; evaluating it copies the block binding out.
    bool annex_b_hoisted = false;
};

struct ExpressionStatement : Statement {
    ExpressionStatement()
        : Statement(NodeType::ExpressionStatement)
    {
    }
    Expression* expression = nullptr;
};

struct BlockStatement : Statement {
    BlockStatement()
        : Statement(NodeType::BlockStatement)
    {
    }
    std::vector<Statement*> body;
    Declarations declarations; // lexicals and block-level functions only
};

struct EmptyStatement : Statement {
    EmptyStatement()
        : Statement(NodeType::EmptyStatement)
    {
    }
};

struct DebuggerStatement : Statement {
    DebuggerStatement()
        : Statement(NodeType::DebuggerStatement)
    {
    }
};

struct IfStatement : Statement {
    IfStatement()
        : Statement(NodeType::IfStatement)
    {
    }
    Expression* test = nullptr;
    Statement* consequent = nullptr;
    Statement* alternate = nullptr;
};

struct ForStatement : Statement {
    ForStatement()
        : Statement(NodeType::ForStatement)
    {
    }
    Statement* init = nullptr; // a VariableDeclaration or an ExpressionStatement
    Expression* test = nullptr;
    Expression* update = nullptr;
    Statement* body = nullptr;
    Declarations declarations; // a let/const in the head, copied per iteration (§14.7.4.4)
};

struct ForInStatement : Statement {
    ForInStatement()
        : Statement(NodeType::ForInStatement)
    {
    }
    VariableDeclaration* declaration = nullptr; // `for (var x in …)`
    Expression* target = nullptr; // `for (x in …)`; declaration is null then
    Expression* object = nullptr;
    Statement* body = nullptr;
};

struct ForOfStatement : Statement {
    ForOfStatement()
        : Statement(NodeType::ForOfStatement)
    {
    }
    VariableDeclaration* declaration = nullptr; // `for (const x of …)`
    Expression* target = nullptr; // `for (x of …)`; declaration is null then
    Expression* iterable = nullptr;
    Statement* body = nullptr;
};

struct WhileStatement : Statement {
    WhileStatement()
        : Statement(NodeType::WhileStatement)
    {
    }
    Expression* test = nullptr;
    Statement* body = nullptr;
};

struct DoWhileStatement : Statement {
    DoWhileStatement()
        : Statement(NodeType::DoWhileStatement)
    {
    }
    Statement* body = nullptr;
    Expression* test = nullptr;
};

struct ReturnStatement : Statement {
    ReturnStatement()
        : Statement(NodeType::ReturnStatement)
    {
    }
    Expression* argument = nullptr;
};

struct BreakStatement : Statement {
    BreakStatement()
        : Statement(NodeType::BreakStatement)
    {
    }
    JsString* label = nullptr;
};

struct ContinueStatement : Statement {
    ContinueStatement()
        : Statement(NodeType::ContinueStatement)
    {
    }
    JsString* label = nullptr;
};

struct ThrowStatement : Statement {
    ThrowStatement()
        : Statement(NodeType::ThrowStatement)
    {
    }
    Expression* argument = nullptr;
};

struct TryStatement : Statement {
    TryStatement()
        : Statement(NodeType::TryStatement)
    {
    }
    BlockStatement* block = nullptr;
    JsString* catch_parameter = nullptr; // null for `catch {` and when there is no handler
    BlockStatement* handler = nullptr;
    BlockStatement* finalizer = nullptr;
};

struct SwitchCase {
    Expression* test = nullptr; // null for `default:`
    std::vector<Statement*> consequent;
};

struct SwitchStatement : Statement {
    SwitchStatement()
        : Statement(NodeType::SwitchStatement)
    {
    }
    Expression* discriminant = nullptr;
    std::vector<SwitchCase> cases;
    Declarations declarations; // the case block's lexicals
};

struct LabeledStatement : Statement {
    LabeledStatement()
        : Statement(NodeType::LabeledStatement)
    {
    }
    JsString* label = nullptr;
    Statement* body = nullptr;
};

struct WithStatement : Statement {
    WithStatement()
        : Statement(NodeType::WithStatement)
    {
    }
    Expression* object = nullptr;
    Statement* body = nullptr;
};

class Program {
public:
    std::vector<Statement*> body;
    Declarations declarations;
    bool is_strict = false;
    std::u16string source;
    std::string name; // where it came from, for error messages

    template<typename T>
    T* make()
    {
        auto node = std::make_unique<T>();
        T* raw = node.get();
        m_nodes.push_back(std::move(node));
        return raw;
    }
    FunctionNode* make_function()
    {
        auto node = std::make_unique<FunctionNode>();
        FunctionNode* raw = node.get();
        raw->program = this;
        m_functions.push_back(std::move(node));
        return raw;
    }
    std::size_t node_count() const { return m_nodes.size(); }

private:
    std::vector<std::unique_ptr<Node>> m_nodes;
    std::vector<std::unique_ptr<FunctionNode>> m_functions;
};

}
