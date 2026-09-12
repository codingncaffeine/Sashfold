#include "js/Compiler.h"

// One pass over a function body, statements in order and expressions by
// recursive descent, emitting the stack code of Bytecode.h. Two things
// the compiler must get exactly right, and checks as it goes:
//
// The control scopes: every environment the body pushes is popped on
// every way out of it — the fall-through, a break or continue or return
// that crosses it (emitted inline, scope by scope), and a throw (the
// handler table records the depths to cut back to). A `finally` is
// entered with a token that says how it was reached, and a jump table
// after it continues the exit; a break/continue/return that crosses one
// records itself as a deferred command whose arm the table gets.
//
// The stack discipline: the compiler tracks the operand depth after each
// instruction and refuses to bind a label at two different depths, so a
// mismatch is a compile error here and never a wrong value there.

#include "js/Evaluator.h"
#include "js/Heap.h"
#include "js/Strings.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::js {

namespace {

using BindMode = Interpreter::Impl::BindMode;

class Compiler {
public:
    Compiler(FunctionNode const& node, Heap& heap)
        : m_node(node)
        , m_heap(heap)
        , m_code(std::make_unique<CodeBlock>())
    {
        m_code->function = &node;
        m_code->strict = node.is_strict;
        m_code->is_generator = node.is_generator;
        m_code->is_async = node.is_async;
    }

    std::unique_ptr<CodeBlock> compile(std::string* error)
    {
        if (m_node.expression_body) {
            compile_expression(m_node.expression_body);
            emit(Opcode::Return);
        } else {
            compile_statements(m_node.body);
            if (!m_unreachable) {
                emit(Opcode::PushUndefined);
                emit(Opcode::Return);
            }
        }
        if (!m_error.empty()) {
            if (error)
                *error = m_error;
            return nullptr;
        }
        return std::move(m_code);
    }

private:
    // ---- labels, scopes, emission ----------------------------------------

    struct Label {
        std::vector<std::uint32_t> sites;
        std::uint32_t target = None;
        int depth = -1;
    };

    struct Deferred {
        enum class Kind : std::uint8_t { Break, Continue, Return };
        Kind kind;
        JsString* label; // the target label, or null
    };

    struct Scope {
        enum class Kind : std::uint8_t { Block, With, Loop, Label, Finally, Switch };
        explicit Scope(Kind k)
            : kind(k)
        {
        }
        Kind kind;
        std::vector<JsString*> labels; // Loop, Label, Switch: what `break L` may name
        Label* break_label = nullptr;
        Label* continue_label = nullptr; // Loop
        std::uint32_t iterator_reg = None; // Loop: a for-of's iterator, closed on break and on crossing
        bool async_iterator = false; // Loop: a for-await's, closed with an await
        bool owns_env = false; // a PopEnv on the way out
        std::uint32_t token_reg = 0; // Finally
        std::uint32_t value_reg = 0;
        Label* finally_entry = nullptr;
        std::vector<Deferred> deferred; // token i + 2 → deferred[i]
        int depth = 0; // the operand depth where the scope began
    };

    // The chain of optional accesses an expression may be: every `?.`
    // that finds a nullish base jumps to its own fixup, which discards
    // what the chain had pushed so far and lands on undefined.
    struct Fixup {
        Label label;
        int pops;
        int ref_drops;
    };
    struct ChainContext {
        std::vector<Fixup> fixups;
        int base_depth = 0;
        int base_refs = 0;
    };

    void fail(std::string message)
    {
        if (m_error.empty())
            m_error = std::move(message);
    }

    std::uint32_t here() const { return static_cast<std::uint32_t>(m_code->code.size()); }

    void adjust(int delta)
    {
        if (m_unreachable)
            return;
        m_depth += delta;
        if (m_depth < 0) {
            fail("internal: operand stack underflow in the compiler");
            m_depth = 0;
        }
        m_code->max_stack = std::max(m_code->max_stack, static_cast<std::uint32_t>(m_depth));
    }

    static int ref_effect(Opcode op)
    {
        switch (op) {
        case Opcode::RefName:
        case Opcode::RefMember:
        case Opcode::RefMemberNamed:
        case Opcode::RefSuper:
        case Opcode::RefSuperNamed:
        case Opcode::RefPrivate:
            return 1;
        case Opcode::RefPut:
        case Opcode::RefPutKeep:
        case Opcode::RefDrop:
        case Opcode::RefDelete:
            return -1;
        default:
            return 0;
        }
    }

    std::uint32_t emit(Opcode op, std::uint32_t a = 0, std::uint32_t b = 0, std::uint8_t flags = 0)
    {
        std::uint32_t const index = here();
        m_code->code.push_back(Instruction { op, flags, 0, a, b });
        int const effect = stack_effect(op);
        if (effect != Var)
            adjust(effect);
        if (!m_unreachable) {
            m_refs += ref_effect(op);
            if (m_refs < 0) {
                fail("internal: reference stack underflow in the compiler");
                m_refs = 0;
            }
        }
        if (op == Opcode::Jump || op == Opcode::Return || op == Opcode::Throw || op == Opcode::ThrowTypeErrorConst)
            m_unreachable = true;
        return index;
    }

    // A jump to a label; the depth after the jump's own effect is what
    // the label must be bound at.
    void jump(Opcode op, Label& label)
    {
        bool const was_unreachable = m_unreachable;
        int const depth_before = m_depth;
        std::uint32_t const site = emit(op);
        label.sites.push_back(site);
        if (label.target != None)
            m_code->code[site].a = label.target; // already bound: a backward jump
        if (was_unreachable)
            return;
        // A plain Jump pops nothing; the conditional ones already adjusted.
        int const depth_at_target = op == Opcode::Jump ? depth_before : m_depth;
        if (label.depth == -1)
            label.depth = depth_at_target;
        else if (label.depth != depth_at_target)
            fail("internal: a label reached at two operand depths");
    }

    void bind(Label& label)
    {
        label.target = here();
        for (std::uint32_t const site : label.sites)
            m_code->code[site].a = label.target;
        if (m_unreachable) {
            // Reached only by jumps: their depth is the truth. A label no
            // reachable jump targets is dead code still, and stays so:
            // what follows a throw or a return binds labels of its own.
            if (label.depth == -1)
                return;
            m_depth = label.depth;
            m_unreachable = false;
        } else if (label.depth == -1) {
            label.depth = m_depth;
        } else if (label.depth != m_depth) {
            fail("internal: a label bound at a depth its jumps disagree with");
        }
    }

    // A landing pad for the unwinder: reached by no jump, at the depth the
    // handler restores plus the thrown value.
    void land(int depth, int refs)
    {
        m_unreachable = true;
        Label landing;
        landing.depth = depth;
        bind(landing);
        m_refs = refs;
    }

    std::uint32_t new_register() { return m_code->register_count++; }
    std::uint32_t new_jump_table()
    {
        m_code->jump_tables.emplace_back();
        return static_cast<std::uint32_t>(m_code->jump_tables.size() - 1);
    }

    void push_scope(Scope scope)
    {
        scope.depth = m_depth;
        m_scopes.push_back(std::move(scope));
    }
    void pop_scope() { m_scopes.pop_back(); }

    std::uint32_t env_depth() const
    {
        std::uint32_t depth = 0;
        for (Scope const& scope : m_scopes)
            depth += scope.owns_env ? 1 : 0;
        return depth;
    }

    void add_handler(std::uint32_t start, std::uint32_t end, std::uint32_t target, int depth, int refs, std::uint32_t envs)
    {
        Handler handler;
        handler.start = start;
        handler.end = end;
        handler.target = target;
        handler.stack_depth = static_cast<std::uint32_t>(depth);
        handler.ref_depth = static_cast<std::uint32_t>(refs);
        handler.env_depth = envs;
        m_code->handlers.push_back(handler);
    }

    // ---- pools ------------------------------------------------------------

    template<typename T>
    static std::uint32_t pooled(std::vector<T>& pool, T const& item)
    {
        for (std::size_t i = 0; i < pool.size(); ++i) {
            if (pool[i] == item)
                return static_cast<std::uint32_t>(i);
        }
        pool.push_back(item);
        return static_cast<std::uint32_t>(pool.size() - 1);
    }
    std::uint32_t constant(Value const& value) { return pooled(m_code->constants, value); }
    std::uint32_t bigint(BigInteger const& value) { return pooled(m_code->bigints, value); }
    std::uint32_t name(JsString* atom) { return pooled(m_code->names, atom); }
    std::uint32_t name_of(std::u16string_view text) { return name(m_heap.atom(text)); }
    std::uint32_t constant_string(std::u16string_view text) { return constant(Value::string(m_heap.atom(text))); }
    std::uint32_t function(FunctionNode const* node) { return pooled(m_code->functions, node); }
    std::uint32_t class_node(ClassNode const* node) { return pooled(m_code->classes, node); }
    std::uint32_t template_index(TemplateLiteral const* node) { return pooled(m_code->templates, node); }
    std::uint32_t regexp(RegExpLiteral const* node) { return pooled(m_code->regexps, node); }
    std::uint32_t declarations(Declarations const* node) { return pooled(m_code->declarations, node); }
    std::uint32_t node_index(Expression const* node) { return pooled(m_code->nodes, node); }
    std::uint32_t name_list(std::vector<JsString*> names)
    {
        m_code->name_lists.push_back(std::move(names));
        return static_cast<std::uint32_t>(m_code->name_lists.size() - 1);
    }

    // ---- exits ------------------------------------------------------------

