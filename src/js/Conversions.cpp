#include "js/Interpreter.h"

// The abstract operations of §7 — type conversion, testing and comparison,
// the operations on objects — plus the interpreter's makers and throwers.
// Everything that can run script returns std::optional; the pieces that
// cannot are static. The evaluator itself lives in Interpreter.cpp.

#include "js/Object.h"
#include "js/Runtime.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
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

namespace {

constexpr double max_safe_integer = 9007199254740991.0; // 2^53 − 1
constexpr double two_to_the_32 = 4294967296.0;

std::size_t error_index(ErrorType type)
{
    return static_cast<std::size_t>(type);
}

// A data property's value found along the prototype chain without
// running script: an accessor stops the search, since reading it would.
std::optional<Value> data_property_up_chain(Object const& object, PropertyKey const& key)
{
    for (Object const* link = &object; link != nullptr; link = link->prototype()) {
        Property const* property = link->find_own(key);
        if (property == nullptr)
            continue;
        if (property->accessor)
            return std::nullopt;
        return property->value;
    }
    return std::nullopt;
}

std::string class_name(Object const& object)
{
    switch (object.class_id()) {
    case Object::Class::Object: return "Object";
    case Object::Class::Array: return "Array";
    case Object::Class::Function: return "Function";
    case Object::Class::BoundFunction: return "Function";
    case Object::Class::Error: return "Error";
    case Object::Class::Boolean: return "Boolean";
    case Object::Class::Number: return "Number";
    case Object::Class::String: return "String";
    case Object::Class::Symbol: return "Symbol";
    case Object::Class::Date: return "Date";
    case Object::Class::RegExp: return "RegExp";
    case Object::Class::Arguments: return "Arguments";
    case Object::Class::ArrayIterator: return "Array Iterator";
    case Object::Class::StringIterator: return "String Iterator";
    case Object::Class::Math: return "Math";
    case Object::Class::Json: return "JSON";
    case Object::Class::Global: return "global";
    case Object::Class::Host: return "Object";
    }
    return "Object";
}

} // namespace

// ------------------------------------------------------- shared helpers

std::string key_description(PropertyKey const& key)
{
    switch (key.kind()) {
    case PropertyKey::Kind::Atom:
        return key.as_atom()->to_utf8();
    case PropertyKey::Kind::Index:
        return std::to_string(key.as_index());
    case PropertyKey::Kind::Symbol: {
        JsString const* description = key.as_symbol()->description();
        return "Symbol(" + (description ? description->to_utf8() : std::string()) + ")";
    }
    }
    return "";
}

std::string value_description(Interpreter& interpreter, Value const& value)
{
    return interpreter.describe(value);
}

// ----------------------------------------------------------- throwing

std::nullopt_t Interpreter::throw_value(Value value)
{
    m_exception = value;
    m_has_exception = true;
    return std::nullopt;
}

std::nullopt_t Interpreter::throw_error(ErrorType type, std::string_view message)
{
    return throw_value(Value::object(new_error(type, message)));
}

Value Interpreter::take_exception()
{
    Value const value = m_exception;
    clear_exception();
    return value;
}

void Interpreter::clear_exception()
{
    m_exception = Value::undefined();
    m_has_exception = false;
}

// ------------------------------------------------------------- makers

Object* Interpreter::new_object(Object* prototype)
{
    return m_heap->allocate<Object>(prototype ? prototype : m_intrinsics.object_prototype);
}

ArrayObject* Interpreter::new_array(std::span<Value const> elements)
{
    return m_heap->allocate<ArrayObject>(m_intrinsics.array_prototype, elements);
}

NativeFunction* Interpreter::new_native(std::string_view name, int length, NativeFunction::Callback call,
    NativeFunction::ConstructCallback construct)
{
    // CreateBuiltinFunction (§10.3.3): `length` first, then `name`, both
    // read-only and hidden from enumeration. The atom for the name may be
    // new, so nothing may collect until the function holds it.
    Heap::NoCollect const guard(*m_heap);
    auto* function = m_heap->allocate<NativeFunction>(m_intrinsics.function_prototype, std::move(call), std::move(construct));
    function->put(PropertyKey::atom(atoms().length), Value::number(static_cast<double>(length)), Configurable);
    function->put(PropertyKey::atom(atoms().name), Value::string(m_heap->atom(name)), Configurable);
    return function;
}

