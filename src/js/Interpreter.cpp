#include "js/Interpreter.h"

// The evaluator: a tree-walking interpreter over the Ast (§13–§16), the
// function objects' [[Call]] and [[Construct]] (§10.2), declaration
// instantiation for scripts, functions, blocks and eval (§16.1.7,
// §10.2.11, §14.2.3, §19.2.1.3), and the realm's life cycle. The abstract
// operations it leans on are in Conversions.cpp; the library in the
// Runtime*.cpp files.
//
// Two conventions run through every function here. A throw is a return
// value — nullopt from an expression, a Throw completion from a statement
// — with the thrown value pending in the interpreter. And any Value a C++
// frame keeps across an allocation is rooted first, because the heap may
// collect inside any allocation (and does, at every one, under the tests'
// stress mode); environments are reached through the context stack, which
// is traced as a root.

#include "js/Ast.h"
#include "js/Object.h"
#include "js/Parser.h"
#include "js/Runtime.h"
#include "js/Strings.h"

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

// A completion record (§6.2.4). The value is `empty` when the statement
// produced none, which UpdateEmpty resolves at the statement above.
struct Completion {
    enum class Type : std::uint8_t { Normal, Return, Break, Continue, Throw };
    Type type = Type::Normal;
    Value value = Value::empty();
    JsString* target = nullptr;

    static Completion normal(Value value = Value::empty())
    {
        Completion completion;
        completion.value = value;
        return completion;
    }
    static Completion thrown()
    {
        Completion completion;
        completion.type = Type::Throw;
        return completion;
    }
    bool is_abrupt() const { return type != Type::Normal; }
    // UpdateEmpty (§6.2.4.3).
    Completion with_value_if_empty(Value const& fallback) const
    {
        Completion result = *this;
        if (result.value.is_empty())
            result.value = fallback;
        return result;
    }
};

// The running execution context (§9.4): the environment the code resolves
// names in, the one its `var`s land in, and what it is evaluating.
struct Context {
    Environment* lexical = nullptr;
    Environment* variable = nullptr;
    Program const* program = nullptr;
    ScriptFunction* function = nullptr; // null for global and eval code
    bool strict = false;
};

// A Reference Record (§6.2.5) in the shapes the evaluator produces: a
// binding in a declarative environment, a property of an object
// environment (the global object or a `with` target), a property of a
// value, an unresolvable name, or no reference at all — a plain value,
// which `delete` and `typeof` accept.
struct Reference {
    enum class Kind : std::uint8_t { Unresolvable, Binding, ObjectEnvironment, Property, Value };
    Kind kind = Kind::Value;
    Environment* environment = nullptr;
    JsString* name = nullptr;
    Value base; // Property: the base; Value: the value itself
    PropertyKey key; // Property (once ready) and ObjectEnvironment
    Value key_value; // Property: the key before ToPropertyKey, when not yet converted
    bool key_ready = false;
};

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

bool is_anonymous_function_definition(Expression const* expression)
{
    // IsAnonymousFunctionDefinition (§8.4.3) for the forms this grammar
    // has: an arrow, or a function expression without a name.
    if (expression->type == NodeType::ArrowFunction)
        return true;
    if (expression->type == NodeType::FunctionExpression)
        return static_cast<FunctionExpression const*>(expression)->function->name == nullptr;
    return false;
}

