#include "js/Interpreter.h"

// The evaluator: the mechanisms every script runs on — name resolution,
// references, environments, the function objects' [[Call]] and
// [[Construct]] (§10.2), classes in steps for the bytecode machine, declaration
// instantiation for scripts, functions, blocks and eval (§16.1.7,
// §10.2.11, §14.2.3, §19.2.1.3), and the realm's life cycle. The abstract
// operations it leans on are in Conversions.cpp; the library in the
// Runtime*.cpp files.
//
// Two conventions run through every function here. A throw is a return
// value — nullopt, or false — with the thrown value pending in the
// interpreter. And any Value a C++
// frame keeps across an allocation is rooted first, because the heap may
// collect inside any allocation (and does, at every one, under the tests'
// stress mode); environments are reached through the context stack, which
// is traced as a root.

#include "js/Ast.h"
#include "js/Evaluator.h"
#include "js/Module.h"
#include "js/Object.h"
#include "js/Parser.h"
#include "js/Runtime.h"
#include "js/Strings.h"
#include "js/Vm.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sashfold::js {

namespace {


// A mapped arguments object (§10.4.4): the indices that correspond to
// formal parameters read and write the parameters' bindings, until a
// delete or a redefinition unmaps them.
class ArgumentsObject : public Object {
public:
    ArgumentsObject(Object* prototype, Environment* environment, std::vector<JsString*> mapped)
        : Object(prototype, Class::Arguments)
        , m_environment(environment)
        , m_mapped(std::move(mapped))
    {
    }

    std::optional<PropertyDescriptor> get_own_property(PropertyKey const& key) const override
    {
        // §10.4.4.1: the ordinary descriptor with the live parameter value.
        std::optional<PropertyDescriptor> desc = Object::get_own_property(key);
        if (desc && mapped_name(key))
            desc->value = binding_value(key);
        return desc;
    }

    bool define_own_property(PropertyKey const& key, PropertyDescriptor const& desc) override
    {
        // §10.4.4.2: a value written through the descriptor reaches the
        // parameter; an accessor, or writable going false, ends the mapping.
        JsString* name = mapped_name(key);
        PropertyDescriptor new_desc = desc;
        if (name && desc.is_data() && !desc.value && desc.writable && !*desc.writable)
            new_desc.value = binding_value(key);
        if (!Object::define_own_property(key, new_desc))
            return false;
        if (name) {
            if (desc.is_accessor()) {
                unmap(key);
            } else {
                if (desc.value)
                    set_binding_value(key, *desc.value);
                if (desc.writable && !*desc.writable)
                    unmap(key);
            }
        }
        return true;
    }

    std::optional<Value> get(Interpreter& interpreter, PropertyKey const& key, Value const& receiver) override
    {
        // §10.4.4.3.
        if (mapped_name(key))
            return binding_value(key);
        return Object::get(interpreter, key, receiver);
    }

    std::optional<bool> set(Interpreter& interpreter, PropertyKey const& key, Value const& value, Value const& receiver) override
    {
        // §10.4.4.4: a write through this object itself lands on the
        // parameter as well as on the property.
        if (receiver.is_object() && receiver.as_object() == this && mapped_name(key))
            set_binding_value(key, value);
        return Object::set(interpreter, key, value, receiver);
    }

    bool delete_property(PropertyKey const& key) override
    {
        // §10.4.4.5.
        bool const deleted = Object::delete_property(key);
        if (deleted && mapped_name(key))
            unmap(key);
        return deleted;
    }

    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_environment);
    }

private:
    JsString* mapped_name(PropertyKey const& key) const
    {
        if (!key.is_index() || key.as_index() >= m_mapped.size())
            return nullptr;
        return m_mapped[key.as_index()];
    }
    Value binding_value(PropertyKey const& key) const
    {
        Environment::Binding const* binding = m_environment->find(mapped_name(key));
        return binding ? binding->value : Value::undefined();
    }
    void set_binding_value(PropertyKey const& key, Value const& value)
    {
        if (Environment::Binding* binding = m_environment->find(mapped_name(key)))
            binding->value = value;
    }
    void unmap(PropertyKey const& key) { m_mapped[key.as_index()] = nullptr; }

    Environment* m_environment;
    std::vector<JsString*> m_mapped;
};

// Where the C++ stack stands, for the budget check: the frame address of
// the caller, which every compiler this project builds with provides.
char const* stack_position()
{
    return static_cast<char const*>(__builtin_frame_address(0));
}


