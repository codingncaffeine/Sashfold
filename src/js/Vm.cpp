#include "js/Vm.h"

// The run loop over a Frame, and the drivers that start and resume the
// bodies that need one: a generator's, resumed by next/return/throw; an
// async function's, resumed by the reaction jobs of the promise it
// awaits. Every instruction calls the same mechanisms the tree-walking
// evaluator calls (Interpreter::Impl), so the two agree by construction.
//
// Rooting: an instruction's inputs stay on the frame's operand stack —
// traced through vm_frames — until the operation has finished; only then
// are they popped and the result pushed. A value that must be held in a
// local across an allocation is rooted explicitly.

#include "js/Compiler.h"
#include "js/Evaluator.h"
#include "js/Runtime.h"
#include "js/Strings.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

void ClassBuilder::trace(Tracer& tracer)
{
    tracer.visit(class_env);
    tracer.visit(outer_private);
    tracer.visit(private_env);
    if (has_name_key)
        tracer.visit(name_key);
    tracer.visit(proto);
    tracer.visit(constructor);
    for (StaticElement const& item : statics)
        tracer.visit(item.key);
}

void Frame::trace(Tracer& tracer)
{
    for (Value const& value : stack)
        tracer.visit(value);
    for (Value const& value : registers)
        tracer.visit(value);
    for (Reference const& reference : refs) {
        tracer.visit(reference.environment);
        tracer.visit(reference.name);
        tracer.visit(reference.base);
        tracer.visit(reference.key);
        tracer.visit(reference.key_value);
        tracer.visit(reference.this_value);
    }
    for (Environment* env : envs)
        tracer.visit(env);
    for (Value const& value : arguments)
        tracer.visit(value);
    for (ClassBuilder* builder : builders)
        tracer.visit(builder);
    tracer.visit(field_key);
    tracer.visit(variable);
    tracer.visit(function);
    tracer.visit(private_environment);
    tracer.visit(result);
    tracer.visit(resume_value);
}

namespace {

// A key that went through ToPropertyKey and back onto the stack: a symbol,
// or a string whose canonical form Heap::key tells apart from an index.
PropertyKey key_from_value(Heap& heap, Value const& value)
{
    if (value.is_symbol())
        return PropertyKey::symbol(value.as_symbol());
    return heap.key(value.as_string());
}

Value key_to_value(Heap& heap, PropertyKey const& key)
{
    if (key.is_symbol())
        return Value::symbol(key.as_symbol());
    return Value::string(heap.key_to_string(key));
}

// The elements of an argument array built by the compiler for a spread
// call: dense, no holes.
std::vector<Value> array_arguments(Value const& array_value)
{
    auto* array = static_cast<ArrayObject*>(array_value.as_object());
    std::vector<Value> arguments;
    arguments.reserve(array->length());
    for (std::uint32_t i = 0; i < array->length(); ++i) {
        Value const element = array->element(i);
        arguments.push_back(element.is_empty() ? Value::undefined() : element);
    }
    return arguments;
}

} // namespace

// ---- compiling and framing ------------------------------------------------

CodeBlock const* Interpreter::Impl::compiled_body(FunctionNode const& node)
{
    if (auto const found = code_blocks.find(&node); found != code_blocks.end())
        return found->second.get();
    std::string error;
    std::unique_ptr<CodeBlock> code = compile_function_body(node, heap(), &error);
    if (!code) {
        self.throw_syntax_error(error);
        return nullptr;
    }
    CodeBlock const* raw = code.get();
    code_blocks.emplace(&node, std::move(code));
    return raw;
}

// A plain body on the machine: compiled at its first call, run on a frame
// over the call's environments until it returns or throws. It cannot
// yield or await — the compiler refuses those outside a generator or an
// async body — so the run ends Completed with the return value in the
// frame's result, or Threw with the exception pending.
std::optional<Value> Interpreter::Impl::run_compiled_body(ScriptFunction& function, Context const& cx, PropertyKey const* field_key)
{
    return run_compiled_node(function.node(), cx, field_key);
}

// A parameter list that is not simple — a default, a pattern, a rest —
// bound on the machine (IteratorBindingInitialization of the formals,
// §10.2.11 step 24–26): the block reads the call's arguments by index and
// initializes each parameter in the environment the prologue made for
// them, with the defaults evaluated there in order.
CodeBlock const* Interpreter::Impl::compiled_parameters(FunctionNode const& node)
{
    if (auto const found = parameter_blocks.find(&node); found != parameter_blocks.end())
        return found->second.get();
    std::string error;
    std::unique_ptr<CodeBlock> code = compile_parameter_list(node, heap(), &error);
    if (!code) {
        self.throw_syntax_error(error);
        return nullptr;
    }
    CodeBlock const* raw = code.get();
    parameter_blocks.emplace(&node, std::move(code));
    return raw;
}

bool Interpreter::Impl::run_parameter_block(FunctionNode const& node, Environment* env, std::span<Value const> arguments, Context const& cx)
{
    CodeBlock const* code = compiled_parameters(node);
    if (code == nullptr)
        return false;
    Roots const roots(self);
    Context binding_context = cx;
    binding_context.lexical = env;
    Frame* frame = nullptr;
    {
        Heap::NoCollect const guard(heap());
        frame = new_frame(*code, binding_context);
        frame->arguments.assign(arguments.begin(), arguments.end());
    }
    RunStatus const status = vm_run(*frame);
    return status == RunStatus::Completed;
}

// The same for a body that is no function's: a script's or an eval's
// statement list, compiled as a function body that tracks its completion
// value and returns it.
std::optional<Value> Interpreter::Impl::run_compiled_node(FunctionNode const& node, Context const& cx, PropertyKey const* field_key)
{
    CodeBlock const* code = compiled_body(node);
    if (code == nullptr)
        return std::nullopt;
    Roots const roots(self);
    Frame* frame = nullptr;
    {
        Heap::NoCollect const guard(heap());
        frame = new_frame(*code, cx);
        if (field_key != nullptr)
            frame->field_key = key_to_value(heap(), *field_key);
    }
    RunStatus const status = vm_run(*frame);
    if (status == RunStatus::Threw)
        return std::nullopt;
    if (status != RunStatus::Completed)
        return self.throw_syntax_error("a plain function body suspended");
    return frame->result.is_empty() ? Value::undefined() : frame->result;
}

Frame* Interpreter::Impl::new_frame(CodeBlock const& code, Context const& cx)
{
    Frame* frame = heap().allocate<Frame>();
    frame->code = &code;
    frame->registers.assign(code.register_count, Value::undefined());
    frame->stack.reserve(code.max_stack + 1);
    frame->envs.push_back(cx.lexical);
    frame->variable = cx.variable;
    frame->function = cx.function;
    frame->program = cx.program;
    frame->private_environment = cx.private_environment;
    frame->strict = cx.strict;
    return frame;
}

Context Interpreter::Impl::frame_context(Frame const& frame) const
{
    return Context { frame.envs.back(), frame.variable, frame.program, frame.function, frame.strict, frame.private_environment };
}

// A throw at the instruction just executed: the innermost handler whose
// range covers it takes over with the stacks cut back to its depths and
// the thrown value pushed. A termination (the interrupt) is uncatchable.
bool Interpreter::Impl::vm_unwind(Frame& frame)
{
    if (self.m_terminated)
        return false;
    std::uint32_t const at = frame.pc - 1;
    for (Handler const& handler : frame.code->handlers) {
        if (at < handler.start || at >= handler.end)
            continue;
        Value const thrown = self.take_exception();
        frame.stack.resize(handler.stack_depth);
        frame.refs.resize(handler.ref_depth);
        frame.envs.resize(1 + handler.env_depth);
        if (frame.builders.size() > handler.class_depth) {
            // A throw out of a class body: the classes it left half-made
            // are dropped, and the private names and strictness of the
            // outermost one's outside come back.
            ClassBuilder const& outermost = *frame.builders[handler.class_depth];
            frame.private_environment = outermost.outer_private;
            frame.strict = outermost.saved_strict;
            frame.builders.resize(handler.class_depth);
        }
        frame.stack.push_back(thrown);
        frame.pc = handler.target;
        return true;
    }
    return false;
}

