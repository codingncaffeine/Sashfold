#include "js/Parser.h"

#include "js/Heap.h"
#include "js/Lexer.h"
#include "js/Regex.h" // RegexFlags::parse, the flags' early error
#include "js/Strings.h" // number_to_string, number_to_utf8, utf8_from_utf16

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// A recursive-descent parser for the grammar Ast.h covers (§13–§16). Two
// things shape it:
//
// 1. No exceptions. Every parse function returns null (or false) on the
//    first error, with the ParseError already recorded, and each caller
//    checks and unwinds. The first error is the one reported.
// 2. A script is input-controlled data, so recursion is capped: every
//    statement, expression, parenthesis and array/object literal level
//    counts against Parser::max_nesting_depth.
//
// The lexer cannot tell `/` division from a regular expression, so the
// parser lexes each lookahead with its best guess from the token before
// it and re-lexes the one position where the guess can be wrong: a `/`
// where a primary expression is wanted becomes a regular expression, and a
// regular expression where an operator is wanted becomes division.
//
// dump_ast writes the S-expression the tests assert against:
//
//   (program [strict] stmt…)
//   statements
//     (var (name init?)…) (let (name init?)…) (const (name init?)…)
//     (function name (params…) body…)        name omitted when anonymous
//     (expr e) (block s…) (if c then else?) (empty) (debugger)
//     (for init test update body)             an absent part prints as -
//     (for-in decl|target obj body)           decl is (var (x)) / (let (x)) / (const (x))
//     (while c b) (do b c) (return e?) (break label?) (continue label?) (throw e)
//     (try block (catch param? block)? (finally block)?)
//     (switch d (case test s…)… (default s…))
//     (label name s) (with o s)
//   expressions
//     (id x) (number 1) (string "x") true false null this (regex /p/f)
//     (template ("cooked"…) (exprs…))
//     (array e|hole …)
//     (object (init "key" v) (get "key" f) (set "key" f) (proto v)
//             (computed k v) (get computed k f) (set computed k f)…)
//     (function name (params…) body…) (arrow (params…) body-statements…|expr)
//     (unary op e) (update ++|-- prefix|postfix e)
//     (binary op l r) (logical op l r) (assign op target v) (cond t c a)
//     (call callee args…) (call? callee args…) (new callee args…)
//     (member obj name) (member? obj name) (index obj e) (index? obj e)
//     (seq e…)
//   Numbers print through number_to_utf8; strings through utf8_from_utf16
//   with " and \ escaped; nothing else is escaped.

namespace sashfold::js {

namespace {

constexpr std::u16string_view use_strict_directive = u"use strict";

char16_t const* punctuator_text(Punctuator p)
{
    switch (p) {
    case Punctuator::LeftBrace: return u"{";
    case Punctuator::RightBrace: return u"}";
    case Punctuator::LeftParen: return u"(";
    case Punctuator::RightParen: return u")";
    case Punctuator::LeftBracket: return u"[";
    case Punctuator::RightBracket: return u"]";
    case Punctuator::Dot: return u".";
    case Punctuator::Ellipsis: return u"...";
    case Punctuator::Semicolon: return u";";
    case Punctuator::Comma: return u",";
    case Punctuator::Less: return u"<";
    case Punctuator::Greater: return u">";
    case Punctuator::LessEqual: return u"<=";
    case Punctuator::GreaterEqual: return u">=";
    case Punctuator::Equal: return u"==";
    case Punctuator::NotEqual: return u"!=";
    case Punctuator::StrictEqual: return u"===";
    case Punctuator::StrictNotEqual: return u"!==";
    case Punctuator::Plus: return u"+";
    case Punctuator::Minus: return u"-";
    case Punctuator::Star: return u"*";
    case Punctuator::Slash: return u"/";
    case Punctuator::Percent: return u"%";
    case Punctuator::StarStar: return u"**";
    case Punctuator::PlusPlus: return u"++";
    case Punctuator::MinusMinus: return u"--";
    case Punctuator::LeftShift: return u"<<";
    case Punctuator::RightShift: return u">>";
    case Punctuator::UnsignedRightShift: return u">>>";
    case Punctuator::Ampersand: return u"&";
    case Punctuator::Pipe: return u"|";
    case Punctuator::Caret: return u"^";
    case Punctuator::Exclamation: return u"!";
    case Punctuator::Tilde: return u"~";
    case Punctuator::AmpersandAmpersand: return u"&&";
    case Punctuator::PipePipe: return u"||";
    case Punctuator::QuestionQuestion: return u"??";
    case Punctuator::Question: return u"?";
    case Punctuator::QuestionDot: return u"?.";
    case Punctuator::Colon: return u":";
    case Punctuator::Assign: return u"=";
    case Punctuator::PlusAssign: return u"+=";
    case Punctuator::MinusAssign: return u"-=";
    case Punctuator::StarAssign: return u"*=";
    case Punctuator::SlashAssign: return u"/=";
    case Punctuator::PercentAssign: return u"%=";
    case Punctuator::StarStarAssign: return u"**=";
    case Punctuator::LeftShiftAssign: return u"<<=";
    case Punctuator::RightShiftAssign: return u">>=";
    case Punctuator::UnsignedRightShiftAssign: return u">>>=";
    case Punctuator::AmpersandAssign: return u"&=";
    case Punctuator::PipeAssign: return u"|=";
    case Punctuator::CaretAssign: return u"^=";
    case Punctuator::AndAssign: return u"&&=";
    case Punctuator::OrAssign: return u"||=";
    case Punctuator::NullishAssign: return u"??" u"=";
    case Punctuator::Arrow: return u"=>";
    }
    return u"?";
}

// The words §12.7.2 reserves only in strict mode code; the lexer hands
// them over as identifiers.
bool is_strict_reserved_word(std::u16string_view name)
{
    return name == u"implements" || name == u"interface" || name == u"let" || name == u"package"
        || name == u"private" || name == u"protected" || name == u"public" || name == u"static" || name == u"yield";
}

bool is_eval_or_arguments(std::u16string_view name)
{
    return name == u"eval" || name == u"arguments";
}

std::string describe_token(Token const& token)
{
    switch (token.type) {
    case TokenType::EndOfInput:
        return "Unexpected end of input";
    case TokenType::Identifier:
        return "Unexpected identifier '" + utf8_from_utf16(token.value) + "'";
    case TokenType::Keyword:
        return "Unexpected token '" + utf8_from_utf16(token.value) + "'";
    case TokenType::Punctuator:
        return "Unexpected token '" + utf8_from_utf16(punctuator_text(token.punctuator)) + "'";
    case TokenType::Number:
        return "Unexpected number";
    case TokenType::String:
        return "Unexpected string";
    case TokenType::Template:
        return "Unexpected template string";
    case TokenType::RegExp:
        return "Unexpected regular expression";
    case TokenType::Invalid:
        return token.message;
    }
    return "Unexpected token";
}

// Whether a `/` after this token is division (the token can end an
// expression) rather than the start of a regular expression. The parser
// re-lexes the cases where the token alone cannot decide (see the file
// comment), so this only has to be right most of the time.
bool ends_expression(Token const& token)
{
    switch (token.type) {
    case TokenType::Identifier:
    case TokenType::Number:
    case TokenType::String:
    case TokenType::RegExp:
    case TokenType::EndOfInput:
        return true;
    case TokenType::Keyword:
        return token.keyword == Keyword::This || token.keyword == Keyword::Null || token.keyword == Keyword::True
            || token.keyword == Keyword::False || token.keyword == Keyword::Super;
    case TokenType::Template:
        return token.template_tail;
    case TokenType::Punctuator:
        return token.punctuator == Punctuator::RightParen || token.punctuator == Punctuator::RightBracket
            || token.punctuator == Punctuator::RightBrace || token.punctuator == Punctuator::PlusPlus
            || token.punctuator == Punctuator::MinusMinus;
    case TokenType::Invalid:
        return false;
    }
    return false;
}

// Binary operator precedence (§13.6–§13.13); 0 means "not a binary
// operator here". `in` is excluded where a for-head forbids it.
struct BinaryOperator {
    int precedence = 0;
    bool logical = false;
    BinaryOp op = BinaryOp::Add;
    LogicalOp logical_op = LogicalOp::And;
};

BinaryOperator binary_operator_for(Token const& token, bool allow_in)
{
    BinaryOperator result;
    if (token.type == TokenType::Keyword) {
        if (token.keyword == Keyword::In && allow_in) {
            result.precedence = 8;
            result.op = BinaryOp::In;
        } else if (token.keyword == Keyword::Instanceof) {
            result.precedence = 8;
            result.op = BinaryOp::Instanceof;
        }
        return result;
    }
    if (token.type != TokenType::Punctuator)
        return result;
    auto binary = [&](int precedence, BinaryOp op) {
        result.precedence = precedence;
        result.op = op;
    };
    auto logical = [&](int precedence, LogicalOp op) {
        result.precedence = precedence;
        result.logical = true;
        result.logical_op = op;
    };
    switch (token.punctuator) {
    case Punctuator::QuestionQuestion: logical(1, LogicalOp::Nullish); break;
    case Punctuator::PipePipe: logical(2, LogicalOp::Or); break;
    case Punctuator::AmpersandAmpersand: logical(3, LogicalOp::And); break;
    case Punctuator::Pipe: binary(4, BinaryOp::BitwiseOr); break;
    case Punctuator::Caret: binary(5, BinaryOp::BitwiseXor); break;
    case Punctuator::Ampersand: binary(6, BinaryOp::BitwiseAnd); break;
    case Punctuator::Equal: binary(7, BinaryOp::Equal); break;
    case Punctuator::NotEqual: binary(7, BinaryOp::NotEqual); break;
    case Punctuator::StrictEqual: binary(7, BinaryOp::StrictEqual); break;
    case Punctuator::StrictNotEqual: binary(7, BinaryOp::StrictNotEqual); break;
    case Punctuator::Less: binary(8, BinaryOp::Less); break;
    case Punctuator::LessEqual: binary(8, BinaryOp::LessEqual); break;
    case Punctuator::Greater: binary(8, BinaryOp::Greater); break;
    case Punctuator::GreaterEqual: binary(8, BinaryOp::GreaterEqual); break;
    case Punctuator::LeftShift: binary(9, BinaryOp::LeftShift); break;
    case Punctuator::RightShift: binary(9, BinaryOp::RightShift); break;
    case Punctuator::UnsignedRightShift: binary(9, BinaryOp::UnsignedRightShift); break;
    case Punctuator::Plus: binary(10, BinaryOp::Add); break;
    case Punctuator::Minus: binary(10, BinaryOp::Subtract); break;
    case Punctuator::Star: binary(11, BinaryOp::Multiply); break;
    case Punctuator::Slash: binary(11, BinaryOp::Divide); break;
    case Punctuator::Percent: binary(11, BinaryOp::Remainder); break;
    case Punctuator::StarStar: binary(12, BinaryOp::Exponent); break;
    default: break;
    }
    return result;
}

constexpr int exponent_precedence = 12;

std::optional<AssignmentOp> assignment_operator_for(Token const& token)
{
    if (token.type != TokenType::Punctuator)
        return std::nullopt;
    switch (token.punctuator) {
    case Punctuator::Assign: return AssignmentOp::Assign;
    case Punctuator::PlusAssign: return AssignmentOp::Add;
    case Punctuator::MinusAssign: return AssignmentOp::Subtract;
    case Punctuator::StarAssign: return AssignmentOp::Multiply;
    case Punctuator::SlashAssign: return AssignmentOp::Divide;
    case Punctuator::PercentAssign: return AssignmentOp::Remainder;
    case Punctuator::StarStarAssign: return AssignmentOp::Exponent;
    case Punctuator::LeftShiftAssign: return AssignmentOp::LeftShift;
    case Punctuator::RightShiftAssign: return AssignmentOp::RightShift;
    case Punctuator::UnsignedRightShiftAssign: return AssignmentOp::UnsignedRightShift;
    case Punctuator::AmpersandAssign: return AssignmentOp::BitwiseAnd;
    case Punctuator::PipeAssign: return AssignmentOp::BitwiseOr;
    case Punctuator::CaretAssign: return AssignmentOp::BitwiseXor;
    case Punctuator::AndAssign: return AssignmentOp::LogicalAnd;
    case Punctuator::OrAssign: return AssignmentOp::LogicalOr;
    case Punctuator::NullishAssign: return AssignmentOp::Nullish;
    default: return std::nullopt;
    }
}

std::optional<UnaryOp> unary_operator_for(Token const& token)
{
    if (token.type == TokenType::Keyword) {
        switch (token.keyword) {
        case Keyword::Typeof: return UnaryOp::Typeof;
        case Keyword::Void: return UnaryOp::Void;
        case Keyword::Delete: return UnaryOp::Delete;
        default: return std::nullopt;
        }
    }
    if (token.type != TokenType::Punctuator)
        return std::nullopt;
    switch (token.punctuator) {
    case Punctuator::Minus: return UnaryOp::Minus;
    case Punctuator::Plus: return UnaryOp::Plus;
    case Punctuator::Exclamation: return UnaryOp::Not;
    case Punctuator::Tilde: return UnaryOp::BitwiseNot;
    default: return std::nullopt;
    }
}

// A lexical scope while it is open: what it declares, and every `var`
// name declared inside it (own or nested), which is what a lexical name
// must not collide with (§14.2.1).
struct Scope {
    Declarations* declarations = nullptr; // null for a catch parameter scope
    std::unordered_set<JsString*> lexical_names;
    std::unordered_set<JsString*> function_names; // block-level function declarations, for B.3.2.4
    std::unordered_set<JsString*> var_names;
    int id = 0;
    bool is_function_top = false;
    bool is_catch_parameter = false; // B.3.4: a `var` may redeclare the parameter
    bool is_catch_body = false; // its lexicals must not redeclare the parameter (§14.15.1)
};

struct Label {
    JsString* name;
    bool is_iteration;
};

// A block-level function declaration in sloppy code becomes a `var` of the
// enclosing function (B.3.2.1) unless that would clash with a lexical
// declaration of a scope between them — including ones declared later —
// so the decision waits until the function body is finished.
struct AnnexBCandidate {
    JsString* name;
    std::vector<int> enclosing_scopes;
};

// The state of one function (or the program) being parsed.
struct FunctionContext {
    FunctionNode* node = nullptr; // null for the program
    Declarations* declarations = nullptr;
    std::vector<std::unique_ptr<Scope>> scopes; // scopes[0] is the function's top level
    std::unordered_map<int, std::unordered_set<JsString*>> retired_lexicals; // by scope id, for Annex B
    std::vector<Label> labels;
    std::unordered_set<JsString*> parameter_names;
    std::unordered_set<JsString*> var_set; // what declarations->vars holds
    std::vector<AnnexBCandidate> annex_b;
    int next_scope_id = 0;
    int iteration_depth = 0;
    int breakable_depth = 0;
    bool is_strict = false;
    bool is_arrow = false;
};

} // namespace

struct Parser::Impl {
    Impl(Heap& heap, std::u16string source, ParseOptions options);