    static bool is_target(Scope const& scope, Deferred::Kind kind, JsString* label)
    {
        bool const has_label = label != nullptr
            && std::find(scope.labels.begin(), scope.labels.end(), label) != scope.labels.end();
        switch (kind) {
        case Deferred::Kind::Break:
            if (label)
                return has_label && (scope.kind == Scope::Kind::Loop || scope.kind == Scope::Kind::Label || scope.kind == Scope::Kind::Switch);
            return scope.kind == Scope::Kind::Loop || scope.kind == Scope::Kind::Switch;
        case Deferred::Kind::Continue:
            if (scope.kind != Scope::Kind::Loop)
                return false;
            return label ? has_label : true;
        case Deferred::Kind::Return:
            return false;
        }
        return false;
    }

    // The way out of the scopes from the innermost of the first `count`
    // to the target of the command: each crossed environment popped, each
    // crossed for-of iterator closed (a break out of the loop itself
    // closes too; a continue does not), and at a finally the command is
    // deferred to the finally's own dispatch. For a return the value is
    // on the stack when this begins.
    void emit_exit_from(std::size_t count, Deferred::Kind kind, JsString* label)
    {
        for (std::size_t i = count; i-- > 0;) {
            Scope& scope = m_scopes[i];
            if (is_target(scope, kind, label)) {
                if (kind == Deferred::Kind::Break && scope.iterator_reg != None)
                    emit_iterator_close(scope);
                jump(Opcode::Jump, kind == Deferred::Kind::Break ? *scope.break_label : *scope.continue_label);
                return;
            }
            if (scope.kind == Scope::Kind::Finally) {
                if (kind == Deferred::Kind::Return)
                    emit(Opcode::StoreReg, scope.value_reg);
                // A return from inside an expression (a yield's return
                // resumption) leaves the expression's operands behind.
                while (!m_unreachable && m_depth > scope.depth)
                    emit(Opcode::Pop);
                std::uint32_t const token = 2 + static_cast<std::uint32_t>(scope.deferred.size());
                scope.deferred.push_back(Deferred { kind, label });
                emit(Opcode::PushInt, token);
                emit(Opcode::StoreReg, scope.token_reg);
                jump(Opcode::Jump, *scope.finally_entry);
                return;
            }
            if (scope.owns_env)
                emit(Opcode::PopEnv);
            if (scope.kind == Scope::Kind::Loop && scope.iterator_reg != None)
                emit_iterator_close(scope);
        }
        if (kind == Deferred::Kind::Return) {
            emit(Opcode::Return);
            return;
        }
        fail("internal: a break or continue with no target");
    }

    void emit_exit(Deferred::Kind kind, JsString* label) { emit_exit_from(m_scopes.size(), kind, label); }

    // IteratorClose on a normal exit from a loop: return() runs, and its
    // failure is the outcome. An async iterator's answer is awaited and
    // must be an object (AsyncIteratorClose, §7.4.13).
    void emit_iterator_close(Scope const& scope)
    {
        if (!scope.async_iterator) {
            emit(Opcode::IteratorClose, scope.iterator_reg);
            return;
        }
        Label skip;
        emit(Opcode::IteratorReturnCall, scope.iterator_reg);
        emit(Opcode::Dup);
        jump(Opcode::JumpIfEmpty, skip);
        emit(Opcode::Await);
        compile_resume_dispatch(false);
        emit(Opcode::RequireIterResult);
        bind(skip);
        emit(Opcode::Pop);
    }

    // ---- statements -------------------------------------------------------

    void compile_statements(std::span<Statement* const> statements)
    {
        for (Statement const* statement : statements)
            compile_statement(statement, {});
    }

    // ExportDeclaration evaluation (§16.2.3.7), as the tree-walker has it:
    // a declaration is compiled as itself, a default expression is
    // compiled — named "default" when it is an anonymous function or
    // class — and bound to `*default*`, a default function was
    // instantiated when the module was linked, and the named and star
    // forms did their work then too. Only a module body with a top-level
    // await reaches here: it is the one module code the VM runs.
    void compile_export(ExportDeclaration const& declaration)
    {
        switch (declaration.kind) {
        case ExportDeclaration::Kind::Declaration:
            compile_statement(declaration.declaration, {});
            return;
        case ExportDeclaration::Kind::Default:
            if (declaration.declaration != nullptr) {
                if (declaration.declaration->type != NodeType::FunctionDeclaration)
                    compile_statement(declaration.declaration, {});
                return;
            }
            compile_named_value(declaration.expression, m_heap.atom(u"default"));
            emit(Opcode::InitializeBinding, name(m_heap.atom(u"*default*")));
            return;
        case ExportDeclaration::Kind::Named:
        case ExportDeclaration::Kind::Star:
            return;
        }
    }

    void compile_statement(Statement const* statement, std::vector<JsString*> labels)
    {
        if (!m_error.empty())
            return;
        switch (statement->type) {
        case NodeType::VariableDeclaration:
            compile_declaration(*static_cast<VariableDeclaration const*>(statement));
            return;
        case NodeType::FunctionDeclaration: {
            auto const& declaration = *static_cast<FunctionDeclaration const*>(statement);
            if (declaration.annex_b_hoisted)
                emit(Opcode::AnnexBCopy, name(declaration.function->name));
            return;
        }
        case NodeType::ClassDeclaration: {
            auto const& declaration = *static_cast<ClassDeclaration const*>(statement);
            // An anonymous `export default class` is named "default" and
            // binds `*default*` (§16.2.3.7), as execute_class_declaration
            // has it.
            bool const anonymous = declaration.node->name == nullptr;
            emit(Opcode::MakeClass, class_node(declaration.node), anonymous ? name(m_heap.atom(u"default")) : None);
            emit(Opcode::InitializeBinding, name(anonymous ? m_heap.atom(u"*default*") : declaration.node->name));
            return;
        }
        case NodeType::ImportDeclaration:
            // §16.2.2.7: the bindings were made when the module was linked.
            return;
        case NodeType::ExportDeclaration:
            compile_export(*static_cast<ExportDeclaration const*>(statement));
            return;
        case NodeType::ExpressionStatement:
            compile_expression(static_cast<ExpressionStatement const*>(statement)->expression);
            emit(Opcode::Pop);
            return;
        case NodeType::BlockStatement:
            compile_block(*static_cast<BlockStatement const*>(statement));
            return;
        case NodeType::EmptyStatement:
        case NodeType::DebuggerStatement:
            return;
        case NodeType::IfStatement: {
            auto const& branch = *static_cast<IfStatement const*>(statement);
            Label otherwise;
            Label end;
            compile_expression(branch.test);
            jump(Opcode::JumpIfFalse, otherwise);
            compile_statement(branch.consequent, {});
            if (branch.alternate) {
                jump(Opcode::Jump, end);
                bind(otherwise);
                compile_statement(branch.alternate, {});
                bind(end);
            } else {
                bind(otherwise);
            }
            return;
        }
        case NodeType::ForStatement:
            compile_for(*static_cast<ForStatement const*>(statement), std::move(labels));
            return;
        case NodeType::ForInStatement:
            compile_for_in(*static_cast<ForInStatement const*>(statement), std::move(labels));
            return;
        case NodeType::ForOfStatement:
            compile_for_of(*static_cast<ForOfStatement const*>(statement), std::move(labels));
            return;
        case NodeType::WhileStatement:
            compile_while(*static_cast<WhileStatement const*>(statement), std::move(labels));
            return;
        case NodeType::DoWhileStatement:
            compile_do_while(*static_cast<DoWhileStatement const*>(statement), std::move(labels));
            return;
        case NodeType::ReturnStatement: {
            auto const& ret = *static_cast<ReturnStatement const*>(statement);
            if (ret.argument)
                compile_expression(ret.argument);
            else
                emit(Opcode::PushUndefined);
            emit_exit(Deferred::Kind::Return, nullptr);
            return;
        }
        case NodeType::BreakStatement:
            emit_exit(Deferred::Kind::Break, static_cast<BreakStatement const*>(statement)->label);
            return;
        case NodeType::ContinueStatement:
            emit_exit(Deferred::Kind::Continue, static_cast<ContinueStatement const*>(statement)->label);
            return;
        case NodeType::ThrowStatement:
            compile_expression(static_cast<ThrowStatement const*>(statement)->argument);
            emit(Opcode::Throw);
            return;
        case NodeType::TryStatement:
            compile_try(*static_cast<TryStatement const*>(statement));
            return;
        case NodeType::SwitchStatement:
            compile_switch(*static_cast<SwitchStatement const*>(statement), std::move(labels));
            return;
        case NodeType::LabeledStatement: {
            // LabelledEvaluation (§14.13.4): the label joins the set a loop
            // or switch directly inside takes; any other body gets a scope
            // whose only exit is `break L`.
            auto const& labelled = *static_cast<LabeledStatement const*>(statement);
            labels.push_back(labelled.label);
            NodeType const body = labelled.body->type;
            bool const takes_labels = body == NodeType::ForStatement || body == NodeType::ForInStatement
                || body == NodeType::ForOfStatement || body == NodeType::WhileStatement || body == NodeType::DoWhileStatement
                || body == NodeType::SwitchStatement || body == NodeType::LabeledStatement;
            if (takes_labels) {
                compile_statement(labelled.body, std::move(labels));
                return;
            }
            Label end;
            Scope scope { Scope::Kind::Label };
            scope.labels = std::move(labels);
            scope.break_label = &end;
            push_scope(scope);
            compile_statement(labelled.body, {});
            pop_scope();
            bind(end);
            return;
        }
        case NodeType::WithStatement: {
            auto const& with = *static_cast<WithStatement const*>(statement);
            compile_expression(with.object);
            emit(Opcode::PushWithEnv);
            Scope scope { Scope::Kind::With };
            scope.owns_env = true;
            push_scope(scope);
            compile_statement(with.body, {});
            pop_scope();
            emit(Opcode::PopEnv);
            return;
        }
        default:
            break;
        }
        fail("internal: a statement the compiler does not know");
    }

