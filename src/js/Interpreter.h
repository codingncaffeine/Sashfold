#pragma once

// The evaluator: a tree-walking interpreter over the Ast, one realm per
// Interpreter (its heap, its global object, its intrinsics). Everything
// that can run script and therefore throw returns std::optional — nullopt
// means a value is pending in `exception()` — so a throw travels up C++
// frames as a return value, never as a C++ exception.
//
// Rooting: any Value a C++ frame holds across a call that may allocate
// must be rooted — push it with root() inside a Roots scope, which pops
// everything pushed since it opened when it closes. Natives that build a
// few cells at once can take a Heap::NoCollect instead. The tests run the
// heap in stress mode, where a missing root is a failure the first time.

#include "js/Ast.h"
#include "js/Heap.h"
#include "js/Object.h"
#include "js/Value.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::js {

class Parser;

enum class ErrorType : std::uint8_t {
    Error,
    EvalError,
    RangeError,
    ReferenceError,
    SyntaxError,
    TypeError,
    UriError,
};

// The prototypes and constructors a realm is born with (§9.3).
struct Intrinsics {
    Object* global = nullptr;
    Environment* global_environment = nullptr;

    Object* object_prototype = nullptr;
    Object* function_prototype = nullptr;
    Object* array_prototype = nullptr;
    Object* string_prototype = nullptr;
    Object* number_prototype = nullptr;
    Object* boolean_prototype = nullptr;
    Object* symbol_prototype = nullptr;
    Object* error_prototype = nullptr;
    Object* error_prototypes[7] = {}; // by ErrorType
    Object* date_prototype = nullptr;
    Object* regexp_prototype = nullptr;
    Object* arguments_prototype = nullptr; // Object.prototype; kept for clarity

    Function* object_constructor = nullptr;
    Function* function_constructor = nullptr;
    Function* array_constructor = nullptr;
    Function* string_constructor = nullptr;
    Function* number_constructor = nullptr;
    Function* boolean_constructor = nullptr;
    Function* symbol_constructor = nullptr;
    Function* error_constructor = nullptr;
    Function* error_constructors[7] = {};
    Function* date_constructor = nullptr;
    Function* regexp_constructor = nullptr;
    Function* eval = nullptr;
    Function* throw_type_error = nullptr; // %ThrowTypeError% (§10.2.4.1)
    Object* math = nullptr;
    Object* json = nullptr;
};

// The outcome of running a script or calling into it from the outside.
struct Outcome {
    bool ok = true;
    Value value; // the completion value, or the thrown value
};

enum class PreferredType : std::uint8_t { Default, Number, String };

class Interpreter : public RootProvider {
public:
    Interpreter();
    ~Interpreter() override;
    Interpreter(Interpreter const&) = delete;
    Interpreter& operator=(Interpreter const&) = delete;

    Heap& heap() { return *m_heap; }
    Intrinsics const& intrinsics() const { return m_intrinsics; }
    Intrinsics& intrinsics() { return m_intrinsics; }
    Object* global() const { return m_intrinsics.global; }
    WellKnownAtoms const& atoms() const { return m_heap->atoms(); }

    // Runs a script as global code (§16.1.6). A parse error is a thrown
    // SyntaxError. `name` is for messages: a URL, "<inline>", a test path.
    Outcome run_script(std::u16string_view source, std::string name = "");
    Outcome run_script(std::string_view utf8_source, std::string name = "");

    // Calling into script from C++ (bindings, the event loop).
    std::optional<Value> call(Value const& callee, Value const& this_value, std::span<Value const> arguments);
    std::optional<Value> construct(Value const& callee, std::span<Value const> arguments);
    // Wraps call(): the thrown value becomes an Outcome instead of a
    // pending exception.
    Outcome call_outcome(Value const& callee, Value const& this_value, std::span<Value const> arguments);

