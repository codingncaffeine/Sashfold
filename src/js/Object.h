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
#include "js/Shape.h"
#include "js/Value.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::js {

class Interpreter;
class Environment;
class Function;
class Program;
class RealmRecord;
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

struct NativeSpec;

// What makes a data property's value the first time anything looks at the
// property (Property::LazyValue): called once, with the object, the key and
// the realm the property was defined in. It may run script and may define
// other properties of the object, this one included; what it answers is the
// value unless it has put one itself.
using LazyValueMaker = Value (*)(Object& holder, PropertyKey const& key, RealmRecord& realm);

// One own property as it stands, read out of the object's shape and slot:
// what find_own and peek_own answer and properties() lists — a copy, so
// writing to it changes nothing. A property may be `lazy`: it is there —
// its key, its attributes, its place in the order — and what it holds is
// not made yet. A native method or accessor (LazyNative) shows the
// description of the method (or of the getter) and of the setter; a value
// made on demand (LazyValue) shows its maker; both show the realm they are
// made in. Only peek_own and properties() ever show one: find_own makes
// the property whole first.
struct Property {
    enum : std::uint8_t { NotLazy = 0, LazyNative = 1, LazyValue = 2 };

    PropertyKey key;
    union {
        Value value = {}; // data property
        RealmRecord* lazy_realm; // lazy: the realm it is made in
    };
    union {
        Object* getter = nullptr; // accessor property
        NativeSpec const* lazy_get; // LazyNative: the method's description, or the getter's
        LazyValueMaker lazy_make; // LazyValue: what makes the value
    };
    union {
        Object* setter = nullptr;
        NativeSpec const* lazy_set; // LazyNative: the setter's description, or null
    };
    std::uint8_t attributes = default_attributes;
    bool accessor = false;
    std::uint8_t lazy = NotLazy;

    bool writable() const { return (attributes & Writable) != 0; }
    bool enumerable() const { return (attributes & Enumerable) != 0; }
    bool configurable() const { return (attributes & Configurable) != 0; }
};

// What find_own and peek_own answer: the property, or none, compared with
// nullptr as the pointer they once answered was.
class PropertyRef {
public:
    PropertyRef() = default;
    explicit PropertyRef(Property const& property)
        : m_property(property)
        , m_found(true)
    {
    }
    explicit operator bool() const { return m_found; }
    bool operator==(std::nullptr_t) const { return !m_found; }
    Property const* operator->() const { return &m_property; }
    Property const& operator*() const { return m_property; }

private:
    Property m_property;
    bool m_found = false;
};

// What a slot keeps for a native accessor with a setter, or for a value
// made on demand, until it is made: interned for the life of the process,
// like the descriptions it names, so that the slot can hold its address.
// A native method, or a getter alone, is kept as its description itself.
struct LazyRecord {
    NativeSpec const* get = nullptr;
    NativeSpec const* set = nullptr;
    LazyValueMaker make = nullptr;

    bool operator==(LazyRecord const&) const = default;
    static LazyRecord const& intern(LazyRecord const&);
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
        BigInt,
        Date,
        RegExp,
        Arguments,
        ArrayIterator, // %ArrayIteratorPrototype%'s instances (§23.1.5)
        StringIterator, // %StringIteratorPrototype%'s instances (§22.1.5)
        RegExpStringIterator, // %RegExpStringIteratorPrototype%'s instances (§22.2.9)
        WeakRef, // §26.1
        FinalizationRegistry, // §26.2
        Map, // §24.1
        Set, // §24.2
        WeakMap, // §24.3
        WeakSet, // §24.4
        CollectionIterator, // %MapIteratorPrototype% and %SetIteratorPrototype%'s instances (§24.1.5, §24.2.5)
        Promise, // §27.2
        Generator, // §27.5: a generator, its frame suspended between next() calls
        AsyncGenerator, // §27.6
        AsyncContext, // an async function between an await and its resumption; never a script value
        AsyncFromSyncIterator, // §27.1.6
        Math,
        Json,
        Global,
        Host, // a DOM wrapper or another object the bindings own the meaning of
        ArrayBuffer, // §25.1
        TypedArray, // an Integer-Indexed exotic object (§10.4.5): a view of one element type over a buffer
        DataView, // §25.3
        ModuleNamespace, // a module namespace exotic object (§10.4.6): a module's exports as live properties
        Proxy, // a proxy exotic object (§10.5): every internal method is a call to a handler's trap
        Intl, // an object an ECMA-402 constructor made (js/Intl.h: IntlObject, its kind says which)
        DisposableStack, // §27.4 and §27.5: a stack of resources to dispose of, sync or async
    };

    // Made by a heap's allocate(), which it takes its first shape from: the
    // root of its prototype's (js/Shape.h).
    explicit Object(Object* prototype, Class class_id = Class::Object);
    ~Object() override;

    Class class_id() const { return m_class; }
    Object* prototype() const { return m_shape->prototype(); }
    // [[SetPrototypeOf]]: false when the object is not extensible or the
    // new chain would loop.
    bool set_prototype(Object*);
    bool is_extensible() const { return m_shape->is_extensible(); }
    void prevent_extensions();

    virtual bool is_callable() const { return false; }
    virtual bool is_constructor() const { return false; }
    bool is_array() const { return m_class == Class::Array; }
    bool is_error() const { return m_class == Class::Error; }
    bool is_host() const { return m_class == Class::Host; }

    // The realm a host's object belongs to (WebIDL's relevant realm of a
    // platform object): what a host's native called on it runs in, whichever
    // realm's copy of the native was called (NativeFunction::run_in_receivers_realm).
    // Null for every object of the language, which is no realm's.
    virtual RealmRecord* home_realm() const { return nullptr; }

    // [[IsHTMLDDA]] (HTML's "the all exotic object" is the only object the
    // web ever gives this slot, via `document.all`): ToBoolean, the abstract
    // equality comparison and typeof all treat an object with this slot as
    // if it were undefined, even though it is very much an object.
    bool is_html_dda() const { return m_is_html_dda; }
    void set_html_dda() { m_is_html_dda = true; }

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
    // Is this object a proxy? The chain walks in get() and set() hand over
    // to one they meet above themselves, since only a proxy's own internal
    // method can run its trap.
    bool is_proxy() const { return m_class == Class::Proxy; }

    // Shortcuts that never run script and never collect. The property
    // found is whole: a native not yet made is made first, and a property
    // the object has by its nature and has not been given room for (a
    // function's length, name and prototype) is given it.
    PropertyRef find_own(PropertyKey const&) const;
    // Define or overwrite a data property outright, no validation: how
    // intrinsics are built and how a fresh object is filled.
    void put(PropertyKey const&, Value const&, std::uint8_t attributes = default_attributes);
    void put_accessor(PropertyKey const&, Object* getter, Object* setter, std::uint8_t attributes = Configurable);
    // The same for a native kept as its description until something asks
    // for the function: a method, or an accessor's pair (the setter may be
    // null). The function, when made, is of `realm` whoever asks.
    void put_lazy(PropertyKey const&, NativeSpec const& method, RealmRecord& realm, std::uint8_t attributes);
    void put_lazy_accessor(PropertyKey const&, NativeSpec const* getter, NativeSpec const* setter, RealmRecord& realm,
        std::uint8_t attributes);
    // A data property whose value `make` makes when something first looks at
    // it: an interface's object on the global, which a host builds when a
    // page first names it.
    void put_lazy_value(PropertyKey const&, LazyValueMaker make, RealmRecord& realm, std::uint8_t attributes);
    // The property as it stands — a native not yet made stays unmade — for
    // a host that reshapes its members before any script has seen them.
    // None for a key the storage does not hold.
    PropertyRef peek_own(PropertyKey const&) const;
    // An own data property's value written where it stands, its attributes
    // untouched; false, and nothing written, when there is no such data
    // property (an accessor, or none).
    bool write_own_value(PropertyKey const&, Value const&);
    // Gives every property the object still owes itself its room now (a
    // function's length, name and prototype): what the eager mode does to
    // each function as it is made.
    void settle_pending();
    // Room for the properties a maker is about to put, taken once rather
    // than grown a property at a time.
    void reserve_properties(std::size_t count);
    bool remove_own(PropertyKey const&); // unconditional erase
    std::size_t own_property_count() const;
    // The properties as they stand, in creation order, for callers that
    // look at everything (the bindings learning what a group added).
    std::vector<Property> properties() const;

    // What it has, apart from what those hold (js/Shape.h).
    Shape* shape() const { return m_shape; }
    // For the inline caches (js/Feedback.h), whose answers say which slot
    // and are checked against the shape before they are used: a slot as it
    // stands (a value, an accessor's pair, or the mark of a native not yet
    // made), a slot written, and a property added by the transition a
    // cache saw a write make (`after` adds the slot `index`, this object's
    // next).
    Value const& slot_value(std::uint32_t index) const { return m_slots[index]; }
    void cache_store(std::uint32_t index, Value const& value) { m_slots[index] = value; }
    void cache_add(Shape* after, std::uint32_t index, Value const& value)
    {
        if (index >= m_slot_capacity)
            grow_slots(index + 1);
        m_slots[index] = value;
        m_slot_count = index + 1;
        m_shape = after;
    }
    // Said once by the constructor of an exotic object some of whose own
    // properties are not its storage's: its shapes are uncacheable.
    void mark_uncacheable();
    // Leaves the shared shapes for a dictionary of its own (js/Shape.h says
    // when); nothing a script can see changes.
    void become_dictionary();
    // A binding in front of its properties appeared (a script's let or
    // const in front of the global object's): what the caches answered from
    // them is answered no more.
    void shadowed() { changed(); }
    // A dictionary that says it is another object's prototype (Shape::Prototype).
    void become_prototype();

    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override;

    // How many values an object holds inside itself before its slots move
    // to a block of their own.
    static constexpr std::uint32_t inline_slot_capacity = 4;

