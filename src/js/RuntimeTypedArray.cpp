#include "js/Runtime.h"

// %TypedArray% and its nine kinds (§23.2): the constructors with their four
// ways of being called, %TypedArray%.from and .of, the shared prototype's
// methods, and the two creation operations — species and same-type —
// those methods build their results with. The storage, the bounds
// arithmetic and the exotic property behaviour are in
// RuntimeArrayBuffer.cpp.
//
// Every method reads the length once, right after ValidateTypedArray, as
// the specification orders; an element read after a callback detached or
// shrank the buffer is undefined, and a write there is silently dropped,
// never an error.

#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

namespace {

constexpr ElementType element_types[element_type_count] = {
    ElementType::Int8,
    ElementType::Uint8,
    ElementType::Uint8Clamped,
    ElementType::Int16,
    ElementType::Uint16,
    ElementType::Int32,
    ElementType::Uint32,
    ElementType::Float16,
    ElementType::Float32,
    ElementType::Float64,
    ElementType::BigInt64,
    ElementType::BigUint64,
};

std::size_t type_index(ElementType type)
{
    return static_cast<std::size_t>(type);
}

TypedArrayObject* as_typed_array(Value const& value)
{
    if (!value.is_object() || value.as_object()->class_id() != Object::Class::TypedArray)
        return nullptr;
    return static_cast<TypedArrayObject*>(value.as_object());
}

std::string method_name(std::string_view method)
{
    return "%TypedArray%.prototype." + std::string(method);
}

// RequireInternalSlot(O, [[TypedArrayName]]) alone: a typed array, in
// bounds or not.
std::optional<TypedArrayObject*> this_typed_array(Interpreter& in, Value const& this_value, std::string_view method)
{
    TypedArrayObject* array = as_typed_array(this_value);
    if (array == nullptr)
        return in.throw_type_error(method_name(method) + " called on incompatible receiver " + in.describe(this_value));
    return array;
}

// ValidateTypedArray with the method's name in the message.
std::optional<TypedArrayObject*> validated(Interpreter& in, Value const& this_value, std::string_view method)
{
    return validate_typed_array(in, this_value, method_name(method));
}

NativeFunction::Callback requires_new(std::string name)
{
    return [name](Interpreter& in, Value const&, Args) -> std::optional<Value> {
        return in.throw_type_error("Constructor " + name + " requires 'new'");
    };
}

std::optional<Value> require_callable(Interpreter& in, Value const& value)
{
    if (!Interpreter::is_callable(value))
        return in.throw_type_error(in.describe(value) + " is not a function");
    return value;
}

// TypedArrayGetElement (§10.4.5.16) as Get(O, k) reads it: undefined once
// the index is no longer valid.
Value element_at(TypedArrayObject const& array, double index)
{
    if (!array.is_valid_index(index))
        return Value::undefined();
    return array.get_element(static_cast<std::size_t>(index));
}

// TypedArraySetElement (§10.4.5.17), which Set(O, k, v, true) on a typed
// array comes down to: the value converted first (ToNumber, or ToBigInt
// for a BigInt kind), stored if the index is still valid, never an error.
std::optional<bool> set_element_at(Interpreter& in, TypedArrayObject& array, double index, Value const& value)
{
    Interpreter::Roots const roots(in);
    in.root(Value::object(&array));
    in.root(value);
    std::optional<Value> const numeric = to_element_value(in, array.element_type(), value);
    if (!numeric)
        return std::nullopt;
    if (array.is_valid_index(index))
        array.set_element(static_cast<std::size_t>(index), *numeric);
    return true;
}

// The two content types never mix (§23.2.4.1 and the constructors): a
// BigInt kind takes from a BigInt kind only.
bool same_content_type(TypedArrayObject const& a, TypedArrayObject const& b)
{
    return is_bigint_element(a.element_type()) == is_bigint_element(b.element_type());
}

// A relative index argument (§23.2.3.6 step 5 and friends): negative
// counts from the end, both ends clamp, undefined is the fallback.
std::optional<double> relative_index(Interpreter& in, Value const& argument_value, double length, double fallback)
{
    if (argument_value.is_undefined())
        return fallback;
    std::optional<double> const relative = in.to_integer_or_infinity(argument_value);
    if (!relative)
        return std::nullopt;
    if (*relative < 0)
        return std::max(length + *relative, 0.0);
    return std::min(*relative, length);
}

// AllocateTypedArray (§23.2.5.1.1): the prototype from new_target (null =
// the kind's own), and with a length a fresh zeroed buffer; without one,
// a view that has no buffer yet, which its initializer supplies.
std::optional<TypedArrayObject*> allocate_typed_array(Interpreter& in, ElementType type, Object* new_target, std::optional<double> length)
{
    Interpreter::Roots const roots(in);
    if (new_target != nullptr)
        in.root(Value::object(new_target));
    std::optional<Object*> const prototype = in.get_prototype_from_constructor(new_target,
        [type](Intrinsics const& intrinsics) { return intrinsics.typed_array_prototypes[type_index(type)]; });
    if (!prototype)
        return std::nullopt;
    in.root(Value::object(*prototype));
    ArrayBufferObject* buffer = nullptr;
    std::optional<std::size_t> count;
    if (length) {
        std::optional<ArrayBufferObject*> const made = allocate_array_buffer(in, nullptr, *length * static_cast<double>(element_size(type)), std::nullopt);
        if (!made)
            return std::nullopt;
        buffer = *made;
        in.root(Value::object(buffer));
        count = static_cast<std::size_t>(*length);
    }
    return in.heap().allocate<TypedArrayObject>(*prototype, type, buffer, 0, count);
}

// TypedArrayCreateFromConstructor (§23.2.4.2): construct, validate, and
// with a single numeric argument insist on at least that many elements.
std::optional<TypedArrayObject*> create_from_constructor(Interpreter& in, Value const& constructor, Args arguments)
{
    Interpreter::Roots const roots(in);
    in.root(constructor);
    std::optional<Value> const made = in.construct(constructor, arguments);
    if (!made)
        return std::nullopt;
    in.root(*made);
    std::optional<TypedArrayObject*> const array = validate_typed_array(in, *made, "TypedArray constructor");
    if (!array)
        return std::nullopt;
    if (arguments.size() == 1 && arguments[0].is_number() && static_cast<double>((*array)->length()) < arguments[0].as_number())
        return in.throw_type_error("Derived TypedArray constructor created an array which was too small");
    return *array;
}

// TypedArraySpeciesCreate (§23.2.4.1): the species' array, which must hold
// the exemplar's content type.
std::optional<TypedArrayObject*> species_create(Interpreter& in, TypedArrayObject& exemplar, Args arguments)
{
    Function* default_constructor = in.intrinsics().typed_array_constructors[type_index(exemplar.element_type())];
    std::optional<Value> const constructor = in.species_constructor(exemplar, default_constructor);
    if (!constructor)
        return std::nullopt;
    std::optional<TypedArrayObject*> const made = create_from_constructor(in, *constructor, arguments);
    if (!made)
        return std::nullopt;
    if (!same_content_type(**made, exemplar))
        return in.throw_type_error("The species constructor made a TypedArray of the other content type");
    return made;
}

// TypedArrayCreateSameType (§23.2.4.3).
std::optional<TypedArrayObject*> create_same_type(Interpreter& in, TypedArrayObject& exemplar, Args arguments)
{
    Function* constructor = in.intrinsics().typed_array_constructors[type_index(exemplar.element_type())];
    return create_from_constructor(in, Value::object(constructor), arguments);
}

// IteratorToList (§7.4.13) over a record already made, each value rooted
// in the caller's scope.
std::optional<std::vector<Value>> list_from_iterator(Interpreter& in, IteratorRecord& record)
{
    std::vector<Value> values;
    while (true) {
        Value value;
        std::optional<bool> const stepped = in.iterator_step(record, value);
        if (!stepped)
            return std::nullopt;
        if (!*stepped)
            return values;
        in.root(value);
        values.push_back(value);
    }
}

// ---- the constructors' initializers (§23.2.5.1.2–.5)

std::optional<bool> initialize_from_typed_array(Interpreter& in, TypedArrayObject& target, TypedArrayObject& source)
{
    // InitializeTypedArrayFromTypedArray (§23.2.5.1.2): a fresh buffer of
    // the source's length, the bytes copied outright when the kinds match
    // and converted element by element when they do not.
    if (source.is_out_of_bounds())
        return in.throw_type_error("Cannot construct a TypedArray from a detached or out-of-bounds TypedArray");
    if (!same_content_type(target, source))
        return in.throw_type_error("Cannot mix BigInt and other types, use explicit conversions");
    std::size_t const length = source.length();
    std::size_t const size = target.element_size();
    Interpreter::Roots const roots(in);
    in.root(Value::object(&target));
    in.root(Value::object(&source));
    std::optional<ArrayBufferObject*> const buffer = allocate_array_buffer(in, nullptr, static_cast<double>(length) * static_cast<double>(size), std::nullopt);
    if (!buffer)
        return std::nullopt;
    target.attach(*buffer, 0, length);
    if (source.element_type() == target.element_type()) {
        if (length > 0)
            std::memcpy((*buffer)->data(), source.buffer()->data() + source.byte_offset(), length * size);
    } else {
        for (std::size_t k = 0; k < length; ++k)
            target.set_element(k, source.get_element(k));
    }
    return true;
}

std::optional<bool> initialize_from_buffer(Interpreter& in, TypedArrayObject& target, ArrayBufferObject& buffer, Value const& byte_offset, Value const& length)
{
    // InitializeTypedArrayFromArrayBuffer (§23.2.5.1.3): the offset must be
    // a multiple of the element size; without a length over a resizable
    // buffer the view tracks the buffer's end; otherwise the view must fit.
    Interpreter::Roots const roots(in);
    in.root(Value::object(&target));
    in.root(Value::object(&buffer));
    in.root(length);
    std::string const name(element_type_name(target.element_type()));
    double const size = static_cast<double>(target.element_size());
    std::optional<double> const offset = in.to_index(byte_offset);
    if (!offset)
        return std::nullopt;
    if (std::fmod(*offset, size) != 0)
        return in.throw_range_error("Start offset of " + name + " should be a multiple of " + number_to_utf8(size));
    std::optional<double> new_length;
    if (!length.is_undefined()) {
        std::optional<double> const index = in.to_index(length);
        if (!index)
            return std::nullopt;
        new_length = *index;
    }
    if (buffer.is_detached())
        return in.throw_type_error("Cannot construct a " + name + " on a detached ArrayBuffer");
    double const buffer_byte_length = static_cast<double>(buffer.byte_length());
    if (!new_length && buffer.is_resizable()) {
        if (*offset > buffer_byte_length)
            return in.throw_range_error("Start offset " + number_to_utf8(*offset) + " is outside the bounds of the buffer");
        target.attach(&buffer, static_cast<std::size_t>(*offset), std::nullopt);
        return true;
    }
    double new_byte_length = 0;
    if (!new_length) {
        if (std::fmod(buffer_byte_length, size) != 0)
            return in.throw_range_error("Byte length of " + name + " should be a multiple of " + number_to_utf8(size));
        new_byte_length = buffer_byte_length - *offset;
        if (new_byte_length < 0)
            return in.throw_range_error("Start offset " + number_to_utf8(*offset) + " is outside the bounds of the buffer");
    } else {
        new_byte_length = *new_length * size;
        if (*offset + new_byte_length > buffer_byte_length)
            return in.throw_range_error("Invalid typed array length: " + number_to_utf8(*new_length));
    }
    target.attach(&buffer, static_cast<std::size_t>(*offset), static_cast<std::size_t>(new_byte_length / size));
    return true;
}

std::optional<bool> initialize_from_values(Interpreter& in, TypedArrayObject& target, std::span<Value const> values)
{
    // InitializeTypedArrayFromList (§23.2.5.1.4): a buffer for as many
    // elements, each set through ToNumber. The values are the caller's,
    // rooted there.
    Interpreter::Roots const roots(in);
    in.root(Value::object(&target));
    std::optional<ArrayBufferObject*> const buffer = allocate_array_buffer(in, nullptr,
        static_cast<double>(values.size()) * static_cast<double>(target.element_size()), std::nullopt);
    if (!buffer)
        return std::nullopt;
    target.attach(*buffer, 0, values.size());
    for (std::size_t k = 0; k < values.size(); ++k) {
        if (!set_element_at(in, target, static_cast<double>(k), values[k]))
            return std::nullopt;
    }
    return true;
}

std::optional<bool> initialize_from_array_like(Interpreter& in, TypedArrayObject& target, Object& source)
{
    // InitializeTypedArrayFromArrayLike (§23.2.5.1.5).
    Interpreter::Roots const roots(in);
    in.root(Value::object(&target));
    in.root(Value::object(&source));
    std::optional<double> const length = in.length_of_array_like(source);
    if (!length)
        return std::nullopt;
    std::optional<ArrayBufferObject*> const buffer = allocate_array_buffer(in, nullptr, *length * static_cast<double>(target.element_size()), std::nullopt);
    if (!buffer)
        return std::nullopt;
    target.attach(*buffer, 0, static_cast<std::size_t>(*length));
    for (double k = 0; k < *length; ++k) {
        Interpreter::Roots const element_roots(in);
        std::optional<Value> const value = in.get(source, in.heap().key(k));
        if (!value)
            return std::nullopt;
        in.root(*value);
        if (!set_element_at(in, target, k, *value))
            return std::nullopt;
    }
    return true;
}

// The [[Construct]] of one kind (§23.2.5.1): no argument, a length, another
// typed array, a buffer with an offset and a length, or an iterable or
// array-like. The prototype is read before the argument is looked at,
// except that a length is converted first, as the steps order it.
NativeFunction::ConstructCallback kind_constructor(ElementType type)
{
    return [type](Interpreter& in, Args args, Object* new_target) -> std::optional<Value> {
        Interpreter::Roots const roots(in);
        if (new_target != nullptr)
            in.root(Value::object(new_target));
        if (args.empty()) {
            std::optional<TypedArrayObject*> const array = allocate_typed_array(in, type, new_target, 0.0);
            if (!array)
                return std::nullopt;
            return Value::object(*array);
        }
        Value const first = args[0];
        in.root(first);
        if (!first.is_object()) {
            std::optional<double> const length = in.to_index(first);
            if (!length)
                return std::nullopt;
            std::optional<TypedArrayObject*> const array = allocate_typed_array(in, type, new_target, *length);
            if (!array)
                return std::nullopt;
            return Value::object(*array);
        }
        std::optional<TypedArrayObject*> const made = allocate_typed_array(in, type, new_target, std::nullopt);
        if (!made)
            return std::nullopt;
        TypedArrayObject& target = **made;
        in.root(Value::object(&target));
        Object& source = *first.as_object();
        std::optional<bool> initialized;
        if (source.class_id() == Object::Class::TypedArray) {
            initialized = initialize_from_typed_array(in, target, static_cast<TypedArrayObject&>(source));
        } else if (source.class_id() == Object::Class::ArrayBuffer) {
            initialized = initialize_from_buffer(in, target, static_cast<ArrayBufferObject&>(source), argument(args, 1), argument(args, 2));
        } else {
            std::optional<Value> const using_iterator = in.get_method(first, PropertyKey::symbol(in.atoms().symbol_iterator));
            if (!using_iterator)
                return std::nullopt;
            if (!using_iterator->is_undefined()) {
                in.root(*using_iterator);
                std::optional<IteratorRecord> record = in.get_iterator_from_method(first, *using_iterator);
                if (!record)
                    return std::nullopt;
                in.root(record->iterator);
                in.root(record->next_method);
                std::optional<std::vector<Value>> const values = list_from_iterator(in, *record);
                if (!values)
                    return std::nullopt;
                initialized = initialize_from_values(in, target, *values);
            } else {
                initialized = initialize_from_array_like(in, target, source);
            }
        }
        if (!initialized)
            return std::nullopt;
        return Value::object(&target);
    };
}

// ---- the prototype's methods

enum class Visit { Every, Some, ForEach, Find, FindIndex, FindLast, FindLastIndex };

std::optional<Value> visit_elements(Interpreter& in, Value const& this_value, Args args, Visit kind, std::string_view method)
{
    // every, some, forEach, find, findIndex, findLast, findLastIndex
    // (§23.2.3.8, .12, .13, .14, .15, .16, .28): the callback sees the
    // element, its index and the array; the length is the one read at the
    // start, whatever the callback does to the buffer.
    std::optional<TypedArrayObject*> const found = validated(in, this_value, method);
    if (!found)
        return std::nullopt;
    TypedArrayObject& array = **found;
    double const length = static_cast<double>(array.length());
    std::optional<Value> const callback = require_callable(in, argument(args, 0));
    if (!callback)
        return std::nullopt;
    Value const this_argument = argument(args, 1);
    Interpreter::Roots const roots(in);
    in.root(this_value);
    in.root(*callback);
    in.root(this_argument);
    bool const from_end = kind == Visit::FindLast || kind == Visit::FindLastIndex;
    for (double i = 0; i < length; ++i) {
        double const k = from_end ? length - 1 - i : i;
        Value const element = element_at(array, k);
        Value const arguments[3] = { element, Value::number(k), this_value };
        std::optional<Value> const result = in.call(*callback, this_argument, arguments);
        if (!result)
            return std::nullopt;
        bool const truthy = Interpreter::to_boolean(*result);
        switch (kind) {
        case Visit::Every:
            if (!truthy)
                return Value::boolean(false);
            break;
        case Visit::Some:
            if (truthy)
                return Value::boolean(true);
            break;
        case Visit::ForEach:
            break;
        case Visit::Find:
        case Visit::FindLast:
            if (truthy)
                return element;
            break;
        case Visit::FindIndex:
        case Visit::FindLastIndex:
            if (truthy)
                return Value::number(k);
            break;
        }
    }
    switch (kind) {
    case Visit::Every:
        return Value::boolean(true);
    case Visit::Some:
        return Value::boolean(false);
    case Visit::ForEach:
    case Visit::Find:
    case Visit::FindLast:
        return Value::undefined();
    case Visit::FindIndex:
    case Visit::FindLastIndex:
        return Value::number(-1);
    }
    return Value::undefined();
}

std::optional<Value> reduce_elements(Interpreter& in, Value const& this_value, Args args, bool from_right)
{
    // reduce and reduceRight (§23.2.3.23, .24).
    std::string_view const method = from_right ? "reduceRight" : "reduce";
    std::optional<TypedArrayObject*> const found = validated(in, this_value, method);
    if (!found)
        return std::nullopt;
    TypedArrayObject& array = **found;
    double const length = static_cast<double>(array.length());
    std::optional<Value> const callback = require_callable(in, argument(args, 0));
    if (!callback)
        return std::nullopt;
    if (length == 0 && args.size() < 2)
        return in.throw_type_error("Reduce of empty array with no initial value");
    Interpreter::Roots const roots(in);
    in.root(this_value);
    in.root(*callback);
    double k = from_right ? length - 1 : 0;
    double const step = from_right ? -1 : 1;
    Value& accumulator = in.root(Value::undefined());
    if (args.size() >= 2) {
        accumulator = args[1];
    } else {
        accumulator = element_at(array, k);
        k += step;
    }
    for (; from_right ? k >= 0 : k < length; k += step) {
        Value const arguments[4] = { accumulator, element_at(array, k), Value::number(k), this_value };
        std::optional<Value> const result = in.call(*callback, Value::undefined(), arguments);
        if (!result)
            return std::nullopt;
        accumulator = *result;
    }
    return accumulator;
}

enum class Search { Includes, IndexOf, LastIndexOf };

std::optional<Value> search_elements(Interpreter& in, Value const& this_value, Args args, Search kind)
{
    // includes, indexOf, lastIndexOf (§23.2.3.17, .18, .20): includes
    // compares by SameValueZero and reads through a hole (a detached
    // buffer's undefined can match), the other two by strict equality over
    // present elements only.
    std::string_view const method = kind == Search::Includes ? "includes" : kind == Search::IndexOf ? "indexOf" : "lastIndexOf";
    std::optional<TypedArrayObject*> const found = validated(in, this_value, method);
    if (!found)
        return std::nullopt;
    TypedArrayObject& array = **found;
    double const length = static_cast<double>(array.length());
    Value const not_found = kind == Search::Includes ? Value::boolean(false) : Value::number(-1);
    if (length == 0)
        return not_found;
    Value const target = argument(args, 0);
    Interpreter::Roots const roots(in);
    in.root(this_value);
    in.root(target);
    if (kind == Search::LastIndexOf) {
        double n = length - 1;
        if (args.size() > 1) {
            std::optional<double> const from = in.to_integer_or_infinity(args[1]);
            if (!from)
                return std::nullopt;
            n = *from;
        }
        if (n == -std::numeric_limits<double>::infinity())
            return not_found;
        for (double k = n >= 0 ? std::min(n, length - 1) : length + n; k >= 0; k -= 1) {
            if (array.is_valid_index(k) && Interpreter::strict_equals(array.get_element(static_cast<std::size_t>(k)), target))
                return Value::number(k);
        }
        return not_found;
    }
    std::optional<double> const from = in.to_integer_or_infinity(argument(args, 1));
    if (!from)
        return std::nullopt;
    double n = *from;
    if (n == std::numeric_limits<double>::infinity())
        return not_found;
    if (n == -std::numeric_limits<double>::infinity())
        n = 0;
    for (double k = n >= 0 ? n : std::max(length + n, 0.0); k < length; k += 1) {
        if (kind == Search::Includes) {
            if (Interpreter::same_value_zero(element_at(array, k), target))
                return Value::boolean(true);
        } else if (array.is_valid_index(k) && Interpreter::strict_equals(array.get_element(static_cast<std::size_t>(k)), target)) {
            return Value::number(k);
        }
    }
    return not_found;
}

// TypedArraySortCompare (§23.2.3.29.1) with no comparator: numeric order,
// −0 before +0, NaN last; two BigInts by their integers.
int default_compare(Value const& a, Value const& b)
{
    if (a.is_bigint() && b.is_bigint())
        return compare(a.as_bigint()->value(), b.as_bigint()->value());
    double const x = a.is_number() ? a.as_number() : std::numeric_limits<double>::quiet_NaN();
    double const y = b.is_number() ? b.as_number() : std::numeric_limits<double>::quiet_NaN();
    bool const x_nan = std::isnan(x);
    bool const y_nan = std::isnan(y);
    if (x_nan && y_nan)
        return 0;
    if (x_nan)
        return 1;
    if (y_nan)
        return -1;
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    if (x == 0 && y == 0) {
        if (std::signbit(x) && !std::signbit(y))
            return -1;
        if (!std::signbit(x) && std::signbit(y))
            return 1;
    }
    return 0;
}

// A stable merge sort whose comparator may throw; false = abandoned.
template<typename Compare>
bool merge_sort(std::vector<Value>& values, std::vector<Value>& scratch, std::size_t begin, std::size_t end, Compare const& compare)
{
    if (end - begin < 2)
        return true;
    std::size_t const middle = begin + (end - begin) / 2;
    if (!merge_sort(values, scratch, begin, middle, compare) || !merge_sort(values, scratch, middle, end, compare))
        return false;
    std::size_t left = begin;
    std::size_t right = middle;
    std::size_t out = begin;
    while (left < middle && right < end) {
        std::optional<int> const order = compare(values[left], values[right]);
        if (!order)
            return false;
        scratch[out++] = *order <= 0 ? values[left++] : values[right++];
    }
    while (left < middle)
        scratch[out++] = values[left++];
    while (right < end)
        scratch[out++] = values[right++];
    for (std::size_t k = begin; k < end; ++k)
        values[k] = scratch[k];
    return true;
}

// SortIndexedProperties (§23.1.3.30.1) over a typed array, read-through-
// holes: every element is read before the first comparison, so a
// comparator that detaches the buffer changes nothing about the order.
std::optional<std::vector<Value>> sorted_elements(Interpreter& in, TypedArrayObject& array, std::size_t length, Value const& comparefn)
{
    // The elements are rooted in the caller's scope: a BigInt kind's are
    // fresh cells, and the comparator runs script.
    std::vector<Value> values(length);
    for (std::size_t k = 0; k < length; ++k) {
        values[k] = element_at(array, static_cast<double>(k));
        in.root(values[k]);
    }
    std::vector<Value> scratch(length);
    bool sorted = false;
    if (comparefn.is_undefined()) {
        sorted = merge_sort(values, scratch, 0, length, [](Value const& x, Value const& y) -> std::optional<int> { return default_compare(x, y); });
    } else {
        sorted = merge_sort(values, scratch, 0, length, [&](Value const& x, Value const& y) -> std::optional<int> {
            Value const arguments[2] = { x, y };
            std::optional<Value> const result = in.call(comparefn, Value::undefined(), arguments);
            if (!result)
                return std::nullopt;
            std::optional<double> const number = in.to_number(*result);
            if (!number)
                return std::nullopt;
            if (std::isnan(*number) || *number == 0)
                return 0;
            return *number < 0 ? -1 : 1;
        });
    }
    if (!sorted)
        return std::nullopt;
    return values;
}

std::optional<Value> set_from_typed_array(Interpreter& in, TypedArrayObject& target, double target_offset, TypedArrayObject& source)
{
    // SetTypedArrayFromTypedArray (§23.2.3.26.1): the bytes move outright
    // between two views of one kind, and memmove covers the case of one
    // buffer under both; between kinds the source is copied out first when
    // the buffers are one, as CloneArrayBuffer does.
    if (target.is_out_of_bounds())
        return in.throw_type_error("Cannot perform %TypedArray%.prototype.set on a detached or out-of-bounds TypedArray");
    std::size_t const target_length = target.length();
    if (source.is_out_of_bounds())
        return in.throw_type_error("Cannot perform %TypedArray%.prototype.set from a detached or out-of-bounds TypedArray");
    std::size_t const source_length = source.length();
    if (std::isinf(target_offset) || static_cast<double>(source_length) + target_offset > static_cast<double>(target_length))
        return in.throw_range_error("offset is out of bounds");
    std::size_t const offset = static_cast<std::size_t>(target_offset);
    std::size_t const size = target.element_size();
    std::uint8_t* destination = target.buffer()->data() + target.byte_offset() + offset * size;
    std::uint8_t const* origin = source.buffer()->data() + source.byte_offset();
    if (source.element_type() == target.element_type()) {
        if (source_length > 0)
            std::memmove(destination, origin, source_length * size);
        return Value::undefined();
    }
    if (!same_content_type(target, source))
        return in.throw_type_error("Cannot mix BigInt and other types, use explicit conversions");
    std::vector<std::uint8_t> clone;
    if (source.buffer() == target.buffer()) {
        clone.assign(origin, origin + source_length * source.element_size());
        origin = clone.data();
    }
    for (std::size_t k = 0; k < source_length; ++k)
        target.set_element(offset + k, read_element_value(in.heap(), source.element_type(), origin + k * source.element_size(), true));
    return Value::undefined();
}

std::optional<Value> set_from_array_like(Interpreter& in, TypedArrayObject& target, double target_offset, Value const& source)
{
    // SetTypedArrayFromArrayLike (§23.2.3.26.2): each element read and
    // converted in turn, a write past the end silently dropped.
    if (target.is_out_of_bounds())
        return in.throw_type_error("Cannot perform %TypedArray%.prototype.set on a detached or out-of-bounds TypedArray");
    double const target_length = static_cast<double>(target.length());
    Interpreter::Roots const roots(in);
    in.root(Value::object(&target));
    std::optional<Object*> const object = in.to_object(source);
    if (!object)
        return std::nullopt;
    in.root(Value::object(*object));
    std::optional<double> const source_length = in.length_of_array_like(**object);
    if (!source_length)
        return std::nullopt;
    if (std::isinf(target_offset) || *source_length + target_offset > target_length)
        return in.throw_range_error("offset is out of bounds");
    for (double k = 0; k < *source_length; ++k) {
        Interpreter::Roots const element_roots(in);
        std::optional<Value> const value = in.get(**object, in.heap().key(k));
        if (!value)
            return std::nullopt;
        in.root(*value);
        if (!set_element_at(in, target, target_offset + k, *value))
            return std::nullopt;
    }
    return Value::undefined();
}

std::optional<Value> make_iterator(Interpreter& in, Value const& this_value, ArrayIteratorObject::Kind kind, std::string_view method)
{
    // entries, keys, values (§23.2.3.7, .19, .35): CreateArrayIterator over
    // a validated typed array; the iterator re-checks the bounds per step.
    std::optional<TypedArrayObject*> const found = validated(in, this_value, method);
    if (!found)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    return Value::object(in.heap().allocate<ArrayIteratorObject>(in.intrinsics().array_iterator_prototype, *found, kind));
}

// %TypedArray%.from (§23.2.2.1) and .of (§23.2.2.2): `this` is the
// constructor to build with, which must make a typed array of at least
// the source's length.
std::optional<Value> typed_array_from(Interpreter& in, Value const& this_value, Args args)
{
    if (!Interpreter::is_constructor(this_value))
        return in.throw_type_error("%TypedArray%.from: this is not a constructor");
    Value const source = argument(args, 0);
    Value const mapper = argument(args, 1);
    if (!mapper.is_undefined() && !Interpreter::is_callable(mapper))
        return in.throw_type_error(in.describe(mapper) + " is not a function");
    Value const this_argument = argument(args, 2);
    Interpreter::Roots const roots(in);
    in.root(this_value);
    in.root(source);
    in.root(mapper);
    in.root(this_argument);
    auto const map_and_set = [&](TypedArrayObject& target, Value const& value, double k) -> std::optional<bool> {
        Interpreter::Roots const element_roots(in);
        in.root(value);
        Value mapped = value;
        if (!mapper.is_undefined()) {
            Value const arguments[2] = { value, Value::number(k) };
            std::optional<Value> const result = in.call(mapper, this_argument, arguments);
            if (!result)
                return std::nullopt;
            mapped = *result;
            in.root(mapped);
        }
        return set_element_at(in, target, k, mapped);
    };
    std::optional<Value> const using_iterator = in.get_method(source, PropertyKey::symbol(in.atoms().symbol_iterator));
    if (!using_iterator)
        return std::nullopt;
    if (!using_iterator->is_undefined()) {
        in.root(*using_iterator);
        std::optional<IteratorRecord> record = in.get_iterator_from_method(source, *using_iterator);
        if (!record)
            return std::nullopt;
        in.root(record->iterator);
        in.root(record->next_method);
        std::optional<std::vector<Value>> const values = list_from_iterator(in, *record);
        if (!values)
            return std::nullopt;
        Value const length[1] = { Value::number(static_cast<double>(values->size())) };
        std::optional<TypedArrayObject*> const target = create_from_constructor(in, this_value, length);
        if (!target)
            return std::nullopt;
        in.root(Value::object(*target));
        for (std::size_t k = 0; k < values->size(); ++k) {
            if (!map_and_set(**target, (*values)[k], static_cast<double>(k)))
                return std::nullopt;
        }
        return Value::object(*target);
    }
    std::optional<Object*> const array_like = in.to_object(source);
    if (!array_like)
        return std::nullopt;
    in.root(Value::object(*array_like));
    std::optional<double> const count = in.length_of_array_like(**array_like);
    if (!count)
        return std::nullopt;
    Value const length[1] = { Value::number(*count) };
    std::optional<TypedArrayObject*> const target = create_from_constructor(in, this_value, length);
    if (!target)
        return std::nullopt;
    in.root(Value::object(*target));
    for (double k = 0; k < *count; ++k) {
        std::optional<Value> const value = in.get(**array_like, in.heap().key(k));
        if (!value)
            return std::nullopt;
        if (!map_and_set(**target, *value, k))
            return std::nullopt;
    }
    return Value::object(*target);
}

std::optional<Value> typed_array_of(Interpreter& in, Value const& this_value, Args args)
{
    if (!Interpreter::is_constructor(this_value))
        return in.throw_type_error("%TypedArray%.of: this is not a constructor");
    Interpreter::Roots const roots(in);
    in.root(this_value);
    Value const length[1] = { Value::number(static_cast<double>(args.size())) };
    std::optional<TypedArrayObject*> const target = create_from_constructor(in, this_value, length);
    if (!target)
        return std::nullopt;
    in.root(Value::object(*target));
    for (std::size_t k = 0; k < args.size(); ++k) {
        if (!set_element_at(in, **target, static_cast<double>(k), args[k]))
            return std::nullopt;
    }
    return Value::object(*target);
}

void install_prototype(Interpreter& in, Object& prototype)
{
    WellKnownAtoms const& atoms = in.atoms();

    // The getters (§23.2.3.1–.4, .19): a view out of bounds answers 0 for
    // its lengths and offset rather than throwing.
    define_accessor(in, prototype, "buffer", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<TypedArrayObject*> const array = this_typed_array(interp, this_value, "buffer");
        if (!array)
            return std::nullopt;
        return Value::object((*array)->buffer());
    });
    define_accessor(in, prototype, "byteLength", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<TypedArrayObject*> const array = this_typed_array(interp, this_value, "byteLength");
        if (!array)
            return std::nullopt;
        return Value::number(static_cast<double>((*array)->byte_length()));
    });
    define_accessor(in, prototype, "byteOffset", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<TypedArrayObject*> const array = this_typed_array(interp, this_value, "byteOffset");
        if (!array)
            return std::nullopt;
        if ((*array)->is_out_of_bounds())
            return Value::number(0);
        return Value::number(static_cast<double>((*array)->byte_offset()));
    });
    define_accessor(in, prototype, "length", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<TypedArrayObject*> const array = this_typed_array(interp, this_value, "length");
        if (!array)
            return std::nullopt;
        return Value::number(static_cast<double>((*array)->length()));
    });
    {
        // get [Symbol.toStringTag] (§23.2.3.38): the kind's name, and
        // undefined rather than a throw for anything else.
        NativeFunction* tag = in.new_native("get [Symbol.toStringTag]", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
            TypedArrayObject const* array = as_typed_array(this_value);
            if (array == nullptr)
                return Value::undefined();
            return Value::string(interp.atom(element_type_name(array->element_type())));
        });
        prototype.put_accessor(PropertyKey::symbol(atoms.symbol_to_string_tag), tag, nullptr, Configurable);
    }

    define_method(in, prototype, "at", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.2.3.1.
        std::optional<TypedArrayObject*> const array = validated(interp, this_value, "at");
        if (!array)
            return std::nullopt;
        double const length = static_cast<double>((*array)->length());
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<double> const relative = interp.to_integer_or_infinity(argument(args, 0));
        if (!relative)
            return std::nullopt;
        double const k = *relative >= 0 ? *relative : length + *relative;
        if (k < 0 || k >= length)
            return Value::undefined();
        return element_at(**array, k);
    });
    define_method(in, prototype, "copyWithin", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.2.3.6: the byte loop of the specification, which stops at the
        // buffer's end when a coercion shrank it, direction and all.
        std::optional<TypedArrayObject*> const found = validated(interp, this_value, "copyWithin");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        double length = static_cast<double>(array.length());
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<double> const to = relative_index(interp, argument(args, 0), length, 0);
        if (!to)
            return std::nullopt;
        std::optional<double> const from = relative_index(interp, argument(args, 1), length, 0);
        if (!from)
            return std::nullopt;
        std::optional<double> const final = relative_index(interp, argument(args, 2), length, length);
        if (!final)
            return std::nullopt;
        double const count = std::min(*final - *from, length - *to);
        if (count > 0) {
            if (array.is_out_of_bounds())
                return interp.throw_type_error("Cannot perform %TypedArray%.prototype.copyWithin on a detached or out-of-bounds TypedArray");
            length = static_cast<double>(array.length());
            double const size = static_cast<double>(array.element_size());
            double const byte_offset = static_cast<double>(array.byte_offset());
            double const buffer_byte_limit = length * size + byte_offset;
            double to_byte = *to * size + byte_offset;
            double from_byte = *from * size + byte_offset;
            double count_bytes = count * size;
            double direction = 1;
            if (from_byte < to_byte && to_byte < from_byte + count_bytes) {
                direction = -1;
                from_byte += count_bytes - 1;
                to_byte += count_bytes - 1;
            }
            std::uint8_t* bytes = array.buffer()->data();
            while (count_bytes > 0) {
                if (from_byte < buffer_byte_limit && to_byte < buffer_byte_limit) {
                    bytes[static_cast<std::size_t>(to_byte)] = bytes[static_cast<std::size_t>(from_byte)];
                    from_byte += direction;
                    to_byte += direction;
                    count_bytes -= 1;
                } else {
                    count_bytes = 0;
                }
            }
        }
        return this_value;
    });
    define_method(in, prototype, "entries", 0, [](Interpreter& interp, Value const& this_value, Args) {
        return make_iterator(interp, this_value, ArrayIteratorObject::Kind::Entries, "entries");
    });
    define_method(in, prototype, "every", 1, [](Interpreter& interp, Value const& this_value, Args args) {
        return visit_elements(interp, this_value, args, Visit::Every, "every");
    });
    define_method(in, prototype, "fill", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.2.3.9: the value is converted before the range is read, and
        // the bounds are checked again after both, since either may have
        // changed the buffer.
        std::optional<TypedArrayObject*> const found = validated(interp, this_value, "fill");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        double length = static_cast<double>(array.length());
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<Value> const numeric = to_element_value(interp, array.element_type(), argument(args, 0));
        if (!numeric)
            return std::nullopt;
        interp.root(*numeric);
        std::optional<double> const start = relative_index(interp, argument(args, 1), length, 0);
        if (!start)
            return std::nullopt;
        std::optional<double> const end = relative_index(interp, argument(args, 2), length, length);
        if (!end)
            return std::nullopt;
        if (array.is_out_of_bounds())
            return interp.throw_type_error("Cannot perform %TypedArray%.prototype.fill on a detached or out-of-bounds TypedArray");
        length = static_cast<double>(array.length());
        double const stop = std::min(*end, length);
        for (double k = *start; k < stop; k += 1)
            array.set_element(static_cast<std::size_t>(k), *numeric);
        return this_value;
    });
    define_method(in, prototype, "filter", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.2.3.10: the kept elements are gathered first, then a species
        // array of exactly that many takes them.
        std::optional<TypedArrayObject*> const found = validated(interp, this_value, "filter");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        double const length = static_cast<double>(array.length());
        std::optional<Value> const callback = require_callable(interp, argument(args, 0));
        if (!callback)
            return std::nullopt;
        Value const this_argument = argument(args, 1);
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        interp.root(*callback);
        interp.root(this_argument);
        std::vector<Value> kept;
        for (double k = 0; k < length; k += 1) {
            Value const element = element_at(array, k);
            interp.root(element);
            Value const arguments[3] = { element, Value::number(k), this_value };
            std::optional<Value> const result = interp.call(*callback, this_argument, arguments);
            if (!result)
                return std::nullopt;
            if (Interpreter::to_boolean(*result))
                kept.push_back(element);
        }
        Value const count[1] = { Value::number(static_cast<double>(kept.size())) };
        std::optional<TypedArrayObject*> const target = species_create(interp, array, count);
        if (!target)
            return std::nullopt;
        for (std::size_t n = 0; n < kept.size(); ++n) {
            if ((*target)->is_valid_index(static_cast<double>(n)))
                (*target)->set_element(n, kept[n]);
        }
        return Value::object(*target);
    });
    define_method(in, prototype, "find", 1, [](Interpreter& interp, Value const& this_value, Args args) {
        return visit_elements(interp, this_value, args, Visit::Find, "find");
    });
    define_method(in, prototype, "findIndex", 1, [](Interpreter& interp, Value const& this_value, Args args) {
        return visit_elements(interp, this_value, args, Visit::FindIndex, "findIndex");
    });
    define_method(in, prototype, "findLast", 1, [](Interpreter& interp, Value const& this_value, Args args) {
        return visit_elements(interp, this_value, args, Visit::FindLast, "findLast");
    });
    define_method(in, prototype, "findLastIndex", 1, [](Interpreter& interp, Value const& this_value, Args args) {
        return visit_elements(interp, this_value, args, Visit::FindLastIndex, "findLastIndex");
    });
    define_method(in, prototype, "forEach", 1, [](Interpreter& interp, Value const& this_value, Args args) {
        return visit_elements(interp, this_value, args, Visit::ForEach, "forEach");
    });
    define_method(in, prototype, "includes", 1, [](Interpreter& interp, Value const& this_value, Args args) {
        return search_elements(interp, this_value, args, Search::Includes);
    });
    define_method(in, prototype, "indexOf", 1, [](Interpreter& interp, Value const& this_value, Args args) {
        return search_elements(interp, this_value, args, Search::IndexOf);
    });
    define_method(in, prototype, "join", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.2.3.18: an element that has become undefined joins as the
        // empty string.
        std::optional<TypedArrayObject*> const found = validated(interp, this_value, "join");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        double const length = static_cast<double>(array.length());
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::u16string separator = u",";
        if (!argument(args, 0).is_undefined()) {
            std::optional<JsString*> const text = interp.to_string(argument(args, 0));
            if (!text)
                return std::nullopt;
            separator = (*text)->data();
        }
        std::u16string result;
        for (double k = 0; k < length; k += 1) {
            if (k > 0)
                result += separator;
            Value const element = element_at(array, k);
            if (element.is_bigint()) {
                std::string const digits = element.as_bigint()->value().to_string();
                result.append(digits.begin(), digits.end());
            } else if (!element.is_undefined()) {
                result += number_to_string(element.as_number());
            }
        }
        return Value::string(interp.heap().string(std::move(result)));
    });
    define_method(in, prototype, "keys", 0, [](Interpreter& interp, Value const& this_value, Args) {
        return make_iterator(interp, this_value, ArrayIteratorObject::Kind::Keys, "keys");
    });
    define_method(in, prototype, "lastIndexOf", 1, [](Interpreter& interp, Value const& this_value, Args args) {
        return search_elements(interp, this_value, args, Search::LastIndexOf);
    });
    define_method(in, prototype, "map", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.2.3.22: the species array is made before the first call.
        std::optional<TypedArrayObject*> const found = validated(interp, this_value, "map");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        double const length = static_cast<double>(array.length());
        std::optional<Value> const callback = require_callable(interp, argument(args, 0));
        if (!callback)
            return std::nullopt;
        Value const this_argument = argument(args, 1);
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        interp.root(*callback);
        interp.root(this_argument);
        Value const count[1] = { Value::number(length) };
        std::optional<TypedArrayObject*> const target = species_create(interp, array, count);
        if (!target)
            return std::nullopt;
        interp.root(Value::object(*target));
        for (double k = 0; k < length; k += 1) {
            Interpreter::Roots const element_roots(interp);
            Value const arguments[3] = { element_at(array, k), Value::number(k), this_value };
            std::optional<Value> const mapped = interp.call(*callback, this_argument, arguments);
            if (!mapped)
                return std::nullopt;
            interp.root(*mapped);
            if (!set_element_at(interp, **target, k, *mapped))
                return std::nullopt;
        }
        return Value::object(*target);
    });
    define_method(in, prototype, "reduce", 1, [](Interpreter& interp, Value const& this_value, Args args) {
        return reduce_elements(interp, this_value, args, false);
    });
    define_method(in, prototype, "reduceRight", 1, [](Interpreter& interp, Value const& this_value, Args args) {
        return reduce_elements(interp, this_value, args, true);
    });
    define_method(in, prototype, "reverse", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §23.2.3.25: in place, nothing runs script.
        std::optional<TypedArrayObject*> const found = validated(interp, this_value, "reverse");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        std::size_t const length = array.length();
        // The bytes themselves change places: reading a BigInt kind's
        // element would make a cell, and a second read could collect it.
        std::size_t const size = array.element_size();
        std::uint8_t* const bytes = array.buffer()->data() + array.byte_offset();
        for (std::size_t lower = 0; lower < length / 2; ++lower) {
            std::size_t const upper = length - 1 - lower;
            for (std::size_t b = 0; b < size; ++b)
                std::swap(bytes[lower * size + b], bytes[upper * size + b]);
        }
        return this_value;
    });
    define_method(in, prototype, "set", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.2.3.26: the receiver need only be a typed array here; the
        // bounds are checked once the offset is known.
        std::optional<TypedArrayObject*> const found = this_typed_array(interp, this_value, "set");
        if (!found)
            return std::nullopt;
        TypedArrayObject& target = **found;
        Value const source = argument(args, 0);
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        interp.root(source);
        std::optional<double> const target_offset = interp.to_integer_or_infinity(argument(args, 1));
        if (!target_offset)
            return std::nullopt;
        if (*target_offset < 0)
            return interp.throw_range_error("offset is out of bounds");
        if (TypedArrayObject* array = as_typed_array(source))
            return set_from_typed_array(interp, target, *target_offset, *array);
        return set_from_array_like(interp, target, *target_offset, source);
    });
    define_method(in, prototype, "slice", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.2.3.27: a species array of the count, then — if there is
        // anything to copy and the source is still in bounds — the bytes
        // outright between views of one kind, else element by element.
        std::optional<TypedArrayObject*> const found = validated(interp, this_value, "slice");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        double length = static_cast<double>(array.length());
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<double> const start = relative_index(interp, argument(args, 0), length, 0);
        if (!start)
            return std::nullopt;
        std::optional<double> end = relative_index(interp, argument(args, 1), length, length);
        if (!end)
            return std::nullopt;
        double count = std::max(*end - *start, 0.0);
        Value const count_argument[1] = { Value::number(count) };
        std::optional<TypedArrayObject*> const made = species_create(interp, array, count_argument);
        if (!made)
            return std::nullopt;
        TypedArrayObject& target = **made;
        interp.root(Value::object(&target));
        if (count > 0) {
            if (array.is_out_of_bounds())
                return interp.throw_type_error("Cannot perform %TypedArray%.prototype.slice on a detached or out-of-bounds TypedArray");
            length = static_cast<double>(array.length());
            end = std::min(*end, length);
            count = std::max(*end - *start, 0.0);
            if (array.element_type() == target.element_type()) {
                // One byte at a time, forward, as the specification's loop
                // has it: a species result may view the same buffer, and
                // then the order of the copy is observable.
                std::size_t const size = array.element_size();
                std::size_t const count_bytes = std::min(static_cast<std::size_t>(count) * size, target.byte_length());
                std::uint8_t* to = target.buffer()->data() + target.byte_offset();
                std::uint8_t const* from = array.buffer()->data() + array.byte_offset() + static_cast<std::size_t>(*start) * size;
                for (std::size_t n = 0; n < count_bytes; ++n)
                    to[n] = from[n];
            } else {
                double n = 0;
                for (double k = *start; k < *end; k += 1, n += 1) {
                    if (!set_element_at(interp, target, n, element_at(array, k)))
                        return std::nullopt;
                }
            }
        }
        return Value::object(&target);
    });
    define_method(in, prototype, "some", 1, [](Interpreter& interp, Value const& this_value, Args args) {
        return visit_elements(interp, this_value, args, Visit::Some, "some");
    });
    define_method(in, prototype, "sort", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.2.3.29: the comparator is checked before the receiver; the
        // sorted values are written back through Set, so a buffer the
        // comparator detached takes none of them.
        Value const comparefn = argument(args, 0);
        if (!comparefn.is_undefined() && !Interpreter::is_callable(comparefn))
            return interp.throw_type_error("The comparison function must be either a function or undefined");
        std::optional<TypedArrayObject*> const found = validated(interp, this_value, "sort");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        std::size_t const length = array.length();
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        interp.root(comparefn);
        std::optional<std::vector<Value>> const sorted = sorted_elements(interp, array, length, comparefn);
        if (!sorted)
            return std::nullopt;
        for (std::size_t j = 0; j < length; ++j) {
            if (array.is_valid_index(static_cast<double>(j)))
                array.set_element(j, (*sorted)[j]);
        }
        return this_value;
    });
    define_method(in, prototype, "subarray", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.2.3.30: a species view over the same buffer — length-tracking
        // when the source tracks and no end was given — made without
        // validating the source, whose species constructor then decides.
        std::optional<TypedArrayObject*> const found = this_typed_array(interp, this_value, "subarray");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        double const source_length = array.is_out_of_bounds() ? 0 : static_cast<double>(array.length());
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<double> const start = relative_index(interp, argument(args, 0), source_length, 0);
        if (!start)
            return std::nullopt;
        double const size = static_cast<double>(array.element_size());
        double const begin_byte_offset = static_cast<double>(array.byte_offset()) + *start * size;
        Value const buffer = Value::object(array.buffer());
        if (array.is_length_tracking() && argument(args, 1).is_undefined()) {
            Value const arguments[2] = { buffer, Value::number(begin_byte_offset) };
            std::optional<TypedArrayObject*> const made = species_create(interp, array, arguments);
            if (!made)
                return std::nullopt;
            return Value::object(*made);
        }
        std::optional<double> const end = relative_index(interp, argument(args, 1), source_length, source_length);
        if (!end)
            return std::nullopt;
        double const new_length = std::max(*end - *start, 0.0);
        Value const arguments[3] = { buffer, Value::number(begin_byte_offset), Value::number(new_length) };
        std::optional<TypedArrayObject*> const made = species_create(interp, array, arguments);
        if (!made)
            return std::nullopt;
        return Value::object(*made);
    });
    define_method(in, prototype, "toLocaleString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §23.2.3.31: each element's own toLocaleString, joined by commas.
        std::optional<TypedArrayObject*> const found = validated(interp, this_value, "toLocaleString");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        double const length = static_cast<double>(array.length());
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::u16string result;
        for (double k = 0; k < length; k += 1) {
            if (k > 0)
                result += u",";
            Value const element = element_at(array, k);
            if (element.is_undefined())
                continue;
            Interpreter::Roots const element_roots(interp);
            std::optional<Value> const text = interp.invoke(element, interp.key("toLocaleString"), {});
            if (!text)
                return std::nullopt;
            interp.root(*text);
            std::optional<JsString*> const string = interp.to_string(*text);
            if (!string)
                return std::nullopt;
            result += (*string)->view();
        }
        return Value::string(interp.heap().string(std::move(result)));
    });
    define_method(in, prototype, "toReversed", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §23.2.3.32: a fresh array of the same kind, never a species.
        std::optional<TypedArrayObject*> const found = validated(interp, this_value, "toReversed");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        double const length = static_cast<double>(array.length());
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        Value const count[1] = { Value::number(length) };
        std::optional<TypedArrayObject*> const target = create_same_type(interp, array, count);
        if (!target)
            return std::nullopt;
        interp.root(Value::object(*target)); // reading a BigInt kind's element makes a cell
        for (double k = 0; k < length; k += 1) {
            if (!set_element_at(interp, **target, k, element_at(array, length - 1 - k)))
                return std::nullopt;
        }
        return Value::object(*target);
    });
    define_method(in, prototype, "toSorted", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.2.3.33.
        Value const comparefn = argument(args, 0);
        if (!comparefn.is_undefined() && !Interpreter::is_callable(comparefn))
            return interp.throw_type_error("The comparison function must be either a function or undefined");
        std::optional<TypedArrayObject*> const found = validated(interp, this_value, "toSorted");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        std::size_t const length = array.length();
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        interp.root(comparefn);
        Value const count[1] = { Value::number(static_cast<double>(length)) };
        std::optional<TypedArrayObject*> const target = create_same_type(interp, array, count);
        if (!target)
            return std::nullopt;
        interp.root(Value::object(*target));
        std::optional<std::vector<Value>> const sorted = sorted_elements(interp, array, length, comparefn);
        if (!sorted)
            return std::nullopt;
        for (std::size_t j = 0; j < length; ++j) {
            if ((*target)->is_valid_index(static_cast<double>(j)))
                (*target)->set_element(j, (*sorted)[j]);
        }
        return Value::object(*target);
    });
    define_method(in, prototype, "values", 0, [](Interpreter& interp, Value const& this_value, Args) {
        return make_iterator(interp, this_value, ArrayIteratorObject::Kind::Values, "values");
    });
    define_method(in, prototype, "with", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.2.3.36: the value is converted before the index is judged,
        // and an index that is not valid by then is a RangeError.
        std::optional<TypedArrayObject*> const found = validated(interp, this_value, "with");
        if (!found)
            return std::nullopt;
        TypedArrayObject& array = **found;
        double const length = static_cast<double>(array.length());
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<double> const relative = interp.to_integer_or_infinity(argument(args, 0));
        if (!relative)
            return std::nullopt;
        double const actual = *relative >= 0 ? *relative : length + *relative;
        std::optional<Value> const numeric = to_element_value(interp, array.element_type(), argument(args, 1));
        if (!numeric)
            return std::nullopt;
        interp.root(*numeric);
        if (!array.is_valid_index(actual))
            return interp.throw_range_error("Invalid typed array index");
        Value const count[1] = { Value::number(length) };
        std::optional<TypedArrayObject*> const target = create_same_type(interp, array, count);
        if (!target)
            return std::nullopt;
        interp.root(Value::object(*target)); // reading a BigInt kind's element makes a cell
        for (double k = 0; k < length; k += 1) {
            if (!set_element_at(interp, **target, k, k == actual ? *numeric : element_at(array, k)))
                return std::nullopt;
        }
        return Value::object(*target);
    });

    // toString is Array.prototype's very function (§23.2.3.34), and
    // @@iterator is values (§23.2.3.37).
    if (Property const* to_string = in.intrinsics().array_prototype->find_own(PropertyKey::atom(atoms.to_string)))
        prototype.put(PropertyKey::atom(atoms.to_string), to_string->value, builtin_attributes);
    if (Property const* values = prototype.find_own(in.key("values")))
        prototype.put(PropertyKey::symbol(atoms.symbol_iterator), values->value, builtin_attributes);
}

} // namespace

