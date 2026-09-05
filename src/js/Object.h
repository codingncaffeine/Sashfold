#pragma once

// The object model (§10): ordinary objects with insertion-ordered
// properties, and the exotic ones the language cannot do without — arrays,
// functions (script, native, bound), string wrappers, arguments — plus the
// scope records closures capture. The essential internal methods are
// virtual on Object so an exotic object overrides exactly the ones the
// specification says it does; the ones that can run script take the
// interpreter and report a throw as nullopt.

#include "js/Heap.h"
#include "js/Regex.h"
#include "js/Value.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::js {

class Interpreter;
class Environment;
class Function;
class Program;
struct FunctionNode;

enum Attribute : std::uint8_t {
    Writable = 1,
    Enumerable = 2,
    Configurable = 4,
};
// What assignment and object literals create.
inline constexpr std::uint8_t default_attributes = Writable | Enumerable | Configurable;
// What the built-in library's methods carry (§17: not enumerable).
inline constexpr std::uint8_t builtin_attributes = Writable | Configurable;
inline constexpr std::uint8_t frozen_attributes = 0;

// A property descriptor (§6.2.6); each field absent when not specified.
struct PropertyDescriptor {
    std::optional<Value> value;
    std::optional<Object*> get; // nullptr inside the optional = `undefined`
    std::optional<Object*> set;
    std::optional<bool> writable;
    std::optional<bool> enumerable;
    std::optional<bool> configurable;

    bool is_accessor() const { return get.has_value() || set.has_value(); }
    bool is_data() const { return value.has_value() || writable.has_value(); }
    bool is_generic() const { return !is_accessor() && !is_data(); }

    static PropertyDescriptor data(Value value, std::uint8_t attributes)
    {
        PropertyDescriptor d;
        d.value = value;
        d.writable = (attributes & Writable) != 0;
        d.enumerable = (attributes & Enumerable) != 0;
        d.configurable = (attributes & Configurable) != 0;
        return d;
    }
    static PropertyDescriptor accessor(Object* getter, Object* setter, std::uint8_t attributes)
    {
        PropertyDescriptor d;
        d.get = getter;
        d.set = setter;
        d.enumerable = (attributes & Enumerable) != 0;
        d.configurable = (attributes & Configurable) != 0;
        return d;
    }
};

struct Property {
    PropertyKey key;
    Value value; // data property
    Object* getter = nullptr; // accessor property
    Object* setter = nullptr;
    std::uint8_t attributes = default_attributes;
    bool accessor = false;

    bool writable() const { return (attributes & Writable) != 0; }
    bool enumerable() const { return (attributes & Enumerable) != 0; }
    bool configurable() const { return (attributes & Configurable) != 0; }
};

class Object : public Cell {
public:
    enum class Class : std::uint8_t {
        Object,
        Array,
        Function,
        BoundFunction,
        Error,
        Boolean,
        Number,
        String,
        Symbol,
        Date,
        RegExp,
        Arguments,
        ArrayIterator, // %ArrayIteratorPrototype%'s instances (§23.1.5)
        StringIterator, // %StringIteratorPrototype%'s instances (§22.1.5)
        Math,
        Json,
        Global,
        Host, // a DOM wrapper or another object the bindings own the meaning of
    };

    explicit Object(Object* prototype, Class class_id = Class::Object)
        : m_prototype(prototype)
        , m_class(class_id)
    {
    }

    Class class_id() const { return m_class; }
    Object* prototype() const { return m_prototype; }
    // [[SetPrototypeOf]]: false when the object is not extensible or the
    // new chain would loop.
    bool set_prototype(Object*);
    bool is_extensible() const { return m_extensible; }
    void prevent_extensions() { m_extensible = false; }

    virtual bool is_callable() const { return false; }
    virtual bool is_constructor() const { return false; }
    bool is_array() const { return m_class == Class::Array; }
    bool is_error() const { return m_class == Class::Error; }
    bool is_host() const { return m_class == Class::Host; }

    // The essential internal methods (§10.1), ordinary here.
    virtual std::optional<PropertyDescriptor> get_own_property(PropertyKey const&) const;
    // ValidateAndApplyPropertyDescriptor; false = rejected. The caller
    // decides whether a rejection throws.
    virtual bool define_own_property(PropertyKey const&, PropertyDescriptor const&);
    virtual bool has_property(PropertyKey const&) const; // own or inherited
    virtual std::optional<Value> get(Interpreter&, PropertyKey const&, Value const& receiver);
    // false = could not be set (read-only, or a missing setter).
    virtual std::optional<bool> set(Interpreter&, PropertyKey const&, Value const&, Value const& receiver);
    virtual bool delete_property(PropertyKey const&); // false = non-configurable
    // OrdinaryOwnPropertyKeys order: indices ascending, then strings and
    // then symbols each in creation order.
    virtual std::vector<PropertyKey> own_keys() const;