protected:
    // The properties an object has by its nature and gives room to only
    // when one is first looked for: a function's `length`, `name` and
    // `prototype`, in the order the standard lists them (§10.2.3, §10.2.5,
    // §10.3.3). One bit each, lowest first; the shape keeps them (an
    // object's pending properties are its shape's), and whenever one is
    // given room, those before it are given theirs first, and all of them
    // before any other property is added — so the standard's order is the
    // order they are added in.
    enum : std::uint8_t {
        PendingLength = 1,
        PendingName = 2,
        PendingPrototype = 4,
    };
    void set_pending(std::uint8_t which);
    bool is_pending(std::uint8_t which) const { return (m_shape->pending() & which) != 0; }
    // What a pending property is: its value and attributes. Called once,
    // with no collection possible, when the property is first looked for.
    virtual std::pair<Value, std::uint8_t> make_pending(std::uint8_t which);

    // The storage, for the exotic objects built on it. own_entry is the
    // shape's entry as stored; whole_entry makes the property whole first.
    // Either is good until the object's properties next change.
    ShapeEntry const* own_entry(PropertyKey const& key) const { return m_shape->find(key); }
    ShapeEntry const* whole_entry(PropertyKey const&) const;
    Value const& slot(std::uint32_t index) const { return m_slots[index]; }
    // A data property added (the key must be new) or its value and
    // attributes replaced, with no regard to the routing an array's index
    // takes in put(), and the property's descriptor as it stands.
    void put_data(PropertyKey const&, Value const&, std::uint8_t attributes);
    PropertyDescriptor descriptor_of(ShapeEntry const&) const;
    // Steps 2.c–d and 6 of ValidateAndApplyPropertyDescriptor (§10.1.6.3)
    // over the storage, once validation has admitted the descriptor.
    void create_property(PropertyKey const&, PropertyDescriptor const&);
    void apply_descriptor(ShapeEntry const&, PropertyDescriptor const&);
    // An own property gone, whatever its attributes.
    void delete_entry(PropertyKey const&);

    friend struct MachineLayout; // the offsets the machine code reads (Vm.cpp)
    // The header machine code reads: the shape, then the slots (an
    // object's own block, or its inline room) and how many of them are in
    // use and there is room for.
    Shape* m_shape = nullptr;
    Value* m_slots;
    std::uint32_t m_slot_count = 0;
    std::uint32_t m_slot_capacity = inline_slot_capacity;

public:
    // Bindings park the C++ side of a host object here; untraced, unowned.
    void* host_data = nullptr;

protected:
    Class m_class;
    bool m_is_html_dda = false;
    // The shape is a dictionary of its own (Shape::to_dictionary), which
    // it deletes: known without reading the shape, which a destructor run
    // in a sweep may not (a shared shape may have gone first).
    bool m_owns_shape = false;

private:
    Heap& owner() const;
    std::uint32_t add_property(PropertyKey const&, std::uint8_t attributes, bool accessor);
    bool is_function_own(PropertyKey const&) const;
    void changed(PropertyKey const* key = nullptr, bool chain = false);
    std::uint32_t take_slot();
    void grow_slots(std::uint32_t needed);
    void reconfigure(std::uint32_t position, std::uint8_t attributes, bool accessor);
    void set_accessor_slot(std::uint32_t slot, Object* getter, Object* setter, bool reuse);
    Property view_of(ShapeEntry const&) const;
    ShapeEntry const* find_lazy(PropertyKey const&);
    ShapeEntry const* find_pending(PropertyKey const&);
    void make_lazy(PropertyKey const&, RealmRecord&, bool at_once = false);
    std::uint8_t pending_named(PropertyKey const&) const;
    void settle(std::uint8_t which);
    void settle_one(std::uint8_t which);
    void settle_all();
    bool ready_for_lazy(RealmRecord&);
    void put_mark(PropertyKey const&, Value const& mark, std::uint8_t attributes, bool accessor);

    Value m_inline_slots[inline_slot_capacity];
};