bool contains(std::vector<JsString*> const& names, JsString* name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

// ------------------------------------------------------------------ Impl

struct Interpreter::Impl {
    explicit Impl(Interpreter& interpreter)
        : self(interpreter)
    {
    }

    Interpreter& self;
    // A deque: the evaluator keeps references to running contexts while
    // nested calls push more, and a vector would move them.
    std::deque<Context> contexts;
    // The declarative record of the global environment (§9.1.1.4), in
    // front of the object record over the global object.
    Environment* global_lexical = nullptr;
    // The global environment's [[VarNames]]: every var and function a
    // script has declared, so a later `let` of the same name is refused.
    std::unordered_set<JsString*> global_var_names;

    Heap& heap() { return self.heap(); }
    WellKnownAtoms const& atoms() { return self.atoms(); }

    struct ContextScope {
        ContextScope(Impl& impl, Context context)
            : m_impl(impl)
        {
            m_impl.contexts.push_back(context);
        }
        ~ContextScope() { m_impl.contexts.pop_back(); }
        ContextScope(ContextScope const&) = delete;
        ContextScope& operator=(ContextScope const&) = delete;
        Context& context() { return m_impl.contexts.back(); }

    private:
        Impl& m_impl;
    };

    // ---- limits

    // The C++ stack against the budget, measured from where script was
    // entered: a deep recursion is a RangeError before it is a crash.
    bool stack_ok()
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

    bool step()
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
    std::string expression_text(Expression const* expression, Context const& cx)
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
    Environment* new_environment(Environment* outer, Object* object = nullptr)
    {
        return heap().allocate<Environment>(outer, object);
    }

    // The nearest environment with a `this`; the global object's has one.
    Value resolve_this(Environment* environment)
    {
        for (Environment* e = environment; e != nullptr; e = e->outer()) {
            if (e->has_this())
                return e->this_value();
        }
        return Value::object(self.global());
    }

    // ResolveBinding (§9.4.2): outward through the chain. A `with`
    // object's @@unscopables is not consulted yet.
    Reference resolve(JsString* name, Environment* environment)
    {
        Reference reference;
        reference.name = name;
        for (Environment* e = environment; e != nullptr; e = e->outer()) {
            if (e->is_object_environment()) {
                PropertyKey const key = PropertyKey::atom(name);
                if (e->object()->has_property(key)) {
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

    bool ensure_key(Reference& reference)
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

    std::string reference_key_text(Reference const& reference)
    {
        return reference.key_ready ? key_description(reference.key) : self.describe(reference.key_value);
    }

    // GetValue (§6.2.5.5).
    std::optional<Value> get_value(Reference& reference, Context const& cx)
    {
        switch (reference.kind) {
        case Reference::Kind::Value:
            return reference.base;
        case Reference::Kind::Unresolvable:
            return self.throw_reference_error(reference.name->to_utf8() + " is not defined");
        case Reference::Kind::Binding: {
            Environment::Binding const* binding = reference.environment->find(reference.name);
            if (binding == nullptr)
                return self.throw_reference_error(reference.name->to_utf8() + " is not defined");
            if (!binding->initialized)
                return self.throw_reference_error("Cannot access '" + reference.name->to_utf8() + "' before initialization");
            return binding->value;
        }
        case Reference::Kind::ObjectEnvironment: {
            // §9.1.1.2.6: the property may be gone by now; strict code
            // notices, sloppy code reads undefined.
            Object* object = reference.environment->object();
            if (!object->has_property(reference.key)) {
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
        }
        return Value::undefined();
    }

    // PutValue (§6.2.5.6).
    bool put_value(Reference& reference, Value const& value, Context const& cx)
    {
        switch (reference.kind) {
        case Reference::Kind::Value:
            self.throw_reference_error("Invalid left-hand side in assignment");
            return false;
        case Reference::Kind::Unresolvable: {
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
            if (!object->has_property(reference.key) && cx.strict) {
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
        }
        return false;
    }

    // The `this` a call through this reference gets (§13.3.6.2).
    Value this_for_call(Reference const& reference)
    {
        if (reference.kind == Reference::Kind::Property)
            return reference.base;
        if (reference.kind == Reference::Kind::ObjectEnvironment && reference.environment->is_with_environment())
            return Value::object(reference.environment->object());
        return Value::undefined();
    }

    // ---- functions

    // SetFunctionName (§10.2.9): the `name` own property, with the
    // accessor prefix when there is one; a symbol names as [description].
    void set_function_name(Object& function, PropertyKey const& key, std::string_view prefix = {})
    {
        std::u16string name;
        if (key.is_symbol()) {
            JsString const* description = key.as_symbol()->description();
            if (description)
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
    // environment of its own between the closure and its scope.
    std::optional<Value> make_closure(FunctionNode const& node, Context& cx, PropertyKey const* name_key)
    {
        Heap::NoCollect const guard(heap());
        Environment* scope = cx.lexical;
        if (!node.is_arrow && node.name != nullptr && !node.is_getter && !node.is_setter && node.is_constructable) {
            scope = new_environment(cx.lexical);
            Environment::Binding& binding = scope->declare(node.name, Value::undefined(), false, true);
            binding.strict = false;
        }
        ScriptFunction* function = self.new_script_function(node, scope);
        if (scope != cx.lexical)
            scope->find(node.name)->value = Value::object(function);
        if (name_key && node.name == nullptr)
            set_function_name(*function, *name_key);
        return Value::object(function);
    }

    // NamedEvaluation (§8.4.5): an anonymous function definition takes
    // the name it is assigned to.
    std::optional<Value> evaluate_named(Expression const* expression, Context& cx, PropertyKey const& name_key)
    {
        if (expression->type == NodeType::ArrowFunction)
            return make_closure(*static_cast<ArrowFunction const*>(expression)->function, cx, &name_key);
        if (expression->type == NodeType::FunctionExpression)
            return make_closure(*static_cast<FunctionExpression const*>(expression)->function, cx, &name_key);
        return evaluate(expression, cx);
    }

    // CreateUnmappedArgumentsObject / CreateMappedArgumentsObject
    // (§10.4.4.6, §10.4.4.7). The mapped form aliases the parameters for
    // sloppy functions with simple, distinct parameter lists; Symbol.iterator
    // is not installed, since the realm has no iterators yet.
    Object* make_arguments_object(ScriptFunction& function, Environment* environment, std::span<Value const> arguments, bool mapped)
    {
        Heap::NoCollect const guard(heap());
        FunctionNode const& node = function.node();
        Object* object = nullptr;
        if (mapped) {
            std::vector<JsString*> names(std::min(node.parameters.size(), arguments.size()), nullptr);
            for (std::size_t i = 0; i < names.size(); ++i)
                names[i] = node.parameters[i];
            object = heap().allocate<ArgumentsObject>(self.intrinsics().object_prototype, environment, std::move(names));
        } else {
            object = heap().allocate<Object>(self.intrinsics().object_prototype, Object::Class::Arguments);
        }
        for (std::size_t i = 0; i < arguments.size(); ++i)
            object->put(PropertyKey::index(static_cast<std::uint32_t>(i)), arguments[i]);
        object->put(PropertyKey::atom(atoms().length), Value::number(static_cast<double>(arguments.size())), builtin_attributes);
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
    std::optional<Value> call_script_function(ScriptFunction& function, Value const& this_argument,
        std::span<Value const> arguments, Object* new_target)
    {
        FunctionNode const& node = function.node();
        Roots const roots(self);
        self.root(Value::object(&function));
        self.root(this_argument);
        for (Value const& argument : arguments)
            self.root(argument);

        Environment* variable = new_environment(function.scope());
        variable->set_function(&function);
        ContextScope scope(*this, Context { variable, variable, node.program, &function, node.is_strict });
        Context& cx = scope.context();

        if (!node.is_arrow) {
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
            variable->set_new_target(new_target);
        }

        // Parameters: for a duplicated name the last occurrence wins.
        for (std::size_t i = 0; i < node.parameters.size(); ++i) {
            Value const value = i < arguments.size() ? arguments[i] : Value::undefined();
            if (Environment::Binding* existing = variable->find(node.parameters[i]))
                existing->value = value;
            else
                variable->declare(node.parameters[i], value);
        }

        // The arguments object, when the body can reach it and nothing of
        // its own shadows it.
        if (!node.is_arrow && (node.uses_arguments || node.has_direct_eval)) {
            bool shadowed = contains(node.parameters, atoms().arguments);
            for (FunctionDeclaration const* declaration : node.declarations.functions) {
                if (declaration->function->name == atoms().arguments)
                    shadowed = true;
            }
            for (auto const& [name, is_const] : node.declarations.lexicals) {
                if (name == atoms().arguments)
                    shadowed = true;
            }
            if (!shadowed) {
                bool const mapped = !node.is_strict && !node.has_duplicate_parameters;
                Object* arguments_object = make_arguments_object(function, variable, arguments, mapped);
                variable->declare(atoms().arguments, Value::object(arguments_object), !node.is_strict, true);
            }
        }

        // Vars: undefined unless a parameter or the arguments object
        // already holds the name.
        for (JsString* name : node.declarations.vars) {
            if (!variable->find(name))
                variable->declare(name, Value::undefined());
        }

        // Top-level lexicals live in their own record when there are any,
        // so a direct eval's `var` can tell them apart from the vars.
        Environment* lexical = variable;
        if (!node.declarations.lexicals.empty()) {
            lexical = new_environment(variable);
            cx.lexical = lexical;
        }
        for (FunctionDeclaration const* declaration : node.declarations.functions) {
            ScriptFunction* closure = self.new_script_function(*declaration->function, lexical);
            JsString* name = declaration->function->name;
            if (Environment::Binding* existing = variable->find(name))
                existing->value = Value::object(closure);
            else
                variable->declare(name, Value::object(closure));
        }
        for (auto const& [name, is_const] : node.declarations.lexicals)
            lexical->declare(name, Value::undefined(), !is_const, false);

        if (node.expression_body)
            return evaluate(node.expression_body, cx);
        Completion const completion = execute_list(node.body, cx);
        if (completion.type == Completion::Type::Throw)
            return std::nullopt;
        if (completion.type == Completion::Type::Return)
            return completion.value.is_empty() ? Value::undefined() : completion.value;
        return Value::undefined();
    }

    // ---- declaration instantiation

    // GlobalDeclarationInstantiation (§16.1.7).
    bool global_declaration_instantiation(Program const& program, Context& cx)
    {
        Object* global = self.global();
        for (auto const& [name, is_const] : program.declarations.lexicals) {
            std::string const text = name->to_utf8();
            if (global_var_names.contains(name) || global_lexical->find(name))
                { self.throw_syntax_error("Identifier '" + text + "' has already been declared"); return false; }
            // HasRestrictedGlobalProperty: a non-configurable global
            // property (undefined, NaN, Infinity) cannot be shadowed.
            std::optional<PropertyDescriptor> const existing = global->get_own_property(PropertyKey::atom(name));
            if (existing && !existing->configurable.value_or(false))
                { self.throw_syntax_error("Identifier '" + text + "' has already been declared"); return false; }
        }
        std::vector<JsString*> var_names;
        for (JsString* name : program.declarations.vars) {
            if (global_lexical->find(name))
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
            if (global_lexical->find(name))
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
            global_lexical->declare(name, Value::undefined(), !is_const, false);
        for (FunctionDeclaration const* declaration : functions) {
            JsString* name = declaration->function->name;
            ScriptFunction* closure = self.new_script_function(*declaration->function, global_lexical);
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
    bool create_global_function_binding(JsString* name, Value const& value, bool deletable)
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
        global_var_names.insert(name);
        return true;
    }

    // CreateGlobalVarBinding (§9.1.1.4.17).
    bool create_global_var_binding(JsString* name, bool deletable)
    {
        Object* global = self.global();
        PropertyKey const key = PropertyKey::atom(name);
        if (!global->get_own_property(key) && global->is_extensible()) {
            PropertyDescriptor const desc = PropertyDescriptor::data(Value::undefined(), static_cast<std::uint8_t>(Writable | Enumerable | (deletable ? Configurable : 0)));
            if (!self.define_property_or_throw(*global, key, desc))
                return false;
        }
        global_var_names.insert(name);
        return true;
    }

    // BlockDeclarationInstantiation (§14.2.3), with B.3.2.1's hoisting
    // already settled by the parser.
    void instantiate_block(Declarations const& declarations, Environment* environment)
    {
        for (auto const& [name, is_const] : declarations.lexicals)
            environment->declare(name, Value::undefined(), !is_const, false);
        for (FunctionDeclaration const* declaration : declarations.functions) {
            JsString* name = declaration->function->name;
            ScriptFunction* closure = self.new_script_function(*declaration->function, environment);
            if (Environment::Binding* existing = environment->find(name))
                existing->value = Value::object(closure);
            else
                environment->declare(name, Value::object(closure));
        }
    }

    // EvalDeclarationInstantiation (§19.2.1.3).
    bool eval_declaration_instantiation(Program const& program, Environment* variable, Environment* lexical, bool strict)
    {
        bool const global_scope = variable->is_object_environment();
        if (!strict) {
            std::vector<JsString*> names = program.declarations.vars;
            for (FunctionDeclaration const* declaration : program.declarations.functions)
                names.push_back(declaration->function->name);
            if (global_scope) {
                for (JsString* name : names) {
                    if (global_lexical->find(name))
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
            ScriptFunction* closure = self.new_script_function(*declaration->function, lexical);
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
    Environment* variable_environment_of(Environment* environment)
    {
        for (Environment* e = environment; e != nullptr; e = e->outer()) {
            if (e->function() != nullptr)
                return e;
            if (e->is_object_environment() && !e->is_with_environment())
                return e;
        }
        return self.intrinsics().global_environment;
    }

    // PerformEval (§19.2.1.1) for both the direct and the indirect form.
    std::optional<Value> perform_eval(std::u16string_view source, Environment* scope, bool strict_caller, Value this_value, bool direct)
    {
        ParseOptions options;
        options.strict = direct && strict_caller;
        Environment* variable = direct ? variable_environment_of(scope) : self.intrinsics().global_environment;
        options.in_function = direct && variable->function() != nullptr;
        Parser parser(heap(), std::u16string(source), options);
        std::unique_ptr<Program> program = parser.parse_program("eval");
        if (!program) {
            ParseError const error = parser.error().value_or(ParseError { {}, "parse failed" });
            return self.throw_syntax_error(error.message);
        }
        bool const strict = options.strict || program->is_strict;
        Program const* tree = program.get();
        self.keep(std::move(program));

        Environment* outer = direct ? scope : global_lexical;
        Environment* lexical = new_environment(outer);
        if (strict)
            variable = lexical;
        if (!this_value.is_empty())
            lexical->set_this(this_value);
        ContextScope context_scope(*this, Context { lexical, variable, tree, direct && variable->function() ? static_cast<ScriptFunction*>(variable->function()) : nullptr, strict });
        Context& cx = context_scope.context();
        if (!eval_declaration_instantiation(*tree, variable, lexical, strict))
            return std::nullopt;
        Completion const completion = execute_list(tree->body, cx);
        if (completion.type == Completion::Type::Throw)
            return std::nullopt;
        return completion.value.is_empty() ? Value::undefined() : completion.value;
    }

    // ---- expressions

    std::optional<Value> evaluate(Expression const* expression, Context& cx)
    {
        if (!stack_ok())
            return std::nullopt;
        switch (expression->type) {
        case NodeType::Identifier: {
            Reference reference = resolve(static_cast<Identifier const*>(expression)->name, cx.lexical);
            return get_value(reference, cx);
        }
        case NodeType::NumberLiteral:
            return Value::number(static_cast<NumberLiteral const*>(expression)->value);
        case NodeType::StringLiteral:
            return Value::string(static_cast<StringLiteral const*>(expression)->value);
        case NodeType::BooleanLiteral:
            return Value::boolean(static_cast<BooleanLiteral const*>(expression)->value);
        case NodeType::NullLiteral:
            return Value::null();
        case NodeType::ThisExpression:
            return resolve_this(cx.lexical);
        case NodeType::RegExpLiteral:
            return evaluate_regexp(*static_cast<RegExpLiteral const*>(expression));
        case NodeType::TemplateLiteral:
            return evaluate_template(*static_cast<TemplateLiteral const*>(expression), cx);
        case NodeType::ArrayLiteral:
            return evaluate_array(*static_cast<ArrayLiteral const*>(expression), cx);
        case NodeType::ObjectLiteral:
            return evaluate_object(*static_cast<ObjectLiteral const*>(expression), cx);
        case NodeType::FunctionExpression:
            return make_closure(*static_cast<FunctionExpression const*>(expression)->function, cx, nullptr);
        case NodeType::ArrowFunction:
            return make_closure(*static_cast<ArrowFunction const*>(expression)->function, cx, nullptr);
        case NodeType::UnaryExpression:
            return evaluate_unary(*static_cast<UnaryExpression const*>(expression), cx);
        case NodeType::UpdateExpression:
            return evaluate_update(*static_cast<UpdateExpression const*>(expression), cx);
        case NodeType::BinaryExpression:
            return evaluate_binary(*static_cast<BinaryExpression const*>(expression), cx);
        case NodeType::LogicalExpression:
            return evaluate_logical(*static_cast<LogicalExpression const*>(expression), cx);
        case NodeType::AssignmentExpression:
            return evaluate_assignment(*static_cast<AssignmentExpression const*>(expression), cx);
        case NodeType::ConditionalExpression: {
            auto const& conditional = *static_cast<ConditionalExpression const*>(expression);
            std::optional<Value> const test = evaluate(conditional.test, cx);
            if (!test)
                return std::nullopt;
            return evaluate(to_boolean(*test) ? conditional.consequent : conditional.alternate, cx);
        }
        case NodeType::CallExpression:
        case NodeType::MemberExpression: {
            bool short_circuit = false;
            return evaluate_chain(expression, cx, short_circuit, nullptr);
        }
        case NodeType::NewExpression:
            return evaluate_new(*static_cast<NewExpression const*>(expression), cx);
        case NodeType::SequenceExpression: {
            Value last;
            for (Expression const* part : static_cast<SequenceExpression const*>(expression)->expressions) {
                std::optional<Value> const value = evaluate(part, cx);
                if (!value)
                    return std::nullopt;
                last = *value;
            }
            return last;
        }
        default:
            break;
        }
        return self.throw_syntax_error("unsupported expression");
    }

    static bool to_boolean(Value const& value) { return Interpreter::to_boolean(value); }

    std::optional<Value> evaluate_regexp(RegExpLiteral const& literal)
    {
        // §13.2.7.3: a fresh RegExp object each time the literal is
        // evaluated, made the way the constructor makes one.
        Value const arguments[2] = { Value::string(literal.pattern), Value::string(literal.flags) };
        Function* constructor = self.intrinsics().regexp_constructor;
        if (constructor == nullptr)
            return self.throw_syntax_error("regular expressions are not supported yet");
        return self.construct(Value::object(constructor), arguments);
    }

    std::optional<Value> evaluate_template(TemplateLiteral const& literal, Context& cx)
    {
        // §13.2.8.6: the cooked spans with each substitution's ToString.
        std::u16string result = literal.cooked[0]->data();
        for (std::size_t i = 0; i < literal.expressions.size(); ++i) {
            std::optional<Value> const value = evaluate(literal.expressions[i], cx);
            if (!value)
                return std::nullopt;
            Roots const roots(self);
            self.root(*value);
            std::optional<JsString*> const text = self.to_string(*value);
            if (!text)
                return std::nullopt;
            result += (*text)->data();
            result += literal.cooked[i + 1]->data();
        }
        return Value::string(heap().string(std::move(result)));
    }

    std::optional<Value> evaluate_array(ArrayLiteral const& literal, Context& cx)
    {
        // §13.2.4.2: elisions are holes; the length counts them.
        Roots const roots(self);
        ArrayObject* array = self.new_array();
        self.root(Value::object(array));
        std::uint32_t index = 0;
        for (Expression const* element : literal.elements) {
            if (element) {
                std::optional<Value> const value = evaluate(element, cx);
                if (!value)
                    return std::nullopt;
                array->set_element(index, *value);
            }
            ++index;
        }
        array->set_length(index);
        return Value::object(array);
    }

    std::optional<Value> evaluate_object(ObjectLiteral const& literal, Context& cx)
    {
        // PropertyDefinitionEvaluation (§13.2.5.5).
        Roots const roots(self);
        Object* object = self.new_object();
        self.root(Value::object(object));
        for (PropertyDefinition const& property : literal.properties) {
            if (property.is_proto) {
                std::optional<Value> const value = evaluate(property.value, cx);
                if (!value)
                    return std::nullopt;
                if (value->is_object())
                    object->set_prototype(value->as_object());
                else if (value->is_null())
                    object->set_prototype(nullptr);
                continue;
            }
            PropertyKey key;
            if (property.computed_key) {
                std::optional<Value> const key_value = evaluate(property.computed_key, cx);
                if (!key_value)
                    return std::nullopt;
                self.root(*key_value);
                std::optional<PropertyKey> const converted = self.to_property_key(*key_value);
                if (!converted)
                    return std::nullopt;
                key = *converted;
                if (key.is_symbol())
                    self.root(Value::symbol(key.as_symbol()));
            } else {
                key = heap().key(property.key);
            }
            if (property.kind == PropertyDefinition::Kind::Init) {
                std::optional<Value> value;
                if (property.value->type == NodeType::FunctionExpression || property.value->type == NodeType::ArrowFunction)
                    value = evaluate_named(property.value, cx, key);
                else
                    value = evaluate(property.value, cx);
                if (!value)
                    return std::nullopt;
                self.root(*value);
                if (!self.create_data_property(*object, key, *value))
                    return std::nullopt;
                continue;
            }
            // A getter or setter joins an existing accessor's other half.
            FunctionNode const& node = *static_cast<FunctionExpression const*>(property.value)->function;
            Heap::NoCollect const guard(heap());
            ScriptFunction* accessor = self.new_script_function(node, cx.lexical);
            set_function_name(*accessor, key, property.kind == PropertyDefinition::Kind::Get ? "get" : "set");
            Object* getter = nullptr;
            Object* setter = nullptr;
            if (Property const* existing = object->find_own(key); existing && existing->accessor) {
                getter = existing->getter;
                setter = existing->setter;
            }
            if (property.kind == PropertyDefinition::Kind::Get)
                getter = accessor;
            else
                setter = accessor;
            object->put_accessor(key, getter, setter, Enumerable | Configurable);
        }
        return Value::object(object);
    }

    // A reference to `object.name` / `object[property]`, evaluated as the
    // link of an optional chain it may be (§13.3.9). The key of a
    // computed access stays a value until GetValue/PutValue converts it,
    // after the base's own check, as §6.2.5.5 orders them.
    std::optional<Reference> evaluate_member_reference(MemberExpression const& member, Context& cx, bool& short_circuit)
    {
        Reference reference;
        std::optional<Value> base;
        if ((member.object->type == NodeType::MemberExpression || member.object->type == NodeType::CallExpression) && !member.object->parenthesized) {
            base = evaluate_chain(member.object, cx, short_circuit, nullptr);
            if (!base)
                return std::nullopt;
            if (short_circuit) {
                reference.kind = Reference::Kind::Value;
                reference.base = Value::undefined();
                return reference;
            }
        } else {
            base = evaluate(member.object, cx);
            if (!base)
                return std::nullopt;
        }
        if (member.optional && base->is_nullish()) {
            short_circuit = true;
            reference.kind = Reference::Kind::Value;
            reference.base = Value::undefined();
            return reference;
        }
        reference.kind = Reference::Kind::Property;
        reference.base = *base;
        Roots const roots(self);
        self.root(*base);
        if (member.property) {
            std::optional<Value> const key_value = evaluate(member.property, cx);
            if (!key_value)
                return std::nullopt;
            reference.key_value = *key_value;
            // Strings and numbers convert at once (nothing observable
            // happens); an object waits for the base check.
            if (!key_value->is_object()) {
                std::optional<PropertyKey> const key = self.to_property_key(*key_value);
                if (!key)
                    return std::nullopt;
                reference.key = *key;
                reference.key_ready = true;
            }
        } else {
            reference.key = heap().key(member.name);
            reference.key_ready = true;
        }
        return reference;
    }

    // A member or call expression, as a link of a chain: the value, and
    // for a member the reference too, so a call knows its `this`.
    std::optional<Value> evaluate_chain(Expression const* expression, Context& cx, bool& short_circuit, Reference* out_reference)
    {
        if (expression->type == NodeType::MemberExpression) {
            std::optional<Reference> reference = evaluate_member_reference(*static_cast<MemberExpression const*>(expression), cx, short_circuit);
            if (!reference)
                return std::nullopt;
            if (short_circuit)
                return Value::undefined();
            Roots const roots(self);
            self.root(reference->base);
            self.root(reference->key_value);
            std::optional<Value> const value = get_value(*reference, cx);
            if (!value)
                return std::nullopt;
            if (out_reference)
                *out_reference = *reference;
            return value;
        }
        return evaluate_call(*static_cast<CallExpression const*>(expression), cx, short_circuit);
    }

    std::optional<Value> evaluate_arguments(std::vector<Expression*> const& expressions, Context& cx, std::vector<Value>& values)
    {
        // The caller's Roots scope keeps each argument alive.
        values.reserve(expressions.size());
        for (Expression const* expression : expressions) {
            std::optional<Value> const value = evaluate(expression, cx);
            if (!value)
                return std::nullopt;
            self.root(*value);
            values.push_back(*value);
        }
        return Value::undefined();
    }

    std::optional<Value> evaluate_call(CallExpression const& call, Context& cx, bool& short_circuit)
    {
        // §13.3.6: the callee is evaluated as a reference so that a
        // member call gets its base as `this`; a direct eval is told apart
        // by the callee resolving to the intrinsic.
        Roots const roots(self);
        Value callee_value;
        Value this_value;
        Expression const* callee = call.callee;
        if (callee->type == NodeType::MemberExpression) {
            // Parentheses keep the reference (`(o.m)()` still calls with
            // o as this) but end an optional chain: a short-circuit
            // inside them yields undefined, which is then not callable.
            Reference reference;
            bool inner_short_circuit = false;
            bool& flag = callee->parenthesized ? inner_short_circuit : short_circuit;
            std::optional<Value> const value = evaluate_chain(callee, cx, flag, &reference);
            if (!value)
                return std::nullopt;
            if (flag) {
                if (!callee->parenthesized)
                    return Value::undefined();
                callee_value = Value::undefined();
            } else {
                callee_value = *value;
                this_value = this_for_call(reference);
            }
        } else if (callee->type == NodeType::CallExpression && !callee->parenthesized) {
            std::optional<Value> const value = evaluate_chain(callee, cx, short_circuit, nullptr);
            if (!value)
                return std::nullopt;
            if (short_circuit)
                return Value::undefined();
            callee_value = *value;
        } else if (callee->type == NodeType::Identifier) {
            Reference reference = resolve(static_cast<Identifier const*>(callee)->name, cx.lexical);
            std::optional<Value> const value = get_value(reference, cx);
            if (!value)
                return std::nullopt;
            callee_value = *value;
            this_value = this_for_call(reference);
        } else {
            std::optional<Value> const value = evaluate(callee, cx);
            if (!value)
                return std::nullopt;
            callee_value = *value;
        }
        self.root(callee_value);
        self.root(this_value);
        if (call.optional && callee_value.is_nullish()) {
            short_circuit = true;
            return Value::undefined();
        }
        std::vector<Value> arguments;
        if (!evaluate_arguments(call.arguments, cx, arguments))
            return std::nullopt;
        if (call.is_direct_eval && callee_value.is_object() && callee_value.as_object() == self.intrinsics().eval) {
            // §13.3.6.1 step 6: a direct eval of a string runs in this
            // scope; anything else is returned as it is.
            if (arguments.empty())
                return Value::undefined();
            if (!arguments[0].is_string())
                return arguments[0];
            if (!step())
                return std::nullopt;
            return perform_eval(arguments[0].as_string()->view(), cx.lexical, cx.strict, Value::empty(), true);
        }
        if (!Interpreter::is_callable(callee_value))
            return self.throw_type_error(expression_text(callee, cx) + " is not a function");
        return self.call(callee_value, this_value, arguments);
    }

    std::optional<Value> evaluate_new(NewExpression const& expression, Context& cx)
    {
        // §13.3.5.1.
        Roots const roots(self);
        std::optional<Value> const constructor = evaluate(expression.callee, cx);
        if (!constructor)
            return std::nullopt;
        self.root(*constructor);
        std::vector<Value> arguments;
        if (!evaluate_arguments(expression.arguments, cx, arguments))
            return std::nullopt;
        if (!Interpreter::is_constructor(*constructor))
            return self.throw_type_error(expression_text(expression.callee, cx) + " is not a constructor");
        return self.construct(*constructor, arguments);
    }

    // The reference an assignment, update or delete targets. Only an
    // identifier or a member expression is a reference; anything else is
    // evaluated for its value.
    std::optional<Reference> evaluate_reference(Expression const* expression, Context& cx)
    {
        if (expression->type == NodeType::Identifier)
            return resolve(static_cast<Identifier const*>(expression)->name, cx.lexical);
        if (expression->type == NodeType::MemberExpression) {
            bool short_circuit = false;
            return evaluate_member_reference(*static_cast<MemberExpression const*>(expression), cx, short_circuit);
        }
        std::optional<Value> const value = evaluate(expression, cx);
        if (!value)
            return std::nullopt;
        Reference reference;
        reference.kind = Reference::Kind::Value;
        reference.base = *value;
        return reference;
    }

    std::optional<Value> evaluate_unary(UnaryExpression const& unary, Context& cx)
    {
        switch (unary.op) {
        case UnaryOp::Typeof: {
            // §13.5.3: an unresolvable name is "undefined" rather than a
            // ReferenceError; a binding in its dead zone still throws.
            std::optional<Reference> reference = evaluate_reference(unary.operand, cx);
            if (!reference)
                return std::nullopt;
            if (reference->kind == Reference::Kind::Unresolvable)
                return Value::string(atoms().undefined);
            Roots const roots(self);
            self.root(reference->base);
            self.root(reference->key_value);
            std::optional<Value> const value = get_value(*reference, cx);
            if (!value)
                return std::nullopt;
            return Value::string(self.type_of(*value));
        }
        case UnaryOp::Delete:
            return evaluate_delete(unary, cx);
        case UnaryOp::Void: {
            if (!evaluate(unary.operand, cx))
                return std::nullopt;
            return Value::undefined();
        }
        default:
            break;
        }
        std::optional<Value> const operand = evaluate(unary.operand, cx);
        if (!operand)
            return std::nullopt;
        switch (unary.op) {
        case UnaryOp::Not:
            return Value::boolean(!to_boolean(*operand));
        case UnaryOp::Minus: {
            std::optional<double> const number = self.to_number(*operand);
            if (!number)
                return std::nullopt;
            return Value::number(-*number);
        }
        case UnaryOp::Plus: {
            std::optional<double> const number = self.to_number(*operand);
            if (!number)
                return std::nullopt;
            return Value::number(*number);
        }
        case UnaryOp::BitwiseNot: {
            std::optional<std::int32_t> const number = self.to_int32(*operand);
            if (!number)
                return std::nullopt;
            return Value::number(static_cast<double>(~*number));
        }
        default:
            break;
        }
        return Value::undefined();
    }

    std::optional<Value> evaluate_delete(UnaryExpression const& unary, Context& cx)
    {
        // §13.5.1.2: a property is deleted from the boxed base, strictly a
        // TypeError when that fails; a binding only when it is deletable;
        // anything that is not a reference deletes as true.
        std::optional<Reference> reference = evaluate_reference(unary.operand, cx);
        if (!reference)
            return std::nullopt;
        switch (reference->kind) {
        case Reference::Kind::Value:
        case Reference::Kind::Unresolvable:
            return Value::boolean(true);
        case Reference::Kind::Binding:
            return Value::boolean(reference->environment->remove(reference->name));
        case Reference::Kind::ObjectEnvironment:
            return Value::boolean(reference->environment->object()->delete_property(reference->key));
        case Reference::Kind::Property: {
            Roots const roots(self);
            self.root(reference->base);
            self.root(reference->key_value);
            if (reference->base.is_nullish())
                return self.throw_type_error("Cannot convert undefined or null to object");
            std::optional<Object*> const object = self.to_object(reference->base);
            if (!object)
                return std::nullopt;
            self.root(Value::object(*object));
            if (!ensure_key(*reference))
                return std::nullopt;
            bool const deleted = (*object)->delete_property(reference->key);
            if (!deleted && cx.strict)
                return self.throw_type_error("Cannot delete property '" + key_description(reference->key) + "' of object");
            return Value::boolean(deleted);
        }
        }
        return Value::boolean(true);
    }

    std::optional<Value> evaluate_update(UpdateExpression const& update, Context& cx)
    {
        // §13.4.2–§13.4.5: the old value as a number, the new one stored,
        // the prefix form yielding the new and the postfix the old.
        std::optional<Reference> reference = evaluate_reference(update.target, cx);
        if (!reference)
            return std::nullopt;
        Roots const roots(self);
        self.root(reference->base);
        self.root(reference->key_value);
        std::optional<Value> const old_value = get_value(*reference, cx);
        if (!old_value)
            return std::nullopt;
        self.root(*old_value);
        std::optional<double> const old_number = self.to_number(*old_value);
        if (!old_number)
            return std::nullopt;
        double const new_number = update.increment ? *old_number + 1 : *old_number - 1;
        if (!put_value(*reference, Value::number(new_number), cx))
            return std::nullopt;
        return Value::number(update.prefix ? new_number : *old_number);
    }

    // The operator of a binary or compound-assignment expression applied
    // to two evaluated operands (§13.6–§13.12, §13.15.3 ApplyStringOrNumericBinaryOperator).
    std::optional<Value> apply_binary(BinaryOp op, Value const& left, Value const& right)
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
            std::optional<double> const lnum = self.to_number(*lprim);
            if (!lnum)
                return std::nullopt;
            std::optional<double> const rnum = self.to_number(*rprim);
            if (!rnum)
                return std::nullopt;
            return Value::number(*lnum + *rnum);
        }
        case BinaryOp::Subtract:
        case BinaryOp::Multiply:
        case BinaryOp::Divide:
        case BinaryOp::Remainder:
        case BinaryOp::Exponent: {
            std::optional<double> const lnum = self.to_number(left);
            if (!lnum)
                return std::nullopt;
            std::optional<double> const rnum = self.to_number(right);
            if (!rnum)
                return std::nullopt;
            switch (op) {
            case BinaryOp::Subtract:
                return Value::number(*lnum - *rnum);
            case BinaryOp::Multiply:
                return Value::number(*lnum * *rnum);
            case BinaryOp::Divide:
                return Value::number(*lnum / *rnum);
            case BinaryOp::Remainder:
                return Value::number(std::fmod(*lnum, *rnum));
            default:
                return Value::number(number_exponentiate(*lnum, *rnum));
            }
        }
        case BinaryOp::LeftShift:
        case BinaryOp::RightShift:
        case BinaryOp::UnsignedRightShift:
        case BinaryOp::BitwiseAnd:
        case BinaryOp::BitwiseOr:
        case BinaryOp::BitwiseXor: {
            std::optional<std::int32_t> const lnum = self.to_int32(left);
            if (!lnum)
                return std::nullopt;
            std::optional<std::uint32_t> const rnum = self.to_uint32(right);
            if (!rnum)
                return std::nullopt;
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
            return Value::boolean(right.as_object()->has_property(*key));
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

    std::optional<Value> evaluate_binary(BinaryExpression const& binary, Context& cx)
    {
        std::optional<Value> const left = evaluate(binary.left, cx);
        if (!left)
            return std::nullopt;
        Roots const roots(self);
        self.root(*left);
        std::optional<Value> const right = evaluate(binary.right, cx);
        if (!right)
            return std::nullopt;
        return apply_binary(binary.op, *left, *right);
    }

    std::optional<Value> evaluate_logical(LogicalExpression const& logical, Context& cx)
    {
        // §13.13: the left operand decides whether the right is evaluated.
        std::optional<Value> const left = evaluate(logical.left, cx);
        if (!left)
            return std::nullopt;
        bool evaluate_right = false;
        switch (logical.op) {
        case LogicalOp::And:
            evaluate_right = to_boolean(*left);
            break;
        case LogicalOp::Or:
            evaluate_right = !to_boolean(*left);
            break;
        case LogicalOp::Nullish:
            evaluate_right = left->is_nullish();
            break;
        }
        if (!evaluate_right)
            return *left;
        return evaluate(logical.right, cx);
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

    std::optional<Value> evaluate_assignment(AssignmentExpression const& assignment, Context& cx)
    {
        // §13.15.2: the target reference first, then the right-hand side,
        // then the store. A compound operator reads the target once, and
        // a logical one may not evaluate the right-hand side at all.
        std::optional<Reference> reference = evaluate_reference(assignment.target, cx);
        if (!reference)
            return std::nullopt;
        Roots const roots(self);
        self.root(reference->base);
        self.root(reference->key_value);
        bool const named = assignment.target->type == NodeType::Identifier && is_anonymous_function_definition(assignment.value);
        auto evaluate_value = [&]() -> std::optional<Value> {
            if (named)
                return evaluate_named(assignment.value, cx, PropertyKey::atom(static_cast<Identifier const*>(assignment.target)->name));
            return evaluate(assignment.value, cx);
        };
        if (assignment.op == AssignmentOp::Assign) {
            std::optional<Value> const value = evaluate_value();
            if (!value)
                return std::nullopt;
            self.root(*value);
            if (!put_value(*reference, *value, cx))
                return std::nullopt;
            return *value;
        }
        std::optional<Value> const current = get_value(*reference, cx);
        if (!current)
            return std::nullopt;
        self.root(*current);
        if (assignment.op == AssignmentOp::LogicalAnd || assignment.op == AssignmentOp::LogicalOr || assignment.op == AssignmentOp::Nullish) {
            bool proceed = false;
            switch (assignment.op) {
            case AssignmentOp::LogicalAnd:
                proceed = to_boolean(*current);
                break;
            case AssignmentOp::LogicalOr:
                proceed = !to_boolean(*current);
                break;
            default:
                proceed = current->is_nullish();
                break;
            }
            if (!proceed)
                return *current;
            std::optional<Value> const value = evaluate_value();
            if (!value)
                return std::nullopt;
            self.root(*value);
            if (!put_value(*reference, *value, cx))
                return std::nullopt;
            return *value;
        }
        std::optional<Value> const operand = evaluate(assignment.value, cx);
        if (!operand)
            return std::nullopt;
        self.root(*operand);
        std::optional<Value> const result = apply_binary(*binary_for(assignment.op), *current, *operand);
        if (!result)
            return std::nullopt;
        self.root(*result);
        if (!put_value(*reference, *result, cx))
            return std::nullopt;
        return *result;
    }

    // ---- statements

    // LoopContinues (§14.7.1.2).
    static bool loop_continues(Completion const& completion, std::span<JsString* const> labels)
    {
        if (completion.type == Completion::Type::Normal)
            return true;
        if (completion.type != Completion::Type::Continue)
            return false;
        if (completion.target == nullptr)
            return true;
        return std::find(labels.begin(), labels.end(), completion.target) != labels.end();
    }

    // The exit of a breakable statement (§14.13.4 step 3, §14.7.1.1): a
    // `break` with no label ends it normally, with the value kept so far.
    static Completion finish_loop(Completion const& completion, Value const& last)
    {
        Completion result = completion.with_value_if_empty(last);
        if (result.type == Completion::Type::Break && result.target == nullptr) {
            result.type = Completion::Type::Normal;
            result.target = nullptr;
        }
        return result;
    }

    Completion execute_list(std::span<Statement* const> statements, Context& cx)
    {
        // §14.2.2: the value of a list is the last non-empty value, which
        // an abrupt completion carries out as well (UpdateEmpty).
        Roots const roots(self);
        Value& last = self.root(Value::empty());
        for (Statement const* statement : statements) {
            Completion const completion = execute(statement, cx, {});
            if (completion.is_abrupt())
                return completion.with_value_if_empty(last);
            if (!completion.value.is_empty())
                last = completion.value;
        }
        return Completion::normal(last);
    }

    Completion execute(Statement const* statement, Context& cx, std::span<JsString* const> labels)
    {
        if (!step())
            return Completion::thrown();
        switch (statement->type) {
        case NodeType::VariableDeclaration:
            return execute_declaration(*static_cast<VariableDeclaration const*>(statement), cx);
        case NodeType::FunctionDeclaration:
            return execute_function_declaration(*static_cast<FunctionDeclaration const*>(statement), cx);
        case NodeType::ExpressionStatement: {
            std::optional<Value> const value = evaluate(static_cast<ExpressionStatement const*>(statement)->expression, cx);
            if (!value)
                return Completion::thrown();
            return Completion::normal(*value);
        }
        case NodeType::BlockStatement:
            return execute_block(*static_cast<BlockStatement const*>(statement), cx);
        case NodeType::EmptyStatement:
        case NodeType::DebuggerStatement:
            return Completion::normal();
        case NodeType::IfStatement: {
            auto const& branch = *static_cast<IfStatement const*>(statement);
            std::optional<Value> const test = evaluate(branch.test, cx);
            if (!test)
                return Completion::thrown();
            Completion completion;
            if (to_boolean(*test))
                completion = execute(branch.consequent, cx, {});
            else if (branch.alternate)
                completion = execute(branch.alternate, cx, {});
            return completion.with_value_if_empty(Value::undefined());
        }
        case NodeType::ForStatement:
            return execute_for(*static_cast<ForStatement const*>(statement), cx, labels);
        case NodeType::ForInStatement:
            return execute_for_in(*static_cast<ForInStatement const*>(statement), cx, labels);
        case NodeType::WhileStatement:
            return execute_while(*static_cast<WhileStatement const*>(statement), cx, labels);
        case NodeType::DoWhileStatement:
            return execute_do_while(*static_cast<DoWhileStatement const*>(statement), cx, labels);
        case NodeType::ReturnStatement: {
            auto const& ret = *static_cast<ReturnStatement const*>(statement);
            Completion completion;
            completion.type = Completion::Type::Return;
            completion.value = Value::undefined();
            if (ret.argument) {
                std::optional<Value> const value = evaluate(ret.argument, cx);
                if (!value)
                    return Completion::thrown();
                completion.value = *value;
            }
            return completion;
        }
        case NodeType::BreakStatement: {
            Completion completion;
            completion.type = Completion::Type::Break;
            completion.target = static_cast<BreakStatement const*>(statement)->label;
            return completion;
        }
        case NodeType::ContinueStatement: {
            Completion completion;
            completion.type = Completion::Type::Continue;
            completion.target = static_cast<ContinueStatement const*>(statement)->label;
            return completion;
        }
        case NodeType::ThrowStatement: {
            std::optional<Value> const value = evaluate(static_cast<ThrowStatement const*>(statement)->argument, cx);
            if (!value)
                return Completion::thrown();
            self.throw_value(*value);
            return Completion::thrown();
        }
        case NodeType::TryStatement:
            return execute_try(*static_cast<TryStatement const*>(statement), cx);
        case NodeType::SwitchStatement:
            return execute_switch(*static_cast<SwitchStatement const*>(statement), cx);
        case NodeType::LabeledStatement: {
            // LabelledEvaluation (§14.13.4): the label joins the set the
            // body sees, and a break aimed at it ends here, normally.
            auto const& labelled = *static_cast<LabeledStatement const*>(statement);
            std::vector<JsString*> extended(labels.begin(), labels.end());
            extended.push_back(labelled.label);
            Completion completion = execute(labelled.body, cx, extended);
            if (completion.type == Completion::Type::Break && completion.target == labelled.label) {
                completion.type = Completion::Type::Normal;
                completion.target = nullptr;
            }
            return completion;
        }
        case NodeType::WithStatement:
            return execute_with(*static_cast<WithStatement const*>(statement), cx);
        default:
            break;
        }
        self.throw_syntax_error("unsupported statement");
        return Completion::thrown();
    }

    Completion execute_declaration(VariableDeclaration const& declaration, Context& cx)
    {
        for (VariableDeclarator const& declarator : declaration.declarations) {
            if (declaration.kind == VariableDeclaration::Kind::Var) {
                // §14.3.2.1: a var with an initializer assigns through a
                // reference resolved now, so a `with` in scope can catch it.
                if (!declarator.init)
                    continue;
                Reference reference = resolve(declarator.name, cx.lexical);
                std::optional<Value> value;
                if (is_anonymous_function_definition(declarator.init))
                    value = evaluate_named(declarator.init, cx, PropertyKey::atom(declarator.name));
                else
                    value = evaluate(declarator.init, cx);
                if (!value)
                    return Completion::thrown();
                Roots const roots(self);
                self.root(*value);
                if (!put_value(reference, *value, cx))
                    return Completion::thrown();
                continue;
            }
            // §14.3.1.2: let and const initialize the binding declared at
            // the scope's entry; `let x;` initializes to undefined.
            std::optional<Value> value = Value::undefined();
            if (declarator.init) {
                if (is_anonymous_function_definition(declarator.init))
                    value = evaluate_named(declarator.init, cx, PropertyKey::atom(declarator.name));
                else
                    value = evaluate(declarator.init, cx);
                if (!value)
                    return Completion::thrown();
            }
            Environment::Binding* binding = nullptr;
            for (Environment* e = cx.lexical; e != nullptr && binding == nullptr; e = e->outer())
                binding = e->find(declarator.name);
            if (binding == nullptr) {
                self.throw_reference_error(declarator.name->to_utf8() + " is not defined");
                return Completion::thrown();
            }
            binding->value = *value;
            binding->initialized = true;
        }
        return Completion::normal();
    }

    Completion execute_function_declaration(FunctionDeclaration const& declaration, Context& cx)
    {
        // The declaration itself was instantiated at scope entry. In
        // sloppy code a block-level one also writes its current value to
        // the var binding the parser hoisted for it (B.3.2.1 step 2.b).
        if (cx.strict)
            return Completion::normal();
        JsString* name = declaration.function->name;
        Declarations const& top = cx.function ? cx.function->node().declarations : cx.program->declarations;
        if (!contains(top.vars, name) || cx.lexical == cx.variable)
            return Completion::normal();
        Environment::Binding const* block_binding = cx.lexical->find(name);
        if (block_binding == nullptr)
            return Completion::normal();
        Value const value = block_binding->value;
        Roots const roots(self);
        self.root(value);
        if (cx.variable->is_object_environment()) {
            if (!self.set(*cx.variable->object(), PropertyKey::atom(name), value, false))
                return Completion::thrown();
        } else if (Environment::Binding* var_binding = cx.variable->find(name)) {
            var_binding->value = value;
        }
        return Completion::normal();
    }

    Completion execute_block(BlockStatement const& block, Context& cx)
    {
        // §14.2.2: a block with declarations gets an environment of its own.
        if (block.declarations.lexicals.empty() && block.declarations.functions.empty())
            return execute_list(block.body, cx);
        Environment* const saved = cx.lexical;
        cx.lexical = new_environment(saved);
        instantiate_block(block.declarations, cx.lexical);
        Completion const completion = execute_list(block.body, cx);
        cx.lexical = saved;
        return completion;
    }

    // CreatePerIterationEnvironment (§14.7.4.4): a fresh copy of the loop
    // variables, so that closures made in one iteration keep its values.
    void copy_iteration_environment(Context& cx, std::vector<JsString*> const& names)
    {
        Environment* const previous = cx.lexical;
        Environment* copy = new_environment(previous->outer());
        for (JsString* name : names) {
            Environment::Binding const* binding = previous->find(name);
            copy->declare(name, binding ? binding->value : Value::undefined(), true, binding ? binding->initialized : true);
        }
        cx.lexical = copy;
    }

    Completion execute_for(ForStatement const& loop, Context& cx, std::span<JsString* const> labels)
    {
        // §14.7.4.2, §14.7.4.3 ForBodyEvaluation.
        Environment* const saved = cx.lexical;
        std::vector<JsString*> per_iteration;
        if (!loop.declarations.lexicals.empty()) {
            cx.lexical = new_environment(saved);
            for (auto const& [name, is_const] : loop.declarations.lexicals) {
                cx.lexical->declare(name, Value::undefined(), !is_const, false);
                if (!is_const)
                    per_iteration.push_back(name);
            }
        }
        auto restore = [&](Completion completion) {
            cx.lexical = saved;
            return completion;
        };
        if (loop.init) {
            Completion const init = execute(loop.init, cx, {});
            if (init.is_abrupt())
                return restore(init);
        }
        if (!per_iteration.empty())
            copy_iteration_environment(cx, per_iteration);
        Roots const roots(self);
        Value& last = self.root(Value::undefined());
        while (true) {
            if (loop.test) {
                std::optional<Value> const test = evaluate(loop.test, cx);
                if (!test)
                    return restore(Completion::thrown());
                if (!to_boolean(*test))
                    return restore(Completion::normal(last));
            }
            Completion const body = execute(loop.body, cx, {});
            if (!loop_continues(body, labels))
                return restore(finish_loop(body, last));
            if (!body.value.is_empty())
                last = body.value;
            if (!per_iteration.empty())
                copy_iteration_environment(cx, per_iteration);
            if (loop.update) {
                if (!evaluate(loop.update, cx))
                    return restore(Completion::thrown());
            }
            if (!step())
                return restore(Completion::thrown());
        }
    }

    Completion execute_while(WhileStatement const& loop, Context& cx, std::span<JsString* const> labels)
    {
        Roots const roots(self);
        Value& last = self.root(Value::undefined());
        while (true) {
            std::optional<Value> const test = evaluate(loop.test, cx);
            if (!test)
                return Completion::thrown();
            if (!to_boolean(*test))
                return Completion::normal(last);
            Completion const body = execute(loop.body, cx, {});
            if (!loop_continues(body, labels))
                return finish_loop(body, last);
            if (!body.value.is_empty())
                last = body.value;
            if (!step())
                return Completion::thrown();
        }
    }

    Completion execute_do_while(DoWhileStatement const& loop, Context& cx, std::span<JsString* const> labels)
    {
        Roots const roots(self);
        Value& last = self.root(Value::undefined());
        while (true) {
            Completion const body = execute(loop.body, cx, {});
            if (!loop_continues(body, labels))
                return finish_loop(body, last);
            if (!body.value.is_empty())
                last = body.value;
            std::optional<Value> const test = evaluate(loop.test, cx);
            if (!test)
                return Completion::thrown();
            if (!to_boolean(*test))
                return Completion::normal(last);
            if (!step())
                return Completion::thrown();
        }
    }

    // EnumerateObjectProperties (§14.7.5.9): the own string keys of each
    // object up the chain, snapshotted on arrival, each visited once and
    // only if still present and enumerable when its turn comes.
    struct Enumerator {
        Object* object = nullptr;
        std::vector<PropertyKey> keys;
        std::size_t next = 0;
        std::unordered_set<JsString*> visited;
    };

    void enumerator_load(Enumerator& enumerator)
    {
        enumerator.keys.clear();
        enumerator.next = 0;
        if (enumerator.object == nullptr)
            return;
        for (PropertyKey const& key : enumerator.object->own_keys()) {
            if (key.is_string())
                enumerator.keys.push_back(key);
        }
    }

    // The next key as a string, or null when the chain is exhausted.
    JsString* enumerator_next(Enumerator& enumerator)
    {
        while (enumerator.object != nullptr) {
            while (enumerator.next < enumerator.keys.size()) {
                PropertyKey const key = enumerator.keys[enumerator.next++];
                JsString* name = heap().key_to_string(key);
                if (enumerator.visited.contains(name))
                    continue;
                std::optional<PropertyDescriptor> const desc = enumerator.object->get_own_property(key);
                if (!desc)
                    continue;
                enumerator.visited.insert(name);
                if (desc->enumerable.value_or(false))
                    return name;
            }
            enumerator.object = enumerator.object->prototype();
            enumerator_load(enumerator);
        }
        return nullptr;
    }

    Completion execute_for_in(ForInStatement const& loop, Context& cx, std::span<JsString* const> labels)
    {
        // §14.7.5.6 ForIn/OfHeadEvaluation and §14.7.5.7 ForIn/OfBodyEvaluation.
        Environment* const saved = cx.lexical;
        bool const lexical = loop.declaration && loop.declaration->kind != VariableDeclaration::Kind::Var;
        JsString* const bound_name = loop.declaration ? loop.declaration->declarations[0].name : nullptr;
        auto restore = [&](Completion completion) {
            cx.lexical = saved;
            return completion;
        };
        // B.3.5: `for (var x = 1 in o)` assigns the initializer first.
        if (loop.declaration && loop.declaration->kind == VariableDeclaration::Kind::Var && loop.declaration->declarations[0].init) {
            Completion const init = execute_declaration(*loop.declaration, cx);
            if (init.is_abrupt())
                return init;
        }
        if (lexical) {
            // The head's expression sees the name in its dead zone.
            cx.lexical = new_environment(saved);
            cx.lexical->declare(bound_name, Value::undefined(), true, false);
        }
        std::optional<Value> const subject = evaluate(loop.object, cx);
        cx.lexical = saved;
        if (!subject)
            return Completion::thrown();
        if (subject->is_nullish())
            return Completion::normal();
        Roots const roots(self);
        self.root(*subject);
        std::optional<Object*> const object = self.to_object(*subject);
        if (!object)
            return Completion::thrown();
        self.root(Value::object(*object));
        Enumerator enumerator;
        enumerator.object = *object;
        enumerator_load(enumerator);
        Value& last = self.root(Value::undefined());
        while (true) {
            JsString* key = enumerator_next(enumerator);
            if (key == nullptr)
                return restore(Completion::normal(last));
            Value const key_value = Value::string(key);
            if (lexical) {
                cx.lexical = new_environment(saved);
                cx.lexical->declare(bound_name, key_value, loop.declaration->kind != VariableDeclaration::Kind::Const, true);
            } else if (loop.declaration) {
                Reference reference = resolve(bound_name, cx.lexical);
                if (!put_value(reference, key_value, cx))
                    return restore(Completion::thrown());
            } else {
                std::optional<Reference> reference = evaluate_reference(loop.target, cx);
                if (!reference)
                    return restore(Completion::thrown());
                self.root(reference->base);
                self.root(reference->key_value);
                if (!put_value(*reference, key_value, cx))
                    return restore(Completion::thrown());
            }
            Completion const body = execute(loop.body, cx, {});
            cx.lexical = saved;
            if (!loop_continues(body, labels))
                return restore(finish_loop(body, last));
            if (!body.value.is_empty())
                last = body.value;
            if (!step())
                return restore(Completion::thrown());
        }
    }

    Completion execute_try(TryStatement const& statement, Context& cx)
    {
        // §14.15.3. A termination (the interrupt) is not an exception a
        // script may observe: neither the catch nor the finally runs.
        Completion result = execute_block(*statement.block, cx);
        if (result.type == Completion::Type::Throw && statement.handler && !self.m_terminated) {
            Roots const roots(self);
            Value const thrown = self.take_exception();
            self.root(thrown);
            Environment* const saved = cx.lexical;
            if (statement.catch_parameter) {
                cx.lexical = new_environment(saved);
                cx.lexical->declare(statement.catch_parameter, thrown);
            }
            result = execute_block(*statement.handler, cx);
            cx.lexical = saved;
        }
        if (statement.finalizer && !self.m_terminated) {
            Roots const roots(self);
            Value pending;
            bool const was_throw = result.type == Completion::Type::Throw;
            if (was_throw) {
                pending = self.take_exception();
                self.root(pending);
            } else {
                self.root(result.value);
            }
            Completion const finalizer = execute_block(*statement.finalizer, cx);
            if (finalizer.is_abrupt())
                return finalizer.with_value_if_empty(Value::undefined());
            if (was_throw)
                self.throw_value(pending);
        }
        return result.with_value_if_empty(Value::undefined());
    }

    Completion execute_switch(SwitchStatement const& statement, Context& cx)
    {
        // §14.12.4 CaseBlockEvaluation: the clauses before the default are
        // tried in order, then the ones after it, and a match falls
        // through everything below it, the default included.
        std::optional<Value> const discriminant = evaluate(statement.discriminant, cx);
        if (!discriminant)
            return Completion::thrown();
        Roots const roots(self);
        self.root(*discriminant);
        Environment* const saved = cx.lexical;
        if (!statement.declarations.lexicals.empty() || !statement.declarations.functions.empty()) {
            cx.lexical = new_environment(saved);
            instantiate_block(statement.declarations, cx.lexical);
        }
        auto restore = [&](Completion completion) {
            cx.lexical = saved;
            return completion;
        };
        std::size_t const count = statement.cases.size();
        std::size_t default_index = count;
        for (std::size_t i = 0; i < count; ++i) {
            if (statement.cases[i].test == nullptr)
                default_index = i;
        }
        std::size_t start = count;
        auto matches = [&](std::size_t i, bool& matched) -> bool {
            std::optional<Value> const test = evaluate(statement.cases[i].test, cx);
            if (!test)
                return false;
            matched = Interpreter::strict_equals(*discriminant, *test);
            return true;
        };
        for (std::size_t i = 0; i < count && start == count; ++i) {
            if (i == default_index)
                continue;
            if (i > default_index && default_index != count) {
                // The clauses after the default are tested only once the
                // ones before it have all missed.
            }
            bool matched = false;
            if (!matches(i, matched))
                return restore(Completion::thrown());
            if (matched)
                start = i;
        }
        if (start == count)
            start = default_index;
        Value& last = self.root(Value::undefined());
        for (std::size_t i = start; i < count; ++i) {
            Completion const completion = execute_list(statement.cases[i].consequent, cx);
            if (!completion.value.is_empty())
                last = completion.value;
            if (completion.is_abrupt())
                return restore(finish_loop(completion, last));
        }
        return restore(Completion::normal(last));
    }

    Completion execute_with(WithStatement const& statement, Context& cx)
    {
        // §14.11.2: an object environment whose bindings are the object's
        // properties, marked so that calls through it get it as `this`.
        std::optional<Value> const value = evaluate(statement.object, cx);
        if (!value)
            return Completion::thrown();
        Roots const roots(self);
        self.root(*value);
        std::optional<Object*> const object = self.to_object(*value);
        if (!object)
            return Completion::thrown();
        Environment* const saved = cx.lexical;
        cx.lexical = new_environment(saved, *object);
        cx.lexical->set_with_environment(true);
        Completion const completion = execute(statement.body, cx, {});
        cx.lexical = saved;
        return completion.with_value_if_empty(Value::undefined());
    }

    // ---- tracing
    void trace(Tracer& tracer)
    {
        for (Context const& context : contexts) {
            tracer.visit(context.lexical);
            tracer.visit(context.variable);
            tracer.visit(context.function);
        }
        tracer.visit(global_lexical);
    }
};

// ------------------------------------------------------- Interpreter

Interpreter::Interpreter()
    : m_heap(std::make_unique<Heap>())
    , m_impl(std::make_unique<Impl>(*this))
{
    m_heap->add_root_provider(this);
    install_intrinsics(*this);
    m_impl->global_lexical = m_heap->allocate<Environment>(m_intrinsics.global_environment);
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
    Intrinsics const& i = m_intrinsics;
    tracer.visit(i.global);
    tracer.visit(i.global_environment);
    tracer.visit(i.object_prototype);
    tracer.visit(i.function_prototype);
    tracer.visit(i.array_prototype);
    tracer.visit(i.string_prototype);
    tracer.visit(i.number_prototype);
    tracer.visit(i.boolean_prototype);
    tracer.visit(i.symbol_prototype);
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
    tracer.visit(i.error_constructor);
    for (Function* constructor : i.error_constructors)
        tracer.visit(constructor);
    tracer.visit(i.date_constructor);
    tracer.visit(i.regexp_constructor);
    tracer.visit(i.eval);
    tracer.visit(i.throw_type_error);
    tracer.visit(i.math);
    tracer.visit(i.json);
    tracer.visit(i.symbol_registry);
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
    keep(std::move(program));
    Impl::ContextScope scope(*m_impl, Context { m_impl->global_lexical, m_intrinsics.global_environment, tree, nullptr, tree->is_strict });
    Context& cx = scope.context();
    if (!m_impl->global_declaration_instantiation(*tree, cx)) {
        outcome.ok = false;
        outcome.value = take_exception();
        return outcome;
    }
    Completion const completion = m_impl->execute_list(tree->body, cx);
    if (completion.type == Completion::Type::Throw) {
        outcome.ok = false;
        outcome.value = take_exception();
        return outcome;
    }
    outcome.ok = true;
    outcome.value = completion.value.is_empty() ? Value::undefined() : completion.value;
    return outcome;
}

Outcome Interpreter::run_script(std::string_view utf8_source, std::string name)
{
    return run_script(std::u16string_view(utf16_from_utf8(utf8_source)), std::move(name));
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
    if (!is_constructor(callee))
        return throw_type_error(describe(callee) + " is not a constructor");
    if (m_call_depth == 0)
        m_stack_base = stack_position();
    if (m_call_depth >= m_call_depth_limit)
        return throw_range_error("Maximum call stack size exceeded");
    Roots const roots(*this);
    root(callee);
    for (Value const& argument : arguments)
        root(argument);
    if (!m_impl->step() || !m_impl->stack_ok())
        return std::nullopt;
    ++m_call_depth;
    Object* constructor = callee.as_object();
    std::optional<Value> const result = static_cast<Function*>(constructor)->construct(*this, arguments, constructor);
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

std::optional<Value> Interpreter::eval_in(std::u16string_view source, Environment* scope, bool strict, Value this_value)
{
    return m_impl->perform_eval(source, scope ? scope : m_impl->global_lexical, strict, this_value, true);
}

std::optional<Value> Interpreter::compile_function(std::u16string_view parameters, std::u16string_view body, Environment* scope)
{
    // CreateDynamicFunction (§20.2.1.1.1): the parser assembles and checks
    // the wrapper; the function closes over the global environment.
    ParseError error;
    std::unique_ptr<Program> program = Parser::parse_function_constructor(*m_heap, parameters, body, &error);
    if (!program)
        return throw_syntax_error(error.message);
    auto const* statement = static_cast<ExpressionStatement const*>(program->body[0]);
    FunctionNode const& node = *static_cast<FunctionExpression const*>(statement->expression)->function;
    keep(std::move(program));
    return Value::object(new_script_function(node, scope ? scope : m_impl->global_lexical));
}

ScriptFunction* Interpreter::new_script_function(FunctionNode const& node, Environment* scope, Value)
{
    // OrdinaryFunctionCreate + MakeConstructor (§10.2.3, §10.2.5):
    // `length` then `name`, and for a constructor a fresh `prototype`
    // pointing back. Arrows take their `this` from the scope chain, so
    // the lexical-this parameter has nothing to record.
    Heap::NoCollect const guard(*m_heap);
    auto* function = m_heap->allocate<ScriptFunction>(m_intrinsics.function_prototype, node, scope, node.is_constructable);
    function->put(PropertyKey::atom(atoms().length), Value::number(static_cast<double>(node.parameters.size())), Configurable);
    function->put(PropertyKey::atom(atoms().name), Value::string(node.name ? node.name : atoms().empty), Configurable);
    if (node.is_constructable) {
        Object* prototype = new_object();
        prototype->put(PropertyKey::atom(atoms().constructor), Value::object(function), builtin_attributes);
        function->put(PropertyKey::atom(atoms().prototype), Value::object(prototype), Writable);
    }
    return function;
}

// ---------------------------------------------------- ScriptFunction

std::optional<Value> ScriptFunction::call(Interpreter& interpreter, Value const& this_value, std::span<Value const> arguments)
{
    return interpreter.m_impl->call_script_function(*this, this_value, arguments, nullptr);
}

std::optional<Value> ScriptFunction::construct(Interpreter& interpreter, std::span<Value const> arguments, Object* new_target)
{
    // [[Construct]] (§10.2.2): a fresh object from new.target's
    // prototype is `this`, and stays the result unless the body returns
    // an object of its own.
    if (!m_constructable)
        return interpreter.throw_type_error("not a constructor");
    Interpreter::Roots const roots(interpreter);
    interpreter.root(Value::object(this));
    if (new_target)
        interpreter.root(Value::object(new_target));
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