// ---- the loop -------------------------------------------------------------

RunStatus Interpreter::Impl::vm_run(Frame& frame)
{
    if (!stack_ok())
        return RunStatus::Threw;
    struct FrameGuard {
        std::vector<Frame*>& frames;
        ~FrameGuard() { frames.pop_back(); }
    };
    vm_frames.push_back(&frame);
    FrameGuard const guard { vm_frames };
    ContextScope const context_scope(*this, frame_context(frame));
    if (frame.resume_pending) {
        frame.push(frame.resume_value);
        frame.resume_pending = false;
        frame.resume_value = Value::undefined();
    }
    CodeBlock const& code = *frame.code;
    Heap& h = heap();
    WellKnownAtoms const& well_known = atoms();

    // A property reference to `base[key]` with the key converted at once
    // unless it is an object, which waits for the base check (the order
    // evaluate_member_reference keeps).
    auto member_reference = [&](Value const& base, Value const& key, Reference& reference) -> bool {
        reference.kind = Reference::Kind::Property;
        reference.base = base;
        reference.key_value = key;
        if (key.is_object())
            return true;
        std::optional<PropertyKey> const converted = self.to_property_key(key);
        if (!converted)
            return false;
        reference.key = *converted;
        reference.key_ready = true;
        return true;
    };
    auto named_reference = [&](Value const& base, JsString* name) {
        Reference reference;
        reference.kind = Reference::Kind::Property;
        reference.base = base;
        reference.key = h.key(name);
        reference.key_ready = true;
        return reference;
    };
    auto iterator_record = [&](std::uint32_t reg) {
        IteratorRecord record;
        record.iterator = frame.registers[reg];
        record.next_method = frame.registers[reg + 1];
        record.done = false;
        return record;
    };
    auto iterator_done = [&](std::uint32_t reg) {
        Value const& done = frame.registers[reg + 2];
        return done.is_boolean() && done.as_boolean();
    };
    auto iter_result_field = [&](Value const& result, JsString* field) -> std::optional<Value> {
        if (!result.is_object())
            return self.throw_type_error("Iterator result " + self.describe(result) + " is not an object");
        return self.get(result, PropertyKey::atom(field));
    };
    auto direct_eval = [&](Args arguments) -> std::optional<Value> {
        // §13.3.6.1 step 6: a direct eval of a string runs in this scope;
        // anything else is returned as it is.
        if (arguments.empty())
            return Value::undefined();
        if (!arguments[0].is_string())
            return arguments[0];
        if (!step())
            return std::nullopt;
        return perform_eval(arguments[0].as_string()->view(), frame.envs.back(), frame.strict, Value::empty(), true, frame.private_environment,
            frame.program);
    };
    auto call_with = [&](Instruction const& ins, Value const& callee, Value const& this_value, Args arguments, bool eval) -> std::optional<Value> {
        if (eval && callee.is_object() && callee.as_object() == self.intrinsics().eval)
            return direct_eval(arguments);
        if (!Interpreter::is_callable(callee)) {
            Context const cx = frame_context(frame);
            return self.throw_type_error(expression_text(code.nodes[ins.b], cx) + " is not a function");
        }
        return self.call(callee, this_value, arguments);
    };

    while (true) {
        Instruction const& ins = code.code[frame.pc++];
        bool ok = true;
        switch (ins.op) {
        // ---- stack
        case Opcode::PushUndefined:
            frame.push(Value::undefined());
            break;
        case Opcode::PushNull:
            frame.push(Value::null());
            break;
        case Opcode::PushTrue:
            frame.push(Value::boolean(true));
            break;
        case Opcode::PushFalse:
            frame.push(Value::boolean(false));
            break;
        case Opcode::PushEmpty:
            frame.push(Value::empty());
            break;
        case Opcode::PushConstant:
            frame.push(code.constants[ins.a]);
            break;
        case Opcode::PushBigInt:
            frame.push(self.bigint(code.bigints[ins.a]));
            break;
        case Opcode::PushInt:
            frame.push(Value::number(static_cast<double>(ins.a)));
            break;
        case Opcode::Pop:
            frame.stack.pop_back();
            break;
        case Opcode::Dup: {
            Value const copy = frame.top();
            frame.push(copy);
            break;
        }
        case Opcode::Over: {
            Value const copy = frame.peek(1);
            frame.push(copy);
            break;
        }
        case Opcode::Swap:
            std::swap(frame.peek(0), frame.peek(1));
            break;
        case Opcode::LoadReg:
            frame.push(frame.registers[ins.a]);
            break;
        case Opcode::StoreReg:
            frame.registers[ins.a] = frame.pop();
            break;
        case Opcode::StoreRegKeep:
            frame.registers[ins.a] = frame.top();
            break;

        // ---- environments
        case Opcode::PushBlockEnv: {
            Environment* env = new_environment(frame.envs.back());
            frame.envs.push_back(env);
            instantiate_block(*code.declarations[ins.a], env, frame.private_environment);
            break;
        }
        case Opcode::PushNamesEnv: {
            Environment* env = new_environment(frame.envs.back());
            frame.envs.push_back(env);
            for (JsString* name : code.name_lists[ins.a])
                env->declare(name, Value::undefined(), ins.b != 0, false);
            break;
        }
        case Opcode::PushWithEnv: {
            // §14.11.2: an object environment marked as a with's, so calls
            // through it get the object as `this`.
            std::optional<Object*> const object = self.to_object(frame.top());
            if (!object) {
                ok = false;
                break;
            }
            Roots const roots(self);
            self.root(Value::object(*object));
            Environment* env = new_environment(frame.envs.back(), *object);
            env->set_with_environment(true);
            frame.stack.pop_back();
            frame.envs.push_back(env);
            break;
        }
        case Opcode::PopEnv:
            frame.envs.pop_back();
            break;
        case Opcode::CopyIterationEnv: {
            // CreatePerIterationEnvironment (§14.7.4.4).
            Environment* previous = frame.envs.back();
            Environment* copy = new_environment(previous->outer());
            for (JsString* name : code.name_lists[ins.a]) {
                Environment::Binding const* binding = previous->find(name);
                copy->declare(name, binding ? binding->value : Value::undefined(), true, binding ? binding->initialized : true);
            }
            frame.envs.back() = copy;
            break;
        }
        case Opcode::InitializeBinding:
            if (!initialize_binding(code.names[ins.a], frame.top(), frame.envs.back())) {
                ok = false;
                break;
            }
            frame.stack.pop_back();
            break;
        case Opcode::LoadArgument:
            frame.push(ins.a < frame.arguments.size() ? frame.arguments[ins.a] : Value::undefined());
            break;
        case Opcode::RestArguments: {
            std::span<Value const> const rest = ins.a < frame.arguments.size()
                ? std::span<Value const>(frame.arguments).subspan(ins.a)
                : std::span<Value const>();
            frame.push(Value::object(self.new_array(rest)));
            break;
        }
        case Opcode::AnnexBCopy: {
            // B.3.2.1 step 2.b: a sloppy block-level function's current value
            // to the var binding the parser hoisted for it.
            JsString* name = code.names[ins.a];
            if (frame.strict || frame.envs.back() == frame.variable)
                break;
            Environment::Binding const* block_binding = frame.envs.back()->find(name);
            if (block_binding == nullptr)
                break;
            Value const value = block_binding->value;
            if (frame.variable->is_object_environment()) {
                if (!self.set(*frame.variable->object(), PropertyKey::atom(name), value, false))
                    ok = false;
            } else if (Environment::Binding* var_binding = frame.variable->find(name)) {
                var_binding->value = value;
            }
            break;
        }
        case Opcode::ResolveThis: {
            std::optional<Value> const value = resolve_this(frame.envs.back());
            if (!value) {
                ok = false;
                break;
            }
            frame.push(*value);
            break;
        }
        case Opcode::NewTarget: {
            Context cx = frame_context(frame);
            frame.push(evaluate_new_target(cx));
            break;
        }

        // ---- references
        case Opcode::RefName:
            frame.refs.push_back(resolve(code.names[ins.a], frame.envs.back()));
            break;
        case Opcode::RefMember: {
            Reference reference;
            if (!member_reference(frame.peek(1), frame.peek(0), reference)) {
                ok = false;
                break;
            }
            frame.stack.pop_back();
            frame.stack.pop_back();
            frame.refs.push_back(reference);
            break;
        }
        case Opcode::RefMemberNamed:
            frame.refs.push_back(named_reference(frame.top(), code.names[ins.a]));
            frame.stack.pop_back();
            break;
        case Opcode::RefSuper: {
            Context const cx = frame_context(frame);
            std::optional<Reference> reference = super_reference(cx, &frame.top(), nullptr);
            if (!reference) {
                ok = false;
                break;
            }
            frame.stack.pop_back();
            frame.refs.push_back(*reference);
            break;
        }
        case Opcode::RefSuperNamed: {
            Context const cx = frame_context(frame);
            std::optional<Reference> reference = super_reference(cx, nullptr, code.names[ins.a]);
            if (!reference) {
                ok = false;
                break;
            }
            frame.refs.push_back(*reference);
            break;
        }
        case Opcode::RefPrivate: {
            // MakePrivateReference (§13.3.3).
            JsString* name = code.names[ins.a];
            Symbol* private_name = frame.private_environment ? frame.private_environment->lookup(name) : nullptr;
            if (private_name == nullptr) {
                self.throw_syntax_error("Private field '" + name->to_utf8() + "' must be declared in an enclosing class");
                ok = false;
                break;
            }
            Reference reference;
            reference.kind = Reference::Kind::Private;
            reference.base = frame.top();
            reference.name = name;
            reference.key = PropertyKey::symbol(private_name);
            reference.key_ready = true;
            frame.stack.pop_back();
            frame.refs.push_back(reference);
            break;
        }
        case Opcode::RefGet: {
            Context const cx = frame_context(frame);
            std::optional<Value> const value = get_value(frame.refs.back(), cx);
            if (!value) {
                ok = false;
                break;
            }
            frame.push(*value);
            break;
        }
        case Opcode::RefPut:
        case Opcode::RefPutKeep: {
            Context const cx = frame_context(frame);
            if (!put_value(frame.refs.back(), frame.top(), cx)) {
                ok = false;
                break;
            }
            frame.refs.pop_back();
            if (ins.op == Opcode::RefPut)
                frame.stack.pop_back();
            break;
        }
        case Opcode::RefThis:
            frame.push(this_for_call(frame.refs.back()));
            break;
        case Opcode::RefDrop:
            frame.refs.pop_back();
            break;
        case Opcode::RefDelete: {
            Context const cx = frame_context(frame);
            std::optional<Value> const value = delete_reference(frame.refs.back(), cx);
            frame.refs.pop_back();
            if (!value) {
                ok = false;
                break;
            }
            frame.push(*value);
            break;
        }
        case Opcode::GetName: {
            Reference reference = resolve(code.names[ins.a], frame.envs.back());
            Context const cx = frame_context(frame);
            std::optional<Value> const value = get_value(reference, cx);
            if (!value) {
                ok = false;
                break;
            }
            frame.push(*value);
            break;
        }
        case Opcode::TypeofName: {
            // §13.5.3: an unresolvable name is "undefined", not an error.
            Reference reference = resolve(code.names[ins.a], frame.envs.back());
            if (reference.kind == Reference::Kind::Unresolvable) {
                frame.push(Value::string(well_known.undefined));
                break;
            }
            Context const cx = frame_context(frame);
            std::optional<Value> const value = get_value(reference, cx);
            if (!value) {
                ok = false;
                break;
            }
            frame.push(Value::string(self.type_of(*value)));
            break;
        }
        case Opcode::GetMemberNamed: {
            Reference reference = named_reference(frame.top(), code.names[ins.a]);
            Context const cx = frame_context(frame);
            std::optional<Value> const value = get_value(reference, cx);
            if (!value) {
                ok = false;
                break;
            }
            frame.top() = *value;
            break;
        }
        case Opcode::GetMember: {
            Reference reference;
            if (!member_reference(frame.peek(1), frame.peek(0), reference)) {
                ok = false;
                break;
            }
            Context const cx = frame_context(frame);
            std::optional<Value> const value = get_value(reference, cx);
            if (!value) {
                ok = false;
                break;
            }
            frame.stack.pop_back();
            frame.top() = *value;
            break;
        }

        // ---- operators
        case Opcode::Binary: {
            std::optional<Value> const result = apply_binary(static_cast<BinaryOp>(ins.a), frame.peek(1), frame.peek(0));
            if (!result) {
                ok = false;
                break;
            }
            frame.stack.pop_back();
            frame.top() = *result;
            break;
        }
        case Opcode::Unary: {
            Value& operand = frame.top();
            switch (static_cast<UnaryOp>(ins.a)) {
            case UnaryOp::Not:
                operand = Value::boolean(!to_boolean(operand));
                break;
            case UnaryOp::Minus: {
                std::optional<Value> const numeric = self.to_numeric(operand);
                if (!numeric) {
                    ok = false;
                    break;
                }
                operand = numeric->is_bigint() ? self.bigint(numeric->as_bigint()->value().negated())
                                               : Value::number(-numeric->as_number());
                break;
            }
            case UnaryOp::Plus: {
                std::optional<double> const number = self.to_number(operand);
                if (!number) {
                    ok = false;
                    break;
                }
                operand = Value::number(*number);
                break;
            }
            case UnaryOp::BitwiseNot: {
                std::optional<Value> const numeric = self.to_numeric(operand);
                if (!numeric) {
                    ok = false;
                    break;
                }
                operand = numeric->is_bigint()
                    ? self.bigint(numeric->as_bigint()->value().bitwise_not())
                    : Value::number(static_cast<double>(~Interpreter::double_to_int32(numeric->as_number())));
                break;
            }
            case UnaryOp::Typeof:
                operand = Value::string(self.type_of(operand));
                break;
            default:
                break;
            }
            break;
        }
        case Opcode::ToNumeric: {
            std::optional<Value> const numeric = self.to_numeric(frame.top());
            if (!numeric) {
                ok = false;
                break;
            }
            frame.top() = *numeric;
            break;
        }
        case Opcode::Inc:
        case Opcode::Dec: {
            // The operand is numeric already (ToNumeric went before).
            Value const& operand = frame.top();
            if (operand.is_bigint()) {
                BigInteger const one = BigInteger::from_int64(1);
                BigInteger const& old = operand.as_bigint()->value();
                frame.top() = self.bigint(ins.op == Opcode::Inc ? old + one : old - one);
            } else {
                frame.top() = Value::number(operand.as_number() + (ins.op == Opcode::Inc ? 1 : -1));
            }
            break;
        }
        case Opcode::ToPropertyKey: {
            std::optional<PropertyKey> const key = self.to_property_key(frame.top());
            if (!key) {
                ok = false;
                break;
            }
            frame.top() = key_to_value(h, *key);
            break;
        }
        case Opcode::ToString: {
            std::optional<JsString*> const text = self.to_string(frame.top());
            if (!text) {
                ok = false;
                break;
            }
            frame.top() = Value::string(*text);
            break;
        }
        case Opcode::StringConcat: {
            JsString* joined = h.string(frame.peek(1).as_string()->data() + frame.peek(0).as_string()->data());
            frame.stack.pop_back();
            frame.top() = Value::string(joined);
            break;
        }
        case Opcode::PrivateIn: {
            Context const cx = frame_context(frame);
            std::optional<Value> const value = private_in(code.names[ins.a], frame.top(), cx);
            if (!value) {
                ok = false;
                break;
            }
            frame.top() = *value;
            break;
        }
        case Opcode::RequireObjectCoercible: {
            Value const& value = frame.top();
            if (value.is_nullish()) {
                self.throw_type_error("Cannot destructure '" + self.describe(value) + "' as it is " + (value.is_null() ? "null" : "undefined") + ".");
                ok = false;
            }
            break;
        }
        case Opcode::ThrowTypeErrorConst:
            self.throw_type_error(code.constants[ins.a].as_string()->to_utf8());
            ok = false;
            break;

        // ---- control
        case Opcode::Jump:
            frame.pc = ins.a;
            break;
        case Opcode::JumpIfTrue:
            if (to_boolean(frame.pop()))
                frame.pc = ins.a;
            break;
        case Opcode::JumpIfFalse:
            if (!to_boolean(frame.pop()))
                frame.pc = ins.a;
            break;
        case Opcode::JumpIfTrueKeep:
            if (to_boolean(frame.top()))
                frame.pc = ins.a;
            break;
        case Opcode::JumpIfFalseKeep:
            if (!to_boolean(frame.top()))
                frame.pc = ins.a;
            break;
        case Opcode::JumpIfNullish:
            if (frame.pop().is_nullish())
                frame.pc = ins.a;
            break;
        case Opcode::JumpIfNotNullishKeep:
            if (!frame.top().is_nullish())
                frame.pc = ins.a;
            break;
        case Opcode::JumpIfNotUndefined:
            if (!frame.pop().is_undefined())
                frame.pc = ins.a;
            break;
        case Opcode::JumpIfEmpty:
            if (frame.pop().is_empty())
                frame.pc = ins.a;
            break;
        case Opcode::Switch: {
            Value const& token = frame.registers[ins.a];
            std::vector<std::uint32_t> const& table = code.jump_tables[ins.b];
            std::size_t const index = token.is_number() ? static_cast<std::size_t>(token.as_number()) : table.size();
            if (index >= table.size()) {
                self.throw_type_error("internal: a finally block was entered with no token");
                ok = false;
                break;
            }
            frame.pc = table[index];
            break;
        }
        case Opcode::JumpIfResumeNormal:
            if (frame.resume_kind == ResumeKind::Normal)
                frame.pc = ins.a;
            break;
        case Opcode::JumpIfResumeReturn:
            if (frame.resume_kind == ResumeKind::Return)
                frame.pc = ins.a;
            break;
        case Opcode::Return:
            frame.result = frame.pop();
            return RunStatus::Completed;
        case Opcode::Throw:
            self.throw_value(frame.pop());
            ok = false;
            break;
        case Opcode::Step:
            if (!step())
                ok = false;
            break;

        // ---- calls
        case Opcode::Call:
        case Opcode::CallEval: {
            std::size_t const argc = ins.a;
            std::size_t const size = frame.stack.size();
            Args const arguments(frame.stack.data() + size - argc, argc);
            std::optional<Value> const result = call_with(ins, frame.stack[size - argc - 2], frame.stack[size - argc - 1], arguments,
                ins.op == Opcode::CallEval);
            if (!result) {
                ok = false;
                break;
            }
            frame.stack.resize(size - argc - 2);
            frame.push(*result);
            break;
        }
        case Opcode::CallArray:
        case Opcode::CallEvalArray: {
            std::size_t const size = frame.stack.size();
            std::vector<Value> const arguments = array_arguments(frame.stack[size - 1]);
            std::optional<Value> const result = call_with(ins, frame.stack[size - 3], frame.stack[size - 2], arguments,
                ins.op == Opcode::CallEvalArray);
            if (!result) {
                ok = false;
                break;
            }
            frame.stack.resize(size - 3);
            frame.push(*result);
            break;
        }
        case Opcode::New:
        case Opcode::NewArray: {
            std::size_t const size = frame.stack.size();
            std::vector<Value> spread_arguments;
            Args arguments;
            std::size_t consumed = 0;
            if (ins.op == Opcode::NewArray) {
                spread_arguments = array_arguments(frame.stack[size - 1]);
                arguments = spread_arguments;
                consumed = 2;
            } else {
                arguments = Args(frame.stack.data() + size - ins.a, ins.a);
                consumed = ins.a + 1;
            }
            Value const& constructor = frame.stack[size - consumed];
            if (!Interpreter::is_constructor(constructor)) {
                Context const cx = frame_context(frame);
                self.throw_type_error(expression_text(code.nodes[ins.b], cx) + " is not a constructor");
                ok = false;
                break;
            }
            std::optional<Value> const result = self.construct(constructor, arguments);
            if (!result) {
                ok = false;
                break;
            }
            frame.stack.resize(size - consumed);
            frame.push(*result);
            break;
        }
        case Opcode::SuperCall:
        case Opcode::SuperCallArray: {
            std::size_t const size = frame.stack.size();
            std::vector<Value> spread_arguments;
            Args arguments;
            std::size_t consumed = 0;
            if (ins.op == Opcode::SuperCallArray) {
                spread_arguments = array_arguments(frame.stack[size - 1]);
                arguments = spread_arguments;
                consumed = 1;
            } else {
                arguments = Args(frame.stack.data() + size - ins.a, ins.a);
                consumed = ins.a;
            }
            Context cx = frame_context(frame);
            std::optional<Value> const result = super_call(cx, arguments);
            if (!result) {
                ok = false;
                break;
            }
            frame.stack.resize(size - consumed);
            frame.push(*result);
            break;
        }

        // ---- literals
        case Opcode::NewArrayLiteral:
            frame.push(Value::object(self.new_array()));
            break;
        case Opcode::ArrayPush: {
            auto* array = static_cast<ArrayObject*>(frame.peek(1).as_object());
            array->push(frame.top());
            frame.stack.pop_back();
            break;
        }
        case Opcode::ArrayHole: {
            auto* array = static_cast<ArrayObject*>(frame.top().as_object());
            array->set_length(array->length() + 1);
            break;
        }
        case Opcode::ArraySpread: {
            // §13.2.4.1: the iterable's values, each an element of its own.
            std::optional<std::vector<Value>> const values = self.iterable_to_list(frame.top());
            if (!values) {
                ok = false;
                break;
            }
            frame.stack.pop_back();
            auto* array = static_cast<ArrayObject*>(frame.top().as_object());
            for (Value const& value : *values)
                array->push(value);
            break;
        }
        case Opcode::NewObject:
            frame.push(Value::object(self.new_object()));
            break;
        case Opcode::SetPrototype: {
            Value const value = frame.pop();
            Object* object = frame.top().as_object();
            if (value.is_object())
                object->set_prototype(value.as_object());
            else if (value.is_null())
                object->set_prototype(nullptr);
            break;
        }
        case Opcode::CopyDataProperties: {
            Object* target = frame.peek(1).as_object();
            if (!copy_data_properties(*target, frame.top(), {})) {
                ok = false;
                break;
            }
            frame.stack.pop_back();
            break;
        }
        case Opcode::CopyDataPropertiesExcluding: {
            auto* taken = static_cast<ArrayObject*>(frame.registers[ins.a].as_object());
            std::vector<PropertyKey> excluded;
            for (std::uint32_t i = 0; i < taken->length(); ++i)
                excluded.push_back(key_from_value(h, taken->element(i)));
            Object* target = frame.peek(1).as_object();
            if (!copy_data_properties(*target, frame.top(), excluded)) {
                ok = false;
                break;
            }
            frame.stack.pop_back();
            break;
        }
        case Opcode::DefinePropertyNamed: {
            Object* object = frame.peek(1).as_object();
            if (!self.create_data_property(*object, h.key(code.names[ins.a]), frame.top())) {
                ok = false;
                break;
            }
            frame.stack.pop_back();
            break;
        }
        case Opcode::DefinePropertyDyn: {
            Object* object = frame.peek(2).as_object();
            PropertyKey const key = key_from_value(h, frame.peek(1));
            if (!self.create_data_property(*object, key, frame.top())) {
                ok = false;
                break;
            }
            frame.stack.pop_back();
            frame.stack.pop_back();
            break;
        }
        case Opcode::DefineMethod:
        case Opcode::DefineMethodDyn: {
            // A method shorthand (§15.4.5): named after its key, with the
            // object as its home for `super.x`.
            bool const dynamic = ins.op == Opcode::DefineMethodDyn;
            Object* object = dynamic ? frame.peek(1).as_object() : frame.top().as_object();
            PropertyKey const key = dynamic ? key_from_value(h, frame.top()) : h.key(code.names[ins.b]);
            Context cx = frame_context(frame);
            std::optional<Value> const value = make_closure(*code.functions[ins.a], cx, &key);
            if (!value) {
                ok = false;
                break;
            }
            Roots const roots(self);
            self.root(*value);
            static_cast<ScriptFunction*>(value->as_object())->set_home_object(object);
            if (!self.create_data_property(*object, key, *value)) {
                ok = false;
                break;
            }
            if (dynamic)
                frame.stack.pop_back();
            break;
        }
        case Opcode::DefineAccessor:
        case Opcode::DefineAccessorDyn: {
            // A getter or setter joins an existing accessor's other half.
            bool const dynamic = ins.op == Opcode::DefineAccessorDyn;
            bool const is_setter = (ins.flags & 1) != 0;
            Object* object = dynamic ? frame.peek(1).as_object() : frame.top().as_object();
            PropertyKey const key = dynamic ? key_from_value(h, frame.top()) : h.key(code.names[ins.b]);
            Heap::NoCollect const no_collect(h);
            ScriptFunction* accessor = self.new_script_function(*code.functions[ins.a], frame.envs.back(), frame.private_environment);
            accessor->set_home_object(object);
            set_function_name(*accessor, key, is_setter ? "set" : "get");
            Object* getter = nullptr;
            Object* setter = nullptr;
            if (Property const* existing = object->find_own(key); existing && existing->accessor) {
                getter = existing->getter;
                setter = existing->setter;
            }
            (is_setter ? setter : getter) = accessor;
            object->put_accessor(key, getter, setter, Enumerable | Configurable);
            if (dynamic)
                frame.stack.pop_back();
            break;
        }
        case Opcode::NewRegExp: {
            std::optional<Value> const value = evaluate_regexp(*code.regexps[ins.a]);
            if (!value) {
                ok = false;
                break;
            }
            frame.push(*value);
            break;
        }
        case Opcode::TemplateObject: {
            std::optional<Object*> const site = template_object(*code.templates[ins.a]);
            if (!site) {
                ok = false;
                break;
            }
            frame.push(Value::object(*site));
            break;
        }
        case Opcode::MakeClosure:
        case Opcode::MakeClosureNamedDyn: {
            bool const dynamic = ins.op == Opcode::MakeClosureNamedDyn;
            PropertyKey key;
            PropertyKey const* name_key = nullptr;
            if (dynamic) {
                key = key_from_value(h, frame.top());
                name_key = &key;
            } else if (ins.b != None) {
                key = h.key(code.names[ins.b]);
                name_key = &key;
            }
            Context cx = frame_context(frame);
            std::optional<Value> const value = make_closure(*code.functions[ins.a], cx, name_key);
            if (!value) {
                ok = false;
                break;
            }
            if (dynamic)
                frame.stack.pop_back();
            frame.push(*value);
            break;
        }
        case Opcode::LoadFieldKey:
            frame.push(frame.field_key);
            break;
        case Opcode::ClassScope:
        case Opcode::ClassScopeNamedDyn: {
            // The class's scope goes on the frame: its environment is the
            // lexical one until ClassFinish, its body strict.
            bool const dynamic = ins.op == Opcode::ClassScopeNamedDyn;
            PropertyKey key;
            PropertyKey const* name_key = nullptr;
            if (dynamic) {
                key = key_from_value(h, frame.top());
                name_key = &key;
            } else if (ins.b != None) {
                key = h.key(code.names[ins.b]);
                name_key = &key;
            }
            ClassBuilder* builder = class_scope(*code.classes[ins.a], frame.envs.back(), frame.private_environment, frame.strict, name_key);
            if (dynamic)
                frame.stack.pop_back();
            frame.builders.push_back(builder);
            frame.envs.push_back(builder->class_env);
            frame.strict = true;
            break;
        }
        case Opcode::ClassBegin:
        case Opcode::ClassBeginHeritage: {
            ClassBuilder& builder = *frame.builders.back();
            Value heritage;
            if (ins.op == Opcode::ClassBeginHeritage)
                heritage = frame.pop();
            if (!class_begin(builder, ins.op == Opcode::ClassBeginHeritage ? &heritage : nullptr)) {
                ok = false;
                break;
            }
            frame.private_environment = builder.private_env; // the class's names, for its keys and bodies
            break;
        }
        case Opcode::ClassElement:
        case Opcode::ClassElementKeyed: {
            ClassBuilder& builder = *frame.builders.back();
            Value key_value;
            if (ins.op == Opcode::ClassElementKeyed)
                key_value = frame.pop();
            if (!class_element(builder, ins.a, ins.op == Opcode::ClassElementKeyed ? &key_value : nullptr))
                ok = false;
            break;
        }
        case Opcode::ClassFinish: {
            ClassBuilder& builder = *frame.builders.back();
            std::optional<Value> const value = class_finish(builder);
            frame.envs.pop_back();
            frame.private_environment = builder.outer_private;
            frame.strict = builder.saved_strict;
            frame.builders.pop_back();
            if (!value) {
                ok = false;
                break;
            }
            frame.push(*value);
            break;
        }
        case Opcode::AppendToReg: {
            auto* array = static_cast<ArrayObject*>(frame.registers[ins.a].as_object());
            array->push(frame.top());
            frame.stack.pop_back();
            break;
        }

        // ---- iteration
        case Opcode::GetIterator:
        case Opcode::GetAsyncIterator: {
            std::optional<IteratorRecord> const record
                = ins.op == Opcode::GetIterator ? self.get_iterator(frame.top()) : self.get_async_iterator(frame.top());
            if (!record) {
                ok = false;
                break;
            }
            frame.top() = record->iterator;
            frame.push(record->next_method);
            break;
        }
        case Opcode::IteratorNextCall: {
            // The async protocol's step: next() called, its answer pushed
            // for the Await that follows; the object check comes after.
            std::optional<Value> const result = self.call(frame.registers[ins.a + 1], frame.registers[ins.a], {});
            if (!result) {
                ok = false;
                break;
            }
            frame.push(*result);
            break;
        }
        case Opcode::IteratorReturnCall:
        case Opcode::IteratorReturnCallQuiet: {
            // AsyncIteratorClose's first half: return() called when there
            // is one, its answer pushed for an Await; Empty when there is
            // none. The quiet form runs under a pending throw, which wins
            // over anything return() does.
            Value const iterator = frame.registers[ins.a];
            bool const quiet = ins.op == Opcode::IteratorReturnCallQuiet;
            Value pending;
            if (quiet)
                pending = frame.top();
            std::optional<Value> const method = self.get_method(iterator, self.key("return"));
            std::optional<Value> result;
            if (method && !method->is_undefined())
                result = self.call(*method, iterator, {});
            if ((!method || (!method->is_undefined() && !result))) {
                if (!quiet || self.m_terminated) {
                    ok = false;
                    break;
                }
                self.take_exception();
                frame.push(Value::empty());
                break;
            }
            frame.push(method->is_undefined() ? Value::empty() : *result);
            break;
        }
        case Opcode::RequireIterResult: {
            if (!frame.top().is_object()) {
                self.throw_type_error("Iterator result " + self.describe(frame.top()) + " is not an object");
                ok = false;
            }
            break;
        }
        case Opcode::IteratorNext: {
            std::optional<Value> const result = self.call(frame.registers[ins.a + 1], frame.registers[ins.a], {});
            if (!result) {
                ok = false;
                break;
            }
            if (!result->is_object()) {
                self.throw_type_error("Iterator result " + self.describe(*result) + " is not an object");
                ok = false;
                break;
            }
            frame.push(*result);
            break;
        }
        case Opcode::IteratorResultDone: {
            std::optional<Value> const done = iter_result_field(frame.top(), well_known.done);
            if (!done) {
                ok = false;
                break;
            }
            frame.top() = Value::boolean(to_boolean(*done));
            break;
        }
        case Opcode::IteratorResultValue: {
            std::optional<Value> const value = iter_result_field(frame.top(), well_known.value);
            if (!value) {
                ok = false;
                break;
            }
            frame.top() = *value;
            break;
        }
        case Opcode::IteratorStep: {
            // IteratorStepValue for a pattern: undefined once the iterator
            // is done; the iterator's own throw marks it done unclosed.
            if (iterator_done(ins.a)) {
                frame.push(Value::undefined());
                break;
            }
            IteratorRecord record = iterator_record(ins.a);
            Value out;
            std::optional<bool> const stepped = self.iterator_step(record, out);
            if (!stepped) {
                frame.registers[ins.a + 2] = Value::boolean(true);
                ok = false;
                break;
            }
            if (!*stepped) {
                frame.registers[ins.a + 2] = Value::boolean(true);
                frame.push(Value::undefined());
                break;
            }
            frame.push(out);
            break;
        }
        case Opcode::IteratorRestArray: {
            ArrayObject* rest = self.new_array();
            Roots const roots(self);
            self.root(Value::object(rest));
            while (!iterator_done(ins.a)) {
                IteratorRecord record = iterator_record(ins.a);
                Value out;
                std::optional<bool> const stepped = self.iterator_step(record, out);
                if (!stepped) {
                    frame.registers[ins.a + 2] = Value::boolean(true);
                    ok = false;
                    break;
                }
                if (!*stepped) {
                    frame.registers[ins.a + 2] = Value::boolean(true);
                    break;
                }
                rest->push(out);
            }
            if (ok)
                frame.push(Value::object(rest));
            break;
        }
        case Opcode::IteratorClose: {
            // IteratorClose on a normal exit: return() runs, and its own
            // failure is the outcome.
            if (iterator_done(ins.a))
                break;
            IteratorRecord const record = iterator_record(ins.a);
            if (!self.iterator_close(record, false))
                ok = false;
            break;
        }
        case Opcode::IteratorCloseThrowing: {
            // IteratorClose with a throw pending — the thrown value is on the
            // stack for the Throw that follows; it stays the outcome whatever
            // return() does.
            if (iterator_done(ins.a))
                break;
            self.throw_value(frame.top());
            IteratorRecord const record = iterator_record(ins.a);
            self.iterator_close(record, true);
            self.throw_value(frame.top());
            break;
        }
        case Opcode::ForInStart: {
            // §14.7.5.6 enumerate: nothing to walk for null or undefined.
            Value const& subject = frame.top();
            Object* object = nullptr;
            if (!subject.is_nullish()) {
                std::optional<Object*> const boxed = self.to_object(subject);
                if (!boxed) {
                    ok = false;
                    break;
                }
                object = *boxed;
            }
            Roots const roots(self);
            if (object)
                self.root(Value::object(object));
            auto* enumerator = h.allocate<ForInIteratorObject>(object);
            frame.top() = Value::object(enumerator);
            if (!enumerator_load(enumerator->enumerator()))
                ok = false;
            break;
        }
        case Opcode::ForInNext: {
            auto* enumerator = static_cast<ForInIteratorObject*>(frame.registers[ins.a].as_object());
            JsString* key = enumerator_next(enumerator->enumerator());
            if (key == nullptr && self.has_exception()) {
                ok = false;
                break;
            }
            frame.push(key ? Value::string(key) : Value::empty());
            break;
        }

        // ---- suspension
        case Opcode::Yield:
            frame.result = frame.pop();
            frame.result_is_iter_result = (ins.flags & 1) != 0;
            frame.resume_pending = true;
            return RunStatus::Yielded;
        case Opcode::Await:
            frame.result = frame.pop();
            frame.resume_pending = true;
            return RunStatus::Awaiting;

        // ---- modules
        case Opcode::ImportCall: {
            // The specifier sits below the options; both stay on the
            // stack — and so traced — until the promise replaces them.
            Value const specifier = frame.peek(1);
            Value const options = frame.peek(0);
            std::optional<Value> const promise = self.perform_import_call(frame.program, specifier, options);
            if (!promise) {
                ok = false;
                break;
            }
            frame.pop();
            frame.top() = *promise;
            break;
        }
        case Opcode::ImportMeta: {
            std::optional<Value> const meta = self.import_meta_for(frame.program);
            if (!meta) {
                ok = false;
                break;
            }
            frame.push(*meta);
            break;
        }
        case Opcode::Nop:
            break;
        }
        if (!ok && !vm_unwind(frame))
            return RunStatus::Threw;
    }
}