    void compile_block(BlockStatement const& block)
    {
        // §14.2.2: a block with declarations gets an environment of its own.
        bool const env = !block.declarations.lexicals.empty() || !block.declarations.functions.empty();
        if (!env) {
            compile_statements(block.body);
            return;
        }
        emit(Opcode::PushBlockEnv, declarations(&block.declarations));
        Scope scope { Scope::Kind::Block };
        scope.owns_env = true;
        push_scope(scope);
        compile_statements(block.body);
        pop_scope();
        emit(Opcode::PopEnv);
    }

    void compile_declaration(VariableDeclaration const& declaration)
    {
        bool const is_var = declaration.kind == VariableDeclaration::Kind::Var;
        for (VariableDeclarator const& declarator : declaration.declarations) {
            if (declarator.pattern) {
                if (declarator.init)
                    compile_expression(declarator.init);
                else
                    emit(Opcode::PushUndefined);
                compile_pattern(declarator.pattern, is_var ? BindMode::VarAssign : BindMode::Initialize);
                continue;
            }
            if (is_var) {
                // §14.3.2.1: a var with an initializer assigns through a
                // reference resolved first, so a `with` in scope can catch it.
                if (!declarator.init)
                    continue;
                emit(Opcode::RefName, name(declarator.name));
                compile_named_value(declarator.init, declarator.name);
                emit(Opcode::RefPut);
                continue;
            }
            if (declarator.init)
                compile_named_value(declarator.init, declarator.name);
            else
                emit(Opcode::PushUndefined);
            emit(Opcode::InitializeBinding, name(declarator.name));
        }
    }

    Scope loop_scope(std::vector<JsString*> labels, Label& end, Label& next, std::uint32_t iterator_reg = None,
        bool async_iterator = false)
    {
        Scope scope { Scope::Kind::Loop };
        scope.labels = std::move(labels);
        scope.break_label = &end;
        scope.continue_label = &next;
        scope.iterator_reg = iterator_reg;
        scope.async_iterator = async_iterator;
        return scope;
    }

    void compile_while(WhileStatement const& loop, std::vector<JsString*> labels)
    {
        Label top;
        Label end;
        Label next;
        bind(top);
        compile_expression(loop.test);
        jump(Opcode::JumpIfFalse, end);
        push_scope(loop_scope(std::move(labels), end, next));
        compile_statement(loop.body, {});
        pop_scope();
        bind(next);
        emit(Opcode::Step);
        jump(Opcode::Jump, top);
        bind(end);
    }

    void compile_do_while(DoWhileStatement const& loop, std::vector<JsString*> labels)
    {
        Label top;
        Label end;
        Label next;
        bind(top);
        push_scope(loop_scope(std::move(labels), end, next));
        compile_statement(loop.body, {});
        pop_scope();
        bind(next);
        compile_expression(loop.test);
        jump(Opcode::JumpIfFalse, end);
        emit(Opcode::Step);
        jump(Opcode::Jump, top);
        bind(end);
    }

    void compile_for(ForStatement const& loop, std::vector<JsString*> labels)
    {
        // §14.7.4.2, §14.7.4.3 ForBodyEvaluation: a let/const head gets a
        // scope around the whole statement, copied per iteration so that
        // closures keep their iteration's values.
        bool const lexical = !loop.declarations.lexicals.empty();
        std::vector<JsString*> per_iteration;
        if (lexical) {
            emit(Opcode::PushBlockEnv, declarations(&loop.declarations));
            for (auto const& [name_atom, is_const] : loop.declarations.lexicals) {
                if (!is_const)
                    per_iteration.push_back(name_atom);
            }
            Scope scope { Scope::Kind::Block };
            scope.owns_env = true;
            push_scope(scope);
        }
        std::uint32_t const copies = per_iteration.empty() ? None : name_list(per_iteration);
        if (loop.init)
            compile_statement(loop.init, {});
        if (copies != None)
            emit(Opcode::CopyIterationEnv, copies);
        Label top;
        Label end;
        Label next;
        bind(top);
        if (loop.test) {
            compile_expression(loop.test);
            jump(Opcode::JumpIfFalse, end);
        }
        push_scope(loop_scope(std::move(labels), end, next));
        compile_statement(loop.body, {});
        pop_scope();
        bind(next);
        if (copies != None)
            emit(Opcode::CopyIterationEnv, copies);
        if (loop.update) {
            compile_expression(loop.update);
            emit(Opcode::Pop);
        }
        emit(Opcode::Step);
        jump(Opcode::Jump, top);
        bind(end);
        if (lexical) {
            pop_scope();
            emit(Opcode::PopEnv);
        }
    }

    // The head of a for-in/of loop binds the iteration's value (§14.7.5.7
    // steps 6.g–6.i), with the value on the stack: a let/const gets a
    // fresh environment for the body — pushed here as a Block scope the
    // caller pops after the body; a var or an expression assigns through a
    // reference; a pattern destructures.
    bool compile_loop_head_binding(VariableDeclaration const* declaration, Expression const* target)
    {
        if (declaration && declaration->kind != VariableDeclaration::Kind::Var) {
            VariableDeclarator const& declarator = declaration->declarations[0];
            std::vector<JsString*> names;
            Interpreter::Impl::collect_bound_names(declarator.name, declarator.pattern, names);
            emit(Opcode::PushNamesEnv, name_list(names), declaration->kind == VariableDeclaration::Kind::Const ? 0u : 1u);
            Scope scope { Scope::Kind::Block };
            scope.owns_env = true;
            push_scope(scope);
            if (declarator.pattern)
                compile_pattern(declarator.pattern, BindMode::Initialize);
            else
                emit(Opcode::InitializeBinding, name(declarator.name));
            return true;
        }
        if (declaration) {
            VariableDeclarator const& declarator = declaration->declarations[0];
            if (declarator.pattern) {
                compile_pattern(declarator.pattern, BindMode::VarAssign);
            } else {
                emit(Opcode::RefName, name(declarator.name));
                emit(Opcode::RefPut);
            }
            return false;
        }
        if (is_pattern(target)) {
            compile_pattern(target, BindMode::Assign);
            return false;
        }
        compile_reference(target);
        emit(Opcode::RefPut);
        return false;
    }

    // The names a let/const head declares, in their dead zone around the
    // head's own expression (§14.7.5.6).
    std::vector<JsString*> head_names(VariableDeclaration const* declaration)
    {
        std::vector<JsString*> names;
        if (declaration)
            Interpreter::Impl::collect_bound_names(declaration->declarations[0].name, declaration->declarations[0].pattern, names);
        return names;
    }

    void compile_for_in(ForInStatement const& loop, std::vector<JsString*> labels)
    {
        bool const lexical = loop.declaration && loop.declaration->kind != VariableDeclaration::Kind::Var;
        // B.3.5: `for (var x = 1 in o)` assigns the initializer first.
        if (loop.declaration && loop.declaration->kind == VariableDeclaration::Kind::Var && loop.declaration->declarations[0].init)
            compile_declaration(*loop.declaration);
        if (lexical) {
            emit(Opcode::PushNamesEnv, name_list(head_names(loop.declaration)), 1);
            Scope scope { Scope::Kind::Block };
            scope.owns_env = true;
            push_scope(scope);
        }
        compile_expression(loop.object);
        if (lexical) {
            pop_scope();
            emit(Opcode::PopEnv);
        }
        std::uint32_t const iterator = new_register();
        emit(Opcode::ForInStart);
        emit(Opcode::StoreReg, iterator);
        Label top;
        Label end;
        Label end_pop;
        Label next;
        bind(top);
        emit(Opcode::ForInNext, iterator);
        // The key stays for the head binding; exhaustion drops it at
        // end_pop, so a break reaches `end` at the same depth.
        emit(Opcode::Dup);
        jump(Opcode::JumpIfEmpty, end_pop);
        push_scope(loop_scope(std::move(labels), end, next));
        bool const pushed = compile_loop_head_binding(loop.declaration, loop.target);
        compile_statement(loop.body, {});
        if (pushed) {
            pop_scope();
            emit(Opcode::PopEnv);
        }
        pop_scope();
        bind(next);
        emit(Opcode::Step);
        jump(Opcode::Jump, top);
        bind(end_pop);
        emit(Opcode::Pop);
        bind(end);
    }

