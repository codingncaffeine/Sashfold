#include "js/Runtime.h"

// The realm's birth and the object layer of the library: the intrinsic
// objects made in dependency order, then Object (§20.1), Function
// (§20.2), Error and the NativeErrors (§20.5), Symbol (§20.4), Reflect
// (§28.1), and the console the shell listens to. The helpers every
// installer shares are at the top.

#include "js/Ast.h"
#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

// ------------------------------------------------------------ helpers

NativeFunction* define_method(Interpreter& in, Object& target, std::string_view name, int length, NativeFunction::Callback callback)
{
    Heap::NoCollect const guard(in.heap());
    NativeFunction* function = in.new_native(name, length, std::move(callback));
    target.put(in.key(name), Value::object(function), builtin_attributes);
    return function;
}

void define_accessor(Interpreter& in, Object& target, std::string_view name, NativeFunction::Callback getter, NativeFunction::Callback setter)
{
    Heap::NoCollect const guard(in.heap());
    NativeFunction* get = in.new_native("get " + std::string(name), 0, std::move(getter));
    NativeFunction* set = setter ? in.new_native("set " + std::string(name), 1, std::move(setter)) : nullptr;
    target.put_accessor(in.key(name), get, set, Configurable);
}

void define_value(Interpreter& in, Object& target, std::string_view name, Value value, std::uint8_t attributes)
{
    Heap::NoCollect const guard(in.heap());
    target.put(in.key(name), value, attributes);
}

std::optional<JsString*> this_string_value(Interpreter& in, Value const& this_value, std::string_view method)
{
    // RequireObjectCoercible then ToString, the way every
    // String.prototype method starts (§22.1.3).
    if (this_value.is_nullish())
        return in.throw_type_error("String.prototype." + std::string(method) + " called on null or undefined");
    return in.to_string(this_value);
}

std::optional<double> this_number_value(Interpreter& in, Value const& this_value, std::string_view method)
{
    // ThisNumberValue (§21.1.3.7.1).
    if (this_value.is_number())
        return this_value.as_number();
    if (this_value.is_object() && this_value.as_object()->class_id() == Object::Class::Number)
        return static_cast<PrimitiveObject*>(this_value.as_object())->primitive().as_number();
    return in.throw_type_error("Number.prototype." + std::string(method) + " requires that 'this' be a Number");
}

std::optional<bool> this_boolean_value(Interpreter& in, Value const& this_value, std::string_view method)
{
    // ThisBooleanValue (§20.3.3.3.1).
    if (this_value.is_boolean())
        return this_value.as_boolean();
    if (this_value.is_object() && this_value.as_object()->class_id() == Object::Class::Boolean)
        return static_cast<PrimitiveObject*>(this_value.as_object())->primitive().as_boolean();
    return in.throw_type_error("Boolean.prototype." + std::string(method) + " requires that 'this' be a Boolean");
}