// ---- generators -------------------------------------------------------------

// EvaluateGeneratorBody (§15.5.2): the arguments are bound (the caller
// did that), the generator object is made with the function's own
// `prototype` — or the intrinsic when that is not an object — and
// nothing of the body runs until the first next().
std::optional<Value> Interpreter::Impl::start_generator(ScriptFunction& function, Context const& cx)
{
    CodeBlock const* code = compiled_body(function.node());
    if (code == nullptr)
        return std::nullopt;
    Roots const roots(self);
    std::optional<Object*> const prototype = self.get_prototype_from_constructor(&function, self.intrinsics().generator_prototype);
    if (!prototype)
        return std::nullopt;
    self.root(Value::object(*prototype));
    Heap::NoCollect const guard(heap());
    Frame* frame = new_frame(*code, cx);
    GeneratorObject* generator = heap().allocate<GeneratorObject>(*prototype, frame);
    return Value::object(generator);
}

// GeneratorResume and GeneratorResumeAbrupt (§27.5.3.3, §27.5.3.4): the
// body runs to its next yield, its end or a throw; what it produces is
// wrapped as an iterator result unless a yield* handed one through.
std::optional<Value> Interpreter::Impl::generator_resume(GeneratorObject& generator, ResumeKind kind, Value const& value)
{
    Roots const roots(self);
    self.root(Value::object(&generator));
    self.root(value);
    switch (generator.state()) {
    case GeneratorObject::State::Executing:
        return self.throw_type_error("Generator is already running");
    case GeneratorObject::State::Completed:
        if (kind == ResumeKind::Throw)
            return self.throw_value(value);
        return Value::object(self.create_iter_result(kind == ResumeKind::Return ? value : Value::undefined(), true));
    case GeneratorObject::State::SuspendedStart:
        if (kind == ResumeKind::Return) {
            generator.set_state(GeneratorObject::State::Completed);
            generator.release_frame();
            return Value::object(self.create_iter_result(value, true));
        }
        if (kind == ResumeKind::Throw) {
            generator.set_state(GeneratorObject::State::Completed);
            generator.release_frame();
            return self.throw_value(value);
        }
        break;
    case GeneratorObject::State::SuspendedYield:
        break;
    }
    Frame& frame = *generator.frame();
    frame.resume_kind = kind;
    frame.resume_value = value;
    generator.set_state(GeneratorObject::State::Executing);
    RunStatus const status = vm_run(frame);
    switch (status) {
    case RunStatus::Yielded: {
        generator.set_state(GeneratorObject::State::SuspendedYield);
        Value const result = frame.result;
        frame.result = Value::empty();
        if (frame.result_is_iter_result)
            return result;
        self.root(result);
        return Value::object(self.create_iter_result(result, false));
    }
    case RunStatus::Completed: {
        generator.set_state(GeneratorObject::State::Completed);
        Value const result = frame.result;
        self.root(result);
        generator.release_frame();
        return Value::object(self.create_iter_result(result, true));
    }
    case RunStatus::Threw:
        generator.set_state(GeneratorObject::State::Completed);
        generator.release_frame();
        return std::nullopt;
    case RunStatus::Awaiting:
        break;
    }
    generator.set_state(GeneratorObject::State::Completed);
    generator.release_frame();
    return self.throw_syntax_error("await inside a generator body");
}

