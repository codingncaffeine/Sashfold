#include "js/Runtime.h"

// Explicit resource management (ECMA-262 2026: §20.5.9 SuppressedError,
// §27.4 DisposableStack, §27.5 AsyncDisposableStack, and the well-known
// symbols @@dispose and @@asyncDispose with the iterator prototypes'
// methods for them): a stack holds the resources a scope took and lets
// go of them in the reverse order, each by its own dispose method; an
// error thrown while another was already on its way is carried as a
// SuppressedError, the newer as `error`, the older as `suppressed`. The
// `using` and `await using` declarations that drive a stack from syntax
// are the parser's and the compiler's, and are not written yet.

#include "js/Object.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

namespace {

// One resource on a stack (a DisposableResource Record, §9.14): what to
// call and how. A used value's own method is called on the value; an
// adopted value's callback is called with the value; a deferred callback
// is called with nothing. A sync method on an async stack is called the
// same way, its result not awaited, as GetDisposeMethod's closure has it.
struct DisposeRecord {
    enum class Kind : std::uint8_t { Method, Adopted, Deferred };
    Kind kind = Kind::Method;
    bool sync_on_async = false;
    Value value;
    Value method;
};

class DisposableStackObject final : public Object {
public:
    DisposableStackObject(Object* prototype, bool async)
        : Object(prototype, Class::DisposableStack)
        , m_async(async)
    {
    }
    bool is_async() const { return m_async; }
    bool disposed = false;
    std::vector<DisposeRecord> records;
    void trace(Tracer& tracer) override
    {
        Object::trace(tracer);
        for (DisposeRecord const& record : records) {
            tracer.visit(record.value);
            tracer.visit(record.method);
        }
    }
    std::size_t size_in_bytes() const override { return Object::size_in_bytes() + records.size() * sizeof(DisposeRecord); }

private:
    bool m_async;
};

std::string_view stack_name(bool async)
{
    return async ? "AsyncDisposableStack" : "DisposableStack";
}

std::optional<DisposableStackObject*> this_stack(Interpreter& in, Value const& this_value, bool async, std::string_view method)
{
    if (this_value.is_object() && this_value.as_object()->class_id() == Object::Class::DisposableStack) {
        auto* stack = static_cast<DisposableStackObject*>(this_value.as_object());
        if (stack->is_async() == async)
            return stack;
    }
    return in.throw_type_error(std::string(stack_name(async)) + ".prototype." + std::string(method) + " called on incompatible receiver");
}

// GetDisposeMethod (§9.14.4): the value's @@asyncDispose for an async
// stack, else its @@dispose; undefined when it has neither.
std::optional<DisposeRecord> dispose_method_of(Interpreter& in, Value const& value, bool async)
{
    DisposeRecord record;
    record.value = value;
    if (async) {
        std::optional<Value> const method = in.get_method(value, PropertyKey::symbol(in.atoms().symbol_async_dispose));
        if (!method)
            return std::nullopt;
        if (!method->is_undefined()) {
            record.method = *method;
            return record;
        }
        record.sync_on_async = true;
    }
    std::optional<Value> const method = in.get_method(value, PropertyKey::symbol(in.atoms().symbol_dispose));
    if (!method)
        return std::nullopt;
    record.method = *method;
    return record;
}

// A SuppressedError made by the engine (§9.14.6 DisposeResources step
// 1.a.ii.2): the newer error and the one it suppressed, with the message
// the shipping engines give it.
Object* new_suppressed_error(Interpreter& in, Value const& error, Value const& suppressed)
{
    Heap::NoCollect const guard(in.heap());
    auto* made = in.heap().allocate<ErrorObject>(in.intrinsics().suppressed_error_prototype);
    made->put(PropertyKey::atom(in.atoms().message), Value::string(in.atom("An error was suppressed during disposal")), builtin_attributes);
    made->put(in.key("error"), error, builtin_attributes);
    made->put(in.key("suppressed"), suppressed, builtin_attributes);
    std::string const line = in.stack_text(Value::object(made));
    made->set_stack(in.string(std::string_view(line)));
    return made;
}

// The errors of a disposal so far: none, or one, the later ones wrapped
// around the earlier.
struct Suppression {
    bool has_error = false;
    Value error;
    void add(Interpreter& in, Value const& thrown)
    {
        if (has_error)
            error = Value::object(new_suppressed_error(in, thrown, error));
        else
            error = thrown;
        has_error = true;
    }
};

// Calls one record's method the way its kind says; the result, or the
// throw left on the interpreter.
std::optional<Value> call_record(Interpreter& in, DisposeRecord const& record)
{
    switch (record.kind) {
    case DisposeRecord::Kind::Method:
        if (record.method.is_undefined())
            return Value::undefined(); // a null or undefined resource on an async stack
        return in.call(record.method, record.value, {});
    case DisposeRecord::Kind::Adopted: {
        Value const arguments[] = { record.value };
        return in.call(record.method, Value::undefined(), arguments);
    }
    case DisposeRecord::Kind::Deferred:
        return in.call(record.method, Value::undefined(), {});
    }
    return Value::undefined();
}

// DisposeResources (§9.14.6) with the sync-dispose hint: every record in
// reverse, each error suppressing the one before.
std::optional<Value> dispose_all(Interpreter& in, DisposableStackObject& stack)
{
    Interpreter::Roots const roots(in);
    in.root(Value::object(&stack));
    Suppression errors;
    while (!stack.records.empty()) {
        DisposeRecord const record = stack.records.back();
        stack.records.pop_back();
        in.root(record.value);
        in.root(record.method);
        std::optional<Value> const result = call_record(in, record);
        if (!result) {
            Value const thrown = in.take_exception();
            in.root(thrown);
            errors.add(in, thrown);
            in.root(errors.error);
        }
    }
    if (errors.has_error)
        return in.throw_value(errors.error);
    return Value::undefined();
}

// The async-dispose hint: each record's result is awaited before the
// next is called, and the promise handed back settles at the end. The
// loop is a closure that calls itself from each await's reactions; its
// slots are the stack, the capability's two functions, and the errors so
// far (empty for none).
constexpr std::size_t slot_stack = 0;
constexpr std::size_t slot_resolve = 1;
constexpr std::size_t slot_reject = 2;
constexpr std::size_t slot_error = 3;

void dispose_async_step(Interpreter& in, ClosureFunction& step);

void finish_async(Interpreter& in, ClosureFunction& step)
{
    Value const error = step.slot(slot_error);
    if (error.is_empty()) {
        static_cast<void>(in.call(step.slot(slot_resolve), Value::undefined(), {}));
        return;
    }
    Value const arguments[] = { error };
    static_cast<void>(in.call(step.slot(slot_reject), Value::undefined(), arguments));
}

void add_async_error(Interpreter& in, ClosureFunction& step, Value const& thrown)
{
    Value const earlier = step.slot(slot_error);
    if (earlier.is_empty())
        step.set_slot(slot_error, thrown);
    else
        step.set_slot(slot_error, Value::object(new_suppressed_error(in, thrown, earlier)));
}

void dispose_async_step(Interpreter& in, ClosureFunction& step)
{
    Interpreter::Roots const roots(in);
    in.root(Value::object(&step));
    auto& stack = *static_cast<DisposableStackObject*>(step.slot(slot_stack).as_object());
    if (stack.records.empty()) {
        finish_async(in, step);
        return;
    }
    DisposeRecord const record = stack.records.back();
    stack.records.pop_back();
    in.root(record.value);
    in.root(record.method);
    std::optional<Value> const result = call_record(in, record);
    if (!result) {
        // A throw is not awaited: the next record follows at once.
        Value const thrown = in.take_exception();
        in.root(thrown);
        add_async_error(in, step, thrown);
        dispose_async_step(in, step);
        return;
    }
    // Await the result — undefined for a sync method on this async stack,
    // whose own result is not awaited.
    Value const awaited = record.sync_on_async ? Value::undefined() : *result;
    in.root(awaited);
    std::optional<Value> const promise = promise_resolve(in, Value::object(in.intrinsics().promise_constructor), awaited);
    if (!promise) {
        Value const thrown = in.take_exception();
        in.root(thrown);
        add_async_error(in, step, thrown);
        dispose_async_step(in, step);
        return;
    }
    in.root(*promise);
    ClosureFunction* on_fulfilled = in.new_closure("", 1, { Value::object(&step) },
        [](Interpreter& inner, ClosureFunction& self, Value const&, std::span<Value const>) -> std::optional<Value> {
            dispose_async_step(inner, *static_cast<ClosureFunction*>(self.slot(0).as_object()));
            return Value::undefined();
        });
    in.root(Value::object(on_fulfilled));
    ClosureFunction* on_rejected = in.new_closure("", 1, { Value::object(&step) },
        [](Interpreter& inner, ClosureFunction& self, Value const&, std::span<Value const> arguments) -> std::optional<Value> {
            auto& the_step = *static_cast<ClosureFunction*>(self.slot(0).as_object());
            Interpreter::Roots const inner_roots(inner);
            inner.root(Value::object(&the_step));
            add_async_error(inner, the_step, argument(arguments, 0));
            dispose_async_step(inner, the_step);
            return Value::undefined();
        });
    in.root(Value::object(on_rejected));
    perform_then(in, *static_cast<PromiseObject*>(promise->as_object()), Value::object(on_fulfilled), Value::object(on_rejected), std::nullopt);
}

// A constructor on the global whose prototype has `constructor` back.
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

std::nullopt_t already_disposed(Interpreter& in, bool async)
{
    return in.throw_reference_error(std::string(stack_name(async)) + " already disposed");
}

// The members the two stacks share: `disposed`, use, adopt, defer, move.
void install_stack_members(Interpreter& in, Object& prototype, bool async)
{
    // The receiver checks read `async` from a slot: a closure keeps the
    // flag with the function.
    auto method = [&](std::string_view name, int length, auto body) {
        ClosureFunction* function = in.new_closure(name, length, { Value::boolean(async) },
            [body](Interpreter& interp, ClosureFunction& self, Value const& this_value, std::span<Value const> arguments) -> std::optional<Value> {
                return body(interp, self.slot(0).as_boolean(), this_value, arguments);
            });
        prototype.put(in.key(name), Value::object(function), builtin_attributes);
        return function;
    };
    {
        ClosureFunction* getter = in.new_closure("get disposed", 0, { Value::boolean(async) },
            [](Interpreter& interp, ClosureFunction& self, Value const& this_value, std::span<Value const>) -> std::optional<Value> {
                std::optional<DisposableStackObject*> const stack = this_stack(interp, this_value, self.slot(0).as_boolean(), "disposed");
                if (!stack)
                    return std::nullopt;
                return Value::boolean((*stack)->disposed);
            });
        prototype.put_accessor(in.key("disposed"), getter, nullptr, Configurable);
    }
    // use(value): a resource with a dispose method, or null/undefined for
    // nothing (§27.4.3.7).
    method("use", 1, [](Interpreter& interp, bool is_async, Value const& this_value, std::span<Value const> arguments) -> std::optional<Value> {
        std::optional<DisposableStackObject*> const stack = this_stack(interp, this_value, is_async, "use");
        if (!stack)
            return std::nullopt;
        if ((*stack)->disposed)
            return already_disposed(interp, is_async);
        Value const value = argument(arguments, 0);
        if (value.is_nullish()) {
            // Nothing to dispose; on an async stack still a record with no
            // method, so that disposal awaits once for it (§9.14.5
            // AddDisposableResource step 1.b).
            if (is_async) {
                DisposeRecord empty;
                empty.value = value;
                (*stack)->records.push_back(std::move(empty));
            }
            return value;
        }
        Interpreter::Roots const roots(interp);
        interp.root(value);
        std::optional<DisposeRecord> record = dispose_method_of(interp, value, is_async);
        if (!record)
            return std::nullopt;
        if (record->method.is_undefined())
            return interp.throw_type_error(interp.describe(value) + " is not disposable: it has no " + (is_async ? "[Symbol.asyncDispose] or " : "") + "[Symbol.dispose] method");
        (*stack)->records.push_back(std::move(*record));
        return value;
    });
    // adopt(value, onDispose): onDispose(value) at disposal (§27.4.3.1).
    method("adopt", 2, [](Interpreter& interp, bool is_async, Value const& this_value, std::span<Value const> arguments) -> std::optional<Value> {
        std::optional<DisposableStackObject*> const stack = this_stack(interp, this_value, is_async, "adopt");
        if (!stack)
            return std::nullopt;
        if ((*stack)->disposed)
            return already_disposed(interp, is_async);
        Value const value = argument(arguments, 0);
        Value const on_dispose = argument(arguments, 1);
        if (!Interpreter::is_callable(on_dispose))
            return interp.throw_type_error(interp.describe(on_dispose) + " is not a function");
        DisposeRecord record;
        record.kind = DisposeRecord::Kind::Adopted;
        record.value = value;
        record.method = on_dispose;
        (*stack)->records.push_back(std::move(record));
        return value;
    });
    // defer(onDispose): onDispose() at disposal (§27.4.3.2).
    method("defer", 1, [](Interpreter& interp, bool is_async, Value const& this_value, std::span<Value const> arguments) -> std::optional<Value> {
        std::optional<DisposableStackObject*> const stack = this_stack(interp, this_value, is_async, "defer");
        if (!stack)
            return std::nullopt;
        if ((*stack)->disposed)
            return already_disposed(interp, is_async);
        Value const on_dispose = argument(arguments, 0);
        if (!Interpreter::is_callable(on_dispose))
            return interp.throw_type_error(interp.describe(on_dispose) + " is not a function");
        DisposeRecord record;
        record.kind = DisposeRecord::Kind::Deferred;
        record.method = on_dispose;
        (*stack)->records.push_back(std::move(record));
        return Value::undefined();
    });
    // move(): a new stack of the same kind takes the resources; this one is
    // disposed with nothing left in it (§27.4.3.5).
    method("move", 0, [](Interpreter& interp, bool is_async, Value const& this_value, std::span<Value const>) -> std::optional<Value> {
        std::optional<DisposableStackObject*> const stack = this_stack(interp, this_value, is_async, "move");
        if (!stack)
            return std::nullopt;
        if ((*stack)->disposed)
            return already_disposed(interp, is_async);
        Heap::NoCollect const guard(interp.heap());
        Intrinsics const& i = interp.intrinsics();
        auto* moved = interp.heap().allocate<DisposableStackObject>(is_async ? i.async_disposable_stack_prototype : i.disposable_stack_prototype, is_async);
        moved->records = std::move((*stack)->records);
        (*stack)->records.clear();
        (*stack)->disposed = true;
        return Value::object(moved);
    });
}

NativeFunction::ConstructCallback stack_constructor(bool async)
{
    return [async](Interpreter& interp, std::span<Value const>, Object* new_target) -> std::optional<Value> {
        std::optional<Object*> const prototype = interp.get_prototype_from_constructor(
            new_target, [async](Intrinsics const& i) { return async ? i.async_disposable_stack_prototype : i.disposable_stack_prototype; });
        if (!prototype)
            return std::nullopt;
        return Value::object(interp.heap().allocate<DisposableStackObject>(*prototype, async));
    };
}

void install_disposable_stack(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    WellKnownAtoms const& atoms = in.atoms();
    Heap::NoCollect const guard(in.heap());

    // DisposableStack (§27.4).
    Object* prototype = in.new_object();
    i.disposable_stack_prototype = prototype;
    define_constructor(
        in, "DisposableStack", 0, *prototype,
        [](Interpreter& interp, Value const&, std::span<Value const>) -> std::optional<Value> { return interp.throw_type_error("Constructor DisposableStack requires 'new'"); },
        stack_constructor(false));
    install_stack_members(in, *prototype, false);
    NativeFunction* dispose = define_method(in, *prototype, "dispose", 0, [](Interpreter& interp, Value const& this_value, std::span<Value const>) -> std::optional<Value> {
        std::optional<DisposableStackObject*> const stack = this_stack(interp, this_value, false, "dispose");
        if (!stack)
            return std::nullopt;
        if ((*stack)->disposed)
            return Value::undefined();
        (*stack)->disposed = true;
        return dispose_all(interp, **stack);
    });
    // @@dispose is the very same function (§27.4.3.8).
    prototype->put(PropertyKey::symbol(atoms.symbol_dispose), Value::object(dispose), builtin_attributes);
    prototype->put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("DisposableStack")), Configurable);

    // AsyncDisposableStack (§27.5).
    Object* async_prototype = in.new_object();
    i.async_disposable_stack_prototype = async_prototype;
    define_constructor(
        in, "AsyncDisposableStack", 0, *async_prototype,
        [](Interpreter& interp, Value const&, std::span<Value const>) -> std::optional<Value> { return interp.throw_type_error("Constructor AsyncDisposableStack requires 'new'"); },
        stack_constructor(true));
    install_stack_members(in, *async_prototype, true);
    NativeFunction* dispose_async = define_method(in, *async_prototype, "disposeAsync", 0, [](Interpreter& interp, Value const& this_value, std::span<Value const>) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<PromiseCapability> const capability = new_promise_capability(interp, Value::object(interp.intrinsics().promise_constructor));
        if (!capability)
            return std::nullopt;
        interp.root(capability->promise);
        interp.root(capability->resolve);
        interp.root(capability->reject);
        // A wrong receiver rejects rather than throws (§27.5.3.3 step 3).
        std::optional<DisposableStackObject*> const stack = this_stack(interp, this_value, true, "disposeAsync");
        if (!stack) {
            Value const thrown[] = { interp.take_exception() };
            interp.root(thrown[0]);
            static_cast<void>(interp.call(capability->reject, Value::undefined(), thrown));
            return capability->promise;
        }
        if ((*stack)->disposed) {
            static_cast<void>(interp.call(capability->resolve, Value::undefined(), {}));
            return capability->promise;
        }
        (*stack)->disposed = true;
        ClosureFunction* step = interp.new_closure("", 0, { this_value, capability->resolve, capability->reject, Value::empty() },
            [](Interpreter& inner, ClosureFunction& self, Value const&, std::span<Value const>) -> std::optional<Value> {
                dispose_async_step(inner, self);
                return Value::undefined();
            });
        interp.root(Value::object(step));
        dispose_async_step(interp, *step);
        return capability->promise;
    });
    async_prototype->put(PropertyKey::symbol(atoms.symbol_async_dispose), Value::object(dispose_async), builtin_attributes);
    async_prototype->put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("AsyncDisposableStack")), Configurable);
}

