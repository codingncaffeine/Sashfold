#pragma once

// The evaluator's core: the completion and reference records, the running
// context, and the declaration of Interpreter::Impl — the mechanisms every
// evaluator in this directory shares (name resolution, get/put through a
// reference, function and class instantiation, patterns, declaration
// instantiation, eval). The definitions live in Interpreter.cpp. Internal
// to the engine: nothing outside src/js includes this.

#include "js/Ast.h"
#include "js/Heap.h"
#include "js/Interpreter.h"
#include "js/Object.h"
#include "js/Value.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sashfold::js {

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
    PrivateEnvironment* private_environment = nullptr; // the class bodies' Private Names in scope (§9.2)
};

// A Reference Record (§6.2.5) in the shapes the evaluator produces: a
// binding in a declarative environment, a property of an object
// environment (the global object or a `with` target), a property of a
// value, an unresolvable name, or no reference at all — a plain value,
// which `delete` and `typeof` accept.
struct Reference {
    // Super: a property of the home object's prototype (the base), read
    // and written with `this` (this_value) as the receiver (§13.3.7.3).
    // Private: `o.#x` — the base and the Private Name as the key (§13.3.3),
    // read and written on the base alone, with `name` for the messages.
    enum class Kind : std::uint8_t { Unresolvable, Binding, ObjectEnvironment, Property, Super, Private, Value };
    Kind kind = Kind::Value;
    Environment* environment = nullptr;
    JsString* name = nullptr;
    Value base; // Property, Super: the base; Value: the value itself
    PropertyKey key; // Property, Super (once ready) and ObjectEnvironment
    Value key_value; // Property, Super: the key before ToPropertyKey, when not yet converted
    Value this_value; // Super: the receiver
    bool key_ready = false;
};