    // Throwing. Each returns nullopt so a native can `return throw_…(…)`.
    std::nullopt_t throw_value(Value);
    std::nullopt_t throw_error(ErrorType, std::string_view message);
    std::nullopt_t throw_type_error(std::string_view message) { return throw_error(ErrorType::TypeError, message); }
    std::nullopt_t throw_range_error(std::string_view message) { return throw_error(ErrorType::RangeError, message); }
    std::nullopt_t throw_reference_error(std::string_view message) { return throw_error(ErrorType::ReferenceError, message); }
    std::nullopt_t throw_syntax_error(std::string_view message) { return throw_error(ErrorType::SyntaxError, message); }
    bool has_exception() const { return m_has_exception; }
    Value const& exception() const { return m_exception; }
    Value take_exception();
    void clear_exception();

    // Abstract operations (§7.1–§7.3). The ones that cannot throw are static.
    static bool to_boolean(Value const&);
    std::optional<Value> to_primitive(Value const&, PreferredType = PreferredType::Default);
    std::optional<double> to_number(Value const&);
    std::optional<double> to_integer_or_infinity(Value const&);
    std::optional<std::int32_t> to_int32(Value const&);
    std::optional<std::uint32_t> to_uint32(Value const&);
    std::optional<double> to_length(Value const&); // 0 … 2^53 − 1
    std::optional<double> to_index(Value const&); // RangeError past 2^53 − 1
    std::optional<JsString*> to_string(Value const&);
    std::optional<Object*> to_object(Value const&);
    std::optional<PropertyKey> to_property_key(Value const&);
    static double to_number(double d) { return d; }
    static std::int32_t double_to_int32(double);
    static std::uint32_t double_to_uint32(double);
    static double to_integer_or_infinity(double);

    // Property access on any value (GetV boxes primitives through their
    // prototypes; a nullish base is a TypeError).
    std::optional<Value> get(Value const& base, PropertyKey const&);
    std::optional<Value> get(Value const& base, std::string_view name); // name interned on the way
    std::optional<Value> get(Object&, PropertyKey const&);
    // Set; in strict code a failure is a TypeError, otherwise silent.
    std::optional<bool> set(Value const& base, PropertyKey const&, Value const&, bool strict);
    std::optional<bool> set(Object&, PropertyKey const&, Value const&, bool strict);
    // CreateDataProperty(OrThrow) and DefinePropertyOrThrow.
    std::optional<bool> create_data_property(Object&, PropertyKey const&, Value const&, bool or_throw = true);
    std::optional<bool> define_property_or_throw(Object&, PropertyKey const&, PropertyDescriptor const&);
    std::optional<bool> delete_property_or_throw(Object&, PropertyKey const&);
    std::optional<bool> has_property(Value const& base, PropertyKey const&);
    std::optional<Value> get_method(Value const& base, PropertyKey const&); // undefined when absent; TypeError when not callable
    std::optional<Function*> get_function(Value const& base, PropertyKey const&); // nullptr when absent

    std::optional<Value> invoke(Value const& base, PropertyKey const&, std::span<Value const> arguments);
    std::optional<Value> ordinary_to_primitive(Object&, PreferredType);
    std::optional<bool> instance_of(Value const&, Value const& target);
    std::optional<bool> ordinary_has_instance(Value const& constructor, Value const&);
    std::optional<Value> species_constructor(Object&, Function* default_constructor);
    std::optional<Object*> get_prototype_from_constructor(Object* new_target, Object* default_prototype);

    // Equality (§7.2.13–§7.2.16).
    static bool strict_equals(Value const&, Value const&);
    static bool same_value(Value const&, Value const&);
    static bool same_value_zero(Value const&, Value const&);
    std::optional<bool> loose_equals(Value const&, Value const&);
    // Abstract relational comparison; nullopt-inner = undefined (NaN).
    std::optional<std::optional<bool>> less_than(Value const& left, Value const& right, bool left_first);

    static bool is_callable(Value const& v) { return v.is_object() && v.as_object()->is_callable(); }
    static bool is_constructor(Value const& v) { return v.is_object() && v.as_object()->is_constructor(); }
    static bool is_array(Value const& v) { return v.is_object() && v.as_object()->is_array(); }
    std::optional<bool> is_regexp(Value const&);
    std::optional<double> length_of_array_like(Object&);
    std::optional<std::vector<Value>> create_list_from_array_like(Value const&);
    // The typeof string (§13.5.3).
    JsString* type_of(Value const&);

