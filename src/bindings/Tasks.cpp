#include "bindings/Internal.h"

// The page's task-posting interfaces: AbortController and AbortSignal
// (DOM §3.2 — a flag, a reason and an abort event that a fetch watches),
// MessageChannel and MessagePort (HTML §9.5: two entangled ports, a
// message posted on one delivered to the other as a task, which is how a
// library schedules "the next turn" without a timer), and
// window.postMessage to the window itself. A message is delivered as the
// value that was posted: same realm, no clone.

#include "js/Object.h"

#include <cmath>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

// A DOMException as a value: the realm's thrower builds it and throws, so
// the exception is taken back.
js::Value dom_error(Realm::Internals& in, std::string_view name, std::string_view message)
{
    if (in.throw_dom_exception(name, message))
        return js::Value::undefined();
    return in.interpreter.take_exception();
}

std::optional<AbortSignalObject*> this_signal(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* signal = dynamic_cast<AbortSignalObject*>(this_value.as_object()))
            return signal;
    }
    return interp.throw_type_error("Illegal invocation");
}

// A native function the timers can call: what AbortSignal.timeout arms.
void schedule_native(Realm::Internals& in, double delay_ms, js::Value const& function)
{
    Timer timer;
    timer.id = in.next_timer_id++;
    timer.due = in.now() + delay_ms;
    timer.sequence = in.next_sequence++;
    timer.callback = std::make_unique<js::Persistent>(in.interpreter.heap(), function);
    in.timers.push_back(std::move(timer));
}

// --- MessagePort ---------------------------------------------------------------------------

class MessagePortObject final : public EventTargetObject {
public:
    explicit MessagePortObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    MessagePortObject* entangled = nullptr;
    bool started = false;
    bool closed = false;
    // Messages posted to this port before it was started.
    std::vector<std::unique_ptr<js::Persistent>> pending;
    void trace(js::Tracer& tracer) override
    {
        EventTargetObject::trace(tracer);
        tracer.visit(entangled);
    }
};

std::optional<MessagePortObject*> this_port(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* port = dynamic_cast<MessagePortObject*>(this_value.as_object()))
            return port;
    }
    return interp.throw_type_error("Illegal invocation");
}

// Fires a MessageEvent carrying `data` at `target` (a port or the window).
void deliver_message(Realm::Internals& in, js::Object* target, js::Value const& data, std::string_view origin, js::Value const& source)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(target));
    in.interpreter.root(data);
    in.interpreter.root(source);
    EventObject* event = in.new_event("MessageEvent", "message", false, false);
    in.interpreter.root(js::Value::object(event));
    event->detail_value = data;
    event->origin = std::string(origin);
    event->source_value = source;
    event->is_trusted = true;
    in.dispatch(*event, target);
}

// A task that delivers `data` at the port once it runs; a port closed or
// collected by then gets nothing.
void post_message_task(Realm::Internals& in, MessagePortObject& port, js::Value const& data)
{
    auto target = std::make_shared<js::Persistent>(in.interpreter.heap(), js::Value::object(&port));
    auto payload = std::make_shared<js::Persistent>(in.interpreter.heap(), data);
    in.post_task([&in, target, payload] {
        auto* destination = static_cast<MessagePortObject*>(target->value().as_object());
        if (destination->closed)
            return;
        deliver_message(in, destination, payload->value(), in.url.serialize_origin(), js::Value::null());
    });
}

void start_port(Realm::Internals& in, MessagePortObject& port)
{
    if (port.started)
        return;
    port.started = true;
    std::vector<std::unique_ptr<js::Persistent>> pending = std::move(port.pending);
    port.pending.clear();
    for (auto const& message : pending)
        post_message_task(in, port, message->value());
}