    void compile_for_of(ForOfStatement const& loop, std::vector<JsString*> labels)
    {
        // §14.7.5.6 (iterate) and §14.7.5.7: the iterator is stepped to
        // its end; a body that leaves the loop any other way closes it,
        // a throw from the body closing it with the throw kept. A `for
        // await` (§14.7.5.6 async-iterate) awaits each step's answer,
        // which must then be an iterator result, and awaits the close.
        bool const async_loop = loop.is_await;
        bool const lexical = loop.declaration && loop.declaration->kind != VariableDeclaration::Kind::Var;
        if (lexical) {
            emit(Opcode::PushNamesEnv, name_list(head_names(loop.declaration)), 1);
            Scope scope { Scope::Kind::Block };
            scope.owns_env = true;
            push_scope(scope);
        }
        compile_expression(loop.iterable);
        if (lexical) {
            pop_scope();
            emit(Opcode::PopEnv);
        }
        std::uint32_t const iterator = new_register();
        new_register(); // the next method
        new_register(); // done — a for-of never steps past exhaustion, so it stays unset
        emit(async_loop ? Opcode::GetAsyncIterator : Opcode::GetIterator);
        emit(Opcode::StoreReg, iterator + 1);
        emit(Opcode::StoreReg, iterator);
        Label top;
        Label end;
        Label end_pop;
        Label next;
        bind(top);
        if (async_loop) {
            emit(Opcode::IteratorNextCall, iterator);
            emit(Opcode::Await);
            compile_resume_dispatch(false);
            emit(Opcode::RequireIterResult);
        } else {
            emit(Opcode::IteratorNext, iterator);
        }
        emit(Opcode::Dup);
        emit(Opcode::IteratorResultDone);
        jump(Opcode::JumpIfTrue, end_pop);
        emit(Opcode::IteratorResultValue);
        // The head binding and the body are protected: a throw closes the
        // iterator, and the throw stays the outcome.
        std::uint32_t const protected_start = here();
        int const depth_at_start = m_depth - 1; // the binding consumes the value
        int const refs_at_start = m_refs;
        std::uint32_t const envs_at_start = env_depth();
        push_scope(loop_scope(std::move(labels), end, next, iterator, async_loop));
        bool const pushed = compile_loop_head_binding(loop.declaration, loop.target);
        compile_statement(loop.body, {});
        if (pushed) {
            pop_scope();
            emit(Opcode::PopEnv);
        }
        pop_scope();
        std::uint32_t const protected_end = here();
        bind(next);
        emit(Opcode::Step);
        jump(Opcode::Jump, top);
        std::uint32_t const handler = here();
        land(depth_at_start + 1, refs_at_start);
        if (async_loop) {
            // AsyncIteratorClose under a throw: return() is called and its
            // answer awaited, and whatever either does, the throw wins.
            Label done;
            emit(Opcode::IteratorReturnCallQuiet, iterator);
            emit(Opcode::Dup);
            jump(Opcode::JumpIfEmpty, done);
            emit(Opcode::Await);
            jump(Opcode::JumpIfResumeNormal, done);
            bind(done);
            emit(Opcode::Pop);
        } else {
            emit(Opcode::IteratorCloseThrowing, iterator);
        }
        emit(Opcode::Throw);
        add_handler(protected_start, protected_end, handler, depth_at_start, refs_at_start, envs_at_start);
        bind(end_pop);
        emit(Opcode::Pop);
        bind(end);
    }

    void compile_try(TryStatement const& statement)
    {
        // §14.15.3. The finally, when there is one, is entered with a
        // token: 0 fell through, 1 rethrows the value register, 2 and up
        // continue a break, continue or return that crossed it.
        bool const has_finally = statement.finalizer != nullptr;
        Label finally_entry;
        std::uint32_t token_reg = 0;
        std::uint32_t value_reg = 0;
        if (has_finally) {
            token_reg = new_register();
            value_reg = new_register();
            Scope scope { Scope::Kind::Finally };
            scope.token_reg = token_reg;
            scope.value_reg = value_reg;
            scope.finally_entry = &finally_entry;
            push_scope(scope);
        }
        int const depth_at_start = m_depth;
        int const refs_at_start = m_refs;
        std::uint32_t const envs_at_start = env_depth();
        std::uint32_t const try_start = here();
        compile_block(*statement.block);
        std::uint32_t const try_end = here();
        if (statement.handler) {
            Label after_catch;
            jump(Opcode::Jump, after_catch);
            std::uint32_t const catch_entry = here();
            land(depth_at_start + 1, refs_at_start);
            add_handler(try_start, try_end, catch_entry, depth_at_start, refs_at_start, envs_at_start);
            if (statement.catch_pattern) {
                std::vector<JsString*> names;
                Interpreter::Impl::collect_bound_names(statement.catch_pattern, names);
                emit(Opcode::PushNamesEnv, name_list(names), 1);
                Scope scope { Scope::Kind::Block };
                scope.owns_env = true;
                push_scope(scope);
                compile_pattern(statement.catch_pattern, BindMode::Initialize);
                compile_block(*statement.handler);
                pop_scope();
                emit(Opcode::PopEnv);
            } else if (statement.catch_parameter) {
                emit(Opcode::PushNamesEnv, name_list({ statement.catch_parameter }), 1);
                Scope scope { Scope::Kind::Block };
                scope.owns_env = true;
                push_scope(scope);
                emit(Opcode::InitializeBinding, name(statement.catch_parameter));
                compile_block(*statement.handler);
                pop_scope();
                emit(Opcode::PopEnv);
            } else {
                emit(Opcode::Pop);
                compile_block(*statement.handler);
            }
            bind(after_catch);
        }
        if (!has_finally)
            return;
        std::uint32_t const catch_end = here();
        Scope const finally_scope = m_scopes.back();
        pop_scope();
        // Fell through: token 0.
        emit(Opcode::PushInt, 0);
        emit(Opcode::StoreReg, token_reg);
        jump(Opcode::Jump, finally_entry);
        // Thrown: the value kept, token 1.
        std::uint32_t const finally_handler = here();
        land(depth_at_start + 1, refs_at_start);
        add_handler(try_start, catch_end, finally_handler, depth_at_start, refs_at_start, envs_at_start);
        emit(Opcode::StoreReg, value_reg);
        emit(Opcode::PushInt, 1);
        emit(Opcode::StoreReg, token_reg);
        jump(Opcode::Jump, finally_entry);
        bind(finally_entry);
        compile_block(*statement.finalizer);
        // The dispatch on the token.
        std::uint32_t const table = new_jump_table();
        emit(Opcode::Switch, token_reg, table);
        Label after;
        m_code->jump_tables[table].push_back(here());
        jump(Opcode::Jump, after);
        m_code->jump_tables[table].push_back(here());
        land(depth_at_start, refs_at_start);
        emit(Opcode::LoadReg, value_reg);
        emit(Opcode::Throw);
        for (Deferred const& command : finally_scope.deferred) {
            m_code->jump_tables[table].push_back(here());
            land(depth_at_start, refs_at_start);
            if (command.kind == Deferred::Kind::Return)
                emit(Opcode::LoadReg, value_reg);
            emit_exit(command.kind, command.label);
        }
        bind(after);
    }

    void compile_switch(SwitchStatement const& statement, std::vector<JsString*> labels)
    {
        // §14.12.4 CaseBlockEvaluation: the clauses before the default are
        // tried in order, then the ones after it; a match falls through
        // everything below it, the default included.
        compile_expression(statement.discriminant);
        std::uint32_t const discriminant = new_register();
        emit(Opcode::StoreReg, discriminant);
        bool const env = !statement.declarations.lexicals.empty() || !statement.declarations.functions.empty();
        if (env) {
            emit(Opcode::PushBlockEnv, declarations(&statement.declarations));
            Scope scope { Scope::Kind::Block };
            scope.owns_env = true;
            push_scope(scope);
        }
        Label end;
        Scope scope { Scope::Kind::Switch };
        scope.labels = std::move(labels);
        scope.break_label = &end;
        push_scope(scope);
        std::vector<Label> bodies(statement.cases.size());
        std::size_t default_index = statement.cases.size();
        for (std::size_t i = 0; i < statement.cases.size(); ++i) {
            if (statement.cases[i].test == nullptr) {
                default_index = i;
                continue;
            }
            emit(Opcode::LoadReg, discriminant);
            compile_expression(statement.cases[i].test);
            emit(Opcode::Binary, static_cast<std::uint32_t>(BinaryOp::StrictEqual));
            jump(Opcode::JumpIfTrue, bodies[i]);
        }
        if (default_index < statement.cases.size())
            jump(Opcode::Jump, bodies[default_index]);
        else
            jump(Opcode::Jump, end);
        for (std::size_t i = 0; i < statement.cases.size(); ++i) {
            bind(bodies[i]);
            compile_statements(statement.cases[i].consequent);
        }
        pop_scope();
        bind(end);
        if (env) {
            pop_scope();
            emit(Opcode::PopEnv);
        }
    }

    // ---- expressions ------------------------------------------------------

    // NamedEvaluation (§8.4.5): an anonymous function or class takes the
    // name it is bound to; anything else is just evaluated.
    void compile_named_value(Expression const* value, JsString* binding_name)
    {
        if (is_anonymous_function_definition(value))
            compile_function_value(value, name(binding_name));
        else
            compile_expression(value);
    }

    void compile_function_value(Expression const* value, std::uint32_t name_index)
    {
        switch (value->type) {
        case NodeType::FunctionExpression:
            emit(Opcode::MakeClosure, function(static_cast<FunctionExpression const*>(value)->function), name_index);
            return;
        case NodeType::ArrowFunction:
            emit(Opcode::MakeClosure, function(static_cast<ArrowFunction const*>(value)->function), name_index);
            return;
        case NodeType::ClassExpression:
            emit(Opcode::MakeClass, class_node(static_cast<ClassExpression const*>(value)->node), name_index);
            return;
        default:
            compile_expression(value);
        }
    }

    // The same with the name on the stack (a computed key): the key is
    // consumed, so the caller duplicates it first when it needs it again.
    void compile_function_value_dyn(Expression const* value)
    {
        switch (value->type) {
        case NodeType::FunctionExpression:
            emit(Opcode::MakeClosureNamedDyn, function(static_cast<FunctionExpression const*>(value)->function));
            return;
        case NodeType::ArrowFunction:
            emit(Opcode::MakeClosureNamedDyn, function(static_cast<ArrowFunction const*>(value)->function));
            return;
        case NodeType::ClassExpression:
            emit(Opcode::MakeClassNamedDyn, class_node(static_cast<ClassExpression const*>(value)->node));
            return;
        default:
            fail("internal: a dynamic name for something that is not a function");
        }
    }