// ---- async functions ----------------------------------------------------------

// [[Call]] of an async function (§15.8.4 EvaluateAsyncFunctionBody): the
// promise capability comes first, so a throw while the parameters are
// bound rejects the promise rather than propagating.
std::optional<Value> Interpreter::Impl::call_async_function(ScriptFunction& function, Value const& this_argument, std::span<Value const> arguments)
{
    Roots const roots(self);
    std::optional<PromiseCapability> const capability = new_promise_capability(self, Value::object(self.intrinsics().promise_constructor));
    if (!capability)
        return std::nullopt;
    self.root(capability->promise);
    self.root(capability->resolve);
    self.root(capability->reject);
    std::optional<Value> const result = run_script_function(function, this_argument, arguments, nullptr, nullptr, &*capability);
    if (result)
        return *result;
    if (self.m_terminated)
        return std::nullopt;
    Value const thrown = self.take_exception();
    self.root(thrown);
    Value const reject_arguments[1] = { thrown };
    if (!self.call(capability->reject, Value::undefined(), reject_arguments))
        return std::nullopt;
    return capability->promise;
}

// AsyncFunctionStart (§27.7.5.1) and AsyncBlockStart (§27.7.5.2): the body
// runs to its first await, its end or a throw.
std::optional<Value> Interpreter::Impl::start_async(FunctionNode const& node, Context const& cx, PromiseCapability const& capability)
{
    CodeBlock const* code = compiled_body(node);
    if (code == nullptr)
        return std::nullopt;
    Roots const roots(self);
    AsyncContextObject* context = nullptr;
    {
        Heap::NoCollect const guard(heap());
        Frame* frame = new_frame(*code, cx);
        context = heap().allocate<AsyncContextObject>(nullptr, frame, capability.promise, capability.resolve, capability.reject);
    }
    self.root(Value::object(context));
    async_step(*context);
    return capability.promise;
}