void install_message_channel(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;

    js::Object* port = define_interface(in, "MessagePort", in.prototype("EventTarget"));
    js::define_method(interpreter, *port, "postMessage", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<MessagePortObject*> const found = this_port(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        MessagePortObject* other = (*found)->entangled;
        if (other == nullptr || other->closed)
            return js::Value::undefined();
        js::Value const data = js::argument(args, 0);
        if (other->started)
            post_message_task(internals, *other, data);
        else
            other->pending.push_back(std::make_unique<js::Persistent>(interp.heap(), data));
        return js::Value::undefined();
    });
    js::define_method(interpreter, *port, "start", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<MessagePortObject*> const found = this_port(interp, this_value);
        if (!found)
            return std::nullopt;
        start_port(internals_of(interp), **found);
        return js::Value::undefined();
    });
    js::define_method(interpreter, *port, "close", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<MessagePortObject*> const found = this_port(interp, this_value);
        if (!found)
            return std::nullopt;
        (*found)->closed = true;
        if ((*found)->entangled != nullptr) {
            (*found)->entangled->entangled = nullptr;
            (*found)->entangled = nullptr;
        }
        return js::Value::undefined();
    });
    // onmessage: setting it starts the port (HTML §9.5.2.1).
    define_getter(in, *port, "onmessage",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<MessagePortObject*> const found = this_port(interp, this_value);
            if (!found)
                return std::nullopt;
            HandlerMap* handlers = internals_of(interp).handlers_of(*found);
            if (handlers == nullptr)
                return js::Value::null();
            auto const it = handlers->find("message");
            if (it == handlers->end() || it->second.function.is_undefined())
                return js::Value::null();
            return it->second.function;
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<MessagePortObject*> const found = this_port(interp, this_value);
            if (!found)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            HandlerMap* handlers = internals.handlers_of(*found);
            if (handlers == nullptr)
                return js::Value::undefined();
            js::Value const value = js::argument(args, 0);
            EventHandler handler;
            handler.function = js::Interpreter::is_callable(value) ? value : js::Value::undefined();
            (*handlers)["message"] = handler;
            start_port(internals, **found);
            return js::Value::undefined();
        });
    static constexpr std::string_view port_events[] = { "messageerror" };
    define_event_handlers(in, *port, port_events);

    define_interface(in, "MessageChannel", nullptr,
        [](js::Interpreter& interp, Args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Heap::NoCollect const no_collect(interp.heap());
            auto* first = interp.heap().allocate<MessagePortObject>(internals.prototype("MessagePort"));
            auto* second = interp.heap().allocate<MessagePortObject>(internals.prototype("MessagePort"));
            first->entangled = second;
            second->entangled = first;
            js::Object* channel = interp.new_object(internals.prototype("MessageChannel"));
            channel->put(interp.key("port1"), js::Value::object(first), js::Enumerable);
            channel->put(interp.key("port2"), js::Value::object(second), js::Enumerable);
            return js::Value::object(channel);
        },
        0);

    // window.postMessage(message, targetOrigin): to the window itself, as a
    // task; a target origin that is neither "*", "/" nor the page's own is
    // silently not this window.
    js::define_method(interpreter, *interpreter.global(), "postMessage", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Value const data = js::argument(args, 0);
        js::Value const target_origin = js::argument(args, 1);
        std::string const own = internals.url.serialize_origin();
        if (target_origin.is_object()) {
            std::optional<js::Value> const origin_member = interp.get(*target_origin.as_object(), interp.key("targetOrigin"));
            if (!origin_member)
                return std::nullopt;
            if (!origin_member->is_undefined()) {
                std::optional<std::string> const text = internals.to_utf8(*origin_member);
                if (!text)
                    return std::nullopt;
                if (*text != "*" && *text != "/" && *text != own)
                    return js::Value::undefined();
            }
        } else if (!target_origin.is_undefined()) {
            std::optional<std::string> const text = internals.to_utf8(target_origin);
            if (!text)
                return std::nullopt;
            if (*text != "*" && *text != "/") {
                std::optional<net::Url> const parsed = net::parse_url(*text);
                if (!parsed)
                    return interp.throw_error(js::ErrorType::SyntaxError, "Failed to execute 'postMessage' on 'Window': Invalid target origin '" + *text + "'");
                if (parsed->serialize_origin() != own)
                    return js::Value::undefined();
            }
        }
        auto payload = std::make_shared<js::Persistent>(interp.heap(), data);
        internals.post_task([&internals, payload, own] {
            deliver_message(internals, internals.interpreter.global(), payload->value(), own, js::Value::object(internals.interpreter.global()));
        });
        return js::Value::undefined();
    });
}

// --- AbortController / AbortSignal --------------------------------------------------------

