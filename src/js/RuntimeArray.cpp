#include "js/Runtime.h"

// Array (§23.1): the constructor, isArray/of/from, and the prototype
// methods — every one written generically over an array-like `this`, the
// way the specification has them, so they serve arguments objects and
// plain objects with a length as well as arrays. The iterator-returning
// members (entries, keys, values, @@iterator) wait for the iterator
// protocol.

#include "js/Object.h"
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

using Args = std::span<Value const>;

namespace {

constexpr double max_safe_integer = 9007199254740991.0;
constexpr double max_array_length = 4294967295.0;

std::optional<Value> get_at(Interpreter& in, Object& object, double index)
{
    return in.get(object, in.heap().key(index));
}

std::optional<bool> set_at(Interpreter& in, Object& object, double index, Value const& value)
{
    return in.set(object, in.heap().key(index), value, true);
}

bool has_at(Interpreter& in, Object& object, double index)
{
    return object.has_property(in.heap().key(index));
}

std::optional<bool> delete_at(Interpreter& in, Object& object, double index)
{
    return in.delete_property_or_throw(object, in.heap().key(index));
}

std::optional<bool> create_at(Interpreter& in, Object& object, double index, Value const& value)
{
    return in.create_data_property(object, in.heap().key(index), value);
}

std::optional<bool> set_length(Interpreter& in, Object& object, double length)
{
    return in.set(object, PropertyKey::atom(in.atoms().length), Value::number(length), true);
}

// A relative index argument (§23.1.3.28 step 5 and friends): negative
// counts from the end, and both ends clamp.
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

// ArrayCreate (§10.4.2.2), with the length check the constructors share.
std::optional<ArrayObject*> array_create(Interpreter& in, double length)
{
    if (length > max_array_length)
        return in.throw_range_error("Invalid array length");
    ArrayObject* array = in.new_array();
    array->set_length(static_cast<std::uint32_t>(length));
    return array;
}

// ArraySpeciesCreate (§10.4.2.3): an array unless the original is one
// whose constructor names a species, which is then constructed.
std::optional<Value> array_species_create(Interpreter& in, Object& original, double length)
{
    auto const plain = [&]() -> std::optional<Value> {
        std::optional<ArrayObject*> const array = array_create(in, length);
        if (!array)
            return std::nullopt;
        return Value::object(*array);
    };
    if (!original.is_array())
        return plain();
    Interpreter::Roots const roots(in);
    std::optional<Value> constructor = in.get(original, PropertyKey::atom(in.atoms().constructor));
    if (!constructor)
        return std::nullopt;
    if (constructor->is_object()) {
        in.root(*constructor);
        constructor = in.get(*constructor->as_object(), PropertyKey::symbol(in.atoms().symbol_species));
        if (!constructor)
            return std::nullopt;
        if (constructor->is_null())
            constructor = Value::undefined();
    }
    if (constructor->is_undefined())
        return plain();
    if (!Interpreter::is_constructor(*constructor))
        return in.throw_type_error("object.constructor[Symbol.species] is not a constructor");
    in.root(*constructor);
    Value const arguments[1] = { Value::number(length) };
    return in.construct(*constructor, arguments);
}

std::optional<Value> require_callable(Interpreter& in, Value const& value)
{
    if (!Interpreter::is_callable(value))
        return in.throw_type_error(in.describe(value) + " is not a function");
    return value;
}

// The `this` of a prototype method as an object with its length read.
struct Subject {
    Object* object = nullptr;
    double length = 0;
};

std::optional<Subject> subject_of(Interpreter& in, Value const& this_value)
{
    std::optional<Object*> const object = in.to_object(this_value);
    if (!object)
        return std::nullopt;
    in.root(Value::object(*object));
    std::optional<double> const length = in.length_of_array_like(**object);
    if (!length)
        return std::nullopt;
    return Subject { *object, *length };
}

// IsConcatSpreadable (§23.1.3.2.1).
std::optional<bool> is_concat_spreadable(Interpreter& in, Value const& value)
{
    if (!value.is_object())
        return false;
    std::optional<Value> const spreadable = in.get(*value.as_object(), PropertyKey::symbol(in.atoms().symbol_is_concat_spreadable));
    if (!spreadable)
        return std::nullopt;
    if (!spreadable->is_undefined())
        return Interpreter::to_boolean(*spreadable);
    return Interpreter::is_array(value);
}

// SortCompare (§23.1.3.30.2): undefined sorts last, a comparator's
// number decides, and otherwise strings compare by code units.
std::optional<int> sort_compare(Interpreter& in, Value const& x, Value const& y, Value const& comparefn)
{
    if (x.is_undefined() && y.is_undefined())
        return 0;
    if (x.is_undefined())
        return 1;
    if (y.is_undefined())
        return -1;
    if (!comparefn.is_undefined()) {
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
    }
    Interpreter::Roots const roots(in);
    std::optional<JsString*> const xs = in.to_string(x);
    if (!xs)
        return std::nullopt;
    in.root(Value::string(*xs));
    std::optional<JsString*> const ys = in.to_string(y);
    if (!ys)
        return std::nullopt;
    if ((*xs)->view() < (*ys)->view())
        return -1;
    if ((*ys)->view() < (*xs)->view())
        return 1;
    return 0;
}

// A stable merge sort whose comparator may throw; false = abandoned.
bool merge_sort(Interpreter& in, std::vector<Value>& values, std::vector<Value>& scratch, std::size_t begin, std::size_t end, Value const& comparefn)
{
    if (end - begin < 2)
        return true;
    std::size_t const middle = begin + (end - begin) / 2;
    if (!merge_sort(in, values, scratch, begin, middle, comparefn) || !merge_sort(in, values, scratch, middle, end, comparefn))
        return false;
    std::size_t left = begin;
    std::size_t right = middle;
    std::size_t out = begin;
    while (left < middle && right < end) {
        std::optional<int> const order = sort_compare(in, values[left], values[right], comparefn);
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

// SortIndexedProperties (§23.1.3.30.1) with holes skipped, the values
// rooted for the comparator's calls.
std::optional<std::vector<Value>> sorted_values(Interpreter& in, Object& object, double length, Value const& comparefn, bool skip_holes)
{
    std::vector<Value> values;
    for (double k = 0; k < length; ++k) {
        if (skip_holes && !has_at(in, object, k))
            continue;
        std::optional<Value> const value = get_at(in, object, k);
        if (!value)
            return std::nullopt;
        in.root(*value);
        values.push_back(*value);
    }
    std::vector<Value> scratch(values.size());
    if (!merge_sort(in, values, scratch, 0, values.size(), comparefn))
        return std::nullopt;
    return values;
}

// FlattenIntoArray (§23.1.3.13.1).
std::optional<double> flatten_into(Interpreter& in, Object& target, Object& source, double source_length, double start, double depth,
    Value const& mapper, Value const& this_argument)
{
    double target_index = start;
    for (double source_index = 0; source_index < source_length; ++source_index) {
        if (!has_at(in, source, source_index))
            continue;
        Interpreter::Roots const roots(in);
        std::optional<Value> element = get_at(in, source, source_index);
        if (!element)
            return std::nullopt;
        in.root(*element);
        if (!mapper.is_undefined()) {
            Value const arguments[3] = { *element, Value::number(source_index), Value::object(&source) };
            element = in.call(mapper, this_argument, arguments);
            if (!element)
                return std::nullopt;
            in.root(*element);
        }
        bool flatten = false;
        if (depth > 0) {
            flatten = Interpreter::is_array(*element);
        }
        if (flatten) {
            std::optional<double> const inner_length = in.length_of_array_like(*element->as_object());
            if (!inner_length)
                return std::nullopt;
            std::optional<double> const next = flatten_into(in, target, *element->as_object(), *inner_length, target_index, depth - 1, Value::undefined(), Value::undefined());
            if (!next)
                return std::nullopt;
            target_index = *next;
        } else {
            if (target_index >= max_safe_integer)
                return in.throw_type_error("array too long");
            if (!create_at(in, target, target_index, *element))
                return std::nullopt;
            ++target_index;
        }
    }
    return target_index;
}

// The shared shape of every/some/forEach/map/filter/find*: the callback
// over each present index, with (value, index, object).
enum class Visit { Every, Some, ForEach, Map, Filter, Find, FindIndex, FindLast, FindLastIndex };

std::optional<Value> visit_elements(Interpreter& in, Value const& this_value, Args args, Visit kind)
{
    Interpreter::Roots const roots(in);
    std::optional<Subject> const subject = subject_of(in, this_value);
    if (!subject)
        return std::nullopt;
    Object& object = *subject->object;
    double const length = subject->length;
    std::optional<Value> const callback = require_callable(in, argument(args, 0));
    if (!callback)
        return std::nullopt;
    Value const this_argument = argument(args, 1);
    in.root(this_argument);
    Object* result = nullptr;
    if (kind == Visit::Map) {
        std::optional<Value> const created = array_species_create(in, object, length);
        if (!created)
            return std::nullopt;
        result = created->as_object();
        in.root(*created);
    } else if (kind == Visit::Filter) {
        std::optional<Value> const created = array_species_create(in, object, 0);
        if (!created)
            return std::nullopt;
        result = created->as_object();
        in.root(*created);
    }
    bool const backwards = kind == Visit::FindLast || kind == Visit::FindLastIndex;
    bool const skip_holes = kind != Visit::Find && kind != Visit::FindIndex && !backwards;
    double filtered = 0;
    for (double step = 0; step < length; ++step) {
        double const k = backwards ? length - 1 - step : step;
        if (skip_holes && !has_at(in, object, k))
            continue;
        Interpreter::Roots const element_roots(in);
        std::optional<Value> const element = get_at(in, object, k);
        if (!element)
            return std::nullopt;
        in.root(*element);
        Value const arguments[3] = { *element, Value::number(k), Value::object(&object) };
        std::optional<Value> const outcome = in.call(*callback, this_argument, arguments);
        if (!outcome)
            return std::nullopt;
        in.root(*outcome);
        bool const truthy = Interpreter::to_boolean(*outcome);
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
        case Visit::Map:
            if (!create_at(in, *result, k, *outcome))
                return std::nullopt;
            break;
        case Visit::Filter:
            if (truthy) {
                if (!create_at(in, *result, filtered, *element))
                    return std::nullopt;
                ++filtered;
            }
            break;
        case Visit::Find:
        case Visit::FindLast:
            if (truthy)
                return *element;
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
    case Visit::Map:
    case Visit::Filter:
        return Value::object(result);
    case Visit::FindIndex:
    case Visit::FindLastIndex:
        return Value::number(-1);
    default:
        return Value::undefined();
    }
}

std::optional<Value> reduce_elements(Interpreter& in, Value const& this_value, Args args, bool from_right)
{
    // §23.1.3.24 / §23.1.3.25.
    Interpreter::Roots const roots(in);
    std::optional<Subject> const subject = subject_of(in, this_value);
    if (!subject)
        return std::nullopt;
    Object& object = *subject->object;
    double const length = subject->length;
    std::optional<Value> const callback = require_callable(in, argument(args, 0));
    if (!callback)
        return std::nullopt;
    if (length == 0 && args.size() < 2)
        return in.throw_type_error("Reduce of empty array with no initial value");
    double k = from_right ? length - 1 : 0;
    auto const in_range = [&] { return from_right ? k >= 0 : k < length; };
    auto const advance = [&] { k += from_right ? -1 : 1; };
    Value& accumulator = in.root(Value::undefined());
    if (args.size() >= 2) {
        accumulator = args[1];
    } else {
        bool present = false;
        while (!present && in_range()) {
            present = has_at(in, object, k);
            if (present) {
                std::optional<Value> const value = get_at(in, object, k);
                if (!value)
                    return std::nullopt;
                accumulator = *value;
            }
            advance();
        }
        if (!present)
            return in.throw_type_error("Reduce of empty array with no initial value");
    }
    for (; in_range(); advance()) {
        if (!has_at(in, object, k))
            continue;
        Interpreter::Roots const element_roots(in);
        std::optional<Value> const element = get_at(in, object, k);
        if (!element)
            return std::nullopt;
        in.root(*element);
        Value const arguments[4] = { accumulator, *element, Value::number(k), Value::object(&object) };
        std::optional<Value> const outcome = in.call(*callback, Value::undefined(), arguments);
        if (!outcome)
            return std::nullopt;
        accumulator = *outcome;
    }
    return accumulator;
}

std::optional<Value> join_elements(Interpreter& in, Object& object, double length, std::u16string_view separator, bool locale)
{
    // §23.1.3.18 join, and the element loop of toLocaleString (§23.1.3.32).
    std::u16string out;
    for (double k = 0; k < length; ++k) {
        if (k > 0)
            out += separator;
        Interpreter::Roots const roots(in);
        std::optional<Value> const element = get_at(in, object, k);
        if (!element)
            return std::nullopt;
        if (element->is_nullish())
            continue;
        in.root(*element);
        std::optional<Value> value = *element;
        if (locale) {
            value = in.invoke(*element, in.key("toLocaleString"), {});
            if (!value)
                return std::nullopt;
            in.root(*value);
        }
        std::optional<JsString*> const text = in.to_string(*value);
        if (!text)
            return std::nullopt;
        out += (*text)->view();
    }
    return Value::string(in.string(std::u16string_view(out)));
}

std::optional<Value> index_of(Interpreter& in, Value const& this_value, Args args, bool from_end, bool includes)
{
    // indexOf (§23.1.3.17), lastIndexOf (§23.1.3.20) and includes
    // (§23.1.3.16): strict equality for the first two, SameValueZero and
    // holes read as undefined for the last.
    Interpreter::Roots const roots(in);
    std::optional<Subject> const subject = subject_of(in, this_value);
    if (!subject)
        return std::nullopt;
    Object& object = *subject->object;
    double const length = subject->length;
    Value const not_found = includes ? Value::boolean(false) : Value::number(-1);
    if (length == 0)
        return not_found;
    Value const wanted = argument(args, 0);
    in.root(wanted);
    double start = 0;
    if (from_end) {
        double n = length - 1;
        if (args.size() > 1) {
            std::optional<double> const given = in.to_integer_or_infinity(args[1]);
            if (!given)
                return std::nullopt;
            n = *given;
        }
        if (n == -std::numeric_limits<double>::infinity())
            return not_found;
        start = n >= 0 ? std::min(n, length - 1) : length + n;
        for (double k = start; k >= 0; --k) {
            if (!has_at(in, object, k))
                continue;
            std::optional<Value> const element = get_at(in, object, k);
            if (!element)
                return std::nullopt;
            if (Interpreter::strict_equals(*element, wanted))
                return Value::number(k);
        }
        return not_found;
    }
    if (args.size() > 1) {
        std::optional<double> const given = in.to_integer_or_infinity(args[1]);
        if (!given)
            return std::nullopt;
        if (*given == std::numeric_limits<double>::infinity())
            return not_found;
        start = *given >= 0 ? *given : std::max(length + *given, 0.0);
    }
    for (double k = start; k < length; ++k) {
        if (!includes && !has_at(in, object, k))
            continue;
        std::optional<Value> const element = get_at(in, object, k);
        if (!element)
            return std::nullopt;
        if (includes ? Interpreter::same_value_zero(*element, wanted) : Interpreter::strict_equals(*element, wanted))
            return includes ? Value::boolean(true) : Value::number(k);
    }
    return not_found;
}

std::optional<Value> array_from_values(Interpreter& in, std::vector<Value> const& values)
{
    // The values are the caller's and rooted by it.
    std::optional<ArrayObject*> const array = array_create(in, 0);
    if (!array)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(Value::object(*array));
    for (std::size_t k = 0; k < values.size(); ++k) {
        if (!create_at(in, **array, static_cast<double>(k), values[k]))
            return std::nullopt;
    }
    return Value::object(*array);
}

// Array(...) called or constructed (§23.1.1.1).
std::optional<Value> construct_array(Interpreter& in, Args args, Object* new_target)
{
    Interpreter::Roots const roots(in);
    for (Value const& arg : args)
        in.root(arg);
    std::optional<Object*> const prototype = in.get_prototype_from_constructor(new_target, in.intrinsics().array_prototype);
    if (!prototype)
        return std::nullopt;
    ArrayObject* array = in.heap().allocate<ArrayObject>(*prototype);
    in.root(Value::object(array));
    if (args.size() == 1) {
        if (!args[0].is_number()) {
            array->set_element(0, args[0]);
            return Value::object(array);
        }
        double const length = args[0].as_number();
        if (length < 0 || length > max_array_length || std::trunc(length) != length)
            return in.throw_range_error("Invalid array length");
        array->set_length(static_cast<std::uint32_t>(length));
        return Value::object(array);
    }
    for (std::size_t k = 0; k < args.size(); ++k)
        array->set_element(static_cast<std::uint32_t>(k), args[k]);
    return Value::object(array);
}

void install_prototype(Interpreter& in, Object& prototype)
{
    define_method(in, prototype, "at", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        std::optional<double> const relative = interp.to_integer_or_infinity(argument(args, 0));
        if (!relative)
            return std::nullopt;
        double const k = *relative >= 0 ? *relative : subject->length + *relative;
        if (k < 0 || k >= subject->length)
            return Value::undefined();
        return get_at(interp, *subject->object, k);
    });
    define_method(in, prototype, "concat", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.2: spreadable arguments contribute their elements,
        // holes included as holes; the rest are appended whole.
        Interpreter::Roots const roots(interp);
        std::optional<Object*> const object = interp.to_object(this_value);
        if (!object)
            return std::nullopt;
        interp.root(Value::object(*object));
        std::optional<Value> const created = array_species_create(interp, **object, 0);
        if (!created)
            return std::nullopt;
        interp.root(*created);
        Object& result = *created->as_object();
        double n = 0;
        std::vector<Value> items;
        items.push_back(Value::object(*object));
        for (Value const& arg : args) {
            interp.root(arg);
            items.push_back(arg);
        }
        for (Value const& item : items) {
            std::optional<bool> const spreadable = is_concat_spreadable(interp, item);
            if (!spreadable)
                return std::nullopt;
            if (*spreadable) {
                Object& source = *item.as_object();
                std::optional<double> const length = interp.length_of_array_like(source);
                if (!length)
                    return std::nullopt;
                if (n + *length > max_safe_integer)
                    return interp.throw_type_error("array too long");
                for (double k = 0; k < *length; ++k, ++n) {
                    if (!has_at(interp, source, k))
                        continue;
                    std::optional<Value> const element = get_at(interp, source, k);
                    if (!element)
                        return std::nullopt;
                    if (!create_at(interp, result, n, *element))
                        return std::nullopt;
                }
            } else {
                if (n >= max_safe_integer)
                    return interp.throw_type_error("array too long");
                if (!create_at(interp, result, n, item))
                    return std::nullopt;
                ++n;
            }
        }
        if (!set_length(interp, result, n))
            return std::nullopt;
        return Value::object(&result);
    });
    define_method(in, prototype, "copyWithin", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.4.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        Object& object = *subject->object;
        double const length = subject->length;
        std::optional<double> const to = relative_index(interp, argument(args, 0), length, 0);
        if (!to)
            return std::nullopt;
        std::optional<double> const from = relative_index(interp, argument(args, 1), length, 0);
        if (!from)
            return std::nullopt;
        std::optional<double> const final = relative_index(interp, argument(args, 2), length, length);
        if (!final)
            return std::nullopt;
        double count = std::min(*final - *from, length - *to);
        double source = *from;
        double target = *to;
        double direction = 1;
        if (source < target && target < source + count) {
            direction = -1;
            source += count - 1;
            target += count - 1;
        }
        while (count > 0) {
            if (has_at(interp, object, source)) {
                std::optional<Value> const element = get_at(interp, object, source);
                if (!element)
                    return std::nullopt;
                if (!set_at(interp, object, target, *element))
                    return std::nullopt;
            } else if (!delete_at(interp, object, target)) {
                return std::nullopt;
            }
            source += direction;
            target += direction;
            --count;
        }
        return Value::object(&object);
    });
    define_method(in, prototype, "every", 1, [](Interpreter& interp, Value const& this_value, Args args) { return visit_elements(interp, this_value, args, Visit::Every); });
    define_method(in, prototype, "fill", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.7.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        double const length = subject->length;
        std::optional<double> const start = relative_index(interp, argument(args, 1), length, 0);
        if (!start)
            return std::nullopt;
        std::optional<double> const end = relative_index(interp, argument(args, 2), length, length);
        if (!end)
            return std::nullopt;
        Value const value = argument(args, 0);
        interp.root(value);
        for (double k = *start; k < *end; ++k) {
            if (!set_at(interp, *subject->object, k, value))
                return std::nullopt;
        }
        return Value::object(subject->object);
    });
    define_method(in, prototype, "filter", 1, [](Interpreter& interp, Value const& this_value, Args args) { return visit_elements(interp, this_value, args, Visit::Filter); });
    define_method(in, prototype, "find", 1, [](Interpreter& interp, Value const& this_value, Args args) { return visit_elements(interp, this_value, args, Visit::Find); });
    define_method(in, prototype, "findIndex", 1, [](Interpreter& interp, Value const& this_value, Args args) { return visit_elements(interp, this_value, args, Visit::FindIndex); });
    define_method(in, prototype, "findLast", 1, [](Interpreter& interp, Value const& this_value, Args args) { return visit_elements(interp, this_value, args, Visit::FindLast); });
    define_method(in, prototype, "findLastIndex", 1, [](Interpreter& interp, Value const& this_value, Args args) { return visit_elements(interp, this_value, args, Visit::FindLastIndex); });
    define_method(in, prototype, "flat", 0, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.13.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        double depth = 1;
        if (!argument(args, 0).is_undefined()) {
            std::optional<double> const given = interp.to_integer_or_infinity(argument(args, 0));
            if (!given)
                return std::nullopt;
            depth = *given < 0 ? 0 : *given;
        }
        std::optional<Value> const created = array_species_create(interp, *subject->object, 0);
        if (!created)
            return std::nullopt;
        interp.root(*created);
        if (!flatten_into(interp, *created->as_object(), *subject->object, subject->length, 0, depth, Value::undefined(), Value::undefined()))
            return std::nullopt;
        return *created;
    });
    define_method(in, prototype, "flatMap", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.14.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        std::optional<Value> const mapper = require_callable(interp, argument(args, 0));
        if (!mapper)
            return std::nullopt;
        std::optional<Value> const created = array_species_create(interp, *subject->object, 0);
        if (!created)
            return std::nullopt;
        interp.root(*created);
        if (!flatten_into(interp, *created->as_object(), *subject->object, subject->length, 0, 1, *mapper, argument(args, 1)))
            return std::nullopt;
        return *created;
    });
    define_method(in, prototype, "forEach", 1, [](Interpreter& interp, Value const& this_value, Args args) { return visit_elements(interp, this_value, args, Visit::ForEach); });
    define_method(in, prototype, "includes", 1, [](Interpreter& interp, Value const& this_value, Args args) { return index_of(interp, this_value, args, false, true); });
    define_method(in, prototype, "indexOf", 1, [](Interpreter& interp, Value const& this_value, Args args) { return index_of(interp, this_value, args, false, false); });
    define_method(in, prototype, "join", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        std::u16string separator = u",";
        if (!argument(args, 0).is_undefined()) {
            std::optional<JsString*> const text = interp.to_string(argument(args, 0));
            if (!text)
                return std::nullopt;
            separator = (*text)->data();
        }
        return join_elements(interp, *subject->object, subject->length, separator, false);
    });
    define_method(in, prototype, "lastIndexOf", 1, [](Interpreter& interp, Value const& this_value, Args args) { return index_of(interp, this_value, args, true, false); });
    define_method(in, prototype, "map", 1, [](Interpreter& interp, Value const& this_value, Args args) { return visit_elements(interp, this_value, args, Visit::Map); });
    define_method(in, prototype, "pop", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §23.1.3.22.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        Object& object = *subject->object;
        if (subject->length == 0) {
            if (!set_length(interp, object, 0))
                return std::nullopt;
            return Value::undefined();
        }
        double const last = subject->length - 1;
        std::optional<Value> const element = get_at(interp, object, last);
        if (!element)
            return std::nullopt;
        interp.root(*element);
        if (!delete_at(interp, object, last))
            return std::nullopt;
        if (!set_length(interp, object, last))
            return std::nullopt;
        return *element;
    });
    define_method(in, prototype, "push", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.23.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        Object& object = *subject->object;
        double length = subject->length;
        if (length + static_cast<double>(args.size()) > max_safe_integer)
            return interp.throw_type_error("Pushing " + std::to_string(args.size()) + " elements on an array-like of length " + number_to_utf8(length) + " is disallowed, as the total surpasses 2**53-1");
        for (Value const& arg : args) {
            if (!set_at(interp, object, length, arg))
                return std::nullopt;
            ++length;
        }
        if (!set_length(interp, object, length))
            return std::nullopt;
        return Value::number(length);
    });
    define_method(in, prototype, "reduce", 1, [](Interpreter& interp, Value const& this_value, Args args) { return reduce_elements(interp, this_value, args, false); });
    define_method(in, prototype, "reduceRight", 1, [](Interpreter& interp, Value const& this_value, Args args) { return reduce_elements(interp, this_value, args, true); });
    define_method(in, prototype, "reverse", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §23.1.3.26: pairs swap from both ends, holes staying holes.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        Object& object = *subject->object;
        double const length = subject->length;
        double const middle = std::floor(length / 2);
        for (double lower = 0; lower != middle; ++lower) {
            Interpreter::Roots const pair_roots(interp);
            double const upper = length - lower - 1;
            bool const lower_exists = has_at(interp, object, lower);
            Value lower_value;
            if (lower_exists) {
                std::optional<Value> const value = get_at(interp, object, lower);
                if (!value)
                    return std::nullopt;
                lower_value = *value;
                interp.root(lower_value);
            }
            bool const upper_exists = has_at(interp, object, upper);
            Value upper_value;
            if (upper_exists) {
                std::optional<Value> const value = get_at(interp, object, upper);
                if (!value)
                    return std::nullopt;
                upper_value = *value;
                interp.root(upper_value);
            }
            if (lower_exists && upper_exists) {
                if (!set_at(interp, object, lower, upper_value) || !set_at(interp, object, upper, lower_value))
                    return std::nullopt;
            } else if (upper_exists) {
                if (!set_at(interp, object, lower, upper_value) || !delete_at(interp, object, upper))
                    return std::nullopt;
            } else if (lower_exists) {
                if (!delete_at(interp, object, lower) || !set_at(interp, object, upper, lower_value))
                    return std::nullopt;
            }
        }
        return Value::object(&object);
    });
    define_method(in, prototype, "shift", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §23.1.3.27.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        Object& object = *subject->object;
        double const length = subject->length;
        if (length == 0) {
            if (!set_length(interp, object, 0))
                return std::nullopt;
            return Value::undefined();
        }
        std::optional<Value> const first = get_at(interp, object, 0);
        if (!first)
            return std::nullopt;
        interp.root(*first);
        for (double k = 1; k < length; ++k) {
            if (has_at(interp, object, k)) {
                std::optional<Value> const element = get_at(interp, object, k);
                if (!element)
                    return std::nullopt;
                if (!set_at(interp, object, k - 1, *element))
                    return std::nullopt;
            } else if (!delete_at(interp, object, k - 1)) {
                return std::nullopt;
            }
        }
        if (!delete_at(interp, object, length - 1))
            return std::nullopt;
        if (!set_length(interp, object, length - 1))
            return std::nullopt;
        return *first;
    });
    define_method(in, prototype, "slice", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.28.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        Object& object = *subject->object;
        double const length = subject->length;
        std::optional<double> const start = relative_index(interp, argument(args, 0), length, 0);
        if (!start)
            return std::nullopt;
        std::optional<double> const end = relative_index(interp, argument(args, 1), length, length);
        if (!end)
            return std::nullopt;
        double const count = std::max(*end - *start, 0.0);
        std::optional<Value> const created = array_species_create(interp, object, count);
        if (!created)
            return std::nullopt;
        interp.root(*created);
        Object& result = *created->as_object();
        double n = 0;
        for (double k = *start; k < *end; ++k, ++n) {
            if (!has_at(interp, object, k))
                continue;
            std::optional<Value> const element = get_at(interp, object, k);
            if (!element)
                return std::nullopt;
            if (!create_at(interp, result, n, *element))
                return std::nullopt;
        }
        if (!set_length(interp, result, n))
            return std::nullopt;
        return *created;
    });
    define_method(in, prototype, "some", 1, [](Interpreter& interp, Value const& this_value, Args args) { return visit_elements(interp, this_value, args, Visit::Some); });
    define_method(in, prototype, "sort", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.30: the comparator is checked before `this` is touched.
        Value const comparefn = argument(args, 0);
        if (!comparefn.is_undefined() && !Interpreter::is_callable(comparefn))
            return interp.throw_type_error("The comparison function must be either a function or undefined");
        Interpreter::Roots const roots(interp);
        interp.root(comparefn);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        Object& object = *subject->object;
        double const length = subject->length;
        std::optional<std::vector<Value>> const sorted = sorted_values(interp, object, length, comparefn, true);
        if (!sorted)
            return std::nullopt;
        double k = 0;
        for (Value const& value : *sorted) {
            if (!set_at(interp, object, k, value))
                return std::nullopt;
            ++k;
        }
        for (; k < length; ++k) {
            if (!delete_at(interp, object, k))
                return std::nullopt;
        }
        return Value::object(&object);
    });
    define_method(in, prototype, "splice", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.31.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        Object& object = *subject->object;
        double const length = subject->length;
        std::optional<double> const start = relative_index(interp, argument(args, 0), length, 0);
        if (!start)
            return std::nullopt;
        double insert_count = 0;
        double delete_count = 0;
        if (args.size() == 1) {
            delete_count = length - *start;
        } else if (args.size() > 1) {
            insert_count = static_cast<double>(args.size() - 2);
            std::optional<double> const requested = interp.to_integer_or_infinity(args[1]);
            if (!requested)
                return std::nullopt;
            delete_count = std::min(std::max(*requested, 0.0), length - *start);
        }
        if (length + insert_count - delete_count > max_safe_integer)
            return interp.throw_type_error("array too long");
        std::optional<Value> const created = array_species_create(interp, object, delete_count);
        if (!created)
            return std::nullopt;
        interp.root(*created);
        Object& removed = *created->as_object();
        for (double k = 0; k < delete_count; ++k) {
            double const from = *start + k;
            if (!has_at(interp, object, from))
                continue;
            std::optional<Value> const element = get_at(interp, object, from);
            if (!element)
                return std::nullopt;
            if (!create_at(interp, removed, k, *element))
                return std::nullopt;
        }
        if (!set_length(interp, removed, delete_count))
            return std::nullopt;
        if (insert_count < delete_count) {
            for (double k = *start; k < length - delete_count; ++k) {
                double const from = k + delete_count;
                double const to = k + insert_count;
                if (has_at(interp, object, from)) {
                    std::optional<Value> const element = get_at(interp, object, from);
                    if (!element)
                        return std::nullopt;
                    if (!set_at(interp, object, to, *element))
                        return std::nullopt;
                } else if (!delete_at(interp, object, to)) {
                    return std::nullopt;
                }
            }
            for (double k = length; k > length - delete_count + insert_count; --k) {
                if (!delete_at(interp, object, k - 1))
                    return std::nullopt;
            }
        } else if (insert_count > delete_count) {
            for (double k = length - delete_count; k > *start; --k) {
                double const from = k + delete_count - 1;
                double const to = k + insert_count - 1;
                if (has_at(interp, object, from)) {
                    std::optional<Value> const element = get_at(interp, object, from);
                    if (!element)
                        return std::nullopt;
                    if (!set_at(interp, object, to, *element))
                        return std::nullopt;
                } else if (!delete_at(interp, object, to)) {
                    return std::nullopt;
                }
            }
        }
        for (std::size_t k = 2; k < args.size(); ++k) {
            if (!set_at(interp, object, *start + static_cast<double>(k - 2), args[k]))
                return std::nullopt;
        }
        if (!set_length(interp, object, length - delete_count + insert_count))
            return std::nullopt;
        return *created;
    });
    define_method(in, prototype, "toLocaleString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        return join_elements(interp, *subject->object, subject->length, u",", true);
    });
    define_method(in, prototype, "toReversed", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §23.1.3.33.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        std::optional<ArrayObject*> const array = array_create(interp, subject->length);
        if (!array)
            return std::nullopt;
        interp.root(Value::object(*array));
        for (double k = 0; k < subject->length; ++k) {
            std::optional<Value> const element = get_at(interp, *subject->object, subject->length - k - 1);
            if (!element)
                return std::nullopt;
            if (!create_at(interp, **array, k, *element))
                return std::nullopt;
        }
        return Value::object(*array);
    });
    define_method(in, prototype, "toSorted", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.34: holes read as undefined and sort to the end.
        Value const comparefn = argument(args, 0);
        if (!comparefn.is_undefined() && !Interpreter::is_callable(comparefn))
            return interp.throw_type_error("The comparison function must be either a function or undefined");
        Interpreter::Roots const roots(interp);
        interp.root(comparefn);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        std::optional<ArrayObject*> const array = array_create(interp, subject->length);
        if (!array)
            return std::nullopt;
        interp.root(Value::object(*array));
        std::optional<std::vector<Value>> const sorted = sorted_values(interp, *subject->object, subject->length, comparefn, false);
        if (!sorted)
            return std::nullopt;
        for (std::size_t k = 0; k < sorted->size(); ++k) {
            if (!create_at(interp, **array, static_cast<double>(k), (*sorted)[k]))
                return std::nullopt;
        }
        return Value::object(*array);
    });
    define_method(in, prototype, "toSpliced", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.35.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        Object& object = *subject->object;
        double const length = subject->length;
        std::optional<double> const start = relative_index(interp, argument(args, 0), length, 0);
        if (!start)
            return std::nullopt;
        double insert_count = 0;
        double skip_count = 0;
        if (args.size() == 1) {
            skip_count = length - *start;
        } else if (args.size() > 1) {
            insert_count = static_cast<double>(args.size() - 2);
            std::optional<double> const requested = interp.to_integer_or_infinity(args[1]);
            if (!requested)
                return std::nullopt;
            skip_count = std::min(std::max(*requested, 0.0), length - *start);
        }
        double const new_length = length + insert_count - skip_count;
        if (new_length > max_safe_integer)
            return interp.throw_type_error("array too long");
        std::optional<ArrayObject*> const array = array_create(interp, new_length);
        if (!array)
            return std::nullopt;
        interp.root(Value::object(*array));
        double k = 0;
        for (; k < *start; ++k) {
            std::optional<Value> const element = get_at(interp, object, k);
            if (!element)
                return std::nullopt;
            if (!create_at(interp, **array, k, *element))
                return std::nullopt;
        }
        for (std::size_t j = 2; j < args.size(); ++j, ++k) {
            if (!create_at(interp, **array, k, args[j]))
                return std::nullopt;
        }
        for (double from = *start + skip_count; k < new_length; ++k, ++from) {
            std::optional<Value> const element = get_at(interp, object, from);
            if (!element)
                return std::nullopt;
            if (!create_at(interp, **array, k, *element))
                return std::nullopt;
        }
        return Value::object(*array);
    });
    define_method(in, prototype, "toString", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        // §23.1.3.36: join when there is one, Object.prototype.toString
        // otherwise.
        Interpreter::Roots const roots(interp);
        std::optional<Object*> const object = interp.to_object(this_value);
        if (!object)
            return std::nullopt;
        interp.root(Value::object(*object));
        std::optional<Value> const join = interp.get(**object, interp.key("join"));
        if (!join)
            return std::nullopt;
        if (Interpreter::is_callable(*join))
            return interp.call(*join, Value::object(*object), {});
        std::optional<Value> const to_string = interp.get(*interp.intrinsics().object_prototype, PropertyKey::atom(interp.atoms().to_string));
        if (!to_string)
            return std::nullopt;
        return interp.call(*to_string, Value::object(*object), {});
    });
    define_method(in, prototype, "unshift", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.37.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        Object& object = *subject->object;
        double const length = subject->length;
        auto const count = static_cast<double>(args.size());
        if (count > 0) {
            if (length + count > max_safe_integer)
                return interp.throw_type_error("array too long");
            for (double k = length; k > 0; --k) {
                double const from = k - 1;
                double const to = k + count - 1;
                if (has_at(interp, object, from)) {
                    std::optional<Value> const element = get_at(interp, object, from);
                    if (!element)
                        return std::nullopt;
                    if (!set_at(interp, object, to, *element))
                        return std::nullopt;
                } else if (!delete_at(interp, object, to)) {
                    return std::nullopt;
                }
            }
            for (std::size_t j = 0; j < args.size(); ++j) {
                if (!set_at(interp, object, static_cast<double>(j), args[j]))
                    return std::nullopt;
            }
        }
        if (!set_length(interp, object, length + count))
            return std::nullopt;
        return Value::number(length + count);
    });
    define_method(in, prototype, "with", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §23.1.3.39.
        Interpreter::Roots const roots(interp);
        std::optional<Subject> const subject = subject_of(interp, this_value);
        if (!subject)
            return std::nullopt;
        double const length = subject->length;
        std::optional<double> const relative = interp.to_integer_or_infinity(argument(args, 0));
        if (!relative)
            return std::nullopt;
        double const actual = *relative >= 0 ? *relative : length + *relative;
        if (actual >= length || actual < 0)
            return interp.throw_range_error("Invalid index");
        Value const value = argument(args, 1);
        interp.root(value);
        std::optional<ArrayObject*> const array = array_create(interp, length);
        if (!array)
            return std::nullopt;
        interp.root(Value::object(*array));
        for (double k = 0; k < length; ++k) {
            Value element = value;
            if (k != actual) {
                std::optional<Value> const read = get_at(interp, *subject->object, k);
                if (!read)
                    return std::nullopt;
                element = *read;
            }
            if (!create_at(interp, **array, k, element))
                return std::nullopt;
        }
        return Value::object(*array);
    });
}

} // namespace