// IsCompatiblePropertyDescriptor (§10.1.6.2): may `desc` be applied over
// `current` — absent when the property does not exist — on an object of
// this extensibility? Steps 2 through 5 of
// ValidateAndApplyPropertyDescriptor, with nothing changed; a proxy's
// [[GetOwnProperty]] and [[DefineOwnProperty]] need the test on its own,
// against a descriptor a trap invented. Defined in Objects.cpp.
bool is_compatible_property_descriptor(bool extensible, PropertyDescriptor const& desc,
    std::optional<PropertyDescriptor> const& current);

// An array's dense elements: a block of ours — the pointer, the length and
// the room — at fixed offsets, which machine code reads (std::vector's
// layout differs between the standard libraries). A hole is
// Value::empty(); the room grows by half again, as std::vector's would.
class ElementBlock {
public:
    ElementBlock() = default;
    explicit ElementBlock(std::span<Value const>);
    ~ElementBlock();
    ElementBlock(ElementBlock const&) = delete;
    ElementBlock& operator=(ElementBlock const&) = delete;

    std::size_t size() const { return m_length; }
    std::size_t capacity() const { return m_capacity; }
    bool empty() const { return m_length == 0; }
    Value* data() { return m_data; }
    Value const* data() const { return m_data; }
    Value* begin() { return m_data; }
    Value* end() { return m_data + m_length; }
    Value const* begin() const { return m_data; }
    Value const* end() const { return m_data + m_length; }
    Value& operator[](std::size_t index) { return m_data[index]; }
    Value const& operator[](std::size_t index) const { return m_data[index]; }
    // New elements take `fill`; a shorter length lets the rest go (the
    // room stays).
    void resize(std::size_t length, Value fill = Value());
    void reserve(std::size_t capacity);
    void push_back(Value const& value)
    {
        if (m_length == m_capacity)
            reserve(m_capacity + m_capacity / 2 + 4);
        m_data[m_length++] = value;
    }

private:
    friend struct MachineLayout; // the offsets the machine code reads (Vm.cpp)
    Value* m_data = nullptr;
    std::uint32_t m_length = 0;
    std::uint32_t m_capacity = 0;
};

static_assert(sizeof(ElementBlock) == 16);

// An Array exotic object (§10.4.2). Indices below dense_size() live in its
// element block, holes as Value::empty(); anything sparse beyond it is an
// ordinary property. `length` is virtual: it is answered from m_length and
// never stored as a property.
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
    void reserve_elements(std::size_t count) { m_elements.reserve(count); } // room, taken once, for a literal's elements
    std::uint32_t dense_size() const { return static_cast<std::uint32_t>(m_elements.size()); }
    ElementBlock& dense() { return m_elements; }
    ElementBlock const& dense() const { return m_elements; }
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
    friend struct MachineLayout; // the offsets the machine code reads (Vm.cpp)
    ElementBlock m_elements;
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

    // [[Realm]] (§10.2, §10.3): the realm the function was made in. A call
    // makes it the current realm for as long as the call runs, so what the
    // function creates and throws is that realm's. A bound function and a
    // proxy have none of their own: null, and a call through one runs in
    // its caller's realm until it reaches the target.
    RealmRecord* realm() const { return m_realm; }
    void set_realm(RealmRecord* realm) { m_realm = realm; }
    void trace(Tracer&) override;
    // A ScriptFunction: what the run loop asks before it makes a call itself.
    bool is_script() const { return m_script; }

protected:
    void mark_script() { m_script = true; }

private:
    RealmRecord* m_realm = nullptr;
    bool m_script = false;
};

class ScriptFunction;
struct CodeBlock;

// A class field a constructor defines on each instance (§15.7.10
// ClassFieldDefinition): the key — a Private Name for `#x` — and the
// initializer as a function called with the instance as `this`, or none.
struct ClassField {
    PropertyKey key;
    ScriptFunction* initializer = nullptr;
};

// A private method or accessor a class installs on each instance (§6.2.10
// PrivateElement of kind method or accessor): the name, and the method, or
// a getter and a setter of which either may be missing.
struct PrivateMethod {
    Symbol* name = nullptr;
    Object* method = nullptr;
    Object* getter = nullptr;
    Object* setter = nullptr;
};

// A PrivateEnvironment Record (§9.2): the Private Names a class body
// declares, each a fresh cell per evaluation of the class, chained to the
// enclosing class's. Every function made inside the body keeps the one it
// was made in, and `this.#x` resolves `#x` outward through the chain.
class PrivateEnvironment : public Cell {
public:
    explicit PrivateEnvironment(PrivateEnvironment* outer)
        : m_outer(outer)
    {
    }
    PrivateEnvironment* outer() const { return m_outer; }
    void add(JsString* description, Symbol* name) { m_names.emplace_back(description, name); }
    // ResolvePrivateIdentifier (§9.2.1.2): null only when no enclosing
    // class declared the name, which the parser has already refused.
    Symbol* lookup(JsString* description) const
    {
        for (PrivateEnvironment const* env = this; env != nullptr; env = env->m_outer) {
            for (auto const& [known, name] : env->m_names) {
                if (known == description)
                    return name;
            }
        }
        return nullptr;
    }
    std::vector<std::pair<JsString*, Symbol*>> const& names() const { return m_names; }
    void trace(Tracer& tracer) override
    {
        tracer.visit(m_outer);
        for (auto const& [description, name] : m_names) {
            tracer.visit(description);
            tracer.visit(name);
        }
    }
    std::size_t size_in_bytes() const override { return sizeof(*this) + m_names.size() * sizeof(m_names[0]); }

private:
    PrivateEnvironment* m_outer;
    std::vector<std::pair<JsString*, Symbol*>> m_names;
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
    // [[PrivateMethods]]: what a class constructor installs on each
    // instance before its fields (§7.3.34).
    std::vector<PrivateMethod>& private_methods() { return m_private_methods; }
    std::vector<PrivateMethod> const& private_methods() const { return m_private_methods; }
    // [[PrivateEnvironment]]: the class body's, when made inside one.
    PrivateEnvironment* private_environment() const { return m_private_environment; }
    void set_private_environment(PrivateEnvironment* environment) { m_private_environment = environment; }
    // Its body compiled, once the first call has asked: the interpreter's,
    // kept here so that a call needs no lookup.
    CodeBlock const* compiled() const { return m_compiled; }
    void set_compiled(CodeBlock const* code) { m_compiled = code; }
    // `length` and `name` (OrdinaryFunctionCreate, §10.2.3) and, when asked
    // and the function is a constructor or a generator, the `prototype`
    // object MakeConstructor gives it (§10.2.5), put off until one of them
    // is looked for: most closures are made, called and dropped without
    // anyone asking. Said once by the function's maker, with its realm set.
    void defer_own_properties(bool with_prototype);