    void compile_expression(Expression const* expression)
    {
        if (!m_error.empty())
            return;
        switch (expression->type) {
        case NodeType::Identifier:
            emit(Opcode::GetName, name(static_cast<Identifier const*>(expression)->name));
            return;
        case NodeType::NumberLiteral:
            emit(Opcode::PushConstant, constant(Value::number(static_cast<NumberLiteral const*>(expression)->value)));
            return;
        case NodeType::BigIntLiteral:
            emit(Opcode::PushBigInt, bigint(static_cast<BigIntLiteral const*>(expression)->value));
            return;
        case NodeType::StringLiteral:
            emit(Opcode::PushConstant, constant(Value::string(static_cast<StringLiteral const*>(expression)->value)));
            return;
        case NodeType::BooleanLiteral:
            emit(static_cast<BooleanLiteral const*>(expression)->value ? Opcode::PushTrue : Opcode::PushFalse);
            return;
        case NodeType::NullLiteral:
            emit(Opcode::PushNull);
            return;
        case NodeType::ThisExpression:
            emit(Opcode::ResolveThis);
            return;
        case NodeType::RegExpLiteral:
            emit(Opcode::NewRegExp, regexp(static_cast<RegExpLiteral const*>(expression)));
            return;
        case NodeType::TemplateLiteral:
            compile_template(*static_cast<TemplateLiteral const*>(expression));
            return;
        case NodeType::TaggedTemplate:
            compile_tagged_template(*static_cast<TaggedTemplate const*>(expression));
            return;
        case NodeType::ArrayLiteral:
            compile_array(*static_cast<ArrayLiteral const*>(expression));
            return;
        case NodeType::ObjectLiteral:
            compile_object(*static_cast<ObjectLiteral const*>(expression));
            return;
        case NodeType::FunctionExpression:
        case NodeType::ArrowFunction:
        case NodeType::ClassExpression:
            compile_function_value(expression, None);
            return;
        case NodeType::SuperMember:
            compile_super_reference(*static_cast<SuperMember const*>(expression));
            emit(Opcode::RefGet);
            emit(Opcode::RefDrop);
            return;
        case NodeType::SuperCall: {
            auto const& call = *static_cast<SuperCall const*>(expression);
            bool spread = false;
            std::uint32_t argc = 0;
            compile_arguments(call.arguments, spread, argc);
            if (spread) {
                emit(Opcode::SuperCallArray);
            } else {
                emit(Opcode::SuperCall, argc);
                adjust(1 - static_cast<int>(argc));
            }
            return;
        }
        case NodeType::NewTargetExpression:
            emit(Opcode::NewTarget);
            return;
        case NodeType::UnaryExpression:
            compile_unary(*static_cast<UnaryExpression const*>(expression));
            return;
        case NodeType::UpdateExpression:
            compile_update(*static_cast<UpdateExpression const*>(expression));
            return;
        case NodeType::BinaryExpression: {
            auto const& binary = *static_cast<BinaryExpression const*>(expression);
            compile_expression(binary.left);
            compile_expression(binary.right);
            emit(Opcode::Binary, static_cast<std::uint32_t>(binary.op));
            return;
        }
        case NodeType::LogicalExpression:
            compile_logical(*static_cast<LogicalExpression const*>(expression));
            return;
        case NodeType::AssignmentExpression:
            compile_assignment(*static_cast<AssignmentExpression const*>(expression));
            return;
        case NodeType::ConditionalExpression: {
            auto const& conditional = *static_cast<ConditionalExpression const*>(expression);
            Label otherwise;
            Label end;
            compile_expression(conditional.test);
            jump(Opcode::JumpIfFalse, otherwise);
            compile_expression(conditional.consequent);
            jump(Opcode::Jump, end);
            bind(otherwise);
            compile_expression(conditional.alternate);
            bind(end);
            return;
        }
        case NodeType::CallExpression:
        case NodeType::MemberExpression: {
            ChainContext context;
            context.base_depth = m_depth;
            context.base_refs = m_refs;
            compile_chain(expression, context);
            finish_chain(context, 1);
            return;
        }
        case NodeType::NewExpression: {
            auto const& expr = *static_cast<NewExpression const*>(expression);
            compile_expression(expr.callee);
            bool spread = false;
            std::uint32_t argc = 0;
            compile_arguments(expr.arguments, spread, argc);
            if (spread) {
                emit(Opcode::NewArray, 0, node_index(expr.callee));
                adjust(-1);
            } else {
                emit(Opcode::New, argc, node_index(expr.callee));
                adjust(-static_cast<int>(argc));
            }
            return;
        }
        case NodeType::SequenceExpression: {
            auto const& sequence = *static_cast<SequenceExpression const*>(expression);
            for (std::size_t i = 0; i < sequence.expressions.size(); ++i) {
                if (i > 0)
                    emit(Opcode::Pop);
                compile_expression(sequence.expressions[i]);
            }
            return;
        }
        case NodeType::PrivateIn: {
            auto const& expr = *static_cast<PrivateInExpression const*>(expression);
            compile_expression(expr.right);
            emit(Opcode::PrivateIn, name(expr.name));
            return;
        }
        case NodeType::YieldExpression: {
            auto const& yield = *static_cast<YieldExpression const*>(expression);
            if (yield.delegate)
                compile_yield_star(yield);
            else
                compile_yield(yield);
            return;
        }
        case NodeType::AwaitExpression:
            compile_expression(static_cast<AwaitExpression const*>(expression)->argument);
            emit(Opcode::Await);
            compile_resume_dispatch(false);
            return;
        case NodeType::ImportCall: {
            // §13.3.10.1: the specifier and then the options — undefined
            // when the call gave none, which is what "absent" means to
            // the instruction — and the opcode does the rest.
            auto const& call = *static_cast<ImportCall const*>(expression);
            compile_expression(call.specifier);
            if (call.options != nullptr)
                compile_expression(call.options);
            else
                emit(Opcode::PushUndefined);
            emit(Opcode::ImportCall);
            return;
        }
        case NodeType::ImportMeta:
            emit(Opcode::ImportMeta);
            return;
        default:
            break;
        }
        fail("internal: an expression the compiler does not know");
    }

    // After a suspension the resume value is on the stack: a normal resume
    // goes on with it; a throw throws it; a return (generators only)
    // returns it through the enclosing finally blocks and iterator closes.
    void compile_resume_dispatch(bool may_return)
    {
        Label ok;
        jump(Opcode::JumpIfResumeNormal, ok);
        if (may_return) {
            Label ret;
            jump(Opcode::JumpIfResumeReturn, ret);
            emit(Opcode::Throw);
            bind(ret);
            emit_exit(Deferred::Kind::Return, nullptr);
        } else {
            emit(Opcode::Throw);
        }
        bind(ok);
    }

    void compile_yield(YieldExpression const& yield)
    {
        if (yield.argument)
            compile_expression(yield.argument);
        else
            emit(Opcode::PushUndefined);
        emit(Opcode::Yield);
        compile_resume_dispatch(true);
    }

    // yield* (§27.5.3.8): every result the inner iterator produces is
    // yielded as it is, and what the generator is resumed with — a value,
    // a throw, a return — is forwarded to the inner iterator's next, throw
    // or return method; the inner iterator's end is the expression's value.
    void compile_yield_star(YieldExpression const& yield)
    {
        compile_expression(yield.argument);
        std::uint32_t const iterator = new_register();
        new_register(); // next
        new_register(); // done (unused)
        std::uint32_t const received_kind = new_register(); // 0 normal, 1 throw, 2 return
        std::uint32_t const received_value = new_register();
        std::uint32_t const node = node_index(yield.argument);
        emit(Opcode::GetIterator);
        emit(Opcode::StoreReg, iterator + 1);
        emit(Opcode::StoreReg, iterator);
        emit(Opcode::PushUndefined);
        emit(Opcode::StoreReg, received_value);
        emit(Opcode::PushInt, 0);
        emit(Opcode::StoreReg, received_kind);
        Label loop;
        Label not_normal;
        Label is_return;
        Label got_result;
        Label yield_it;
        Label done_exit;
        bind(loop);
        emit(Opcode::LoadReg, received_kind);
        emit(Opcode::PushInt, 0);
        emit(Opcode::Binary, static_cast<std::uint32_t>(BinaryOp::StrictEqual));
        jump(Opcode::JumpIfFalse, not_normal);
        // next(received)
        emit(Opcode::LoadReg, iterator + 1);
        emit(Opcode::LoadReg, iterator);
        emit(Opcode::LoadReg, received_value);
        emit(Opcode::Call, 1, node);
        adjust(-2);
        jump(Opcode::Jump, got_result);
        bind(not_normal);
        emit(Opcode::LoadReg, received_kind);
        emit(Opcode::PushInt, 1);
        emit(Opcode::Binary, static_cast<std::uint32_t>(BinaryOp::StrictEqual));
        jump(Opcode::JumpIfFalse, is_return);
        {
            // throw(received): without a throw method the inner iterator is
            // closed and the protocol violation is a TypeError.
            Label no_method;
            emit(Opcode::LoadReg, iterator);
            emit(Opcode::GetMemberNamed, name_of(u"throw"));
            emit(Opcode::Dup);
            jump(Opcode::JumpIfNullish, no_method);
            emit(Opcode::LoadReg, iterator);
            emit(Opcode::LoadReg, received_value);
            emit(Opcode::Call, 1, node);
            adjust(-2);
            jump(Opcode::Jump, got_result);
            bind(no_method);
            emit(Opcode::Pop);
            emit(Opcode::IteratorClose, iterator);
            emit(Opcode::ThrowTypeErrorConst, constant_string(u"The iterator does not provide a 'throw' method"));
        }
        bind(is_return);
        {
            // return(received): without a return method the generator
            // returns the value; with one, a done result returns its value
            // and anything else is yielded on.
            Label no_method;
            emit(Opcode::LoadReg, iterator);
            emit(Opcode::GetMemberNamed, name_of(u"return"));
            emit(Opcode::Dup);
            jump(Opcode::JumpIfNullish, no_method);
            emit(Opcode::LoadReg, iterator);
            emit(Opcode::LoadReg, received_value);
            emit(Opcode::Call, 1, node);
            adjust(-2);
            emit(Opcode::Dup);
            emit(Opcode::IteratorResultDone);
            jump(Opcode::JumpIfFalse, yield_it);
            emit(Opcode::IteratorResultValue);
            emit_exit(Deferred::Kind::Return, nullptr);
            bind(no_method);
            emit(Opcode::Pop);
            emit(Opcode::LoadReg, received_value);
            emit_exit(Deferred::Kind::Return, nullptr);
        }
        bind(got_result);
        emit(Opcode::Dup);
        emit(Opcode::IteratorResultDone);
        jump(Opcode::JumpIfTrue, done_exit);
        bind(yield_it);
        emit(Opcode::Yield, 0, 0, 1);
        {
            Label resumed_normal;
            Label resumed_return;
            jump(Opcode::JumpIfResumeNormal, resumed_normal);
            jump(Opcode::JumpIfResumeReturn, resumed_return);
            emit(Opcode::StoreReg, received_value);
            emit(Opcode::PushInt, 1);
            emit(Opcode::StoreReg, received_kind);
            jump(Opcode::Jump, loop);
            bind(resumed_return);
            emit(Opcode::StoreReg, received_value);
            emit(Opcode::PushInt, 2);
            emit(Opcode::StoreReg, received_kind);
            jump(Opcode::Jump, loop);
            bind(resumed_normal);
            emit(Opcode::StoreReg, received_value);
            emit(Opcode::PushInt, 0);
            emit(Opcode::StoreReg, received_kind);
            jump(Opcode::Jump, loop);
        }
        bind(done_exit);
        emit(Opcode::IteratorResultValue);
    }

