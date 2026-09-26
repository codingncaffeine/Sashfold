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
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
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
        // A function the parser resolved and that has no direct eval and
        // no with: its bindings are registers and environment slots, the
        // registers first in the frame's file and the compiler's own after.
        // Anything else (a function with either, program code, a module's
        // body) is compiled by name, as its environments are made by name.
        if (node.scope != nullptr && !node.dynamic) {
            m_slots = true;
            m_code->slots = true;
            m_code->register_count = node.register_count;
            m_code->register_names.assign(node.register_count, nullptr);
            m_register_immutable.assign(node.register_count, false);
            for (ScopeInfo const* scope : node.scopes) {
                for (ScopeInfo::Binding const& binding : scope->bindings) {
                    if (binding.captured || binding.slot >= node.register_count)
                        continue;
                    m_code->register_names[binding.slot] = binding.name;
                    m_register_immutable[binding.slot] = is_immutable(*scope, binding);
                }
            }
        }
    }

    // A script's or an eval's body: its completion value (§8.4) is what
    // the run returns. A register carries it — every expression statement
    // stores its value there; a compound statement (if, a loop, switch,
    // try, with) starts it empty and, when nothing inside wrote it, leaves
    // undefined, which is UpdateEmpty(…, undefined); a finally block's own
    // statements write nothing, since its normal completion is discarded.
    void track_completion()
    {
        m_track_completion = true;
        m_completion = new_register();
    }

    // The parameter list as a block of its own (compile_parameter_list):
    // for each formal, the argument by index or the rest of them, the
    // default in the argument's place when it is undefined — named after
    // the parameter when it is an anonymous function — then the name or
    // the pattern initialized. The block returns undefined.
    std::unique_ptr<CodeBlock> compile_parameters(std::string* error)
    {
        m_code->is_generator = false;
        m_code->is_async = false;
        compile_formals();
        emit(Opcode::PushUndefined);
        emit(Opcode::Return);
        if (!m_error.empty()) {
            if (error)
                *error = m_error;
            return nullptr;
        }
        return std::move(m_code);
    }

    std::unique_ptr<CodeBlock> compile(std::string* error)
    {
        if (m_slots)
            compile_prologue();
        if (m_node.expression_body) {
            if (m_node.is_field_initializer && is_anonymous_function_definition(m_node.expression_body)) {
                // A field's anonymous function is named after the field
                // (§15.7.10 step 2.b): the key rides on the frame.
                emit(Opcode::LoadFieldKey);
                compile_function_value_dyn(m_node.expression_body);
            } else {
                compile_expression(m_node.expression_body);
            }
            emit(Opcode::Return);
        } else {
            compile_statements(m_node.body);
            if (!m_unreachable && m_track_completion) {
                Label empty;
                emit(Opcode::LoadReg, m_completion);
                emit(Opcode::Dup);
                jump(Opcode::JumpIfEmpty, empty);
                emit(Opcode::Return);
                bind(empty);
                emit(Opcode::Pop);
                emit(Opcode::PushUndefined);
                emit(Opcode::Return);
            } else if (!m_unreachable) {
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

    // A script's or an eval's completion value, tracked in a register
    // (see track_completion); off for a function body.
    bool m_track_completion = false;
    std::uint32_t m_completion = None;
    std::uint32_t m_class_depth = 0; // classes under construction at this point of the code

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
        case Opcode::RefLocal:
        case Opcode::RefScoped:
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
        // The function's own environments, pushed by the prologue, stay
        // for the whole body.
        std::uint32_t depth = m_base_envs;
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
        handler.class_depth = m_class_depth;
        m_code->handlers.push_back(handler);
    }

    // A class in the steps the machine takes (§15.7.14): its scope, the
    // heritage evaluated in it, each element with its computed key
    // evaluated in it and under the class's private names, then the
    // class finished. The scope's environment counts as one the body
    // owns, so a handler around the class unwinds it.
    void compile_class(ClassNode const& node, std::uint32_t name_index, bool dynamic_name)
    {
        std::uint32_t const index = class_node(&node);
        // Resolved code makes the class's scope an environment only when it
        // materializes (a function inside reads the name); otherwise the
        // class's functions close over what is current here, and a named
        // class keeps its own name in a register, in its dead zone until
        // the class is made (the heritage and the computed keys may read it).
        ScopeInfo const* const own = m_slots ? node.scope : nullptr;
        bool const without_environment = m_slots && (own == nullptr || !own->materializes);
        ScopeInfo::Binding const* const name_register
            = own != nullptr && !own->materializes && !own->bindings.empty() ? &own->bindings.front() : nullptr;
        if (name_register) {
            emit(Opcode::PushEmpty);
            emit(Opcode::StoreReg, name_register->slot);
        }
        std::uint8_t const flags = without_environment ? 1 : 0;
        if (dynamic_name)
            emit(Opcode::ClassScopeNamedDyn, index, 0, flags);
        else
            emit(Opcode::ClassScope, index, name_index, flags);
        if (own)
            m_chain.push_back(own);
        Scope scope { Scope::Kind::Block };
        scope.owns_env = true;
        push_scope(scope);
        ++m_class_depth;
        if (node.has_heritage) {
            compile_expression(node.heritage);
            emit(Opcode::ClassBeginHeritage);
        } else {
            emit(Opcode::ClassBegin);
        }
        for (std::size_t i = 0; i < node.elements.size(); ++i) {
            ClassElement const& element = node.elements[i];
            if (element.computed_key) {
                compile_expression(element.computed_key);
                emit(Opcode::ClassElementKeyed, static_cast<std::uint32_t>(i));
            } else {
                emit(Opcode::ClassElement, static_cast<std::uint32_t>(i));
            }
        }
        --m_class_depth;
        pop_scope();
        emit(Opcode::ClassFinish);
        if (own)
            m_chain.pop_back();
        if (name_register) {
            emit(Opcode::Dup);
            emit(Opcode::StoreReg, name_register->slot);
        }
    }

    // ---- bindings ---------------------------------------------------------

    // Where a binding lives, as the code at this point reaches it: by name
    // (through the environments at run time), in a register of the frame,
    // or `hops` materialized scopes out at an environment slot.
    struct Slot {
        enum class Kind : std::uint8_t { Name, Local, Scoped };
        Kind kind = Kind::Name;
        std::uint32_t a = 0; // the register, or the hops
        std::uint32_t b = 0; // the slot
        bool immutable = false; // a register holding a const or a class's own name
    };

    static bool is_immutable(ScopeInfo const& scope, ScopeInfo::Binding const& binding)
    {
        return binding.kind == ScopeInfo::Binding::Kind::Const
            || (binding.kind == ScopeInfo::Binding::Kind::Class && scope.kind == ScopeInfo::Kind::ClassName);
    }

    // A reference as the parser resolved it.
    Slot slot_of(Identifier const& identifier) const
    {
        if (!m_slots)
            return {};
        if (identifier.resolution == Resolution::Local && identifier.slot < m_register_immutable.size())
            return Slot { Slot::Kind::Local, identifier.slot, 0, m_register_immutable[identifier.slot] };
        if (identifier.resolution == Resolution::Scoped)
            return Slot { Slot::Kind::Scoped, identifier.hops, identifier.slot, false };
        return {};
    }

    // A name a declaration binds, found in the scopes this code stands in:
    // the innermost that declares it, which is what a lookup by name at run
    // time would find, since no with and no eval comes between here.
    Slot slot_named(JsString* binding_name)
    {
        if (!m_slots || binding_name == nullptr)
            return {};
        std::uint32_t hops = 0;
        for (auto it = m_chain.rbegin(); it != m_chain.rend(); ++it) {
            ScopeInfo const& scope = **it;
            if (ScopeInfo::Binding const* binding = find_binding(scope, binding_name))
                return slot_in(scope, *binding, hops);
            hops += scope.materializes ? 1 : 0;
        }
        return {};
    }

    static Slot slot_in(ScopeInfo const& scope, ScopeInfo::Binding const& binding, std::uint32_t hops)
    {
        if (binding.captured)
            return Slot { Slot::Kind::Scoped, hops, binding.slot, false };
        return Slot { Slot::Kind::Local, binding.slot, 0, is_immutable(scope, binding) };
    }

    ScopeInfo::Binding const* find_binding(ScopeInfo const& scope, JsString* binding_name)
    {
        // A big scope (a bundle's outermost function has thousands of
        // names) is searched through an index made the first time; a small
        // one by walking it.
        if (scope.bindings.size() <= 16) {
            for (ScopeInfo::Binding const& binding : scope.bindings) {
                if (binding.name == binding_name)
                    return &binding;
            }
            return nullptr;
        }
        auto [entry, made] = m_binding_index.try_emplace(&scope);
        if (made) {
            entry->second.reserve(scope.bindings.size());
            for (std::size_t i = 0; i < scope.bindings.size(); ++i) {
                if (scope.bindings[i].name)
                    entry->second.emplace(scope.bindings[i].name, static_cast<std::uint32_t>(i));
            }
        }
        auto const found = entry->second.find(binding_name);
        return found == entry->second.end() ? nullptr : &scope.bindings[found->second];
    }

    void emit_load(Slot const& slot, JsString* binding_name)
    {
        switch (slot.kind) {
        case Slot::Kind::Local:
            emit(Opcode::GetLocal, slot.a);
            return;
        case Slot::Kind::Scoped:
            emit(Opcode::GetScoped, slot.a, slot.b);
            return;
        case Slot::Kind::Name:
            emit(Opcode::GetName, name(binding_name));
            return;
        }
    }

    // PutValue to a resolved binding, the value kept on the stack.
    void emit_assign(Slot const& slot)
    {
        if (slot.kind == Slot::Kind::Local)
            emit(Opcode::SetLocal, slot.a, 0, slot.immutable ? 1 : 0);
        else
            emit(Opcode::SetScoped, slot.a, slot.b);
    }

    // InitializeReferencedBinding: the value popped into the binding.
    void emit_initialize(Slot const& slot, JsString* binding_name)
    {
        switch (slot.kind) {
        case Slot::Kind::Local:
            emit(Opcode::StoreReg, slot.a);
            return;
        case Slot::Kind::Scoped:
            emit(Opcode::InitScoped, slot.a, slot.b);
            return;
        case Slot::Kind::Name:
            emit(Opcode::InitializeBinding, name(binding_name));
            return;
        }
    }

    void emit_reference(Slot const& slot, JsString* binding_name)
    {
        switch (slot.kind) {
        case Slot::Kind::Local:
            emit(Opcode::RefLocal, slot.a, 0, slot.immutable ? 1 : 0);
            return;
        case Slot::Kind::Scoped:
            emit(Opcode::RefScoped, slot.a, slot.b);
            return;
        case Slot::Kind::Name:
            emit(Opcode::RefName, name(binding_name));
            return;
        }
    }

    // A scope's environment laid out once: its captured bindings in slot
    // order, each with its name and the state it starts in — a let, const,
    // class or catch parameter in its dead zone, a parameter too when the
    // list is not simple (a default may read a later one), a const and a
    // class's own name immutable.
    std::uint32_t environment_shape(ScopeInfo const* scope)
    {
        if (auto const found = m_shape_index.find(scope); found != m_shape_index.end())
            return found->second;
        using Kind = ScopeInfo::Binding::Kind;
        CodeBlock::EnvironmentShape shape;
        shape.scope = scope;
        shape.bindings.reserve(scope->environment_size);
        bool const parameters_wait = scope->function != nullptr && !scope->function->has_simple_parameter_list;
        for (ScopeInfo::Binding const& binding : scope->bindings) {
            if (!binding.captured)
                continue;
            Environment::Binding laid {};
            laid.name = binding.name;
            laid.value = Value::undefined();
            switch (binding.kind) {
            case Kind::Parameter:
                laid.initialized = !parameters_wait;
                break;
            case Kind::Let:
            case Kind::CatchParameter:
                laid.initialized = false;
                break;
            case Kind::Const:
                laid.initialized = false;
                laid.mutable_ = false;
                break;
            case Kind::Class:
                laid.initialized = false;
                laid.mutable_ = scope->kind != ScopeInfo::Kind::ClassName;
                break;
            case Kind::FunctionName:
                laid.mutable_ = false;
                laid.strict = false;
                break;
            case Kind::Import:
                laid.mutable_ = false;
                break;
            case Kind::Var:
            case Kind::Function:
            case Kind::This:
            case Kind::NewTarget:
            case Kind::HomeObject:
            case Kind::Arguments:
                break;
            }
            shape.bindings.push_back(laid);
        }
        m_code->environments.push_back(std::move(shape));
        auto const index = static_cast<std::uint32_t>(m_code->environments.size() - 1);
        m_shape_index.emplace(scope, index);
        return index;
    }

    // A scope entered: its environment pushed when it materializes, owned
    // by a control scope so that every way out pops it.
    bool enter_scope(ScopeInfo const* scope)
    {
        m_chain.push_back(scope);
        if (!scope->materializes)
            return false;
        emit(Opcode::PushEnv, environment_shape(scope));
        Scope owned { Scope::Kind::Block };
        owned.owns_env = true;
        push_scope(owned);
        return true;
    }

    void leave_scope(bool pushed)
    {
        if (pushed) {
            pop_scope();
            emit(Opcode::PopEnv);
        }
        m_chain.pop_back();
    }

    // The dead zone in registers: a let, const or class there starts as the
    // hole each time its scope is entered — and, with `patterns`, so do the
    // parameters of a list that is not simple and a catch parameter bound
    // by a pattern, whose defaults may read a later name.
    void emit_holes(ScopeInfo const* scope, bool patterns = false)
    {
        using Kind = ScopeInfo::Binding::Kind;
        for (ScopeInfo::Binding const& binding : scope->bindings) {
            if (binding.captured)
                continue;
            bool const lexical = binding.kind == Kind::Let || binding.kind == Kind::Const || binding.kind == Kind::Class;
            bool const bound_by_pattern = patterns && (binding.kind == Kind::Parameter || binding.kind == Kind::CatchParameter);
            if (!lexical && !bound_by_pattern)
                continue;
            emit(Opcode::PushEmpty);
            emit(Opcode::StoreReg, binding.slot);
        }
    }

    // Function declarations instantiated where their scope begins
    // (§10.2.11 step 36, §14.2.3), each closing over what is current here.
    void instantiate_functions(std::vector<FunctionDeclaration const*> const& functions)
    {
        for (FunctionDeclaration const* declaration : functions) {
            JsString* const function_name = declaration->function->name;
            if (function_name == nullptr)
                continue;
            emit(Opcode::MakeClosure, function(declaration->function), None, 1);
            emit_initialize(slot_named(function_name), function_name);
        }
    }

    // The formal parameters bound in order (IteratorBindingInitialization,
    // §10.2.11 steps 24–26): for each, the argument by index or the rest of
    // them, the default in the argument's place when it is undefined —
    // named after the parameter when it is an anonymous function — then
    // the name or the pattern initialized.
    void compile_formals()
    {
        std::uint32_t index = 0;
        for (Parameter const& parameter : m_node.parameters) {
            emit(parameter.is_rest ? Opcode::RestArguments : Opcode::LoadArgument, index);
            ++index;
            if (parameter.initializer) {
                Label given;
                emit(Opcode::Dup);
                jump(Opcode::JumpIfNotUndefined, given);
                emit(Opcode::Pop);
                if (parameter.name)
                    compile_named_value(parameter.initializer, parameter.name);
                else
                    compile_expression(parameter.initializer);
                bind(given);
            }
            if (parameter.pattern)
                compile_pattern(parameter.pattern, BindMode::Initialize);
            else
                emit_initialize(slot_named(parameter.name), parameter.name);
        }
    }

    // FunctionDeclarationInstantiation (§10.2.11) as the body's first
    // instructions: the function's environment when its scope materializes,
    // the arguments object, the parameters (each argument into its register
    // or slot, or a list that is not simple bound in order with its
    // defaults), the body's own scope when the parameters have expressions
    // (a var named like a parameter starting with its value), the hoisted
    // functions, and the top-level lexicals in their dead zone. A generator
    // then goes back to its caller, to run on at its first next().
    void compile_prologue()
    {
        FunctionNode const& fn = m_node;
        ScopeInfo const* const function_scope = fn.scope;
        ScopeInfo const* const body_scope = fn.body_scope ? fn.body_scope : fn.scope;
        m_chain.push_back(function_scope);
        if (function_scope->materializes) {
            emit(Opcode::PushEnv, environment_shape(function_scope), 0, 1);
            ++m_base_envs;
        }
        if (!fn.has_simple_parameter_list)
            emit_holes(function_scope, true);
        for (ScopeInfo::Binding const& binding : function_scope->bindings) {
            if (binding.kind != ScopeInfo::Binding::Kind::Arguments)
                continue;
            bool const mapped = !fn.is_strict && fn.has_simple_parameter_list && !fn.has_duplicate_parameters;
            emit(Opcode::MakeArguments, 0, 0, mapped ? 1 : 0);
            emit_initialize(slot_in(*function_scope, binding, 0), binding.name);
        }
        if (fn.has_simple_parameter_list) {
            for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
                emit(Opcode::LoadArgument, static_cast<std::uint32_t>(i));
                emit_initialize(slot_named(fn.parameters[i].name), fn.parameters[i].name);
            }
        } else {
            compile_formals();
        }
        if (body_scope != function_scope) {
            m_chain.push_back(body_scope);
            if (body_scope->materializes) {
                emit(Opcode::PushEnv, environment_shape(body_scope), 0, 2);
                ++m_base_envs;
            }
            std::uint32_t const out = body_scope->materializes ? 1 : 0;
            for (ScopeInfo::Binding const& binding : body_scope->bindings) {
                if (binding.kind != ScopeInfo::Binding::Kind::Var || binding.name == nullptr)
                    continue;
                ScopeInfo::Binding const* parameter = find_binding(*function_scope, binding.name);
                if (parameter == nullptr)
                    continue;
                emit_load(slot_in(*function_scope, *parameter, out), binding.name);
                emit_initialize(slot_in(*body_scope, binding, 0), binding.name);
            }
        }
        instantiate_functions(fn.declarations.functions);
        emit_holes(body_scope);
        if (fn.is_generator) {
            emit(Opcode::Suspend);
            emit(Opcode::Pop);
        }
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
    // A pool is asked for by the thousand in a large body — a bundle's
    // outermost function holds every literal of a site — and a pool
    // searched from the front costs its length each time, the whole body
    // the square of it: an index beside each answers at once. The constants
    // are keyed the way the pool compares them: numbers by their bits,
    // cells by identity.
    template<typename T, typename Index>
    static std::uint32_t pooled_indexed(std::vector<T>& pool, Index& index, T const& item)
    {
        auto const found = index.find(item);
        if (found != index.end())
            return found->second;
        pool.push_back(item);
        auto const at = static_cast<std::uint32_t>(pool.size() - 1);
        index.emplace(item, at);
        return at;
    }
    struct ConstantHash {
        std::size_t operator()(Value const& value) const
        {
            std::size_t const seed = static_cast<std::size_t>(value.type()) * 0x9E3779B97F4A7C15ull;
            switch (value.type()) {
            case Value::Type::Number:
                return seed ^ std::hash<std::uint64_t> {}(std::bit_cast<std::uint64_t>(value.as_number()));
            case Value::Type::Boolean:
                return seed ^ (value.as_boolean() ? 1u : 0u);
            case Value::Type::String:
            case Value::Type::Object:
            case Value::Type::Symbol:
            case Value::Type::BigInt:
                return seed ^ std::hash<void const*> {}(value.as_cell());
            default:
                return seed;
            }
        }
    };
    std::uint32_t constant(Value const& value) { return pooled_indexed(m_code->constants, m_constant_index, value); }
    std::uint32_t bigint(BigInteger const& value) { return pooled(m_code->bigints, value); }
    std::uint32_t name(JsString* atom) { return pooled_indexed(m_code->names, m_name_index, atom); }
    std::uint32_t name_of(std::u16string_view text) { return name(m_heap.atom(text)); }
    std::uint32_t constant_string(std::u16string_view text) { return constant(Value::string(m_heap.atom(text))); }
    std::uint32_t function(FunctionNode const* node) { return pooled_indexed(m_code->functions, m_function_index, node); }
    std::uint32_t class_node(ClassNode const* node) { return pooled_indexed(m_code->classes, m_class_index, node); }
    std::uint32_t template_index(TemplateLiteral const* node) { return pooled_indexed(m_code->templates, m_template_index, node); }
    std::uint32_t regexp(RegExpLiteral const* node) { return pooled_indexed(m_code->regexps, m_regexp_index, node); }
    std::uint32_t declarations(Declarations const* node) { return pooled_indexed(m_code->declarations, m_declarations_index, node); }
    std::uint32_t node_index(Expression const* node) { return pooled_indexed(m_code->nodes, m_node_index, node); }
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

    // The body being compiled is an async generator's: yields and returns
    // await their values, and yield* speaks the async iterator protocol.
    bool in_async_generator() const { return m_code->is_async && m_code->is_generator; }

    // IteratorClose on a normal exit from a loop: return() runs, and its
    // failure is the outcome. An async iterator's answer is awaited and
    // must be an object (AsyncIteratorClose, §7.4.13).
    void emit_iterator_close(Scope const& scope)
    {
        if (scope.async_iterator)
            emit_async_iterator_close(scope.iterator_reg);
        else
            emit(Opcode::IteratorClose, scope.iterator_reg);
    }

    void emit_async_iterator_close(std::uint32_t iterator_reg)
    {
        Label skip;
        emit(Opcode::IteratorReturnCall, iterator_reg);
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

    // ExportDeclaration evaluation (§16.2.3.7):
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

    // The completion register starts a compound statement empty, and is
    // undefined after one that wrote nothing (see track_completion).
    void completion_reset()
    {
        emit(Opcode::PushEmpty);
        emit(Opcode::StoreReg, m_completion);
    }

    void completion_settle()
    {
        Label empty;
        Label done;
        emit(Opcode::LoadReg, m_completion);
        jump(Opcode::JumpIfEmpty, empty);
        jump(Opcode::Jump, done);
        bind(empty);
        emit(Opcode::PushUndefined);
        emit(Opcode::StoreReg, m_completion);
        bind(done);
    }

    static bool is_compound_statement(NodeType type)
    {
        return type == NodeType::IfStatement || type == NodeType::ForStatement || type == NodeType::ForInStatement
            || type == NodeType::ForOfStatement || type == NodeType::WhileStatement || type == NodeType::DoWhileStatement
            || type == NodeType::SwitchStatement || type == NodeType::TryStatement || type == NodeType::WithStatement;
    }

    void compile_statement(Statement const* statement, std::vector<JsString*> labels)
    {
        bool const compound = m_track_completion && is_compound_statement(statement->type);
        if (compound)
            completion_reset();
        compile_statement_inner(statement, std::move(labels));
        if (compound && !m_unreachable)
            completion_settle();
    }

    void compile_statement_inner(Statement const* statement, std::vector<JsString*> labels)
    {
        if (!m_error.empty())
            return;
        // Where this statement's instructions begin; a statement that emits
        // nothing before the next one does is overwritten by it.
        if (!m_code->positions.empty() && m_code->positions.back().instruction == here())
            m_code->positions.back().position = statement->position;
        else
            m_code->positions.push_back({ here(), statement->position });
        switch (statement->type) {
        case NodeType::VariableDeclaration:
            compile_declaration(*static_cast<VariableDeclaration const*>(statement));
            return;
        case NodeType::FunctionDeclaration: {
            auto const& declaration = *static_cast<FunctionDeclaration const*>(statement);
            if (!declaration.annex_b_hoisted)
                return;
            if (!m_slots) {
                emit(Opcode::AnnexBCopy, name(declaration.function->name));
                return;
            }
            // B.3.2.1 step 2.b resolved: the block's binding (the innermost
            // of the name) copied to the function's var of it.
            JsString* const function_name = declaration.function->name;
            ScopeInfo const* const var_scope = m_node.body_scope ? m_node.body_scope : m_node.scope;
            ScopeInfo::Binding const* var_binding = find_binding(*var_scope, function_name);
            if (var_binding == nullptr)
                return;
            std::uint32_t hops = 0;
            for (auto it = m_chain.rbegin(); it != m_chain.rend() && *it != var_scope; ++it)
                hops += (*it)->materializes ? 1 : 0;
            emit_load(slot_named(function_name), function_name);
            emit_assign(slot_in(*var_scope, *var_binding, hops));
            emit(Opcode::Pop);
            return;
        }
        case NodeType::ClassDeclaration: {
            auto const& declaration = *static_cast<ClassDeclaration const*>(statement);
            // An anonymous `export default class` is named "default" and
            // binds `*default*` (§16.2.3.7), as execute_class_declaration
            // has it.
            bool const anonymous = declaration.node->name == nullptr;
            compile_class(*declaration.node, anonymous ? name(m_heap.atom(u"default")) : None, false);
            JsString* const binding_name = anonymous ? m_heap.atom(u"*default*") : declaration.node->name;
            emit_initialize(slot_named(binding_name), binding_name);
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
            if (m_track_completion)
                emit(Opcode::StoreReg, m_completion);
            else
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
            if (ret.argument) {
                compile_expression(ret.argument);
                if (in_async_generator()) {
                    // §15.6 / §14.10.1: an async generator awaits what it returns.
                    emit(Opcode::Await);
                    compile_resume_dispatch(false);
                }
            } else {
                emit(Opcode::PushUndefined);
            }
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
        if (m_slots) {
            // Resolved: an environment only when the block's scope
            // materializes; its lexicals in their dead zone either way, its
            // functions made on entry (§14.2.3).
            if (block.scope == nullptr) {
                compile_statements(block.body);
                return;
            }
            bool const pushed = enter_scope(block.scope);
            emit_holes(block.scope);
            instantiate_functions(block.declarations.functions);
            compile_statements(block.body);
            leave_scope(pushed);
            return;
        }
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
                // reference resolved first, so a `with` in scope can catch it
                // (resolved code has none, so the store comes after).
                if (!declarator.init)
                    continue;
                Slot const slot = slot_named(declarator.name);
                if (slot.kind != Slot::Kind::Name) {
                    compile_named_value(declarator.init, declarator.name);
                    emit_assign(slot);
                    emit(Opcode::Pop);
                    continue;
                }
                emit(Opcode::RefName, name(declarator.name));
                compile_named_value(declarator.init, declarator.name);
                emit(Opcode::RefPut);
                continue;
            }
            if (declarator.init)
                compile_named_value(declarator.init, declarator.name);
            else
                emit(Opcode::PushUndefined);
            emit_initialize(slot_named(declarator.name), declarator.name);
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
        for (auto const& [name_atom, is_const] : loop.declarations.lexicals) {
            if (!is_const)
                per_iteration.push_back(name_atom);
        }
        // Resolved: the head's scope is an environment only when it
        // materializes (a closure captures a variable), and only then is it
        // copied per iteration, every slot at once; registers need no copy.
        bool const resolved_head = m_slots && loop.scope != nullptr;
        bool head_pushed = false;
        bool copy_all = false;
        std::uint32_t copies = None;
        if (resolved_head) {
            head_pushed = enter_scope(loop.scope);
            emit_holes(loop.scope);
            copy_all = head_pushed && !per_iteration.empty();
        } else if (lexical) {
            emit(Opcode::PushBlockEnv, declarations(&loop.declarations));
            Scope scope { Scope::Kind::Block };
            scope.owns_env = true;
            push_scope(scope);
            copies = per_iteration.empty() ? None : name_list(per_iteration);
        }
        auto const emit_copy = [&] {
            if (copy_all)
                emit(Opcode::CopyIterationEnv, 0, 0, 1);
            else if (copies != None)
                emit(Opcode::CopyIterationEnv, copies);
        };
        if (loop.init) {
            // The head's expression is no statement of the loop's: it
            // leaves the completion value alone (§14.7.4.2 starts V at
            // undefined after it).
            bool const tracking = m_track_completion;
            m_track_completion = false;
            compile_statement(loop.init, {});
            m_track_completion = tracking;
        }
        emit_copy();
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
        emit_copy();
        if (loop.update) {
            compile_expression(loop.update);
            emit(Opcode::Pop);
        }
        emit(Opcode::Step);
        jump(Opcode::Jump, top);
        bind(end);
        if (resolved_head) {
            leave_scope(head_pushed);
        } else if (lexical) {
            pop_scope();
            emit(Opcode::PopEnv);
        }
    }

    // The head of a for-in/of loop binds the iteration's value (§14.7.5.7
    // steps 6.g–6.i), with the value on the stack: a let/const gets a
    // fresh environment for the body — pushed here as a Block scope the
    // caller pops after the body; a var or an expression assigns through a
    // reference; a pattern destructures. Resolved code enters the head's
    // scope here, an environment only when it materializes (the names then
    // start in their dead zone, and so do registers under a pattern).
    struct HeadScope {
        bool resolved = false; // the head's scope is on the chain
        bool pushed = false; // an environment to pop after the body
    };
    HeadScope compile_loop_head_binding(VariableDeclaration const* declaration, Expression const* target, ScopeInfo const* head_scope)
    {
        if (declaration && declaration->kind != VariableDeclaration::Kind::Var) {
            VariableDeclarator const& declarator = declaration->declarations[0];
            if (m_slots) {
                HeadScope head;
                if (head_scope) {
                    head.resolved = true;
                    head.pushed = enter_scope(head_scope);
                    if (declarator.pattern)
                        emit_holes(head_scope);
                }
                if (declarator.pattern)
                    compile_pattern(declarator.pattern, BindMode::Initialize);
                else
                    emit_initialize(slot_named(declarator.name), declarator.name);
                return head;
            }
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
            return HeadScope { false, true };
        }
        if (declaration) {
            VariableDeclarator const& declarator = declaration->declarations[0];
            if (declarator.pattern) {
                compile_pattern(declarator.pattern, BindMode::VarAssign);
            } else if (Slot const slot = slot_named(declarator.name); slot.kind != Slot::Kind::Name) {
                emit_assign(slot);
                emit(Opcode::Pop);
            } else {
                emit(Opcode::RefName, name(declarator.name));
                emit(Opcode::RefPut);
            }
            return {};
        }
        if (is_pattern(target)) {
            compile_pattern(target, BindMode::Assign);
            return {};
        }
        compile_reference(target);
        emit(Opcode::RefPut);
        return {};
    }

    void leave_loop_head(HeadScope const& head)
    {
        if (head.resolved) {
            leave_scope(head.pushed);
        } else if (head.pushed) {
            pop_scope();
            emit(Opcode::PopEnv);
        }
    }

    // The head's names in their dead zone around its own expression
    // (§14.7.5.6): an environment of them by name, or, resolved, the head's
    // scope entered as it is (its registers holes). Returns what
    // leave_loop_head undoes.
    HeadScope enter_loop_head_expression(VariableDeclaration const* declaration, ScopeInfo const* head_scope)
    {
        if (!declaration || declaration->kind == VariableDeclaration::Kind::Var)
            return {};
        if (m_slots) {
            if (!head_scope)
                return {};
            HeadScope head { true, enter_scope(head_scope) };
            emit_holes(head_scope);
            return head;
        }
        emit(Opcode::PushNamesEnv, name_list(head_names(declaration)), 1);
        Scope scope { Scope::Kind::Block };
        scope.owns_env = true;
        push_scope(scope);
        return HeadScope { false, true };
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
        // B.3.5: `for (var x = 1 in o)` assigns the initializer first.
        if (loop.declaration && loop.declaration->kind == VariableDeclaration::Kind::Var && loop.declaration->declarations[0].init)
            compile_declaration(*loop.declaration);
        HeadScope const dead_zone = enter_loop_head_expression(loop.declaration, loop.scope);
        compile_expression(loop.object);
        leave_loop_head(dead_zone);
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
        HeadScope const head = compile_loop_head_binding(loop.declaration, loop.target, loop.scope);
        compile_statement(loop.body, {});
        leave_loop_head(head);
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
        HeadScope const dead_zone = enter_loop_head_expression(loop.declaration, loop.scope);
        compile_expression(loop.iterable);
        leave_loop_head(dead_zone);
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
        HeadScope const head = compile_loop_head_binding(loop.declaration, loop.target, loop.scope);
        compile_statement(loop.body, {});
        leave_loop_head(head);
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
            if (m_slots && (statement.catch_pattern || statement.catch_parameter)) {
                // Resolved: the parameter's scope is an environment only when
                // it materializes; a pattern's registers start as holes.
                ScopeInfo const* const scope = statement.scope;
                bool pushed = false;
                if (scope) {
                    pushed = enter_scope(scope);
                    if (statement.catch_pattern)
                        emit_holes(scope, true);
                }
                if (statement.catch_pattern)
                    compile_pattern(statement.catch_pattern, BindMode::Initialize);
                else
                    emit_initialize(slot_named(statement.catch_parameter), statement.catch_parameter);
                compile_block(*statement.handler);
                if (scope)
                    leave_scope(pushed);
            } else if (statement.catch_pattern) {
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
        // §14.15.3: the finally block's own completion starts empty (a bare
        // `break` out of it leaves undefined, `42; break` leaves 42), and
        // when it completes normally it is discarded — the try's or the
        // catch's value, kept aside at the entry, is put back on every way
        // out through the token.
        std::uint32_t kept_completion = None;
        if (m_track_completion) {
            kept_completion = new_register();
            emit(Opcode::LoadReg, m_completion);
            emit(Opcode::StoreReg, kept_completion);
            completion_reset();
        }
        compile_block(*statement.finalizer);
        if (m_track_completion && !m_unreachable) {
            emit(Opcode::LoadReg, kept_completion);
            emit(Opcode::StoreReg, m_completion);
        }
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
        bool const resolved = m_slots && statement.scope != nullptr;
        bool const env = !resolved && (!statement.declarations.lexicals.empty() || !statement.declarations.functions.empty());
        bool resolved_pushed = false;
        if (resolved) {
            // The case block as a block scope (§14.12.4), resolved.
            resolved_pushed = enter_scope(statement.scope);
            emit_holes(statement.scope);
            instantiate_functions(statement.declarations.functions);
        } else if (env) {
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
        if (resolved) {
            leave_scope(resolved_pushed);
        } else if (env) {
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
        case NodeType::FunctionExpression: {
            // A named function expression's own name gets an environment
            // only when its scope materializes (the body reads the name).
            auto const& expression = *static_cast<FunctionExpression const*>(value);
            bool const without_environment = expression.scope != nullptr && !expression.scope->materializes;
            emit(Opcode::MakeClosure, function(expression.function), name_index, without_environment ? 1 : 0);
            return;
        }
        case NodeType::ArrowFunction:
            emit(Opcode::MakeClosure, function(static_cast<ArrowFunction const*>(value)->function), name_index);
            return;
        case NodeType::ClassExpression:
            compile_class(*static_cast<ClassExpression const*>(value)->node, name_index, false);
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
            compile_class(*static_cast<ClassExpression const*>(value)->node, None, true);
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
        case NodeType::Identifier: {
            auto const& identifier = *static_cast<Identifier const*>(expression);
            emit_load(slot_of(identifier), identifier.name);
            return;
        }
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
            // A plain function's own `this` is its frame's; an arrow's, or
            // one an arrow also reads, is found through the environments.
            if (m_slots && static_cast<ThisExpression const*>(expression)->resolution == Resolution::Local)
                emit(Opcode::LoadThis);
            else
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
            if (m_slots && static_cast<NewTargetExpression const*>(expression)->resolution == Resolution::Local)
                emit(Opcode::LoadNewTarget);
            else
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
            if (in_async_generator()) {
                // AsyncGeneratorUnwrapYieldResumption (§27.6.3.7): the
                // value a return resumption carries is awaited first, and a
                // rejection is the throw instead.
                emit(Opcode::Await);
                compile_resume_dispatch(false);
            }
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
        if (in_async_generator()) {
            // AsyncGeneratorYield (§27.6.3.8): the value is awaited before
            // it goes out.
            emit(Opcode::Await);
            compile_resume_dispatch(false);
        }
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
        bool const async = in_async_generator();
        // In an async generator every inner answer is awaited and must then
        // be an iterator result (§27.5.3.8's generatorKind async arms).
        auto const await_inner_result = [&] {
            if (!async)
                return;
            emit(Opcode::Await);
            compile_resume_dispatch(false);
            emit(Opcode::RequireIterResult);
        };
        emit(async ? Opcode::GetAsyncIterator : Opcode::GetIterator);
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
        await_inner_result();
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
            await_inner_result();
            jump(Opcode::Jump, got_result);
            bind(no_method);
            emit(Opcode::Pop);
            if (async)
                emit_async_iterator_close(iterator);
            else
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
            await_inner_result();
            emit(Opcode::Dup);
            emit(Opcode::IteratorResultDone);
            jump(Opcode::JumpIfFalse, yield_it);
            emit(Opcode::IteratorResultValue);
            if (async) {
                emit(Opcode::Await);
                compile_resume_dispatch(false);
            }
            emit_exit(Deferred::Kind::Return, nullptr);
            bind(no_method);
            emit(Opcode::Pop);
            emit(Opcode::LoadReg, received_value);
            if (async) {
                emit(Opcode::Await);
                compile_resume_dispatch(false);
            }
            emit_exit(Deferred::Kind::Return, nullptr);
        }
        bind(got_result);
        emit(Opcode::Dup);
        emit(Opcode::IteratorResultDone);
        jump(Opcode::JumpIfTrue, done_exit);
        bind(yield_it);
        if (async) {
            // AsyncGeneratorYield over the inner result's value — which,
            // unlike a `yield v`, is not awaited first (§27.5.3.8 step 7.a.vi).
            emit(Opcode::IteratorResultValue);
            emit(Opcode::Yield);
        } else {
            emit(Opcode::Yield, 0, 0, 1);
        }
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
            if (async) {
                // AsyncGeneratorUnwrapYieldResumption: the return value is
                // awaited; a rejection is a throw handed to the inner iterator.
                Label awaited;
                emit(Opcode::Await);
                jump(Opcode::JumpIfResumeNormal, awaited);
                emit(Opcode::StoreReg, received_value);
                emit(Opcode::PushInt, 1);
                emit(Opcode::StoreReg, received_kind);
                jump(Opcode::Jump, loop);
                bind(awaited);
            }
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
        // The count rides on the instruction so the array is made with
        // room for its elements rather than grown one at a time.
        emit(Opcode::NewArrayLiteral, static_cast<std::uint32_t>(literal.elements.size()));
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
        // PropertyDefinitionEvaluation (§13.2.5.5), in source order; the
        // count rides on the instruction so the object is made with room.
        emit(Opcode::NewObject, static_cast<std::uint32_t>(literal.properties.size()));
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
        case NodeType::Identifier: {
            auto const& identifier = *static_cast<Identifier const*>(target);
            emit_reference(slot_of(identifier), identifier.name);
            return;
        }
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
        // In the method itself (the home object resolved Local), `this` and
        // the home object are the frame's: flag 1.
        bool const own = m_slots && member.resolution == Resolution::Local;
        std::uint8_t const flags = own ? 1 : 0;
        if (member.property) {
            // §13.3.7.1: `this` is resolved before the key is evaluated, so
            // `super[super()]` in a derived constructor is a ReferenceError
            // before the parent constructor ever runs.
            emit(own ? Opcode::LoadThis : Opcode::ResolveThis);
            emit(Opcode::Pop);
            compile_expression(member.property);
            emit(Opcode::RefSuper, 0, 0, flags);
        } else {
            emit(Opcode::RefSuperNamed, name(member.name), 0, flags);
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
        case NodeType::Identifier: {
            // A resolved binding is never an object environment's, so the
            // call's `this` is undefined without a reference.
            auto const& identifier = *static_cast<Identifier const*>(callee);
            if (Slot const slot = slot_of(identifier); slot.kind != Slot::Kind::Name) {
                emit_load(slot, identifier.name);
                if (optional_call)
                    optional_check(context, Opcode::Dup, 1, 0);
                emit(Opcode::PushUndefined);
                return;
            }
            emit(Opcode::RefName, name(identifier.name));
            emit(Opcode::RefGet);
            if (optional_call)
                optional_check(context, Opcode::Dup, 1, 1);
            emit(Opcode::RefThis);
            emit(Opcode::RefDrop);
            return;
        }
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
            emit(Opcode::NewArrayLiteral, static_cast<std::uint32_t>(arguments.size()));
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
    // The chain's landings: each short-circuit discards what its link had
    // pushed and lands on `pushes` values of `landing` — undefined for a
    // value, true for `delete a?.b` (§13.5.1.2 step 4).
    void finish_chain(ChainContext& context, int pushes, Opcode landing = Opcode::PushUndefined)
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
                emit(landing);
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
                // A resolved binding exists, so its value is read — a let
                // in its dead zone throwing as a read does (§13.5.3).
                auto const& identifier = *static_cast<Identifier const*>(unary.operand);
                if (Slot const slot = slot_of(identifier); slot.kind != Slot::Kind::Name) {
                    emit_load(slot, identifier.name);
                    emit(Opcode::Unary, static_cast<std::uint32_t>(UnaryOp::Typeof));
                    return;
                }
                emit(Opcode::TypeofName, name(identifier.name));
                return;
            }
            compile_expression(unary.operand);
            emit(Opcode::Unary, static_cast<std::uint32_t>(UnaryOp::Typeof));
            return;
        case UnaryOp::Delete:
            switch (unary.operand->type) {
            case NodeType::MemberExpression: {
                // The member as the last link of a chain: `delete a?.b` on a
                // nullish `a` is true without a reference ever being made.
                auto const& member = *static_cast<MemberExpression const*>(unary.operand);
                ChainContext context;
                context.base_depth = m_depth;
                context.base_refs = m_refs;
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
                emit(Opcode::RefDelete);
                finish_chain(context, 1, Opcode::PushTrue);
                return;
            }
            case NodeType::Identifier:
                // A declared binding cannot be deleted (§9.1.1.1.7): false,
                // and nothing to look up for a resolved one.
                if (slot_of(*static_cast<Identifier const*>(unary.operand)).kind != Slot::Kind::Name) {
                    emit(Opcode::PushFalse);
                    return;
                }
                compile_reference(unary.operand);
                emit(Opcode::RefDelete);
                return;
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
        if (update.target->type == NodeType::Identifier) {
            auto const& identifier = *static_cast<Identifier const*>(update.target);
            if (Slot const slot = slot_of(identifier); slot.kind != Slot::Kind::Name) {
                emit_load(slot, identifier.name);
                emit(Opcode::ToNumeric);
                if (update.prefix) {
                    emit(update.increment ? Opcode::Inc : Opcode::Dec);
                    emit_assign(slot);
                } else {
                    emit(Opcode::Dup);
                    emit(update.increment ? Opcode::Inc : Opcode::Dec);
                    emit_assign(slot);
                    emit(Opcode::Pop);
                }
                return;
            }
        }
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
        if (assignment.target->type == NodeType::Identifier) {
            // A resolved binding needs no reference: nothing is looked up
            // before the value, and the store checks its own dead zone and
            // immutability.
            auto const& identifier = *static_cast<Identifier const*>(assignment.target);
            if (Slot const slot = slot_of(identifier); slot.kind != Slot::Kind::Name) {
                compile_resolved_assignment(assignment, slot, identifier);
                return;
            }
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

    void compile_resolved_assignment(AssignmentExpression const& assignment, Slot const& slot, Identifier const& identifier)
    {
        if (assignment.op == AssignmentOp::Assign) {
            compile_named_value(assignment.value, identifier.name);
            emit_assign(slot);
            return;
        }
        emit_load(slot, identifier.name);
        if (assignment.op == AssignmentOp::LogicalAnd || assignment.op == AssignmentOp::LogicalOr || assignment.op == AssignmentOp::Nullish) {
            // The right-hand side may not be evaluated at all; then the
            // current value is the result and nothing is stored.
            Label skip;
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
            compile_named_value(assignment.value, identifier.name);
            emit_assign(slot);
            bind(skip);
            return;
        }
        compile_expression(assignment.value);
        emit(Opcode::Binary, static_cast<std::uint32_t>(*binary_for(assignment.op)));
        emit_assign(slot);
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
            auto const& identifier = *static_cast<Identifier const*>(target);
            emit_initialize(slot_of(identifier), identifier.name);
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
    std::unordered_map<JsString*, std::uint32_t> m_name_index; // the names pool, by name
    std::unordered_map<FunctionNode const*, std::uint32_t> m_function_index; // the functions pool, by node
    std::unordered_map<Expression const*, std::uint32_t> m_node_index; // the nodes pool (a call site each), by node
    std::unordered_map<Value, std::uint32_t, ConstantHash> m_constant_index; // the constants pool, by value
    std::unordered_map<ClassNode const*, std::uint32_t> m_class_index;
    std::unordered_map<TemplateLiteral const*, std::uint32_t> m_template_index;
    std::unordered_map<RegExpLiteral const*, std::uint32_t> m_regexp_index;
    std::unordered_map<Declarations const*, std::uint32_t> m_declarations_index;
    std::vector<Scope> m_scopes;
    std::string m_error;
    int m_depth = 0;
    int m_refs = 0;
    bool m_unreachable = false;
    // Resolved bindings (see the constructor): which registers hold an
    // immutable binding, the parser's scopes this point of the code stands
    // in (the function's first, whether they materialize or not), the
    // environments the prologue pushed for the whole body, each scope's
    // shape once made, and the big scopes' bindings by name.
    bool m_slots = false;
    std::vector<bool> m_register_immutable;
    std::vector<ScopeInfo const*> m_chain;
    std::uint32_t m_base_envs = 0;
    std::unordered_map<ScopeInfo const*, std::uint32_t> m_shape_index;
    std::unordered_map<ScopeInfo const*, std::unordered_map<JsString*, std::uint32_t>> m_binding_index;
};

} // namespace

std::unique_ptr<CodeBlock> compile_function_body(FunctionNode const& node, Heap& heap, std::string* error)
{
    Compiler compiler(node, heap);
    if (node.is_program_body)
        compiler.track_completion();
    return compiler.compile(error);
}

std::unique_ptr<CodeBlock> compile_parameter_list(FunctionNode const& node, Heap& heap, std::string* error)
{
    Compiler compiler(node, heap);
    return compiler.compile_parameters(error);
}

// ---- disassembly ------------------------------------------------------

std::optional<SourcePosition> source_position_of(CodeBlock const& code, std::uint32_t instruction)
{
    if (instruction < code.code.size()) {
        Instruction const& ins = code.code[instruction];
        bool const has_node = ins.op == Opcode::Call || ins.op == Opcode::CallArray || ins.op == Opcode::CallEval
            || ins.op == Opcode::CallEvalArray || ins.op == Opcode::New || ins.op == Opcode::NewArray;
        if (has_node && ins.b < code.nodes.size() && code.nodes[ins.b] != nullptr)
            return code.nodes[ins.b]->position;
    }
    // The last statement that began at or before the instruction.
    auto const after = std::upper_bound(code.positions.begin(), code.positions.end(), instruction,
        [](std::uint32_t at, CodeBlock::Position const& entry) { return at < entry.instruction; });
    if (after == code.positions.begin())
        return std::nullopt;
    return std::prev(after)->position;
}

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
        case Opcode::ClassScope:
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
        case Opcode::ClassScopeNamedDyn:
        case Opcode::ClassElement:
        case Opcode::ClassElementKeyed:
        case Opcode::LoadArgument:
        case Opcode::RestArguments:
        case Opcode::DefineMethodDyn:
        case Opcode::DefineAccessorDyn:
        case Opcode::ThrowTypeErrorConst:
            out += " " + std::to_string(ins.a);
            break;
        case Opcode::GetLocal:
        case Opcode::SetLocal:
        case Opcode::RefLocal:
            out += " r" + std::to_string(ins.a);
            if (ins.a < code.register_names.size() && code.register_names[ins.a])
                out += " " + code.register_names[ins.a]->to_utf8();
            if (ins.flags)
                out += " const";
            break;
        case Opcode::GetScoped:
        case Opcode::SetScoped:
        case Opcode::InitScoped:
        case Opcode::RefScoped:
            out += " hops=" + std::to_string(ins.a) + " slot=" + std::to_string(ins.b);
            break;
        case Opcode::PushEnv:
            out += " " + std::to_string(ins.a);
            if (ins.a < code.environments.size())
                out += " size=" + std::to_string(code.environments[ins.a].bindings.size());
            if (ins.flags & 1)
                out += " function";
            if (ins.flags & 2)
                out += " var";
            break;
        case Opcode::MakeArguments:
            out += ins.flags ? " mapped" : " unmapped";
            break;
        case Opcode::RefSuper:
            if (ins.flags)
                out += " own";
            break;
        case Opcode::PushNamesEnv:
        case Opcode::CopyIterationEnv:
            if (ins.op == Opcode::CopyIterationEnv && ins.flags) {
                out += " all";
                break;
            }
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