void install_abort(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;

    js::Object* signal = define_interface(in, "AbortSignal", in.prototype("EventTarget"));
    define_getter(in, *signal, "aborted", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AbortSignalObject*> const found = this_signal(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::boolean((*found)->aborted);
    });
    define_getter(in, *signal, "reason", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AbortSignalObject*> const found = this_signal(interp, this_value);
        if (!found)
            return std::nullopt;
        return (*found)->reason;
    });
    js::define_method(interpreter, *signal, "throwIfAborted", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AbortSignalObject*> const found = this_signal(interp, this_value);
        if (!found)
            return std::nullopt;
        if ((*found)->aborted)
            return interp.throw_value((*found)->reason);
        return js::Value::undefined();
    });
    static constexpr std::string_view signal_events[] = { "abort" };
    define_event_handlers(in, *signal, signal_events);
    // The statics: an already-aborted signal, and one that aborts after a delay.
    js::Value const signal_constructor = *interpreter.get(*interpreter.global(), interpreter.key("AbortSignal"));
    js::define_method(interpreter, *signal_constructor.as_object(), "abort", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        js::Value const reason = js::argument(args, 0);
        interp.root(reason);
        AbortSignalObject* made = new_abort_signal(internals);
        interp.root(js::Value::object(made));
        made->aborted = true;
        made->reason = reason.is_undefined() ? abort_error(internals, "signal is aborted without reason") : reason;
        return js::Value::object(made);
    });
    js::define_method(interpreter, *signal_constructor.as_object(), "timeout", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<double> const delay = interp.to_number(js::argument(args, 0));
        if (!delay)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        AbortSignalObject* made = new_abort_signal(internals);
        js::Value const signal_value = js::Value::object(made);
        interp.root(signal_value);
        js::ClosureFunction* fire = interp.new_closure("timeout", 0, { signal_value },
            [](js::Interpreter& inner, js::ClosureFunction& self, js::Value const&, Args) -> Native {
                Realm::Internals& internals_inner = internals_of(inner);
                auto* target = static_cast<AbortSignalObject*>(self.slot(0).as_object());
                js::Interpreter::Roots const inner_roots(inner);
                js::Value const error = dom_error(internals_inner, "TimeoutError", "signal timed out");
                inner.root(error);
                signal_abort(internals_inner, *target, error);
                return js::Value::undefined();
            });
        interp.root(js::Value::object(fire));
        schedule_native(internals, std::isnan(*delay) || *delay < 0 ? 0 : *delay, js::Value::object(fire));
        return signal_value;
    });

    js::Object* controller = define_interface(in, "AbortController", nullptr,
        [](js::Interpreter& interp, Args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Heap::NoCollect const no_collect(interp.heap());
            js::Object* object = interp.new_object(internals.prototype("AbortController"));
            object->put(interp.key("signal"), js::Value::object(new_abort_signal(internals)), js::Enumerable);
            return js::Value::object(object);
        },
        0);
    js::define_method(interpreter, *controller, "abort", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        if (!this_value.is_object())
            return interp.throw_type_error("Illegal invocation");
        std::optional<js::Value> const signal_value = interp.get(*this_value.as_object(), interp.key("signal"));
        if (!signal_value)
            return std::nullopt;
        if (!signal_value->is_object())
            return interp.throw_type_error("Illegal invocation");
        auto* target = dynamic_cast<AbortSignalObject*>(signal_value->as_object());
        if (target == nullptr)
            return interp.throw_type_error("Illegal invocation");
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        interp.root(*signal_value);
        signal_abort(internals_of(interp), *target, js::argument(args, 0));
        return js::Value::undefined();
    });
}

} // namespace

AbortSignalObject* new_abort_signal(Realm::Internals& in)
{
    return in.interpreter.heap().allocate<AbortSignalObject>(in.prototype("AbortSignal"));
}

js::Value abort_error(Realm::Internals& in, std::string_view message)
{
    return dom_error(in, "AbortError", message);
}

void signal_abort(Realm::Internals& in, AbortSignalObject& signal, js::Value const& reason)
{
    // "Signal abort" (DOM §3.2.3): once only; the reason, then the event.
    if (signal.aborted)
        return;
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&signal));
    in.interpreter.root(reason);
    signal.aborted = true;
    signal.reason = reason.is_undefined() ? abort_error(in, "signal is aborted without reason") : reason;
    EventObject* event = in.new_event("Event", "abort", false, false);
    in.interpreter.root(js::Value::object(event));
    event->is_trusted = true;
    in.dispatch(*event, &signal);
}

void install_tasks(Realm::Internals& in)
{
    install_abort(in);
    install_message_channel(in);
}

}