    void compile_template(TemplateLiteral const& literal)
    {
        // §13.2.8.6: the cooked spans with each substitution's ToString.
        emit(Opcode::PushConstant, constant(Value::string(literal.cooked[0])));
        for (std::size_t i = 0; i < literal.expressions.size(); ++i) {
            compile_expression(literal.expressions[i]);
            emit(Opcode::ToString);
            emit(Opcode::StringConcat);
            emit(Opcode::PushConstant, constant(Value::string(literal.cooked[i + 1])));
            emit(Opcode::StringConcat);
        }
    }

    void compile_tagged_template(TaggedTemplate const& tagged)
    {
        // §13.3.11.1: the tag as a callee, then the site's template object
        // and the substitutions as its arguments.
        ChainContext context;
        context.base_depth = m_depth;
        context.base_refs = m_refs;
        compile_callee(tagged.tag, context, false);
        finish_chain(context, 2);
        emit(Opcode::TemplateObject, template_index(tagged.quasi));
        for (Expression const* expression : tagged.quasi->expressions)
            compile_expression(expression);
        std::uint32_t const argc = 1 + static_cast<std::uint32_t>(tagged.quasi->expressions.size());
        emit(Opcode::Call, argc, node_index(tagged.tag));
        adjust(-static_cast<int>(argc) - 1);
    }

    void compile_array(ArrayLiteral const& literal)
    {
        emit(Opcode::NewArrayLiteral);
        for (Expression const* element : literal.elements) {
            if (element == nullptr) {
                emit(Opcode::ArrayHole);
                continue;
            }
            if (element->type == NodeType::SpreadElement) {
                compile_expression(static_cast<SpreadElement const*>(element)->argument);
                emit(Opcode::ArraySpread);
                continue;
            }
            compile_expression(element);
            emit(Opcode::ArrayPush);
        }
    }

    void compile_object(ObjectLiteral const& literal)
    {
        // PropertyDefinitionEvaluation (§13.2.5.5), in source order.
        emit(Opcode::NewObject);
        for (PropertyDefinition const& property : literal.properties) {
            if (property.is_proto) {
                compile_expression(property.value);
                emit(Opcode::SetPrototype);
                continue;
            }
            if (property.kind == PropertyDefinition::Kind::Spread) {
                compile_expression(property.value);
                emit(Opcode::CopyDataProperties);
                continue;
            }
            bool const is_method = property.kind == PropertyDefinition::Kind::Init && property.value->type == NodeType::FunctionExpression
                && static_cast<FunctionExpression const*>(property.value)->function->is_method;
            bool const is_accessor = property.kind == PropertyDefinition::Kind::Get || property.kind == PropertyDefinition::Kind::Set;
            std::uint8_t const accessor_flags = property.kind == PropertyDefinition::Kind::Set ? 1 : 0;
            if (property.computed_key) {
                compile_expression(property.computed_key);
                emit(Opcode::ToPropertyKey);
                if (is_accessor) {
                    emit(Opcode::DefineAccessorDyn, function(static_cast<FunctionExpression const*>(property.value)->function), 0, accessor_flags);
                } else if (is_method) {
                    emit(Opcode::DefineMethodDyn, function(static_cast<FunctionExpression const*>(property.value)->function));
                } else if (is_anonymous_function_definition(property.value)) {
                    emit(Opcode::Dup);
                    compile_function_value_dyn(property.value);
                    emit(Opcode::DefinePropertyDyn);
                } else {
                    compile_expression(property.value);
                    emit(Opcode::DefinePropertyDyn);
                }
                continue;
            }
            std::uint32_t const key = name(property.key);
            if (is_accessor) {
                emit(Opcode::DefineAccessor, function(static_cast<FunctionExpression const*>(property.value)->function), key, accessor_flags);
            } else if (is_method) {
                emit(Opcode::DefineMethod, function(static_cast<FunctionExpression const*>(property.value)->function), key);
            } else {
                if (is_anonymous_function_definition(property.value))
                    compile_function_value(property.value, key);
                else
                    compile_expression(property.value);
                emit(Opcode::DefinePropertyNamed, key);
            }
        }
    }

    // ---- references and chains -------------------------------------------

    // Pushes the reference an assignment, update or delete targets.
    void compile_reference(Expression const* target)
    {
        switch (target->type) {
        case NodeType::Identifier:
            emit(Opcode::RefName, name(static_cast<Identifier const*>(target)->name));
            return;
        case NodeType::MemberExpression: {
            auto const& member = *static_cast<MemberExpression const*>(target);
            compile_expression(member.object);
            if (member.is_private) {
                emit(Opcode::RefPrivate, name(member.name));
            } else if (member.property) {
                compile_expression(member.property);
                emit(Opcode::RefMember);
            } else {
                emit(Opcode::RefMemberNamed, name(member.name));
            }
            return;
        }
        case NodeType::SuperMember:
            compile_super_reference(*static_cast<SuperMember const*>(target));
            return;
        default:
            break;
        }
        fail("internal: a reference to something that is not a target");
    }

    void compile_super_reference(SuperMember const& member)
    {
        if (member.property) {
            compile_expression(member.property);
            emit(Opcode::RefSuper);
        } else {
            emit(Opcode::RefSuperNamed, name(member.name));
        }
    }

    // `Dup; JumpIfNullish fixup`: the short-circuit of an optional link,
    // recorded with what the landing must discard.
    void optional_check(ChainContext& context, Opcode dup, int pops, int ref_drops)
    {
        Fixup fixup;
        fixup.pops = pops;
        fixup.ref_drops = ref_drops;
        emit(dup);
        jump(Opcode::JumpIfNullish, fixup.label);
        context.fixups.push_back(std::move(fixup));
    }

    // A member or call as a link of a chain, leaving its value.
    void compile_chain(Expression const* expression, ChainContext& context)
    {
        if (expression->type == NodeType::CallExpression) {
            compile_call(*static_cast<CallExpression const*>(expression), context);
            return;
        }
        auto const& member = *static_cast<MemberExpression const*>(expression);
        compile_chain_base(member.object, context);
        if (member.optional)
            optional_check(context, Opcode::Dup, 1, 0);
        if (member.is_private) {
            emit(Opcode::RefPrivate, name(member.name));
            emit(Opcode::RefGet);
            emit(Opcode::RefDrop);
        } else if (member.property) {
            compile_expression(member.property);
            emit(Opcode::GetMember);
        } else {
            emit(Opcode::GetMemberNamed, name(member.name));
        }
    }

    // The object a member reads from: a link of the same chain when it is
    // a member or call that is not parenthesised (§13.3.9: parentheses end
    // an optional chain), else an expression of its own.
    void compile_chain_base(Expression const* object, ChainContext& context)
    {
        if ((object->type == NodeType::MemberExpression || object->type == NodeType::CallExpression) && !object->parenthesized)
            compile_chain(object, context);
        else
            compile_expression(object);
    }

    // The callee of a call, tagged template or optional call: leaves
    // [callee, this] with the `this` a member call gets from its base
    // (§13.3.6.2), undefined otherwise.
    void compile_callee(Expression const* callee, ChainContext& context, bool optional_call)
    {
        switch (callee->type) {
        case NodeType::MemberExpression: {
            auto const& member = *static_cast<MemberExpression const*>(callee);
            if (callee->parenthesized) {
                // `(o.m)()` keeps the reference (`this` is o) but ends any
                // optional chain inside: a short-circuit there yields an
                // undefined callee with an undefined this.
                ChainContext inner;
                inner.base_depth = m_depth;
                inner.base_refs = m_refs;
                compile_member_callee(member, inner);
                finish_chain(inner, 2);
            } else {
                compile_member_callee(member, context);
            }
            // [callee, this]: an optional call checks the callee under this.
            if (optional_call)
                optional_check(context, Opcode::Over, 2, 0);
            return;
        }
        case NodeType::CallExpression:
            if (callee->parenthesized)
                compile_expression(callee);
            else
                compile_call(*static_cast<CallExpression const*>(callee), context);
            if (optional_call)
                optional_check(context, Opcode::Dup, 1, 0);
            emit(Opcode::PushUndefined);
            return;
        case NodeType::Identifier:
            emit(Opcode::RefName, name(static_cast<Identifier const*>(callee)->name));
            emit(Opcode::RefGet);
            if (optional_call)
                optional_check(context, Opcode::Dup, 1, 1);
            emit(Opcode::RefThis);
            emit(Opcode::RefDrop);
            return;
        case NodeType::SuperMember:
            compile_super_reference(*static_cast<SuperMember const*>(callee));
            emit(Opcode::RefGet);
            if (optional_call)
                optional_check(context, Opcode::Dup, 1, 1);
            emit(Opcode::RefThis);
            emit(Opcode::RefDrop);
            return;
        default:
            compile_expression(callee);
            if (optional_call)
                optional_check(context, Opcode::Dup, 1, 0);
            emit(Opcode::PushUndefined);
            return;
        }
    }