    std::optional<Value> call(Interpreter&, Value const& this_value, std::span<Value const> arguments) override;
    std::optional<Value> construct(Interpreter&, std::span<Value const> arguments, Object* new_target) override;

    void trace(Tracer&) override;

protected:
    std::pair<Value, std::uint8_t> make_pending(std::uint8_t which) override;

private:
    FunctionNode const* m_node;
    Environment* m_scope;
    Object* m_home_object = nullptr;
    PrivateEnvironment* m_private_environment = nullptr;
    std::vector<ClassField> m_fields;
    std::vector<PrivateMethod> m_private_methods;
    CodeBlock const* m_compiled = nullptr;
    bool m_constructable;
};

// A function written in C++: the built-in library and the DOM bindings.
class NativeFunction : public Function {
public:
    // A native that carries state of its own: a closure.
    using Callback = std::function<std::optional<Value>(Interpreter&, Value const& this_value, std::span<Value const> arguments)>;
    using ConstructCallback = std::function<std::optional<Value>(Interpreter&, std::span<Value const> arguments, Object* new_target)>;
    // A native that carries none, which is nearly every one: a plain
    // function. It can be described once for every realm (NativeSpec) and
    // its function object made only when something asks for it.
    using Entry = std::optional<Value> (*)(Interpreter&, Value const& this_value, std::span<Value const> arguments);
    using ConstructEntry = std::optional<Value> (*)(Interpreter&, std::span<Value const> arguments, Object* new_target);
    // A host's function in front of a described native's own: handed the
    // function called, it decides what the entry is called with — the
    // bindings' receiver and argument checks, which read their terms from
    // the description's flags instead of each holding a closure.
    using Guard = std::optional<Value> (*)(Interpreter&, NativeFunction& function, Value const& this_value,
        std::span<Value const> arguments);
    // What a described native is to the property it was defined as: its
    // name is the key, or the key after "get " or "set ".
    enum class Role : std::uint8_t { Method, Getter, Setter };

    NativeFunction(Object* prototype, Callback call, ConstructCallback construct = {});
    NativeFunction(Object* prototype, NativeSpec const& spec, PropertyKey key, Role role, Object* home);
    ~NativeFunction() override;

    bool is_constructor() const override;
    std::optional<Value> call(Interpreter&, Value const& this_value, std::span<Value const> arguments) override;
    std::optional<Value> construct(Interpreter&, std::span<Value const> arguments, Object* new_target) override;
    // The native's own steps with nothing in front of them — no realm
    // entered, no guard — for a host that performs them in another
    // function's place (the bindings' cross-origin members).
    std::optional<Value> perform(Interpreter&, Value const& this_value, std::span<Value const> arguments);
    // A host's operation or attribute: called on an object that names a home
    // realm, it runs in that realm and not in the one it was made in, so the
    // state it works on is the receiver's. A built-in of the language runs
    // in its own realm whatever it is called on (§10.3.1).
    void run_in_receivers_realm() { m_in_receivers_realm = true; }
    // A host's interface constructor: the object it makes takes its
    // prototype from new.target when that is another function (WebIDL
    // §3.7.1, "internally create a new object implementing the interface"),
    // so `class Mine extends EventTarget` makes a Mine. A built-in of the
    // language reads new.target itself.
    void take_prototype_from_new_target() { m_prototype_from_new_target = true; }

    // A described native: its description, the key and role it was defined
    // with, and the object it was defined on (an interface's prototype,
    // which its guard compares a receiver's chain against). Null, an empty
    // key and null for a native made of closures.
    NativeSpec const* spec() const { return m_closures_made ? nullptr : m_spec; }
    PropertyKey const& key() const { return m_key; }
    Role role() const { return m_role; }
    Object* home() const { return m_home; }
    // A closure-made native's callback; null for a described one.
    Callback const* closure() const;
    // A closure-made native's `name` (an atom) and `length`, which its maker
    // says once; a described one reads them off its key and description.
    void set_name_and_length(JsString* name, int length);
    // A guard's own note on this function (what it worked out at the first
    // call and need not work out again).
    std::uint8_t guard_note = 0;

    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override;

protected:
    std::pair<Value, std::uint8_t> make_pending(std::uint8_t which) override;

private:
    struct Closures;
    union {
        NativeSpec const* m_spec;
        Closures* m_closures;
    };
    PropertyKey m_key;
    Object* m_home = nullptr;
    Role m_role = Role::Method;
    bool m_closures_made = false;
    bool m_in_receivers_realm = false;
    bool m_prototype_from_new_target = false;
};

// What a native function is, apart from any realm: its entry, what `length`
// it reports, what a host put in front of it and the host's flags for that,
// and a number of the definer's own (which of several properties one entry
// serves). Interned for the life of the process — the same description is
// the same record in every realm and every engine — so a property can hold
// one in place of the function until the function is asked for.
struct NativeSpec {
    NativeFunction::Entry call = nullptr;
    NativeFunction::ConstructEntry construct = nullptr;
    NativeFunction::Guard guard = nullptr;
    std::uint32_t datum = 0;
    std::uint16_t flags = 0; // ReceiversRealm, and from HostFlag up the host's own
    std::uint8_t length = 0;

    static constexpr std::uint16_t ReceiversRealm = 1; // NativeFunction::run_in_receivers_realm
    static constexpr std::uint16_t HostFlag = 2;

    bool operator==(NativeSpec const&) const = default;
    // The record for this description: one per distinct description, never freed.
    static NativeSpec const& intern(NativeSpec const&);
};

// Whether natives are made when asked for (the default) or all at once as
// they are defined, which is what the engine did before and is kept as the
// differential mode: SASHFOLD_LAZY=0 in the environment, read once.
bool lazy_natives();
// For a test that runs both ways in one process.
void set_lazy_natives(bool);

// A function written in C++ that carries values: the resolving functions
// of a promise, a combinator's element functions, `finally`'s thunks. The
// slots are traced, which a capture in a std::function could not be, so
// a callback reaches its state through `self.slot(i)` and never by
// capturing a cell.
class ClosureFunction : public Function {
public:
    using Callback = std::function<std::optional<Value>(Interpreter&, ClosureFunction& self, Value const& this_value,
        std::span<Value const> arguments)>;

    ClosureFunction(Object* prototype, std::vector<Value> slots, Callback callback)
        : Function(prototype)
        , m_slots(std::move(slots))
        , m_callback(std::move(callback))
    {
    }