namespace {

// A method keyed by a well-known symbol, named "[Symbol.x]" (§10.2.9).
NativeFunction* define_symbol_method(Interpreter& in, Object& target, Symbol* symbol, std::string_view name, int length,
    NativeFunction::Callback callback, std::uint8_t attributes = builtin_attributes)
{
    Heap::NoCollect const guard(in.heap());
    NativeFunction* function = in.new_native(name, length, std::move(callback));
    target.put(PropertyKey::symbol(symbol), Value::object(function), attributes);
    return function;
}

// A constructor: callable and constructable, with `prototype` pointing at
// its prototype object and `constructor` pointing back (§10.2.5 for the
// shape; the built-ins' prototype property is neither writable nor
// configurable).
NativeFunction* define_constructor(Interpreter& in, std::string_view name, int length, Object& prototype,
    NativeFunction::Callback call, NativeFunction::ConstructCallback construct)
{
    Heap::NoCollect const guard(in.heap());
    NativeFunction* function = in.new_native(name, length, std::move(call), std::move(construct));
    function->put(PropertyKey::atom(in.atoms().prototype), Value::object(&prototype), frozen_attributes);
    prototype.put(PropertyKey::atom(in.atoms().constructor), Value::object(function), builtin_attributes);
    in.global()->put(in.key(name), Value::object(function), builtin_attributes);
    return function;
}

std::optional<Object*> require_object(Interpreter& in, Value const& value, std::string_view what)
{
    if (!value.is_object())
        return in.throw_type_error(std::string(what) + " called on non-object");
    return value.as_object();
}

// ToPropertyDescriptor (§6.2.6.5): the fields read through the chain,
// getters included, with the accessor/data conflict refused.
std::optional<PropertyDescriptor> to_property_descriptor(Interpreter& in, Value const& value)
{
    if (!value.is_object())
        return in.throw_type_error("Property description must be an object: " + in.describe(value));
    Interpreter::Roots const roots(in);
    in.root(value);
    Object& object = *value.as_object();
    PropertyDescriptor desc;
    auto const field = [&](JsString* name, auto&& apply) -> bool {
        PropertyKey const key = PropertyKey::atom(name);
        if (!object.has_property(key))
            return true;
        std::optional<Value> const field_value = in.get(object, key);
        if (!field_value)
            return false;
        in.root(*field_value);
        return apply(*field_value);
    };
    WellKnownAtoms const& atoms = in.atoms();
    if (!field(atoms.enumerable, [&](Value const& v) { desc.enumerable = Interpreter::to_boolean(v); return true; }))
        return std::nullopt;
    if (!field(atoms.configurable, [&](Value const& v) { desc.configurable = Interpreter::to_boolean(v); return true; }))
        return std::nullopt;
    if (!field(atoms.value, [&](Value const& v) { desc.value = v; return true; }))
        return std::nullopt;
    if (!field(atoms.writable, [&](Value const& v) { desc.writable = Interpreter::to_boolean(v); return true; }))
        return std::nullopt;
    bool bad_accessor = false;
    std::string bad_text;
    if (!field(atoms.get, [&](Value const& v) {
            if (v.is_undefined()) {
                desc.get = nullptr;
            } else if (Interpreter::is_callable(v)) {
                desc.get = v.as_object();
            } else {
                bad_accessor = true;
                bad_text = "Getter must be a function: " + in.describe(v);
            }
            return true;
        }))
        return std::nullopt;
    if (bad_accessor)
        return in.throw_type_error(bad_text);
    if (!field(atoms.set, [&](Value const& v) {
            if (v.is_undefined()) {
                desc.set = nullptr;
            } else if (Interpreter::is_callable(v)) {
                desc.set = v.as_object();
            } else {
                bad_accessor = true;
                bad_text = "Setter must be a function: " + in.describe(v);
            }
            return true;
        }))
        return std::nullopt;
    if (bad_accessor)
        return in.throw_type_error(bad_text);
    if (desc.is_accessor() && desc.is_data())
        return in.throw_type_error("Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
    return desc;
}

// FromPropertyDescriptor (§6.2.6.4).
Object* from_property_descriptor(Interpreter& in, PropertyDescriptor const& desc)
{
    Heap::NoCollect const guard(in.heap());
    Object* object = in.new_object();
    WellKnownAtoms const& atoms = in.atoms();
    if (desc.value)
        object->put(PropertyKey::atom(atoms.value), *desc.value);
    if (desc.writable)
        object->put(PropertyKey::atom(atoms.writable), Value::boolean(*desc.writable));
    if (desc.get)
        object->put(PropertyKey::atom(atoms.get), *desc.get ? Value::object(*desc.get) : Value::undefined());
    if (desc.set)
        object->put(PropertyKey::atom(atoms.set), *desc.set ? Value::object(*desc.set) : Value::undefined());
    if (desc.enumerable)
        object->put(PropertyKey::atom(atoms.enumerable), Value::boolean(*desc.enumerable));
    if (desc.configurable)
        object->put(PropertyKey::atom(atoms.configurable), Value::boolean(*desc.configurable));
    return object;
}

Value key_to_value(Interpreter& in, PropertyKey const& key)
{
    if (key.is_symbol())
        return Value::symbol(key.as_symbol());
    return Value::string(in.heap().key_to_string(key));
}

// SetIntegrityLevel (§7.3.15): sealed = nothing configurable, frozen =
// nothing writable either.
std::optional<bool> set_integrity_level(Interpreter& in, Object& object, bool frozen)
{
    object.prevent_extensions();
    for (PropertyKey const& key : object.own_keys()) {
        PropertyDescriptor desc;
        desc.configurable = false;
        if (frozen) {
            std::optional<PropertyDescriptor> const current = object.get_own_property(key);
            if (!current)
                continue;
            if (current->is_data())
                desc.writable = false;
        }
        if (!in.define_property_or_throw(object, key, desc))
            return std::nullopt;
    }
    return true;
}

// TestIntegrityLevel (§7.3.16).
bool test_integrity_level(Object const& object, bool frozen)
{
    if (object.is_extensible())
        return false;
    for (PropertyKey const& key : object.own_keys()) {
        std::optional<PropertyDescriptor> const current = object.get_own_property(key);
        if (!current)
            continue;
        if (current->configurable.value_or(false))
            return false;
        if (frozen && current->is_data() && current->writable.value_or(false))
            return false;
    }
    return true;
}

// ObjectDefineProperties (§20.1.2.3.1): every descriptor is read before
// any is applied.
std::optional<bool> object_define_properties(Interpreter& in, Object& object, Value const& properties)
{
    Interpreter::Roots const roots(in);
    std::optional<Object*> const props = in.to_object(properties);
    if (!props)
        return std::nullopt;
    in.root(Value::object(*props));
    std::vector<std::pair<PropertyKey, PropertyDescriptor>> descriptors;
    for (PropertyKey const& key : (*props)->own_keys()) {
        std::optional<PropertyDescriptor> const own = (*props)->get_own_property(key);
        if (!own || !own->enumerable.value_or(false))
            continue;
        std::optional<Value> const descriptor_object = in.get(**props, key);
        if (!descriptor_object)
            return std::nullopt;
        in.root(*descriptor_object);
        std::optional<PropertyDescriptor> const desc = to_property_descriptor(in, *descriptor_object);
        if (!desc)
            return std::nullopt;
        if (desc->value)
            in.root(*desc->value);
        if (desc->get && *desc->get)
            in.root(Value::object(*desc->get));
        if (desc->set && *desc->set)
            in.root(Value::object(*desc->set));
        if (key.is_symbol())
            in.root(Value::symbol(key.as_symbol()));
        descriptors.emplace_back(key, *desc);
    }
    for (auto const& [key, desc] : descriptors) {
        if (!in.define_property_or_throw(object, key, desc))
            return std::nullopt;
    }
    return true;
}

// EnumerableOwnProperties (§7.3.24) in its three flavours.
enum class OwnKind { Keys, Values, Entries };

std::optional<Value> enumerable_own_properties(Interpreter& in, Object& object, OwnKind kind)
{
    Interpreter::Roots const roots(in);
    in.root(Value::object(&object));
    ArrayObject* result = in.new_array();
    in.root(Value::object(result));
    for (PropertyKey const& key : object.own_keys()) {
        if (key.is_symbol())
            continue;
        std::optional<PropertyDescriptor> const desc = object.get_own_property(key);
        if (!desc || !desc->enumerable.value_or(false))
            continue;
        Value const name = Value::string(in.heap().key_to_string(key));
        if (kind == OwnKind::Keys) {
            result->push(name);
            continue;
        }
        std::optional<Value> const value = in.get(object, key);
        if (!value)
            return std::nullopt;
        if (kind == OwnKind::Values) {
            result->push(*value);
            continue;
        }
        in.root(*value);
        Value const pair[2] = { name, *value };
        ArrayObject* entry = in.new_array(pair);
        result->push(Value::object(entry));
    }
    return Value::object(result);
}

Object* ordinary_create_from_constructor(Interpreter& in, Object* new_target, Object* default_prototype, std::optional<Object*>& out)
{
    std::optional<Object*> const prototype = in.get_prototype_from_constructor(new_target, default_prototype);
    if (!prototype) {
        out = std::nullopt;
        return nullptr;
    }
    out = *prototype;
    return *prototype;
}

// --------------------------------------------------------------- Object

void install_object_statics(Interpreter& in, Object& constructor)
{
    define_method(in, constructor, "assign", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        // §20.1.2.1: every enumerable own property of each source, read
        // through Get (so getters run) and written through Set.
        Interpreter::Roots const roots(interp);
        std::optional<Object*> const target = interp.to_object(argument(args, 0));
        if (!target)
            return std::nullopt;
        interp.root(Value::object(*target));
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i].is_nullish())
                continue;
            std::optional<Object*> const from = interp.to_object(args[i]);
            if (!from)
                return std::nullopt;
            interp.root(Value::object(*from));
            for (PropertyKey const& key : (*from)->own_keys()) {
                std::optional<PropertyDescriptor> const desc = (*from)->get_own_property(key);
                if (!desc || !desc->enumerable.value_or(false))
                    continue;
                std::optional<Value> const value = interp.get(**from, key);
                if (!value)
                    return std::nullopt;
                interp.root(*value);
                if (!interp.set(**target, key, *value, true))
                    return std::nullopt;
            }
        }
        return Value::object(*target);
    });
    define_method(in, constructor, "create", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Value const proto = argument(args, 0);
        if (!proto.is_object() && !proto.is_null())
            return interp.throw_type_error("Object prototype may only be an Object or null: " + interp.describe(proto));
        Interpreter::Roots const roots(interp);
        Object* object = interp.new_object(proto.is_null() ? nullptr : proto.as_object());
        if (proto.is_null())
            object->set_prototype(nullptr);
        interp.root(Value::object(object));
        Value const properties = argument(args, 1);
        if (!properties.is_undefined()) {
            if (!object_define_properties(interp, *object, properties))
                return std::nullopt;
        }
        return Value::object(object);
    });
    define_method(in, constructor, "defineProperties", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Object.defineProperties");
        if (!object)
            return std::nullopt;
        if (!object_define_properties(interp, **object, argument(args, 1)))
            return std::nullopt;
        return Value::object(*object);
    });
    define_method(in, constructor, "defineProperty", 3, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Object.defineProperty");
        if (!object)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 1));
        if (!key)
            return std::nullopt;
        if (key->is_symbol())
            interp.root(Value::symbol(key->as_symbol()));
        std::optional<PropertyDescriptor> const desc = to_property_descriptor(interp, argument(args, 2));
        if (!desc)
            return std::nullopt;
        if (!interp.define_property_or_throw(**object, *key, *desc))
            return std::nullopt;
        return Value::object(*object);
    });
    define_method(in, constructor, "entries", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = interp.to_object(argument(args, 0));
        if (!object)
            return std::nullopt;
        return enumerable_own_properties(interp, **object, OwnKind::Entries);
    });
    define_method(in, constructor, "freeze", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        if (!value.is_object())
            return value;
        if (!set_integrity_level(interp, *value.as_object(), true))
            return std::nullopt;
        return value;
    });
    define_method(in, constructor, "fromEntries", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        // §20.1.2.7: AddEntriesFromIterable over the iterable's pairs. A
        // pair that is not an object, or a read that throws, closes the
        // iterator with that error as the outcome.
        Interpreter::Roots const roots(interp);
        Value const iterable = argument(args, 0);
        if (iterable.is_nullish())
            return interp.throw_type_error("Object.fromEntries requires an iterable");
        interp.root(iterable);
        Object* object = interp.new_object();
        interp.root(Value::object(object));
        std::optional<IteratorRecord> record = interp.get_iterator(iterable);
        if (!record)
            return std::nullopt;
        interp.root(record->iterator);
        interp.root(record->next_method);
        while (true) {
            Interpreter::Roots const entry_roots(interp);
            Value entry;
            std::optional<bool> const stepped = interp.iterator_step(*record, entry);
            if (!stepped)
                return std::nullopt;
            if (!*stepped)
                return Value::object(object);
            interp.root(entry);
            if (!entry.is_object()) {
                interp.throw_type_error("Iterator value " + interp.describe(entry) + " is not an entry object");
                interp.iterator_close(*record, true);
                return std::nullopt;
            }
            std::optional<Value> const k = interp.get(*entry.as_object(), PropertyKey::index(0));
            if (!k) {
                interp.iterator_close(*record, true);
                return std::nullopt;
            }
            interp.root(*k);
            std::optional<Value> const v = interp.get(*entry.as_object(), PropertyKey::index(1));
            if (!v) {
                interp.iterator_close(*record, true);
                return std::nullopt;
            }
            interp.root(*v);
            std::optional<PropertyKey> const key = interp.to_property_key(*k);
            if (!key) {
                interp.iterator_close(*record, true);
                return std::nullopt;
            }
            if (!interp.create_data_property(*object, *key, *v)) {
                interp.iterator_close(*record, true);
                return std::nullopt;
            }
        }
    });
    define_method(in, constructor, "getOwnPropertyDescriptor", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<Object*> const object = interp.to_object(argument(args, 0));
        if (!object)
            return std::nullopt;
        interp.root(Value::object(*object));
        std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 1));
        if (!key)
            return std::nullopt;
        std::optional<PropertyDescriptor> const desc = (*object)->get_own_property(*key);
        if (!desc)
            return Value::undefined();
        return Value::object(from_property_descriptor(interp, *desc));
    });
    define_method(in, constructor, "getOwnPropertyDescriptors", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<Object*> const object = interp.to_object(argument(args, 0));
        if (!object)
            return std::nullopt;
        interp.root(Value::object(*object));
        Object* result = interp.new_object();
        interp.root(Value::object(result));
        for (PropertyKey const& key : (*object)->own_keys()) {
            std::optional<PropertyDescriptor> const desc = (*object)->get_own_property(key);
            if (!desc)
                continue;
            Object* descriptor = from_property_descriptor(interp, *desc);
            if (!interp.create_data_property(*result, key, Value::object(descriptor)))
                return std::nullopt;
        }
        return Value::object(result);
    });
    define_method(in, constructor, "getOwnPropertyNames", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<Object*> const object = interp.to_object(argument(args, 0));
        if (!object)
            return std::nullopt;
        interp.root(Value::object(*object));
        ArrayObject* result = interp.new_array();
        interp.root(Value::object(result));
        for (PropertyKey const& key : (*object)->own_keys()) {
            if (!key.is_symbol())
                result->push(key_to_value(interp, key));
        }
        return Value::object(result);
    });
    define_method(in, constructor, "getOwnPropertySymbols", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<Object*> const object = interp.to_object(argument(args, 0));
        if (!object)
            return std::nullopt;
        interp.root(Value::object(*object));
        ArrayObject* result = interp.new_array();
        interp.root(Value::object(result));
        for (PropertyKey const& key : (*object)->own_keys()) {
            if (key.is_symbol())
                result->push(key_to_value(interp, key));
        }
        return Value::object(result);
    });
    define_method(in, constructor, "getPrototypeOf", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = interp.to_object(argument(args, 0));
        if (!object)
            return std::nullopt;
        Object* prototype = (*object)->prototype();
        return prototype ? Value::object(prototype) : Value::null();
    });
    define_method(in, constructor, "hasOwn", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<Object*> const object = interp.to_object(argument(args, 0));
        if (!object)
            return std::nullopt;
        interp.root(Value::object(*object));
        std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 1));
        if (!key)
            return std::nullopt;
        return Value::boolean((*object)->get_own_property(*key).has_value());
    });
    define_method(in, constructor, "is", 2, [](Interpreter&, Value const&, Args args) -> std::optional<Value> {
        return Value::boolean(Interpreter::same_value(argument(args, 0), argument(args, 1)));
    });
    define_method(in, constructor, "isExtensible", 1, [](Interpreter&, Value const&, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        return Value::boolean(value.is_object() && value.as_object()->is_extensible());
    });
    define_method(in, constructor, "isFrozen", 1, [](Interpreter&, Value const&, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        return Value::boolean(!value.is_object() || test_integrity_level(*value.as_object(), true));
    });
    define_method(in, constructor, "isSealed", 1, [](Interpreter&, Value const&, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        return Value::boolean(!value.is_object() || test_integrity_level(*value.as_object(), false));
    });
    define_method(in, constructor, "keys", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = interp.to_object(argument(args, 0));
        if (!object)
            return std::nullopt;
        return enumerable_own_properties(interp, **object, OwnKind::Keys);
    });
    define_method(in, constructor, "preventExtensions", 1, [](Interpreter&, Value const&, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        if (value.is_object())
            value.as_object()->prevent_extensions();
        return value;
    });
    define_method(in, constructor, "seal", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        if (!value.is_object())
            return value;
        if (!set_integrity_level(interp, *value.as_object(), false))
            return std::nullopt;
        return value;
    });
    define_method(in, constructor, "setPrototypeOf", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        Value const proto = argument(args, 1);
        if (value.is_nullish())
            return interp.throw_type_error("Object.setPrototypeOf called on null or undefined");
        if (!proto.is_object() && !proto.is_null())
            return interp.throw_type_error("Object prototype may only be an Object or null: " + interp.describe(proto));
        if (!value.is_object())
            return value;
        if (!value.as_object()->set_prototype(proto.is_null() ? nullptr : proto.as_object())) {
            if (!value.as_object()->is_extensible())
                return interp.throw_type_error("#<Object> is not extensible");
            return interp.throw_type_error("Cyclic __proto__ value");
        }
        return value;
    });
    define_method(in, constructor, "values", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = interp.to_object(argument(args, 0));
        if (!object)
            return std::nullopt;
        return enumerable_own_properties(interp, **object, OwnKind::Values);
    });
}

