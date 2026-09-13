#include "js/Runtime.h"

// Iterators (§7.4, §27.1): the abstract operations the evaluator and the
// library walk an iterable with — GetIterator, IteratorStepValue,
// IteratorClose, IteratorToList — then %IteratorPrototype% and the array
// and string iterators (§23.1.5, §22.1.5) with the members that hand them
// out: Array.prototype.entries/keys/values and @@iterator, and
// String.prototype[@@iterator], which walks code points.

#include "js/Object.h"
#include "js/Vm.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

// ------------------------------------------------- the abstract operations

Object* Interpreter::create_iter_result(Value const& value, bool done)
{
    // CreateIterResultObject (§7.4.14): `value`, then `done`.
    Heap::NoCollect const guard(*m_heap);
    Object* result = new_object();
    result->put(PropertyKey::atom(atoms().value), value);
    result->put(PropertyKey::atom(atoms().done), Value::boolean(done));
    return result;
}

std::optional<IteratorRecord> Interpreter::get_iterator_from_method(Value const& iterable, Value const& method)
{
    // GetIteratorFromMethod (§7.4.2): the method, called on the iterable
    // with no arguments, must answer an object; its `next` is read once.
    Roots const roots(*this);
    root(iterable);
    root(method);
    std::optional<Value> const iterator = call(method, iterable, {});
    if (!iterator)
        return std::nullopt;
    if (!iterator->is_object())
        return throw_type_error("Result of the Symbol.iterator method is not an object");
    root(*iterator);
    std::optional<Value> const next = get(*iterator->as_object(), PropertyKey::atom(atoms().next));
    if (!next)
        return std::nullopt;
    IteratorRecord record;
    record.iterator = *iterator;
    record.next_method = *next;
    return record;
}

std::optional<IteratorRecord> Interpreter::get_iterator(Value const& iterable)
{
    // GetIterator (§7.4.3), the sync kind: @@iterator, absent or nullish
    // means the value is not iterable.
    Roots const roots(*this);
    root(iterable);
    std::optional<Value> const method = get_method(iterable, PropertyKey::symbol(atoms().symbol_iterator));
    if (!method)
        return std::nullopt;
    if (method->is_undefined())
        return throw_type_error(describe(iterable) + " is not iterable");
    return get_iterator_from_method(iterable, *method);
}

std::optional<IteratorRecord> Interpreter::get_async_iterator(Value const& iterable)
{
    // GetIterator (§7.4.3), the async kind: @@asyncIterator when the value
    // has one; else its sync iterator behind an async-from-sync iterator
    // (CreateAsyncFromSyncIterator, §27.1.6.1), whose `next` is read once.
    Roots const roots(*this);
    root(iterable);
    std::optional<Value> const method = get_method(iterable, PropertyKey::symbol(atoms().symbol_async_iterator));
    if (!method)
        return std::nullopt;
    if (!method->is_undefined())
        return get_iterator_from_method(iterable, *method);
    std::optional<IteratorRecord> const sync = get_iterator(iterable);
    if (!sync)
        return std::nullopt;
    root(sync->iterator);
    root(sync->next_method);
    auto* wrapper = m_heap->allocate<AsyncFromSyncIteratorObject>(m_realm->intrinsics.async_from_sync_iterator_prototype, *sync);
    root(Value::object(wrapper));
    std::optional<Value> const next = get(*wrapper, PropertyKey::atom(atoms().next));
    if (!next)
        return std::nullopt;
    IteratorRecord record;
    record.iterator = Value::object(wrapper);
    record.next_method = *next;
    return record;
}

std::optional<bool> Interpreter::iterator_step(IteratorRecord& record, Value& out)
{
    // IteratorStepValue (§7.4.9). A throw — from next(), from a result
    // that is not an object, or from reading it — marks the record done,
    // so the caller knows not to close it.
    Roots const roots(*this);
    root(record.iterator);
    root(record.next_method);
    std::optional<Value> const result = call(record.next_method, record.iterator, {});
    if (!result) {
        record.done = true;
        return std::nullopt;
    }
    if (!result->is_object()) {
        record.done = true;
        return throw_type_error("Iterator result " + describe(*result) + " is not an object");
    }
    root(*result);
    std::optional<Value> const done = get(*result->as_object(), PropertyKey::atom(atoms().done));
    if (!done) {
        record.done = true;
        return std::nullopt;
    }
    if (to_boolean(*done)) {
        record.done = true;
        return false;
    }
    std::optional<Value> const value = get(*result->as_object(), PropertyKey::atom(atoms().value));
    if (!value) {
        record.done = true;
        return std::nullopt;
    }
    out = *value;
    return true;
}