Object* Interpreter::new_error(ErrorType type, std::string_view message)
{
    Heap::NoCollect const guard(*m_heap);
    return new_error(type, m_heap->string(message));
}

Object* Interpreter::new_error(ErrorType type, JsString* message)
{
    // The error's own `message` (§20.5.1.1 step 3) and the non-standard
    // `stack` every engine gives it — here the "Name: message" line alone,
    // since the tree-walker keeps no frame list yet.
    Heap::NoCollect const guard(*m_heap);
    auto* error = m_heap->allocate<ErrorObject>(m_intrinsics.error_prototypes[error_index(type)]);
    if (message)
        error->put(PropertyKey::atom(atoms().message), Value::string(message), builtin_attributes);
    std::string const line = describe(Value::object(error));
    error->set_stack(m_heap->string(std::string_view(line)));
    return error;
}

// -------------------------------------------------------------- typeof

JsString* Interpreter::type_of(Value const& value)
{
    // §13.5.3 table 41: a callable object is "function", every other
    // object — null among them — is "object".
    switch (value.type()) {
    case Value::Type::Undefined:
        return atoms().undefined;
    case Value::Type::Null:
        return atoms().object;
    case Value::Type::Boolean:
        return atoms().boolean;
    case Value::Type::Number:
        return atoms().number;
    case Value::Type::String:
        return atoms().string;
    case Value::Type::Symbol:
        return atoms().symbol;
    case Value::Type::Object:
        return value.as_object()->is_callable() ? atoms().function : atoms().object;
    case Value::Type::Empty:
        break;
    }
    return atoms().undefined;
}

// --------------------------------------------------- type conversion §7.1

bool Interpreter::to_boolean(Value const& value)
{
    // §7.1.2: only the falsy set fails — undefined, null, false, ±0, NaN
    // and the empty string; every object and every symbol is true.
    switch (value.type()) {
    case Value::Type::Undefined:
    case Value::Type::Null:
    case Value::Type::Empty:
        return false;
    case Value::Type::Boolean:
        return value.as_boolean();
    case Value::Type::Number:
        return value.as_number() != 0 && !std::isnan(value.as_number());
    case Value::Type::String:
        return !value.as_string()->is_empty();
    case Value::Type::Symbol:
    case Value::Type::Object:
        return true;
    }
    return false;
}

std::optional<Value> Interpreter::to_primitive(Value const& input, PreferredType hint)
{
    // §7.1.1: an object asks its @@toPrimitive first, with the hint
    // spelled out; a method that answers with an object is a TypeError.
    // Without one, OrdinaryToPrimitive with "default" read as "number".
    if (!input.is_object())
        return input;
    Roots const roots(*this);
    root(input);
    std::optional<Value> const exotic = get_method(input, PropertyKey::symbol(atoms().symbol_to_primitive));
    if (!exotic)
        return std::nullopt;
    if (!exotic->is_undefined()) {
        root(*exotic);
        std::string_view const name = hint == PreferredType::Default ? "default" : hint == PreferredType::Number ? "number" : "string";
        Value const arguments[1] = { Value::string(m_heap->atom(name)) };
        std::optional<Value> const result = call(*exotic, input, arguments);
        if (!result)
            return std::nullopt;
        if (result->is_object())
            return throw_type_error("Cannot convert object to primitive value");
        return *result;
    }
    return ordinary_to_primitive(*input.as_object(), hint == PreferredType::Default ? PreferredType::Number : hint);
}

std::optional<Value> Interpreter::ordinary_to_primitive(Object& object, PreferredType hint)
{
    // §7.1.1.1: valueOf then toString, or the reverse for a string hint;
    // the first callable one whose answer is not an object wins.
    JsString* const order[2] = {
        hint == PreferredType::String ? atoms().to_string : atoms().value_of,
        hint == PreferredType::String ? atoms().value_of : atoms().to_string,
    };
    Roots const roots(*this);
    Value const self = Value::object(&object);
    root(self);
    for (JsString* name : order) {
        std::optional<Value> const method = get(self, PropertyKey::atom(name));
        if (!method)
            return std::nullopt;
        if (!is_callable(*method))
            continue;
        root(*method);
        std::optional<Value> const result = call(*method, self, {});
        if (!result)
            return std::nullopt;
        if (!result->is_object())
            return *result;
    }
    return throw_type_error("Cannot convert object to primitive value");
}