    // Tokens.
    void advance();
    void relex(bool regex_allowed);
    void relex_as_division();
    struct Snapshot {
        Lexer::State lexer_state;
        Token current;
        Lexer::State current_start;
        bool current_regex_allowed;
        std::uint32_t previous_end;
    };
    Snapshot snapshot() const;
    void rewind(Snapshot const& snapshot);
    Token peek();
    bool expect(Punctuator);
    bool consume_semicolon();
    bool fail(SourcePosition, std::string message);
    bool fail_unexpected();
    bool fail_unsupported(std::string_view feature);
    bool enter();
    void leave() { --m_depth; }

    // Scopes and declarations.
    FunctionContext& function() { return *m_functions.back(); }
    FunctionContext const& function() const { return *m_functions.back(); }
    bool is_strict() const { return function().is_strict; }
    Scope& scope() { return *function().scopes.back(); }
    void push_function(FunctionNode*, Declarations*, bool is_arrow);
    bool pop_function();
    Scope& push_scope(Declarations*);
    void pop_scope();
    bool declare_var(JsString* name, SourcePosition);
    bool declare_lexical(JsString* name, bool is_const, SourcePosition);
    bool declare_function(FunctionDeclaration*, SourcePosition);
    std::unordered_set<JsString*> const* lexicals_of(int scope_id) const;
    void note_this();
    void note_arguments();
    void note_direct_eval();
    bool check_binding_identifier(Token const&, bool strict);
    bool check_simple_target(Expression const*, SourcePosition, std::string_view what);

    // Nodes.
    template<typename T>
    T* make(SourcePosition position)
    {
        T* node = m_program->make<T>();
        node->position = position;
        return node;
    }
    template<typename T>
    T* finish(T* node)
    {
        node->end_offset = m_previous_end;
        return node;
    }
    JsString* atom(std::u16string_view text) { return m_heap.atom(text); }

    // Expressions.
    Expression* parse_expression(bool allow_in);
    Expression* parse_assignment(bool allow_in);
    Expression* parse_conditional(bool allow_in);
    Expression* parse_binary(int min_precedence, bool allow_in);
    Expression* parse_unary();
    Expression* parse_left_hand_side();
    Expression* parse_new();
    Expression* parse_call_tail(Expression*, bool allow_call);
    bool parse_arguments(std::vector<Expression*>&);
    Expression* parse_primary();
    Expression* parse_identifier_reference();
    Expression* parse_parenthesised();
    Expression* parse_array_literal();
    Expression* parse_object_literal();
    bool parse_property_key(PropertyDefinition&, Token* key_token);
    Expression* parse_template();
    Expression* parse_function_expression();
    bool looks_like_arrow_parameters();
    bool balanced_parens_then_arrow();
    Expression* parse_arrow(bool allow_in);

    // Functions.
    enum class FunctionKind { Declaration, Expression, Method, Getter, Setter, Arrow };
    bool parse_function_rest(FunctionNode*, FunctionKind, std::optional<Token> name_token);
    bool parse_function_body(FunctionNode*);
    bool parse_directive_prologue(std::vector<Statement*>& body);
    bool finish_parameters(FunctionNode*, FunctionKind, std::vector<Token> const& parameter_tokens,
        std::optional<Token> const& name_token);

    // Statements.
    bool parse_program_body();
    Statement* parse_statement_list_item();
    Statement* parse_statement(bool is_body);
    Statement* parse_statement_inner(bool is_body);
    Statement* parse_iteration_body();
    bool parse_statement_list(std::vector<Statement*>&, bool until_case);
    BlockStatement* parse_block(bool is_catch_body);
    Statement* parse_variable_statement();
    VariableDeclaration* parse_declaration_list(VariableDeclaration::Kind, bool allow_in, bool in_for_head);
    Statement* parse_function_declaration();
    Statement* parse_if();
    Statement* parse_for();
    Statement* parse_while();
    Statement* parse_do_while();
    Statement* parse_continue();
    Statement* parse_break();
    Statement* parse_return();
    Statement* parse_with();
    Statement* parse_switch();
    Statement* parse_throw();
    Statement* parse_try();
    Statement* parse_labelled(bool is_body);
    Statement* parse_expression_statement();
    bool is_let_declaration_start();