bool Interpreter::iterator_close(IteratorRecord const& record, bool throwing)
{
    // IteratorClose (§7.4.11). return() is looked up and called. With an
    // exception already pending, that exception is the outcome whatever
    // happens here; otherwise a throw from the lookup or the call, or a
    // result that is not an object, becomes the outcome.
    Roots const roots(*this);
    root(record.iterator);
    Value pending;
    if (throwing) {
        pending = take_exception();
        root(pending);
    }
    std::optional<Value> const method = get_method(record.iterator, key("return"));
    std::optional<Value> result;
    if (method && !method->is_undefined()) {
        root(*method);
        result = call(*method, record.iterator, {});
    }
    if (throwing) {
        throw_value(pending);
        return false;
    }
    if (!method)
        return false;
    if (method->is_undefined())
        return true;
    if (!result)
        return false;
    if (!result->is_object()) {
        throw_type_error("Iterator result " + describe(*result) + " is not an object");
        return false;
    }
    return true;
}

std::optional<std::vector<Value>> Interpreter::iterable_to_list(Value const& iterable)
{
    // IteratorToList over GetIterator (§7.4.12): every value, in order.
    // Each is rooted until the enclosing scope closes; the caller roots
    // the list it keeps.
    std::optional<IteratorRecord> record = get_iterator(iterable);
    if (!record)
        return std::nullopt;
    Roots const roots(*this);
    root(record->iterator);
    root(record->next_method);
    std::vector<Value> values;
    while (true) {
        Value value;
        std::optional<bool> const stepped = iterator_step(*record, value);
        if (!stepped)
            return std::nullopt;
        if (!*stepped)
            return values;
        root(value);
        values.push_back(value);
    }
}

// ---------------------------------------------------------- the library