    Value const& slot(std::size_t index) const { return m_slots[index]; }
    void set_slot(std::size_t index, Value const& value) { m_slots[index] = value; }
    std::size_t slot_count() const { return m_slots.size(); }
    // Its `name` (an atom) and `length`, said once by its maker and given
    // room as properties only when one is looked for: a promise's resolving
    // functions are made by the pair for every promise and almost never asked.
    void set_name_and_length(JsString* name, int length);

    std::optional<Value> call(Interpreter&, Value const& this_value, std::span<Value const> arguments) override;
    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override { return Object::size_in_bytes() + m_slots.size() * sizeof(Value); }

protected:
    std::pair<Value, std::uint8_t> make_pending(std::uint8_t which) override;

private:
    std::vector<Value> m_slots;
    Callback m_callback;
    JsString* m_name = nullptr;
    std::int32_t m_length = 0;
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
    RegExpObject(Object* prototype, Regex regex, JsString* source, JsString* flags, RealmRecord* realm, bool legacy_features)
        : Object(prototype, Class::RegExp)
        , m_regex(std::move(regex))
        , m_source(source)
        , m_flags(flags)
        , m_realm(realm)
        , m_legacy_features(legacy_features)
    {
    }
    Regex const& regex() const { return m_regex; }
    JsString* source() const { return m_source; }
    JsString* flags() const { return m_flags; }
    // The legacy RegExp features' slots: the realm the object was made in,
    // and whether that realm's own RegExp made it rather than a subclass.
    RealmRecord* realm() const { return m_realm; }
    bool legacy_features() const { return m_legacy_features; }
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
    RealmRecord* m_realm;
    bool m_legacy_features;
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

class CollectionIteratorObject;

// The table behind Map, Set, WeakMap and WeakSet (§24.1–§24.4): entries in
// the order they were added, keyed by SameValueZero, a deleted one left
// as a hole so that an iterator mid-walk keeps its place. The holes are
// squeezed out when they outnumber the live entries, every attached
// iterator's position moved along with them; an iterator and its table
// know each other for as long as both live, and whichever dies first
// tells the other. Defined in RuntimeCollections.cpp.
class CollectionTable {
public:
    struct Entry {
        Value key;
        Value value;
        bool live = false;
    };

    // The cell that holds the table, so that an entry added can be told to
    // that cell's heap; null for a table in no cell.
    explicit CollectionTable(Cell const* owner = nullptr)
        : m_owner(owner)
    {
    }
    ~CollectionTable();
    CollectionTable(CollectionTable const&) = delete;
    CollectionTable& operator=(CollectionTable const&) = delete;

    std::size_t size() const { return m_live; }
    Entry const* find(Value const& key) const; // null when absent
    bool has(Value const& key) const { return find(key) != nullptr; }
    void set(Value const& key, Value const& value); // adds or replaces; a −0 key is stored as +0
    bool remove(Value const& key);
    void clear();
    std::vector<Entry> const& entries() const { return m_entries; } // holes included, for iteration by index

    void attach(CollectionIteratorObject*);
    void detach(CollectionIteratorObject*);
    void trace(Tracer&) const;
    std::size_t size_in_bytes() const { return m_entries.size() * sizeof(Entry) + m_index.size() * 3 * sizeof(void*); }

    // A forEach counting through the entries by index: no compaction
    // while one is open.
    class WalkGuard {
    public:
        explicit WalkGuard(CollectionTable& table)
            : m_table(table)
        {
            ++m_table.m_walkers;
        }
        ~WalkGuard() { --m_table.m_walkers; }
        WalkGuard(WalkGuard const&) = delete;
        WalkGuard& operator=(WalkGuard const&) = delete;

    private:
        CollectionTable& m_table;
    };

private:
    struct Hash {
        std::size_t operator()(Value const&) const;
    };
    struct Equal {
        bool operator()(Value const&, Value const&) const;
    };
    void compact();

    Cell const* m_owner;
    std::vector<Entry> m_entries;
    std::unordered_map<Value, std::size_t, Hash, Equal> m_index;
    std::size_t m_live = 0;
    int m_walkers = 0;
    std::vector<CollectionIteratorObject*> m_iterators;
};

// A Map, Set, WeakMap or WeakSet; the class says which. The weak kinds
// hold their keys as strongly as the others for now: the collector has no
// ephemerons, and nothing a script can observe tells the difference.
class CollectionObject : public Object {
public:
    CollectionObject(Object* prototype, Class class_id)
        : Object(prototype, class_id)
        , m_table(this)
    {
    }
    CollectionTable& table() { return m_table; }
    CollectionTable const& table() const { return m_table; }
    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override { return Object::size_in_bytes() + m_table.size_in_bytes(); }

private:
    CollectionTable m_table;
};

// %MapIteratorPrototype% and %SetIteratorPrototype%'s instances: the
// collection being walked, the position, and what a step yields.
// Exhausted, it lets the collection go.
class CollectionIteratorObject : public Object {
public:
    enum class Kind : std::uint8_t { Keys, Values, Entries };

    CollectionIteratorObject(Object* prototype, CollectionObject* collection, Kind kind, bool is_map);
    ~CollectionIteratorObject() override;

    CollectionObject* collection() const { return m_collection; }
    Kind kind() const { return m_kind; }
    bool is_map() const { return m_is_map; }
    std::size_t index() const { return m_index; }
    void set_index(std::size_t index) { m_index = index; }
    void finish();
    // The table telling the iterator it is gone.
    void table_gone() { m_attached = false; }
    void trace(Tracer&) override;

private:
    CollectionObject* m_collection;
    std::size_t m_index = 0;
    Kind m_kind;
    bool m_is_map;
    bool m_attached = true;
};

// A PromiseReaction Record (§27.2.1.2): what to do when a promise settles
// — the handler, or none (the value or reason passes through), and the
// capability of the promise `then` derived, or none (an await's).
struct PromiseReaction {
    enum class Type : std::uint8_t { Fulfill, Reject };
    Type type = Type::Fulfill;
    Value handler = Value::empty(); // empty = pass through
    Value capability_promise = Value::empty(); // empty = no derived promise
    Value capability_resolve = Value::empty();
    Value capability_reject = Value::empty();
};

// A promise (§27.2.6): its state, its result once settled, and until
// then the reactions waiting on each outcome. `handled` is
// [[PromiseIsHandled]]: whether anything has ever asked for the outcome,
// which decides whether a rejection is reported.
class PromiseObject : public Object {
public:
    enum class State : std::uint8_t { Pending, Fulfilled, Rejected };

    explicit PromiseObject(Object* prototype)
        : Object(prototype, Class::Promise)
    {
    }