    Heap& m_heap;
    std::unique_ptr<Program> m_program;
    Lexer m_lexer;
    ParseOptions m_options;
    Token m_current;
    Lexer::State m_current_start; // where m_current was lexed from, for relex()
    bool m_current_regex_allowed = false;
    std::uint32_t m_previous_end = 0;
    std::optional<ParseError> m_error;
    int m_depth = 0;
    std::vector<std::unique_ptr<FunctionContext>> m_functions;
    // Expressions that were wrapped in parentheses; the tree has no node
    // for that, and a few early errors depend on it.
    std::unordered_set<Expression const*> m_parenthesised;
};

Parser::Impl::Impl(Heap& heap, std::u16string source, ParseOptions options)
    : m_heap(heap)
    , m_program(std::make_unique<Program>())
    , m_lexer(std::u16string_view())
    , m_options(options)
{
    m_program->source = std::move(source);
    m_lexer = Lexer(m_program->source);
    m_current_start = m_lexer.save();
    m_current_regex_allowed = true;
    m_current = m_lexer.next(true);
}

// ---- tokens ---------------------------------------------------------------

void Parser::Impl::advance()
{
    m_previous_end = m_current.end_offset;
    bool const regex_allowed = !ends_expression(m_current);
    m_current_start = m_lexer.save();
    m_current_regex_allowed = regex_allowed;
    m_current = m_lexer.next(regex_allowed);
}

// Lexes the current token again with the other reading of `/`.
void Parser::Impl::relex(bool regex_allowed)
{
    m_lexer.restore(m_current_start);
    m_current_regex_allowed = regex_allowed;
    m_current = m_lexer.next(regex_allowed);
}

Parser::Impl::Snapshot Parser::Impl::snapshot() const
{
    return Snapshot { m_lexer.save(), m_current, m_current_start, m_current_regex_allowed, m_previous_end };
}

void Parser::Impl::rewind(Snapshot const& snapshot)
{
    m_lexer.restore(snapshot.lexer_state);
    m_current = snapshot.current;
    m_current_start = snapshot.current_start;
    m_current_regex_allowed = snapshot.current_regex_allowed;
    m_previous_end = snapshot.previous_end;
}

// The token after the current one, without consuming anything.
Token Parser::Impl::peek()
{
    Snapshot const saved = snapshot();
    advance();
    Token next = m_current;
    rewind(saved);
    return next;
}

bool Parser::Impl::fail(SourcePosition position, std::string message)
{
    if (!m_error)
        m_error = ParseError { position, std::move(message) };
    return false;
}

bool Parser::Impl::fail_unexpected()
{
    return fail(m_current.position, describe_token(m_current));
}

bool Parser::Impl::fail_unsupported(std::string_view feature)
{
    return fail(m_current.position, std::string(feature) + " are not supported yet");
}

bool Parser::Impl::expect(Punctuator p)
{
    if (!m_current.is(p))
        return fail_unexpected();
    advance();
    return true;
}

// Automatic semicolon insertion (§12.10.1): a `;` is taken; otherwise one
// is inserted before a `}`, at the end of the input, or when the
// offending token sits on a new line.
bool Parser::Impl::consume_semicolon()
{
    if (m_current.is(Punctuator::Semicolon)) {
        advance();
        return true;
    }
    if (m_current.is(Punctuator::RightBrace) || m_current.type == TokenType::EndOfInput || m_current.newline_before)
        return true;
    return fail_unexpected();
}

bool Parser::Impl::enter()
{
    if (++m_depth > Parser::max_nesting_depth) {
        --m_depth;
        return fail(m_current.position, "too much nesting");
    }
    return true;
}

// ---- scopes -----------------------------------------------------------------

void Parser::Impl::push_function(FunctionNode* node, Declarations* declarations, bool is_arrow)
{
    auto context = std::make_unique<FunctionContext>();
    context->node = node;
    context->declarations = declarations;
    context->is_arrow = is_arrow;
    context->is_strict = m_functions.empty() ? m_options.strict : function().is_strict;
    if (node)
        node->is_strict = context->is_strict;
    m_functions.push_back(std::move(context));
    Scope& top = push_scope(declarations);
    top.is_function_top = true;
}

Scope& Parser::Impl::push_scope(Declarations* declarations)
{
    FunctionContext& fn = function();
    auto scope_ptr = std::make_unique<Scope>();
    scope_ptr->declarations = declarations;
    scope_ptr->id = fn.next_scope_id++;
    fn.scopes.push_back(std::move(scope_ptr));
    return *fn.scopes.back();
}

void Parser::Impl::pop_scope()
{
    FunctionContext& fn = function();
    Scope& closing = *fn.scopes.back();
    fn.retired_lexicals[closing.id] = std::move(closing.lexical_names);
    fn.scopes.pop_back();
}

std::unordered_set<JsString*> const* Parser::Impl::lexicals_of(int scope_id) const
{
    FunctionContext const& fn = function();
    for (auto const& open : fn.scopes) {
        if (open->id == scope_id)
            return &open->lexical_names;
    }
    auto const retired = fn.retired_lexicals.find(scope_id);
    return retired == fn.retired_lexicals.end() ? nullptr : &retired->second;
}

// Closes the function: settles the Annex B block-function hoisting and
// drops the context.
bool Parser::Impl::pop_function()
{
    FunctionContext& fn = function();
    for (AnnexBCandidate const& candidate : fn.annex_b) {
        bool clashes = false;
        for (int const id : candidate.enclosing_scopes) {
            std::unordered_set<JsString*> const* names = lexicals_of(id);
            if (names && names->contains(candidate.name)) {
                clashes = true;
                break;
            }
        }
        if (clashes || fn.parameter_names.contains(candidate.name))
            continue;
        if (fn.var_set.insert(candidate.name).second)
            fn.declarations->vars.push_back(candidate.name);
    }
    m_functions.pop_back();
    return true;
}

// VarDeclaredNames: the name is checked against the lexical names of
// every open scope of this function (§14.2.1, §15.2.1, §16.1.1) and
// recorded on each so a later lexical declaration finds it.
bool Parser::Impl::declare_var(JsString* name, SourcePosition position)
{
    FunctionContext& fn = function();
    for (auto it = fn.scopes.rbegin(); it != fn.scopes.rend(); ++it) {
        Scope& s = **it;
        if (!s.is_catch_parameter && s.lexical_names.contains(name))
            return fail(position, "Identifier '" + utf8_from_utf16(name->view()) + "' has already been declared");
        s.var_names.insert(name);
    }
    if (fn.parameter_names.contains(name))
        return true; // parameters are not in the vars list (Ast.h)
    if (fn.var_set.insert(name).second)
        fn.declarations->vars.push_back(name);
    return true;
}

bool Parser::Impl::declare_lexical(JsString* name, bool is_const, SourcePosition position)
{
    FunctionContext& fn = function();
    Scope& s = scope();
    std::string const text = utf8_from_utf16(name->view());
    if (name->view() == u"let")
        return fail(position, "let is disallowed as a lexically bound name");
    if (s.lexical_names.contains(name) || s.var_names.contains(name))
        return fail(position, "Identifier '" + text + "' has already been declared");
    if (s.is_function_top && fn.parameter_names.contains(name))
        return fail(position, "Identifier '" + text + "' has already been declared");
    if (s.is_catch_body && fn.scopes.size() >= 2 && fn.scopes[fn.scopes.size() - 2]->lexical_names.contains(name))
        return fail(position, "Identifier '" + text + "' has already been declared");
    s.lexical_names.insert(name);
    if (s.declarations)
        s.declarations->lexicals.emplace_back(name, is_const);
    return true;
}

// A function declaration: var-like at a function's top level (§15.2.1),
// lexical in a block (§14.2.1), where sloppy code may also repeat it
// (B.3.2.4) and hoists it as a var (B.3.2.1).
bool Parser::Impl::declare_function(FunctionDeclaration* declaration, SourcePosition position)
{
    FunctionContext& fn = function();
    Scope& s = scope();
    JsString* name = declaration->function->name;
    std::string const text = utf8_from_utf16(name->view());
    if (s.is_function_top) {
        if (s.lexical_names.contains(name))
            return fail(position, "Identifier '" + text + "' has already been declared");
        s.var_names.insert(name);
        fn.declarations->functions.push_back(declaration);
        return true;
    }
    if (s.lexical_names.contains(name)) {
        bool const sloppy_duplicate = !fn.is_strict && s.function_names.contains(name);
        if (!sloppy_duplicate)
            return fail(position, "Identifier '" + text + "' has already been declared");
    }
    if (s.var_names.contains(name))
        return fail(position, "Identifier '" + text + "' has already been declared");
    if (s.is_catch_body && fn.scopes.size() >= 2 && fn.scopes[fn.scopes.size() - 2]->lexical_names.contains(name))
        return fail(position, "Identifier '" + text + "' has already been declared");
    s.lexical_names.insert(name);
    s.function_names.insert(name);
    if (s.declarations)
        s.declarations->functions.push_back(declaration);
    if (!fn.is_strict) {
        AnnexBCandidate candidate;
        candidate.name = name;
        for (std::size_t i = 0; i + 1 < fn.scopes.size(); ++i)
            candidate.enclosing_scopes.push_back(fn.scopes[i]->id);
        fn.annex_b.push_back(std::move(candidate));
    }
    return true;
}

// `this` and `arguments` belong to the nearest non-arrow function; the
// arrows between are marked too, since they must carry them through.
void Parser::Impl::note_this()
{
    for (auto it = m_functions.rbegin(); it != m_functions.rend(); ++it) {
        FunctionContext& fn = **it;
        if (!fn.node)
            return;
        fn.node->uses_this = true;
        if (!fn.is_arrow)
            return;
    }
}

void Parser::Impl::note_arguments()
{
    for (auto it = m_functions.rbegin(); it != m_functions.rend(); ++it) {
        FunctionContext& fn = **it;
        if (!fn.node)
            return;
        fn.node->uses_arguments = true;
        if (!fn.is_arrow)
            return;
    }
}

// A direct eval can reach every enclosing scope, so none of them may be
// optimised away.
void Parser::Impl::note_direct_eval()
{
    for (auto const& fn : m_functions) {
        if (fn->node)
            fn->node->has_direct_eval = true;
    }
}

// BindingIdentifier (§13.1.1): a reserved word spelled with escapes is
// never an identifier; in strict code the strict reserved words and
// eval/arguments cannot be bound.
bool Parser::Impl::check_binding_identifier(Token const& token, bool strict)
{
    if (token.type != TokenType::Identifier)
        return fail(token.position, describe_token(token));
    if (token.has_escape && Lexer::keyword_for(token.value))
        return fail(token.position, "Keyword must not contain escaped characters");
    if (strict) {
        if (is_strict_reserved_word(token.value))
            return fail(token.position, "Unexpected strict mode reserved word");
        if (is_eval_or_arguments(token.value))
            return fail(token.position, "Unexpected eval or arguments in strict mode");
    }
    return true;
}

// AssignmentTargetType must be simple (§13.15.1, §13.4.1): an identifier
// (not eval/arguments in strict code) or a property access that is not
// part of an optional chain.
bool Parser::Impl::check_simple_target(Expression const* target, SourcePosition position, std::string_view what)
{
    if (target->type == NodeType::Identifier) {
        if (is_strict() && is_eval_or_arguments(static_cast<Identifier const*>(target)->name->view()))
            return fail(position, "Unexpected eval or arguments in strict mode");
        return true;
    }
    if (target->type == NodeType::MemberExpression) {
        Expression const* link = target;
        while (link) {
            if (link->type == NodeType::MemberExpression) {
                auto const* member = static_cast<MemberExpression const*>(link);
                if (member->optional)
                    break;
                link = member->object;
            } else if (link->type == NodeType::CallExpression) {
                auto const* call = static_cast<CallExpression const*>(link);
                if (call->optional)
                    break;
                link = call->callee;
            } else {
                return true;
            }
        }
    }
    return fail(position, "Invalid left-hand side " + std::string(what));
}

// ---- expressions ------------------------------------------------------------

// Where an operator is wanted, a `/` the lexer read as a regular
// expression (or choked on as one) is division.
void Parser::Impl::relex_as_division()
{
    if (!m_current_regex_allowed)
        return;
    std::u16string const& source = m_program->source;
    if (m_current.position.offset < source.size() && source[m_current.position.offset] == u'/')
        relex(false);
}

// Expression (§13.16): a comma-separated sequence.
Expression* Parser::Impl::parse_expression(bool allow_in)
{
    SourcePosition const start = m_current.position;
    Expression* first = parse_assignment(allow_in);
    if (!first)
        return nullptr;
    if (!m_current.is(Punctuator::Comma))
        return first;
    auto* sequence = make<SequenceExpression>(start);
    sequence->expressions.push_back(first);
    while (m_current.is(Punctuator::Comma)) {
        advance();
        Expression* next = parse_assignment(allow_in);
        if (!next)
            return nullptr;
        sequence->expressions.push_back(next);
    }
    return finish(sequence);
}

// AssignmentExpression (§13.15), which is also where an arrow function
// starts (§15.3): `x =>` and `( … ) =>` are recognised by looking ahead.
Expression* Parser::Impl::parse_assignment(bool allow_in)
{
    if (!enter())
        return nullptr;
    Expression* result = nullptr;
    bool arrow = false;
    if (m_current.type == TokenType::Identifier && !m_current.has_escape) {
        if (m_current.value == u"async") {
            Snapshot const saved = snapshot();
            advance();
            bool const async_form = !m_current.newline_before
                && (m_current.is(Keyword::Function) || m_current.type == TokenType::Identifier
                    || (m_current.is(Punctuator::LeftParen) && balanced_parens_then_arrow()));
            rewind(saved);
            if (async_form) {
                leave();
                fail_unsupported("async functions");
                return nullptr;
            }
        }
        Token const next = peek();
        arrow = next.is(Punctuator::Arrow) && !next.newline_before;
    } else if (m_current.is(Punctuator::LeftParen)) {
        arrow = looks_like_arrow_parameters();
    }
    if (arrow) {
        result = parse_arrow(allow_in);
        leave();
        return result;
    }

    SourcePosition const start = m_current.position;
    Expression* left = parse_conditional(allow_in);
    if (!left) {
        leave();
        return nullptr;
    }
    relex_as_division();
    std::optional<AssignmentOp> const op = assignment_operator_for(m_current);
    if (!op) {
        leave();
        return left;
    }
    // An array or object literal as the target is a destructuring
    // pattern (§13.15.5), which this grammar leaves out: say so.
    if ((left->type == NodeType::ArrayLiteral || left->type == NodeType::ObjectLiteral) && !m_parenthesised.contains(left)) {
        leave();
        fail_unsupported("destructuring patterns");
        return nullptr;
    }
    if (!check_simple_target(left, start, "in assignment")) {
        leave();
        return nullptr;
    }
    advance();
    Expression* value = parse_assignment(allow_in);
    if (value) {
        auto* assignment = make<AssignmentExpression>(start);
        assignment->op = *op;
        assignment->target = left;
        assignment->value = value;
        result = finish(assignment);
    }
    leave();
    return result;
}

// ConditionalExpression (§13.14). The middle operand always admits `in`.
Expression* Parser::Impl::parse_conditional(bool allow_in)
{
    SourcePosition const start = m_current.position;
    Expression* test = parse_binary(1, allow_in);
    if (!test)
        return nullptr;
    if (!m_current.is(Punctuator::Question))
        return test;
    advance();
    Expression* consequent = parse_assignment(true);
    if (!consequent)
        return nullptr;
    if (!expect(Punctuator::Colon))
        return nullptr;
    Expression* alternate = parse_assignment(allow_in);
    if (!alternate)
        return nullptr;
    auto* conditional = make<ConditionalExpression>(start);
    conditional->test = test;
    conditional->consequent = consequent;
    conditional->alternate = alternate;
    return finish(conditional);
}

// The binary levels (§13.6–§13.13) by precedence climbing. `**` is
// right-associative and its left operand may not be a bare unary
// expression (§13.6: UpdateExpression ** ExponentiationExpression).
// `??` may not mix with `||` or `&&` without parentheses (§13.13:
// CoalesceExpression's operands are BitwiseORExpressions).
Expression* Parser::Impl::parse_binary(int min_precedence, bool allow_in)
{
    SourcePosition const start = m_current.position;
    Expression* left = parse_unary();
    if (!left)
        return nullptr;
    while (true) {
        relex_as_division();
        BinaryOperator const op = binary_operator_for(m_current, allow_in);
        if (op.precedence == 0 || op.precedence < min_precedence)
            break;
        SourcePosition const operator_position = m_current.position;
        if (op.precedence == exponent_precedence && left->type == NodeType::UnaryExpression
            && !m_parenthesised.contains(left)) {
            fail(operator_position, "Unary operator used immediately before exponentiation expression. Parenthesis must be used to disambiguate operator precedence");
            return nullptr;
        }
        advance();
        int const next_precedence = op.precedence == exponent_precedence ? op.precedence : op.precedence + 1;
        // The right operand recurses (deeply, for a right-associative
        // chain), so it counts as a nesting level.
        if (!enter())
            return nullptr;
        Expression* right = parse_binary(next_precedence, allow_in);
        leave();
        if (!right)
            return nullptr;
        if (op.logical) {
            auto mixes = [&](Expression const* operand) {
                if (operand->type != NodeType::LogicalExpression || m_parenthesised.contains(operand))
                    return false;
                bool const nullish = static_cast<LogicalExpression const*>(operand)->op == LogicalOp::Nullish;
                return nullish != (op.logical_op == LogicalOp::Nullish);
            };
            if (mixes(left) || mixes(right)) {
                fail(operator_position, "Mixing '?" "?' with '||' or '&&' without parentheses is not allowed");
                return nullptr;
            }
            auto* logical = make<LogicalExpression>(start);
            logical->op = op.logical_op;
            logical->left = left;
            logical->right = right;
            left = finish(logical);
        } else {
            auto* binary = make<BinaryExpression>(start);
            binary->op = op.op;
            binary->left = left;
            binary->right = right;
            left = finish(binary);
        }
    }
    return left;
}

// UnaryExpression and UpdateExpression (§13.4, §13.5). A postfix ++/--
// on a new line belongs to the next statement (§12.10.1 restricted
// production).
Expression* Parser::Impl::parse_unary()
{
    SourcePosition const start = m_current.position;
    if (std::optional<UnaryOp> const op = unary_operator_for(m_current)) {
        if (!enter())
            return nullptr;
        advance();
        Expression* operand = parse_unary();
        leave();
        if (!operand)
            return nullptr;
        // §13.5.1.1: in strict code `delete x` is an early error, and so is
        // `delete (x)` — the rule looks through the parentheses.
        if (*op == UnaryOp::Delete && is_strict() && operand->type == NodeType::Identifier) {
            fail(start, "Delete of an unqualified identifier in strict mode");
            return nullptr;
        }
        auto* unary = make<UnaryExpression>(start);
        unary->op = *op;
        unary->operand = operand;
        return finish(unary);
    }
    if (m_current.is(Punctuator::PlusPlus) || m_current.is(Punctuator::MinusMinus)) {
        bool const increment = m_current.is(Punctuator::PlusPlus);
        if (!enter())
            return nullptr;
        advance();
        Expression* operand = parse_unary();
        leave();
        if (!operand)
            return nullptr;
        if (!check_simple_target(operand, start, "expression in prefix operation"))
            return nullptr;
        auto* update = make<UpdateExpression>(start);
        update->increment = increment;
        update->prefix = true;
        update->target = operand;
        return finish(update);
    }
    // `await x` and `yield x` are not expressions of this grammar; name
    // the feature rather than tripping over the operand.
    if (m_current.type == TokenType::Identifier && !m_current.has_escape
        && (m_current.value == u"await" || m_current.value == u"yield")) {
        bool const is_await = m_current.value == u"await";
        Token const next = peek();
        bool const operand_follows = !next.newline_before
            && (next.type == TokenType::Identifier || next.type == TokenType::Number || next.type == TokenType::String
                || next.type == TokenType::Template || next.is(Punctuator::LeftBracket) || next.is(Punctuator::LeftBrace)
                || next.is(Keyword::This) || next.is(Keyword::New) || next.is(Keyword::Function) || next.is(Keyword::Null)
                || next.is(Keyword::True) || next.is(Keyword::False) || next.is(Keyword::Typeof));
        if (operand_follows) {
            fail_unsupported(is_await ? "async functions" : "generators");
            return nullptr;
        }
    }
    Expression* expression = parse_left_hand_side();
    if (!expression)
        return nullptr;
    if ((m_current.is(Punctuator::PlusPlus) || m_current.is(Punctuator::MinusMinus)) && !m_current.newline_before) {
        if (!check_simple_target(expression, start, "expression in postfix operation"))
            return nullptr;
        auto* update = make<UpdateExpression>(start);
        update->increment = m_current.is(Punctuator::PlusPlus);
        update->prefix = false;
        update->target = expression;
        advance();
        return finish(update);
    }
    return expression;
}

Expression* Parser::Impl::parse_left_hand_side()
{
    Expression* base = m_current.is(Keyword::New) ? parse_new() : parse_primary();
    if (!base)
        return nullptr;
    return parse_call_tail(base, true);
}

// NewExpression / MemberExpression: new … (§13.3.5). `new a.b.c()` takes
// the member chain as the constructor and the arguments that follow;
// `new new X()()` nests; `new X` without arguments is allowed; a call in
// the chain stops it (`new a()()` calls the result).
Expression* Parser::Impl::parse_new()
{
    SourcePosition const start = m_current.position;
    if (!enter())
        return nullptr;
    advance(); // new
    if (m_current.is(Punctuator::Dot)) {
        leave();
        fail_unsupported("new.target");
        return nullptr;
    }
    Expression* callee = m_current.is(Keyword::New) ? parse_new() : parse_primary();
    if (callee)
        callee = parse_call_tail(callee, false);
    leave();
    if (!callee)
        return nullptr;
    auto* expression = make<NewExpression>(start);
    expression->callee = callee;
    if (m_current.is(Punctuator::LeftParen) && !parse_arguments(expression->arguments))
        return nullptr;
    return finish(expression);
}

// The accessors and calls after a primary (§13.3): `.name`, `[expr]`,
// `(args)`, and the optional forms behind `?.`.
Expression* Parser::Impl::parse_call_tail(Expression* expression, bool allow_call)
{
    SourcePosition const start = expression->position;
    while (true) {
        if (m_current.is(Punctuator::Dot)) {
            advance();
            if (m_current.type != TokenType::Identifier && m_current.type != TokenType::Keyword) {
                fail_unexpected();
                return nullptr;
            }
            auto* member = make<MemberExpression>(start);
            member->object = expression;
            member->name = atom(m_current.value);
            advance();
            expression = finish(member);
        } else if (m_current.is(Punctuator::QuestionDot)) {
            if (!allow_call) {
                fail(m_current.position, "Invalid optional chain from new expression");
                return nullptr;
            }
            advance();
            if (m_current.is(Punctuator::LeftParen)) {
                auto* call = make<CallExpression>(start);
                call->callee = expression;
                call->optional = true;
                if (!parse_arguments(call->arguments))
                    return nullptr;
                expression = finish(call);
            } else if (m_current.is(Punctuator::LeftBracket)) {
                advance();
                if (!enter())
                    return nullptr;
                Expression* property = parse_expression(true);
                leave();
                if (!property || !expect(Punctuator::RightBracket))
                    return nullptr;
                auto* member = make<MemberExpression>(start);
                member->object = expression;
                member->property = property;
                member->optional = true;
                expression = finish(member);
            } else if (m_current.type == TokenType::Identifier || m_current.type == TokenType::Keyword) {
                auto* member = make<MemberExpression>(start);
                member->object = expression;
                member->name = atom(m_current.value);
                member->optional = true;
                advance();
                expression = finish(member);
            } else if (m_current.type == TokenType::Template) {
                fail_unsupported("tagged templates");
                return nullptr;
            } else {
                fail_unexpected();
                return nullptr;
            }
        } else if (m_current.is(Punctuator::LeftBracket)) {
            advance();
            if (!enter())
                return nullptr;
            Expression* property = parse_expression(true);
            leave();
            if (!property || !expect(Punctuator::RightBracket))
                return nullptr;
            auto* member = make<MemberExpression>(start);
            member->object = expression;
            member->property = property;
            expression = finish(member);
        } else if (m_current.is(Punctuator::LeftParen)) {
            if (!allow_call)
                break;
            auto* call = make<CallExpression>(start);
            call->callee = expression;
            // §13.3.6.1: a call whose callee evaluates to the reference
            // named `eval` is a direct eval — parentheses do not change
            // that, an optional call does.
            if (expression->type == NodeType::Identifier
                && static_cast<Identifier const*>(expression)->name == m_heap.atoms().eval) {
                call->is_direct_eval = true;
                note_direct_eval();
            }
            if (!parse_arguments(call->arguments))
                return nullptr;
            expression = finish(call);
        } else if (m_current.type == TokenType::Template) {
            fail_unsupported("tagged templates");
            return nullptr;
        } else {
            break;
        }
    }
    return expression;
}

bool Parser::Impl::parse_arguments(std::vector<Expression*>& arguments)
{
    if (!enter())
        return false;
    advance(); // (
    while (!m_current.is(Punctuator::RightParen)) {
        if (m_current.is(Punctuator::Ellipsis)) {
            leave();
            return fail_unsupported("spread syntax");
        }
        Expression* argument = parse_assignment(true);
        if (!argument) {
            leave();
            return false;
        }
        arguments.push_back(argument);
        if (m_current.is(Punctuator::Comma)) {
            advance(); // a trailing comma is allowed (§13.3.8)
            continue;
        }
        if (!m_current.is(Punctuator::RightParen)) {
            leave();
            return fail_unexpected();
        }
    }
    advance();
    leave();
    return true;
}

// PrimaryExpression (§13.2).
Expression* Parser::Impl::parse_primary()
{
    // A `/` here starts a regular expression whatever the lexer guessed.
    if (!m_current_regex_allowed && (m_current.is(Punctuator::Slash) || m_current.is(Punctuator::SlashAssign)))
        relex(true);
    SourcePosition const start = m_current.position;
    switch (m_current.type) {
    case TokenType::Identifier:
        return parse_identifier_reference();
    case TokenType::Keyword:
        switch (m_current.keyword) {
        case Keyword::This: {
            note_this();
            advance();
            return finish(make<ThisExpression>(start));
        }
        case Keyword::Null: {
            advance();
            return finish(make<NullLiteral>(start));
        }
        case Keyword::True:
        case Keyword::False: {
            auto* literal = make<BooleanLiteral>(start);
            literal->value = m_current.keyword == Keyword::True;
            advance();
            return finish(literal);
        }
        case Keyword::Function:
            return parse_function_expression();
        case Keyword::Class:
            fail_unsupported("class expressions");
            return nullptr;
        case Keyword::Super:
            fail_unsupported("super references");
            return nullptr;
        case Keyword::Import:
            fail_unsupported("modules");
            return nullptr;
        default:
            fail_unexpected();
            return nullptr;
        }
    case TokenType::Number: {
        if (m_current.legacy_octal && is_strict()) {
            fail(start, "Octal literals are not allowed in strict mode");
            return nullptr;
        }
        auto* literal = make<NumberLiteral>(start);
        literal->value = m_current.number;
        advance();
        return finish(literal);
    }
    case TokenType::String: {
        if (m_current.legacy_octal && is_strict()) {
            fail(start, "Octal escape sequences are not allowed in strict mode");
            return nullptr;
        }
        auto* literal = make<StringLiteral>(start);
        literal->value = atom(m_current.value);
        advance();
        return finish(literal);
    }
    case TokenType::Template:
        return parse_template();
    case TokenType::RegExp: {
        // §13.2.7.1: the flags must be a valid combination and the pattern
        // must parse — both are early errors, so the pattern is compiled
        // here once for its verdict and again, for its program, when the
        // literal is evaluated.
        std::optional<RegexFlags> const flags = RegexFlags::parse(m_current.raw);
        if (!flags) {
            fail(start, "Invalid regular expression flags");
            return nullptr;
        }
        Regex::CompileError compile_error;
        if (!Regex::compile(m_current.value, *flags, &compile_error)) {
            fail(start, "Invalid regular expression: /" + utf8_from_utf16(m_current.value) + "/" + utf8_from_utf16(m_current.raw) + ": " + compile_error.message);
            return nullptr;
        }
        auto* literal = make<RegExpLiteral>(start);
        literal->pattern = atom(m_current.value);
        literal->flags = atom(m_current.raw);
        advance();
        return finish(literal);
    }
    case TokenType::Punctuator:
        if (m_current.is(Punctuator::LeftParen))
            return parse_parenthesised();
        if (m_current.is(Punctuator::LeftBracket))
            return parse_array_literal();
        if (m_current.is(Punctuator::LeftBrace))
            return parse_object_literal();
        if (m_current.is(Punctuator::Ellipsis)) {
            fail_unsupported("spread syntax");
            return nullptr;
        }
        fail_unexpected();
        return nullptr;
    case TokenType::Invalid:
    case TokenType::EndOfInput:
        break;
    }
    fail_unexpected();
    return nullptr;
}

// IdentifierReference (§13.1.1).
Expression* Parser::Impl::parse_identifier_reference()
{
    SourcePosition const start = m_current.position;
    if (m_current.has_escape && Lexer::keyword_for(m_current.value)) {
        fail(start, "Keyword must not contain escaped characters");
        return nullptr;
    }
    if (is_strict() && is_strict_reserved_word(m_current.value)) {
        fail(start, "Unexpected strict mode reserved word");
        return nullptr;
    }
    auto* identifier = make<Identifier>(start);
    identifier->name = atom(m_current.value);
    if (identifier->name == m_heap.atoms().arguments)
        note_arguments();
    advance();
    return finish(identifier);
}

// ( Expression ) — the tree keeps no node for the parentheses, only the
// membership in m_parenthesised that a few early errors consult.
Expression* Parser::Impl::parse_parenthesised()
{
    if (!enter())
        return nullptr;
    SourcePosition const open = m_current.position;
    advance(); // (
    if (m_current.is(Punctuator::RightParen)) {
        leave();
        fail_unexpected();
        return nullptr;
    }
    // The list is parsed by hand rather than as an Expression so that a
    // trailing comma, legal only in an arrow head, is told apart.
    std::vector<Expression*> items;
    bool trailing_comma = false;
    while (true) {
        Expression* item = parse_assignment(true);
        if (!item) {
            leave();
            return nullptr;
        }
        items.push_back(item);
        if (!m_current.is(Punctuator::Comma))
            break;
        advance();
        if (m_current.is(Punctuator::RightParen)) {
            trailing_comma = true;
            break;
        }
    }
    leave();
    SourcePosition const close = m_current.position;
    if (!expect(Punctuator::RightParen))
        return nullptr;
    Expression* inner = items[0];
    if (items.size() > 1) {
        auto* sequence = make<SequenceExpression>(items[0]->position);
        sequence->end_offset = items.back()->end_offset;
        sequence->expressions = std::move(items);
        inner = sequence;
    }
    bool const arrow_follows = m_current.is(Punctuator::Arrow) && !m_current.newline_before;
    if (trailing_comma && !arrow_follows) {
        fail(close, "Unexpected token ')'");
        return nullptr;
    }
    m_parenthesised.insert(inner);
    inner->parenthesized = true;
    // The expression's span takes in its parentheses, so a message that
    // quotes it quotes what was written.
    inner->position = open;
    inner->end_offset = m_previous_end;
    // An arrow head the lookahead rejected: say which unsupported form
    // it was rather than "unexpected =>".
    if (arrow_follows) {
        bool defaults = false;
        bool patterns = false;
        auto classify = [&](Expression const* e) {
            if (e->type == NodeType::AssignmentExpression)
                defaults = true;
            if (e->type == NodeType::ArrayLiteral || e->type == NodeType::ObjectLiteral)
                patterns = true;
        };
        if (inner->type == NodeType::SequenceExpression) {
            for (Expression const* e : static_cast<SequenceExpression const*>(inner)->expressions)
                classify(e);
        } else {
            classify(inner);
        }
        if (patterns)
            fail_unsupported("destructuring patterns");
        else if (defaults)
            fail_unsupported("default parameters");
        else
            fail_unexpected();
        return nullptr;
    }
    return inner;
}

// ArrayLiteral (§13.2.4): an elision is a hole; a trailing comma is not.
Expression* Parser::Impl::parse_array_literal()
{
    SourcePosition const start = m_current.position;
    if (!enter())
        return nullptr;
    advance(); // [
    auto* array = make<ArrayLiteral>(start);
    while (!m_current.is(Punctuator::RightBracket)) {
        if (m_current.is(Punctuator::Comma)) {
            advance();
            array->elements.push_back(nullptr);
            continue;
        }
        if (m_current.is(Punctuator::Ellipsis)) {
            leave();
            fail_unsupported("spread syntax");
            return nullptr;
        }
        Expression* element = parse_assignment(true);
        if (!element) {
            leave();
            return nullptr;
        }
        array->elements.push_back(element);
        if (m_current.is(Punctuator::Comma)) {
            advance();
            continue;
        }
        if (!m_current.is(Punctuator::RightBracket)) {
            leave();
            fail_unexpected();
            return nullptr;
        }
    }
    advance();
    leave();
    return finish(array);
}

// PropertyName (§13.2.5): an IdentifierName, a string, a number (stored
// as its canonical string, §13.2.5.4) or a computed [expr].
bool Parser::Impl::parse_property_key(PropertyDefinition& property, Token* key_token)
{
    *key_token = m_current;
    switch (m_current.type) {
    case TokenType::Identifier:
    case TokenType::Keyword:
        property.key = atom(m_current.value);
        advance();
        return true;
    case TokenType::String:
        if (m_current.legacy_octal && is_strict())
            return fail(m_current.position, "Octal escape sequences are not allowed in strict mode");
        property.key = atom(m_current.value);
        advance();
        return true;
    case TokenType::Number:
        if (m_current.legacy_octal && is_strict())
            return fail(m_current.position, "Octal literals are not allowed in strict mode");
        property.key = atom(number_to_string(m_current.number));
        advance();
        return true;
    case TokenType::Punctuator:
        if (m_current.is(Punctuator::LeftBracket)) {
            advance();
            property.computed_key = parse_assignment(true);
            if (!property.computed_key)
                return false;
            return expect(Punctuator::RightBracket);
        }
        break;
    default:
        break;
    }
    return fail_unexpected();
}

// ObjectLiteral (§13.2.5): `key: value`, shorthand `key`, method
// shorthand `key() {}`, `get key() {}` / `set key(v) {}`, computed keys,
// and `__proto__: value` (at most once, §13.2.5.1).
Expression* Parser::Impl::parse_object_literal()
{
    SourcePosition const start = m_current.position;
    if (!enter())
        return nullptr;
    advance(); // {
    auto* object = make<ObjectLiteral>(start);
    bool has_proto = false;
    while (!m_current.is(Punctuator::RightBrace)) {
        PropertyDefinition property;
        SourcePosition const property_start = m_current.position;
        if (m_current.is(Punctuator::Ellipsis)) {
            leave();
            fail_unsupported("spread syntax");
            return nullptr;
        }
        if (m_current.is(Punctuator::Star)) {
            leave();
            fail_unsupported("generators");
            return nullptr;
        }
        // `get`/`set`/`async` are accessor prefixes only when a key follows.
        bool accessor = false;
        if (m_current.type == TokenType::Identifier && !m_current.has_escape
            && (m_current.value == u"get" || m_current.value == u"set" || m_current.value == u"async")) {
            Token const next = peek();
            bool const key_follows = next.type == TokenType::Identifier || next.type == TokenType::Keyword
                || next.type == TokenType::String || next.type == TokenType::Number
                || next.is(Punctuator::LeftBracket) || next.is(Punctuator::Star);
            if (key_follows) {
                if (m_current.value == u"async") {
                    leave();
                    fail_unsupported("async functions");
                    return nullptr;
                }
                accessor = true;
                property.kind = m_current.value == u"get" ? PropertyDefinition::Kind::Get : PropertyDefinition::Kind::Set;
                advance();
            }
        }
        Token key_token;
        if (!parse_property_key(property, &key_token)) {
            leave();
            return nullptr;
        }
        if (accessor || m_current.is(Punctuator::LeftParen)) {
            if (!m_current.is(Punctuator::LeftParen)) {
                leave();
                fail_unexpected();
                return nullptr;
            }
            FunctionNode* fn = m_program->make_function();
            fn->position = property_start;
            fn->source_start = property_start.offset;
            fn->name = property.computed_key ? nullptr : property.key;
            fn->is_constructable = false; // §15.4: methods and accessors have no [[Construct]]
            fn->is_getter = property.kind == PropertyDefinition::Kind::Get;
            fn->is_setter = property.kind == PropertyDefinition::Kind::Set;
            FunctionKind const kind = fn->is_getter ? FunctionKind::Getter : fn->is_setter ? FunctionKind::Setter : FunctionKind::Method;
            if (!parse_function_rest(fn, kind, std::nullopt)) {
                leave();
                return nullptr;
            }
            auto* expression = make<FunctionExpression>(property_start);
            expression->function = fn;
            property.value = finish(expression);
        } else if (m_current.is(Punctuator::Colon)) {
            advance();
            Expression* value = parse_assignment(true);
            if (!value) {
                leave();
                return nullptr;
            }
            property.value = value;
            bool const proto_key = !property.computed_key && property.key == m_heap.atoms().proto
                && (key_token.type == TokenType::Identifier || key_token.type == TokenType::String);
            if (proto_key) {
                if (has_proto) {
                    leave();
                    fail(property_start, "Duplicate __proto__ fields are not allowed in object literals");
                    return nullptr;
                }
                has_proto = true;
                property.is_proto = true;
            } else if (value->type == NodeType::ArrowFunction
                || (value->type == NodeType::FunctionExpression && !static_cast<FunctionExpression const*>(value)->function->name)) {
                property.is_anonymous_function = true; // §13.2.5.5: the value takes the key as its name
            }
        } else if (key_token.type == TokenType::Identifier && !property.computed_key) {
            // Shorthand `{ x }` is the reference `x`; `{ x = 1 }` is only
            // valid as a pattern, which is not supported.
            if (m_current.is(Punctuator::Assign)) {
                leave();
                fail_unsupported("destructuring patterns");
                return nullptr;
            }
            if (key_token.has_escape && Lexer::keyword_for(key_token.value)) {
                leave();
                fail(key_token.position, "Keyword must not contain escaped characters");
                return nullptr;
            }
            if (is_strict() && is_strict_reserved_word(key_token.value)) {
                leave();
                fail(key_token.position, "Unexpected strict mode reserved word");
                return nullptr;
            }
            auto* identifier = make<Identifier>(key_token.position);
            identifier->name = property.key;
            identifier->end_offset = key_token.end_offset;
            if (identifier->name == m_heap.atoms().arguments)
                note_arguments();
            property.value = identifier;
        } else {
            leave();
            fail_unexpected();
            return nullptr;
        }
        object->properties.push_back(property);
        if (m_current.is(Punctuator::Comma)) {
            advance();
            continue;
        }
        if (!m_current.is(Punctuator::RightBrace)) {
            leave();
            fail_unexpected();
            return nullptr;
        }
    }
    advance();
    leave();
    return finish(object);
}

// TemplateLiteral (§13.2.8), untagged. After the `}` that closes a
// substitution the lexer is asked for the next span directly, since a
// `}` read as a punctuator would be followed by ordinary tokens.
Expression* Parser::Impl::parse_template()
{
    SourcePosition const start = m_current.position;
    if (!enter())
        return nullptr;
    auto* literal = make<TemplateLiteral>(start);
    while (true) {
        if (m_current.type != TokenType::Template) {
            leave();
            fail_unexpected();
            return nullptr;
        }
        // §12.9.6.1: an untagged template with an invalid escape has no
        // cooked value and is an early error.
        if (!m_current.cooked_valid) {
            leave();
            fail(m_current.position, "Invalid escape sequence in template literal");
            return nullptr;
        }
        literal->cooked.push_back(atom(m_current.value));
        literal->raw.push_back(atom(m_current.raw));
        if (m_current.template_tail) {
            advance();
            break;
        }
        advance();
        Expression* expression = parse_expression(true);
        if (!expression) {
            leave();
            return nullptr;
        }
        literal->expressions.push_back(expression);
        if (!m_current.is(Punctuator::RightBrace)) {
            leave();
            fail_unexpected();
            return nullptr;
        }
        m_previous_end = m_current.end_offset;
        m_current_start = m_lexer.save();
        m_current_regex_allowed = false;
        m_current = m_lexer.next_template_continuation();
    }
    leave();
    return finish(literal);
}

Expression* Parser::Impl::parse_function_expression()
{
    SourcePosition const start = m_current.position;
    advance(); // function
    if (m_current.is(Punctuator::Star)) {
        fail_unsupported("generators");
        return nullptr;
    }
    std::optional<Token> name_token;
    if (m_current.type == TokenType::Identifier) {
        name_token = m_current;
        advance();
    }
    FunctionNode* fn = m_program->make_function();
    fn->position = start;
    fn->source_start = start.offset;
    if (name_token)
        fn->name = atom(name_token->value);
    if (!parse_function_rest(fn, FunctionKind::Expression, name_token))
        return nullptr;
    auto* expression = make<FunctionExpression>(start);
    expression->function = fn;
    return finish(expression);
}

// Is the `(` at the current token the head of an arrow function? Scans
// `( [Identifier (, Identifier)*] ) =>` on a saved lexer state and puts
// everything back (§15.3: ArrowParameters, with the =>
// [no LineTerminator here] restriction).
bool Parser::Impl::looks_like_arrow_parameters()
{
    Snapshot const saved = snapshot();
    advance(); // (
    bool matches = true;
    while (!m_current.is(Punctuator::RightParen)) {
        if (m_current.type != TokenType::Identifier) {
            matches = false;
            break;
        }
        advance();
        if (m_current.is(Punctuator::Comma)) {
            advance();
            continue;
        }
        if (!m_current.is(Punctuator::RightParen))
            matches = false;
        break;
    }
    if (matches) {
        advance(); // )
        matches = m_current.is(Punctuator::Arrow) && !m_current.newline_before;
    }
    rewind(saved);
    return matches;
}

// After `async`: does the `(` at the current token open a balanced group
// followed by `=>` on the same line? Only used to name async arrows,
// which are not supported, so an inexact scan (a template inside the
// group can mislead it) costs nothing but a less specific message.
bool Parser::Impl::balanced_parens_then_arrow()
{
    Snapshot const saved = snapshot();
    int depth = 0;
    bool matches = false;
    do {
        if (m_current.is(Punctuator::LeftParen))
            ++depth;
        else if (m_current.is(Punctuator::RightParen))
            --depth;
        else if (m_current.type == TokenType::EndOfInput || m_current.type == TokenType::Invalid)
            break;
        advance();
    } while (depth > 0);
    if (depth == 0)
        matches = m_current.is(Punctuator::Arrow) && !m_current.newline_before;
    rewind(saved);
    return matches;
}

// ArrowFunction (§15.3), the lookahead having confirmed the shape.
Expression* Parser::Impl::parse_arrow(bool allow_in)
{
    SourcePosition const start = m_current.position;
    FunctionNode* fn = m_program->make_function();
    fn->position = start;
    fn->source_start = start.offset;
    fn->is_arrow = true;
    fn->is_constructable = false;
    std::vector<Token> parameter_tokens;
    if (m_current.type == TokenType::Identifier) {
        parameter_tokens.push_back(m_current);
        advance();
    } else {
        advance(); // (
        while (!m_current.is(Punctuator::RightParen)) {
            parameter_tokens.push_back(m_current);
            advance();
            if (m_current.is(Punctuator::Comma))
                advance();
        }
        advance(); // )
    }
    advance(); // =>
    if (!enter())
        return nullptr;
    push_function(fn, &fn->declarations, true);
    FunctionContext& context = function();
    for (Token const& token : parameter_tokens) {
        JsString* name = atom(token.value);
        fn->parameters.push_back(name);
        if (!context.parameter_names.insert(name).second)
            fn->has_duplicate_parameters = true;
    }
    if (m_current.is(Punctuator::LeftBrace)) {
        if (!parse_function_body(fn))
            return nullptr;
    } else {
        Expression* body = parse_assignment(allow_in);
        if (!body)
            return nullptr;
        fn->expression_body = body;
        fn->source_end = m_previous_end;
    }
    if (!finish_parameters(fn, FunctionKind::Arrow, parameter_tokens, std::nullopt))
        return nullptr;
    pop_function();
    leave();
    auto* expression = make<ArrowFunction>(start);
    expression->function = fn;
    return finish(expression);
}

// ---- functions --------------------------------------------------------------

// From the `(` of the parameter list to the `}` of the body. The
// parameter names are recorded before the body so that its declarations
// can be checked against them; their own early errors wait until the
// body's directive prologue has settled the function's strictness
// (§15.2.1: "use strict" applies to the parameters too).
bool Parser::Impl::parse_function_rest(FunctionNode* fn, FunctionKind kind, std::optional<Token> name_token)
{
    if (!enter())
        return false;
    push_function(fn, &fn->declarations, false);
    if (!expect(Punctuator::LeftParen))
        return false;
    std::vector<Token> parameter_tokens;
    while (!m_current.is(Punctuator::RightParen)) {
        if (m_current.is(Punctuator::Ellipsis))
            return fail_unsupported("rest parameters");
        if (m_current.is(Punctuator::LeftBracket) || m_current.is(Punctuator::LeftBrace))
            return fail_unsupported("destructuring patterns");
        if (m_current.type != TokenType::Identifier)
            return fail_unexpected();
        parameter_tokens.push_back(m_current);
        advance();
        if (m_current.is(Punctuator::Assign))
            return fail_unsupported("default parameters");
        if (m_current.is(Punctuator::Comma)) {
            advance();
            continue;
        }
        if (!m_current.is(Punctuator::RightParen))
            return fail_unexpected();
    }
    advance(); // )
    FunctionContext& context = function();
    for (Token const& token : parameter_tokens) {
        JsString* name = atom(token.value);
        fn->parameters.push_back(name);
        if (!context.parameter_names.insert(name).second)
            fn->has_duplicate_parameters = true;
    }
    if (!parse_function_body(fn))
        return false;
    if (!finish_parameters(fn, kind, parameter_tokens, name_token))
        return false;
    leave();
    return pop_function();
}

bool Parser::Impl::parse_function_body(FunctionNode* fn)
{
    if (!expect(Punctuator::LeftBrace))
        return false;
    if (!parse_directive_prologue(fn->body))
        return false;
    if (!parse_statement_list(fn->body, false))
        return false;
    if (!m_current.is(Punctuator::RightBrace))
        return fail_unexpected();
    advance();
    fn->source_end = m_previous_end;
    return true;
}

// The Directive Prologue (§11.2.1): the leading ExpressionStatements
// that are nothing but a string literal. The exact "use strict" (no
// escapes) makes the scope strict; an earlier directive with a legacy
// octal escape is then an error after the fact.
bool Parser::Impl::parse_directive_prologue(std::vector<Statement*>& body)
{
    std::vector<SourcePosition> octal_positions;
    FunctionContext& fn = function();
    while (m_current.type == TokenType::String) {
        Token const directive = m_current;
        Statement* statement = parse_statement(false);
        if (!statement)
            return false;
        body.push_back(statement);
        if (statement->type != NodeType::ExpressionStatement)
            break;
        Expression const* expression = static_cast<ExpressionStatement const*>(statement)->expression;
        if (expression->type != NodeType::StringLiteral || m_parenthesised.contains(expression)
            || expression->end_offset != directive.end_offset)
            break;
        if (directive.value == use_strict_directive && !directive.has_escape) {
            fn.is_strict = true;
            if (fn.node)
                fn.node->is_strict = true;
            else
                m_program->is_strict = true;
        }
        if (directive.legacy_octal)
            octal_positions.push_back(directive.position);
    }
    if (fn.is_strict && !octal_positions.empty())
        return fail(octal_positions.front(), "Octal escape sequences are not allowed in strict mode");
    return true;
}

// The early errors on a function's name and parameters (§15.2.1, §15.3.1,
// §15.4.1) with the function's final strictness.
bool Parser::Impl::finish_parameters(FunctionNode* fn, FunctionKind kind, std::vector<Token> const& parameter_tokens,
    std::optional<Token> const& name_token)
{
    bool const strict = fn->is_strict;
    for (Token const& token : parameter_tokens) {
        if (!check_binding_identifier(token, strict))
            return false;
    }
    // Duplicates are allowed only in a sloppy plain function (§15.2.1);
    // arrows, methods and accessors use UniqueFormalParameters.
    bool const plain = kind == FunctionKind::Declaration || kind == FunctionKind::Expression;
    if (fn->has_duplicate_parameters && (strict || !plain)) {
        std::unordered_set<JsString*> seen;
        for (Token const& token : parameter_tokens) {
            if (!seen.insert(atom(token.value)).second)
                return fail(token.position, "Duplicate parameter name not allowed in this context");
        }
    }
    if (name_token && !check_binding_identifier(*name_token, strict))
        return false;
    if (kind == FunctionKind::Getter && !fn->parameters.empty())
        return fail(parameter_tokens.front().position, "Getter must not have any formal parameters");
    if (kind == FunctionKind::Setter && fn->parameters.size() != 1)
        return fail(fn->position, "Setter must have exactly one formal parameter");
    return true;
}

Statement* Parser::Impl::parse_function_declaration()
{
    SourcePosition const start = m_current.position;
    advance(); // function
    if (m_current.is(Punctuator::Star)) {
        fail_unsupported("generators");
        return nullptr;
    }
    if (m_current.type != TokenType::Identifier) {
        if (m_current.is(Punctuator::LeftParen))
            fail(m_current.position, "Function statements require a function name");
        else
            fail_unexpected();
        return nullptr;
    }
    Token const name_token = m_current;
    advance();
    FunctionNode* fn = m_program->make_function();
    fn->position = start;
    fn->source_start = start.offset;
    fn->name = atom(name_token.value);
    auto* declaration = make<FunctionDeclaration>(start);
    declaration->function = fn;
    // The name binds in the enclosing scope, which is still the current
    // one here.
    if (!declare_function(declaration, name_token.position))
        return nullptr;
    if (!parse_function_rest(fn, FunctionKind::Declaration, name_token))
        return nullptr;
    return finish(declaration);
}

// ---- statements -------------------------------------------------------------

bool Parser::Impl::parse_program_body()
{
    push_function(nullptr, &m_program->declarations, false);
    m_program->is_strict = function().is_strict;
    if (!parse_directive_prologue(m_program->body))
        return false;
    if (!parse_statement_list(m_program->body, false))
        return false;
    if (m_current.type != TokenType::EndOfInput)
        return fail_unexpected();
    return pop_function();
}

bool Parser::Impl::parse_statement_list(std::vector<Statement*>& body, bool until_case)
{
    while (m_current.type != TokenType::EndOfInput && !m_current.is(Punctuator::RightBrace)) {
        if (until_case && (m_current.is(Keyword::Case) || m_current.is(Keyword::Default)))
            break;
        Statement* statement = parse_statement_list_item();
        if (!statement)
            return false;
        body.push_back(statement);
    }
    return true;
}

// `let` starts a declaration when an identifier or a pattern follows
// (§14.3.1 and the ExpressionStatement lookahead of §14.5); otherwise it
// is an identifier in sloppy code.
bool Parser::Impl::is_let_declaration_start()
{
    if (m_current.type != TokenType::Identifier || m_current.has_escape || m_current.value != u"let")
        return false;
    Token const next = peek();
    return next.type == TokenType::Identifier || next.is(Punctuator::LeftBracket) || next.is(Punctuator::LeftBrace);
}

// StatementListItem: a Statement or a Declaration (§14).
Statement* Parser::Impl::parse_statement_list_item()
{
    if (m_current.is(Keyword::Function))
        return parse_function_declaration();
    if (m_current.is(Keyword::Class)) {
        fail_unsupported("class declarations");
        return nullptr;
    }
    if (m_current.is(Keyword::Const) || is_let_declaration_start()) {
        VariableDeclaration::Kind const kind = m_current.is(Keyword::Const) ? VariableDeclaration::Kind::Const : VariableDeclaration::Kind::Let;
        VariableDeclaration* declaration = parse_declaration_list(kind, true, false);
        if (!declaration || !consume_semicolon())
            return nullptr;
        return finish(declaration);
    }
    return parse_statement(false);
}

Statement* Parser::Impl::parse_statement(bool is_body)
{
    if (!enter())
        return nullptr;
    Statement* statement = parse_statement_inner(is_body);
    leave();
    return statement;
}

// Statement (§14): `is_body` means the single-statement position of an
// if, loop, with or label, where declarations are not allowed.
Statement* Parser::Impl::parse_statement_inner(bool is_body)
{
    SourcePosition const start = m_current.position;
    if (m_current.is(Punctuator::LeftBrace))
        return parse_block(false);
    if (m_current.is(Punctuator::Semicolon)) {
        advance();
        return finish(make<EmptyStatement>(start));
    }
    if (m_current.type == TokenType::Keyword) {
        switch (m_current.keyword) {
        case Keyword::Var:
            return parse_variable_statement();
        case Keyword::If:
            return parse_if();
        case Keyword::For:
            return parse_for();
        case Keyword::While:
            return parse_while();
        case Keyword::Do:
            return parse_do_while();
        case Keyword::Continue:
            return parse_continue();
        case Keyword::Break:
            return parse_break();
        case Keyword::Return:
            return parse_return();
        case Keyword::With:
            return parse_with();
        case Keyword::Switch:
            return parse_switch();
        case Keyword::Throw:
            return parse_throw();
        case Keyword::Try:
            return parse_try();
        case Keyword::Debugger: {
            advance();
            if (!consume_semicolon())
                return nullptr;
            return finish(make<DebuggerStatement>(start));
        }
        case Keyword::Function:
            // A declaration is a StatementListItem, never a Statement
            // (§14.1.1); Annex B.3.3's if-body exception is handled by
            // parse_if before it gets here.
            fail(start, is_strict()
                    ? "In strict mode code, functions can only be declared at top level or inside a block"
                    : "In non-strict mode code, functions can only be declared at top level, inside a block, or as the body of an if statement");
            return nullptr;
        case Keyword::Class:
            fail_unsupported("class declarations");
            return nullptr;
        case Keyword::Const:
            fail(start, "Lexical declaration cannot appear in a single-statement context");
            return nullptr;
        case Keyword::Import:
        case Keyword::Export:
            fail_unsupported("modules");
            return nullptr;
        default:
            break;
        }
    } else if (m_current.type == TokenType::Identifier) {
        if (is_body && !m_current.has_escape && m_current.value == u"let") {
            // A declaration is not a Statement, and §14.5 forbids an
            // ExpressionStatement from starting with `let [` at all. `let`
            // alone on its line before an identifier or block is the
            // identifier, and ASI ends the statement after it.
            Token const next = peek();
            bool const declaration_shape = next.is(Punctuator::LeftBracket)
                || ((next.type == TokenType::Identifier || next.is(Punctuator::LeftBrace)) && !next.newline_before);
            if (declaration_shape) {
                fail(start, "Lexical declaration cannot appear in a single-statement context");
                return nullptr;
            }
        }
        if (peek().is(Punctuator::Colon))
            return parse_labelled(is_body);
    }
    return parse_expression_statement();
}

Statement* Parser::Impl::parse_iteration_body()
{
    FunctionContext& fn = function();
    ++fn.iteration_depth;
    ++fn.breakable_depth;
    Statement* body = parse_statement(true);
    --fn.iteration_depth;
    --fn.breakable_depth;
    return body;
}

Statement* Parser::Impl::parse_expression_statement()
{
    SourcePosition const start = m_current.position;
    Expression* expression = parse_expression(true);
    if (!expression || !consume_semicolon())
        return nullptr;
    auto* statement = make<ExpressionStatement>(start);
    statement->expression = expression;
    return finish(statement);
}

BlockStatement* Parser::Impl::parse_block(bool is_catch_body)
{
    SourcePosition const start = m_current.position;
    if (!enter())
        return nullptr;
    advance(); // {
    auto* block = make<BlockStatement>(start);
    Scope& block_scope = push_scope(&block->declarations);
    block_scope.is_catch_body = is_catch_body;
    if (!parse_statement_list(block->body, false)) {
        leave();
        return nullptr;
    }
    if (!m_current.is(Punctuator::RightBrace)) {
        leave();
        fail_unexpected();
        return nullptr;
    }
    advance();
    pop_scope();
    leave();
    return finish(block);
}

Statement* Parser::Impl::parse_variable_statement()
{
    VariableDeclaration* declaration = parse_declaration_list(VariableDeclaration::Kind::Var, true, false);
    if (!declaration || !consume_semicolon())
        return nullptr;
    return finish(declaration);
}

// The declarator list of var/let/const (§14.3). A const without an
// initializer is an error except in a for-in head, which the caller
// checks once it knows.
VariableDeclaration* Parser::Impl::parse_declaration_list(VariableDeclaration::Kind kind, bool allow_in, bool in_for_head)
{
    SourcePosition const start = m_current.position;
    advance(); // var, let or const
    auto* declaration = make<VariableDeclaration>(start);
    declaration->kind = kind;
    while (true) {
        if (m_current.is(Punctuator::LeftBracket) || m_current.is(Punctuator::LeftBrace)) {
            fail_unsupported("destructuring patterns");
            return nullptr;
        }
        if (!check_binding_identifier(m_current, is_strict()))
            return nullptr;
        VariableDeclarator declarator;
        declarator.name = atom(m_current.value);
        SourcePosition const name_position = m_current.position;
        advance();
        if (m_current.is(Punctuator::Assign)) {
            advance();
            declarator.init = parse_assignment(allow_in);
            if (!declarator.init)
                return nullptr;
        } else if (kind == VariableDeclaration::Kind::Const && !in_for_head) {
            fail(m_current.position, "Missing initializer in const declaration");
            return nullptr;
        }
        bool const declared = kind == VariableDeclaration::Kind::Var
            ? declare_var(declarator.name, name_position)
            : declare_lexical(declarator.name, kind == VariableDeclaration::Kind::Const, name_position);
        if (!declared)
            return nullptr;
        declaration->declarations.push_back(declarator);
        if (!m_current.is(Punctuator::Comma))
            break;
        advance();
    }
    return declaration;
}

Statement* Parser::Impl::parse_if()
{
    SourcePosition const start = m_current.position;
    advance(); // if
    if (!expect(Punctuator::LeftParen))
        return nullptr;
    Expression* test = parse_expression(true);
    if (!test || !expect(Punctuator::RightParen))
        return nullptr;
    // B.3.3: sloppy code may put a function declaration straight in an
    // if branch; it behaves as the sole item of a block.
    auto parse_branch = [&]() -> Statement* {
        if (m_current.is(Keyword::Function) && !is_strict()) {
            SourcePosition const branch_start = m_current.position;
            auto* block = make<BlockStatement>(branch_start);
            push_scope(&block->declarations);
            Statement* declaration = parse_function_declaration();
            if (!declaration)
                return nullptr;
            block->body.push_back(declaration);
            pop_scope();
            return finish(block);
        }
        return parse_statement(true);
    };
    auto* statement = make<IfStatement>(start);
    statement->test = test;
    statement->consequent = parse_branch();
    if (!statement->consequent)
        return nullptr;
    if (m_current.is(Keyword::Else)) {
        advance();
        statement->alternate = parse_branch();
        if (!statement->alternate)
            return nullptr;
    }
    return finish(statement);
}

// for (init; test; update) and for (x in obj) (§14.7.4, §14.7.5). The
// head's `in` is excluded from its expressions so the loop kind can be
// told apart; a let/const head gets a scope of its own around the whole
// statement.
Statement* Parser::Impl::parse_for()
{
    SourcePosition const start = m_current.position;
    advance(); // for
    if (m_current.is_identifier(u"await") && !m_current.has_escape) {
        fail_unsupported("async functions");
        return nullptr;
    }
    if (!expect(Punctuator::LeftParen))
        return nullptr;
    Declarations head_declarations;
    bool lexical_scope = false;
    VariableDeclaration* declaration = nullptr;
    Expression* target = nullptr;
    SourcePosition const head_start = m_current.position;
    if (m_current.is(Keyword::Var)) {
        declaration = parse_declaration_list(VariableDeclaration::Kind::Var, false, true);
        if (!declaration)
            return nullptr;
    } else if (m_current.is(Keyword::Const) || is_let_declaration_start()) {
        lexical_scope = true;
        push_scope(&head_declarations);
        VariableDeclaration::Kind const kind = m_current.is(Keyword::Const) ? VariableDeclaration::Kind::Const : VariableDeclaration::Kind::Let;
        declaration = parse_declaration_list(kind, false, true);
        if (!declaration)
            return nullptr;
    } else if (!m_current.is(Punctuator::Semicolon)) {
        target = parse_expression(false);
        if (!target)
            return nullptr;
    }

    if (m_current.is(Keyword::In) || (m_current.is_identifier(u"of") && !m_current.has_escape)) {
        if (!m_current.is(Keyword::In)) {
            fail_unsupported("for-of loops");
            return nullptr;
        }
        if (declaration) {
            if (declaration->declarations.size() != 1) {
                fail(head_start, "Invalid left-hand side in for-in loop: Must have a single binding");
                return nullptr;
            }
            // B.3.5: `for (var x = 1 in o)` survives in sloppy code only.
            if (declaration->declarations[0].init
                && (declaration->kind != VariableDeclaration::Kind::Var || is_strict())) {
                fail(head_start, "for-in loop variable declaration may not have an initializer");
                return nullptr;
            }
            finish(declaration);
        } else if ((target->type == NodeType::ArrayLiteral || target->type == NodeType::ObjectLiteral)
            && !m_parenthesised.contains(target)) {
            fail_unsupported("destructuring patterns");
            return nullptr;
        } else if (!check_simple_target(target, head_start, "in for-in loop")) {
            return nullptr;
        }
        advance(); // in
        auto* statement = make<ForInStatement>(start);
        statement->declaration = declaration;
        statement->target = target;
        statement->object = parse_expression(true);
        if (!statement->object || !expect(Punctuator::RightParen))
            return nullptr;
        statement->body = parse_iteration_body();
        if (!statement->body)
            return nullptr;
        if (lexical_scope)
            pop_scope();
        return finish(statement);
    }

    auto* statement = make<ForStatement>(start);
    if (declaration) {
        if (declaration->kind == VariableDeclaration::Kind::Const) {
            for (VariableDeclarator const& declarator : declaration->declarations) {
                if (!declarator.init) {
                    fail(head_start, "Missing initializer in const declaration");
                    return nullptr;
                }
            }
        }
        statement->init = finish(declaration);
    } else if (target) {
        auto* init = make<ExpressionStatement>(head_start);
        init->expression = target;
        statement->init = finish(init);
    }
    if (!expect(Punctuator::Semicolon))
        return nullptr;
    if (!m_current.is(Punctuator::Semicolon)) {
        statement->test = parse_expression(true);
        if (!statement->test)
            return nullptr;
    }
    if (!expect(Punctuator::Semicolon))
        return nullptr;
    if (!m_current.is(Punctuator::RightParen)) {
        statement->update = parse_expression(true);
        if (!statement->update)
            return nullptr;
    }
    if (!expect(Punctuator::RightParen))
        return nullptr;
    statement->body = parse_iteration_body();
    if (!statement->body)
        return nullptr;
    if (lexical_scope) {
        pop_scope();
        statement->declarations = std::move(head_declarations);
    }
    return finish(statement);
}

Statement* Parser::Impl::parse_while()
{
    SourcePosition const start = m_current.position;
    advance(); // while
    if (!expect(Punctuator::LeftParen))
        return nullptr;
    auto* statement = make<WhileStatement>(start);
    statement->test = parse_expression(true);
    if (!statement->test || !expect(Punctuator::RightParen))
        return nullptr;
    statement->body = parse_iteration_body();
    if (!statement->body)
        return nullptr;
    return finish(statement);
}

Statement* Parser::Impl::parse_do_while()
{
    SourcePosition const start = m_current.position;
    advance(); // do
    auto* statement = make<DoWhileStatement>(start);
    statement->body = parse_iteration_body();
    if (!statement->body)
        return nullptr;
    if (!m_current.is(Keyword::While)) {
        fail_unexpected();
        return nullptr;
    }
    advance();
    if (!expect(Punctuator::LeftParen))
        return nullptr;
    statement->test = parse_expression(true);
    if (!statement->test || !expect(Punctuator::RightParen))
        return nullptr;
    // §12.10.1: a semicolon is inserted after the `)` of a do-while even
    // on the same line as what follows.
    if (m_current.is(Punctuator::Semicolon))
        advance();
    return finish(statement);
}

Statement* Parser::Impl::parse_continue()
{
    SourcePosition const start = m_current.position;
    advance(); // continue
    auto* statement = make<ContinueStatement>(start);
    FunctionContext const& fn = function();
    if (m_current.type == TokenType::Identifier && !m_current.newline_before) {
        if (m_current.has_escape && Lexer::keyword_for(m_current.value)) {
            fail(m_current.position, "Keyword must not contain escaped characters");
            return nullptr;
        }
        statement->label = atom(m_current.value);
        std::string const text = utf8_from_utf16(m_current.value);
        auto const label = std::find_if(fn.labels.begin(), fn.labels.end(), [&](Label const& l) { return l.name == statement->label; });
        if (label == fn.labels.end()) {
            fail(m_current.position, "Undefined label '" + text + "'");
            return nullptr;
        }
        if (!label->is_iteration) {
            fail(m_current.position, "Illegal continue statement: '" + text + "' does not denote an iteration statement");
            return nullptr;
        }
        advance();
    } else if (fn.iteration_depth == 0) {
        fail(start, "Illegal continue statement: no surrounding iteration statement");
        return nullptr;
    }
    if (!consume_semicolon())
        return nullptr;
    return finish(statement);
}

Statement* Parser::Impl::parse_break()
{
    SourcePosition const start = m_current.position;
    advance(); // break
    auto* statement = make<BreakStatement>(start);
    FunctionContext const& fn = function();
    if (m_current.type == TokenType::Identifier && !m_current.newline_before) {
        if (m_current.has_escape && Lexer::keyword_for(m_current.value)) {
            fail(m_current.position, "Keyword must not contain escaped characters");
            return nullptr;
        }
        statement->label = atom(m_current.value);
        bool const known = std::any_of(fn.labels.begin(), fn.labels.end(), [&](Label const& l) { return l.name == statement->label; });
        if (!known) {
            fail(m_current.position, "Undefined label '" + utf8_from_utf16(m_current.value) + "'");
            return nullptr;
        }
        advance();
    } else if (fn.breakable_depth == 0) {
        fail(start, "Illegal break statement");
        return nullptr;
    }
    if (!consume_semicolon())
        return nullptr;
    return finish(statement);
}

Statement* Parser::Impl::parse_return()
{
    SourcePosition const start = m_current.position;
    if (!function().node && !m_options.allow_return) {
        fail(start, "Illegal return statement");
        return nullptr;
    }
    advance(); // return
    auto* statement = make<ReturnStatement>(start);
    // [no LineTerminator here] before the argument (§14.10).
    if (!m_current.newline_before && !m_current.is(Punctuator::Semicolon) && !m_current.is(Punctuator::RightBrace)
        && m_current.type != TokenType::EndOfInput) {
        statement->argument = parse_expression(true);
        if (!statement->argument)
            return nullptr;
    }
    if (!consume_semicolon())
        return nullptr;
    return finish(statement);
}

Statement* Parser::Impl::parse_with()
{
    SourcePosition const start = m_current.position;
    if (is_strict()) {
        fail(start, "Strict mode code may not include a with statement");
        return nullptr;
    }
    advance(); // with
    if (!expect(Punctuator::LeftParen))
        return nullptr;
    auto* statement = make<WithStatement>(start);
    statement->object = parse_expression(true);
    if (!statement->object || !expect(Punctuator::RightParen))
        return nullptr;
    statement->body = parse_statement(true);
    if (!statement->body)
        return nullptr;
    return finish(statement);
}

Statement* Parser::Impl::parse_switch()
{
    SourcePosition const start = m_current.position;
    advance(); // switch
    if (!expect(Punctuator::LeftParen))
        return nullptr;
    auto* statement = make<SwitchStatement>(start);
    statement->discriminant = parse_expression(true);
    if (!statement->discriminant || !expect(Punctuator::RightParen) || !expect(Punctuator::LeftBrace))
        return nullptr;
    push_scope(&statement->declarations);
    FunctionContext& fn = function();
    ++fn.breakable_depth;
    bool has_default = false;
    while (!m_current.is(Punctuator::RightBrace)) {
        SwitchCase clause;
        if (m_current.is(Keyword::Case)) {
            advance();
            clause.test = parse_expression(true);
            if (!clause.test)
                return nullptr;
        } else if (m_current.is(Keyword::Default)) {
            if (has_default) {
                fail(m_current.position, "More than one default clause in switch statement");
                return nullptr;
            }
            has_default = true;
            advance();
        } else {
            fail_unexpected();
            return nullptr;
        }
        if (!expect(Punctuator::Colon))
            return nullptr;
        if (!parse_statement_list(clause.consequent, true))
            return nullptr;
        statement->cases.push_back(std::move(clause));
    }
    advance(); // }
    --fn.breakable_depth;
    pop_scope();
    return finish(statement);
}

Statement* Parser::Impl::parse_throw()
{
    SourcePosition const start = m_current.position;
    advance(); // throw
    if (m_current.newline_before) {
        fail(m_current.position, "Illegal newline after throw");
        return nullptr;
    }
    auto* statement = make<ThrowStatement>(start);
    statement->argument = parse_expression(true);
    if (!statement->argument || !consume_semicolon())
        return nullptr;
    return finish(statement);
}

Statement* Parser::Impl::parse_try()
{
    SourcePosition const start = m_current.position;
    advance(); // try
    if (!m_current.is(Punctuator::LeftBrace)) {
        fail_unexpected();
        return nullptr;
    }
    auto* statement = make<TryStatement>(start);
    statement->block = parse_block(false);
    if (!statement->block)
        return nullptr;
    if (m_current.is(Keyword::Catch)) {
        advance();
        if (m_current.is(Punctuator::LeftParen)) {
            advance();
            if (m_current.is(Punctuator::LeftBracket) || m_current.is(Punctuator::LeftBrace)) {
                fail_unsupported("destructuring patterns");
                return nullptr;
            }
            if (!check_binding_identifier(m_current, is_strict()))
                return nullptr;
            statement->catch_parameter = atom(m_current.value);
            advance();
            if (!expect(Punctuator::RightParen))
                return nullptr;
            // The parameter's own scope (§14.15.1): the body's lexicals
            // may not redeclare it; a `var` may (B.3.4).
            Scope& parameter_scope = push_scope(nullptr);
            parameter_scope.is_catch_parameter = true;
            parameter_scope.lexical_names.insert(statement->catch_parameter);
            if (!m_current.is(Punctuator::LeftBrace)) {
                fail_unexpected();
                return nullptr;
            }
            statement->handler = parse_block(true);
            if (!statement->handler)
                return nullptr;
            pop_scope();
        } else {
            // Optional catch binding (ES2019).
            if (!m_current.is(Punctuator::LeftBrace)) {
                fail_unexpected();
                return nullptr;
            }
            statement->handler = parse_block(false);
            if (!statement->handler)
                return nullptr;
        }
    }
    if (m_current.is(Keyword::Finally)) {
        advance();
        if (!m_current.is(Punctuator::LeftBrace)) {
            fail_unexpected();
            return nullptr;
        }
        statement->finalizer = parse_block(false);
        if (!statement->finalizer)
            return nullptr;
    }
    if (!statement->handler && !statement->finalizer) {
        fail(m_current.position, "Missing catch or finally after try");
        return nullptr;
    }
    return finish(statement);
}

// LabelledStatement (§14.13): `a: b: stmt` nests one node per label. The
// labels name an iteration statement — a `continue` target — when a
// loop follows them directly.
Statement* Parser::Impl::parse_labelled(bool is_body)
{
    FunctionContext& fn = function();
    struct Pending {
        JsString* name;
        SourcePosition position;
    };
    std::vector<Pending> pending;
    while (m_current.type == TokenType::Identifier && peek().is(Punctuator::Colon)) {
        if (m_current.has_escape && Lexer::keyword_for(m_current.value)) {
            fail(m_current.position, "Keyword must not contain escaped characters");
            return nullptr;
        }
        if (is_strict() && is_strict_reserved_word(m_current.value)) {
            fail(m_current.position, "Unexpected strict mode reserved word");
            return nullptr;
        }
        JsString* name = atom(m_current.value);
        bool const duplicate = std::any_of(fn.labels.begin(), fn.labels.end(), [&](Label const& l) { return l.name == name; });
        if (duplicate) {
            fail(m_current.position, "Label '" + utf8_from_utf16(m_current.value) + "' has already been declared");
            return nullptr;
        }
        pending.push_back({ name, m_current.position });
        fn.labels.push_back({ name, false });
        advance(); // the name
        advance(); // :
    }
    bool const iteration = m_current.is(Keyword::For) || m_current.is(Keyword::While) || m_current.is(Keyword::Do);
    for (std::size_t i = 0; i < pending.size(); ++i)
        fn.labels[fn.labels.size() - pending.size() + i].is_iteration = iteration;
    Statement* body = nullptr;
    if (m_current.is(Keyword::Function)) {
        // B.3.2 allows a labelled function declaration in sloppy code, but
        // never as the body of an if, loop or with (§14.1.1).
        if (is_strict()) {
            fail(m_current.position, "In strict mode code, functions can only be declared at top level or inside a block");
            return nullptr;
        }
        if (is_body) {
            fail(m_current.position, "In non-strict mode code, functions can only be declared at top level, inside a block, or as the body of an if statement");
            return nullptr;
        }
        body = parse_function_declaration();
    } else {
        body = parse_statement(true);
    }
    if (!body)
        return nullptr;
    fn.labels.resize(fn.labels.size() - pending.size());
    for (std::size_t i = pending.size(); i > 0; --i) {
        auto* labelled = make<LabeledStatement>(pending[i - 1].position);
        labelled->label = pending[i - 1].name;
        labelled->body = body;
        body = finish(labelled);
    }
    return body;
}

// ---- the public face ----------------------------------------------------------

Parser::Parser(Heap& heap, std::u16string source, ParseOptions options)
    : m_impl(std::make_unique<Impl>(heap, std::move(source), options))
{
}

Parser::~Parser() = default;

std::unique_ptr<Program> Parser::parse_program(std::string name)
{
    Impl& impl = *m_impl;
    if (!impl.m_program) {
        if (!m_error)
            m_error = ParseError { SourcePosition {}, "the parser has already produced its program" };
        return nullptr;
    }
    impl.m_program->name = std::move(name);
    if (!impl.parse_program_body()) {
        m_error = impl.m_error ? *impl.m_error : ParseError { impl.m_current.position, "parse failed" };
        impl.m_program.reset();
        return nullptr;
    }
    return std::move(impl.m_program);
}

// CreateDynamicFunction (§20.2.1.1.1): the texts are wrapped as the
// specification lays them out and parsed as a script. The function must
// span the whole wrapper — a body text that closes the function early
// and smuggles statements after it is rejected, as the specification's
// separate parse of the body would reject it.
std::unique_ptr<Program> Parser::parse_function_constructor(Heap& heap, std::u16string_view parameters,
    std::u16string_view body, ParseError* error)
{
    // The parameter text must be FormalParameters on its own (step 17 of
    // §20.2.1.1.1): parsed first with an empty body, so that a comment or
    // bracket left open in it cannot borrow its closing from the body.
    {
        std::u16string head = u"(function anonymous(";
        head += parameters;
        head += u"\n) {\n})";
        std::size_t const head_end = head.size();
        Parser head_parser(heap, std::move(head));
        std::unique_ptr<Program> const head_program = head_parser.parse_program("<Function>");
        bool head_well_formed = head_program && head_program->body.size() == 1 && head_program->body[0]->type == NodeType::ExpressionStatement;
        if (head_well_formed) {
            Expression const* expression = static_cast<ExpressionStatement const*>(head_program->body[0])->expression;
            head_well_formed = expression->type == NodeType::FunctionExpression
                && static_cast<FunctionExpression const*>(expression)->function->source_end + 1 == head_end;
        }
        if (!head_well_formed) {
            if (error)
                *error = head_parser.error() ? *head_parser.error() : ParseError { SourcePosition {}, "Unexpected token in function parameters" };
            return nullptr;
        }
    }
    std::u16string source = u"(function anonymous(";
    source += parameters;
    source += u"\n) {\n";
    source += body;
    source += u"\n})";
    std::size_t const wrapper_end = source.size();
    Parser parser(heap, std::move(source));
    std::unique_ptr<Program> program = parser.parse_program("<Function>");
    if (!program) {
        if (error)
            *error = parser.error() ? *parser.error() : ParseError { SourcePosition {}, "parse failed" };
        return nullptr;
    }
    bool well_formed = program->body.size() == 1 && program->body[0]->type == NodeType::ExpressionStatement;
    if (well_formed) {
        Expression const* expression = static_cast<ExpressionStatement const*>(program->body[0])->expression;
        well_formed = expression->type == NodeType::FunctionExpression
            && static_cast<FunctionExpression const*>(expression)->function->source_end + 1 == wrapper_end;
    }
    if (!well_formed) {
        if (error)
            *error = ParseError { SourcePosition {}, "Unexpected token in function body" };
        return nullptr;
    }
    return program;
}

// ---- dump_ast ---------------------------------------------------------------------

namespace {

// A tree can be deeper than the parser's nesting cap along a left-nested
// operator chain, which the parser builds iteratively; the dump stops
// descending past this and prints # in place of the subtree.
constexpr int dump_depth_limit = 4 * Parser::max_nesting_depth;

char const* unary_text(UnaryOp op)
{
    switch (op) {
    case UnaryOp::Minus: return "-";
    case UnaryOp::Plus: return "+";
    case UnaryOp::Not: return "!";
    case UnaryOp::BitwiseNot: return "~";
    case UnaryOp::Typeof: return "typeof";
    case UnaryOp::Void: return "void";
    case UnaryOp::Delete: return "delete";
    }
    return "?";
}

char const* binary_text(BinaryOp op)
{
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Subtract: return "-";
    case BinaryOp::Multiply: return "*";
    case BinaryOp::Divide: return "/";
    case BinaryOp::Remainder: return "%";
    case BinaryOp::Exponent: return "**";
    case BinaryOp::LeftShift: return "<<";
    case BinaryOp::RightShift: return ">>";
    case BinaryOp::UnsignedRightShift: return ">>>";
    case BinaryOp::BitwiseAnd: return "&";
    case BinaryOp::BitwiseOr: return "|";
    case BinaryOp::BitwiseXor: return "^";
    case BinaryOp::Equal: return "==";
    case BinaryOp::NotEqual: return "!=";
    case BinaryOp::StrictEqual: return "===";
    case BinaryOp::StrictNotEqual: return "!==";
    case BinaryOp::Less: return "<";
    case BinaryOp::LessEqual: return "<=";
    case BinaryOp::Greater: return ">";
    case BinaryOp::GreaterEqual: return ">=";
    case BinaryOp::In: return "in";
    case BinaryOp::Instanceof: return "instanceof";
    }
    return "?";
}

char const* logical_text(LogicalOp op)
{
    switch (op) {
    case LogicalOp::And: return "&&";
    case LogicalOp::Or: return "||";
    case LogicalOp::Nullish: return "??";
    }
    return "?";
}

char const* assignment_text(AssignmentOp op)
{
    switch (op) {
    case AssignmentOp::Assign: return "=";
    case AssignmentOp::Add: return "+=";
    case AssignmentOp::Subtract: return "-=";
    case AssignmentOp::Multiply: return "*=";
    case AssignmentOp::Divide: return "/=";
    case AssignmentOp::Remainder: return "%=";
    case AssignmentOp::Exponent: return "**=";
    case AssignmentOp::LeftShift: return "<<=";
    case AssignmentOp::RightShift: return ">>=";
    case AssignmentOp::UnsignedRightShift: return ">>>=";
    case AssignmentOp::BitwiseAnd: return "&=";
    case AssignmentOp::BitwiseOr: return "|=";
    case AssignmentOp::BitwiseXor: return "^=";
    case AssignmentOp::LogicalAnd: return "&&=";
    case AssignmentOp::LogicalOr: return "||=";
    case AssignmentOp::Nullish: return "??" "=";
    }
    return "?";
}

struct Dumper {
    std::string out;
    int depth = 0;