namespace {

std::optional<Value> array_iterator_next(Interpreter& in, Value const& this_value, Args)
{
    // %ArrayIteratorPrototype%.next (§23.1.5.2.1): the length is read
    // again at every step, so an array that grows while it is walked is
    // walked to its new end; once past it the array is let go.
    if (!this_value.is_object() || this_value.as_object()->class_id() != Object::Class::ArrayIterator)
        return in.throw_type_error("next method called on incompatible receiver " + in.describe(this_value));
    auto& iterator = *static_cast<ArrayIteratorObject*>(this_value.as_object());
    Interpreter::Roots const roots(in);
    in.root(this_value);
    Object* const array = iterator.iterated();
    if (array == nullptr)
        return Value::object(in.create_iter_result(Value::undefined(), true));
    double length = 0;
    if (array->class_id() == Object::Class::TypedArray) {
        // A typed array's length is its own (§23.1.5.1 step 1.b.iv), and
        // a view gone out of bounds ends the walk with a TypeError.
        auto const& typed = static_cast<TypedArrayObject const&>(*array);
        if (typed.is_out_of_bounds())
            return in.throw_type_error("Cannot iterate a detached or out-of-bounds TypedArray");
        length = static_cast<double>(typed.length());
    } else {
        std::optional<double> const array_length = in.length_of_array_like(*array);
        if (!array_length)
            return std::nullopt;
        length = *array_length;
    }
    double const index = iterator.next_index();
    if (index >= length) {
        iterator.finish();
        return Value::object(in.create_iter_result(Value::undefined(), true));
    }
    iterator.set_next_index(index + 1);
    if (iterator.kind() == ArrayIteratorObject::Kind::Keys)
        return Value::object(in.create_iter_result(Value::number(index), false));
    std::optional<Value> const element = in.get(*array, in.heap().key(index));
    if (!element)
        return std::nullopt;
    if (iterator.kind() == ArrayIteratorObject::Kind::Values)
        return Value::object(in.create_iter_result(*element, false));
    in.root(*element);
    Value const pair[2] = { Value::number(index), *element };
    ArrayObject* entry = in.new_array(pair);
    in.root(Value::object(entry));
    return Value::object(in.create_iter_result(Value::object(entry), false));
}

std::optional<Value> make_array_iterator(Interpreter& in, Value const& this_value, ArrayIteratorObject::Kind kind)
{
    // CreateArrayIterator over ToObject(this) (§23.1.3.5, .19, .37).
    Interpreter::Roots const roots(in);
    std::optional<Object*> const object = in.to_object(this_value);
    if (!object)
        return std::nullopt;
    in.root(Value::object(*object));
    return Value::object(in.heap().allocate<ArrayIteratorObject>(in.intrinsics().array_iterator_prototype, *object, kind));
}

std::optional<Value> string_iterator_next(Interpreter& in, Value const& this_value, Args)
{
    // %StringIteratorPrototype%.next (§22.1.5.1.1): one code point per
    // step, a surrogate pair as one string of two units.
    if (!this_value.is_object() || this_value.as_object()->class_id() != Object::Class::StringIterator)
        return in.throw_type_error("next method called on incompatible receiver " + in.describe(this_value));
    auto& iterator = *static_cast<StringIteratorObject*>(this_value.as_object());
    JsString* const string = iterator.string();
    if (string == nullptr)
        return Value::object(in.create_iter_result(Value::undefined(), true));
    std::u16string_view const text = string->view();
    std::size_t const position = iterator.position();
    if (position >= text.size()) {
        iterator.finish();
        return Value::object(in.create_iter_result(Value::undefined(), true));
    }
    std::size_t count = 1;
    char16_t const first = text[position];
    if (first >= 0xD800 && first <= 0xDBFF && position + 1 < text.size()) {
        char16_t const second = text[position + 1];
        if (second >= 0xDC00 && second <= 0xDFFF)
            count = 2;
    }
    iterator.set_position(position + count);
    Heap::NoCollect const guard(in.heap());
    JsString* piece = in.heap().string(text.substr(position, count));
    return Value::object(in.create_iter_result(Value::string(piece), false));
}

// ---- %AsyncFromSyncIteratorPrototype% (§27.1.6.2)

// The receiver: made by the engine alone, so anything else is a TypeError.
std::optional<AsyncFromSyncIteratorObject*> this_async_from_sync(Interpreter& in, Value const& this_value)
{
    if (!this_value.is_object() || this_value.as_object()->class_id() != Object::Class::AsyncFromSyncIterator)
        return in.throw_type_error("Receiver is not an async-from-sync iterator");
    return static_cast<AsyncFromSyncIteratorObject*>(this_value.as_object());
}

// IfAbruptRejectPromise: the pending throw becomes the promise's rejection.
std::optional<Value> reject_with_exception(Interpreter& in, PromiseCapability const& capability)
{
    Value const thrown = in.take_exception();
    Interpreter::Roots const roots(in);
    in.root(thrown);
    Value const arguments[1] = { thrown };
    if (!in.call(capability.reject, Value::undefined(), arguments))
        return std::nullopt;
    return capability.promise;
}

// AsyncFromSyncIteratorContinuation (§27.1.6.4): the sync result's value
// is awaited through a promise, and handed back as a result with the sync
// result's `done`; a value that rejects closes the sync iterator when the
// result was not done and the caller asked for that.
std::optional<Value> async_from_sync_continuation(Interpreter& in, Value const& result, PromiseCapability const& capability,
    AsyncFromSyncIteratorObject& self, bool close_on_rejection)
{
    Interpreter::Roots const roots(in);
    in.root(result);
    in.root(capability.promise);
    in.root(capability.resolve);
    in.root(capability.reject);
    in.root(Value::object(&self));
    std::optional<Value> const done_value = in.get(*result.as_object(), PropertyKey::atom(in.atoms().done));
    if (!done_value)
        return reject_with_exception(in, capability);
    bool const done = Interpreter::to_boolean(*done_value);
    std::optional<Value> const value = in.get(*result.as_object(), PropertyKey::atom(in.atoms().value));
    if (!value)
        return reject_with_exception(in, capability);
    in.root(*value);
    std::optional<Value> const wrapper = promise_resolve(in, Value::object(in.intrinsics().promise_constructor), *value);
    if (!wrapper) {
        if (!done && close_on_rejection)
            in.iterator_close(self.record(), true); // the pending throw stays
        return reject_with_exception(in, capability);
    }
    in.root(*wrapper);
    ClosureFunction* unwrap = in.new_closure("", 1, { Value::boolean(done) },
        [](Interpreter& interp, ClosureFunction& function, Value const&, Args arguments) -> std::optional<Value> {
            return Value::object(interp.create_iter_result(argument(arguments, 0), function.slot(0).as_boolean()));
        });
    in.root(Value::object(unwrap));
    Value on_rejected = Value::undefined();
    if (!done && close_on_rejection) {
        ClosureFunction* closer = in.new_closure("", 1, { Value::object(&self) },
            [](Interpreter& interp, ClosureFunction& function, Value const&, Args arguments) -> std::optional<Value> {
                // The sync iterator is closed with the rejection as the
                // pending throw, which stays the outcome.
                auto* iterator = static_cast<AsyncFromSyncIteratorObject*>(function.slot(0).as_object());
                interp.throw_value(argument(arguments, 0));
                interp.iterator_close(iterator->record(), true);
                return std::nullopt;
            });
        on_rejected = Value::object(closer);
        in.root(on_rejected);
    }
    perform_then(in, *static_cast<PromiseObject*>(wrapper->as_object()), Value::object(unwrap), on_rejected, capability);
    return capability.promise;
}

std::optional<Value> async_from_sync_next(Interpreter& in, Value const& this_value, Args arguments)
{
    // §27.1.6.2.1: the sync next, with the value when one was given.
    std::optional<AsyncFromSyncIteratorObject*> const self = this_async_from_sync(in, this_value);
    if (!self)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    std::optional<PromiseCapability> const capability = new_promise_capability(in, Value::object(in.intrinsics().promise_constructor));
    if (!capability)
        return std::nullopt;
    in.root(capability->promise);
    in.root(capability->resolve);
    in.root(capability->reject);
    IteratorRecord& record = (*self)->record();
    std::optional<Value> const result = arguments.empty() ? in.call(record.next_method, record.iterator, {})
                                                          : in.call(record.next_method, record.iterator, arguments.subspan(0, 1));
    if (!result)
        return reject_with_exception(in, *capability);
    if (!result->is_object()) {
        in.throw_type_error("Iterator result " + in.describe(*result) + " is not an object");
        return reject_with_exception(in, *capability);
    }
    return async_from_sync_continuation(in, *result, *capability, **self, true);
}

std::optional<Value> async_from_sync_return(Interpreter& in, Value const& this_value, Args arguments)
{
    // §27.1.6.2.2: no return method means done at once with the value.
    std::optional<AsyncFromSyncIteratorObject*> const self = this_async_from_sync(in, this_value);
    if (!self)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    std::optional<PromiseCapability> const capability = new_promise_capability(in, Value::object(in.intrinsics().promise_constructor));
    if (!capability)
        return std::nullopt;
    in.root(capability->promise);
    in.root(capability->resolve);
    in.root(capability->reject);
    IteratorRecord& record = (*self)->record();
    std::optional<Value> const method = in.get_method(record.iterator, in.key("return"));
    if (!method)
        return reject_with_exception(in, *capability);
    if (method->is_undefined()) {
        Object* iter_result = in.create_iter_result(argument(arguments, 0), true);
        Value const resolve_arguments[1] = { Value::object(iter_result) };
        if (!in.call(capability->resolve, Value::undefined(), resolve_arguments))
            return std::nullopt;
        return capability->promise;
    }
    in.root(*method);
    std::optional<Value> const result = arguments.empty() ? in.call(*method, record.iterator, {})
                                                          : in.call(*method, record.iterator, arguments.subspan(0, 1));
    if (!result)
        return reject_with_exception(in, *capability);
    if (!result->is_object()) {
        in.throw_type_error("Iterator result " + in.describe(*result) + " is not an object");
        return reject_with_exception(in, *capability);
    }
    return async_from_sync_continuation(in, *result, *capability, **self, false);
}

std::optional<Value> async_from_sync_throw(Interpreter& in, Value const& this_value, Args arguments)
{
    // §27.1.6.2.3, with ES2024's rule: a sync iterator with no throw
    // method is closed, and the answer is a TypeError.
    std::optional<AsyncFromSyncIteratorObject*> const self = this_async_from_sync(in, this_value);
    if (!self)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    std::optional<PromiseCapability> const capability = new_promise_capability(in, Value::object(in.intrinsics().promise_constructor));
    if (!capability)
        return std::nullopt;
    in.root(capability->promise);
    in.root(capability->resolve);
    in.root(capability->reject);
    IteratorRecord& record = (*self)->record();
    std::optional<Value> const method = in.get_method(record.iterator, in.key("throw"));
    if (!method)
        return reject_with_exception(in, *capability);
    if (method->is_undefined()) {
        if (!in.iterator_close(record, false))
            return reject_with_exception(in, *capability);
        in.throw_type_error("The iterator does not provide a 'throw' method");
        return reject_with_exception(in, *capability);
    }
    in.root(*method);
    std::optional<Value> const result = arguments.empty() ? in.call(*method, record.iterator, {})
                                                          : in.call(*method, record.iterator, arguments.subspan(0, 1));
    if (!result)
        return reject_with_exception(in, *capability);
    if (!result->is_object()) {
        in.throw_type_error("Iterator result " + in.describe(*result) + " is not an object");
        return reject_with_exception(in, *capability);
    }
    return async_from_sync_continuation(in, *result, *capability, **self, true);
}

} // namespace