void install_object_prototype(Interpreter& in, Object& prototype)
{
    define_method(in, prototype, "hasOwnProperty", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §20.1.3.2: the key converts before `this` does.
        Interpreter::Roots const roots(interp);
        std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 0));
        if (!key)
            return std::nullopt;
        if (key->is_symbol())
            interp.root(Value::symbol(key->as_symbol()));
        std::optional<Object*> const object = interp.to_object(this_value);
        if (!object)
            return std::nullopt;
        return Value::boolean((*object)->get_own_property(*key).has_value());
    });
    define_method(in, prototype, "isPrototypeOf", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Value const value = argument(args, 0);
        if (!value.is_object())
            return Value::boolean(false);
        std::optional<Object*> const object = interp.to_object(this_value);
        if (!object)
            return std::nullopt;
        for (Object const* link = value.as_object()->prototype(); link != nullptr; link = link->prototype()) {
            if (link == *object)
                return Value::boolean(true);
        }
        return Value::boolean(false);
    });
    define_method(in, prototype, "propertyIsEnumerable", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 0));
        if (!key)
            return std::nullopt;
        if (key->is_symbol())
            interp.root(Value::symbol(key->as_symbol()));
        std::optional<Object*> const object = interp.to_object(this_value);
        if (!object)
            return std::nullopt;
        std::optional<PropertyDescriptor> const desc = (*object)->get_own_property(*key);
        return Value::boolean(desc && desc->enumerable.value_or(false));
    });
    define_method(in, prototype, "toLocaleString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        return interp.invoke(this_value, PropertyKey::atom(interp.atoms().to_string), {});
    });
    define_method(in, prototype, "toString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §20.1.3.6: the builtin tag from the class, overridden by a
        // string @@toStringTag.
        if (this_value.is_undefined())
            return Value::string(interp.atom("[object Undefined]"));
        if (this_value.is_null())
            return Value::string(interp.atom("[object Null]"));
        Interpreter::Roots const roots(interp);
        std::optional<Object*> const object = interp.to_object(this_value);
        if (!object)
            return std::nullopt;
        interp.root(Value::object(*object));
        std::string tag;
        switch ((*object)->class_id()) {
        case Object::Class::Array: tag = "Array"; break;
        case Object::Class::Arguments: tag = "Arguments"; break;
        case Object::Class::Error: tag = "Error"; break;
        case Object::Class::Boolean: tag = "Boolean"; break;
        case Object::Class::Number: tag = "Number"; break;
        case Object::Class::String: tag = "String"; break;
        case Object::Class::Date: tag = "Date"; break;
        case Object::Class::RegExp: tag = "RegExp"; break;
        default: tag = (*object)->is_callable() ? "Function" : "Object"; break;
        }
        std::optional<Value> const explicit_tag = interp.get(**object, PropertyKey::symbol(interp.atoms().symbol_to_string_tag));
        if (!explicit_tag)
            return std::nullopt;
        if (explicit_tag->is_string())
            tag = explicit_tag->as_string()->to_utf8();
        return Value::string(interp.string(std::string_view("[object " + tag + "]")));
    });
    define_method(in, prototype, "valueOf", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<Object*> const object = interp.to_object(this_value);
        if (!object)
            return std::nullopt;
        return Value::object(*object);
    });
    // Annex B.2.2.1: the __proto__ accessor.
    define_accessor(
        in, prototype, "__proto__",
        [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
            std::optional<Object*> const object = interp.to_object(this_value);
            if (!object)
                return std::nullopt;
            Object* proto = (*object)->prototype();
            return proto ? Value::object(proto) : Value::null();
        },
        [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            if (this_value.is_nullish())
                return interp.throw_type_error("Object.prototype.__proto__ called on null or undefined");
            Value const proto = argument(args, 0);
            if (!proto.is_object() && !proto.is_null())
                return Value::undefined();
            if (!this_value.is_object())
                return Value::undefined();
            if (!this_value.as_object()->set_prototype(proto.is_null() ? nullptr : proto.as_object())) {
                if (!this_value.as_object()->is_extensible())
                    return interp.throw_type_error("#<Object> is not extensible");
                return interp.throw_type_error("Cyclic __proto__ value");
            }
            return Value::undefined();
        });
    // Annex B.2.2.2–5: the legacy accessor definers and lookups.
    auto const define_legacy = [&](std::string_view name, bool getter) {
        define_method(in, prototype, name, 2, [getter](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            Interpreter::Roots const roots(interp);
            std::optional<Object*> const object = interp.to_object(this_value);
            if (!object)
                return std::nullopt;
            interp.root(Value::object(*object));
            Value const function = argument(args, 1);
            if (!Interpreter::is_callable(function))
                return interp.throw_type_error(std::string(getter ? "Getter" : "Setter") + " must be a function: " + interp.describe(function));
            PropertyDescriptor desc;
            if (getter)
                desc.get = function.as_object();
            else
                desc.set = function.as_object();
            desc.enumerable = true;
            desc.configurable = true;
            std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 0));
            if (!key)
                return std::nullopt;
            if (!interp.define_property_or_throw(**object, *key, desc))
                return std::nullopt;
            return Value::undefined();
        });
    };
    define_legacy("__defineGetter__", true);
    define_legacy("__defineSetter__", false);
    auto const lookup_legacy = [&](std::string_view name, bool getter) {
        define_method(in, prototype, name, 1, [getter](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            Interpreter::Roots const roots(interp);
            std::optional<Object*> const object = interp.to_object(this_value);
            if (!object)
                return std::nullopt;
            interp.root(Value::object(*object));
            std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 0));
            if (!key)
                return std::nullopt;
            for (Object const* link = *object; link != nullptr; link = link->prototype()) {
                std::optional<PropertyDescriptor> const desc = link->get_own_property(*key);
                if (!desc)
                    continue;
                if (!desc->is_accessor())
                    return Value::undefined();
                Object* function = getter ? desc->get.value_or(nullptr) : desc->set.value_or(nullptr);
                return function ? Value::object(function) : Value::undefined();
            }
            return Value::undefined();
        });
    };
    lookup_legacy("__lookupGetter__", true);
    lookup_legacy("__lookupSetter__", false);
}

