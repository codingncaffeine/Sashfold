#pragma once

// The built-in library (§19–§22, §25.5): what install_intrinsics puts on
// a fresh realm's global object. One installer per library, each in its
// own file, all called once from the Interpreter's constructor in the
// order the dependencies want (Object and Function first, since every
// other prototype hangs off theirs).

#include "js/Interpreter.h"

#include <concepts>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sashfold::js {

void install_intrinsics(Interpreter&); // calls all of the below, in order

void install_object(Interpreter&); // Object, Object.prototype — RuntimeObject.cpp
void install_function(Interpreter&); // Function, Function.prototype — RuntimeObject.cpp
void install_error(Interpreter&); // Error and the six native errors — RuntimeObject.cpp
void install_symbol(Interpreter&); // Symbol — RuntimeObject.cpp
void install_boolean(Interpreter&); // Boolean — RuntimeNumber.cpp
void install_number(Interpreter&); // Number — RuntimeNumber.cpp
void install_bigint(Interpreter&); // BigInt — RuntimeBigInt.cpp
void install_math(Interpreter&); // Math — RuntimeNumber.cpp
void install_global_functions(Interpreter&); // eval, parseInt, parseFloat, isNaN, isFinite, the URI functions, globalThis — RuntimeNumber.cpp
void install_array(Interpreter&); // Array — RuntimeArray.cpp
void install_string(Interpreter&); // String — RuntimeString.cpp
void install_regexp(Interpreter&); // RegExp — RuntimeString.cpp
void install_json(Interpreter&); // JSON — RuntimeJson.cpp
void install_date(Interpreter&); // Date — RuntimeDate.cpp
void install_iterators(Interpreter&); // %IteratorPrototype%, the array and string iterators, Array.prototype.values and kin — RuntimeIterator.cpp
void install_collections(Interpreter&); // Map, Set, WeakMap, WeakSet, their iterators, Map.groupBy and Object.groupBy — RuntimeCollections.cpp
void install_promise(Interpreter&); // Promise, AggregateError, and the job queue's definitions — RuntimePromise.cpp
void install_disposable(Interpreter&); // SuppressedError, DisposableStack, AsyncDisposableStack, @@dispose on the iterator prototypes — RuntimeDisposable.cpp
void install_generators(Interpreter&); // %GeneratorFunction%, %GeneratorPrototype%, %AsyncFunction% — RuntimeGenerator.cpp
void install_array_buffer(Interpreter&); // ArrayBuffer — RuntimeArrayBuffer.cpp
void install_typed_arrays(Interpreter&); // %TypedArray% and its nine kinds — RuntimeTypedArray.cpp
void install_data_view(Interpreter&); // DataView — RuntimeArrayBuffer.cpp
void install_proxy(Interpreter&); // Proxy and Proxy.revocable — RuntimeProxy.cpp
void install_intl(Interpreter&); // Intl and its constructors, and the locale-sensitive methods of String, Number, BigInt, Date and Array — RuntimeIntl*.cpp

// The promise operations the engine itself needs (an await, an async
// function's result): NewPromiseCapability (§27.2.1.5), PromiseResolve
// (§27.2.4.7.1) and PerformPromiseThen (§27.2.5.4.1) — the last with no
// derived capability when the caller wants none, in which case the
// reactions settle nothing and the result is undefined.
std::optional<PromiseCapability> new_promise_capability(Interpreter&, Value const& constructor);
std::optional<Value> promise_resolve(Interpreter&, Value const& constructor, Value const& value);
Value perform_then(Interpreter&, PromiseObject& promise, Value const& on_fulfilled, Value const& on_rejected,
    std::optional<PromiseCapability> const& capability);

// Helpers shared by the installers and the bindings.

// A method as its definer gets it back: the property is defined, and the
// function object behind it is made when something first asks for it — a
// script that reads the property, or the definer through this handle (to
// put the one function under a second key) — since a realm has thousands
// of built-ins and a page calls few.
class DefinedMethod {
public:
    DefinedMethod(Object& target, PropertyKey key)
        : m_target(&target)
        , m_key(key)
    {
    }
    NativeFunction* function() const;
    operator NativeFunction*() const { return function(); }
    NativeFunction* operator->() const { return function(); }

private:
    Object* m_target;
    PropertyKey m_key;
};