void install_iterators(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    WellKnownAtoms const& atoms = in.atoms();
    Heap::NoCollect const guard(in.heap());

    // %IteratorPrototype% (§27.1.2): @@iterator answers the receiver, so
    // an iterator is itself iterable.
    i.iterator_prototype = in.new_object();
    {
        NativeFunction* self = in.new_native("[Symbol.iterator]", 0, [](Interpreter&, Value const& this_value, Args) -> std::optional<Value> {
            return this_value;
        });
        i.iterator_prototype->put(PropertyKey::symbol(atoms.symbol_iterator), Value::object(self), builtin_attributes);
    }

    // %AsyncIteratorPrototype% (§27.1.3): @@asyncIterator answers the
    // receiver. %AsyncFromSyncIteratorPrototype% (§27.1.6.2) hangs off it.
    i.async_iterator_prototype = in.new_object();
    {
        NativeFunction* self = in.new_native("[Symbol.asyncIterator]", 0, [](Interpreter&, Value const& this_value, Args) -> std::optional<Value> {
            return this_value;
        });
        i.async_iterator_prototype->put(PropertyKey::symbol(atoms.symbol_async_iterator), Value::object(self), builtin_attributes);
    }
    i.async_from_sync_iterator_prototype = in.new_object(i.async_iterator_prototype);
    define_method(in, *i.async_from_sync_iterator_prototype, "next", 1, async_from_sync_next);
    define_method(in, *i.async_from_sync_iterator_prototype, "return", 1, async_from_sync_return);
    define_method(in, *i.async_from_sync_iterator_prototype, "throw", 1, async_from_sync_throw);

    // %ArrayIteratorPrototype% (§23.1.5.2).
    i.array_iterator_prototype = in.new_object(i.iterator_prototype);
    define_method(in, *i.array_iterator_prototype, "next", 0, array_iterator_next);
    i.array_iterator_prototype->put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("Array Iterator")), Configurable);

    // %StringIteratorPrototype% (§22.1.5.1).
    i.string_iterator_prototype = in.new_object(i.iterator_prototype);
    define_method(in, *i.string_iterator_prototype, "next", 0, string_iterator_next);
    i.string_iterator_prototype->put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("String Iterator")), Configurable);

    // Array.prototype.entries, keys, values (§23.1.3.5, .19, .37), and
    // @@iterator, which is the very same function as values (§23.1.3.40).
    Object& array_prototype = *i.array_prototype;
    define_method(in, array_prototype, "entries", 0, [](Interpreter& interp, Value const& this_value, Args) {
        return make_array_iterator(interp, this_value, ArrayIteratorObject::Kind::Entries);
    });
    define_method(in, array_prototype, "keys", 0, [](Interpreter& interp, Value const& this_value, Args) {
        return make_array_iterator(interp, this_value, ArrayIteratorObject::Kind::Keys);
    });
    NativeFunction* values = define_method(in, array_prototype, "values", 0, [](Interpreter& interp, Value const& this_value, Args) {
        return make_array_iterator(interp, this_value, ArrayIteratorObject::Kind::Values);
    });
    array_prototype.put(PropertyKey::symbol(atoms.symbol_iterator), Value::object(values), builtin_attributes);
    i.array_prototype_values = values;

    // String.prototype[@@iterator] (§22.1.3.36).
    NativeFunction* string_iterator = in.new_native("[Symbol.iterator]", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<JsString*> const string = this_string_value(interp, this_value, "String.prototype[Symbol.iterator]");
        if (!string)
            return std::nullopt;
        Heap::NoCollect const string_guard(interp.heap());
        return Value::object(interp.heap().allocate<StringIteratorObject>(interp.intrinsics().string_iterator_prototype, *string));
    });
    i.string_prototype->put(PropertyKey::symbol(atoms.symbol_iterator), Value::object(string_iterator), builtin_attributes);
}

}