// ------------------------------------------------------------- Function

std::optional<Value> function_to_string(Interpreter& interp, Value const& this_value)
{
    // §20.2.3.5: a script function's own source text; a native's shape.
    if (!Interpreter::is_callable(this_value))
        return interp.throw_type_error("Function.prototype.toString requires that 'this' be a Function");
    Object* function = this_value.as_object();
    if (function->class_id() == Object::Class::Function && dynamic_cast<ScriptFunction*>(function)) {
        FunctionNode const& node = static_cast<ScriptFunction*>(function)->node();
        if (node.program && node.source_end > node.source_start && node.source_end <= node.program->source.size()) {
            std::u16string_view const source = node.program->source;
            return Value::string(interp.string(source.substr(node.source_start, node.source_end - node.source_start)));
        }
    }
    std::string name;
    if (Property const* property = function->find_own(PropertyKey::atom(interp.atoms().name)); property && !property->accessor && property->value.is_string())
        name = property->value.as_string()->to_utf8();
    if (function->class_id() == Object::Class::BoundFunction)
        name.clear();
    return Value::string(interp.string(std::string_view("function " + name + "() { [native code] }")));
}

// ------------------------------------------------------------- Error

std::size_t error_index(ErrorType type)
{
    return static_cast<std::size_t>(type);
}