// SuppressedError (§20.5.9): (error, suppressed, message, options), the
// prototype under %Error.prototype%, the constructor under %Error%.
void install_suppressed_error(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    WellKnownAtoms const& atoms = in.atoms();
    Heap::NoCollect const guard(in.heap());
    Object* prototype = in.new_object(i.error_prototype);
    i.suppressed_error_prototype = prototype;
    auto const construct = [](Interpreter& interp, std::span<Value const> arguments, Object* new_target) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        if (new_target)
            interp.root(Value::object(new_target));
        for (Value const& argument_value : arguments)
            interp.root(argument_value);
        std::optional<Object*> const prototype_of = interp.get_prototype_from_constructor(new_target, &Intrinsics::suppressed_error_prototype);
        if (!prototype_of)
            return std::nullopt;
        auto* made = interp.heap().allocate<ErrorObject>(*prototype_of);
        interp.root(Value::object(made));
        Value const message = argument(arguments, 2);
        if (!message.is_undefined()) {
            std::optional<JsString*> const text = interp.to_string(message);
            if (!text)
                return std::nullopt;
            made->put(PropertyKey::atom(interp.atoms().message), Value::string(*text), builtin_attributes);
        }
        Value const options = argument(arguments, 3);
        if (options.is_object()) {
            // InstallErrorCause (§20.5.8.1).
            std::optional<bool> const has = interp.has_property(*options.as_object(), PropertyKey::atom(interp.atoms().cause));
            if (!has)
                return std::nullopt;
            if (*has) {
                std::optional<Value> const cause = interp.get(*options.as_object(), PropertyKey::atom(interp.atoms().cause));
                if (!cause)
                    return std::nullopt;
                made->put(PropertyKey::atom(interp.atoms().cause), *cause, builtin_attributes);
            }
        }
        made->put(interp.key("error"), argument(arguments, 0), builtin_attributes);
        made->put(interp.key("suppressed"), argument(arguments, 1), builtin_attributes);
        std::string const line = interp.stack_text(Value::object(made));
        made->set_stack(interp.string(std::string_view(line)));
        return Value::object(made);
    };
    NativeFunction* constructor = define_constructor(
        in, "SuppressedError", 3, *prototype,
        [construct](Interpreter& interp, Value const&, std::span<Value const> arguments) -> std::optional<Value> {
            // Called as a function, new.target is the constructor itself (§20.5.9.1.1 step 1).
            return construct(interp, arguments, interp.intrinsics().suppressed_error_constructor);
        },
        construct);
    constructor->set_prototype(i.error_constructor);
    i.suppressed_error_constructor = constructor;
    prototype->put(PropertyKey::atom(atoms.name), Value::string(in.atom("SuppressedError")), builtin_attributes);
    prototype->put(PropertyKey::atom(atoms.message), Value::string(atoms.empty), builtin_attributes);
}

