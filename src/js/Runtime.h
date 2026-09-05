#pragma once

// The built-in library (§19–§22, §25.5): what install_intrinsics puts on
// a fresh realm's global object. One installer per library, each in its
// own file, all called once from the Interpreter's constructor in the
// order the dependencies want (Object and Function first, since every
// other prototype hangs off theirs).

#include "js/Interpreter.h"

#include <span>
#include <string>
#include <string_view>

namespace sashfold::js {

void install_intrinsics(Interpreter&); // calls all of the below, in order

void install_object(Interpreter&); // Object, Object.prototype — RuntimeObject.cpp
void install_function(Interpreter&); // Function, Function.prototype — RuntimeObject.cpp
void install_error(Interpreter&); // Error and the six native errors — RuntimeObject.cpp
void install_symbol(Interpreter&); // Symbol — RuntimeObject.cpp
void install_boolean(Interpreter&); // Boolean — RuntimeNumber.cpp
void install_number(Interpreter&); // Number — RuntimeNumber.cpp
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

// Helpers shared by the installers and the bindings.

// Defines `name` on `target` as a non-enumerable native method.
NativeFunction* define_method(Interpreter&, Object& target, std::string_view name, int length,
    NativeFunction::Callback);
// A getter (and optional setter) pair, non-enumerable, configurable.
void define_accessor(Interpreter&, Object& target, std::string_view name, NativeFunction::Callback getter,
    NativeFunction::Callback setter = {});
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

// The current time in ms since the epoch, and the local zone's offset
// (minutes east of UTC) at a given UTC time — the two places Date reads
// the clock. Platform code; both are overridable for tests.
double current_time_ms();
double local_time_zone_offset_minutes(double utc_ms);
void set_time_source(double (*now)(), double (*offset)(double));

}