// The shared body of Error and the NativeErrors (§20.5.1.1, §20.5.6.1.1):
// the object from new.target's prototype, `message` when given, `cause`
// from the options object, and the non-standard `stack`.
std::optional<Value> construct_error(Interpreter& interp, ErrorType type, Args args, Object* new_target)
{
    Interpreter::Roots const roots(interp);
    if (new_target)
        interp.root(Value::object(new_target));
    for (Value const& arg : args)
        interp.root(arg);
    std::optional<Object*> prototype;
    ordinary_create_from_constructor(interp, new_target, interp.intrinsics().error_prototypes[error_index(type)], prototype);
    if (!prototype)
        return std::nullopt;
    auto* error = interp.heap().allocate<ErrorObject>(*prototype);
    interp.root(Value::object(error));
    Value const message = argument(args, 0);
    if (!message.is_undefined()) {
        std::optional<JsString*> const text = interp.to_string(message);
        if (!text)
            return std::nullopt;
        error->put(PropertyKey::atom(interp.atoms().message), Value::string(*text), builtin_attributes);
    }
    Value const options = argument(args, 1);
    if (options.is_object() && options.as_object()->has_property(PropertyKey::atom(interp.atoms().cause))) {
        std::optional<Value> const cause = interp.get(*options.as_object(), PropertyKey::atom(interp.atoms().cause));
        if (!cause)
            return std::nullopt;
        error->put(PropertyKey::atom(interp.atoms().cause), *cause, builtin_attributes);
    }
    std::string const line = interp.describe(Value::object(error));
    error->set_stack(interp.string(std::string_view(line)));
    return Value::object(error);
}

// ------------------------------------------------------------- Symbol

std::optional<Symbol*> this_symbol_value(Interpreter& interp, Value const& this_value, std::string_view method)
{
    if (this_value.is_symbol())
        return this_value.as_symbol();
    if (this_value.is_object() && this_value.as_object()->class_id() == Object::Class::Symbol)
        return static_cast<PrimitiveObject*>(this_value.as_object())->primitive().as_symbol();
    return interp.throw_type_error("Symbol.prototype." + std::string(method) + " requires that 'this' be a Symbol");
}

JsString* symbol_descriptive_string(Interpreter& interp, Symbol const& symbol)
{
    std::u16string text = u"Symbol(";
    if (symbol.description())
        text += symbol.description()->view();
    text += u")";
    return interp.string(std::u16string_view(text));
}

// ------------------------------------------------------------ Reflect

void install_reflect(Interpreter& in)
{
    Heap::NoCollect const guard(in.heap());
    Object* reflect = in.new_object();
    in.global()->put(in.key("Reflect"), Value::object(reflect), builtin_attributes);
    reflect->put(PropertyKey::symbol(in.atoms().symbol_to_string_tag), Value::string(in.atom("Reflect")), Configurable);
    define_method(in, *reflect, "apply", 3, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Value const target = argument(args, 0);
        if (!Interpreter::is_callable(target))
            return interp.throw_type_error("Reflect.apply requires a callable target");
        std::optional<std::vector<Value>> const list = interp.create_list_from_array_like(argument(args, 2));
        if (!list)
            return std::nullopt;
        return interp.call(target, argument(args, 1), *list);
    });
    define_method(in, *reflect, "construct", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Value const target = argument(args, 0);
        if (!Interpreter::is_constructor(target))
            return interp.throw_type_error("Reflect.construct requires a constructor target");
        Value const new_target = args.size() > 2 ? args[2] : target;
        if (!Interpreter::is_constructor(new_target))
            return interp.throw_type_error("Reflect.construct requires a constructor newTarget");
        Interpreter::Roots const roots(interp);
        interp.root(target);
        interp.root(new_target);
        std::optional<std::vector<Value>> const list = interp.create_list_from_array_like(argument(args, 1));
        if (!list)
            return std::nullopt;
        for (Value const& value : *list)
            interp.root(value);
        return static_cast<Function*>(target.as_object())->construct(interp, *list, new_target.as_object());
    });
    define_method(in, *reflect, "defineProperty", 3, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Reflect.defineProperty");
        if (!object)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 1));
        if (!key)
            return std::nullopt;
        if (key->is_symbol())
            interp.root(Value::symbol(key->as_symbol()));
        std::optional<PropertyDescriptor> const desc = to_property_descriptor(interp, argument(args, 2));
        if (!desc)
            return std::nullopt;
        std::optional<bool> const defined = interp.define_own_property(**object, *key, *desc);
        if (!defined)
            return std::nullopt;
        return Value::boolean(*defined);
    });
    define_method(in, *reflect, "deleteProperty", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Reflect.deleteProperty");
        if (!object)
            return std::nullopt;
        std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 1));
        if (!key)
            return std::nullopt;
        return Value::boolean((*object)->delete_property(*key));
    });
    define_method(in, *reflect, "get", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Reflect.get");
        if (!object)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 1));
        if (!key)
            return std::nullopt;
        if (key->is_symbol())
            interp.root(Value::symbol(key->as_symbol()));
        Value const receiver = args.size() > 2 ? args[2] : argument(args, 0);
        return (*object)->get(interp, *key, receiver);
    });
    define_method(in, *reflect, "getOwnPropertyDescriptor", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Reflect.getOwnPropertyDescriptor");
        if (!object)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 1));
        if (!key)
            return std::nullopt;
        std::optional<PropertyDescriptor> const desc = (*object)->get_own_property(*key);
        if (!desc)
            return Value::undefined();
        return Value::object(from_property_descriptor(interp, *desc));
    });
    define_method(in, *reflect, "getPrototypeOf", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Reflect.getPrototypeOf");
        if (!object)
            return std::nullopt;
        Object* prototype = (*object)->prototype();
        return prototype ? Value::object(prototype) : Value::null();
    });
    define_method(in, *reflect, "has", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Reflect.has");
        if (!object)
            return std::nullopt;
        std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 1));
        if (!key)
            return std::nullopt;
        return Value::boolean((*object)->has_property(*key));
    });
    define_method(in, *reflect, "isExtensible", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Reflect.isExtensible");
        if (!object)
            return std::nullopt;
        return Value::boolean((*object)->is_extensible());
    });
    define_method(in, *reflect, "ownKeys", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Reflect.ownKeys");
        if (!object)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        ArrayObject* result = interp.new_array();
        interp.root(Value::object(result));
        for (PropertyKey const& key : (*object)->own_keys())
            result->push(key_to_value(interp, key));
        return Value::object(result);
    });
    define_method(in, *reflect, "preventExtensions", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Reflect.preventExtensions");
        if (!object)
            return std::nullopt;
        (*object)->prevent_extensions();
        return Value::boolean(true);
    });
    define_method(in, *reflect, "set", 3, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Reflect.set");
        if (!object)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        std::optional<PropertyKey> const key = interp.to_property_key(argument(args, 1));
        if (!key)
            return std::nullopt;
        if (key->is_symbol())
            interp.root(Value::symbol(key->as_symbol()));
        Value const receiver = args.size() > 3 ? args[3] : argument(args, 0);
        std::optional<bool> const result = (*object)->set(interp, *key, argument(args, 2), receiver);
        if (!result)
            return std::nullopt;
        return Value::boolean(*result);
    });
    define_method(in, *reflect, "setPrototypeOf", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<Object*> const object = require_object(interp, argument(args, 0), "Reflect.setPrototypeOf");
        if (!object)
            return std::nullopt;
        Value const proto = argument(args, 1);
        if (!proto.is_object() && !proto.is_null())
            return interp.throw_type_error("Object prototype may only be an Object or null: " + interp.describe(proto));
        return Value::boolean((*object)->set_prototype(proto.is_null() ? nullptr : proto.as_object()));
    });
}