    void quoted(std::u16string_view text)
    {
        out += '"';
        for (char const c : utf8_from_utf16(text)) {
            if (c == '"' || c == '\\')
                out += '\\';
            out += c;
        }
        out += '"';
    }

    void name(JsString const* atom)
    {
        out += atom ? utf8_from_utf16(atom->view()) : std::string("-");
    }

    // True when the walk may go deeper; otherwise a marker was written.
    bool descend()
    {
        if (depth >= dump_depth_limit) {
            out += '#';
            return false;
        }
        ++depth;
        return true;
    }

    void function(FunctionNode const& fn)
    {
        if (fn.is_arrow) {
            out += "(arrow (";
        } else {
            out += "(function ";
            if (fn.name) {
                name(fn.name);
                out += ' ';
            }
            out += '(';
        }
        for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
            if (i > 0)
                out += ' ';
            name(fn.parameters[i]);
        }
        out += ')';
        if (fn.expression_body) {
            out += ' ';
            expression(fn.expression_body);
        } else {
            for (Statement const* s : fn.body) {
                out += ' ';
                statement(s);
            }
        }
        out += ')';
    }

    void declarators(VariableDeclaration const& declaration)
    {
        switch (declaration.kind) {
        case VariableDeclaration::Kind::Var: out += "(var"; break;
        case VariableDeclaration::Kind::Let: out += "(let"; break;
        case VariableDeclaration::Kind::Const: out += "(const"; break;
        }
        for (VariableDeclarator const& declarator : declaration.declarations) {
            out += " (";
            name(declarator.name);
            if (declarator.init) {
                out += ' ';
                expression(declarator.init);
            }
            out += ')';
        }
        out += ')';
    }

