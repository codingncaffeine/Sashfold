#include "js/Runtime.h"

// Promises (§27.2) and the job queue they run on (§9.5): the constructor
// and its resolving functions, then/catch/finally, the combinators
// (all, allSettled, any, race), resolve/reject/withResolvers/try,
// AggregateError, and the reaction and thenable jobs. The queue is the
// interpreter's own; the host drains it at its microtask checkpoints
// (Interpreter::run_jobs), and a rejection nobody handled by the end of
// a drain is reported once to the console.

#include "js/Object.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

namespace {

// ---- the reactions and jobs

// A PromiseCapability Record (§27.2.1.1) as three values.
struct Capability {
    Value promise;
    Value resolve;
    Value reject;
};

bool is_promise(Value const& value)
{
    return value.is_object() && value.as_object()->class_id() == Object::Class::Promise;
}

std::optional<PromiseObject*> this_promise(Interpreter& in, Value const& this_value, std::string_view method)
{
    if (!is_promise(this_value))
        return in.throw_type_error("Method Promise.prototype." + std::string(method) + " called on incompatible receiver " + in.describe(this_value));
    return static_cast<PromiseObject*>(this_value.as_object());
}

// TriggerPromiseReactions (§27.2.1.8): every reaction becomes a job.
void trigger_reactions(Interpreter& in, std::vector<PromiseReaction> const& reactions, Value const& argument)
{
    for (PromiseReaction const& reaction : reactions) {
        Job job;
        job.kind = Job::Kind::Reaction;
        job.reaction = reaction;
        job.argument = argument;
        in.enqueue_job(std::move(job));
    }
}

// FulfillPromise and RejectPromise (§27.2.1.4, §27.2.1.7).
void fulfill_promise(Interpreter& in, PromiseObject& promise, Value const& value)
{
    std::vector<PromiseReaction> const reactions = std::move(promise.fulfill_reactions());
    promise.settle(PromiseObject::State::Fulfilled, value);
    trigger_reactions(in, reactions, value);
}

void reject_promise(Interpreter& in, PromiseObject& promise, Value const& reason)
{
    std::vector<PromiseReaction> const reactions = std::move(promise.reject_reactions());
    promise.settle(PromiseObject::State::Rejected, reason);
    if (!promise.is_handled())
        in.track_rejection(promise, true);
    trigger_reactions(in, reactions, reason);
}

// CreateResolvingFunctions (§27.2.1.3): resolve and reject sharing one
// "already resolved" flag; resolve unwraps a thenable through a job.
struct ResolvingFunctions {
    Value resolve;
    Value reject;
};

// Promise Resolve Functions (§27.2.1.3.2) steps 7–16.
void resolve_promise(Interpreter& in, PromiseObject& promise, Value const& resolution)
{
    Interpreter::Roots const roots(in);
    in.root(Value::object(&promise));
    in.root(resolution);
    if (resolution.is_object() && resolution.as_object() == &promise) {
        Object* error = in.new_error(ErrorType::TypeError, "Chaining cycle detected for promise");
        reject_promise(in, promise, Value::object(error));
        return;
    }
    if (!resolution.is_object()) {
        fulfill_promise(in, promise, resolution);
        return;
    }
    std::optional<Value> const then = in.get(resolution, PropertyKey::atom(in.atoms().then));
    if (!then) {
        Value const reason = in.take_exception();
        in.root(reason);
        reject_promise(in, promise, reason);
        return;
    }
    if (!Interpreter::is_callable(*then)) {
        fulfill_promise(in, promise, resolution);
        return;
    }
    Job job;
    job.kind = Job::Kind::ResolveThenable;
    job.promise = &promise;
    job.argument = resolution;
    job.then = *then;
    in.enqueue_job(std::move(job));
}

ResolvingFunctions create_resolving_functions(Interpreter& in, PromiseObject& promise)
{
    Heap::NoCollect const guard(in.heap());
    auto already_resolved = std::make_shared<bool>(false);
    ClosureFunction* resolve = in.new_closure("", 1, { Value::object(&promise) },
        [already_resolved](Interpreter& interp, ClosureFunction& self, Value const&, Args args) -> std::optional<Value> {
            if (*already_resolved)
                return Value::undefined();
            *already_resolved = true;
            resolve_promise(interp, *static_cast<PromiseObject*>(self.slot(0).as_object()), argument(args, 0));
            return Value::undefined();
        });
    ClosureFunction* reject = in.new_closure("", 1, { Value::object(&promise) },
        [already_resolved](Interpreter& interp, ClosureFunction& self, Value const&, Args args) -> std::optional<Value> {
            if (*already_resolved)
                return Value::undefined();
            *already_resolved = true;
            reject_promise(interp, *static_cast<PromiseObject*>(self.slot(0).as_object()), argument(args, 0));
            return Value::undefined();
        });
    return ResolvingFunctions { Value::object(resolve), Value::object(reject) };
}

// NewPromiseCapability (§27.2.1.5): a promise from any constructor, with
// the resolve and reject its executor was handed.
std::optional<Capability> new_promise_capability(Interpreter& in, Value const& constructor)
{
    if (!Interpreter::is_constructor(constructor))
        return in.throw_type_error(in.describe(constructor) + " is not a constructor");
    Interpreter::Roots const roots(in);
    in.root(constructor);
    // The executor records what it is called with, once each.
    ClosureFunction* executor = in.new_closure("", 2, { Value::undefined(), Value::undefined() },
        [](Interpreter& interp, ClosureFunction& self, Value const&, Args args) -> std::optional<Value> {
            if (!self.slot(0).is_undefined())
                return interp.throw_type_error("Promise executor has already been invoked with non-undefined arguments");
            if (!self.slot(1).is_undefined())
                return interp.throw_type_error("Promise executor has already been invoked with non-undefined arguments");
            self.set_slot(0, argument(args, 0));
            self.set_slot(1, argument(args, 1));
            return Value::undefined();
        });
    in.root(Value::object(executor));
    Value const arguments[1] = { Value::object(executor) };
    std::optional<Value> const promise = in.construct(constructor, arguments);
    if (!promise)
        return std::nullopt;
    if (!Interpreter::is_callable(executor->slot(0)))
        return in.throw_type_error("Promise resolve function is not callable");
    if (!Interpreter::is_callable(executor->slot(1)))
        return in.throw_type_error("Promise reject function is not callable");
    return Capability { *promise, executor->slot(0), executor->slot(1) };
}

// PromiseResolve (§27.2.4.7.1): the value itself when it is a promise
// of this constructor, else a new one resolved with it.
std::optional<Value> promise_resolve(Interpreter& in, Value const& constructor, Value const& value)
{
    Interpreter::Roots const roots(in);
    in.root(constructor);
    in.root(value);
    if (is_promise(value)) {
        std::optional<Value> const value_constructor = in.get(value, PropertyKey::atom(in.atoms().constructor));
        if (!value_constructor)
            return std::nullopt;
        if (Interpreter::same_value(*value_constructor, constructor))
            return value;
    }
    std::optional<Capability> const capability = new_promise_capability(in, constructor);
    if (!capability)
        return std::nullopt;
    in.root(capability->promise);
    in.root(capability->resolve);
    Value const arguments[1] = { value };
    if (!in.call(capability->resolve, Value::undefined(), arguments))
        return std::nullopt;
    return capability->promise;
}

// PerformPromiseThen (§27.2.5.4.1): the reactions recorded or, for a
// settled promise, queued at once; the derived capability may be absent
// (await's internal use).
Value perform_then(Interpreter& in, PromiseObject& promise, Value const& on_fulfilled, Value const& on_rejected,
    std::optional<Capability> const& capability)
{
    PromiseReaction fulfill;
    fulfill.type = PromiseReaction::Type::Fulfill;
    fulfill.handler = Interpreter::is_callable(on_fulfilled) ? on_fulfilled : Value::empty();
    PromiseReaction reject;
    reject.type = PromiseReaction::Type::Reject;
    reject.handler = Interpreter::is_callable(on_rejected) ? on_rejected : Value::empty();
    if (capability) {
        fulfill.capability_promise = reject.capability_promise = capability->promise;
        fulfill.capability_resolve = reject.capability_resolve = capability->resolve;
        fulfill.capability_reject = reject.capability_reject = capability->reject;
    }
    switch (promise.state()) {
    case PromiseObject::State::Pending:
        promise.fulfill_reactions().push_back(fulfill);
        promise.reject_reactions().push_back(reject);
        break;
    case PromiseObject::State::Fulfilled: {
        Job job;
        job.kind = Job::Kind::Reaction;
        job.reaction = fulfill;
        job.argument = promise.result();
        in.enqueue_job(std::move(job));
        break;
    }
    case PromiseObject::State::Rejected: {
        if (!promise.is_handled())
            in.track_rejection(promise, false);
        Job job;
        job.kind = Job::Kind::Reaction;
        job.reaction = reject;
        job.argument = promise.result();
        in.enqueue_job(std::move(job));
        break;
    }
    }
    promise.set_handled();
    return capability ? capability->promise : Value::undefined();
}

// The combinators' shared shape (§27.2.4.1.2 and kin): the iterable
// walked with the constructor's `resolve`, each element's promise given
// handlers that fill one slot of the result, the iterator closed on a
// throw that is not its own.
struct CombinatorState {
    std::size_t remaining = 1;
};

enum class Combinator { All, AllSettled, Any };

std::optional<Value> perform_combinator(Interpreter& in, Combinator kind, Value const& constructor, Value const& iterable,
    Capability const& capability, Value const& resolve_function)
{
    Interpreter::Roots const roots(in);
    in.root(constructor);
    in.root(iterable);
    in.root(capability.promise);
    in.root(capability.resolve);
    in.root(capability.reject);
    in.root(resolve_function);
    std::optional<IteratorRecord> record = in.get_iterator(iterable);
    if (!record)
        return std::nullopt;
    in.root(record->iterator);
    in.root(record->next_method);
    ArrayObject* values = in.new_array();
    in.root(Value::object(values));
    auto state = std::make_shared<CombinatorState>();
    std::uint32_t index = 0;
    // Every element's promise has been asked, and none is still pending:
    // the result settles now — for `any`, an AggregateError of the reasons.
    auto settle_now = [&]() -> std::optional<Value> {
        if (kind == Combinator::Any) {
            Object* error = in.new_aggregate_error(Value::object(values), "All promises were rejected");
            in.root(Value::object(error));
            Value const arguments[1] = { Value::object(error) };
            if (!in.call(capability.reject, Value::undefined(), arguments))
                return std::nullopt;
        } else {
            Value const arguments[1] = { Value::object(values) };
            if (!in.call(capability.resolve, Value::undefined(), arguments))
                return std::nullopt;
        }
        return capability.promise;
    };
    while (true) {
        Value next;
        std::optional<bool> const stepped = in.iterator_step(*record, next);
        if (!stepped)
            return std::nullopt; // the iterator's own throw: nothing to close
        if (!*stepped) {
            if (--state->remaining == 0)
                return settle_now();
            return capability.promise;
        }
        Interpreter::Roots const step_roots(in);
        in.root(next);
        values->set_element(index, Value::undefined());
        Value const resolve_arguments[1] = { next };
        std::optional<Value> const next_promise = in.call(resolve_function, constructor, resolve_arguments);
        if (!next_promise) {
            in.iterator_close(*record, true);
            return std::nullopt;
        }
        in.root(*next_promise);
        // An element function: its index, the values array, the
        // capability's resolve and reject; called at most once.
        auto make_element_function = [&](bool on_fulfilled) -> ClosureFunction* {
            auto called = std::make_shared<bool>(false);
            Combinator const which = kind;
            return in.new_closure("", 1,
                { Value::number(static_cast<double>(index)), Value::object(values), capability.resolve, capability.reject },
                [state, called, which, on_fulfilled](Interpreter& interp, ClosureFunction& self, Value const&, Args args) -> std::optional<Value> {
                    if (*called)
                        return Value::undefined();
                    *called = true;
                    Interpreter::Roots const inner_roots(interp);
                    auto* array = static_cast<ArrayObject*>(self.slot(1).as_object());
                    auto const slot_index = static_cast<std::uint32_t>(self.slot(0).as_number());
                    Value const value = argument(args, 0);
                    interp.root(value);
                    Value stored = value;
                    if (which == Combinator::AllSettled) {
                        Heap::NoCollect const guard(interp.heap());
                        Object* entry = interp.new_object();
                        entry->put(interp.key("status"), Value::string(interp.atom(on_fulfilled ? "fulfilled" : "rejected")));
                        entry->put(interp.key(on_fulfilled ? "value" : "reason"), value);
                        stored = Value::object(entry);
                    }
                    array->set_element(slot_index, stored);
                    if (--state->remaining != 0)
                        return Value::undefined();
                    if (which == Combinator::Any) {
                        Object* error = interp.new_aggregate_error(self.slot(1), "All promises were rejected");
                        interp.root(Value::object(error));
                        Value const arguments[1] = { Value::object(error) };
                        return interp.call(self.slot(3), Value::undefined(), arguments);
                    }
                    Value const arguments[1] = { self.slot(1) };
                    return interp.call(self.slot(2), Value::undefined(), arguments);
                });
        };
        Value on_fulfilled;
        Value on_rejected;
        {
            Heap::NoCollect const guard(in.heap());
            switch (kind) {
            case Combinator::All:
                on_fulfilled = Value::object(make_element_function(true));
                on_rejected = capability.reject;
                break;
            case Combinator::AllSettled:
                on_fulfilled = Value::object(make_element_function(true));
                on_rejected = Value::object(make_element_function(false));
                break;
            case Combinator::Any:
                on_fulfilled = capability.resolve;
                on_rejected = Value::object(make_element_function(false));
                break;
            }
        }
        in.root(on_fulfilled);
        in.root(on_rejected);
        ++state->remaining;
        Value const then_arguments[2] = { on_fulfilled, on_rejected };
        if (!in.invoke(*next_promise, PropertyKey::atom(in.atoms().then), then_arguments)) {
            in.iterator_close(*record, true);
            return std::nullopt;
        }
        ++index;
    }
}

// GetPromiseResolve (§27.2.4.1.1): the constructor's `resolve`, callable.
std::optional<Value> get_promise_resolve(Interpreter& in, Value const& constructor)
{
    std::optional<Value> const resolve = in.get(constructor, in.key("resolve"));
    if (!resolve)
        return std::nullopt;
    if (!Interpreter::is_callable(*resolve))
        return in.throw_type_error("Promise resolve is not a function");
    return resolve;
}

// Promise.all / allSettled / any (§27.2.4.1, .2, .3): the capability from
// `this`, its `resolve` read once, and the loop above — any throw
// becomes the capability's rejection (IfAbruptRejectPromise).
std::optional<Value> combinator(Interpreter& in, Value const& this_value, Args args, Combinator kind)
{
    std::optional<Capability> const capability = new_promise_capability(in, this_value);
    if (!capability)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(capability->promise);
    in.root(capability->resolve);
    in.root(capability->reject);
    auto reject_with_exception = [&]() -> std::optional<Value> {
        Value const reason = in.take_exception();
        in.root(reason);
        Value const arguments[1] = { reason };
        if (!in.call(capability->reject, Value::undefined(), arguments))
            return std::nullopt;
        return capability->promise;
    };
    std::optional<Value> const resolve_function = get_promise_resolve(in, this_value);
    if (!resolve_function)
        return reject_with_exception();
    in.root(*resolve_function);
    std::optional<Value> const result = perform_combinator(in, kind, this_value, argument(args, 0), *capability, *resolve_function);
    if (!result)
        return reject_with_exception();
    return *result;
}

// Promise.race (§27.2.4.5): every element's promise gets the
// capability's own resolve and reject.
std::optional<Value> promise_race(Interpreter& in, Value const& this_value, Args args)
{
    std::optional<Capability> const capability = new_promise_capability(in, this_value);
    if (!capability)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(capability->promise);
    in.root(capability->resolve);
    in.root(capability->reject);
    auto reject_with_exception = [&]() -> std::optional<Value> {
        Value const reason = in.take_exception();
        in.root(reason);
        Value const arguments[1] = { reason };
        if (!in.call(capability->reject, Value::undefined(), arguments))
            return std::nullopt;
        return capability->promise;
    };
    std::optional<Value> const resolve_function = get_promise_resolve(in, this_value);
    if (!resolve_function)
        return reject_with_exception();
    in.root(*resolve_function);
    std::optional<IteratorRecord> record = in.get_iterator(argument(args, 0));
    if (!record)
        return reject_with_exception();
    in.root(record->iterator);
    in.root(record->next_method);
    while (true) {
        Value next;
        std::optional<bool> const stepped = in.iterator_step(*record, next);
        if (!stepped)
            return reject_with_exception();
        if (!*stepped)
            return capability->promise;
        Interpreter::Roots const step_roots(in);
        in.root(next);
        Value const resolve_arguments[1] = { next };
        std::optional<Value> const next_promise = in.call(*resolve_function, this_value, resolve_arguments);
        if (!next_promise) {
            in.iterator_close(*record, true);
            return reject_with_exception();
        }
        in.root(*next_promise);
        Value const then_arguments[2] = { capability->resolve, capability->reject };
        if (!in.invoke(*next_promise, PropertyKey::atom(in.atoms().then), then_arguments)) {
            in.iterator_close(*record, true);
            return reject_with_exception();
        }
    }
}

// One of `finally`'s two closures (§27.2.5.3.1, §27.2.5.3.2): onFinally
// runs, its result is waited for through the constructor's PromiseResolve,
// and then the original value passes through — or the original reason is
// thrown again.
std::optional<Value> finally_closure(Interpreter& in, ClosureFunction& self, Args args, bool rethrow)
{
    Interpreter::Roots const roots(in);
    Value const passed = argument(args, 0);
    in.root(passed);
    std::optional<Value> const result = in.call(self.slot(0), Value::undefined(), {});
    if (!result)
        return std::nullopt;
    in.root(*result);
    std::optional<Value> const promise = promise_resolve(in, self.slot(1), *result);
    if (!promise)
        return std::nullopt;
    in.root(*promise);
    ClosureFunction* thunk = rethrow
        ? in.new_closure("", 0, { passed }, [](Interpreter& interp, ClosureFunction& thrower, Value const&, Args) -> std::optional<Value> {
            return interp.throw_value(thrower.slot(0));
        })
        : in.new_closure("", 0, { passed }, [](Interpreter&, ClosureFunction& valuer, Value const&, Args) -> std::optional<Value> {
            return valuer.slot(0);
        });
    in.root(Value::object(thunk));
    Value const then_arguments[1] = { Value::object(thunk) };
    return in.invoke(*promise, PropertyKey::atom(in.atoms().then), then_arguments);
}

} // namespace