    // Shortcuts that never run script.
    Property const* find_own(PropertyKey const&) const;
    Property* find_own(PropertyKey const&);
    // Define or overwrite a data property outright, no validation: how
    // intrinsics are built and how a fresh object is filled.
    void put(PropertyKey const&, Value const&, std::uint8_t attributes = default_attributes);
    void put_accessor(PropertyKey const&, Object* getter, Object* setter, std::uint8_t attributes = Configurable);
    bool remove_own(PropertyKey const&); // unconditional erase
    std::size_t own_property_count() const { return m_properties.size(); }
    // The storage in creation order, for callers that iterate everything
    // (JSON, for-in, the devtools).
    std::vector<Property> const& properties() const { return m_properties; }

    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override { return sizeof(*this) + m_properties.size() * sizeof(Property); }

    // Bindings park the C++ side of a host object here; untraced, unowned.
    void* host_data = nullptr;

protected:
    Property& insert(PropertyKey const&);
    void erase_at(std::size_t index);
    void rebuild_index();

    std::vector<Property> m_properties;
    // Built once the object has more properties than a linear scan likes.
    std::unordered_map<PropertyKey, std::size_t, PropertyKeyHash> m_index;
    Object* m_prototype;
    Class m_class;
    bool m_extensible = true;
};

// An Array exotic object (§10.4.2). Indices below dense_size() live in a
// vector, holes as Value::empty(); anything sparse beyond it is an ordinary
// property. `length` is virtual: it is answered from m_length and never
// stored as a Property.
class ArrayObject : public Object {
public:
    explicit ArrayObject(Object* prototype, std::span<Value const> elements = {});

    std::uint32_t length() const { return m_length; }
    // ArraySetLength; false when a non-configurable element blocks the
    // truncation (the caller throws in strict code).
    bool set_length(std::uint32_t);
    // Fast element access. element() is empty for a hole or an index past
    // the dense storage that has no ordinary property either.
    Value element(std::uint32_t) const;
    bool has_element(std::uint32_t) const;
    void set_element(std::uint32_t, Value const&); // grows dense storage when the index is near
    void push(Value const&);
    std::uint32_t dense_size() const { return static_cast<std::uint32_t>(m_elements.size()); }
    std::vector<Value>& dense() { return m_elements; }
    std::vector<Value> const& dense() const { return m_elements; }
    // True when every index below length() is a plain, writable, dense
    // element and the prototype chain has no indexed properties: the
    // fast paths' precondition.
    bool is_simple_dense() const;

    std::optional<PropertyDescriptor> get_own_property(PropertyKey const&) const override;
    bool define_own_property(PropertyKey const&, PropertyDescriptor const&) override;
    bool has_property(PropertyKey const&) const override;
    std::optional<Value> get(Interpreter&, PropertyKey const&, Value const& receiver) override;
    std::optional<bool> set(Interpreter&, PropertyKey const&, Value const&, Value const& receiver) override;
    bool delete_property(PropertyKey const&) override;
    std::vector<PropertyKey> own_keys() const override;

    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override
    {
        return Object::size_in_bytes() + m_elements.size() * sizeof(Value);
    }

private:
    std::vector<Value> m_elements;
    std::uint32_t m_length = 0;
    bool m_length_writable = true;
};

class Function : public Object {
public:
    explicit Function(Object* prototype, Class class_id = Class::Function)
        : Object(prototype, class_id)
    {
    }
    bool is_callable() const override { return true; }
    virtual std::optional<Value> call(Interpreter&, Value const& this_value, std::span<Value const> arguments) = 0;
    // [[Construct]]; only when is_constructor(). new_target is the
    // constructor `new` was applied to.
    virtual std::optional<Value> construct(Interpreter&, std::span<Value const> arguments, Object* new_target);
};

class ScriptFunction;

// A class field a constructor defines on each instance (§15.7.10
// ClassFieldDefinition): the key, and the initializer as a function
// called with the instance as `this`, or none.
struct ClassField {
    PropertyKey key;
    ScriptFunction* initializer = nullptr;
};

// A function written in script: the AST plus the scope it closed over.
// Its `call` and `construct` are defined beside the evaluator
// (Interpreter.cpp). A method carries its home object for `super`; a
// class constructor carries the fields its instances get.
class ScriptFunction : public Function {
public:
    ScriptFunction(Object* prototype, FunctionNode const& node, Environment* scope, bool constructable);