bool contains(std::vector<JsString*> const& names, JsString* name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

// ------------------------------------------------------------------ Impl

// ---- limits
// The C++ stack against the budget, measured from where script was
// entered: a deep recursion is a RangeError before it is a crash.
bool Interpreter::Impl::stack_ok()
{
    char const* here = stack_position();
    if (self.m_stack_base == nullptr)
        self.m_stack_base = here;
    std::ptrdiff_t const used = self.m_stack_base > here ? self.m_stack_base - here : here - self.m_stack_base;
    if (static_cast<std::size_t>(used) > self.m_stack_budget) {
        self.throw_range_error("Maximum call stack size exceeded");
        return false;
    }
    return true;
}


bool Interpreter::stack_ok()
{
    return m_impl->stack_ok();
}


bool Interpreter::Impl::step()
{
    ++self.m_steps;
    if (self.m_should_stop && self.m_steps % self.m_interrupt_interval == 0 && self.m_should_stop()) {
        self.m_terminated = true;
        self.throw_error(ErrorType::RangeError, "script terminated");
        return false;
    }
    return true;
}


// ---- messages
std::string Interpreter::Impl::expression_text(Expression const* expression, Context const& cx)
{
    if (!cx.program || expression->end_offset <= expression->position.offset)
        return "expression";
    std::u16string_view const source = cx.program->source;
    std::size_t const start = expression->position.offset;
    std::size_t const end = std::min<std::size_t>(expression->end_offset, source.size());
    if (end <= start)
        return "expression";
    std::string text = utf8_from_utf16(source.substr(start, end - start));
    if (text.size() > 60)
        text = text.substr(0, 57) + "...";
    return text;
}


// ---- environments
Environment* Interpreter::Impl::new_environment(Environment* outer, Object* object)
{
    return heap().allocate<Environment>(outer, object);
}


// The nearest environment with a `this`; the global object's has one.
// A derived constructor's is a ReferenceError until super() has run.
std::optional<Value> Interpreter::Impl::resolve_this(Environment* environment)
{
    for (Environment* e = environment; e != nullptr; e = e->outer()) {
        if (e->has_this()) {
            if (!e->this_initialized())
                return self.throw_reference_error("Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
            return e->this_value();
        }
    }
    return Value::object(self.global());
}


// GetThisEnvironment (§9.4.3): the record that owns `this`, or null
// when none does (global code).
Environment* Interpreter::Impl::this_environment(Environment* environment)
{
    for (Environment* e = environment; e != nullptr; e = e->outer()) {
        if (e->has_this())
            return e;
    }
    return nullptr;
}


// ResolveBinding (§9.4.2): outward through the chain. A `with`
// object's @@unscopables is not consulted yet.
Reference Interpreter::Impl::resolve(JsString* name, Environment* environment)
{
    Reference reference;
    reference.name = name;
    for (Environment* e = environment; e != nullptr; e = e->outer()) {
        if (e->is_object_environment()) {
            PropertyKey const key = PropertyKey::atom(name);
            // HasBinding (§9.1.1.2.1) is [[HasProperty]] on the object, so
            // a `with` over a proxy runs the handler's `has` trap and may
            // throw. The name then resolves to nothing with that exception
            // pending, and GetValue and PutValue hand it on rather than
            // reporting the name undefined.
            std::optional<bool> const has = self.has_property(*e->object(), key);
            if (!has)
                break;
            if (*has) {
                reference.kind = Reference::Kind::ObjectEnvironment;
                reference.environment = e;
                reference.key = key;
                return reference;
            }
        } else if (e->find(name)) {
            reference.kind = Reference::Kind::Binding;
            reference.environment = e;
            return reference;
        }
    }
    reference.kind = Reference::Kind::Unresolvable;
    return reference;
}


bool Interpreter::Impl::ensure_key(Reference& reference)
{
    if (reference.key_ready)
        return true;
    std::optional<PropertyKey> const key = self.to_property_key(reference.key_value);
    if (!key)
        return false;
    reference.key = *key;
    reference.key_ready = true;
    return true;
}


std::string Interpreter::Impl::reference_key_text(Reference const& reference)
{
    return reference.key_ready ? key_description(reference.key) : self.describe(reference.key_value);
}


// GetValue (§6.2.5.5).
std::optional<Value> Interpreter::Impl::get_value(Reference& reference, Context const& cx)
{
    switch (reference.kind) {
    case Reference::Kind::Value:
        return reference.base;
    case Reference::Kind::Unresolvable:
        // A `with` object's [[HasProperty]] may have thrown while the
        // reference was being resolved; that throw is the outcome, not a
        // ReferenceError about the name.
        if (self.has_exception())
            return std::nullopt;
        return self.throw_reference_error(reference.name->to_utf8() + " is not defined");
    case Reference::Kind::Binding: {
        Environment::Binding const* binding = reference.environment->find(reference.name);
        if (binding == nullptr)
            return self.throw_reference_error(reference.name->to_utf8() + " is not defined");
        // An import binding reads the exporting module's binding, live
        // (§9.1.1.5.1 GetBindingValue): its dead zone crosses the import,
        // and a module whose environment does not exist yet is one too.
        for (int hops = 0; binding->import_module != nullptr && hops < 64; ++hops) {
            Environment* target = binding->import_module->environment();
            Environment::Binding const* target_binding = target != nullptr ? target->find(binding->import_name) : nullptr;
            if (target_binding == nullptr)
                return self.throw_reference_error("Cannot access '" + reference.name->to_utf8() + "' before initialization");
            binding = target_binding;
        }
        if (!binding->initialized)
            return self.throw_reference_error("Cannot access '" + reference.name->to_utf8() + "' before initialization");
        return binding->value;
    }
    case Reference::Kind::ObjectEnvironment: {
        // §9.1.1.2.6: the property may be gone by now; strict code
        // notices, sloppy code reads undefined.
        Object* object = reference.environment->object();
        std::optional<bool> const has = self.has_property(*object, reference.key);
        if (!has)
            return std::nullopt;
        if (!*has) {
            if (cx.strict)
                return self.throw_reference_error(reference.name->to_utf8() + " is not defined");
            return Value::undefined();
        }
        return self.get(*object, reference.key);
    }
    case Reference::Kind::Property: {
        if (reference.base.is_nullish())
            return self.throw_type_error("Cannot read properties of " + std::string(reference.base.is_null() ? "null" : "undefined")
                + " (reading '" + reference_key_text(reference) + "')");
        if (!ensure_key(reference))
            return std::nullopt;
        return self.get(reference.base, reference.key);
    }
    case Reference::Kind::Super: {
        // §13.3.7.3: the home object's prototype, `this` receiving.
        if (reference.base.is_nullish())
            return self.throw_type_error("Cannot read properties of null (reading '" + reference_key_text(reference) + "')");
        if (!ensure_key(reference))
            return std::nullopt;
        return reference.base.as_object()->get(self, reference.key, reference.this_value);
    }
    case Reference::Kind::Private:
        return private_get(reference.base, reference.key);
    }
    return Value::undefined();
}


// PutValue (§6.2.5.6).
bool Interpreter::Impl::put_value(Reference& reference, Value const& value, Context const& cx)
{
    switch (reference.kind) {
    case Reference::Kind::Value:
        self.throw_reference_error("Invalid left-hand side in assignment");
        return false;
    case Reference::Kind::Unresolvable: {
        // As in GetValue: a `with` object's `has` trap may already have
        // thrown while the name was being resolved.
        if (self.has_exception())
            return false;
        if (cx.strict) {
            self.throw_reference_error(reference.name->to_utf8() + " is not defined");
            return false;
        }
        return self.set(*self.global(), PropertyKey::atom(reference.name), value, false).has_value();
    }
    case Reference::Kind::Binding: {
        // SetMutableBinding (§9.1.1.1.5).
        Environment::Binding* binding = reference.environment->find(reference.name);
        if (binding == nullptr) {
            if (cx.strict) {
                self.throw_reference_error(reference.name->to_utf8() + " is not defined");
                return false;
            }
            reference.environment->declare(reference.name, value, true, true, true);
            return true;
        }
        if (!binding->initialized) {
            self.throw_reference_error("Cannot access '" + reference.name->to_utf8() + "' before initialization");
            return false;
        }
        if (!binding->mutable_) {
            if (binding->strict || cx.strict) {
                self.throw_type_error("Assignment to constant variable.");
                return false;
            }
            return true;
        }
        binding->value = value;
        return true;
    }
    case Reference::Kind::ObjectEnvironment: {
        // §9.1.1.2.5.
        Object* object = reference.environment->object();
        std::optional<bool> const has = self.has_property(*object, reference.key);
        if (!has)
            return false;
        if (!*has && cx.strict) {
            self.throw_reference_error(reference.name->to_utf8() + " is not defined");
            return false;
        }
        return self.set(*object, reference.key, value, cx.strict).has_value();
    }
    case Reference::Kind::Property: {
        if (reference.base.is_nullish()) {
            self.throw_type_error("Cannot set properties of " + std::string(reference.base.is_null() ? "null" : "undefined")
                + " (setting '" + reference_key_text(reference) + "')");
            return false;
        }
        if (!ensure_key(reference))
            return false;
        return self.set(reference.base, reference.key, value, cx.strict).has_value();
    }
    case Reference::Kind::Private:
        return private_set(reference.base, reference.key, value);
    case Reference::Kind::Super: {
        if (reference.base.is_nullish()) {
            self.throw_type_error("Cannot set properties of null (setting '" + reference_key_text(reference) + "')");
            return false;
        }
        if (!ensure_key(reference))
            return false;
        std::optional<bool> const stored = reference.base.as_object()->set(self, reference.key, value, reference.this_value);
        if (!stored)
            return false;
        if (!*stored && cx.strict) {
            self.throw_type_error("Cannot assign to read only property '" + key_description(reference.key) + "' of object");
            return false;
        }
        return true;
    }
    }
    return false;
}


// The `this` a call through this reference gets (§13.3.6.2).
Value Interpreter::Impl::this_for_call(Reference const& reference)
{
    if (reference.kind == Reference::Kind::Super)
        return reference.this_value;
    if (reference.kind == Reference::Kind::Property || reference.kind == Reference::Kind::Private)
        return reference.base;
    if (reference.kind == Reference::Kind::ObjectEnvironment && reference.environment->is_with_environment())
        return Value::object(reference.environment->object());
    return Value::undefined();
}


// ---- functions
// SetFunctionName (§10.2.9): the `name` own property, with the
// accessor prefix when there is one; a symbol names as [description].
void Interpreter::Impl::set_function_name(Object& function, PropertyKey const& key, std::string_view prefix)
{
    std::u16string name;
    if (key.is_symbol()) {
        JsString const* description = key.as_symbol()->description();
        if (key.as_symbol()->is_private())
            name = description->data(); // §10.2.9 step 2.a: a Private Name's description as it is, `#x`
        else if (description)
            name = u"[" + description->data() + u"]";
    } else {
        name = heap().key_to_string(key)->data();
    }
    if (!prefix.empty())
        name = utf16_from_utf8(prefix) + u" " + name;
    Heap::NoCollect const guard(heap());
    function.put(PropertyKey::atom(atoms().name), Value::string(heap().string(std::move(name))), Configurable);
}


// The closure for a function expression or arrow (§15.2.5, §15.3.4).
// A named function expression binds its own name, immutably, in an
// environment of its own between the closure and its scope; generator
// and async function expressions too (§15.5.5, §15.8.5). A method's
// name is its key, not a binding.
std::optional<Value> Interpreter::Impl::make_closure(FunctionNode const& node, Context& cx, PropertyKey const* name_key)
{
    Heap::NoCollect const guard(heap());
    Environment* scope = cx.lexical;
    if (!node.is_arrow && !node.is_method && node.name != nullptr) {
        scope = new_environment(cx.lexical);
        Environment::Binding& binding = scope->declare(node.name, Value::undefined(), false, true);
        binding.strict = false;
    }
    ScriptFunction* function = self.new_script_function(node, scope, cx.private_environment);
    if (scope != cx.lexical)
        scope->find(node.name)->value = Value::object(function);
    if (name_key && node.name == nullptr)
        set_function_name(*function, *name_key);
    return Value::object(function);
}


// CreateUnmappedArgumentsObject / CreateMappedArgumentsObject
// (§10.4.4.6, §10.4.4.7). The mapped form aliases the parameters for
// sloppy functions with simple, distinct parameter lists; both carry
// %Array.prototype.values% as their @@iterator.
Object* Interpreter::Impl::make_arguments_object(ScriptFunction& function, Environment* environment, std::span<Value const> arguments, bool mapped)
{
    Heap::NoCollect const guard(heap());
    FunctionNode const& node = function.node();
    Object* object = nullptr;
    if (mapped) {
        // Only a simple list is mapped, so every parameter is a name.
        std::vector<JsString*> names(std::min(node.parameters.size(), arguments.size()), nullptr);
        for (std::size_t i = 0; i < names.size(); ++i)
            names[i] = node.parameters[i].name;
        object = heap().allocate<ArgumentsObject>(self.intrinsics().object_prototype, environment, std::move(names));
    } else {
        object = heap().allocate<Object>(self.intrinsics().object_prototype, Object::Class::Arguments);
    }
    for (std::size_t i = 0; i < arguments.size(); ++i)
        object->put(PropertyKey::index(static_cast<std::uint32_t>(i)), arguments[i]);
    object->put(PropertyKey::atom(atoms().length), Value::number(static_cast<double>(arguments.size())), builtin_attributes);
    object->put(PropertyKey::symbol(atoms().symbol_iterator), Value::object(self.intrinsics().array_prototype_values), builtin_attributes);
    if (mapped)
        object->put(PropertyKey::atom(atoms().callee), Value::object(&function), builtin_attributes);
    else
        object->put_accessor(PropertyKey::atom(atoms().callee), self.intrinsics().throw_type_error, self.intrinsics().throw_type_error, 0);
    return object;
}


// [[Call]] and [[Construct]] of a script function share this:
// PrepareForOrdinaryCall, OrdinaryCallBindThis and
// FunctionDeclarationInstantiation (§10.2.1, §10.2.1.2, §10.2.11), then
// the body.
std::optional<Value> Interpreter::Impl::call_script_function(ScriptFunction& function, Value const& this_argument,
    std::span<Value const> arguments, Object* new_target, PropertyKey const* field_key)
{
    FunctionNode const& node = function.node();
    if (node.is_async && !node.is_generator)
        return call_async_function(function, this_argument, arguments);
    return run_script_function(function, this_argument, arguments, new_target, field_key, nullptr);
}

std::optional<Value> Interpreter::Impl::run_script_function(ScriptFunction& function, Value const& this_argument,
    std::span<Value const> arguments, Object* new_target, PropertyKey const* field_key, PromiseCapability const* async_capability)
{
    FunctionNode const& node = function.node();
    Roots const roots(self);
    self.root(Value::object(&function));
    self.root(this_argument);
    for (Value const& argument : arguments)
        self.root(argument);

    Environment* variable = new_environment(function.scope());
    variable->set_function(&function);
    ContextScope scope(*this, Context { variable, variable, node.program, &function, node.is_strict, function.private_environment() });
    Context& cx = scope.context();

    if (!node.is_arrow) {
        if (node.is_derived_constructor) {
            // A derived constructor's `this` waits for super() (§10.2.1.1).
            variable->set_this_uninitialized();
        } else {
            // OrdinaryCallBindThis: sloppy code sees the global object for
            // a nullish `this` and a wrapper for a primitive one.
            Value this_value = this_argument;
            if (!node.is_strict) {
                if (this_value.is_nullish()) {
                    this_value = Value::object(self.global());
                } else if (!this_value.is_object()) {
                    std::optional<Object*> const boxed = self.to_object(this_value);
                    if (!boxed)
                        return std::nullopt;
                    this_value = Value::object(*boxed);
                }
            }
            variable->set_this(this_value);
        }
        variable->set_new_target(new_target);
        // A base class constructor defines its fields on the fresh
        // instance before anything else runs (§10.2.2 step 6.b).
        if (node.is_class_constructor && !node.is_derived_constructor && new_target != nullptr && this_argument.is_object()) {
            if (!initialize_instance_elements(*this_argument.as_object(), function))
                return std::nullopt;
        }
    }

    // Parameters (steps 21–26). A simple list binds each name to its
    // argument, the last occurrence of a duplicated name winning; any
    // other list declares every name uninitialised first, so that a
    // default reading a later parameter finds its dead zone, and
    // binds them in order once the arguments object exists.
    std::vector<JsString*> parameter_names;
    collect_parameter_names(node, parameter_names);
    // Sloppy code with parameter expressions binds them in a record of
    // their own (step 20), so that a direct eval in a default declares
    // its vars in the callee's record beneath and finds the parameters
    // in between — a var named like one is its SyntaxError
    // (EvalDeclarationInstantiation step 3.d).
    Environment* parameter_env = variable;
    if (node.has_parameter_expressions && !node.is_strict) {
        parameter_env = new_environment(variable);
        cx.lexical = parameter_env;
    }
    if (node.has_simple_parameter_list) {
        for (std::size_t i = 0; i < node.parameters.size(); ++i) {
            Value const value = i < arguments.size() ? arguments[i] : Value::undefined();
            if (Environment::Binding* existing = variable->find(node.parameters[i].name))
                existing->value = value;
            else
                variable->declare(node.parameters[i].name, value);
        }
    } else {
        for (JsString* name : parameter_names)
            parameter_env->declare(name, Value::undefined(), true, false);
    }

    // The arguments object, when the body can reach it and nothing of
    // its own shadows it — a body declaration only does so without
    // parameter expressions (step 18); mapped only for a sloppy simple list.
    if (!node.is_arrow && (node.uses_arguments || node.has_direct_eval)) {
        bool shadowed = contains(parameter_names, atoms().arguments);
        if (!node.has_parameter_expressions) {
            for (FunctionDeclaration const* declaration : node.declarations.functions) {
                if (declaration->function->name == atoms().arguments)
                    shadowed = true;
            }
            for (auto const& [name, is_const] : node.declarations.lexicals) {
                if (name == atoms().arguments)
                    shadowed = true;
            }
        }
        if (!shadowed) {
            bool const mapped = !node.is_strict && node.has_simple_parameter_list && !node.has_duplicate_parameters;
            Object* arguments_object = make_arguments_object(function, parameter_env, arguments, mapped);
            parameter_env->declare(atoms().arguments, Value::object(arguments_object), !node.is_strict, true);
        }
    }

    if (!node.has_simple_parameter_list && !run_parameter_block(node, parameter_env, arguments, cx))
        return std::nullopt;

    // Vars (steps 27–28): in the parameters' record unless a default
    // or a computed key could have made a closure there, in which case
    // they get a record of their own that such a closure cannot see;
    // a var named like a parameter starts with the parameter's value.
    Environment* var_env = variable;
    if (node.has_parameter_expressions) {
        var_env = new_environment(parameter_env);
        var_env->set_var_scope();
        cx.variable = var_env;
        for (JsString* name : node.declarations.vars) {
            if (var_env->find(name))
                continue;
            Environment::Binding const* parameter = parameter_env->find(name);
            var_env->declare(name, parameter ? parameter->value : Value::undefined());
        }
    } else {
        for (JsString* name : node.declarations.vars) {
            if (!variable->find(name))
                variable->declare(name, Value::undefined());
        }
    }

    // Top-level lexicals live in their own record when there are any,
    // so a direct eval's `var` can tell them apart from the vars.
    Environment* lexical = var_env;
    if (!node.declarations.lexicals.empty())
        lexical = new_environment(var_env);
    cx.lexical = lexical;
    for (FunctionDeclaration const* declaration : node.declarations.functions) {
        ScriptFunction* closure = self.new_script_function(*declaration->function, lexical, cx.private_environment);
        JsString* name = declaration->function->name;
        if (Environment::Binding* existing = var_env->find(name))
            existing->value = Value::object(closure);
        else
            var_env->declare(name, Value::object(closure));
    }
    for (auto const& [name, is_const] : node.declarations.lexicals)
        lexical->declare(name, Value::undefined(), !is_const, false);

    // A body that can suspend gets a frame of its own: a generator's
    // waits for its first next() (§15.5.2, §15.6.2 for the async kind), an
    // async function's runs to its first await (§15.8.4) — each with this
    // call's environments.
    if (node.is_generator)
        return node.is_async ? start_async_generator(function, cx) : start_generator(function, cx);
    if (async_capability != nullptr)
        return start_async(node, cx, *async_capability);

    // The body. A field initializer is its expression, named after
    // the field when it is an anonymous function; a default derived
    // constructor hands its arguments to the parent class.
    std::optional<Value> value;
    if (node.is_default_constructor) {
        if (node.is_derived_constructor) {
            Object* parent = function.prototype();
            Value const parent_value = parent ? Value::object(parent) : Value::undefined();
            if (!Interpreter::is_constructor(parent_value))
                return self.throw_type_error("Super constructor " + self.describe(parent_value) + " of anonymous class is not a constructor");
            std::optional<Value> const result = self.construct(parent_value, arguments, new_target);
            if (!result)
                return std::nullopt;
            variable->set_this(*result);
            if (!initialize_instance_elements(*result->as_object(), function))
                return std::nullopt;
        }
        value = Value::undefined();
    } else {
        // The body compiled and run on the bytecode machine, on the
        // environments this call's prologue made; a field initializer's
        // key rides on the frame, to name an anonymous function after.
        value = run_compiled_body(function, cx, field_key);
        if (!value)
            return std::nullopt;
    }
    // [[Construct]] of a derived class (§10.2.2 steps 10–12): an object
    // returned is the result, anything else but undefined a TypeError,
    // and undefined yields the `this` that super() bound.
    if (new_target != nullptr && node.is_derived_constructor) {
        if (value->is_object())
            return *value;
        if (!value->is_undefined())
            return self.throw_type_error("Derived constructors may only return object or undefined");
        if (!variable->this_initialized())
            return self.throw_reference_error("Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
        return variable->this_value();
    }
    return *value;
}


// ---- classes
// The [[HomeObject]] of the running method: the nearest non-arrow
// function's, arrows looking outward for theirs (§9.4.3).
Object* Interpreter::Impl::home_object_of(Context const& cx)
{
    for (Environment* e = cx.lexical; e != nullptr; e = e->outer()) {
        Function* function = e->function();
        if (function == nullptr)
            continue;
        auto* script = static_cast<ScriptFunction*>(function);
        if (script->node().is_arrow)
            continue;
        return script->home_object();
    }
    return nullptr;
}


std::optional<Reference> Interpreter::Impl::super_reference(Context const& cx, Value const* key_value, JsString* name)
{
    std::optional<Value> const this_value = resolve_this(cx.lexical);
    if (!this_value)
        return std::nullopt;
    Roots const roots(self);
    self.root(*this_value);
    Object* home = home_object_of(cx);
    if (home == nullptr)
        return self.throw_syntax_error("'super' keyword unexpected here");
    Reference reference;
    reference.kind = Reference::Kind::Super;
    reference.this_value = *this_value;
    Object* base = home->prototype();
    reference.base = base ? Value::object(base) : Value::null();
    if (key_value) {
        reference.key_value = *key_value;
        if (!key_value->is_object()) {
            std::optional<PropertyKey> const key = self.to_property_key(*key_value);
            if (!key)
                return std::nullopt;
            reference.key = *key;
            reference.key_ready = true;
        }
    } else {
        reference.key = heap().key(name);
        reference.key_ready = true;
    }
    return reference;
}


std::optional<Value> Interpreter::Impl::super_call(Context& cx, std::span<Value const> arguments)
{
    Environment* this_env = this_environment(cx.lexical);
    if (this_env == nullptr || this_env->function() == nullptr)
        return self.throw_syntax_error("'super' keyword unexpected here");
    auto* active = static_cast<ScriptFunction*>(this_env->function());
    Object* new_target = this_env->new_target();
    Roots const roots(self);
    Object* parent = active->prototype();
    Value const parent_value = parent ? Value::object(parent) : Value::undefined();
    self.root(parent_value);
    if (!Interpreter::is_constructor(parent_value))
        return self.throw_type_error("Super constructor " + self.describe(parent_value) + " of anonymous class is not a constructor");
    if (new_target == nullptr)
        return self.throw_syntax_error("'super' keyword unexpected here");
    std::optional<Value> const result = self.construct(parent_value, arguments, new_target);
    if (!result)
        return std::nullopt;
    self.root(*result);
    if (this_env->this_initialized())
        return self.throw_reference_error("Super constructor may only be called once");
    this_env->set_this(*result);
    if (!initialize_instance_elements(*result->as_object(), *active))
        return std::nullopt;
    return *result;
}


// NewTarget (§13.3.12.1): the running function's, undefined when it
// was called rather than constructed, and in global code.
Value Interpreter::Impl::evaluate_new_target(Context& cx)
{
    Environment* this_env = this_environment(cx.lexical);
    if (this_env == nullptr || this_env->new_target() == nullptr)
        return Value::undefined();
    return Value::object(this_env->new_target());
}


// A field initializer runs as a call with the instance as `this`, the
// field's key naming an anonymous function it evaluates to.
std::optional<Value> Interpreter::Impl::call_field_initializer(ScriptFunction& initializer, Value const& this_value, PropertyKey const& key)
{
    if (self.m_call_depth >= self.m_call_depth_limit)
        return self.throw_range_error("Maximum call stack size exceeded");
    if (!step() || !stack_ok())
        return std::nullopt;
    ++self.m_call_depth;
    std::optional<Value> const result = call_script_function(initializer, this_value, {}, nullptr, &key);
    --self.m_call_depth;
    return result;
}


// ---- private names (§7.3.30–§7.3.33)
//
// A private element is a property keyed by the class's Private Name,
// found on the object alone — never up the prototype chain — and never
// listed (Object::own_keys skips the key). A private method is a frozen
// data property, so a write to it is the TypeError the specification
// asks for; a private accessor is an accessor.
std::string Interpreter::Impl::private_name_text(PropertyKey const& key)
{
    JsString const* description = key.as_symbol()->description();
    return description ? description->to_utf8() : std::string("#");
}


// PrivateGet (§7.3.32).
std::optional<Value> Interpreter::Impl::private_get(Value const& base, PropertyKey const& key)
{
    Property const* element = base.is_object() ? base.as_object()->find_own(key) : nullptr;
    if (element == nullptr)
        return self.throw_type_error("Cannot read private member " + private_name_text(key) + " from an object whose class did not declare it");
    if (!element->accessor)
        return element->value;
    if (element->getter == nullptr)
        return self.throw_type_error("'" + private_name_text(key) + "' was defined without a getter");
    return self.call(Value::object(element->getter), base, {});
}


// PrivateSet (§7.3.33).
bool Interpreter::Impl::private_set(Value const& base, PropertyKey const& key, Value const& value)
{
    Property* element = base.is_object() ? base.as_object()->find_own(key) : nullptr;
    if (element == nullptr) {
        self.throw_type_error("Cannot write private member " + private_name_text(key) + " to an object whose class did not declare it");
        return false;
    }
    if (element->accessor) {
        if (element->setter == nullptr) {
            self.throw_type_error("'" + private_name_text(key) + "' was defined without a setter");
            return false;
        }
        Value const arguments[1] = { value };
        return self.call(Value::object(element->setter), base, arguments).has_value();
    }
    if (!element->writable()) {
        self.throw_type_error("Private method '" + private_name_text(key) + "' is not writable");
        return false;
    }
    element->value = value;
    return true;
}


// PrivateFieldAdd (§7.3.30): once per object, and never on an object
// that is no longer extensible (the 2025 rule) — a constructor that
// returns an object already carrying the field, or a sealed one, is
// refused.
bool Interpreter::Impl::private_field_add(Object& object, PropertyKey const& key, Value const& value)
{
    if (!object.is_extensible()) {
        self.throw_type_error("Cannot define private field " + private_name_text(key) + " on a non-extensible object");
        return false;
    }
    if (object.find_own(key) != nullptr) {
        self.throw_type_error("Cannot initialize " + private_name_text(key) + " twice on the same object");
        return false;
    }
    object.put(key, value, Writable);
    return true;
}


// PrivateMethodOrAccessorAdd (§7.3.31).
bool Interpreter::Impl::private_method_add(Object& object, PrivateMethod const& method)
{
    PropertyKey const key = PropertyKey::symbol(method.name);
    if (!object.is_extensible()) {
        self.throw_type_error("Cannot define private method " + private_name_text(key) + " on a non-extensible object");
        return false;
    }
    if (object.find_own(key) != nullptr) {
        self.throw_type_error("Cannot initialize private methods of class twice on the same object");
        return false;
    }
    if (method.method != nullptr)
        object.put(key, Value::object(method.method), frozen_attributes);
    else
        object.put_accessor(key, method.getter, method.setter, frozen_attributes);
    return true;
}


std::optional<Value> Interpreter::Impl::private_in(JsString* description, Value const& right, Context const& cx)
{
    if (!right.is_object())
        return self.throw_type_error("Cannot use 'in' operator to search for '" + description->to_utf8() + "' in " + self.describe(right));
    Symbol* name = cx.private_environment ? cx.private_environment->lookup(description) : nullptr;
    if (name == nullptr)
        return self.throw_syntax_error("Private field '" + description->to_utf8() + "' must be declared in an enclosing class");
    return Value::boolean(right.as_object()->find_own(PropertyKey::symbol(name)) != nullptr);
}


// DefineField (§7.3.33).
bool Interpreter::Impl::define_field(Object& receiver, ClassField const& field)
{
    Roots const roots(self);
    self.root(Value::object(&receiver));
    self.root(field.key.is_symbol() ? Value::symbol(field.key.as_symbol()) : Value::undefined());
    Value value = Value::undefined();
    if (field.initializer) {
        self.root(Value::object(field.initializer));
        std::optional<Value> const result = call_field_initializer(*field.initializer, Value::object(&receiver), field.key);
        if (!result)
            return false;
        value = *result;
    }
    self.root(value);
    if (field.key.is_symbol() && field.key.as_symbol()->is_private())
        return private_field_add(receiver, field.key, value);
    return self.create_data_property(receiver, field.key, value).has_value();
}


// InitializeInstanceElements (§7.3.34): the constructor's private
// methods and accessors, then its fields, in order.
bool Interpreter::Impl::initialize_instance_elements(Object& instance, ScriptFunction& constructor)
{
    Roots const roots(self);
    self.root(Value::object(&constructor));
    std::vector<PrivateMethod> const methods = constructor.private_methods();
    for (PrivateMethod const& method : methods) {
        if (!private_method_add(instance, method))
            return false;
    }
    std::vector<ClassField> const fields = constructor.fields();
    for (ClassField const& field : fields) {
        if (!define_field(instance, field))
            return false;
    }
    return true;
}


// The constructor function of a class (§15.7.14 steps 14–17): its
// [[Prototype]] is the parent class, its `prototype` the class's
// prototype object, immutable, and the two point at each other.
ScriptFunction* Interpreter::Impl::new_class_constructor(FunctionNode const& node, Environment* scope, Object* proto, Object* constructor_parent)
{
    Heap::NoCollect const guard(heap());
    auto* function = heap().allocate<ScriptFunction>(constructor_parent, node, scope, true);
    function->put(PropertyKey::atom(atoms().length), Value::number(static_cast<double>(node.expected_argument_count)), Configurable);
    function->put(PropertyKey::atom(atoms().name), Value::string(node.name ? node.name : atoms().empty), Configurable);
    function->put(PropertyKey::atom(atoms().prototype), Value::object(proto), frozen_attributes);
    proto->put(PropertyKey::atom(atoms().constructor), Value::object(function), builtin_attributes);
    function->set_home_object(proto);
    return function;
}


// ClassDefinitionEvaluation (§15.7.14): the heritage, the prototype
// object and the constructor, then every element in order — methods
// and accessors defined at once, instance fields recorded on the
// constructor, static fields and blocks run last with the class as
// `this` — the class's own name bound throughout its body.
// The class's scope (§15.7.14 steps 1–3): a record with the class name
// uninitialized, strict from here to the end; the private names come
// only with the heritage settled, since the heritage sees the outer ones.
ClassBuilder* Interpreter::Impl::class_scope(ClassNode const& node, Environment* outer, PrivateEnvironment* outer_private,
    bool outer_strict, PropertyKey const* name_key)
{
    Heap::NoCollect const guard(heap());
    ClassBuilder* builder = heap().allocate<ClassBuilder>();
    builder->node = &node;
    builder->class_env = new_environment(outer);
    if (node.name)
        builder->class_env->declare(node.name, Value::undefined(), false, false);
    builder->outer_private = outer_private;
    builder->private_env = outer_private;
    builder->saved_strict = outer_strict;
    if (name_key) {
        builder->name_key = *name_key;
        builder->has_name_key = true;
    }
    return builder;
}

// Steps 4–14 with the heritage evaluated: the prototype's parent and the
// constructor's from it (null extends nothing), the class's Private
// Names — a fresh cell per declared `#x`, chained to the enclosing
// class's, in scope for everything the body makes — then the prototype
// and the constructor.
bool Interpreter::Impl::class_begin(ClassBuilder& builder, Value const* heritage)
{
    Roots const roots(self);
    ClassNode const& node = *builder.node;
    Object* proto_parent = self.intrinsics().object_prototype;
    Object* constructor_parent = self.intrinsics().function_prototype;
    if (node.has_heritage) {
        Value const superclass = heritage ? *heritage : Value::undefined();
        self.root(superclass);
        if (superclass.is_null()) {
            proto_parent = nullptr;
        } else if (!Interpreter::is_constructor(superclass)) {
            self.throw_type_error("Class extends value " + self.describe(superclass) + " is not a constructor or null");
            return false;
        } else {
            std::optional<Value> const parent_proto = self.get(superclass, PropertyKey::atom(atoms().prototype));
            if (!parent_proto)
                return false;
            if (parent_proto->is_object()) {
                proto_parent = parent_proto->as_object();
            } else if (parent_proto->is_null()) {
                proto_parent = nullptr;
            } else {
                self.throw_type_error("Class extends value does not have valid prototype property " + self.describe(*parent_proto));
                return false;
            }
            constructor_parent = superclass.as_object();
        }
    }
    bool has_private = false;
    for (ClassElement const& element : node.elements)
        has_private = has_private || element.is_private;
    if (has_private) {
        builder.private_env = heap().allocate<PrivateEnvironment>(builder.outer_private);
        for (ClassElement const& element : node.elements) {
            if (!element.is_private)
                continue;
            bool known = false;
            for (auto const& [description, name] : builder.private_env->names())
                known = known || description == element.key;
            if (!known)
                builder.private_env->add(element.key, heap().private_symbol(element.key));
        }
    }
    Object* proto = self.new_object();
    proto->set_prototype(proto_parent);
    builder.proto = proto;
    ScriptFunction* constructor = new_class_constructor(*node.constructor, builder.class_env, proto, constructor_parent);
    constructor->set_private_environment(builder.private_env);
    builder.constructor = constructor;
    if (!node.name && builder.has_name_key)
        set_function_name(*constructor, builder.name_key);
    return true;
}

// One element (steps 15–24), its computed key evaluated: a method or an
// accessor defined on the prototype or the class, a private one kept on
// the constructor's list, an instance field recorded for the
// constructor, a static field or block kept for the end.
bool Interpreter::Impl::class_element(ClassBuilder& builder, std::size_t index, Value const* key_value)
{
    Roots const roots(self);
    ClassElement const& element = builder.node->elements[index];
    Environment* const class_env = builder.class_env;
    PrivateEnvironment* const private_env = builder.private_env;
    ScriptFunction* const constructor = builder.constructor;
    Object* const proto = builder.proto;
    Object* target = element.is_static ? static_cast<Object*>(constructor) : proto;
    PropertyKey key;
    if (element.kind != ClassElement::Kind::StaticBlock) {
        if (element.computed_key) {
            Value const value = key_value ? *key_value : Value::undefined();
            self.root(value);
            std::optional<PropertyKey> const converted = self.to_property_key(value);
            if (!converted)
                return false;
            key = *converted;
            if (key.is_symbol())
                self.root(Value::symbol(key.as_symbol()));
        } else if (element.is_private) {
            key = PropertyKey::symbol(private_env->lookup(element.key));
        } else {
            key = heap().key(element.key);
        }
    }
    switch (element.kind) {
    case ClassElement::Kind::Method: {
        Heap::NoCollect const guard(heap());
        ScriptFunction* closure = self.new_script_function(*element.function, class_env, private_env);
        closure->set_home_object(target);
        set_function_name(*closure, key);
        if (element.is_private) {
            // A private method (§15.7.14 steps 22–23): a static one goes
            // on the class now; an instance one waits on the constructor
            // for each new object.
            PrivateMethod method;
            method.name = key.as_symbol();
            method.method = closure;
            if (!element.is_static)
                constructor->private_methods().push_back(method);
            else if (!private_method_add(*constructor, method))
                return false;
            return true;
        }
        // DefineMethodProperty: writable, configurable, not enumerable —
        // through validation, so a key the target already holds as
        // non-configurable (a static `['prototype']`) is a TypeError.
        return self.define_property_or_throw(*target, key, PropertyDescriptor::data(Value::object(closure), builtin_attributes)).has_value();
    }
    case ClassElement::Kind::Getter:
    case ClassElement::Kind::Setter: {
        Heap::NoCollect const guard(heap());
        ScriptFunction* accessor = self.new_script_function(*element.function, class_env, private_env);
        accessor->set_home_object(target);
        bool const is_getter = element.kind == ClassElement::Kind::Getter;
        set_function_name(*accessor, key, is_getter ? "get" : "set");
        if (element.is_private && !element.is_static) {
            // An instance private accessor joins its other half on the
            // constructor's list (§15.7.14 step 24).
            PrivateMethod* method = nullptr;
            for (PrivateMethod& candidate : constructor->private_methods()) {
                if (candidate.name == key.as_symbol())
                    method = &candidate;
            }
            if (method == nullptr) {
                constructor->private_methods().push_back(PrivateMethod {});
                method = &constructor->private_methods().back();
                method->name = key.as_symbol();
            }
            (is_getter ? method->getter : method->setter) = accessor;
            return true;
        }
        Object* getter = nullptr;
        Object* setter = nullptr;
        if (Property const* existing = target->find_own(key); existing && existing->accessor) {
            getter = existing->getter;
            setter = existing->setter;
        }
        if (is_getter)
            getter = accessor;
        else
            setter = accessor;
        // A static private accessor is a frozen private element of the
        // class; a public one is configurable, and defined through
        // validation so a non-configurable key on the target throws.
        if (element.is_private) {
            target->put_accessor(key, getter, setter, frozen_attributes);
            return true;
        }
        return self.define_property_or_throw(*target, key, PropertyDescriptor::accessor(getter, setter, Configurable)).has_value();
    }
    case ClassElement::Kind::Field: {
        if (element.is_static) {
            builder.statics.push_back({ &element, key });
            return true;
        }
        Heap::NoCollect const guard(heap());
        ClassField field;
        field.key = key;
        if (element.function) {
            field.initializer = self.new_script_function(*element.function, class_env, private_env);
            field.initializer->set_home_object(proto);
        }
        constructor->fields().push_back(field);
        return true;
    }
    case ClassElement::Kind::StaticBlock:
        builder.statics.push_back({ &element, PropertyKey() });
        return true;
    }
    return true;
}

// Steps 25–31: the name bound in the class scope, then the static fields
// and blocks in order with the class as `this`; the constructor is the
// class's value.
std::optional<Value> Interpreter::Impl::class_finish(ClassBuilder& builder)
{
    Roots const roots(self);
    ClassNode const& node = *builder.node;
    ScriptFunction* const constructor = builder.constructor;
    if (node.name) {
        Environment::Binding* binding = builder.class_env->find(node.name);
        binding->value = Value::object(constructor);
        binding->initialized = true;
    }
    for (ClassBuilder::StaticElement const& item : builder.statics) {
        ScriptFunction* closure = nullptr;
        if (item.element->function) {
            Heap::NoCollect const guard(heap());
            closure = self.new_script_function(*item.element->function, builder.class_env, builder.private_env);
            closure->set_home_object(constructor);
        }
        if (closure)
            self.root(Value::object(closure));
        if (item.element->kind == ClassElement::Kind::StaticBlock) {
            if (!self.call(Value::object(closure), Value::object(constructor), {}))
                return std::nullopt;
            continue;
        }
        ClassField field;
        field.key = item.key;
        field.initializer = closure;
        if (!define_field(*constructor, field))
            return std::nullopt;
    }
    return Value::object(constructor);
}


// BoundNames (§8.2.1) of a target.
void Interpreter::Impl::collect_bound_names(Expression const* target, std::vector<JsString*>& out)
{
    if (target->type == NodeType::Identifier) {
        out.push_back(static_cast<Identifier const*>(target)->name);
    } else if (target->type == NodeType::ArrayPattern) {
        auto const& pattern = *static_cast<ArrayPattern const*>(target);
        for (PatternElement const& element : pattern.elements) {
            if (element.target)
                collect_bound_names(element.target, out);
        }
        if (pattern.rest)
            collect_bound_names(pattern.rest, out);
    } else if (target->type == NodeType::ObjectPattern) {
        auto const& pattern = *static_cast<ObjectPattern const*>(target);
        for (PatternProperty const& property : pattern.properties)
            collect_bound_names(property.target, out);
        if (pattern.rest)
            collect_bound_names(pattern.rest, out);
    }
    // A member expression binds nothing.
}


void Interpreter::Impl::collect_bound_names(JsString* name, Expression const* pattern, std::vector<JsString*>& out)
{
    if (name)
        out.push_back(name);
    else if (pattern)
        collect_bound_names(pattern, out);
}


void Interpreter::Impl::collect_parameter_names(FunctionNode const& node, std::vector<JsString*>& out)
{
    for (Parameter const& parameter : node.parameters)
        collect_bound_names(parameter.name, parameter.pattern, out);
}


// InitializeReferencedBinding for a name declared, uninitialised, in
// `env` or one of its outers.
bool Interpreter::Impl::initialize_binding(JsString* name, Value const& value, Environment* env)
{
    Environment::Binding* binding = nullptr;
    for (Environment* e = env; e != nullptr && binding == nullptr; e = e->outer())
        binding = e->find(name);
    if (binding == nullptr) {
        self.throw_reference_error(name->to_utf8() + " is not defined");
        return false;
    }
    binding->value = value;
    binding->initialized = true;
    return true;
}


// CopyDataProperties (§7.3.25): the source's own enumerable properties,
// read one by one into data properties of the target; null and
// undefined copy nothing; the excluded keys are skipped.
bool Interpreter::Impl::copy_data_properties(Object& target, Value const& source, std::span<PropertyKey const> excluded)
{
    if (source.is_nullish())
        return true;
    Roots const roots(self);
    self.root(source);
    std::optional<Object*> const from = self.to_object(source);
    if (!from)
        return false;
    self.root(Value::object(*from));
    std::optional<std::vector<PropertyKey>> const keys = self.own_keys(**from);
    if (!keys)
        return false;
    for (PropertyKey const& key : *keys) {
        if (std::find(excluded.begin(), excluded.end(), key) != excluded.end())
            continue;
        if (key.is_symbol())
            self.root(Value::symbol(key.as_symbol()));
        std::optional<std::optional<PropertyDescriptor>> const desc = self.get_own_property(**from, key);
        if (!desc)
            return false;
        if (!*desc || !(*desc)->enumerable.value_or(false))
            continue;
        std::optional<Value> const value = self.get(**from, key);
        if (!value)
            return false;
        self.root(*value);
        if (!self.create_data_property(target, key, *value))
            return false;
    }
    return true;
}


// ---- declaration instantiation
// GlobalDeclarationInstantiation (§16.1.7).
bool Interpreter::Impl::global_declaration_instantiation(Program const& program, Context& cx)
{
    Object* global = self.global();
    for (auto const& [name, is_const] : program.declarations.lexicals) {
        std::string const text = name->to_utf8();
        if (realm().var_names.contains(name) || realm().global_lexical->find(name))
            { self.throw_syntax_error("Identifier '" + text + "' has already been declared"); return false; }
        // HasRestrictedGlobalProperty: a non-configurable global
        // property (undefined, NaN, Infinity) cannot be shadowed.
        std::optional<PropertyDescriptor> const existing = global->get_own_property(PropertyKey::atom(name));
        if (existing && !existing->configurable.value_or(false))
            { self.throw_syntax_error("Identifier '" + text + "' has already been declared"); return false; }
    }
    std::vector<JsString*> var_names;
    for (JsString* name : program.declarations.vars) {
        if (realm().global_lexical->find(name))
            { self.throw_syntax_error("Identifier '" + name->to_utf8() + "' has already been declared"); return false; }
        var_names.push_back(name);
    }
    // Functions, last declaration of a name first; each must be
    // declarable on the global object (§9.1.1.4.16).
    std::vector<FunctionDeclaration const*> functions;
    std::vector<JsString*> function_names;
    for (auto it = program.declarations.functions.rbegin(); it != program.declarations.functions.rend(); ++it) {
        JsString* name = (*it)->function->name;
        if (contains(function_names, name))
            continue;
        if (realm().global_lexical->find(name))
            { self.throw_syntax_error("Identifier '" + name->to_utf8() + "' has already been declared"); return false; }
        std::optional<PropertyDescriptor> const existing = global->get_own_property(PropertyKey::atom(name));
        bool declarable = true;
        if (!existing)
            declarable = global->is_extensible();
        else if (!existing->configurable.value_or(false))
            declarable = existing->is_data() && existing->writable.value_or(false) && existing->enumerable.value_or(false);
        if (!declarable)
            { self.throw_type_error("Cannot redefine global function '" + name->to_utf8() + "'"); return false; }
        function_names.push_back(name);
        functions.push_back(*it);
    }
    for (JsString* name : var_names) {
        if (contains(function_names, name))
            continue;
        if (!global->get_own_property(PropertyKey::atom(name)) && !global->is_extensible())
            { self.throw_type_error("Cannot define global variable '" + name->to_utf8() + "'"); return false; }
    }
    for (auto const& [name, is_const] : program.declarations.lexicals)
        realm().global_lexical->declare(name, Value::undefined(), !is_const, false);
    for (FunctionDeclaration const* declaration : functions) {
        JsString* name = declaration->function->name;
        ScriptFunction* closure = self.new_script_function(*declaration->function, realm().global_lexical, cx.private_environment);
        Roots const roots(self);
        self.root(Value::object(closure));
        if (!create_global_function_binding(name, Value::object(closure), false))
            return false;
    }
    for (JsString* name : var_names) {
        if (!create_global_var_binding(name, false))
            return false;
    }
    (void)cx;
    return true;
}


// CreateGlobalFunctionBinding (§9.1.1.4.18).
bool Interpreter::Impl::create_global_function_binding(JsString* name, Value const& value, bool deletable)
{
    Object* global = self.global();
    PropertyKey const key = PropertyKey::atom(name);
    std::optional<PropertyDescriptor> const existing = global->get_own_property(key);
    PropertyDescriptor desc;
    if (!existing || existing->configurable.value_or(false)) {
        desc = PropertyDescriptor::data(value, static_cast<std::uint8_t>(Writable | Enumerable | (deletable ? Configurable : 0)));
    } else {
        desc.value = value;
    }
    if (!self.define_property_or_throw(*global, key, desc))
        return false;
    if (!self.set(*global, key, value, false))
        return false;
    realm().var_names.insert(name);
    return true;
}


// CreateGlobalVarBinding (§9.1.1.4.17).
bool Interpreter::Impl::create_global_var_binding(JsString* name, bool deletable)
{
    Object* global = self.global();
    PropertyKey const key = PropertyKey::atom(name);
    if (!global->get_own_property(key) && global->is_extensible()) {
        PropertyDescriptor const desc = PropertyDescriptor::data(Value::undefined(), static_cast<std::uint8_t>(Writable | Enumerable | (deletable ? Configurable : 0)));
        if (!self.define_property_or_throw(*global, key, desc))
            return false;
    }
    realm().var_names.insert(name);
    return true;
}


// BlockDeclarationInstantiation (§14.2.3), with B.3.2.1's hoisting
// already settled by the parser.
void Interpreter::Impl::instantiate_block(Declarations const& declarations, Environment* environment, PrivateEnvironment* private_environment)
{
    for (auto const& [name, is_const] : declarations.lexicals)
        environment->declare(name, Value::undefined(), !is_const, false);
    for (FunctionDeclaration const* declaration : declarations.functions) {
        JsString* name = declaration->function->name;
        ScriptFunction* closure = self.new_script_function(*declaration->function, environment, private_environment);
        if (Environment::Binding* existing = environment->find(name))
            existing->value = Value::object(closure);
        else
            environment->declare(name, Value::object(closure));
    }
}


// EvalDeclarationInstantiation (§19.2.1.3).
bool Interpreter::Impl::eval_declaration_instantiation(Program const& program, Environment* variable, Environment* lexical, bool strict,
    PrivateEnvironment* private_environment)
{
    bool const global_scope = variable->is_object_environment();
    if (!strict) {
        std::vector<JsString*> names = program.declarations.vars;
        for (FunctionDeclaration const* declaration : program.declarations.functions)
            names.push_back(declaration->function->name);
        if (global_scope) {
            for (JsString* name : names) {
                if (realm().global_lexical->find(name))
                    { self.throw_syntax_error("Identifier '" + name->to_utf8() + "' has already been declared"); return false; }
            }
        }
        // A var may not cross a lexical declaration of the same name
        // between here and the var scope.
        for (Environment* e = lexical->outer(); e != nullptr && e != variable; e = e->outer()) {
            if (e->is_object_environment())
                continue;
            for (JsString* name : names) {
                if (e->find(name))
                    { self.throw_syntax_error("Identifier '" + name->to_utf8() + "' has already been declared"); return false; }
            }
        }
    }
    std::vector<FunctionDeclaration const*> functions;
    std::vector<JsString*> function_names;
    for (auto it = program.declarations.functions.rbegin(); it != program.declarations.functions.rend(); ++it) {
        JsString* name = (*it)->function->name;
        if (contains(function_names, name))
            continue;
        if (global_scope) {
            std::optional<PropertyDescriptor> const existing = self.global()->get_own_property(PropertyKey::atom(name));
            bool declarable = true;
            if (!existing)
                declarable = self.global()->is_extensible();
            else if (!existing->configurable.value_or(false))
                declarable = existing->is_data() && existing->writable.value_or(false) && existing->enumerable.value_or(false);
            if (!declarable)
                { self.throw_type_error("Cannot redefine global function '" + name->to_utf8() + "'"); return false; }
        }
        function_names.push_back(name);
        functions.push_back(*it);
    }
    for (JsString* name : program.declarations.vars) {
        if (global_scope && !contains(function_names, name)) {
            if (!self.global()->get_own_property(PropertyKey::atom(name)) && !self.global()->is_extensible())
                { self.throw_type_error("Cannot define global variable '" + name->to_utf8() + "'"); return false; }
        }
    }
    for (auto const& [name, is_const] : program.declarations.lexicals)
        lexical->declare(name, Value::undefined(), !is_const, false);
    for (FunctionDeclaration const* declaration : functions) {
        JsString* name = declaration->function->name;
        ScriptFunction* closure = self.new_script_function(*declaration->function, lexical, private_environment);
        Roots const roots(self);
        self.root(Value::object(closure));
        if (global_scope) {
            if (!create_global_function_binding(name, Value::object(closure), true))
                return false;
        } else if (Environment::Binding* existing = variable->find(name)) {
            existing->value = Value::object(closure);
        } else {
            variable->declare(name, Value::object(closure), true, true, true);
        }
    }
    for (JsString* name : program.declarations.vars) {
        if (global_scope) {
            if (!create_global_var_binding(name, true))
                return false;
        } else if (!variable->find(name)) {
            variable->declare(name, Value::undefined(), true, true, true);
        }
    }
    return true;
}


// The var environment a direct eval declares into: the nearest
// function environment, or the global object's.
Environment* Interpreter::Impl::variable_environment_of(Environment* environment)
{
    for (Environment* e = environment; e != nullptr; e = e->outer()) {
        if (e->function() != nullptr || e->is_var_scope())
            return e;
        if (e->is_object_environment() && !e->is_with_environment())
            return e;
    }
    return self.intrinsics().global_environment;
}


// PerformEval (§19.2.1.1) for both the direct and the indirect form.
std::optional<Value> Interpreter::Impl::perform_eval(std::u16string_view source, Environment* scope, bool strict_caller, Value this_value, bool direct,
    PrivateEnvironment* private_environment, Program const* caller)
{
    // PerformEval step 2: HostEnsureCanCompileStrings.
    if (self.on_compile_strings) {
        if (std::optional<std::string> const refused = self.on_compile_strings())
            return self.throw_error(ErrorType::EvalError, *refused);
    }
    ParseOptions options;
    options.strict = direct && strict_caller;
    if (direct) {
        // §19.2.1.1 step 6: the eval code may name the private names of
        // every class body it runs inside.
        for (PrivateEnvironment const* env = private_environment; env != nullptr; env = env->outer()) {
            for (auto const& [description, name] : env->names())
                options.private_names.emplace_back(description->view());
        }
    }
    Environment* variable = direct ? variable_environment_of(scope) : self.intrinsics().global_environment;
    if (direct) {
        // §19.2.1.1 steps 8–10: what `new.target`, `super` and
        // `arguments` may do in the eval code follows from the function
        // whose `this` it sees — an arrow's eval at the top level is
        // not in function code at all.
        Environment* this_env = this_environment(scope);
        if (this_env && this_env->function()) {
            FunctionNode const& fn = static_cast<ScriptFunction*>(this_env->function())->node();
            options.in_function = true;
            options.allow_super_property = fn.is_method;
            options.allow_super_call = fn.is_derived_constructor;
            options.in_field_initializer = fn.is_field_initializer || fn.is_static_block;
        }
    }
    Parser parser(heap(), std::u16string(source), options);
    std::unique_ptr<Program> program = parser.parse_program("eval");
    if (!program) {
        ParseError const error = parser.error().value_or(ParseError { {}, "parse failed" });
        return self.throw_syntax_error(error.message);
    }
    bool const strict = options.strict || program->is_strict;
    Program const* tree = program.get();
    FunctionNode const* body = program_body(*program, strict);
    self.keep(std::move(program));
    // §19.2.1.1: the eval execution context's [[ScriptOrModule]] is the
    // one the running context has, which for a direct eval is the caller's.
    // This program is neither, so what it inherits is recorded against it —
    // not held on the context, because a function made here outlives the
    // eval and resolves its own `import()` against the same script or
    // module. The indirect form runs from the `eval` function, whose
    // context carries no script or module (§10.3.3), and passes none.
    if (direct && caller != nullptr)
        self.note_eval_referrer(*tree, caller);

    Environment* outer = direct ? scope : realm().global_lexical;
    Environment* lexical = new_environment(outer);
    if (strict)
        variable = lexical;
    if (!this_value.is_empty())
        lexical->set_this(this_value);
    // Eval code owns its own declarations: the function field stays
    // null so a block function's Annex B copy reads the eval's var list.
    PrivateEnvironment* const private_env = direct ? private_environment : nullptr;
    ContextScope context_scope(*this, Context { lexical, variable, tree, nullptr, strict, private_env });
    Context& cx = context_scope.context();
    if (!eval_declaration_instantiation(*tree, variable, lexical, strict, private_env))
        return std::nullopt;
    return run_compiled_node(*body, cx);
}

// The statement list of a script or an eval as a body the machine can run:
// a synthetic function node of the program's own — no parameters, nothing
// to instantiate, since the program's declarations were made by the
// caller — marked so the compiler tracks its completion value.
FunctionNode const* Interpreter::Impl::program_body(Program& program, bool strict)
{
    FunctionNode* body = program.make_function();
    body->body = program.body;
    body->declarations = program.declarations;
    body->is_strict = strict;
    body->is_constructable = false;
    body->is_program_body = true;
    return body;
}


std::optional<Value> Interpreter::Impl::evaluate_regexp(RegExpLiteral const& literal)
{
    // §13.2.7.3: a fresh RegExp object each time the literal is
    // evaluated, made the way the constructor makes one.
    Value const arguments[2] = { Value::string(literal.pattern), Value::string(literal.flags) };
    Function* constructor = self.intrinsics().regexp_constructor;
    if (constructor == nullptr)
        return self.throw_syntax_error("regular expressions are not supported yet");
    return self.construct(Value::object(constructor), arguments);
}


// GetTemplateObject (§13.2.8.4): the site's cooked strings as a frozen
// array whose `raw` is the frozen array of raw strings, made once.
std::optional<Object*> Interpreter::Impl::template_object(TemplateLiteral const& literal)
{
    if (auto const found = realm().template_objects.find(&literal); found != realm().template_objects.end())
        return found->second;
    Roots const roots(self);
    auto freeze = [&](ArrayObject& array) -> bool {
        PropertyDescriptor fixed;
        fixed.writable = false;
        fixed.configurable = false;
        for (std::uint32_t i = 0; i < array.length(); ++i) {
            if (!self.define_property_or_throw(array, PropertyKey::index(i), fixed))
                return false;
        }
        PropertyDescriptor length;
        length.writable = false;
        if (!self.define_property_or_throw(array, PropertyKey::atom(atoms().length), length))
            return false;
        array.prevent_extensions();
        return true;
    };
    std::vector<Value> cooked;
    std::vector<Value> raw;
    for (std::size_t i = 0; i < literal.raw.size(); ++i) {
        cooked.push_back(literal.cooked[i] ? Value::string(literal.cooked[i]) : Value::undefined());
        raw.push_back(Value::string(literal.raw[i]));
    }
    ArrayObject* raw_array = self.new_array(raw);
    self.root(Value::object(raw_array));
    ArrayObject* cooked_array = self.new_array(cooked);
    self.root(Value::object(cooked_array));
    if (!freeze(*raw_array))
        return std::nullopt;
    cooked_array->put(PropertyKey::atom(atoms().raw), Value::object(raw_array), frozen_attributes);
    if (!freeze(*cooked_array))
        return std::nullopt;
    realm().template_objects.emplace(&literal, cooked_array);
    return cooked_array;
}


std::optional<Value> Interpreter::Impl::delete_reference(Reference& reference, Context const& cx)
{
    switch (reference.kind) {
    case Reference::Kind::Value:
    case Reference::Kind::Unresolvable:
        return Value::boolean(true);
    case Reference::Kind::Super:
        // §13.5.1.2 step 5.a: a super reference cannot be deleted.
        return self.throw_reference_error("Unsupported reference to 'super'");
    case Reference::Kind::Private:
        // §13.5.1.1: an early error the parser raises; nothing reaches here.
        return self.throw_syntax_error("Private fields can not be deleted");
    case Reference::Kind::Binding:
        return Value::boolean(reference.environment->remove(reference.name));
    case Reference::Kind::ObjectEnvironment: {
        std::optional<bool> const deleted = self.delete_property(*reference.environment->object(), reference.key);
        if (!deleted)
            return std::nullopt;
        return Value::boolean(*deleted);
    }
    case Reference::Kind::Property: {
        Roots const roots(self);
        self.root(reference.base);
        self.root(reference.key_value);
        if (reference.base.is_nullish())
            return self.throw_type_error("Cannot convert undefined or null to object");
        std::optional<Object*> const object = self.to_object(reference.base);
        if (!object)
            return std::nullopt;
        self.root(Value::object(*object));
        if (!ensure_key(reference))
            return std::nullopt;
        std::optional<bool> const deleted = self.delete_property(**object, reference.key);
        if (!deleted)
            return std::nullopt;
        if (!*deleted && cx.strict)
            return self.throw_type_error("Cannot delete property '" + key_description(reference.key) + "' of object");
        return Value::boolean(*deleted);
    }
    }
    return Value::boolean(true);
}


// The operator of a binary or compound-assignment expression applied
// to two evaluated operands (§13.6–§13.12, §13.15.3 ApplyStringOrNumericBinaryOperator).
std::optional<Value> Interpreter::Impl::apply_binary(BinaryOp op, Value const& left, Value const& right)
{
    Roots const roots(self);
    self.root(left);
    self.root(right);
    switch (op) {
    case BinaryOp::Add: {
        // §13.15.3: primitives first, then a string on either side
        // concatenates, otherwise numbers add.
        std::optional<Value> const lprim = self.to_primitive(left);
        if (!lprim)
            return std::nullopt;
        self.root(*lprim);
        std::optional<Value> const rprim = self.to_primitive(right);
        if (!rprim)
            return std::nullopt;
        self.root(*rprim);
        if (lprim->is_string() || rprim->is_string()) {
            std::optional<JsString*> const lstr = self.to_string(*lprim);
            if (!lstr)
                return std::nullopt;
            self.root(Value::string(*lstr));
            std::optional<JsString*> const rstr = self.to_string(*rprim);
            if (!rstr)
                return std::nullopt;
            if ((*lstr)->is_empty())
                return Value::string(*rstr);
            if ((*rstr)->is_empty())
                return Value::string(*lstr);
            std::u16string joined;
            joined.reserve((*lstr)->length() + (*rstr)->length());
            joined += (*lstr)->view();
            joined += (*rstr)->view();
            return Value::string(heap().string(std::move(joined)));
        }
        std::optional<Value> const lnum = self.to_numeric(*lprim);
        if (!lnum)
            return std::nullopt;
        self.root(*lnum);
        std::optional<Value> const rnum = self.to_numeric(*rprim);
        if (!rnum)
            return std::nullopt;
        if (lnum->is_bigint() != rnum->is_bigint())
            return self.throw_type_error("Cannot mix BigInt and other types, use explicit conversions");
        if (lnum->is_bigint())
            return self.bigint(lnum->as_bigint()->value() + rnum->as_bigint()->value());
        return Value::number(lnum->as_number() + rnum->as_number());
    }
    case BinaryOp::Subtract:
    case BinaryOp::Multiply:
    case BinaryOp::Divide:
    case BinaryOp::Remainder:
    case BinaryOp::Exponent: {
        std::optional<Value> const lnum = self.to_numeric(left);
        if (!lnum)
            return std::nullopt;
        self.root(*lnum);
        std::optional<Value> const rnum = self.to_numeric(right);
        if (!rnum)
            return std::nullopt;
        if (lnum->is_bigint() != rnum->is_bigint())
            return self.throw_type_error("Cannot mix BigInt and other types, use explicit conversions");
        if (lnum->is_bigint()) {
            // §6.1.6.2: exact, with the two errors the integers can raise.
            BigInteger const& a = lnum->as_bigint()->value();
            BigInteger const& b = rnum->as_bigint()->value();
            switch (op) {
            case BinaryOp::Subtract:
                return self.bigint(a - b);
            case BinaryOp::Multiply:
                return self.bigint(a * b);
            case BinaryOp::Divide:
            case BinaryOp::Remainder: {
                std::optional<BigInteger> result
                    = op == BinaryOp::Divide ? BigInteger::divide(a, b) : BigInteger::remainder(a, b);
                if (!result)
                    return self.throw_range_error("Division by zero");
                return self.bigint(std::move(*result));
            }
            default: {
                BigInteger::Power power = BigInteger::power(a, b);
                if (power.negative_exponent)
                    return self.throw_range_error("Exponent must be non-negative");
                if (power.too_large)
                    return self.throw_range_error("Maximum BigInt size exceeded");
                return self.bigint(std::move(*power.value));
            }
            }
        }
        double const l = lnum->as_number();
        double const r = rnum->as_number();
        switch (op) {
        case BinaryOp::Subtract:
            return Value::number(l - r);
        case BinaryOp::Multiply:
            return Value::number(l * r);
        case BinaryOp::Divide:
            return Value::number(l / r);
        case BinaryOp::Remainder:
            return Value::number(std::fmod(l, r));
        default:
            return Value::number(number_exponentiate(l, r));
        }
    }
    case BinaryOp::LeftShift:
    case BinaryOp::RightShift:
    case BinaryOp::UnsignedRightShift:
    case BinaryOp::BitwiseAnd:
    case BinaryOp::BitwiseOr:
    case BinaryOp::BitwiseXor: {
        std::optional<Value> const lval = self.to_numeric(left);
        if (!lval)
            return std::nullopt;
        self.root(*lval);
        std::optional<Value> const rval = self.to_numeric(right);
        if (!rval)
            return std::nullopt;
        if (lval->is_bigint() != rval->is_bigint())
            return self.throw_type_error("Cannot mix BigInt and other types, use explicit conversions");
        if (lval->is_bigint()) {
            BigInteger const& a = lval->as_bigint()->value();
            BigInteger const& b = rval->as_bigint()->value();
            switch (op) {
            case BinaryOp::LeftShift:
            case BinaryOp::RightShift: {
                std::optional<BigInteger> result
                    = op == BinaryOp::LeftShift ? BigInteger::shift_left(a, b) : BigInteger::shift_right(a, b);
                if (!result)
                    return self.throw_range_error("Maximum BigInt size exceeded");
                return self.bigint(std::move(*result));
            }
            case BinaryOp::UnsignedRightShift:
                return self.throw_type_error("BigInts have no unsigned right shift, use >> instead");
            case BinaryOp::BitwiseAnd:
                return self.bigint(a & b);
            case BinaryOp::BitwiseOr:
                return self.bigint(a | b);
            default:
                return self.bigint(a ^ b);
            }
        }
        std::int32_t const lnum_value = Interpreter::double_to_int32(lval->as_number());
        std::uint32_t const rnum_value = Interpreter::double_to_uint32(rval->as_number());
        std::optional<std::int32_t> const lnum = lnum_value;
        std::optional<std::uint32_t> const rnum = rnum_value;
        auto const lbits = static_cast<std::uint32_t>(*lnum);
        std::uint32_t const shift = *rnum & 31u;
        switch (op) {
        case BinaryOp::LeftShift:
            return Value::number(static_cast<double>(static_cast<std::int32_t>(lbits << shift)));
        case BinaryOp::RightShift:
            return Value::number(static_cast<double>(*lnum >> shift));
        case BinaryOp::UnsignedRightShift:
            return Value::number(static_cast<double>(lbits >> shift));
        case BinaryOp::BitwiseAnd:
            return Value::number(static_cast<double>(static_cast<std::int32_t>(lbits & *rnum)));
        case BinaryOp::BitwiseOr:
            return Value::number(static_cast<double>(static_cast<std::int32_t>(lbits | *rnum)));
        default:
            return Value::number(static_cast<double>(static_cast<std::int32_t>(lbits ^ *rnum)));
        }
    }
    case BinaryOp::Equal:
    case BinaryOp::NotEqual: {
        std::optional<bool> const equal = self.loose_equals(left, right);
        if (!equal)
            return std::nullopt;
        return Value::boolean(op == BinaryOp::Equal ? *equal : !*equal);
    }
    case BinaryOp::StrictEqual:
        return Value::boolean(Interpreter::strict_equals(left, right));
    case BinaryOp::StrictNotEqual:
        return Value::boolean(!Interpreter::strict_equals(left, right));
    case BinaryOp::Less: {
        std::optional<std::optional<bool>> const r = self.less_than(left, right, true);
        if (!r)
            return std::nullopt;
        return Value::boolean(r->value_or(false));
    }
    case BinaryOp::Greater: {
        std::optional<std::optional<bool>> const r = self.less_than(right, left, false);
        if (!r)
            return std::nullopt;
        return Value::boolean(r->value_or(false));
    }
    case BinaryOp::LessEqual: {
        std::optional<std::optional<bool>> const r = self.less_than(right, left, false);
        if (!r)
            return std::nullopt;
        return Value::boolean(r->has_value() && !**r);
    }
    case BinaryOp::GreaterEqual: {
        std::optional<std::optional<bool>> const r = self.less_than(left, right, true);
        if (!r)
            return std::nullopt;
        return Value::boolean(r->has_value() && !**r);
    }
    case BinaryOp::In: {
        // §13.10.1: the right operand must be an object; then the
        // left becomes a key.
        if (!right.is_object())
            return self.throw_type_error("Cannot use 'in' operator to search for '" + self.describe(left) + "' in " + self.describe(right));
        std::optional<PropertyKey> const key = self.to_property_key(left);
        if (!key)
            return std::nullopt;
        std::optional<bool> const has = self.has_property(*right.as_object(), *key);
        if (!has)
            return std::nullopt;
        return Value::boolean(*has);
    }
    case BinaryOp::Instanceof: {
        std::optional<bool> const result = self.instance_of(left, right);
        if (!result)
            return std::nullopt;
        return Value::boolean(*result);
    }
    }
    return Value::undefined();
}


std::optional<BinaryOp> Interpreter::Impl::binary_for(AssignmentOp op)
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


// CreatePerIterationEnvironment (§14.7.4.4): a fresh copy of the loop
// variables, so that closures made in one iteration keep its values.
void Interpreter::Impl::copy_iteration_environment(Context& cx, std::vector<JsString*> const& names)
{
    Environment* const previous = cx.lexical;
    Environment* copy = new_environment(previous->outer());
    for (JsString* name : names) {
        Environment::Binding const* binding = previous->find(name);
        copy->declare(name, binding ? binding->value : Value::undefined(), true, binding ? binding->initialized : true);
    }
    cx.lexical = copy;
}


bool Interpreter::Impl::enumerator_load(Enumerator& enumerator)
{
    enumerator.keys.clear();
    enumerator.next = 0;
    if (enumerator.object == nullptr)
        return true;
    // [[OwnPropertyKeys]] through the wrapper: a proxy's is the handler's
    // ownKeys trap (§10.5.11) and can throw. Only string keys are kept,
    // and those are atoms, so nothing here outlives its owner.
    std::optional<std::vector<PropertyKey>> const keys = self.own_keys(*enumerator.object);
    if (!keys)
        return false;
    for (PropertyKey const& key : *keys) {
        if (key.is_string())
            enumerator.keys.push_back(key);
    }
    return true;
}


// The next key as a string, or null when the chain is exhausted — or when
// an internal method threw: a module namespace's export read in its dead
// zone (§10.4.6.4), or one of a proxy's three traps this walk uses. The
// caller tells the cases apart by the exception pending.
JsString* Interpreter::Impl::enumerator_next(Enumerator& enumerator)
{
    while (enumerator.object != nullptr) {
        while (enumerator.next < enumerator.keys.size()) {
            PropertyKey const key = enumerator.keys[enumerator.next++];
            JsString* name = heap().key_to_string(key);
            if (enumerator.visited.contains(name))
                continue;
            std::optional<std::optional<PropertyDescriptor>> const read = self.get_own_property(*enumerator.object, key);
            if (!read)
                return nullptr;
            if (!*read)
                continue;
            enumerator.visited.insert(name);
            if ((*read)->enumerable.value_or(false))
                return name;
        }
        // [[GetPrototypeOf]]: a proxy in the chain answers from its trap.
        std::optional<Object*> const next = self.get_prototype_of(*enumerator.object);
        if (!next)
            return nullptr;
        enumerator.object = *next;
        if (!enumerator_load(enumerator))
            return nullptr;
    }
    return nullptr;
}


// ---- tracing
void Interpreter::Impl::trace(Tracer& tracer)
{
    for (Context const& context : contexts) {
        tracer.visit(context.lexical);
        tracer.visit(context.variable);
        tracer.visit(context.function);
        tracer.visit(context.private_environment);
    }
    for (Frame* frame : vm_frames)
        tracer.visit(frame);
    for (ClassBuilder* builder : class_builders)
        tracer.visit(builder);
}


// ------------------------------------------------------- Interpreter

Interpreter::Interpreter()
    : m_heap(std::make_unique<Heap>())
    , m_impl(std::make_unique<Impl>(*this))
{
    m_heap->add_root_provider(this);
    m_realm = create_realm();
}

RealmRecord* Interpreter::create_realm()
{
    // Nothing is collected while the realm is half built: its record is
    // reachable from no root until whoever asked for it keeps it.
    Heap::NoCollect const guard(*m_heap);
    RealmRecord* const previous = m_realm;
    RealmRecord* const realm = m_heap->allocate<RealmRecord>();
    m_realm = realm;
    install_intrinsics(*this);
    realm->global_lexical = m_heap->allocate<Environment>(realm->intrinsics.global_environment);
    m_realm = previous;
    return realm;
}

void RealmRecord::trace(Tracer& tracer)
{
    Intrinsics const& i = intrinsics;
    tracer.visit(i.global);
    tracer.visit(i.global_environment);
    tracer.visit(i.object_prototype);
    tracer.visit(i.function_prototype);
    tracer.visit(i.array_prototype);
    tracer.visit(i.string_prototype);
    tracer.visit(i.number_prototype);
    tracer.visit(i.boolean_prototype);
    tracer.visit(i.symbol_prototype);
    tracer.visit(i.bigint_prototype);
    tracer.visit(i.error_prototype);
    for (Object* prototype : i.error_prototypes)
        tracer.visit(prototype);
    tracer.visit(i.date_prototype);
    tracer.visit(i.regexp_prototype);
    tracer.visit(i.arguments_prototype);
    tracer.visit(i.object_constructor);
    tracer.visit(i.function_constructor);
    tracer.visit(i.array_constructor);
    tracer.visit(i.string_constructor);
    tracer.visit(i.number_constructor);
    tracer.visit(i.boolean_constructor);
    tracer.visit(i.symbol_constructor);
    tracer.visit(i.bigint_constructor);
    tracer.visit(i.error_constructor);
    for (Function* constructor : i.error_constructors)
        tracer.visit(constructor);
    tracer.visit(i.date_constructor);
    tracer.visit(i.regexp_constructor);
    tracer.visit(i.eval);
    tracer.visit(i.throw_type_error);
    tracer.visit(i.iterator_prototype);
    tracer.visit(i.array_iterator_prototype);
    tracer.visit(i.string_iterator_prototype);
    tracer.visit(i.array_prototype_values);
    tracer.visit(i.map_prototype);
    tracer.visit(i.set_prototype);
    tracer.visit(i.weak_map_prototype);
    tracer.visit(i.weak_set_prototype);
    tracer.visit(i.map_iterator_prototype);
    tracer.visit(i.set_iterator_prototype);
    tracer.visit(i.promise_prototype);
    tracer.visit(i.promise_constructor);
    tracer.visit(i.aggregate_error_prototype);
    tracer.visit(i.aggregate_error_constructor);
    tracer.visit(i.generator_function_prototype);
    tracer.visit(i.generator_function);
    tracer.visit(i.generator_prototype);
    tracer.visit(i.async_function_prototype);
    tracer.visit(i.async_function);
    tracer.visit(i.async_iterator_prototype);
    tracer.visit(i.async_from_sync_iterator_prototype);
    tracer.visit(i.async_generator_function_prototype);
    tracer.visit(i.async_generator_function);
    tracer.visit(i.async_generator_prototype);
    tracer.visit(i.array_buffer_prototype);
    tracer.visit(i.array_buffer_constructor);
    tracer.visit(i.typed_array_prototype);
    tracer.visit(i.typed_array_constructor);
    for (Object* prototype : i.typed_array_prototypes)
        tracer.visit(prototype);
    for (Function* constructor : i.typed_array_constructors)
        tracer.visit(constructor);
    tracer.visit(i.data_view_prototype);
    tracer.visit(i.data_view_constructor);
    tracer.visit(i.proxy_constructor);
    tracer.visit(i.math);
    tracer.visit(i.json);
    tracer.visit(i.symbol_registry);
    tracer.visit(global_lexical);
    for (JsString* name : var_names)
        tracer.visit(name);
    for (auto const& [site, object] : template_objects)
        tracer.visit(object);
}

Interpreter::~Interpreter()
{
    m_heap->remove_root_provider(this);
}

void Interpreter::trace_roots(Tracer& tracer)
{
    for (Value const& value : m_roots)
        tracer.visit(value);
    tracer.visit(m_exception);
    tracer.visit(m_realm);
    for (Job const& job : m_jobs) {
        tracer.visit(job.argument);
        tracer.visit(job.then);
        tracer.visit(job.reaction.handler);
        tracer.visit(job.reaction.capability_promise);
        tracer.visit(job.reaction.capability_resolve);
        tracer.visit(job.reaction.capability_reject);
        tracer.visit(job.promise);
        for (Value const& argument : job.arguments)
            tracer.visit(argument);
    }
    for (PromiseObject* promise : m_unhandled_rejections)
        tracer.visit(promise);
    for (auto const& [key, record] : m_modules)
        tracer.visit(record);
    m_impl->trace(tracer);
}

void Interpreter::keep(std::unique_ptr<Program> program)
{
    m_programs.push_back(std::move(program));
}

Outcome Interpreter::run_script(std::u16string_view source, std::string name)
{
    // ScriptEvaluation (§16.1.6): parse, instantiate the global
    // declarations, evaluate; a parse error is a thrown SyntaxError.
    Outcome outcome;
    if (m_call_depth == 0)
        m_stack_base = stack_position();
    Parser parser(*m_heap, std::u16string(source), {});
    std::unique_ptr<Program> program = parser.parse_program(name);
    if (!program) {
        ParseError const error = parser.error().value_or(ParseError { {}, "parse failed" });
        std::string message = error.message;
        if (!name.empty())
            message += " (" + name + ":" + std::to_string(error.position.line) + ":" + std::to_string(error.position.column) + ")";
        throw_syntax_error(message);
        outcome.ok = false;
        outcome.value = take_exception();
        return outcome;
    }
    Program const* tree = program.get();
    // The script's statement list is a synthetic body of the program's own
    // (kept with it for the realm's life), whose completion value the
    // compiler tracks.
    FunctionNode const* body = m_impl->program_body(*program, tree->is_strict);
    keep(std::move(program));
    Impl::ContextScope scope(*m_impl, Context { m_realm->global_lexical, m_realm->intrinsics.global_environment, tree, nullptr, tree->is_strict, nullptr });
    Context& cx = scope.context();
    if (!m_impl->global_declaration_instantiation(*tree, cx)) {
        outcome.ok = false;
        outcome.value = take_exception();
        return outcome;
    }
    std::optional<Value> const value = m_impl->run_compiled_node(*body, cx);
    if (!value) {
        outcome.ok = false;
        outcome.value = take_exception();
        return outcome;
    }
    outcome.ok = true;
    outcome.value = *value;
    return outcome;
}

Outcome Interpreter::run_script(std::string_view utf8_source, std::string name)
{
    return run_script(std::u16string_view(utf16_from_utf8(utf8_source)), std::move(name));
}

// ---- modules (§16.2)

void Interpreter::set_module_hooks(ModuleResolver resolver, ModuleFetcher fetcher)
{
    m_module_resolver = std::move(resolver);
    m_module_fetcher = std::move(fetcher);
}

ModuleRecord* Interpreter::find_module(std::string_view key) const
{
    auto const found = m_modules.find(std::string(key));
    return found == m_modules.end() ? nullptr : found->second;
}

// ParseModule (§16.2.1.6.1): the Module goal's parse, then a record in
// the map — which roots it; the record owns the tree, and functions made
// from the tree point into it, so the record lives as long as the realm.
ModuleRecord* Interpreter::parse_module(std::u16string_view source, std::string key)
{
    if (ModuleRecord* existing = find_module(key))
        return existing;
    ParseOptions options;
    options.module = true;
    Parser parser(*m_heap, std::u16string(source), options);
    std::unique_ptr<Program> program = parser.parse_program(key);
    if (!program) {
        ParseError const error = parser.error().value_or(ParseError { {}, "parse failed" });
        std::string message = error.message;
        if (!key.empty())
            message += " (" + key + ":" + std::to_string(error.position.line) + ":" + std::to_string(error.position.column) + ")";
        throw_syntax_error(message);
        return nullptr;
    }
    ModuleRecord* record = m_heap->allocate<ModuleRecord>(key, std::move(program));
    m_modules.emplace(std::move(key), record);
    // The tree is how the running code finds its own record: a function
    // carries the program it was written in, so `import()` in a function
    // of one module called from another resolves against the first.
    m_module_programs.emplace(&record->program(), record);
    return record;
}

// GetActiveScriptOrModule (§9.4.1) as this engine keeps it.
ModuleRecord* Interpreter::module_of(Program const& program) const
{
    auto const found = m_module_programs.find(&program);
    return found == m_module_programs.end() ? nullptr : found->second;
}

// GetActiveScriptOrModule for code that may be eval code: an eval Program
// is not a script and not a module, so the answer is the program it
// inherited from the context the eval ran in (§19.2.1.1), which is where
// its relative specifiers resolve against. The loop costs nothing and
// spells out that a chain cannot go round: note_eval_referrer flattens one
// eval within another to the script or module at the end of it.
Program const* Interpreter::referrer_program(Program const* program) const
{
    while (program != nullptr) {
        auto const found = m_eval_referrers.find(program);
        if (found == m_eval_referrers.end())
            break;
        program = found->second;
    }
    return program;
}

void Interpreter::note_eval_referrer(Program const& eval_program, Program const* caller)
{
    Program const* const inherited = referrer_program(caller);
    if (inherited != nullptr && inherited != &eval_program)
        m_eval_referrers.emplace(&eval_program, inherited);
}

// HostLoadImportedModule (§16.2.1.8) for every request of the record and,
// depth first, of what it brings in: the resolver names the module, the
// map answers one already parsed, the fetcher and the parser make the
// rest. A resolution or fetch failure is a TypeError, a parse failure the
// SyntaxError, either pending on return.
bool Interpreter::load_module(ModuleRecord& record)
{
    for (ModuleRequest const& request : record.program().requested_modules) {
        if (record.imported_module(request.specifier) != nullptr)
            continue;
        std::string const specifier = request.specifier->to_utf8();
        if (!m_module_resolver || !m_module_fetcher) {
            throw_type_error("Cannot load module '" + specifier + "': this host loads no modules");
            return false;
        }
        // The only attribute a host understands is `type`, and no type
        // but JavaScript is written (JSON, CSS, text and bytes modules are
        // proposals or other items): refused by name, at load, as the
        // host's HostLoadImportedModule would.
        for (ImportAttribute const& attribute : request.attributes) {
            if (attribute.key->view() == u"type") {
                throw_type_error("Cannot import '" + specifier + "' as a module of type '" + attribute.value->to_utf8()
                    + "': modules of that type are not supported yet");
                return false;
            }
        }
        std::string error;
        std::optional<std::string> const key = m_module_resolver(record.key(), specifier, error);
        if (!key) {
            throw_type_error(error.empty() ? "Failed to resolve module specifier '" + specifier + "'" : error);
            return false;
        }
        ModuleRecord* dependency = find_module(*key);
        if (dependency == nullptr) {
            std::optional<std::u16string> const source = m_module_fetcher(*key, error);
            if (!source) {
                throw_type_error(error.empty() ? "Failed to fetch module '" + *key + "'" : error);
                return false;
            }
            dependency = parse_module(*source, *key);
            if (dependency == nullptr)
                return false;
        }
        record.add_loaded_module(request.specifier, dependency);
        if (!load_module(*dependency))
            return false;
    }
    return true;
}

bool Interpreter::link_module(ModuleRecord& record)
{
    return record.link(*this);
}

std::optional<Value> Interpreter::evaluate_module(ModuleRecord& record)
{
    if (m_call_depth == 0)
        m_stack_base = stack_position();
    return record.evaluate(*this);
}

// AllImportAttributesSupported (§13.3.10.1): HostGetSupportedImportAttributes
// answers one key here, `type`, so a request carrying any other key is not
// supported and the request fails — there is no latitude to ignore it. What
// a `type` it does carry names is a separate question, asked at load: no
// module type but JavaScript is written here.
bool Interpreter::all_import_attributes_supported(std::span<ImportAttribute const> attributes, std::string const& specifier)
{
    for (ImportAttribute const& attribute : attributes) {
        if (attribute.key->view() == u"type")
            continue;
        throw_type_error("Cannot import '" + specifier + "' with the import attribute '" + attribute.key->to_utf8()
            + "': that attribute is not supported");
        return false;
    }
    return true;
}

// EvaluateImportCall (§13.3.10.1) from its fourth step on: the capability
// is made before anything else can fail, so the specifier's ToString, the
// options, the resolution, the fetch, the parse, the link and the
// evaluation all reject the promise this answers instead of throwing out
// of the expression. The operands were evaluated by the caller, which is
// where a throw still propagates.
std::optional<Value> Interpreter::perform_import_call(Program const* referrer, Value const& specifier, Value const& options)
{
    Roots const roots(*this);
    root(specifier);
    root(options);
    std::optional<PromiseCapability> const capability = new_promise_capability(*this, Value::object(m_realm->intrinsics.promise_constructor));
    if (!capability)
        return std::nullopt;
    root(capability->promise);
    root(capability->resolve);
    root(capability->reject);
    // IfAbruptRejectPromise, for every step below.
    auto reject_pending = [&]() -> std::optional<Value> {
        if (m_terminated)
            return std::nullopt;
        Value const thrown = take_exception();
        root(thrown);
        Value const arguments[1] = { thrown };
        if (!call(capability->reject, Value::undefined(), arguments))
            return std::nullopt;
        return capability->promise;
    };
    std::optional<JsString*> const specifier_string = to_string(specifier);
    if (!specifier_string)
        return reject_pending();
    root(Value::string(*specifier_string));
    std::string const specifier_text = (*specifier_string)->to_utf8();
    // The import attributes of the second argument: the own enumerable
    // string-keyed properties of its `with`, each value a string.
    std::vector<ImportAttribute> attributes;
    if (!options.is_undefined()) {
        if (!options.is_object()) {
            throw_type_error("The options of import('" + specifier_text + "') must be an object");
            return reject_pending();
        }
        std::optional<Value> const with = get(options, "with");
        if (!with)
            return reject_pending();
        root(*with);
        if (!with->is_undefined()) {
            if (!with->is_object()) {
                throw_type_error("The 'with' option of import('" + specifier_text + "') must be an object");
                return reject_pending();
            }
            Object& source = *with->as_object();
            for (PropertyKey const& key : source.own_keys()) {
                if (key.is_symbol())
                    continue;
                std::optional<std::optional<PropertyDescriptor>> const descriptor = get_own_property(source, key);
                if (!descriptor)
                    return reject_pending();
                if (!*descriptor || !(*descriptor)->enumerable.value_or(false))
                    continue;
                std::optional<Value> const value = get(*with, key);
                if (!value)
                    return reject_pending();
                JsString* name = m_heap->key_to_string(key);
                root(Value::string(name));
                if (!value->is_string()) {
                    throw_type_error("The import attribute '" + name->to_utf8() + "' must be a string");
                    return reject_pending();
                }
                root(*value);
                attributes.push_back(ImportAttribute { name, value->as_string() });
            }
        }
    }
    // AllImportAttributesSupported: an unsupported key is refused with a
    // TypeError of the capability's own, before the module is named to the
    // host at all. The values were checked as they were collected, which is
    // the order §13.3.10.1 reads them in.
    if (!all_import_attributes_supported(attributes, specifier_text))
        return reject_pending();
    // The referrer: the running module's key, or the name the host gave the
    // script. Eval code has neither of its own and answers with the script
    // or module it inherited, so a specifier in it resolves against the
    // same base as one written beside the eval.
    std::string referrer_key;
    if (Program const* const origin = referrer_program(referrer); origin != nullptr) {
        ModuleRecord* const record = module_of(*origin);
        referrer_key = record != nullptr ? record->key() : origin->name;
    }
    if (!load_imported_module(referrer_key, specifier_text, attributes, *capability))
        return std::nullopt;
    return capability->promise;
}

// HostLoadImportedModule (§16.2.1.8) for a dynamic request, and the
// continuation the specification hands it: load, link, evaluate, and then
// settle the capability with the module's namespace when the module's own
// evaluation promise fulfils — as a reaction on that promise, since a
// module may be waiting on something, not because it has already settled
// — or with the error of whichever phase failed.
bool Interpreter::load_imported_module(std::string const& referrer_key, std::string const& specifier,
    std::span<ImportAttribute const> attributes, PromiseCapability const& capability)
{
    Roots const roots(*this);
    root(capability.promise);
    root(capability.resolve);
    root(capability.reject);
    auto reject_pending = [&]() -> bool {
        if (m_terminated)
            return false;
        Value const thrown = take_exception();
        root(thrown);
        Value const arguments[1] = { thrown };
        return call(capability.reject, Value::undefined(), arguments).has_value();
    };
    // The only attribute a host here understands is `type`, and no type
    // but JavaScript is written: refused by name, as a static import of
    // the same attribute is refused at load.
    for (ImportAttribute const& attribute : attributes) {
        if (attribute.key->view() == u"type") {
            throw_type_error("Cannot import '" + specifier + "' as a module of type '" + attribute.value->to_utf8()
                + "': modules of that type are not supported yet");
            return reject_pending();
        }
    }
    if (!m_module_resolver || !m_module_fetcher) {
        throw_type_error("Cannot load module '" + specifier + "': this host loads no modules");
        return reject_pending();
    }
    std::string error;
    std::optional<std::string> const key = m_module_resolver(referrer_key, specifier, error);
    if (!key) {
        throw_type_error(error.empty() ? "Failed to resolve module specifier '" + specifier + "'" : error);
        return reject_pending();
    }
    ModuleRecord* record = find_module(*key);
    if (record == nullptr) {
        std::optional<std::u16string> const source = m_module_fetcher(*key, error);
        if (!source) {
            throw_type_error(error.empty() ? "Failed to fetch module '" + *key + "'" : error);
            return reject_pending();
        }
        record = parse_module(*source, *key);
        if (record == nullptr)
            return reject_pending();
    }
    if (!load_module(*record))
        return reject_pending();
    // Only a module that has never been linked is linked here. Running
    // InitializeEnvironment a second time would give the record a fresh
    // environment and leave the old one holding every value the module
    // had already initialised — and every importer's indirect bindings
    // pointing into a dead zone that never ends. A module importing
    // itself while its own body runs is exactly that case.
    if (record->status() == ModuleRecord::Status::Unlinked && !link_module(*record))
        return reject_pending();
    // The map is keyed, so a module already evaluated is not evaluated
    // again and one that threw answers its remembered error. A module
    // whose evaluation is under way already has the promise that will say
    // how it ended, and must not be started a second time.
    Value promise = record->evaluation_promise();
    if (promise.is_empty()) {
        std::optional<Value> const evaluated = evaluate_module(*record);
        if (!evaluated)
            return reject_pending();
        promise = *evaluated;
    }
    root(promise);
    // GetModuleNamespace: one object per record, whatever the evaluation
    // does next, so the reaction below has only to hand it over.
    Object* namespace_object = record->get_namespace(*this);
    root(Value::object(namespace_object));
    ClosureFunction* on_fulfilled = new_closure("", 1, { capability.resolve, Value::object(namespace_object) },
        [](Interpreter& in, ClosureFunction& self, Value const&, std::span<Value const>) -> std::optional<Value> {
            Value const arguments[1] = { self.slot(1) };
            if (!in.call(self.slot(0), Value::undefined(), arguments))
                return std::nullopt;
            return Value::undefined();
        });
    root(Value::object(on_fulfilled));
    ClosureFunction* on_rejected = new_closure("", 1, { capability.reject },
        [](Interpreter& in, ClosureFunction& self, Value const&, std::span<Value const> arguments) -> std::optional<Value> {
            Value const reason[1] = { arguments.empty() ? Value::undefined() : arguments[0] };
            if (!in.call(self.slot(0), Value::undefined(), reason))
                return std::nullopt;
            return Value::undefined();
        });
    root(Value::object(on_rejected));
    perform_then(*this, *static_cast<PromiseObject*>(promise.as_object()), Value::object(on_fulfilled),
        Value::object(on_rejected), std::nullopt);
    return true;
}

// ImportMeta : import.meta (§13.3.12): an ordinary object with no
// prototype, one per module record, made when the expression is first
// evaluated and given whatever properties the host adds then. The
// SyntaxError below is unreachable through the parser, which refuses
// `import.meta` outside module code; it is here so that a caller without
// a record cannot get an object that belongs to no module.
std::optional<Value> Interpreter::import_meta_for(Program const* referrer)
{
    Program const* const origin = referrer_program(referrer);
    ModuleRecord* const record = origin != nullptr ? module_of(*origin) : nullptr;
    if (record == nullptr)
        return throw_syntax_error("Cannot use 'import.meta' outside a module");
    if (Object* const existing = record->import_meta())
        return Value::object(existing);
    Roots const roots(*this);
    Object* meta = m_heap->allocate<Object>(nullptr);
    root(Value::object(meta));
    record->set_import_meta(meta);
    if (m_module_meta_hook)
        m_module_meta_hook(*this, *record, *meta);
    return Value::object(meta);
}

std::optional<Value> Interpreter::call(Value const& callee, Value const& this_value, std::span<Value const> arguments)
{
    if (!is_callable(callee))
        return throw_type_error(describe(callee) + " is not a function");
    if (m_call_depth == 0)
        m_stack_base = stack_position();
    if (m_call_depth >= m_call_depth_limit)
        return throw_range_error("Maximum call stack size exceeded");
    Roots const roots(*this);
    root(callee);
    root(this_value);
    for (Value const& argument : arguments)
        root(argument);
    if (!m_impl->step() || !m_impl->stack_ok())
        return std::nullopt;
    ++m_call_depth;
    std::optional<Value> const result = static_cast<Function*>(callee.as_object())->call(*this, this_value, arguments);
    --m_call_depth;
    return result;
}

std::optional<Value> Interpreter::construct(Value const& callee, std::span<Value const> arguments)
{
    return construct(callee, arguments, nullptr);
}

std::optional<Value> Interpreter::construct(Value const& callee, std::span<Value const> arguments, Object* new_target)
{
    if (!is_constructor(callee))
        return throw_type_error(describe(callee) + " is not a constructor");
    if (m_call_depth == 0)
        m_stack_base = stack_position();
    if (m_call_depth >= m_call_depth_limit)
        return throw_range_error("Maximum call stack size exceeded");
    Roots const roots(*this);
    root(callee);
    if (new_target)
        root(Value::object(new_target));
    for (Value const& argument : arguments)
        root(argument);
    if (!m_impl->step() || !m_impl->stack_ok())
        return std::nullopt;
    ++m_call_depth;
    Object* constructor = callee.as_object();
    std::optional<Value> const result = static_cast<Function*>(constructor)->construct(*this, arguments, new_target ? new_target : constructor);
    --m_call_depth;
    return result;
}

Outcome Interpreter::call_outcome(Value const& callee, Value const& this_value, std::span<Value const> arguments)
{
    Outcome outcome;
    std::optional<Value> const result = call(callee, this_value, arguments);
    if (!result) {
        outcome.ok = false;
        outcome.value = take_exception();
        return outcome;
    }
    outcome.ok = true;
    outcome.value = *result;
    return outcome;
}

std::optional<Value> Interpreter::eval_in(std::u16string_view source, Environment* scope, bool strict, Value this_value,
    PrivateEnvironment* private_environment)
{
    // No caller program: the `eval` function's own context has no script or
    // module, so eval code reached this way inherits none either.
    return m_impl->perform_eval(source, scope ? scope : m_realm->global_lexical, strict, this_value, true, private_environment, nullptr);
}

std::optional<Value> Interpreter::create_dynamic_function(std::u16string_view parameters, std::u16string_view body,
    DynamicFunctionKind kind)
{
    // CreateDynamicFunction step 3: HostEnsureCanCompileStrings.
    if (on_compile_strings) {
        if (std::optional<std::string> const refused = on_compile_strings())
            return throw_error(ErrorType::EvalError, *refused);
    }
    return compile_function(parameters, body, nullptr, kind);
}

std::optional<Value> Interpreter::compile_function(std::u16string_view parameters, std::u16string_view body, Environment* scope,
    DynamicFunctionKind kind)
{
    // CreateDynamicFunction (§20.2.1.1.1): the parser assembles and checks
    // the wrapper; the function closes over the global environment.
    ParseError error;
    std::unique_ptr<Program> program = Parser::parse_function_constructor(*m_heap, parameters, body, &error, kind);
    if (!program)
        return throw_syntax_error(error.message);
    auto const* statement = static_cast<ExpressionStatement const*>(program->body[0]);
    FunctionNode const& node = *static_cast<FunctionExpression const*>(statement->expression)->function;
    keep(std::move(program));
    return Value::object(new_script_function(node, scope ? scope : m_realm->global_lexical));
}

ScriptFunction* Interpreter::new_script_function(FunctionNode const& node, Environment* scope, PrivateEnvironment* private_environment)
{
    // OrdinaryFunctionCreate + MakeConstructor (§10.2.3, §10.2.5):
    // `length` then `name`, and for a constructor a fresh `prototype`
    // pointing back. Arrows take their `this` from the scope chain, so
    // there is no lexical this to record; the Private Names in scope are.
    Heap::NoCollect const guard(*m_heap);
    // A generator or async function hangs off its kind's function
    // prototype (§27.3.3, §27.7.3, §27.4.3), and a generator's instances
    // off a fresh object that inherits from the kind's %…Prototype%.
    Object* function_prototype = m_realm->intrinsics.function_prototype;
    Object* instance_prototype = nullptr;
    if (node.is_generator && node.is_async) {
        if (m_realm->intrinsics.async_generator_function_prototype)
            function_prototype = m_realm->intrinsics.async_generator_function_prototype;
        instance_prototype = m_realm->intrinsics.async_generator_prototype;
    } else if (node.is_generator) {
        if (m_realm->intrinsics.generator_function_prototype)
            function_prototype = m_realm->intrinsics.generator_function_prototype;
        instance_prototype = m_realm->intrinsics.generator_prototype;
    } else if (node.is_async && m_realm->intrinsics.async_function_prototype) {
        function_prototype = m_realm->intrinsics.async_function_prototype;
    }
    auto* function = m_heap->allocate<ScriptFunction>(function_prototype, node, scope, node.is_constructable);
    function->set_private_environment(private_environment);
    function->put(PropertyKey::atom(atoms().length), Value::number(static_cast<double>(node.expected_argument_count)), Configurable);
    function->put(PropertyKey::atom(atoms().name), Value::string(node.name ? node.name : atoms().empty), Configurable);
    if (!node.is_strict && !node.is_arrow && node.is_constructable) {
        // A sloppy plain function carries its own null `caller` and
        // `arguments` (§17.1 leaves them implementation-defined; every
        // engine has them), so a read does not reach the poison pill on
        // Function.prototype.
        function->put(PropertyKey::atom(atoms().caller), Value::null(), frozen_attributes);
        function->put(PropertyKey::atom(atoms().arguments), Value::null(), frozen_attributes);
    }
    if (node.is_constructable) {
        Object* prototype = new_object();
        prototype->put(PropertyKey::atom(atoms().constructor), Value::object(function), builtin_attributes);
        function->put(PropertyKey::atom(atoms().prototype), Value::object(prototype), Writable);
    } else if (node.is_generator) {
        // §15.5.4 / §15.6.4: writable, not enumerable, not configurable,
        // and with no `constructor` back-link.
        Object* prototype = new_object(instance_prototype);
        function->put(PropertyKey::atom(atoms().prototype), Value::object(prototype), Writable);
    }
    return function;
}

// ---------------------------------------------------- ScriptFunction

std::optional<Value> ScriptFunction::call(Interpreter& interpreter, Value const& this_value, std::span<Value const> arguments)
{
    // §10.2.1 step 2: a class constructor is only for `new`.
    if (m_node->is_class_constructor)
        return interpreter.throw_type_error("Class constructor " + (m_node->name ? m_node->name->to_utf8() : std::string()) + " cannot be invoked without 'new'");
    return interpreter.m_impl->call_script_function(*this, this_value, arguments, nullptr);
}

std::optional<Value> ScriptFunction::construct(Interpreter& interpreter, std::span<Value const> arguments, Object* new_target)
{
    // [[Construct]] (§10.2.2): for a base constructor a fresh object from
    // new.target's prototype is `this`, and stays the result unless the
    // body returns an object of its own; a derived one gets its `this`
    // from super(), and the evaluator settles its result.
    if (!m_constructable)
        return interpreter.throw_type_error("not a constructor");
    Interpreter::Roots const roots(interpreter);
    interpreter.root(Value::object(this));
    if (new_target)
        interpreter.root(Value::object(new_target));
    if (m_node->is_derived_constructor)
        return interpreter.m_impl->call_script_function(*this, Value::undefined(), arguments, new_target);
    std::optional<Object*> const prototype = interpreter.get_prototype_from_constructor(new_target, interpreter.intrinsics().object_prototype);
    if (!prototype)
        return std::nullopt;
    Object* this_object = interpreter.new_object(*prototype);
    interpreter.root(Value::object(this_object));
    std::optional<Value> const result = interpreter.m_impl->call_script_function(*this, Value::object(this_object), arguments, new_target);
    if (!result)
        return std::nullopt;
    if (result->is_object())
        return *result;
    return Value::object(this_object);
}

}