// ---- the jobs, run by the host's checkpoint

void Interpreter::enqueue_job(Job job)
{
    m_jobs.push_back(std::move(job));
}

void Interpreter::enqueue_microtask(Value const& callback, std::span<Value const> arguments)
{
    Job job;
    job.kind = Job::Kind::Callback;
    job.then = callback;
    job.arguments.assign(arguments.begin(), arguments.end());
    m_jobs.push_back(std::move(job));
}

void Interpreter::clear_jobs()
{
    m_jobs.clear();
    m_unhandled_rejections.clear();
}

bool Interpreter::run_next_job(Value* thrown)
{
    if (m_jobs.empty())
        return false;
    Job const job = std::move(m_jobs.front());
    m_jobs.pop_front();
    // The job left the queue, so its values are no longer traced from
    // there: root them for as long as it runs.
    Roots const roots(*this);
    root(job.argument);
    root(job.then);
    root(job.reaction.handler);
    root(job.reaction.capability_promise);
    root(job.reaction.capability_resolve);
    root(job.reaction.capability_reject);
    if (job.promise)
        root(Value::object(job.promise));
    for (Value const& argument : job.arguments)
        root(argument);
    auto const settle_thrown = [&] {
        if (thrown)
            *thrown = take_exception();
        else
            clear_exception();
    };
    switch (job.kind) {
    case Job::Kind::Callback: {
        if (!call(job.then, Value::undefined(), job.arguments))
            settle_thrown();
        return true;
    }
    case Job::Kind::Reaction: {
        // NewPromiseReactionJob (§27.2.2.1): the handler's result resolves
        // the derived promise, its throw rejects it; no handler passes
        // the value or the rejection straight through.
        PromiseReaction const& reaction = job.reaction;
        Value result;
        bool rejected = false;
        if (reaction.handler.is_empty()) {
            result = job.argument;
            rejected = reaction.type == PromiseReaction::Type::Reject;
        } else {
            Value const arguments[1] = { job.argument };
            std::optional<Value> const returned = call(reaction.handler, Value::undefined(), arguments);
            if (returned) {
                result = *returned;
            } else {
                result = take_exception();
                rejected = true;
            }
        }
        if (reaction.capability_promise.is_empty())
            return true; // an await's reaction: nothing derives from it
        root(result);
        Value const arguments[1] = { result };
        if (!call(rejected ? reaction.capability_reject : reaction.capability_resolve, Value::undefined(), arguments))
            settle_thrown();
        return true;
    }
    case Job::Kind::ResolveThenable: {
        // NewPromiseResolveThenableJob (§27.2.2.2): the thenable's then
        // called with fresh resolving functions; its throw rejects.
        ResolvingFunctions const functions = create_resolving_functions(*this, *job.promise);
        root(functions.resolve);
        root(functions.reject);
        Value const arguments[2] = { functions.resolve, functions.reject };
        if (!call(job.then, job.argument, arguments)) {
            Value const reason = take_exception();
            root(reason);
            Value const reject_arguments[1] = { reason };
            if (!call(functions.reject, Value::undefined(), reject_arguments))
                settle_thrown();
        }
        return true;
    }
    }
    return true;
}