    State state() const { return m_state; }
    Value const& result() const { return m_result; }
    std::vector<PromiseReaction>& fulfill_reactions() { return m_fulfill_reactions; }
    std::vector<PromiseReaction>& reject_reactions() { return m_reject_reactions; }
    // Settles a pending promise; both reaction lists are let go (the
    // caller has taken the ones it will trigger).
    void settle(State state, Value const& result)
    {
        m_state = state;
        m_result = result;
        m_fulfill_reactions.clear();
        m_reject_reactions.clear();
    }
    bool is_handled() const { return m_handled; }
    void set_handled() { m_handled = true; }

    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override
    {
        return Object::size_in_bytes() + (m_fulfill_reactions.size() + m_reject_reactions.size()) * sizeof(PromiseReaction);
    }

private:
    std::vector<PromiseReaction> m_fulfill_reactions;
    std::vector<PromiseReaction> m_reject_reactions;
    Value m_result;
    State m_state = State::Pending;
    bool m_handled = false;
};

// A WeakRef (§26.1). The target is held STRONGLY: the collector has no
// weak edges yet, so deref() always finds it — a valid outcome, since the
// specification lets an engine never collect a target — but nothing is
// released by dropping the last other reference.
class WeakRefObject : public Object {
public:
    WeakRefObject(Object* prototype, Value target)
        : Object(prototype, Class::WeakRef)
        , m_target(target)
    {
    }

    Value target() const { return m_target; }
    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_target);
    }

private:
    Value m_target;
};

// A FinalizationRegistry (§26.2): the cleanup callback and the cells
// registered. Targets, held values and tokens are held strongly (see
// WeakRefObject), so no cell is ever finalized and the callback never runs.
class FinalizationRegistryObject : public Object {
public:
    struct Cell {
        Value target;
        Value held;
        Value token;
    };

    FinalizationRegistryObject(Object* prototype, Value cleanup)
        : Object(prototype, Class::FinalizationRegistry)
        , m_cleanup(cleanup)
    {
    }

    std::vector<Cell>& cells() { return m_cells; }
    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_cleanup);
        for (Cell const& cell : m_cells) {
            tracer.visit(cell.target);
            tracer.visit(cell.held);
            tracer.visit(cell.token);
        }
    }

private:
    Value m_cleanup;
    std::vector<Cell> m_cells;
};

// %RegExpStringIteratorPrototype%'s instances (§22.2.9): the matcher, the
// string it runs over, and whether it is global and unicode; done once a
// step found no match or the pattern was not global.
class RegExpStringIteratorObject : public Object {
public:
    RegExpStringIteratorObject(Object* prototype, Object* matcher, JsString* string, bool global, bool unicode)
        : Object(prototype, Class::RegExpStringIterator)
        , m_matcher(matcher)
        , m_string(string)
        , m_global(global)
        , m_unicode(unicode)
    {
    }

    Object* matcher() const { return m_matcher; }
    JsString* string() const { return m_string; }
    bool global() const { return m_global; }
    bool unicode() const { return m_unicode; }
    bool done() const { return m_done; }
    void finish() { m_done = true; }
    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(m_matcher);
        tracer.visit(m_string);
    }

private:
    Object* m_matcher;
    JsString* m_string;
    bool m_global;
    bool m_unicode;
    bool m_done = false;
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

// ----------------------------------------------- ArrayBuffer and its views

// The element types a typed array or a DataView reads and writes (§23.2,
// Table 71): the integer widths, the clamped byte, the three float widths
// and the two BigInt widths.
enum class ElementType : std::uint8_t {
    Int8,
    Uint8,
    Uint8Clamped,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Float16,
    Float32,
    Float64,
    BigInt64,
    BigUint64,
};
inline constexpr int element_type_count = 12;
constexpr std::size_t element_size(ElementType type)
{
    switch (type) {
    case ElementType::Int8:
    case ElementType::Uint8:
    case ElementType::Uint8Clamped:
        return 1;
    case ElementType::Int16:
    case ElementType::Uint16:
    case ElementType::Float16:
        return 2;
    case ElementType::Int32:
    case ElementType::Uint32:
    case ElementType::Float32:
        return 4;
    case ElementType::Float64:
    case ElementType::BigInt64:
    case ElementType::BigUint64:
        return 8;
    }
    return 1;
}
// IsBigIntElementType (§10.4.5.x): the kinds whose elements are BigInts;
// a typed array's content type, which never mixes with the other.
constexpr bool is_bigint_element(ElementType type)
{
    return type == ElementType::BigInt64 || type == ElementType::BigUint64;
}
// "Int8Array" … "BigUint64Array": the constructor's name and the tag.
std::string_view element_type_name(ElementType);
// NumericToRawBytes and RawBytesToNumeric (§25.1.3.16–.17): one element,
// in the byte order asked for. Defined in RuntimeArrayBuffer.cpp. The
// double forms serve the Number kinds; the Value forms serve every kind,
// a BigInt element being made on the heap as it is read and taken as
// its low sixty-four bits as it is written.
void write_element(ElementType, std::uint8_t* out, double, bool little_endian);
double read_element(ElementType, std::uint8_t const* in, bool little_endian);
void write_element_value(ElementType, std::uint8_t* out, Value const& numeric, bool little_endian);
Value read_element_value(Heap&, ElementType, std::uint8_t const* in, bool little_endian);

// An ArrayBuffer (§25.1): a block of bytes, fixed in length or resizable
// up to a maximum decided when it was made, and detachable — after which
// it has no bytes at all and every view over it is out of bounds. Typed
// arrays and DataViews read and write through it and own no bytes of
// their own.
class ArrayBufferObject : public Object {
public:
    ArrayBufferObject(Object* prototype, std::size_t byte_length, std::optional<std::size_t> max_byte_length)
        : Object(prototype, Class::ArrayBuffer)
        , m_bytes(byte_length, 0)
        , m_max_byte_length(max_byte_length)
    {
    }

    std::size_t byte_length() const { return m_bytes.size(); } // 0 once detached
    std::uint8_t* data() { return m_bytes.data(); }
    std::uint8_t const* data() const { return m_bytes.data(); }
    bool is_detached() const { return m_detached; }
    bool is_resizable() const { return m_max_byte_length.has_value(); } // the opposite of IsFixedLengthArrayBuffer
    std::optional<std::size_t> max_byte_length() const { return m_max_byte_length; }
    // DetachArrayBuffer (§25.1.3.5): the bytes are let go for good.
    void detach()
    {
        std::vector<std::uint8_t>().swap(m_bytes);
        m_detached = true;
    }
    // A resizable buffer's new length, within its maximum; new bytes are zero.
    void resize(std::size_t byte_length)
    {
        std::size_t const before = m_bytes.capacity();
        m_bytes.resize(byte_length, 0);
        if (Heap* owner = heap(); owner != nullptr && m_bytes.capacity() > before)
            owner->grew(m_bytes.capacity() - before);
    }