// One run of an async body (§27.7.5.1 step 4's closure, and §27.7.5.3
// Await): a completion settles the promise; an await hooks the frame's
// resumption onto the awaited value's promise with reactions that carry
// no capability of their own, and returns to the caller.
void Interpreter::Impl::async_step(AsyncContextObject& context)
{
    Roots const roots(self);
    self.root(Value::object(&context));
    Frame* frame = context.frame();
    if (frame == nullptr)
        return;
    while (true) {
        RunStatus const status = vm_run(*frame);
        if (status == RunStatus::Completed || status == RunStatus::Yielded) {
            Value const result = frame->result;
            self.root(result);
            context.release_frame();
            if (status == RunStatus::Yielded) {
                self.throw_syntax_error("yield inside an async function body");
                Value const thrown = self.take_exception();
                self.root(thrown);
                Value const reject_arguments[1] = { thrown };
                self.call(context.reject(), Value::undefined(), reject_arguments);
                return;
            }
            Value const resolve_arguments[1] = { result };
            self.call(context.resolve(), Value::undefined(), resolve_arguments);
            return;
        }
        if (status == RunStatus::Threw) {
            context.release_frame();
            if (self.m_terminated)
                return;
            Value const thrown = self.take_exception();
            self.root(thrown);
            Value const reject_arguments[1] = { thrown };
            self.call(context.reject(), Value::undefined(), reject_arguments);
            return;
        }
        // Awaiting.
        Value const awaited = frame->result;
        frame->result = Value::empty();
        self.root(awaited);
        std::optional<Value> const promise = promise_resolve(self, Value::object(self.intrinsics().promise_constructor), awaited);
        if (!promise) {
            // PromiseResolve threw (a `constructor` getter): that is the
            // await's own throw.
            if (self.m_terminated) {
                context.release_frame();
                return;
            }
            frame->resume_kind = ResumeKind::Throw;
            frame->resume_value = self.take_exception();
            continue;
        }
        self.root(*promise);
        auto resume = [](ResumeKind kind) {
            return [kind](Interpreter& in, ClosureFunction& self_function, Value const&, Args arguments) -> std::optional<Value> {
                auto* async_context = static_cast<AsyncContextObject*>(self_function.slot(0).as_object());
                if (Frame* resumed = async_context->frame()) {
                    resumed->resume_kind = kind;
                    resumed->resume_value = argument(arguments, 0);
                    in.impl().async_step(*async_context);
                }
                return Value::undefined();
            };
        };
        ClosureFunction* on_fulfilled = self.new_closure("", 1, { Value::object(&context) }, resume(ResumeKind::Normal));
        self.root(Value::object(on_fulfilled));
        ClosureFunction* on_rejected = self.new_closure("", 1, { Value::object(&context) }, resume(ResumeKind::Throw));
        self.root(Value::object(on_rejected));
        perform_then(self, *static_cast<PromiseObject*>(promise->as_object()), Value::object(on_fulfilled), Value::object(on_rejected), std::nullopt);
        return;
    }
}