void Interpreter::run_jobs(std::function<void(Value const&)> const& report)
{
    std::size_t budget = 1000000;
    while (budget > 0) {
        --budget;
        Value thrown = Value::empty();
        if (!run_next_job(&thrown))
            break;
        if (!thrown.is_empty() && report) {
            Roots const roots(*this);
            root(thrown);
            report(thrown);
        }
        if (m_terminated)
            break;
    }
    if (budget == 0 && !m_jobs.empty()) {
        m_jobs.clear();
        if (on_console)
            on_console("error", "a million jobs in one checkpoint: the rest were dropped");
    }
    report_unhandled_rejections();
}

// HostPromiseRejectionTracker (§27.2.1.9): a rejection with no handler
// is remembered; a handler arriving later takes it back; what is left
// when a drain ends is reported once.
void Interpreter::track_rejection(PromiseObject& promise, bool rejected)
{
    if (rejected) {
        m_unhandled_rejections.push_back(&promise);
        return;
    }
    std::erase(m_unhandled_rejections, &promise);
}

void Interpreter::report_unhandled_rejections()
{
    std::vector<PromiseObject*> const pending = std::move(m_unhandled_rejections);
    m_unhandled_rejections.clear();
    for (PromiseObject* promise : pending) {
        if (promise->is_handled())
            continue;
        promise->set_handled();
        if (on_console)
            on_console("error", "Uncaught (in promise) " + describe(promise->result()));
    }
}