    void optional_expression(Expression const* e)
    {
        if (e) {
            out += ' ';
            expression(e);
        }
    }

    void expression(Expression const* e)
    {
        if (!descend())
            return;
        switch (e->type) {
        case NodeType::Identifier:
            out += "(id ";
            name(static_cast<Identifier const*>(e)->name);
            out += ')';
            break;
        case NodeType::NumberLiteral:
            out += "(number " + number_to_utf8(static_cast<NumberLiteral const*>(e)->value) + ")";
            break;
        case NodeType::StringLiteral:
            out += "(string ";
            quoted(static_cast<StringLiteral const*>(e)->value->view());
            out += ')';
            break;
        case NodeType::BooleanLiteral:
            out += static_cast<BooleanLiteral const*>(e)->value ? "true" : "false";
            break;
        case NodeType::NullLiteral:
            out += "null";
            break;
        case NodeType::ThisExpression:
            out += "this";
            break;
        case NodeType::RegExpLiteral: {
            auto const* regex = static_cast<RegExpLiteral const*>(e);
            out += "(regex /" + utf8_from_utf16(regex->pattern->view()) + "/" + utf8_from_utf16(regex->flags->view()) + ")";
            break;
        }
        case NodeType::TemplateLiteral: {
            auto const* literal = static_cast<TemplateLiteral const*>(e);
            out += "(template (";
            for (std::size_t i = 0; i < literal->cooked.size(); ++i) {
                if (i > 0)
                    out += ' ';
                quoted(literal->cooked[i]->view());
            }
            out += ") (";
            for (std::size_t i = 0; i < literal->expressions.size(); ++i) {
                if (i > 0)
                    out += ' ';
                expression(literal->expressions[i]);
            }
            out += "))";
            break;
        }
        case NodeType::ArrayLiteral:
            out += "(array";
            for (Expression const* element : static_cast<ArrayLiteral const*>(e)->elements) {
                out += ' ';
                if (element)
                    expression(element);
                else
                    out += "hole";
            }
            out += ')';
            break;
        case NodeType::ObjectLiteral:
            out += "(object";
            for (PropertyDefinition const& property : static_cast<ObjectLiteral const*>(e)->properties) {
                out += " (";
                if (property.computed_key) {
                    out += property.kind == PropertyDefinition::Kind::Get ? "get computed " : property.kind == PropertyDefinition::Kind::Set ? "set computed " : "computed ";
                    expression(property.computed_key);
                } else if (property.is_proto) {
                    out += "proto";
                } else {
                    out += property.kind == PropertyDefinition::Kind::Get ? "get " : property.kind == PropertyDefinition::Kind::Set ? "set " : "init ";
                    quoted(property.key->view());
                }
                out += ' ';
                expression(property.value);
                out += ')';
            }
            out += ')';
            break;
        case NodeType::FunctionExpression:
            function(*static_cast<FunctionExpression const*>(e)->function);
            break;
        case NodeType::ArrowFunction:
            function(*static_cast<ArrowFunction const*>(e)->function);
            break;
        case NodeType::UnaryExpression: {
            auto const* unary = static_cast<UnaryExpression const*>(e);
            out += std::string("(unary ") + unary_text(unary->op) + " ";
            expression(unary->operand);
            out += ')';
            break;
        }
        case NodeType::UpdateExpression: {
            auto const* update = static_cast<UpdateExpression const*>(e);
            out += std::string("(update ") + (update->increment ? "++" : "--") + (update->prefix ? " prefix " : " postfix ");
            expression(update->target);
            out += ')';
            break;
        }
        case NodeType::BinaryExpression: {
            auto const* binary = static_cast<BinaryExpression const*>(e);
            out += std::string("(binary ") + binary_text(binary->op) + " ";
            expression(binary->left);
            out += ' ';
            expression(binary->right);
            out += ')';
            break;
        }
        case NodeType::LogicalExpression: {
            auto const* logical = static_cast<LogicalExpression const*>(e);
            out += std::string("(logical ") + logical_text(logical->op) + " ";
            expression(logical->left);
            out += ' ';
            expression(logical->right);
            out += ')';
            break;
        }
        case NodeType::AssignmentExpression: {
            auto const* assignment = static_cast<AssignmentExpression const*>(e);
            out += std::string("(assign ") + assignment_text(assignment->op) + " ";
            expression(assignment->target);
            out += ' ';
            expression(assignment->value);
            out += ')';
            break;
        }
        case NodeType::ConditionalExpression: {
            auto const* conditional = static_cast<ConditionalExpression const*>(e);
            out += "(cond ";
            expression(conditional->test);
            out += ' ';
            expression(conditional->consequent);
            out += ' ';
            expression(conditional->alternate);
            out += ')';
            break;
        }
        case NodeType::CallExpression: {
            auto const* call = static_cast<CallExpression const*>(e);
            out += call->optional ? "(call? " : "(call ";
            expression(call->callee);
            for (Expression const* argument : call->arguments) {
                out += ' ';
                expression(argument);
            }
            out += ')';
            break;
        }
        case NodeType::NewExpression: {
            auto const* construct = static_cast<NewExpression const*>(e);
            out += "(new ";
            expression(construct->callee);
            for (Expression const* argument : construct->arguments) {
                out += ' ';
                expression(argument);
            }
            out += ')';
            break;
        }
        case NodeType::MemberExpression: {
            auto const* member = static_cast<MemberExpression const*>(e);
            if (member->property) {
                out += member->optional ? "(index? " : "(index ";
                expression(member->object);
                out += ' ';
                expression(member->property);
            } else {
                out += member->optional ? "(member? " : "(member ";
                expression(member->object);
                out += ' ';
                name(member->name);
            }
            out += ')';
            break;
        }
        case NodeType::SequenceExpression:
            out += "(seq";
            for (Expression const* part : static_cast<SequenceExpression const*>(e)->expressions) {
                out += ' ';
                expression(part);
            }
            out += ')';
            break;
        default:
            out += "(?)";
            break;
        }
        --depth;
    }