// Defines `name` on `target` as a non-enumerable native method, or with
// the attributes given (the bindings' operations are enumerable, as
// WebIDL has them). A native given as a closure — a lambda that captures —
// is kept in the current realm's table (RealmRecord::native_closures) and
// described by its place there, so it too waits for a first use.
DefinedMethod define_method(Interpreter&, Object& target, std::string_view name, int length, NativeFunction::Callback);
DefinedMethod define_method(Interpreter&, Object& target, std::string_view name, int length, NativeFunction::Callback,
    std::uint8_t attributes);
// A getter (and optional setter) pair, non-enumerable, configurable; or
// with the attributes given.
void define_accessor(Interpreter&, Object& target, std::string_view name, NativeFunction::Callback getter,
    NativeFunction::Callback setter = {});
void define_accessor(Interpreter&, Object& target, std::string_view name, NativeFunction::Callback getter,
    NativeFunction::Callback setter, std::uint8_t attributes);
// A closure's place in the current realm's table, for a description's
// datum, and the entry every such description has: it calls the closure
// kept at the running function's datum in that function's own realm.
std::uint32_t keep_native_closure(Interpreter&, NativeFunction::Callback);
std::optional<Value> closure_native(Interpreter&, Value const& this_value, std::span<Value const> arguments);

// The same from plain functions — which a lambda that captures nothing is,
// and nearly every built-in is one: nothing is kept per realm at all.
template<typename F>
concept PlainNative = std::convertible_to<F, NativeFunction::Entry> && !std::same_as<std::remove_cvref_t<F>, std::nullptr_t>;

DefinedMethod define_plain_method(Interpreter&, Object& target, std::string_view name, int length, NativeFunction::Entry,
    std::uint8_t attributes);
void define_plain_accessor(Interpreter&, Object& target, std::string_view name, NativeFunction::Entry getter, NativeFunction::Entry setter,
    std::uint8_t attributes);

template<PlainNative F>
DefinedMethod define_method(Interpreter& in, Object& target, std::string_view name, int length, F&& callback)
{
    return define_plain_method(in, target, name, length, static_cast<NativeFunction::Entry>(callback), builtin_attributes);
}
template<PlainNative F>
DefinedMethod define_method(Interpreter& in, Object& target, std::string_view name, int length, F&& callback, std::uint8_t attributes)
{
    return define_plain_method(in, target, name, length, static_cast<NativeFunction::Entry>(callback), attributes);
}
template<PlainNative G>
void define_accessor(Interpreter& in, Object& target, std::string_view name, G&& getter)
{
    define_plain_accessor(in, target, name, static_cast<NativeFunction::Entry>(getter), nullptr, Configurable);
}
template<PlainNative G, PlainNative S>
void define_accessor(Interpreter& in, Object& target, std::string_view name, G&& getter, S&& setter)
{
    define_plain_accessor(in, target, name, static_cast<NativeFunction::Entry>(getter), static_cast<NativeFunction::Entry>(setter), Configurable);
}
template<PlainNative G, PlainNative S>
void define_accessor(Interpreter& in, Object& target, std::string_view name, G&& getter, S&& setter, std::uint8_t attributes)
{
    define_plain_accessor(in, target, name, static_cast<NativeFunction::Entry>(getter), static_cast<NativeFunction::Entry>(setter), attributes);
}
// A plain getter beside a setter given as a closure, or as none (`{}`).
template<PlainNative G>
void define_accessor(Interpreter& in, Object& target, std::string_view name, G&& getter, NativeFunction::Callback setter,
    std::uint8_t attributes = Configurable)
{
    if (setter)
        define_accessor(in, target, name, NativeFunction::Callback(std::forward<G>(getter)), std::move(setter), attributes);
    else
        define_plain_accessor(in, target, name, static_cast<NativeFunction::Entry>(getter), nullptr, attributes);
}
// A data property with the given attributes (a constant like Math.PI).
void define_value(Interpreter&, Object& target, std::string_view name, Value, std::uint8_t attributes = builtin_attributes);
// The argument at `index`, or undefined.
inline Value argument(std::span<Value const> arguments, std::size_t index)
{
    return index < arguments.size() ? arguments[index] : Value::undefined();
}
// Number::exponentiate (§6.1.6.1.3): `**` and Math.pow, with the cases
// where the language and the C library disagree spelled out.
double number_exponentiate(double base, double exponent);
// ToPropertyDescriptor (§6.2.6.5) and FromPropertyDescriptor (§6.2.6.4):
// a descriptor read out of an ordinary object and written back into one.
// Object's reflective methods, Reflect's and a proxy's traps — which hand
// a descriptor to script and take one back — all pass through these.
std::optional<PropertyDescriptor> to_property_descriptor(Interpreter&, Value const&);
Object* from_property_descriptor(Interpreter&, PropertyDescriptor const&);
// A key as the value a trap or a reflective method is handed: a string,
// or the symbol itself.
Value key_to_value(Interpreter&, PropertyKey const&);
// A key's text for a message: the name, the index in decimal, or
// Symbol(description). Never runs script.
std::string key_description(PropertyKey const&);
// A value's spelling for a message, computed without running script
// (Interpreter::describe).
std::string value_description(Interpreter&, Value const&);
// `this` coerced for a String.prototype method: RequireObjectCoercible
// then ToString.
std::optional<JsString*> this_string_value(Interpreter&, Value const& this_value, std::string_view method);
// The [[NumberData]] etc. behind `this`, or a TypeError naming the method.
std::optional<double> this_number_value(Interpreter&, Value const& this_value, std::string_view method);
std::optional<bool> this_boolean_value(Interpreter&, Value const& this_value, std::string_view method);