// %IteratorPrototype%[@@dispose] (§27.1.4.1) and
// %AsyncIteratorPrototype%[@@asyncDispose] (§27.1.5.1): the iterator's
// return method, when it has one.
void install_iterator_dispose(Interpreter& in)
{
    Intrinsics const& i = in.intrinsics();
    WellKnownAtoms const& atoms = in.atoms();
    Heap::NoCollect const guard(in.heap());
    NativeFunction* dispose = in.new_native("[Symbol.dispose]", 0, [](Interpreter& interp, Value const& this_value, std::span<Value const>) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<Value> const return_method = interp.get_method(this_value, interp.key("return"));
        if (!return_method)
            return std::nullopt;
        if (return_method->is_undefined())
            return Value::undefined();
        interp.root(*return_method);
        if (!interp.call(*return_method, this_value, {}))
            return std::nullopt;
        return Value::undefined();
    });
    i.iterator_prototype->put(PropertyKey::symbol(atoms.symbol_dispose), Value::object(dispose), builtin_attributes);

    NativeFunction* async_dispose = in.new_native("[Symbol.asyncDispose]", 0, [](Interpreter& interp, Value const& this_value, std::span<Value const>) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<PromiseCapability> const capability = new_promise_capability(interp, Value::object(interp.intrinsics().promise_constructor));
        if (!capability)
            return std::nullopt;
        interp.root(capability->promise);
        interp.root(capability->resolve);
        interp.root(capability->reject);
        auto const reject_with_exception = [&]() -> std::optional<Value> {
            Value const thrown[] = { interp.take_exception() };
            interp.root(thrown[0]);
            static_cast<void>(interp.call(capability->reject, Value::undefined(), thrown));
            return capability->promise;
        };
        std::optional<Value> const return_method = interp.get_method(this_value, interp.key("return"));
        if (!return_method)
            return reject_with_exception();
        if (return_method->is_undefined()) {
            static_cast<void>(interp.call(capability->resolve, Value::undefined(), {}));
            return capability->promise;
        }
        interp.root(*return_method);
        std::optional<Value> const result = interp.call(*return_method, this_value, {});
        if (!result)
            return reject_with_exception();
        interp.root(*result);
        // Await the result, then resolve with undefined.
        std::optional<Value> const promise = promise_resolve(interp, Value::object(interp.intrinsics().promise_constructor), *result);
        if (!promise)
            return reject_with_exception();
        interp.root(*promise);
        ClosureFunction* on_fulfilled = interp.new_closure("", 1, { capability->resolve },
            [](Interpreter& inner, ClosureFunction& self, Value const&, std::span<Value const>) -> std::optional<Value> {
                static_cast<void>(inner.call(self.slot(0), Value::undefined(), {}));
                return Value::undefined();
            });
        interp.root(Value::object(on_fulfilled));
        ClosureFunction* on_rejected = interp.new_closure("", 1, { capability->reject },
            [](Interpreter& inner, ClosureFunction& self, Value const&, std::span<Value const> arguments) -> std::optional<Value> {
                Value const reason[] = { argument(arguments, 0) };
                static_cast<void>(inner.call(self.slot(0), Value::undefined(), reason));
                return Value::undefined();
            });
        interp.root(Value::object(on_rejected));
        perform_then(interp, *static_cast<PromiseObject*>(promise->as_object()), Value::object(on_fulfilled), Value::object(on_rejected), std::nullopt);
        return capability->promise;
    });
    i.async_iterator_prototype->put(PropertyKey::symbol(atoms.symbol_async_dispose), Value::object(async_dispose), builtin_attributes);
}

} // namespace

void install_disposable(Interpreter& in)
{
    install_suppressed_error(in);
    install_disposable_stack(in);
    install_iterator_dispose(in);
}

}