std::optional<double> Interpreter::to_number(Value const& value)
{
    // §7.1.4.
    switch (value.type()) {
    case Value::Type::Undefined:
    case Value::Type::Empty:
        return std::numeric_limits<double>::quiet_NaN();
    case Value::Type::Null:
        return 0.0;
    case Value::Type::Boolean:
        return value.as_boolean() ? 1.0 : 0.0;
    case Value::Type::Number:
        return value.as_number();
    case Value::Type::String:
        return string_to_number(value.as_string()->view());
    case Value::Type::Symbol:
        return throw_type_error("Cannot convert a Symbol value to a number");
    case Value::Type::Object: {
        std::optional<Value> const primitive = to_primitive(value, PreferredType::Number);
        if (!primitive)
            return std::nullopt;
        return to_number(*primitive);
    }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

double Interpreter::to_integer_or_infinity(double number)
{
    // §7.1.5: NaN becomes 0, the infinities stay, the rest truncate; −0
    // comes out as +0.
    if (std::isnan(number))
        return 0.0;
    if (std::isinf(number))
        return number;
    double const truncated = std::trunc(number);
    return truncated == 0 ? 0.0 : truncated;
}

std::optional<double> Interpreter::to_integer_or_infinity(Value const& value)
{
    std::optional<double> const number = to_number(value);
    if (!number)
        return std::nullopt;
    return to_integer_or_infinity(*number);
}

std::int32_t Interpreter::double_to_int32(double number)
{
    // §7.1.6: modulo 2^32 on the truncated value, then the high bit read
    // as a sign. The unsigned-to-signed step is the modular conversion
    // the language guarantees.
    if (std::isnan(number) || std::isinf(number))
        return 0;
    double modulo = std::fmod(std::trunc(number), two_to_the_32);
    if (modulo < 0)
        modulo += two_to_the_32;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(modulo));
}

std::uint32_t Interpreter::double_to_uint32(double number)
{
    // §7.1.7.
    if (std::isnan(number) || std::isinf(number))
        return 0;
    double modulo = std::fmod(std::trunc(number), two_to_the_32);
    if (modulo < 0)
        modulo += two_to_the_32;
    return static_cast<std::uint32_t>(modulo);
}

std::optional<std::int32_t> Interpreter::to_int32(Value const& value)
{
    std::optional<double> const number = to_number(value);
    if (!number)
        return std::nullopt;
    return double_to_int32(*number);
}

std::optional<std::uint32_t> Interpreter::to_uint32(Value const& value)
{
    std::optional<double> const number = to_number(value);
    if (!number)
        return std::nullopt;
    return double_to_uint32(*number);
}

std::optional<double> Interpreter::to_length(Value const& value)
{
    // §7.1.20: clamped into 0 … 2^53 − 1.
    std::optional<double> const integer = to_integer_or_infinity(value);
    if (!integer)
        return std::nullopt;
    if (*integer <= 0)
        return 0.0;
    return std::min(*integer, max_safe_integer);
}

std::optional<double> Interpreter::to_index(Value const& value)
{
    // §7.1.22: undefined is 0; anything outside 0 … 2^53 − 1 is a
    // RangeError rather than a clamp.
    if (value.is_undefined())
        return 0.0;
    std::optional<double> const integer = to_integer_or_infinity(value);
    if (!integer)
        return std::nullopt;
    if (*integer < 0 || *integer > max_safe_integer)
        return throw_range_error("Invalid index");
    return *integer;
}

std::optional<JsString*> Interpreter::to_string(Value const& value)
{
    // §7.1.17.
    switch (value.type()) {
    case Value::Type::Undefined:
    case Value::Type::Empty:
        return atoms().undefined;
    case Value::Type::Null:
        return atoms().null;
    case Value::Type::Boolean:
        return value.as_boolean() ? atoms().true_ : atoms().false_;
    case Value::Type::Number: {
        double const number = value.as_number();
        if (std::isnan(number))
            return atoms().nan;
        if (number == 0)
            return atoms().zero;
        if (std::isinf(number))
            return number > 0 ? atoms().infinity : atoms().negative_infinity;
        return m_heap->string(number_to_string(number));
    }
    case Value::Type::String:
        return value.as_string();
    case Value::Type::Symbol:
        return throw_type_error("Cannot convert a Symbol value to a string");
    case Value::Type::Object: {
        std::optional<Value> const primitive = to_primitive(value, PreferredType::String);
        if (!primitive)
            return std::nullopt;
        return to_string(*primitive);
    }
    }
    return atoms().undefined;
}

std::optional<Object*> Interpreter::to_object(Value const& value)
{
    // §7.1.18: a primitive is boxed in a wrapper whose prototype is the
    // realm's; strings get the exotic wrapper with indexed code units.
    switch (value.type()) {
    case Value::Type::Undefined:
    case Value::Type::Null:
    case Value::Type::Empty:
        return throw_type_error("Cannot convert undefined or null to object");
    case Value::Type::Boolean:
        return m_heap->allocate<PrimitiveObject>(m_intrinsics.boolean_prototype, Object::Class::Boolean, value);
    case Value::Type::Number:
        return m_heap->allocate<PrimitiveObject>(m_intrinsics.number_prototype, Object::Class::Number, value);
    case Value::Type::String:
        return m_heap->allocate<StringObject>(m_intrinsics.string_prototype, value.as_string());
    case Value::Type::Symbol:
        return m_heap->allocate<PrimitiveObject>(m_intrinsics.symbol_prototype, Object::Class::Symbol, value);
    case Value::Type::Object:
        return value.as_object();
    }
    return throw_type_error("Cannot convert undefined or null to object");
}

std::optional<PropertyKey> Interpreter::to_property_key(Value const& value)
{
    // §7.1.19: a primitive with a string hint, a symbol kept as itself;
    // the heap decides between an atom and an array index.
    switch (value.type()) {
    case Value::Type::String:
        return m_heap->key(value.as_string());
    case Value::Type::Symbol:
        return PropertyKey::symbol(value.as_symbol());
    case Value::Type::Number:
        return m_heap->key(value.as_number());
    default:
        break;
    }
    std::optional<Value> const primitive = to_primitive(value, PreferredType::String);
    if (!primitive)
        return std::nullopt;
    if (primitive->is_symbol())
        return PropertyKey::symbol(primitive->as_symbol());
    std::optional<JsString*> const string = to_string(*primitive);
    if (!string)
        return std::nullopt;
    return m_heap->key(*string);
}

// ---------------------------------------------- operations on objects §7.3

std::optional<Value> Interpreter::get(Value const& base, PropertyKey const& key)
{
    // GetV (§7.3.3): a primitive reads through its wrapper's prototype
    // with itself as the receiver, so a getter sees the primitive. A
    // string's own code units and length are answered here without a
    // wrapper.
    switch (base.type()) {
    case Value::Type::Object:
        return base.as_object()->get(*this, key, base);
    case Value::Type::Undefined:
    case Value::Type::Null:
    case Value::Type::Empty:
        return throw_type_error("Cannot read properties of " + std::string(base.is_null() ? "null" : "undefined")
            + " (reading '" + key_description(key) + "')");
    case Value::Type::String: {
        JsString* string = base.as_string();
        if (key.is_index() && key.as_index() < string->length())
            return Value::string(m_heap->string(string->view()[key.as_index()]));
        if (key.is_atom() && key.as_atom() == atoms().length)
            return Value::number(static_cast<double>(string->length()));
        return m_intrinsics.string_prototype->get(*this, key, base);
    }
    case Value::Type::Number:
        return m_intrinsics.number_prototype->get(*this, key, base);
    case Value::Type::Boolean:
        return m_intrinsics.boolean_prototype->get(*this, key, base);
    case Value::Type::Symbol:
        return m_intrinsics.symbol_prototype->get(*this, key, base);
    }
    return Value::undefined();
}

std::optional<Value> Interpreter::get(Value const& base, std::string_view name)
{
    return get(base, m_heap->key(name));
}

std::optional<Value> Interpreter::get(Object& object, PropertyKey const& key)
{
    return object.get(*this, key, Value::object(&object));
}

std::optional<bool> Interpreter::set(Value const& base, PropertyKey const& key, Value const& value, bool strict)
{
    // PutValue on a property reference (§6.2.5.6): a primitive base is
    // boxed for the lookup but stays the receiver, so a setter on the
    // prototype runs and a plain write has nowhere to land — silently in
    // sloppy code, a TypeError in strict.
    if (base.is_nullish() || base.is_empty())
        return throw_type_error("Cannot set properties of " + std::string(base.is_null() ? "null" : "undefined")
            + " (setting '" + key_description(key) + "')");
    Roots const roots(*this);
    root(base);
    root(value);
    Object* target = nullptr;
    if (base.is_object()) {
        target = base.as_object();
    } else {
        std::optional<Object*> const boxed = to_object(base);
        if (!boxed)
            return std::nullopt;
        target = *boxed;
        root(Value::object(target));
    }
    std::optional<bool> const succeeded = target->set(*this, key, value, base);
    if (!succeeded)
        return std::nullopt;
    if (!*succeeded && strict) {
        if (base.is_object())
            return throw_type_error("Cannot assign to read only property '" + key_description(key) + "' of object");
        return throw_type_error("Cannot create property '" + key_description(key) + "' on " + type_of(base)->to_utf8() + " '" + describe(base) + "'");
    }
    return *succeeded;
}

std::optional<bool> Interpreter::set(Object& object, PropertyKey const& key, Value const& value, bool strict)
{
    return set(Value::object(&object), key, value, strict);
}

std::optional<bool> Interpreter::create_data_property(Object& object, PropertyKey const& key, Value const& value, bool or_throw)
{
    // CreateDataProperty (§7.3.5): a fresh writable, enumerable,
    // configurable property, or a TypeError when the object refuses it.
    bool const ok = object.define_own_property(key, PropertyDescriptor::data(value, default_attributes));
    if (!ok && or_throw)
        return throw_type_error("Cannot define property " + key_description(key) + ", object is not extensible");
    return ok;
}

std::optional<bool> Interpreter::define_own_property(Object& object, PropertyKey const& key, PropertyDescriptor const& descriptor)
{
    // [[DefineOwnProperty]] with the part of ArraySetLength (§10.4.2.4
    // steps 3–5) that runs script and throws: an array's new length must
    // be the same number as ToUint32 and ToNumber make of it.
    if (object.is_array() && key.is_atom() && key.as_atom() == atoms().length && descriptor.value) {
        Roots const roots(*this);
        root(Value::object(&object));
        root(*descriptor.value);
        std::optional<std::uint32_t> const new_length = to_uint32(*descriptor.value);
        if (!new_length)
            return std::nullopt;
        std::optional<double> const number_length = to_number(*descriptor.value);
        if (!number_length)
            return std::nullopt;
        if (static_cast<double>(*new_length) != *number_length)
            return throw_range_error("Invalid array length");
        PropertyDescriptor converted = descriptor;
        converted.value = Value::number(static_cast<double>(*new_length));
        return object.define_own_property(key, converted);
    }
    return object.define_own_property(key, descriptor);
}

std::optional<bool> Interpreter::define_property_or_throw(Object& object, PropertyKey const& key, PropertyDescriptor const& descriptor)
{
    std::optional<bool> const defined = define_own_property(object, key, descriptor);
    if (!defined)
        return std::nullopt;
    if (!*defined)
        return throw_type_error("Cannot redefine property: " + key_description(key));
    return true;
}

std::optional<bool> Interpreter::delete_property_or_throw(Object& object, PropertyKey const& key)
{
    if (!object.delete_property(key))
        return throw_type_error("Cannot delete property '" + key_description(key) + "' of #<" + class_name(object) + ">");
    return true;
}

std::optional<bool> Interpreter::has_property(Value const& base, PropertyKey const& key)
{
    if (!base.is_object())
        return throw_type_error("Cannot use 'in' operator to search for '" + key_description(key) + "' in " + describe(base));
    return base.as_object()->has_property(key);
}

std::optional<Value> Interpreter::get_method(Value const& base, PropertyKey const& key)
{
    // §7.3.11: undefined and null both read as "no method"; anything
    // else present must be callable.
    std::optional<Value> const function = get(base, key);
    if (!function)
        return std::nullopt;
    if (function->is_nullish())
        return Value::undefined();
    if (!is_callable(*function))
        return throw_type_error(key_description(key) + " is not a function");
    return *function;
}

std::optional<Function*> Interpreter::get_function(Value const& base, PropertyKey const& key)
{
    std::optional<Value> const function = get_method(base, key);
    if (!function)
        return std::nullopt;
    if (function->is_undefined())
        return nullptr;
    return static_cast<Function*>(function->as_object());
}

std::optional<Value> Interpreter::invoke(Value const& base, PropertyKey const& key, std::span<Value const> arguments)
{
    // §7.3.20: the method is read from the value itself, which then
    // serves as `this`.
    Roots const roots(*this);
    root(base);
    std::optional<Value> const function = get(base, key);
    if (!function)
        return std::nullopt;
    root(*function);
    if (!is_callable(*function))
        return throw_type_error(key_description(key) + " is not a function");
    return call(*function, base, arguments);
}

std::optional<bool> Interpreter::instance_of(Value const& value, Value const& target)
{
    // InstanceofOperator (§13.10.2): a @@hasInstance on the right-hand
    // side decides; without one the target must be callable and
    // OrdinaryHasInstance walks the chain.
    if (!target.is_object())
        return throw_type_error("Right-hand side of 'instanceof' is not an object");
    Roots const roots(*this);
    root(value);
    root(target);
    std::optional<Value> const handler = get_method(target, PropertyKey::symbol(atoms().symbol_has_instance));
    if (!handler)
        return std::nullopt;
    if (!handler->is_undefined()) {
        root(*handler);
        Value const arguments[1] = { value };
        std::optional<Value> const result = call(*handler, target, arguments);
        if (!result)
            return std::nullopt;
        return to_boolean(*result);
    }
    if (!is_callable(target))
        return throw_type_error("Right-hand side of 'instanceof' is not callable");
    return ordinary_has_instance(target, value);
}

std::optional<bool> Interpreter::ordinary_has_instance(Value const& constructor, Value const& value)
{
    // §7.3.22: a bound function defers to its target; otherwise the
    // constructor's `prototype` must appear on the value's chain.
    if (!is_callable(constructor))
        return false;
    Object* function = constructor.as_object();
    if (function->class_id() == Object::Class::BoundFunction)
        return instance_of(value, Value::object(static_cast<BoundFunction*>(function)->target()));
    if (!value.is_object())
        return false;
    Roots const roots(*this);
    root(constructor);
    root(value);
    std::optional<Value> const prototype = get(constructor, PropertyKey::atom(atoms().prototype));
    if (!prototype)
        return std::nullopt;
    if (!prototype->is_object())
        return throw_type_error("Function has non-object prototype '" + describe(*prototype) + "' in instanceof check");
    Object const* wanted = prototype->as_object();
    for (Object const* link = value.as_object()->prototype(); link != nullptr; link = link->prototype()) {
        if (link == wanted)
            return true;
    }
    return false;
}

std::optional<Value> Interpreter::species_constructor(Object& object, Function* default_constructor)
{
    // SpeciesConstructor (§7.3.23): the object's constructor, then its
    // @@species; either absent falls back to the default.
    Roots const roots(*this);
    root(Value::object(&object));
    std::optional<Value> const constructor = get(object, PropertyKey::atom(atoms().constructor));
    if (!constructor)
        return std::nullopt;
    if (constructor->is_undefined())
        return Value::object(default_constructor);
    if (!constructor->is_object())
        return throw_type_error("object.constructor is not an object");
    root(*constructor);
    std::optional<Value> const species = get(*constructor->as_object(), PropertyKey::symbol(atoms().symbol_species));
    if (!species)
        return std::nullopt;
    if (species->is_nullish())
        return Value::object(default_constructor);
    if (!is_constructor(*species))
        return throw_type_error("object.constructor[Symbol.species] is not a constructor");
    return *species;
}

std::optional<Object*> Interpreter::get_prototype_from_constructor(Object* new_target, Object* default_prototype)
{
    // §10.1.14: `prototype` read from the constructor `new` was applied
    // to, falling back to the realm's intrinsic when it is not an object.
    if (new_target == nullptr)
        return default_prototype;
    std::optional<Value> const prototype = get(*new_target, PropertyKey::atom(atoms().prototype));
    if (!prototype)
        return std::nullopt;
    if (!prototype->is_object())
        return default_prototype;
    return prototype->as_object();
}

// ---------------------------------------------- testing and comparison §7.2

bool Interpreter::strict_equals(Value const& a, Value const& b)
{
    // IsStrictlyEqual (§7.2.15): NaN is unequal to itself, +0 and −0 are
    // equal, strings compare by contents, the rest by identity.
    if (a.type() != b.type())
        return false;
    if (a.is_number())
        return a.as_number() == b.as_number();
    if (a.is_string())
        return a.as_string()->equals(*b.as_string());
    return a == b;
}

bool Interpreter::same_value(Value const& a, Value const& b)
{
    // SameValue (§7.2.10): NaN equals itself, +0 and −0 differ.
    if (a.type() != b.type())
        return false;
    if (a.is_number()) {
        double const x = a.as_number();
        double const y = b.as_number();
        if (std::isnan(x) && std::isnan(y))
            return true;
        return x == y && std::signbit(x) == std::signbit(y);
    }
    if (a.is_string())
        return a.as_string()->equals(*b.as_string());
    return a == b;
}

bool Interpreter::same_value_zero(Value const& a, Value const& b)
{
    // SameValueZero (§7.2.11): as SameValue, but +0 equals −0.
    if (a.is_number() && b.is_number()) {
        double const x = a.as_number();
        double const y = b.as_number();
        if (std::isnan(x) && std::isnan(y))
            return true;
        return x == y;
    }
    return same_value(a, b);
}

std::optional<bool> Interpreter::loose_equals(Value const& x, Value const& y)
{
    // IsLooselyEqual (§7.2.14), rule by rule: same types compare
    // strictly; null and undefined match each other only; a number and a
    // string compare as numbers; a boolean becomes a number; a primitive
    // against an object compares with the object's primitive.
    if (x.type() == y.type())
        return strict_equals(x, y);
    if (x.is_nullish() && y.is_nullish())
        return true;
    if (x.is_nullish() || y.is_nullish())
        return false;
    if (x.is_number() && y.is_string())
        return x.as_number() == string_to_number(y.as_string()->view());
    if (x.is_string() && y.is_number())
        return string_to_number(x.as_string()->view()) == y.as_number();
    if (x.is_boolean())
        return loose_equals(Value::number(x.as_boolean() ? 1.0 : 0.0), y);
    if (y.is_boolean())
        return loose_equals(x, Value::number(y.as_boolean() ? 1.0 : 0.0));
    if ((x.is_string() || x.is_number() || x.is_symbol()) && y.is_object()) {
        Roots const roots(*this);
        root(x);
        std::optional<Value> const primitive = to_primitive(y);
        if (!primitive)
            return std::nullopt;
        return loose_equals(x, *primitive);
    }
    if (x.is_object() && (y.is_string() || y.is_number() || y.is_symbol())) {
        Roots const roots(*this);
        root(y);
        std::optional<Value> const primitive = to_primitive(x);
        if (!primitive)
            return std::nullopt;
        return loose_equals(*primitive, y);
    }
    return false;
}

std::optional<std::optional<bool>> Interpreter::less_than(Value const& left, Value const& right, bool left_first)
{
    // IsLessThan (§7.2.13): the operands become primitives in source
    // order; two strings compare by code units, anything else as
    // numbers, where a NaN makes the answer undefined.
    Roots const roots(*this);
    root(left);
    root(right);
    Value px;
    Value py;
    if (left_first) {
        std::optional<Value> const x = to_primitive(left, PreferredType::Number);
        if (!x)
            return std::nullopt;
        root(*x);
        std::optional<Value> const y = to_primitive(right, PreferredType::Number);
        if (!y)
            return std::nullopt;
        px = *x;
        py = *y;
    } else {
        std::optional<Value> const y = to_primitive(right, PreferredType::Number);
        if (!y)
            return std::nullopt;
        root(*y);
        std::optional<Value> const x = to_primitive(left, PreferredType::Number);
        if (!x)
            return std::nullopt;
        px = *x;
        py = *y;
    }
    if (px.is_string() && py.is_string())
        return std::optional<bool>(px.as_string()->view() < py.as_string()->view());
    std::optional<double> const nx = to_number(px);
    if (!nx)
        return std::nullopt;
    std::optional<double> const ny = to_number(py);
    if (!ny)
        return std::nullopt;
    if (std::isnan(*nx) || std::isnan(*ny))
        return std::optional<bool>();
    return std::optional<bool>(*nx < *ny);
}

std::optional<bool> Interpreter::is_regexp(Value const& value)
{
    // IsRegExp (§7.2.8): a @ property decides when there is one;
    // otherwise the internal class.
    if (!value.is_object())
        return false;
    std::optional<Value> const matcher = get(*value.as_object(), PropertyKey::symbol(atoms().symbol_match));
    if (!matcher)
        return std::nullopt;
    if (!matcher->is_undefined())
        return to_boolean(*matcher);
    return value.as_object()->class_id() == Object::Class::RegExp;
}

std::optional<double> Interpreter::length_of_array_like(Object& object)
{
    std::optional<Value> const length = get(object, PropertyKey::atom(atoms().length));
    if (!length)
        return std::nullopt;
    return to_length(*length);
}

std::optional<std::vector<Value>> Interpreter::create_list_from_array_like(Value const& value)
{
    // §7.3.19. The values are rooted while the list is built, since each
    // Get may run script; the caller takes over from the return.
    if (!value.is_object())
        return throw_type_error("CreateListFromArrayLike called on non-object");
    Roots const roots(*this);
    root(value);
    Object& object = *value.as_object();
    std::optional<double> const length = length_of_array_like(object);
    if (!length)
        return std::nullopt;
    std::vector<Value> list;
    list.reserve(static_cast<std::size_t>(std::min(*length, 65536.0)));
    for (double index = 0; index < *length; ++index) {
        std::optional<Value> const element = get(object, m_heap->key(index));
        if (!element)
            return std::nullopt;
        root(*element);
        list.push_back(*element);
    }
    return list;
}

// -------------------------------------------------------------- describe

std::string Interpreter::describe(Value const& value)
{
    // Never runs script and never allocates: an error is "Name: message"
    // from the data properties along its chain, any other object is its
    // class in brackets, a primitive its own spelling.
    switch (value.type()) {
    case Value::Type::Undefined:
    case Value::Type::Empty:
        return "undefined";
    case Value::Type::Null:
        return "null";
    case Value::Type::Boolean:
        return value.as_boolean() ? "true" : "false";
    case Value::Type::Number:
        return number_to_utf8(value.as_number());
    case Value::Type::String:
        return value.as_string()->to_utf8();
    case Value::Type::Symbol: {
        JsString const* description = value.as_symbol()->description();
        return "Symbol(" + (description ? description->to_utf8() : std::string()) + ")";
    }
    case Value::Type::Object:
        break;
    }
    Object const& object = *value.as_object();
    if (object.is_error() || data_property_up_chain(object, PropertyKey::atom(atoms().message))) {
        std::string name = "Error";
        std::string message;
        if (std::optional<Value> const n = data_property_up_chain(object, PropertyKey::atom(atoms().name)); n && n->is_string())
            name = n->as_string()->to_utf8();
        if (std::optional<Value> const m = data_property_up_chain(object, PropertyKey::atom(atoms().message)); m && m->is_string())
            message = m->as_string()->to_utf8();
        if (name.empty())
            return message;
        if (message.empty())
            return name;
        return name + ": " + message;
    }
    if (object.is_callable()) {
        std::string name;
        if (std::optional<Value> const n = data_property_up_chain(object, PropertyKey::atom(atoms().name)); n && n->is_string())
            name = n->as_string()->to_utf8();
        return "function " + name + "() { [native code] }";
    }
    return "[object " + class_name(object) + "]";
}

}