    // [callee, this] for a member callee, its base a link of the chain.
    void compile_member_callee(MemberExpression const& member, ChainContext& context)
    {
        compile_chain_base(member.object, context);
        if (member.optional)
            optional_check(context, Opcode::Dup, 1, 0);
        if (member.is_private) {
            emit(Opcode::RefPrivate, name(member.name));
        } else if (member.property) {
            compile_expression(member.property);
            emit(Opcode::RefMember);
        } else {
            emit(Opcode::RefMemberNamed, name(member.name));
        }
        emit(Opcode::RefGet);
        emit(Opcode::RefThis);
        emit(Opcode::RefDrop);
    }

    void compile_call(CallExpression const& call, ChainContext& context)
    {
        compile_callee(call.callee, context, call.optional);
        bool spread = false;
        std::uint32_t argc = 0;
        compile_arguments(call.arguments, spread, argc);
        std::uint32_t const node = node_index(call.callee);
        if (spread) {
            emit(call.is_direct_eval ? Opcode::CallEvalArray : Opcode::CallArray, 0, node);
            adjust(-2);
        } else {
            emit(call.is_direct_eval ? Opcode::CallEval : Opcode::Call, argc, node);
            adjust(-static_cast<int>(argc) - 1);
        }
    }

    // The arguments of a call or construction: pushed one by one, or —
    // when any is a spread — gathered into one array.
    void compile_arguments(std::vector<Expression*> const& arguments, bool& spread, std::uint32_t& argc)
    {
        spread = std::any_of(arguments.begin(), arguments.end(), [](Expression const* e) { return e->type == NodeType::SpreadElement; });
        if (spread) {
            emit(Opcode::NewArrayLiteral);
            for (Expression const* argument : arguments) {
                if (argument->type == NodeType::SpreadElement) {
                    compile_expression(static_cast<SpreadElement const*>(argument)->argument);
                    emit(Opcode::ArraySpread);
                } else {
                    compile_expression(argument);
                    emit(Opcode::ArrayPush);
                }
            }
            argc = 0;
            return;
        }
        for (Expression const* argument : arguments)
            compile_expression(argument);
        argc = static_cast<std::uint32_t>(arguments.size());
    }

    // The end of a chain: the short-circuits land here on undefined (or
    // on [undefined, undefined] for a callee).
    void finish_chain(ChainContext& context, int pushes)
    {
        if (context.fixups.empty())
            return;
        Label end;
        jump(Opcode::Jump, end);
        for (Fixup& fixup : context.fixups) {
            bind(fixup.label);
            m_refs = context.base_refs + fixup.ref_drops;
            for (int i = 0; i < fixup.pops; ++i)
                emit(Opcode::Pop);
            for (int i = 0; i < fixup.ref_drops; ++i)
                emit(Opcode::RefDrop);
            for (int i = 0; i < pushes; ++i)
                emit(Opcode::PushUndefined);
            jump(Opcode::Jump, end);
        }
        bind(end);
        m_refs = context.base_refs;
    }

    // ---- operators --------------------------------------------------------

    void compile_unary(UnaryExpression const& unary)
    {
        switch (unary.op) {
        case UnaryOp::Typeof:
            if (unary.operand->type == NodeType::Identifier) {
                emit(Opcode::TypeofName, name(static_cast<Identifier const*>(unary.operand)->name));
                return;
            }
            compile_expression(unary.operand);
            emit(Opcode::Unary, static_cast<std::uint32_t>(UnaryOp::Typeof));
            return;
        case UnaryOp::Delete:
            switch (unary.operand->type) {
            case NodeType::Identifier:
            case NodeType::MemberExpression:
            case NodeType::SuperMember:
                compile_reference(unary.operand);
                emit(Opcode::RefDelete);
                return;
            default:
                compile_expression(unary.operand);
                emit(Opcode::Pop);
                emit(Opcode::PushTrue);
                return;
            }
        case UnaryOp::Void:
            compile_expression(unary.operand);
            emit(Opcode::Pop);
            emit(Opcode::PushUndefined);
            return;
        default:
            compile_expression(unary.operand);
            emit(Opcode::Unary, static_cast<std::uint32_t>(unary.op));
            return;
        }
    }

    void compile_update(UpdateExpression const& update)
    {
        // §13.4.2–§13.4.5: the old value as a number, the new one stored,
        // the prefix form yielding the new and the postfix the old.
        compile_reference(update.target);
        emit(Opcode::RefGet);
        emit(Opcode::ToNumeric);
        if (update.prefix) {
            emit(update.increment ? Opcode::Inc : Opcode::Dec);
            emit(Opcode::RefPutKeep);
        } else {
            emit(Opcode::Dup);
            emit(update.increment ? Opcode::Inc : Opcode::Dec);
            emit(Opcode::RefPut);
        }
    }

    void compile_logical(LogicalExpression const& logical)
    {
        Label end;
        compile_expression(logical.left);
        switch (logical.op) {
        case LogicalOp::And:
            jump(Opcode::JumpIfFalseKeep, end);
            break;
        case LogicalOp::Or:
            jump(Opcode::JumpIfTrueKeep, end);
            break;
        case LogicalOp::Nullish:
            jump(Opcode::JumpIfNotNullishKeep, end);
            break;
        }
        emit(Opcode::Pop);
        compile_expression(logical.right);
        bind(end);
    }

    static std::optional<BinaryOp> binary_for(AssignmentOp op)
    {
        switch (op) {
        case AssignmentOp::Add: return BinaryOp::Add;
        case AssignmentOp::Subtract: return BinaryOp::Subtract;
        case AssignmentOp::Multiply: return BinaryOp::Multiply;
        case AssignmentOp::Divide: return BinaryOp::Divide;
        case AssignmentOp::Remainder: return BinaryOp::Remainder;
        case AssignmentOp::Exponent: return BinaryOp::Exponent;
        case AssignmentOp::LeftShift: return BinaryOp::LeftShift;
        case AssignmentOp::RightShift: return BinaryOp::RightShift;
        case AssignmentOp::UnsignedRightShift: return BinaryOp::UnsignedRightShift;
        case AssignmentOp::BitwiseAnd: return BinaryOp::BitwiseAnd;
        case AssignmentOp::BitwiseOr: return BinaryOp::BitwiseOr;
        case AssignmentOp::BitwiseXor: return BinaryOp::BitwiseXor;
        default: return std::nullopt;
        }
    }

    void compile_assignment(AssignmentExpression const& assignment)
    {
        // §13.15.2: the target reference first, then the right-hand side,
        // then the store. A pattern takes the value first and names its
        // targets as it goes; the value stays as the expression's result.
        if (is_pattern(assignment.target)) {
            compile_expression(assignment.value);
            emit(Opcode::Dup);
            compile_pattern(assignment.target, BindMode::Assign);
            return;
        }
        compile_reference(assignment.target);
        JsString* const binding_name = assignment.target->type == NodeType::Identifier
            ? static_cast<Identifier const*>(assignment.target)->name
            : nullptr;
        auto compile_value = [&]() {
            if (binding_name)
                compile_named_value(assignment.value, binding_name);
            else
                compile_expression(assignment.value);
        };
        if (assignment.op == AssignmentOp::Assign) {
            compile_value();
            emit(Opcode::RefPutKeep);
            return;
        }
        emit(Opcode::RefGet);
        if (assignment.op == AssignmentOp::LogicalAnd || assignment.op == AssignmentOp::LogicalOr || assignment.op == AssignmentOp::Nullish) {
            // The right-hand side may not be evaluated at all; then the
            // current value is the result and the reference is dropped.
            Label skip;
            Label end;
            switch (assignment.op) {
            case AssignmentOp::LogicalAnd:
                jump(Opcode::JumpIfFalseKeep, skip);
                break;
            case AssignmentOp::LogicalOr:
                jump(Opcode::JumpIfTrueKeep, skip);
                break;
            default:
                jump(Opcode::JumpIfNotNullishKeep, skip);
                break;
            }
            emit(Opcode::Pop);
            compile_value();
            emit(Opcode::RefPutKeep);
            int const refs_after = m_refs;
            jump(Opcode::Jump, end);
            bind(skip);
            m_refs = refs_after + 1;
            emit(Opcode::RefDrop);
            bind(end);
            return;
        }
        compile_expression(assignment.value);
        emit(Opcode::Binary, static_cast<std::uint32_t>(*binary_for(assignment.op)));
        emit(Opcode::RefPutKeep);
    }

    // ---- patterns ---------------------------------------------------------

    // BindingInitialization / DestructuringAssignmentEvaluation (§8.6.2,
    // §13.15.5.2) of a pattern over the value on the stack, which it
    // consumes.
    void compile_pattern(Expression const* pattern, BindMode mode)
    {
        if (pattern->type == NodeType::ArrayPattern)
            compile_array_pattern(*static_cast<ArrayPattern const*>(pattern), mode);
        else
            compile_object_pattern(*static_cast<ObjectPattern const*>(pattern), mode);
    }

    // The reference a non-pattern target names, taken before its value is
    // read (§13.15.5.5, §8.6.3).
    void compile_target_prepare(Expression const* target, BindMode mode)
    {
        if (target == nullptr || is_pattern(target) || mode == BindMode::Initialize)
            return;
        compile_reference(target);
    }