    std::size_t size_in_bytes() const override { return sizeof(*this) + m_bytes.capacity(); }

private:
    std::vector<std::uint8_t> m_bytes;
    std::optional<std::size_t> m_max_byte_length;
    bool m_detached = false;
};

// A typed array (§10.4.5, §23.2): a view of elements of one type over an
// ArrayBuffer from a byte offset — either a fixed count of them or, over
// a resizable buffer, as many as fit up to the buffer's end. Its numeric
// keys, every canonical numeric string and not only the array indices,
// are the elements: [[Get]] and [[Set]] read and write the buffer, an
// index past the end or over a detached buffer is absent, and no element
// can be made an accessor, read-only or non-enumerable. Defined in
// RuntimeArrayBuffer.cpp.
class TypedArrayObject : public Object {
public:
    // No length = length-tracking: the view follows a resizable buffer's
    // end. No buffer = still being constructed (out of bounds until then).
    TypedArrayObject(Object* prototype, ElementType, ArrayBufferObject*, std::size_t byte_offset, std::optional<std::size_t> length);

    ElementType element_type() const { return m_type; }
    std::size_t element_size() const { return js::element_size(m_type); }
    ArrayBufferObject* buffer() const { return m_buffer; }
    std::size_t byte_offset() const { return m_byte_offset; } // [[ByteOffset]], whatever the bounds
    bool is_length_tracking() const { return !m_length.has_value(); }
    // The constructors' initializers hand the view its buffer.
    void attach(ArrayBufferObject*, std::size_t byte_offset, std::optional<std::size_t> length);
    // IsTypedArrayOutOfBounds (§10.4.5.12): detached, or reaching past the
    // buffer's current end.
    bool is_out_of_bounds() const;
    // TypedArrayLength (§10.4.5.13) and TypedArrayByteLength (§10.4.5.14);
    // both 0 when out of bounds.
    std::size_t length() const;
    std::size_t byte_length() const { return length() * element_size(); }
    // IsValidIntegerIndex (§10.4.5.15).
    bool is_valid_index(double) const;
    // The storage half of TypedArrayGetElement and TypedArraySetElement:
    // the index must be valid, and the value is a Number or a BigInt
    // already converted for the kind (ToNumber, or ToBigInt for the BigInt
    // kinds). Neither runs script; a read of a BigInt kind makes a cell.
    Value get_element(std::size_t index) const;
    void set_element(std::size_t index, Value const& numeric);
    void set_element(std::size_t index, double number) { set_element(index, Value::number(number)); }
    // The numeric index a key names (CanonicalNumericIndexString, §7.1.21),
    // or none for an ordinary name or a symbol.
    static std::optional<double> numeric_index(PropertyKey const&);
    // [[DefineOwnProperty]] (§10.4.5.3) for a numeric key, with the
    // interpreter at hand so that the value can be converted by ToNumber;
    // the interpreter's wrappers route every such define here.
    std::optional<bool> define_numeric(Interpreter&, double numeric_index, PropertyDescriptor const&);

    std::optional<PropertyDescriptor> get_own_property(PropertyKey const&) const override;
    bool define_own_property(PropertyKey const&, PropertyDescriptor const&) override;
    bool has_property(PropertyKey const&) const override;
    std::optional<Value> get(Interpreter&, PropertyKey const&, Value const& receiver) override;
    std::optional<bool> set(Interpreter&, PropertyKey const&, Value const&, Value const& receiver) override;
    bool delete_property(PropertyKey const&) override;
    std::vector<PropertyKey> own_keys() const override;
    void trace(Tracer&) override;

private:
    ArrayBufferObject* m_buffer;
    std::size_t m_byte_offset;
    std::optional<std::size_t> m_length; // [[ArrayLength]]; absent = auto
    ElementType m_type;
};

// A DataView (§25.3): a window of bytes over an ArrayBuffer, read and
// written one element at a time in the byte order each call names; over a
// resizable buffer it may track the buffer's end. Defined in
// RuntimeArrayBuffer.cpp.
class DataViewObject : public Object {
public:
    DataViewObject(Object* prototype, ArrayBufferObject*, std::size_t byte_offset, std::optional<std::size_t> byte_length);

    ArrayBufferObject* buffer() const { return m_buffer; }
    std::size_t byte_offset() const { return m_byte_offset; }
    bool is_length_tracking() const { return !m_byte_length.has_value(); }
    bool is_out_of_bounds() const; // IsViewOutOfBounds (§25.3.1.3)
    std::size_t view_byte_length() const; // GetViewByteLength (§25.3.1.4); 0 when out of bounds
    void trace(Tracer&) override;

private:
    ArrayBufferObject* m_buffer;
    std::size_t m_byte_offset;
    std::optional<std::size_t> m_byte_length; // [[ByteLength]]; absent = auto
};

// A proxy exotic object (§10.5): [[ProxyTarget]] and [[ProxyHandler]], and
// every essential internal method a call to the handler's trap of that
// name — falling through to the target when the handler has no such trap,
// and checked afterwards against what the target itself reports. Those
// invariant checks are the whole point of the object: a target may have
// committed to a non-configurable property, to a prototype, or to being
// non-extensible, and no trap may contradict a commitment already made.
//
// A trap runs script, so the real internal methods all take the
// interpreter. Seven of them have a non-throwing virtual on Object that
// cannot: each is overridden here to answer what the TARGET answers with
// no trap run, and `Interpreter`'s wrappers route a proxy to the throwing
// method beside it — the same shape ModuleNamespaceObject uses for its
// one throwing [[GetOwnProperty]]. A site that reads the virtual sees the
// target through the proxy rather than a trap's answer.
//
// It derives from Function so that a proxy of a callable target can be
// called at all; `is_callable` and `is_constructor` answer for the target
// as it was when the proxy was made, so a proxy of a plain object is not
// callable anywhere. Defined in RuntimeProxy.cpp.
class ProxyObject : public Function {
public:
    ProxyObject(Object* target, Object* handler)
        : Function(nullptr, Class::Proxy)
        , m_target(target)
        , m_handler(handler)
        , m_callable(target->is_callable())
        , m_constructable(target->is_constructor())
    {
    }

    Object* target() const { return m_target; } // [[ProxyTarget]]; null once revoked
    Object* handler() const { return m_handler; } // [[ProxyHandler]]; null once revoked
    bool is_revoked() const { return m_handler == nullptr; }
    // §10.5.15: both slots let go at once. The methods stay present — a
    // revoked callable proxy is still callable, and throws when called.
    void revoke()
    {
        m_target = nullptr;
        m_handler = nullptr;
    }

    bool is_callable() const override { return m_callable; }
    bool is_constructor() const override { return m_constructable; }