// ---- async generators (§27.6) ------------------------------------------------

std::optional<Value> Interpreter::Impl::start_async_generator(ScriptFunction& function, Context const& cx)
{
    // §27.6.3.2 AsyncGeneratorStart: the frame waits for the first request.
    CodeBlock const* code = compiled_body(function.node());
    if (code == nullptr)
        return std::nullopt;
    Roots const roots(self);
    std::optional<Object*> const prototype = self.get_prototype_from_constructor(&function, self.intrinsics().async_generator_prototype);
    if (!prototype)
        return std::nullopt;
    self.root(Value::object(*prototype));
    Heap::NoCollect const guard(heap());
    Frame* frame = new_frame(*code, cx);
    auto* generator = heap().allocate<AsyncGeneratorObject>(*prototype, frame);
    return Value::object(generator);
}

// AsyncGeneratorCompleteStep (§27.6.3.3): the front request settled, with
// an iterator result or the throw.
void Interpreter::Impl::async_generator_complete_step(AsyncGeneratorObject& generator, ResumeKind kind, Value const& value, bool done)
{
    Roots const roots(self);
    self.root(Value::object(&generator));
    self.root(value);
    AsyncGeneratorObject::Request const request = generator.queue().front();
    generator.queue().pop_front();
    self.root(request.capability.promise);
    self.root(request.capability.resolve);
    self.root(request.capability.reject);
    if (kind == ResumeKind::Throw) {
        Value const arguments[1] = { value };
        self.call(request.capability.reject, Value::undefined(), arguments);
        return;
    }
    Object* result = self.create_iter_result(value, done);
    Value const arguments[1] = { Value::object(result) };
    self.call(request.capability.resolve, Value::undefined(), arguments);
}