inline bool is_anonymous_function_definition(Expression const* expression)
{
    // IsAnonymousFunctionDefinition (§8.4.3) for the forms this grammar
    // has: an arrow, a function expression or a class expression without
    // a name.
    if (expression->type == NodeType::ArrowFunction)
        return true;
    if (expression->type == NodeType::FunctionExpression)
        return static_cast<FunctionExpression const*>(expression)->function->name == nullptr;
    if (expression->type == NodeType::ClassExpression)
        return static_cast<ClassExpression const*>(expression)->node->name == nullptr;
    return false;
}

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
    // The realm's [[TemplateMap]] (§9.3.1): one template object per
    // tagged-template site, the same one every time the site runs.
    std::unordered_map<TemplateLiteral const*, Object*> template_objects;

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
    bool stack_ok();
    bool step();

    // ---- messages
    std::string expression_text(Expression const* expression, Context const& cx);

    // ---- environments
    Environment* new_environment(Environment* outer, Object* object = nullptr);
    std::optional<Value> resolve_this(Environment* environment);
    Environment* this_environment(Environment* environment);
    Reference resolve(JsString* name, Environment* environment);
    bool ensure_key(Reference& reference);
    std::string reference_key_text(Reference const& reference);
    std::optional<Value> get_value(Reference& reference, Context const& cx);
    bool put_value(Reference& reference, Value const& value, Context const& cx);
    Value this_for_call(Reference const& reference);

    // ---- functions
    void set_function_name(Object& function, PropertyKey const& key, std::string_view prefix = {});
    std::optional<Value> make_closure(FunctionNode const& node, Context& cx, PropertyKey const* name_key);
    std::optional<Value> evaluate_named(Expression const* expression, Context& cx, PropertyKey const& name_key);
    Object* make_arguments_object(ScriptFunction& function, Environment* environment, std::span<Value const> arguments, bool mapped);
    std::optional<Value> call_script_function(ScriptFunction& function, Value const& this_argument,
        std::span<Value const> arguments, Object* new_target, PropertyKey const* field_key = nullptr);

    // ---- classes
    Object* home_object_of(Context const& cx);
    std::optional<Reference> evaluate_super_member(SuperMember const& member, Context& cx);
    std::optional<Value> evaluate_super_call(SuperCall const& call, Context& cx);
    Value evaluate_new_target(Context& cx);
    std::optional<Value> call_field_initializer(ScriptFunction& initializer, Value const& this_value, PropertyKey const& key);

    // ---- private names (§7.3.30–§7.3.33)
    std::string private_name_text(PropertyKey const& key);
    std::optional<Value> private_get(Value const& base, PropertyKey const& key);
    bool private_set(Value const& base, PropertyKey const& key, Value const& value);
    bool private_field_add(Object& object, PropertyKey const& key, Value const& value);
    bool private_method_add(Object& object, PrivateMethod const& method);
    std::optional<Value> evaluate_private_in(PrivateInExpression const& expression, Context& cx);
    bool define_field(Object& receiver, ClassField const& field);
    bool initialize_instance_elements(Object& instance, ScriptFunction& constructor);
    ScriptFunction* new_class_constructor(FunctionNode const& node, Environment* scope, Object* proto, Object* constructor_parent);
    std::optional<Value> evaluate_class(ClassNode const& node, Context& cx, PropertyKey const* name_key);
    Completion execute_class_declaration(ClassDeclaration const& declaration, Context& cx);

    // ---- patterns

    // How a pattern's leaves take their values: InitializeReferencedBinding
    // of a name declared in `env` (let/const, parameters, catch, a for
    // head's binding); PutValue through a resolved name (`var`); or
    // PutValue through any reference an assignment pattern names.
    enum class BindMode : std::uint8_t { Initialize, VarAssign, Assign };
    static void collect_bound_names(Expression const* target, std::vector<JsString*>& out);
    static void collect_bound_names(JsString* name, Expression const* pattern, std::vector<JsString*>& out);
    static void collect_parameter_names(FunctionNode const& node, std::vector<JsString*>& out);
    bool initialize_binding(JsString* name, Value const& value, Environment* env);
    bool prepare_target(Expression const* target, BindMode mode, Context& cx, std::optional<Reference>& reference);
    bool bind_element(Expression const* target, Expression const* initializer, Value value,
        std::optional<Reference>& reference, BindMode mode, Environment* env, Context& cx);
    bool bind_declared(JsString* name, Expression const* pattern, Expression const* initializer, Value const& value,
        BindMode mode, Environment* env, Context& cx);
    bool bind_pattern(Expression const* pattern, Value const& value, BindMode mode, Environment* env, Context& cx);
    bool bind_array_pattern(ArrayPattern const& pattern, Value const& value, BindMode mode, Environment* env, Context& cx);
    bool bind_object_pattern(ObjectPattern const& pattern, Value const& value, BindMode mode, Environment* env, Context& cx);
    bool bind_parameters(FunctionNode const& node, Environment* env, std::span<Value const> arguments, Context& cx);
    bool copy_data_properties(Object& target, Value const& source, std::span<PropertyKey const> excluded);

    // ---- declaration instantiation
    bool global_declaration_instantiation(Program const& program, Context& cx);
    bool create_global_function_binding(JsString* name, Value const& value, bool deletable);
    bool create_global_var_binding(JsString* name, bool deletable);
    void instantiate_block(Declarations const& declarations, Environment* environment, PrivateEnvironment* private_environment);
    bool eval_declaration_instantiation(Program const& program, Environment* variable, Environment* lexical, bool strict,
        PrivateEnvironment* private_environment);
    Environment* variable_environment_of(Environment* environment);
    std::optional<Value> perform_eval(std::u16string_view source, Environment* scope, bool strict_caller, Value this_value, bool direct,
        PrivateEnvironment* private_environment);

    // ---- expressions
    std::optional<Value> evaluate(Expression const* expression, Context& cx);

    static bool to_boolean(Value const& value) { return Interpreter::to_boolean(value); }
    std::optional<Value> evaluate_regexp(RegExpLiteral const& literal);
    std::optional<Value> evaluate_template(TemplateLiteral const& literal, Context& cx);
    std::optional<Object*> template_object(TemplateLiteral const& literal);
    std::optional<Value> evaluate_tagged_template(TaggedTemplate const& tagged, Context& cx);
    std::optional<Value> evaluate_array(ArrayLiteral const& literal, Context& cx);
    std::optional<Value> evaluate_object(ObjectLiteral const& literal, Context& cx);
    std::optional<Reference> evaluate_member_reference(MemberExpression const& member, Context& cx, bool& short_circuit);
    std::optional<Value> evaluate_chain(Expression const* expression, Context& cx, bool& short_circuit, Reference* out_reference);
    std::optional<Value> evaluate_arguments(std::vector<Expression*> const& expressions, Context& cx, std::vector<Value>& values);
    std::optional<Value> evaluate_call(CallExpression const& call, Context& cx, bool& short_circuit);
    std::optional<Value> evaluate_new(NewExpression const& expression, Context& cx);
    std::optional<Reference> evaluate_reference(Expression const* expression, Context& cx);
    std::optional<Value> evaluate_unary(UnaryExpression const& unary, Context& cx);
    std::optional<Value> evaluate_delete(UnaryExpression const& unary, Context& cx);
    std::optional<Value> evaluate_update(UpdateExpression const& update, Context& cx);
    std::optional<Value> apply_binary(BinaryOp op, Value const& left, Value const& right);
    std::optional<Value> evaluate_binary(BinaryExpression const& binary, Context& cx);
    std::optional<Value> evaluate_logical(LogicalExpression const& logical, Context& cx);
    static std::optional<BinaryOp> binary_for(AssignmentOp op);
    std::optional<Value> evaluate_assignment(AssignmentExpression const& assignment, Context& cx);

    // ---- statements
    static bool loop_continues(Completion const& completion, std::span<JsString* const> labels);
    static Completion finish_loop(Completion const& completion, Value const& last);
    Completion execute_list(std::span<Statement* const> statements, Context& cx);
    Completion execute(Statement const* statement, Context& cx, std::span<JsString* const> labels);
    Completion execute_declaration(VariableDeclaration const& declaration, Context& cx);
    Completion execute_function_declaration(FunctionDeclaration const& declaration, Context& cx);
    Completion execute_block(BlockStatement const& block, Context& cx);
    void copy_iteration_environment(Context& cx, std::vector<JsString*> const& names);
    Completion execute_for(ForStatement const& loop, Context& cx, std::span<JsString* const> labels);
    Completion execute_while(WhileStatement const& loop, Context& cx, std::span<JsString* const> labels);
    Completion execute_do_while(DoWhileStatement const& loop, Context& cx, std::span<JsString* const> labels);

    // EnumerateObjectProperties (§14.7.5.9): the own string keys of each
    // object up the chain, snapshotted on arrival, each visited once and
    // only if still present and enumerable when its turn comes.
    struct Enumerator {
        Object* object = nullptr;
        std::vector<PropertyKey> keys;
        std::size_t next = 0;
        std::unordered_set<JsString*> visited;
    };
    void enumerator_load(Enumerator& enumerator);
    JsString* enumerator_next(Enumerator& enumerator);
    bool bind_loop_head(VariableDeclaration const* declaration, Expression const* target, std::vector<JsString*> const& names,
        Value const& value, Environment* saved, Context& cx);
    Completion execute_for_in(ForInStatement const& loop, Context& cx, std::span<JsString* const> labels);
    Completion execute_for_of(ForOfStatement const& loop, Context& cx, std::span<JsString* const> labels);
    Completion execute_try(TryStatement const& statement, Context& cx);
    Completion execute_switch(SwitchStatement const& statement, Context& cx);
    Completion execute_with(WithStatement const& statement, Context& cx);

    // ---- tracing
    void trace(Tracer& tracer);
};

}