// ------------------------------------------------------------ console

void install_console(Interpreter& in)
{
    Heap::NoCollect const guard(in.heap());
    Object* console = in.new_object();
    in.global()->put(in.key("console"), Value::object(console), builtin_attributes);
    for (std::string_view level : { "log", "info", "warn", "error", "debug", "trace" }) {
        define_method(in, *console, level, 0, [level](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
            // Every argument's ToString, space-separated, handed to the
            // shell's listener; a page that logs an object sees its
            // description rather than a throw.
            std::string message;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i > 0)
                    message += ' ';
                if (args[i].is_object() && !args[i].as_object()->is_error()) {
                    std::optional<JsString*> const text = interp.to_string(args[i]);
                    if (!text) {
                        interp.clear_exception();
                        message += interp.describe(args[i]);
                    } else {
                        message += (*text)->to_utf8();
                    }
                } else {
                    message += interp.describe(args[i]);
                }
            }
            if (interp.on_console)
                interp.on_console(level, message);
            return Value::undefined();
        });
    }
}

} // namespace

// -------------------------------------------------------------- Object

void install_object(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    Heap::NoCollect const guard(in.heap());
    NativeFunction* constructor = define_constructor(
        in, "Object", 1, *i.object_prototype,
        [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
            // §20.1.1.1 called as a function: a wrapper or a new object.
            Value const value = argument(args, 0);
            if (value.is_nullish())
                return Value::object(interp.new_object());
            std::optional<Object*> const object = interp.to_object(value);
            if (!object)
                return std::nullopt;
            return Value::object(*object);
        },
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            if (new_target != nullptr && new_target != interp.intrinsics().object_constructor) {
                std::optional<Object*> prototype;
                ordinary_create_from_constructor(interp, new_target, interp.intrinsics().object_prototype, prototype);
                if (!prototype)
                    return std::nullopt;
                return Value::object(interp.new_object(*prototype));
            }
            Value const value = argument(args, 0);
            if (value.is_nullish())
                return Value::object(interp.new_object());
            std::optional<Object*> const object = interp.to_object(value);
            if (!object)
                return std::nullopt;
            return Value::object(*object);
        });
    i.object_constructor = constructor;
    install_object_statics(in, *constructor);
    install_object_prototype(in, *i.object_prototype);
}

// ------------------------------------------------------------ Function

void install_function(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    Heap::NoCollect const guard(in.heap());
    Object& prototype = *i.function_prototype;
    NativeFunction* constructor = define_constructor(
        in, "Function", 1, prototype,
        [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
            return interp.intrinsics().function_constructor->construct(interp, args, nullptr);
        },
        [](Interpreter& interp, Args args, Object*) -> std::optional<Value> {
            // CreateDynamicFunction (§20.2.1.1.1): the last argument is
            // the body, the rest join as the parameter list.
            Interpreter::Roots const roots(interp);
            std::u16string parameters;
            std::u16string body;
            for (std::size_t k = 0; k < args.size(); ++k) {
                std::optional<JsString*> const text = interp.to_string(args[k]);
                if (!text)
                    return std::nullopt;
                interp.root(Value::string(*text));
                if (k + 1 == args.size()) {
                    body = (*text)->data();
                } else {
                    if (k > 0)
                        parameters += u",";
                    parameters += (*text)->data();
                }
            }
            return interp.compile_function(parameters, body);
        });
    i.function_constructor = constructor;

    define_method(in, prototype, "apply", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §20.2.3.1.
        if (!Interpreter::is_callable(this_value))
            return interp.throw_type_error("Function.prototype.apply was called on " + interp.describe(this_value) + ", which is not a function");
        Value const arguments_value = argument(args, 1);
        if (arguments_value.is_nullish())
            return interp.call(this_value, argument(args, 0), {});
        std::optional<std::vector<Value>> const list = interp.create_list_from_array_like(arguments_value);
        if (!list)
            return std::nullopt;
        return interp.call(this_value, argument(args, 0), *list);
    });
    define_method(in, prototype, "bind", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §20.2.3.2 with BoundFunctionCreate (§10.4.1.3): the bound
        // function's length is what remains of the target's, its name
        // the target's with "bound " in front.
        if (!Interpreter::is_callable(this_value))
            return interp.throw_type_error("Bind must be called on a function");
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        auto* target = static_cast<Function*>(this_value.as_object());
        std::vector<Value> bound_arguments;
        for (std::size_t k = 1; k < args.size(); ++k) {
            interp.root(args[k]);
            bound_arguments.push_back(args[k]);
        }
        auto* bound = interp.heap().allocate<BoundFunction>(target->prototype(), target, argument(args, 0), std::move(bound_arguments));
        interp.root(Value::object(bound));
        double length = 0;
        if (target->get_own_property(PropertyKey::atom(interp.atoms().length))) {
            std::optional<Value> const target_length = interp.get(*target, PropertyKey::atom(interp.atoms().length));
            if (!target_length)
                return std::nullopt;
            if (target_length->is_number()) {
                double const number = target_length->as_number();
                if (number == std::numeric_limits<double>::infinity())
                    length = number;
                else if (number == -std::numeric_limits<double>::infinity())
                    length = 0;
                else
                    length = std::max(0.0, Interpreter::to_integer_or_infinity(number) - static_cast<double>(args.empty() ? 0 : args.size() - 1));
            }
        }
        bound->put(PropertyKey::atom(interp.atoms().length), Value::number(length), Configurable);
        std::optional<Value> const target_name = interp.get(*target, PropertyKey::atom(interp.atoms().name));
        if (!target_name)
            return std::nullopt;
        std::u16string name = u"bound ";
        if (target_name->is_string())
            name += target_name->as_string()->view();
        bound->put(PropertyKey::atom(interp.atoms().name), Value::string(interp.string(std::u16string_view(name))), Configurable);
        return Value::object(bound);
    });
    define_method(in, prototype, "call", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        if (!Interpreter::is_callable(this_value))
            return interp.throw_type_error("Function.prototype.call was called on " + interp.describe(this_value) + ", which is not a function");
        std::span<Value const> const rest = args.size() > 1 ? args.subspan(1) : std::span<Value const>();
        return interp.call(this_value, argument(args, 0), rest);
    });
    define_method(in, prototype, "toString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        return function_to_string(interp, this_value);
    });
    define_symbol_method(
        in, prototype, in.atoms().symbol_has_instance, "[Symbol.hasInstance]", 1,
        [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            std::optional<bool> const result = interp.ordinary_has_instance(this_value, argument(args, 0));
            if (!result)
                return std::nullopt;
            return Value::boolean(*result);
        },
        frozen_attributes);
    // AddRestrictedFunctionProperties (§10.2.4): caller and arguments
    // throw through %ThrowTypeError%.
    prototype.put_accessor(PropertyKey::atom(in.atoms().caller), i.throw_type_error, i.throw_type_error, Configurable);
    prototype.put_accessor(PropertyKey::atom(in.atoms().arguments), i.throw_type_error, i.throw_type_error, Configurable);
}