// AsyncGeneratorAwaitReturn (§27.6.3.7): a return request's value is
// awaited, then the generator completes with it — or with its rejection.
void Interpreter::Impl::async_generator_await_return(AsyncGeneratorObject& generator)
{
    Roots const roots(self);
    self.root(Value::object(&generator));
    Value const value = generator.queue().front().value;
    self.root(value);
    std::optional<Value> const promise = promise_resolve(self, Value::object(self.intrinsics().promise_constructor), value);
    if (!promise) {
        if (self.m_terminated)
            return;
        generator.set_state(AsyncGeneratorObject::State::Completed);
        Value const thrown = self.take_exception();
        self.root(thrown);
        async_generator_complete_step(generator, ResumeKind::Throw, thrown, true);
        async_generator_drain_queue(generator);
        return;
    }
    self.root(*promise);
    auto settle = [](ResumeKind kind) {
        return [kind](Interpreter& in, ClosureFunction& self_function, Value const&, Args arguments) -> std::optional<Value> {
            auto* target = static_cast<AsyncGeneratorObject*>(self_function.slot(0).as_object());
            target->set_state(AsyncGeneratorObject::State::Completed);
            in.impl().async_generator_complete_step(*target, kind, argument(arguments, 0), true);
            in.impl().async_generator_drain_queue(*target);
            return Value::undefined();
        };
    };
    ClosureFunction* on_fulfilled = self.new_closure("", 1, { Value::object(&generator) }, settle(ResumeKind::Normal));
    self.root(Value::object(on_fulfilled));
    ClosureFunction* on_rejected = self.new_closure("", 1, { Value::object(&generator) }, settle(ResumeKind::Throw));
    self.root(Value::object(on_rejected));
    perform_then(self, *static_cast<PromiseObject*>(promise->as_object()), Value::object(on_fulfilled), Value::object(on_rejected), std::nullopt);
}