    // The value on the stack, or its default when it is undefined — an
    // anonymous function default named after a plain target.
    void compile_default(Expression const* target, Expression const* initializer)
    {
        if (initializer == nullptr)
            return;
        Label skip;
        emit(Opcode::Dup);
        jump(Opcode::JumpIfNotUndefined, skip);
        emit(Opcode::Pop);
        if (target->type == NodeType::Identifier)
            compile_named_value(initializer, static_cast<Identifier const*>(target)->name);
        else
            compile_expression(initializer);
        bind(skip);
    }

    // Stores the value on the stack into the target, by mode.
    void compile_target_store(Expression const* target, BindMode mode)
    {
        if (is_pattern(target)) {
            compile_pattern(target, mode);
            return;
        }
        if (mode == BindMode::Initialize) {
            emit(Opcode::InitializeBinding, name(static_cast<Identifier const*>(target)->name));
            return;
        }
        emit(Opcode::RefPut);
    }

    void compile_array_pattern(ArrayPattern const& pattern, BindMode mode)
    {
        // §8.6.3, §13.15.5.5: one iterator step per element, an elision
        // discarding its step, the rest element taking what is left; the
        // iterator is closed when the pattern ends before it does, and on
        // any throw that is not the iterator's own.
        std::uint32_t const iterator = new_register();
        new_register(); // next
        new_register(); // done
        emit(Opcode::GetIterator);
        emit(Opcode::StoreReg, iterator + 1);
        emit(Opcode::StoreReg, iterator);
        emit(Opcode::PushFalse);
        emit(Opcode::StoreReg, iterator + 2);
        std::uint32_t const protected_start = here();
        int const depth_at_start = m_depth;
        int const refs_at_start = m_refs;
        std::uint32_t const envs_at_start = env_depth();
        for (PatternElement const& element : pattern.elements) {
            if (element.target == nullptr) {
                emit(Opcode::IteratorStep, iterator);
                emit(Opcode::Pop);
                continue;
            }
            compile_target_prepare(element.target, mode);
            emit(Opcode::IteratorStep, iterator);
            compile_default(element.target, element.initializer);
            compile_target_store(element.target, mode);
        }
        if (pattern.rest) {
            compile_target_prepare(pattern.rest, mode);
            emit(Opcode::IteratorRestArray, iterator);
            compile_target_store(pattern.rest, mode);
        }
        std::uint32_t const protected_end = here();
        emit(Opcode::IteratorClose, iterator);
        Label after;
        jump(Opcode::Jump, after);
        std::uint32_t const handler = here();
        land(depth_at_start + 1, refs_at_start);
        emit(Opcode::IteratorCloseThrowing, iterator);
        emit(Opcode::Throw);
        add_handler(protected_start, protected_end, handler, depth_at_start, refs_at_start, envs_at_start);
        bind(after);
    }

    void compile_object_pattern(ObjectPattern const& pattern, BindMode mode)
    {
        // §8.6.2, §13.15.5.3: RequireObjectCoercible, then one Get per
        // property — the key first, then the target's reference, then the
        // read — and a rest property as CopyDataProperties with the keys
        // already taken left out. The source stays on the stack throughout.
        emit(Opcode::RequireObjectCoercible);
        std::uint32_t const taken = pattern.rest ? new_register() : None;
        if (pattern.rest) {
            emit(Opcode::NewArrayLiteral);
            emit(Opcode::StoreReg, taken);
        }
        for (PatternProperty const& property : pattern.properties) {
            if (property.computed_key) {
                compile_expression(property.computed_key);
                emit(Opcode::ToPropertyKey);
                if (pattern.rest) {
                    emit(Opcode::Dup);
                    emit(Opcode::AppendToReg, taken);
                }
                compile_target_prepare(property.target, mode);
                // [source, key] → [source, source, key] → [source, value]
                emit(Opcode::Over);
                emit(Opcode::Swap);
                emit(Opcode::GetMember);
            } else {
                if (pattern.rest) {
                    emit(Opcode::PushConstant, constant(Value::string(property.key)));
                    emit(Opcode::AppendToReg, taken);
                }
                compile_target_prepare(property.target, mode);
                emit(Opcode::Dup);
                emit(Opcode::GetMemberNamed, name(property.key));
            }
            compile_default(property.target, property.initializer);
            compile_target_store(property.target, mode);
        }
        if (pattern.rest) {
            compile_target_prepare(pattern.rest, mode);
            emit(Opcode::NewObject);
            emit(Opcode::Over);
            emit(Opcode::CopyDataPropertiesExcluding, taken);
            compile_target_store(pattern.rest, mode);
        }
        emit(Opcode::Pop);
    }

    FunctionNode const& m_node;
    Heap& m_heap;
    std::unique_ptr<CodeBlock> m_code;
    std::vector<Scope> m_scopes;
    std::string m_error;
    int m_depth = 0;
    int m_refs = 0;
    bool m_unreachable = false;
};

} // namespace

std::unique_ptr<CodeBlock> compile_function_body(FunctionNode const& node, Heap& heap, std::string* error)
{
    Compiler compiler(node, heap);
    return compiler.compile(error);
}

// ---- disassembly ------------------------------------------------------

std::string disassemble(CodeBlock const& code)
{
    std::string out;
    auto name_text = [&](std::uint32_t index) -> std::string {
        if (index == None)
            return "-";
        if (index < code.names.size())
            return code.names[index]->to_utf8();
        return "?";
    };
    for (std::size_t pc = 0; pc < code.code.size(); ++pc) {
        Instruction const& ins = code.code[pc];
        out += std::to_string(pc);
        out += ": ";
        out += opcode_name(ins.op);
        switch (ins.op) {
        case Opcode::PushConstant: {
            out += ' ';
            Value const& value = code.constants[ins.a];
            if (value.is_number())
                out += number_to_utf8(value.as_number());
            else if (value.is_string())
                out += "\"" + value.as_string()->to_utf8() + "\"";
            break;
        }
        case Opcode::RefName:
        case Opcode::RefMemberNamed:
        case Opcode::RefSuperNamed:
        case Opcode::RefPrivate:
        case Opcode::GetName:
        case Opcode::TypeofName:
        case Opcode::GetMemberNamed:
        case Opcode::InitializeBinding:
        case Opcode::AnnexBCopy:
        case Opcode::DefinePropertyNamed:
        case Opcode::PrivateIn:
            out += " " + name_text(ins.a);
            break;
        case Opcode::MakeClosure:
        case Opcode::MakeClass:
        case Opcode::DefineMethod:
        case Opcode::DefineAccessor:
            out += " #" + std::to_string(ins.a) + " " + name_text(ins.b);
            if (ins.flags)
                out += " flags=" + std::to_string(ins.flags);
            break;
        case Opcode::Jump:
        case Opcode::JumpIfTrue:
        case Opcode::JumpIfFalse:
        case Opcode::JumpIfTrueKeep:
        case Opcode::JumpIfFalseKeep:
        case Opcode::JumpIfNullish:
        case Opcode::JumpIfNotNullishKeep:
        case Opcode::JumpIfNotUndefined:
        case Opcode::JumpIfEmpty:
        case Opcode::JumpIfResumeNormal:
        case Opcode::JumpIfResumeReturn:
            out += " ->" + std::to_string(ins.a);
            break;
        case Opcode::Binary:
        case Opcode::Unary:
        case Opcode::PushInt:
        case Opcode::LoadReg:
        case Opcode::StoreReg:
        case Opcode::StoreRegKeep:
        case Opcode::IteratorNext:
        case Opcode::IteratorStep:
        case Opcode::IteratorRestArray:
        case Opcode::IteratorClose:
        case Opcode::IteratorCloseThrowing:
        case Opcode::ForInNext:
        case Opcode::AppendToReg:
        case Opcode::CopyDataPropertiesExcluding:
        case Opcode::PushBlockEnv:
        case Opcode::NewRegExp:
        case Opcode::TemplateObject:
        case Opcode::MakeClosureNamedDyn:
        case Opcode::MakeClassNamedDyn:
        case Opcode::DefineMethodDyn:
        case Opcode::DefineAccessorDyn:
        case Opcode::ThrowTypeErrorConst:
            out += " " + std::to_string(ins.a);
            break;
        case Opcode::PushNamesEnv:
        case Opcode::CopyIterationEnv:
            out += " [";
            if (ins.a < code.name_lists.size()) {
                for (std::size_t i = 0; i < code.name_lists[ins.a].size(); ++i)
                    out += (i ? " " : "") + code.name_lists[ins.a][i]->to_utf8();
            }
            out += "]";
            if (ins.op == Opcode::PushNamesEnv)
                out += ins.b ? " mutable" : " const";
            break;
        case Opcode::Call:
        case Opcode::CallEval:
        case Opcode::New:
        case Opcode::SuperCall:
            out += " argc=" + std::to_string(ins.a);
            break;
        case Opcode::Switch:
            out += " r" + std::to_string(ins.a) + " table=" + std::to_string(ins.b);
            break;
        case Opcode::Yield:
            if (ins.flags)
                out += " delegate";
            break;
        default:
            break;
        }
        out += '\n';
    }
    for (Handler const& handler : code.handlers) {
        out += "handler [" + std::to_string(handler.start) + ", " + std::to_string(handler.end) + ") -> "
            + std::to_string(handler.target) + " depth=" + std::to_string(handler.stack_depth) + " refs="
            + std::to_string(handler.ref_depth) + " envs=" + std::to_string(handler.env_depth) + "\n";
    }
    for (std::size_t t = 0; t < code.jump_tables.size(); ++t) {
        out += "table " + std::to_string(t) + ":";
        for (std::uint32_t const target : code.jump_tables[t])
            out += " " + std::to_string(target);
        out += '\n';
    }
    out += "registers=" + std::to_string(code.register_count) + " max_stack=" + std::to_string(code.max_stack) + "\n";
    return out;
}

}