// Typed arrays for each other and for the bindings (a Uint8Array of the
// bytes a page asked for).
// AllocateArrayBuffer (§25.1.3.1): the prototype from new_target (null =
// the intrinsic); a RangeError when the length passes its maximum or the
// size the engine allows.
std::optional<ArrayBufferObject*> allocate_array_buffer(Interpreter&, Object* new_target, double byte_length,
    std::optional<double> max_byte_length);
// AllocateTypedArray with a length (§23.2.5.1.1): `length` zero elements
// over a fresh buffer, with the kind's own prototype.
std::optional<TypedArrayObject*> new_typed_array(Interpreter&, ElementType, double length);
// A value converted for a kind's elements: ToBigInt for the BigInt kinds
// (a Number is a TypeError there), ToNumber for the rest. May run script.
std::optional<Value> to_element_value(Interpreter&, ElementType, Value const&);
// ValidateTypedArray (§23.2.4.4): a typed array in bounds, or a TypeError
// naming the caller.
std::optional<TypedArrayObject*> validate_typed_array(Interpreter&, Value const&, std::string_view method);

// RegExpCreate (§22.2.3.1) in the current realm: a RegExp of a pattern and
// flags, lastIndex 0, or the SyntaxError they do not compile with. How a
// structured clone makes a RegExp again from its original source and flags.
std::optional<Value> create_regexp(Interpreter&, Value const& pattern, Value const& flags);

// %RegExpStringIteratorPrototype%.next (§22.2.9.2.1), installed with the
// iterator prototypes.
std::optional<Value> regexp_string_iterator_next(Interpreter&, Value const& this_value, std::span<Value const> args);

// SetIntegrityLevel (§7.3.15): sealed, or frozen when `frozen`; nullopt with
// the exception a proxy's trap threw. How the bindings make a FrozenArray,
// such as a MessageEvent's ports.
std::optional<bool> set_integrity_level(Interpreter&, Object&, bool frozen);

// The current time in ms since the epoch, and the local zone's offset
// (minutes east of UTC) at a given UTC time — the two places Date reads
// the clock. Platform code; both are overridable for tests.
double current_time_ms();
double local_time_zone_offset_minutes(double utc_ms);
void set_time_source(double (*now)(), double (*offset)(double));

}