std::optional<TypedArrayObject*> new_typed_array(Interpreter& in, ElementType type, double length)
{
    return allocate_typed_array(in, type, nullptr, length);
}

void install_typed_arrays(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    WellKnownAtoms const& atoms = in.atoms();
    Heap::NoCollect const guard(in.heap());

    // %TypedArray% (§23.2.1) is a constructor that refuses to construct,
    // and the nine kinds inherit from it and from its prototype; it is no
    // global — scripts reach it through a kind's chain.
    i.typed_array_prototype = in.new_object();
    Object& prototype = *i.typed_array_prototype;
    NativeFunction* abstract = in.new_native("TypedArray", 0,
        [](Interpreter& interp, Value const&, Args) -> std::optional<Value> {
            return interp.throw_type_error("Abstract class TypedArray not directly constructable");
        },
        [](Interpreter& interp, Args, Object*) -> std::optional<Value> {
            return interp.throw_type_error("Abstract class TypedArray not directly constructable");
        });
    abstract->put(PropertyKey::atom(atoms.prototype), Value::object(&prototype), frozen_attributes);
    prototype.put(PropertyKey::atom(atoms.constructor), Value::object(abstract), builtin_attributes);
    i.typed_array_constructor = abstract;
    define_method(in, *abstract, "from", 1, typed_array_from);
    define_method(in, *abstract, "of", 0, typed_array_of);
    NativeFunction* species = in.new_native("get [Symbol.species]", 0, [](Interpreter&, Value const& this_value, Args) -> std::optional<Value> {
        return this_value;
    });
    abstract->put_accessor(PropertyKey::symbol(atoms.symbol_species), species, nullptr, Configurable);
    install_prototype(in, prototype);

    // The nine kinds (§23.2.6, §23.2.7): each a constructor of length 3
    // whose [[Prototype]] is %TypedArray%, with BYTES_PER_ELEMENT on both
    // the constructor and its prototype.
    for (ElementType const type : element_types) {
        std::string const name(element_type_name(type));
        Object* kind_prototype = in.new_object(&prototype);
        NativeFunction* constructor = in.new_native(name, 3, requires_new(name), kind_constructor(type));
        constructor->set_prototype(abstract);
        constructor->put(PropertyKey::atom(atoms.prototype), Value::object(kind_prototype), frozen_attributes);
        kind_prototype->put(PropertyKey::atom(atoms.constructor), Value::object(constructor), builtin_attributes);
        Value const bytes = Value::number(static_cast<double>(element_size(type)));
        constructor->put(in.key("BYTES_PER_ELEMENT"), bytes, frozen_attributes);
        kind_prototype->put(in.key("BYTES_PER_ELEMENT"), bytes, frozen_attributes);
        in.global()->put(in.key(name), Value::object(constructor), builtin_attributes);
        i.typed_array_prototypes[type_index(type)] = kind_prototype;
        i.typed_array_constructors[type_index(type)] = constructor;
    }
}

}