// AsyncGeneratorDrainQueue (§27.6.3.8): once the body is done, every
// waiting request is answered — done, with its throw, or, for a return,
// after its value has been awaited (which stops the drain until then).
void Interpreter::Impl::async_generator_drain_queue(AsyncGeneratorObject& generator)
{
    Roots const roots(self);
    self.root(Value::object(&generator));
    while (!generator.queue().empty()) {
        AsyncGeneratorObject::Request const& request = generator.queue().front();
        if (request.kind == ResumeKind::Return) {
            generator.set_state(AsyncGeneratorObject::State::AwaitingReturn);
            async_generator_await_return(generator);
            return;
        }
        ResumeKind const kind = request.kind;
        Value const value = request.value;
        self.root(value);
        async_generator_complete_step(generator, kind, kind == ResumeKind::Throw ? value : Value::undefined(), true);
    }
}

// The body run to its next yield, its end, a throw or an await
// (AsyncGeneratorStart's closure, §27.6.3.2; AsyncGeneratorYield's
// completing and immediate resumption, §27.6.3.8; Await, §27.7.5.3).
void Interpreter::Impl::async_generator_step(AsyncGeneratorObject& generator)
{
    Roots const roots(self);
    self.root(Value::object(&generator));
    Frame* frame = generator.frame();
    if (frame == nullptr)
        return;
    while (true) {
        RunStatus const status = vm_run(*frame);
        if (status == RunStatus::Yielded) {
            Value const value = frame->result;
            frame->result = Value::empty();
            self.root(value);
            async_generator_complete_step(generator, ResumeKind::Normal, value, false);
            if (generator.queue().empty()) {
                generator.set_state(AsyncGeneratorObject::State::SuspendedYield);
                return;
            }
            // A request that arrived while the body ran resumes it at once
            // (the resumption's return value is awaited by the body itself).
            AsyncGeneratorObject::Request const& next = generator.queue().front();
            frame->resume_kind = next.kind;
            frame->resume_value = next.value;
            continue;
        }
        if (status == RunStatus::Completed) {
            generator.set_state(AsyncGeneratorObject::State::Completed);
            Value const result = frame->result;
            self.root(result);
            generator.release_frame();
            async_generator_complete_step(generator, ResumeKind::Normal, result, true);
            async_generator_drain_queue(generator);
            return;
        }
        if (status == RunStatus::Threw) {
            generator.set_state(AsyncGeneratorObject::State::Completed);
            generator.release_frame();
            if (self.m_terminated)
                return;
            Value const thrown = self.take_exception();
            self.root(thrown);
            async_generator_complete_step(generator, ResumeKind::Throw, thrown, true);
            async_generator_drain_queue(generator);
            return;
        }
        // Awaiting, as an async function does.
        Value const awaited = frame->result;
        frame->result = Value::empty();
        self.root(awaited);
        std::optional<Value> const promise = promise_resolve(self, Value::object(self.intrinsics().promise_constructor), awaited);
        if (!promise) {
            if (self.m_terminated) {
                generator.set_state(AsyncGeneratorObject::State::Completed);
                generator.release_frame();
                return;
            }
            frame->resume_kind = ResumeKind::Throw;
            frame->resume_value = self.take_exception();
            continue;
        }
        self.root(*promise);
        auto resume = [](ResumeKind kind) {
            return [kind](Interpreter& in, ClosureFunction& self_function, Value const&, Args arguments) -> std::optional<Value> {
                auto* target = static_cast<AsyncGeneratorObject*>(self_function.slot(0).as_object());
                if (Frame* resumed = target->frame()) {
                    resumed->resume_kind = kind;
                    resumed->resume_value = argument(arguments, 0);
                    in.impl().async_generator_step(*target);
                }
                return Value::undefined();
            };
        };
        ClosureFunction* on_fulfilled = self.new_closure("", 1, { Value::object(&generator) }, resume(ResumeKind::Normal));
        self.root(Value::object(on_fulfilled));
        ClosureFunction* on_rejected = self.new_closure("", 1, { Value::object(&generator) }, resume(ResumeKind::Throw));
        self.root(Value::object(on_rejected));
        perform_then(self, *static_cast<PromiseObject*>(promise->as_object()), Value::object(on_fulfilled), Value::object(on_rejected), std::nullopt);
        return;
    }
}

// AsyncGeneratorResume (§27.6.3.4).
void Interpreter::Impl::async_generator_resume(AsyncGeneratorObject& generator, ResumeKind kind, Value const& value)
{
    Frame* frame = generator.frame();
    if (frame == nullptr)
        return;
    generator.set_state(AsyncGeneratorObject::State::Executing);
    frame->resume_kind = kind;
    frame->resume_value = value;
    async_generator_step(generator);
}

// %AsyncGeneratorPrototype%.next, return and throw past their validation
// (§27.6.1.2–.4): a generator that is done answers at once; otherwise the
// request is queued and, by the generator's state, run now, awaited now
// (a return before the body ever ran), or left for its turn.
std::optional<Value> Interpreter::Impl::async_generator_enqueue(AsyncGeneratorObject& generator, ResumeKind kind, Value const& value,
    PromiseCapability const& capability)
{
    using State = AsyncGeneratorObject::State;
    Roots const roots(self);
    self.root(Value::object(&generator));
    self.root(value);
    self.root(capability.promise);
    self.root(capability.resolve);
    self.root(capability.reject);
    State state = generator.state();
    if (kind == ResumeKind::Normal && state == State::Completed) {
        Object* result = self.create_iter_result(Value::undefined(), true);
        Value const arguments[1] = { Value::object(result) };
        if (!self.call(capability.resolve, Value::undefined(), arguments))
            return std::nullopt;
        return capability.promise;
    }
    if (kind == ResumeKind::Throw && state == State::SuspendedStart) {
        generator.set_state(State::Completed);
        generator.release_frame();
        state = State::Completed;
    }
    if (kind == ResumeKind::Throw && state == State::Completed) {
        Value const arguments[1] = { value };
        if (!self.call(capability.reject, Value::undefined(), arguments))
            return std::nullopt;
        return capability.promise;
    }
    generator.queue().push_back(AsyncGeneratorObject::Request { kind, value, capability });
    if (kind == ResumeKind::Return && (state == State::SuspendedStart || state == State::Completed)) {
        generator.set_state(State::AwaitingReturn);
        generator.release_frame();
        async_generator_await_return(generator);
    } else if (state == State::SuspendedStart || state == State::SuspendedYield) {
        async_generator_resume(generator, kind, value);
    }
    return capability.promise;
}

}