// --------------------------------------------------------------- Error

void install_error(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    Heap::NoCollect const guard(in.heap());
    struct Spec {
        ErrorType type;
        std::string_view name;
    };
    constexpr Spec specs[] = {
        { ErrorType::Error, "Error" },
        { ErrorType::EvalError, "EvalError" },
        { ErrorType::RangeError, "RangeError" },
        { ErrorType::ReferenceError, "ReferenceError" },
        { ErrorType::SyntaxError, "SyntaxError" },
        { ErrorType::TypeError, "TypeError" },
        { ErrorType::UriError, "URIError" },
    };
    for (Spec const& spec : specs) {
        ErrorType const type = spec.type;
        Object& prototype = *i.error_prototypes[error_index(type)];
        NativeFunction* constructor = define_constructor(
            in, spec.name, 1, prototype,
            [type](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
                // §20.5.1.1 step 1: called as a function, new.target is
                // the constructor itself.
                return construct_error(interp, type, args, interp.intrinsics().error_constructors[error_index(type)]);
            },
            [type](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
                return construct_error(interp, type, args, new_target);
            });
        i.error_constructors[error_index(type)] = constructor;
        if (type == ErrorType::Error) {
            i.error_constructor = constructor;
        } else {
            // §20.5.6.2: a NativeError constructor inherits from %Error%.
            constructor->set_prototype(i.error_constructor);
        }
        prototype.put(PropertyKey::atom(in.atoms().name), Value::string(in.atom(spec.name)), builtin_attributes);
        prototype.put(PropertyKey::atom(in.atoms().message), Value::string(in.atoms().empty), builtin_attributes);
    }
    // The error-stack accessor: `stack` reads the text captured at
    // construction, and a write lands as an own data property of the
    // receiver (SetterThatIgnoresPrototypeProperties), never on the
    // prototype itself.
    define_accessor(
        in, *i.error_prototype, "stack",
        [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
            if (!this_value.is_object())
                return interp.throw_type_error("Error.prototype.stack getter called on non-object");
            Object* object = this_value.as_object();
            if (!object->is_error())
                return Value::undefined();
            JsString* stack = static_cast<ErrorObject*>(object)->stack();
            return Value::string(stack ? stack : interp.atoms().empty);
        },
        [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
            if (!this_value.is_object())
                return interp.throw_type_error("Error.prototype.stack setter called on non-object");
            Value const value = argument(args, 0);
            if (!value.is_string())
                return interp.throw_type_error("Error.prototype.stack setter called with a non-string value");
            Object* object = this_value.as_object();
            if (object == interp.intrinsics().error_prototype)
                return interp.throw_type_error("Error.prototype.stack setter called on Error.prototype itself");
            Interpreter::Roots const roots(interp);
            interp.root(this_value);
            interp.root(value);
            PropertyKey const key = PropertyKey::atom(interp.atoms().stack);
            if (!object->get_own_property(key)) {
                if (!interp.create_data_property(*object, key, value))
                    return std::nullopt;
            } else if (!interp.set(*object, key, value, true)) {
                return std::nullopt;
            }
            return Value::undefined();
        });
    define_method(in, *i.error_prototype, "toString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §20.5.3.4.
        if (!this_value.is_object())
            return interp.throw_type_error("Error.prototype.toString requires that 'this' be an Object");
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<Value> const name_value = interp.get(this_value, PropertyKey::atom(interp.atoms().name));
        if (!name_value)
            return std::nullopt;
        interp.root(*name_value);
        std::u16string name = u"Error";
        if (!name_value->is_undefined()) {
            std::optional<JsString*> const text = interp.to_string(*name_value);
            if (!text)
                return std::nullopt;
            name = (*text)->data();
        }
        std::optional<Value> const message_value = interp.get(this_value, PropertyKey::atom(interp.atoms().message));
        if (!message_value)
            return std::nullopt;
        interp.root(*message_value);
        std::u16string message;
        if (!message_value->is_undefined()) {
            std::optional<JsString*> const text = interp.to_string(*message_value);
            if (!text)
                return std::nullopt;
            message = (*text)->data();
        }
        if (name.empty())
            return Value::string(interp.string(std::u16string_view(message)));
        if (message.empty())
            return Value::string(interp.string(std::u16string_view(name)));
        return Value::string(interp.string(std::u16string_view(name + u": " + message)));
    });
}

// -------------------------------------------------------------- Symbol