    FunctionNode const& node() const { return *m_node; }
    Environment* scope() const { return m_scope; }
    bool is_arrow() const;
    bool is_strict() const;
    bool is_constructor() const override { return m_constructable; }
    Object* home_object() const { return m_home_object; }
    void set_home_object(Object* home) { m_home_object = home; }
    std::vector<ClassField>& fields() { return m_fields; }
    std::vector<ClassField> const& fields() const { return m_fields; }

    std::optional<Value> call(Interpreter&, Value const& this_value, std::span<Value const> arguments) override;
    std::optional<Value> construct(Interpreter&, std::span<Value const> arguments, Object* new_target) override;

    void trace(Tracer&) override;

private:
    FunctionNode const* m_node;
    Environment* m_scope;
    Object* m_home_object = nullptr;
    std::vector<ClassField> m_fields;
    bool m_constructable;
};

// A function written in C++: the built-in library and the DOM bindings.
class NativeFunction : public Function {
public:
    using Callback = std::function<std::optional<Value>(Interpreter&, Value const& this_value, std::span<Value const> arguments)>;
    using ConstructCallback = std::function<std::optional<Value>(Interpreter&, std::span<Value const> arguments, Object* new_target)>;

    NativeFunction(Object* prototype, Callback call, ConstructCallback construct = {})
        : Function(prototype)
        , m_call(std::move(call))
        , m_construct(std::move(construct))
    {
    }

    bool is_constructor() const override { return static_cast<bool>(m_construct); }
    std::optional<Value> call(Interpreter&, Value const& this_value, std::span<Value const> arguments) override;
    std::optional<Value> construct(Interpreter&, std::span<Value const> arguments, Object* new_target) override;

private:
    Callback m_call;
    ConstructCallback m_construct;
};

// Function.prototype.bind's result (§10.4.1).
class BoundFunction : public Function {
public:
    BoundFunction(Object* prototype, Function* target, Value bound_this, std::vector<Value> bound_arguments)
        : Function(prototype, Class::BoundFunction)
        , m_target(target)
        , m_bound_this(bound_this)
        , m_bound_arguments(std::move(bound_arguments))
    {
    }

    Function* target() const { return m_target; }
    bool is_constructor() const override { return m_target->is_constructor(); }
    std::optional<Value> call(Interpreter&, Value const& this_value, std::span<Value const> arguments) override;
    std::optional<Value> construct(Interpreter&, std::span<Value const> arguments, Object* new_target) override;
    void trace(Tracer&) override;

private:
    Function* m_target;
    Value m_bound_this;
    std::vector<Value> m_bound_arguments;
};

// A Boolean, Number, String or Symbol wrapper (§10.4.3 for String).
class PrimitiveObject : public Object {
public:
    PrimitiveObject(Object* prototype, Class class_id, Value primitive)
        : Object(prototype, class_id)
        , m_primitive(primitive)
    {
    }
    Value primitive() const { return m_primitive; }
    void trace(Tracer&) override;

private:
    Value m_primitive;
};

// String exotic object: its code units are read-only indexed properties
// and `length` is one too.
class StringObject : public PrimitiveObject {
public:
    StringObject(Object* prototype, JsString* value)
        : PrimitiveObject(prototype, Class::String, Value::string(value))
    {
    }
    JsString* string() const { return primitive().as_string(); }

    std::optional<PropertyDescriptor> get_own_property(PropertyKey const&) const override;
    bool define_own_property(PropertyKey const&, PropertyDescriptor const&) override;
    bool has_property(PropertyKey const&) const override;
    bool delete_property(PropertyKey const&) override;
    std::vector<PropertyKey> own_keys() const override;
};

class ErrorObject : public Object {
public:
    explicit ErrorObject(Object* prototype)
        : Object(prototype, Class::Error)
    {
    }
    // The stack trace text captured at construction, read through the
    // Error.prototype.stack accessor; null until it is captured.
    JsString* stack() const { return m_stack; }
    void set_stack(JsString* stack) { m_stack = stack; }
    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_stack);
    }

private:
    JsString* m_stack = nullptr;
};

class DateObject : public Object {
public:
    DateObject(Object* prototype, double time_value)
        : Object(prototype, Class::Date)
        , m_time_value(time_value)
    {
    }
    double time_value() const { return m_time_value; }
    void set_time_value(double t) { m_time_value = t; }

private:
    double m_time_value; // ms since the epoch, UTC; NaN for an invalid date
};