    // Making things.
    Object* new_object(Object* prototype = nullptr); // null = Object.prototype
    ArrayObject* new_array(std::span<Value const> elements = {});
    JsString* string(std::u16string_view s) { return m_heap->string(s); }
    JsString* string(std::string_view utf8) { return m_heap->string(utf8); }
    JsString* atom(std::string_view utf8) { return m_heap->atom(utf8); }
    PropertyKey key(std::string_view utf8) { return m_heap->key(utf8); }
    PropertyKey key(std::u16string_view s) { return m_heap->key(s); }
    // A native function with `name` and `length` set as §10.3.3 has them.
    NativeFunction* new_native(std::string_view name, int length, NativeFunction::Callback,
        NativeFunction::ConstructCallback = {});
    Object* new_error(ErrorType, std::string_view message);
    Object* new_error(ErrorType, JsString* message);
    // ScriptFunction from an AST node, closed over `scope` (§10.2.3 + MakeConstructor).
    ScriptFunction* new_script_function(FunctionNode const&, Environment* scope, Value lexical_this = Value::empty());

    // Rooting.
    class Roots {
    public:
        explicit Roots(Interpreter& interpreter)
            : m_interpreter(interpreter)
            , m_mark(interpreter.m_roots.size())
        {
        }
        ~Roots() { m_interpreter.m_roots.resize(m_mark); }
        Roots(Roots const&) = delete;
        Roots& operator=(Roots const&) = delete;

    private:
        Interpreter& m_interpreter;
        std::size_t m_mark;
    };
    // Keeps `value` alive until the enclosing Roots closes. Returns a
    // reference into the root stack, stable until that scope closes, so
    // a native can update it in place.
    Value& root(Value value)
    {
        m_roots.push_back(value);
        return m_roots.back();
    }
    void trace_roots(Tracer&) override;

    // Direct eval (§19.2.1.1) from the evaluator; `eval` the function is
    // the indirect form. Exposed for the bindings' inline event handlers.
    std::optional<Value> eval_in(std::u16string_view source, Environment* scope, bool strict, Value this_value);
    // Compiles a function from parameter and body texts (`new Function`,
    // and an `onclick="…"` attribute).
    std::optional<Value> compile_function(std::u16string_view parameters, std::u16string_view body,
        Environment* scope = nullptr);

    // Limits and instrumentation.
    void set_call_depth_limit(int depth) { m_call_depth_limit = depth; }
    int call_depth() const { return m_call_depth; }
    // Stopping a runaway script. The evaluator counts steps (a statement,
    // a loop iteration, a call) and every `interval` of them asks
    // should_stop; a yes ends the script with an uncatchable termination
    // — no catch or finally runs — which run_script reports as a thrown
    // RangeError "script terminated" and terminated() remembers. The
    // shell's slow-script stop and the test runners' deadlines use it.
    void set_interrupt(std::function<bool()> should_stop, std::uint32_t interval = 10000)
    {
        m_should_stop = std::move(should_stop);
        m_interrupt_interval = interval;
    }
    bool terminated() const { return m_terminated; }
    void clear_termination() { m_terminated = false; }
    std::uint64_t steps() const { return m_steps; }
    // Every console.* call and every uncaught error ends up here.
    std::function<void(std::string_view level, std::string_view message)> on_console;
    // A description of a thrown value: "TypeError: x is not a function".
    std::string describe(Value const&);
    // The realm keeps every program it ran: functions point into them.
    void keep(std::unique_ptr<Program>);

private:
    struct Impl;
    friend struct Impl;
    friend class ScriptFunction;
    std::unique_ptr<Heap> m_heap;
    std::unique_ptr<Impl> m_impl;
    Intrinsics m_intrinsics;
    std::vector<Value> m_roots;
    std::vector<std::unique_ptr<Program>> m_programs;
    Value m_exception;
    bool m_has_exception = false;
    int m_call_depth = 0;
    int m_call_depth_limit = 400;
    std::function<bool()> m_should_stop;
    std::uint32_t m_interrupt_interval = 10000;
    std::uint64_t m_steps = 0;
    bool m_terminated = false;
};

}