void install_symbol(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    Heap::NoCollect const guard(in.heap());
    Object& prototype = *i.symbol_prototype;
    NativeFunction* constructor = define_constructor(
        in, "Symbol", 0, prototype,
        [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
            // §20.4.1.1: a fresh symbol with the description's string.
            Value const description = argument(args, 0);
            JsString* text = nullptr;
            if (!description.is_undefined()) {
                std::optional<JsString*> const converted = interp.to_string(description);
                if (!converted)
                    return std::nullopt;
                text = *converted;
            }
            return Value::symbol(interp.heap().symbol(text));
        },
        [](Interpreter& interp, Args, Object*) -> std::optional<Value> {
            return interp.throw_type_error("Symbol is not a constructor");
        });
    i.symbol_constructor = constructor;
    define_method(in, *constructor, "for", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        // §20.4.2.2: one symbol per key, kept in the realm's registry.
        std::optional<JsString*> const key_string = interp.to_string(argument(args, 0));
        if (!key_string)
            return std::nullopt;
        Object& registry = *interp.intrinsics().symbol_registry;
        PropertyKey const key = interp.heap().key(*key_string);
        if (Property const* existing = registry.find_own(key))
            return existing->value;
        Heap::NoCollect const no_collect(interp.heap());
        Symbol* symbol = interp.heap().symbol(interp.heap().atom(*key_string));
        registry.put(key, Value::symbol(symbol));
        return Value::symbol(symbol);
    });
    define_method(in, *constructor, "keyFor", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        Value const symbol = argument(args, 0);
        if (!symbol.is_symbol())
            return interp.throw_type_error(interp.describe(symbol) + " is not a symbol");
        for (Property const& property : interp.intrinsics().symbol_registry->properties()) {
            if (property.value == symbol)
                return Value::string(interp.heap().key_to_string(property.key));
        }
        return Value::undefined();
    });
    struct WellKnown {
        std::string_view name;
        Symbol* symbol;
    };
    WellKnownAtoms const& atoms = in.atoms();
    WellKnown const well_known[] = {
        { "iterator", atoms.symbol_iterator },
        { "toPrimitive", atoms.symbol_to_primitive },
        { "toStringTag", atoms.symbol_to_string_tag },
        { "hasInstance", atoms.symbol_has_instance },
        { "isConcatSpreadable", atoms.symbol_is_concat_spreadable },
        { "match", atoms.symbol_match },
        { "matchAll", atoms.symbol_match_all },
        { "replace", atoms.symbol_replace },
        { "search", atoms.symbol_search },
        { "split", atoms.symbol_split },
        { "species", atoms.symbol_species },
        { "unscopables", atoms.symbol_unscopables },
        { "asyncIterator", atoms.symbol_async_iterator },
    };
    for (WellKnown const& entry : well_known)
        constructor->put(in.key(entry.name), Value::symbol(entry.symbol), frozen_attributes);

    define_method(in, prototype, "toString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<Symbol*> const symbol = this_symbol_value(interp, this_value, "toString");
        if (!symbol)
            return std::nullopt;
        return Value::string(symbol_descriptive_string(interp, **symbol));
    });
    define_method(in, prototype, "valueOf", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<Symbol*> const symbol = this_symbol_value(interp, this_value, "valueOf");
        if (!symbol)
            return std::nullopt;
        return Value::symbol(*symbol);
    });
    define_accessor(in, prototype, "description", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<Symbol*> const symbol = this_symbol_value(interp, this_value, "description");
        if (!symbol)
            return std::nullopt;
        JsString* description = (*symbol)->description();
        return description ? Value::string(description) : Value::undefined();
    });
    define_symbol_method(
        in, prototype, atoms.symbol_to_primitive, "[Symbol.toPrimitive]", 1,
        [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
            std::optional<Symbol*> const symbol = this_symbol_value(interp, this_value, "[Symbol.toPrimitive]");
            if (!symbol)
                return std::nullopt;
            return Value::symbol(*symbol);
        },
        Configurable);
    prototype.put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("Symbol")), Configurable);
}

// ----------------------------------------------------------- the realm

void install_intrinsics(Interpreter& in)
{
    // §9.3.2 CreateIntrinsics, in dependency order: every prototype
    // exists before any constructor or method, since the makers reach
    // for them, and the whole build defers collection — nothing made
    // here is garbage, and rooting each of a few hundred cells by hand
    // would only be a way to forget one.
    Heap& heap = in.heap();
    Heap::NoCollect const guard(heap);
    Intrinsics& i = in.intrinsics();
    WellKnownAtoms const& atoms = in.atoms();

    i.object_prototype = heap.allocate<Object>(nullptr);
    i.function_prototype = heap.allocate<NativeFunction>(i.object_prototype, [](Interpreter&, Value const&, Args) -> std::optional<Value> {
        return Value::undefined();
    });
    i.function_prototype->put(PropertyKey::atom(atoms.length), Value::number(0), Configurable);
    i.function_prototype->put(PropertyKey::atom(atoms.name), Value::string(atoms.empty), Configurable);
    // §23.1.3: Array.prototype is itself an Array exotic object; §22.1.3:
    // String.prototype a String one, wrapping the empty string.
    i.array_prototype = heap.allocate<ArrayObject>(i.object_prototype);
    i.string_prototype = heap.allocate<StringObject>(i.object_prototype, atoms.empty);
    i.number_prototype = heap.allocate<PrimitiveObject>(i.object_prototype, Object::Class::Number, Value::number(0));
    i.boolean_prototype = heap.allocate<PrimitiveObject>(i.object_prototype, Object::Class::Boolean, Value::boolean(false));
    i.symbol_prototype = heap.allocate<Object>(i.object_prototype);
    i.error_prototype = heap.allocate<Object>(i.object_prototype);
    for (std::size_t k = 0; k < 7; ++k)
        i.error_prototypes[k] = k == 0 ? i.error_prototype : heap.allocate<Object>(i.error_prototype);
    i.date_prototype = heap.allocate<Object>(i.object_prototype);
    i.regexp_prototype = heap.allocate<Object>(i.object_prototype);
    i.arguments_prototype = i.object_prototype;
    i.symbol_registry = heap.allocate<Object>(nullptr);

    // The global object and the object environment over it (§9.1.1.4);
    // the evaluator adds the declarative record for lexicals in front.
    i.global = heap.allocate<Object>(i.object_prototype, Object::Class::Global);
    i.global_environment = heap.allocate<Environment>(nullptr, i.global);
    i.global_environment->set_this(Value::object(i.global));

    // %ThrowTypeError% (§10.2.4.1): frozen, and its length and name are
    // not even configurable.
    i.throw_type_error = in.new_native("", 0, [](Interpreter& interp, Value const&, Args) -> std::optional<Value> {
        return interp.throw_type_error("'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions or the arguments objects for calls to them");
    });
    i.throw_type_error->put(PropertyKey::atom(atoms.length), Value::number(0), frozen_attributes);
    i.throw_type_error->put(PropertyKey::atom(atoms.name), Value::string(atoms.empty), frozen_attributes);
    i.throw_type_error->prevent_extensions();

    install_object(in);
    install_function(in);
    install_error(in);
    install_symbol(in);
    install_boolean(in);
    install_number(in);
    install_math(in);
    install_global_functions(in);
    install_array(in);
    install_string(in);
    install_regexp(in);
    install_json(in);
    install_date(in);
    install_iterators(in);
    install_reflect(in);
    install_console(in);
}

}