    void statements(std::vector<Statement*> const& list)
    {
        for (Statement const* s : list) {
            out += ' ';
            statement(s);
        }
    }

    void statement(Statement const* s)
    {
        if (!descend())
            return;
        switch (s->type) {
        case NodeType::VariableDeclaration:
            declarators(*static_cast<VariableDeclaration const*>(s));
            break;
        case NodeType::FunctionDeclaration:
            function(*static_cast<FunctionDeclaration const*>(s)->function);
            break;
        case NodeType::ExpressionStatement:
            out += "(expr ";
            expression(static_cast<ExpressionStatement const*>(s)->expression);
            out += ')';
            break;
        case NodeType::BlockStatement:
            out += "(block";
            statements(static_cast<BlockStatement const*>(s)->body);
            out += ')';
            break;
        case NodeType::EmptyStatement:
            out += "(empty)";
            break;
        case NodeType::DebuggerStatement:
            out += "(debugger)";
            break;
        case NodeType::IfStatement: {
            auto const* branch = static_cast<IfStatement const*>(s);
            out += "(if ";
            expression(branch->test);
            out += ' ';
            statement(branch->consequent);
            if (branch->alternate) {
                out += ' ';
                statement(branch->alternate);
            }
            out += ')';
            break;
        }
        case NodeType::ForStatement: {
            auto const* loop = static_cast<ForStatement const*>(s);
            out += "(for ";
            if (loop->init)
                statement(loop->init);
            else
                out += '-';
            out += ' ';
            if (loop->test)
                expression(loop->test);
            else
                out += '-';
            out += ' ';
            if (loop->update)
                expression(loop->update);
            else
                out += '-';
            out += ' ';
            statement(loop->body);
            out += ')';
            break;
        }
        case NodeType::ForInStatement: {
            auto const* loop = static_cast<ForInStatement const*>(s);
            out += "(for-in ";
            if (loop->declaration)
                declarators(*loop->declaration);
            else
                expression(loop->target);
            out += ' ';
            expression(loop->object);
            out += ' ';
            statement(loop->body);
            out += ')';
            break;
        }
        case NodeType::WhileStatement: {
            auto const* loop = static_cast<WhileStatement const*>(s);
            out += "(while ";
            expression(loop->test);
            out += ' ';
            statement(loop->body);
            out += ')';
            break;
        }
        case NodeType::DoWhileStatement: {
            auto const* loop = static_cast<DoWhileStatement const*>(s);
            out += "(do ";
            statement(loop->body);
            out += ' ';
            expression(loop->test);
            out += ')';
            break;
        }
        case NodeType::ReturnStatement:
            out += "(return";
            optional_expression(static_cast<ReturnStatement const*>(s)->argument);
            out += ')';
            break;
        case NodeType::BreakStatement: {
            auto const* jump = static_cast<BreakStatement const*>(s);
            out += "(break";
            if (jump->label) {
                out += ' ';
                name(jump->label);
            }
            out += ')';
            break;
        }
        case NodeType::ContinueStatement: {
            auto const* jump = static_cast<ContinueStatement const*>(s);
            out += "(continue";
            if (jump->label) {
                out += ' ';
                name(jump->label);
            }
            out += ')';
            break;
        }
        case NodeType::ThrowStatement:
            out += "(throw ";
            expression(static_cast<ThrowStatement const*>(s)->argument);
            out += ')';
            break;
        case NodeType::TryStatement: {
            auto const* guard = static_cast<TryStatement const*>(s);
            out += "(try ";
            statement(guard->block);
            if (guard->handler) {
                out += " (catch ";
                if (guard->catch_parameter) {
                    name(guard->catch_parameter);
                    out += ' ';
                }
                statement(guard->handler);
                out += ')';
            }
            if (guard->finalizer) {
                out += " (finally ";
                statement(guard->finalizer);
                out += ')';
            }
            out += ')';
            break;
        }
        case NodeType::SwitchStatement: {
            auto const* dispatch = static_cast<SwitchStatement const*>(s);
            out += "(switch ";
            expression(dispatch->discriminant);
            for (SwitchCase const& clause : dispatch->cases) {
                if (clause.test) {
                    out += " (case ";
                    expression(clause.test);
                } else {
                    out += " (default";
                }
                statements(clause.consequent);
                out += ')';
            }
            out += ')';
            break;
        }
        case NodeType::LabeledStatement: {
            auto const* labelled = static_cast<LabeledStatement const*>(s);
            out += "(label ";
            name(labelled->label);
            out += ' ';
            statement(labelled->body);
            out += ')';
            break;
        }
        case NodeType::WithStatement: {
            auto const* scope = static_cast<WithStatement const*>(s);
            out += "(with ";
            expression(scope->object);
            out += ' ';
            statement(scope->body);
            out += ')';
            break;
        }
        default:
            out += "(?)";
            break;
        }
        --depth;
    }
};

} // namespace

std::string dump_ast(Program const& program)
{
    Dumper dumper;
    dumper.out += "(program";
    if (program.is_strict)
        dumper.out += " strict";
    dumper.statements(program.body);
    dumper.out += ')';
    std::string out = std::move(dumper.out);
    return out;
}

}