class RegExpObject : public Object {
public:
    RegExpObject(Object* prototype, Regex regex, JsString* source, JsString* flags)
        : Object(prototype, Class::RegExp)
        , m_regex(std::move(regex))
        , m_source(source)
        , m_flags(flags)
    {
    }
    Regex const& regex() const { return m_regex; }
    JsString* source() const { return m_source; }
    JsString* flags() const { return m_flags; }
    // RegExpInitialize over an existing object (B.2.4.1 compile).
    void reset(Regex regex, JsString* source, JsString* flags)
    {
        m_regex = std::move(regex);
        m_source = source;
        m_flags = flags;
    }
    void trace(Tracer&) override;

private:
    Regex m_regex;
    JsString* m_source;
    JsString* m_flags;
};

// %ArrayIteratorPrototype%'s instances (§23.1.5.1): the array-like being
// walked, how far, and whether a step yields the index, the element or
// both. Exhausted, it lets the array go.
class ArrayIteratorObject : public Object {
public:
    enum class Kind : std::uint8_t { Keys, Values, Entries };

    ArrayIteratorObject(Object* prototype, Object* iterated, Kind kind)
        : Object(prototype, Class::ArrayIterator)
        , m_iterated(iterated)
        , m_kind(kind)
    {
    }

    Object* iterated() const { return m_iterated; }
    Kind kind() const { return m_kind; }
    double next_index() const { return m_next_index; }
    void set_next_index(double index) { m_next_index = index; }
    void finish() { m_iterated = nullptr; }
    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_iterated);
    }

private:
    Object* m_iterated;
    double m_next_index = 0;
    Kind m_kind;
};

// %StringIteratorPrototype%'s instances (§22.1.5): the string and the
// position of the next code point.
class StringIteratorObject : public Object {
public:
    StringIteratorObject(Object* prototype, JsString* string)
        : Object(prototype, Class::StringIterator)
        , m_string(string)
    {
    }

    JsString* string() const { return m_string; }
    std::size_t position() const { return m_position; }
    void set_position(std::size_t position) { m_position = position; }
    void finish() { m_string = nullptr; }
    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_string);
    }

private:
    JsString* m_string;
    std::size_t m_position = 0;
};

// A scope (§9.1): the bindings a block, function or script declares, or —
// when made over an object — that object's properties (the global object,
// a `with` target). Closures capture one; the chain runs outward.
class Environment : public Cell {
public:
    struct Binding {
        JsString* name; // an atom
        Value value;
        bool mutable_ = true; // false for const and for a function's own name binding
        bool initialized = true; // false in the temporal dead zone of let/const
        bool deletable = false; // true for a sloppy eval's var
        // An immutable binding that throws on every write (a const), as
        // against one that throws only from strict code: a sloppy function
        // expression's own name (§9.1.1.1.5 step 4, CreateImmutableBinding's S).
        bool strict = true;
    };

    explicit Environment(Environment* outer, Object* object = nullptr)
        : m_outer(outer)
        , m_object(object)
    {
    }

    Environment* outer() const { return m_outer; }
    Object* object() const { return m_object; }
    bool is_object_environment() const { return m_object != nullptr; }
    // `with` provides its object as `this` for calls; the global does not.
    bool is_with_environment() const { return m_with; }
    void set_with_environment(bool with) { m_with = with; }

    Binding* find(JsString* name);
    Binding const* find(JsString* name) const;
    Binding& declare(JsString* name, Value initial = Value::undefined(), bool mutable_ = true, bool initialized = true, bool deletable = false);
    bool remove(JsString* name); // deletable bindings only
    std::vector<Binding> const& bindings() const { return m_bindings; }

    // A function environment binds `this`; an arrow's does not, and a
    // lookup walks outward past it. A derived class constructor's binds
    // it only once `super()` has run (§9.1.1.3).
    bool has_this() const { return m_has_this; }
    bool this_initialized() const { return m_this_initialized; }
    Value this_value() const { return m_this; }
    void set_this(Value this_value)
    {
        m_this = this_value;
        m_has_this = true;
        m_this_initialized = true;
    }
    void set_this_uninitialized()
    {
        m_has_this = true;
        m_this_initialized = false;
    }
    Function* function() const { return m_function; }
    void set_function(Function* function) { m_function = function; }
    Object* new_target() const { return m_new_target; }
    void set_new_target(Object* target) { m_new_target = target; }

    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override { return sizeof(*this) + m_bindings.size() * sizeof(Binding); }

private:
    std::vector<Binding> m_bindings;
    Environment* m_outer;
    Object* m_object;
    Value m_this;
    Function* m_function = nullptr;
    Object* m_new_target = nullptr;
    bool m_has_this = false;
    bool m_this_initialized = true;
    bool m_with = false;
};

}