void install_array(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    Heap::NoCollect const guard(in.heap());
    Object& prototype = *i.array_prototype;
    NativeFunction* constructor = in.new_native(
        "Array", 1,
        [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
            return construct_array(interp, args, interp.intrinsics().array_constructor);
        },
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            return construct_array(interp, args, new_target);
        });
    i.array_constructor = constructor;
    constructor->put(PropertyKey::atom(in.atoms().prototype), Value::object(&prototype), frozen_attributes);
    prototype.put(PropertyKey::atom(in.atoms().constructor), Value::object(constructor), builtin_attributes);
    in.global()->put(in.key("Array"), Value::object(constructor), builtin_attributes);

    // §23.1.2.5: Array[@@species] answers the constructor itself.
    {
        Heap::NoCollect const species_guard(in.heap());
        NativeFunction* getter = in.new_native("get [Symbol.species]", 0, [](Interpreter&, Value const& this_value, Args) -> std::optional<Value> { return this_value; });
        constructor->put_accessor(PropertyKey::symbol(in.atoms().symbol_species), getter, nullptr, Configurable);
    }
    define_method(in, *constructor, "isArray", 1, [](Interpreter&, Value const&, Args args) -> std::optional<Value> {
        return Value::boolean(Interpreter::is_array(argument(args, 0)));
    });
    define_method(in, *constructor, "of", 0, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        // §23.1.2.3, over the realm's Array (no subclass constructors).
        Interpreter::Roots const roots(interp);
        std::vector<Value> values(args.begin(), args.end());
        for (Value const& value : values)
            interp.root(value);
        return array_from_values(interp, values);
    });
    define_method(in, *constructor, "from", 1, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        // §23.1.2.1 over an array-like: the realm has no iterator
        // protocol yet, so a string is read by code unit and an iterable
        // object by its length.
        Interpreter::Roots const roots(interp);
        Value const items = argument(args, 0);
        Value const mapper = argument(args, 1);
        if (!mapper.is_undefined() && !Interpreter::is_callable(mapper))
            return interp.throw_type_error(interp.describe(mapper) + " is not a function");
        Value const this_argument = argument(args, 2);
        interp.root(items);
        interp.root(mapper);
        interp.root(this_argument);
        std::optional<Object*> const array_like = interp.to_object(items);
        if (!array_like)
            return std::nullopt;
        interp.root(Value::object(*array_like));
        std::optional<double> const length = interp.length_of_array_like(**array_like);
        if (!length)
            return std::nullopt;
        std::optional<ArrayObject*> const array = array_create(interp, *length);
        if (!array)
            return std::nullopt;
        interp.root(Value::object(*array));
        for (double k = 0; k < *length; ++k) {
            Interpreter::Roots const element_roots(interp);
            std::optional<Value> element = get_at(interp, **array_like, k);
            if (!element)
                return std::nullopt;
            if (!mapper.is_undefined()) {
                interp.root(*element);
                Value const arguments[2] = { *element, Value::number(k) };
                element = interp.call(mapper, this_argument, arguments);
                if (!element)
                    return std::nullopt;
            }
            if (!create_at(interp, **array, k, *element))
                return std::nullopt;
        }
        if (!set_length(interp, **array, *length))
            return std::nullopt;
        return Value::object(*array);
    });
    install_prototype(in, prototype);
}

}