Object* Interpreter::new_aggregate_error(Value const& errors, std::string_view message)
{
    // An AggregateError as the library makes one (§20.5.7.1.1): the
    // message, `errors` as given, and the stack line every error has.
    Heap::NoCollect const guard(*m_heap);
    auto* error = m_heap->allocate<ErrorObject>(m_intrinsics.aggregate_error_prototype);
    if (!message.empty())
        error->put(PropertyKey::atom(atoms().message), Value::string(m_heap->string(message)), builtin_attributes);
    error->put(key("errors"), errors, builtin_attributes);
    std::string const line = describe(Value::object(error));
    error->set_stack(m_heap->string(std::string_view(line)));
    return error;
}

// ---- the library

void install_promise(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    WellKnownAtoms const& atoms = in.atoms();
    Heap::NoCollect const guard(in.heap());

    // AggregateError (§20.5.7): an Error whose `errors` is an array of the
    // iterable it was given.
    i.aggregate_error_prototype = in.new_object(i.error_prototype);
    {
        Object& prototype = *i.aggregate_error_prototype;
        NativeFunction* constructor = in.new_native(
            "AggregateError", 2,
            [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
                // Called as a function, new.target is the constructor itself.
                return interp.construct(Value::object(interp.intrinsics().aggregate_error_constructor), args);
            },
            [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
                Interpreter::Roots const roots(interp);
                if (new_target)
                    interp.root(Value::object(new_target));
                for (Value const& arg : args)
                    interp.root(arg);
                std::optional<Object*> const instance_prototype = interp.get_prototype_from_constructor(new_target, interp.intrinsics().aggregate_error_prototype);
                if (!instance_prototype)
                    return std::nullopt;
                Object* error = interp.heap().allocate<ErrorObject>(*instance_prototype);
                interp.root(Value::object(error));
                Value const message = argument(args, 1);
                if (!message.is_undefined()) {
                    std::optional<JsString*> const text = interp.to_string(message);
                    if (!text)
                        return std::nullopt;
                    error->put(PropertyKey::atom(interp.atoms().message), Value::string(*text), builtin_attributes);
                }
                Value const options = argument(args, 2);
                if (options.is_object() && options.as_object()->has_property(PropertyKey::atom(interp.atoms().cause))) {
                    std::optional<Value> const cause = interp.get(*options.as_object(), PropertyKey::atom(interp.atoms().cause));
                    if (!cause)
                        return std::nullopt;
                    error->put(PropertyKey::atom(interp.atoms().cause), *cause, builtin_attributes);
                }
                std::optional<std::vector<Value>> const errors = interp.iterable_to_list(argument(args, 0));
                if (!errors)
                    return std::nullopt;
                ArrayObject* array = interp.new_array(*errors);
                error->put(interp.key("errors"), Value::object(array), builtin_attributes);
                std::string const line = interp.describe(Value::object(error));
                static_cast<ErrorObject*>(error)->set_stack(interp.string(std::string_view(line)));
                return Value::object(error);
            });
        i.aggregate_error_constructor = constructor;
        constructor->set_prototype(i.error_constructor);
        constructor->put(PropertyKey::atom(atoms.prototype), Value::object(&prototype), frozen_attributes);
        prototype.put(PropertyKey::atom(atoms.constructor), Value::object(constructor), builtin_attributes);
        prototype.put(PropertyKey::atom(atoms.name), Value::string(in.atom("AggregateError")), builtin_attributes);
        prototype.put(PropertyKey::atom(atoms.message), Value::string(atoms.empty), builtin_attributes);
        in.global()->put(in.key("AggregateError"), Value::object(constructor), builtin_attributes);
    }

    // Promise (§27.2.3): the executor called with the resolving functions;
    // its throw rejects.
    i.promise_prototype = in.new_object();
    Object& prototype = *i.promise_prototype;
    NativeFunction* constructor = in.new_native(
        "Promise", 1,
        [](Interpreter& interp, Value const&, Args) -> std::optional<Value> {
            return interp.throw_type_error("Promise constructor cannot be invoked without 'new'");
        },
        [](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            Value const executor = argument(args, 0);
            if (!Interpreter::is_callable(executor))
                return interp.throw_type_error("Promise resolver " + interp.describe(executor) + " is not a function");
            Interpreter::Roots const roots(interp);
            if (new_target)
                interp.root(Value::object(new_target));
            interp.root(executor);
            std::optional<Object*> const instance_prototype = interp.get_prototype_from_constructor(new_target, interp.intrinsics().promise_prototype);
            if (!instance_prototype)
                return std::nullopt;
            auto* promise = interp.heap().allocate<PromiseObject>(*instance_prototype);
            interp.root(Value::object(promise));
            ResolvingFunctions const functions = create_resolving_functions(interp, *promise);
            interp.root(functions.resolve);
            interp.root(functions.reject);
            Value const arguments[2] = { functions.resolve, functions.reject };
            if (!interp.call(executor, Value::undefined(), arguments)) {
                Value const reason = interp.take_exception();
                interp.root(reason);
                Value const reject_arguments[1] = { reason };
                if (!interp.call(functions.reject, Value::undefined(), reject_arguments))
                    return std::nullopt;
            }
            return Value::object(promise);
        });
    i.promise_constructor = constructor;
    constructor->put(PropertyKey::atom(atoms.prototype), Value::object(&prototype), frozen_attributes);
    prototype.put(PropertyKey::atom(atoms.constructor), Value::object(constructor), builtin_attributes);
    prototype.put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("Promise")), Configurable);
    in.global()->put(in.key("Promise"), Value::object(constructor), builtin_attributes);
    {
        NativeFunction* species = in.new_native("get [Symbol.species]", 0, [](Interpreter&, Value const& this_value, Args) -> std::optional<Value> {
            return this_value;
        });
        constructor->put_accessor(PropertyKey::symbol(atoms.symbol_species), species, nullptr, Configurable);
    }

    // Promise.prototype.then (§27.2.5.4): the species constructor's
    // capability, then the reactions.
    define_method(in, prototype, "then", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<PromiseObject*> const promise = this_promise(interp, this_value, "then");
        if (!promise)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        for (Value const& arg : args)
            interp.root(arg);
        std::optional<Value> const species = interp.species_constructor(**promise, interp.intrinsics().promise_constructor);
        if (!species)
            return std::nullopt;
        interp.root(*species);
        std::optional<Capability> const capability = new_promise_capability(interp, *species);
        if (!capability)
            return std::nullopt;
        interp.root(capability->promise);
        interp.root(capability->resolve);
        interp.root(capability->reject);
        return perform_then(interp, **promise, argument(args, 0), argument(args, 1), capability);
    });
    define_method(in, prototype, "catch", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §27.2.5.1: Invoke(this, "then", « undefined, onRejected »).
        Value const arguments[2] = { Value::undefined(), argument(args, 0) };
        return interp.invoke(this_value, PropertyKey::atom(interp.atoms().then), arguments);
    });
    define_method(in, prototype, "finally", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // §27.2.5.3: onFinally runs either way and its result is waited
        // for; the original value or reason then passes through.
        if (!this_value.is_object())
            return interp.throw_type_error("Promise.prototype.finally called on non-object");
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        for (Value const& arg : args)
            interp.root(arg);
        std::optional<Value> const species = interp.species_constructor(*this_value.as_object(), interp.intrinsics().promise_constructor);
        if (!species)
            return std::nullopt;
        interp.root(*species);
        Value const on_finally = argument(args, 0);
        Value then_finally = on_finally;
        Value catch_finally = on_finally;
        if (Interpreter::is_callable(on_finally)) {
            Heap::NoCollect const no_collect(interp.heap());
            then_finally = Value::object(interp.new_closure("", 1, { on_finally, *species },
                [](Interpreter& in2, ClosureFunction& self, Value const&, Args a) -> std::optional<Value> {
                    return finally_closure(in2, self, a, false);
                }));
            catch_finally = Value::object(interp.new_closure("", 1, { on_finally, *species },
                [](Interpreter& in2, ClosureFunction& self, Value const&, Args a) -> std::optional<Value> {
                    return finally_closure(in2, self, a, true);
                }));
        }
        interp.root(then_finally);
        interp.root(catch_finally);
        Value const arguments[2] = { then_finally, catch_finally };
        return interp.invoke(this_value, PropertyKey::atom(interp.atoms().then), arguments);
    });

    // The statics (§27.2.4).
    define_method(in, *constructor, "resolve", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        if (!this_value.is_object())
            return interp.throw_type_error("PromiseResolve called on non-object");
        return promise_resolve(interp, this_value, argument(args, 0));
    });
    define_method(in, *constructor, "reject", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        Interpreter::Roots const roots(interp);
        for (Value const& arg : args)
            interp.root(arg);
        std::optional<Capability> const capability = new_promise_capability(interp, this_value);
        if (!capability)
            return std::nullopt;
        interp.root(capability->promise);
        interp.root(capability->reject);
        Value const arguments[1] = { argument(args, 0) };
        if (!interp.call(capability->reject, Value::undefined(), arguments))
            return std::nullopt;
        return capability->promise;
    });
    define_method(in, *constructor, "withResolvers", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<Capability> const capability = new_promise_capability(interp, this_value);
        if (!capability)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        interp.root(capability->promise);
        interp.root(capability->resolve);
        interp.root(capability->reject);
        Heap::NoCollect const no_collect(interp.heap());
        Object* result = interp.new_object();
        result->put(interp.key("promise"), capability->promise);
        result->put(interp.key("resolve"), capability->resolve);
        result->put(interp.key("reject"), capability->reject);
        return Value::object(result);
    });
    define_method(in, *constructor, "try", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        // Promise.try (§27.2.4.8 as amended by ecma262 #3883, 2026): the
        // callback runs first; its throw becomes a fresh promise's
        // rejection, its result goes through PromiseResolve — so a promise
        // of this constructor comes back as itself, unwrapped.
        if (!this_value.is_object())
            return interp.throw_type_error("Promise.try called on non-object");
        Interpreter::Roots const roots(interp);
        interp.root(this_value);
        for (Value const& arg : args)
            interp.root(arg);
        Args const rest = args.size() > 1 ? args.subspan(1) : Args();
        std::optional<Value> const result = interp.call(argument(args, 0), Value::undefined(), rest);
        if (result) {
            interp.root(*result);
            return promise_resolve(interp, this_value, *result);
        }
        Value const reason = interp.take_exception();
        interp.root(reason);
        std::optional<Capability> const capability = new_promise_capability(interp, this_value);
        if (!capability)
            return std::nullopt;
        interp.root(capability->promise);
        interp.root(capability->reject);
        Value const arguments[1] = { reason };
        if (!interp.call(capability->reject, Value::undefined(), arguments))
            return std::nullopt;
        return capability->promise;
    });
    define_method(in, *constructor, "all", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        return combinator(interp, this_value, args, Combinator::All);
    });
    define_method(in, *constructor, "allSettled", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        return combinator(interp, this_value, args, Combinator::AllSettled);
    });
    define_method(in, *constructor, "any", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        return combinator(interp, this_value, args, Combinator::Any);
    });
    define_method(in, *constructor, "race", 1, promise_race);
}

}