    // The essential internal methods, as §10.5 has them: the trap, the
    // fall-through to the target, then the invariant checks. nullopt is a
    // throw throughout; for get_own_property the inner optional is "no
    // such property", and for get_prototype_of a contained null is the
    // null prototype.
    //
    // They are virtual for a host's exotic objects whose internal methods
    // also run code and throw — a browser's WindowProxy and Location — which
    // derive from this class to be routed the same way, and override every
    // one of them.
    virtual std::optional<Object*> get_prototype_of(Interpreter&);
    virtual std::optional<bool> set_prototype_of(Interpreter&, Object* prototype);
    virtual std::optional<bool> is_extensible(Interpreter&);
    virtual std::optional<bool> prevent_extensions(Interpreter&);
    virtual std::optional<std::optional<PropertyDescriptor>> get_own_property(Interpreter&, PropertyKey const&);
    // The object whose own property this one's [[GetOwnProperty]] answers
    // with, unchanged, for this key — a host's window proxy standing for
    // its window — so that a listing may ask that object whether the
    // property is enumerable without anything being made for the answer
    // (Interpreter::own_enumerability). Null where the answer is its own:
    // a proxy with a handler always, since its trap must run.
    virtual Object* stands_for_own_property(Interpreter&, PropertyKey const&) { return nullptr; }
    // The object whose [[Get]] and [[Set]] of any key that is not an index
    // this one's are, with this one as the receiver — a host's window proxy
    // standing for its window, to its own origin — so that the inline
    // caches may answer for it from that object's shape. Null where its
    // own are its own: a proxy with a handler always.
    virtual Object* forwards_named_access(Interpreter&) { return nullptr; }
    virtual std::optional<bool> define_own_property(Interpreter&, PropertyKey const&, PropertyDescriptor const&);
    virtual std::optional<bool> has_property(Interpreter&, PropertyKey const&);
    virtual std::optional<bool> delete_property(Interpreter&, PropertyKey const&);
    virtual std::optional<std::vector<PropertyKey>> own_keys(Interpreter&);

    // [[Get]], [[Set]], [[Call]] and [[Construct]] already take the
    // interpreter on Object and Function, so the override IS the trap.
    std::optional<Value> get(Interpreter&, PropertyKey const&, Value const& receiver) override;
    std::optional<bool> set(Interpreter&, PropertyKey const&, Value const&, Value const& receiver) override;
    std::optional<Value> call(Interpreter&, Value const& this_value, std::span<Value const> arguments) override;
    std::optional<Value> construct(Interpreter&, std::span<Value const> arguments, Object* new_target) override;

    // The non-throwing twins: the target's answer, no trap run. The base
    // names stay reachable for code holding a ProxyObject directly.
    using Object::is_extensible;
    using Object::prevent_extensions;
    std::optional<PropertyDescriptor> get_own_property(PropertyKey const&) const override;
    bool define_own_property(PropertyKey const&, PropertyDescriptor const&) override;
    bool has_property(PropertyKey const&) const override;
    bool delete_property(PropertyKey const&) override;
    std::vector<PropertyKey> own_keys() const override;

    void trace(Tracer&) override;

protected:
    // For a host's exotic object that stands for a different object from
    // one moment to the next (a WindowProxy, when its frame navigates): the
    // one it forwards to, which is also its handler, never a real one.
    void retarget(Object* target)
    {
        m_target = target;
        m_handler = target;
    }

private:
    Object* m_target;
    Object* m_handler;
    bool m_callable;
    bool m_constructable;
};

// A scope (§9.1): the bindings a block, function or script declares, or —
// when made over an object — that object's properties (the global object,
// a `with` target). Closures capture one; the chain runs outward.
class ModuleRecord;

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
        // An indirect binding (§9.1.1.5.5 CreateImportBinding): `import { x }`
        // binds x to the exporting module's binding, live — every read goes
        // through that module's environment, and a write is the TypeError of
        // an immutable binding. `value` is unused then.
        ModuleRecord* import_module = nullptr;
        JsString* import_name = nullptr;
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
    // Room for the bindings a prologue is about to declare, taken once.
    void reserve(std::size_t count) { m_bindings.reserve(count); }
    Binding& declare(JsString* name, Value initial = Value::undefined(), bool mutable_ = true, bool initialized = true, bool deletable = false);
    // CreateImportBinding (§9.1.1.5.5): an immutable, initialised binding
    // whose value is another module's binding, followed at every access.
    Binding& declare_import(JsString* name, ModuleRecord* module, JsString* import_name);
    bool remove(JsString* name); // deletable bindings only
    std::vector<Binding> const& bindings() const { return m_bindings; }
    // A scope's environment as the compiler laid it out: every binding at
    // once, in slot order, so that code reaches one by its index without a
    // search while a lookup by name (eval code, a with) still finds it.
    // Only an environment made this way is read by slot, and nothing is
    // ever removed from one (a removal is an eval's var, which lands in an
    // environment made by name).
    void assign_bindings(std::span<Binding const> bindings);
    std::size_t binding_count() const { return m_bindings.size(); }
    Binding& binding_at(std::size_t slot) { return m_bindings[slot]; }
    Binding const& binding_at(std::size_t slot) const { return m_bindings[slot]; }
    // The slot of a name, or binding_count() when the environment has none.
    std::size_t place_of(JsString const* name) const; // m_bindings.size() for none

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
    // A record that is a VariableEnvironment without being a function's
    // own: the vars of a body whose parameters have expressions
    // (FunctionDeclarationInstantiation step 28), where a direct eval's
    // vars land and past which its conflict scan does not look.
    bool is_var_scope() const { return m_var_scope; }
    void set_var_scope() { m_var_scope = true; }

    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override
    {
        return sizeof(*this) + m_bindings.size() * sizeof(Binding) + m_index.size() * (sizeof(void*) * 4);
    }

private:
    friend struct MachineLayout; // the offsets the machine code reads (Vm.cpp)
    // Past a handful of bindings a scope is found by hash rather than by
    // walking: a bundle's outermost function declares hundreds of names, and
    // every identifier in it would otherwise cost a walk of them all — as
    // would every declaration, which looks for its name first. The index
    // names each binding by its place; it is made the first time a scope
    // that large is searched, kept up as bindings are declared, and made
    // again after one is removed. It covers the first m_indexed bindings;
    // a nameless one (a function's this, new.target and home object, laid
    // out by slot) is covered without an entry.
    static constexpr std::size_t indexed_from = 12;

    std::vector<Binding> m_bindings;
    mutable std::unordered_map<JsString const*, std::uint32_t> m_index;
    mutable std::uint32_t m_indexed = 0;
    Environment* m_outer;
    Object* m_object;
    Value m_this;
    Function* m_function = nullptr;
    Object* m_new_target = nullptr;
    bool m_var_scope = false;
    bool m_has_this = false;
    bool m_this_initialized = true;
    bool m_with = false;
};

}
