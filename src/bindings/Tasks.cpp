#include "bindings/Internal.h"

// The page's task-posting interfaces: AbortController and AbortSignal
// (DOM §3.2 — a flag, a reason and an abort event that a fetch watches),
// MessageChannel and MessagePort (HTML §9.5: two entangled ports, a
// message posted on one delivered to the other as a task, which is how a
// library schedules "the next turn" without a timer), and
// window.postMessage between the windows of a page's agent. A message is
// serialized at the call and delivered as a clone made in the receiving
// realm (StructuredClone.cpp), with the ports it transferred.

#include "js/Object.h"

#include <cmath>
#include <deque>
#include <memory>
#include <optional>
#include <span>
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
    timer.id = in.agent.next_timer_id++;
    timer.due = in.now() + delay_ms;
    timer.sequence = in.agent.next_sequence++;
    timer.owner = &in;
    timer.callback = std::make_unique<js::Persistent>(in.interpreter.heap(), function);
    in.agent.timers.push_back(std::move(timer));
}

// --- MessagePort ---------------------------------------------------------------------------

std::optional<MessagePortObject*> this_port(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* port = dynamic_cast<MessagePortObject*>(this_value.as_object()))
            return port;
    }
    return interp.throw_type_error("Illegal invocation");
}

// A message on its way in a task: its serialized form, and the ports it
// transferred, kept alive until it arrives, since nothing traces a task.
struct MessageInFlight {
    MessageInFlight(js::Heap& heap, std::shared_ptr<SerializedMessage const> the_message)
        : message(std::move(the_message))
    {
        for (MessagePortObject* const port : transferred_ports(*message))
            ports.emplace_back(heap, js::Value::object(port));
    }
    std::shared_ptr<SerializedMessage const> message;
    std::vector<js::Persistent> ports;
};

// A MessageEvent's ports (HTML §9.4.1, a FrozenArray<MessagePort>): the
// MessagePorts among the values the message transferred, in the transfer
// list's order — not its buffers — as one frozen array the event keeps, so
// every read answers the same array.
js::Value frozen_ports(Realm::Internals& in, std::span<js::Value const> transferred)
{
    std::vector<js::Value> ports;
    for (js::Value const& value : transferred) {
        if (dynamic_cast<MessagePortObject*>(value.as_object()) != nullptr)
            ports.push_back(value);
    }
    js::Interpreter::Roots const roots(in.interpreter);
    js::ArrayObject* const list = in.interpreter.new_array(ports);
    in.interpreter.root(js::Value::object(list));
    // A fresh ordinary array: freezing it runs no script and cannot fail.
    (void)js::set_integrity_level(in.interpreter, *list, true);
    return js::Value::object(list);
}

// Fires a MessageEvent carrying `data` at `target` (a port or the window),
// with the ports the message transferred; a message a window posted carries
// the origin of that window's document.
void deliver_message(Realm::Internals& in, js::Object* target, js::Value const& data, std::string_view origin, js::Value const& source,
    std::span<js::Value const> transferred, std::optional<Origin> const& sender_origin = std::nullopt)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(target));
    in.interpreter.root(data);
    in.interpreter.root(source);
    in.trace("message delivered from " + std::string(origin));
    EventObject* event = in.new_event("MessageEvent", "message", false, false);
    in.interpreter.root(js::Value::object(event));
    event->detail_value = data;
    event->origin = std::string(origin);
    event->source_value = source;
    event->sender_origin = sender_origin;
    event->is_trusted = true;
    event->ports = frozen_ports(in, transferred);
    in.dispatch(*event, target);
}

// A message that could not be made in the receiving realm: a messageerror
// event in its place (HTML §9.3.3, §9.5.3).
void deliver_message_error(Realm::Internals& in, js::Object* target, std::string_view origin, js::Value const& source,
    std::optional<Origin> const& sender_origin = std::nullopt)
{
    in.interpreter.clear_exception();
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(target));
    in.interpreter.root(source);
    EventObject* event = in.new_event("MessageEvent", "messageerror", false, false);
    in.interpreter.root(js::Value::object(event));
    event->origin = std::string(origin);
    event->source_value = source;
    event->sender_origin = sender_origin;
    event->is_trusted = true;
    event->ports = frozen_ports(in, {});
    in.dispatch(*event, target);
}

// The task that delivers the message at the front of a port's message queue
// (HTML §9.5.3), in the port's realm: one is posted for each message, as the
// message is queued on a port that has started or as the port starts. A port
// closed since, or transferred — its queue gone, in order, to the port made
// for it in the receiving realm — delivers nothing. The message is made in
// the port's realm while it is still in the queue, which keeps the ports it
// transferred alive, and only then taken out.
void post_port_task(MessagePortObject& port)
{
    Realm::Internals& in = port.realm().internals();
    auto target = std::make_shared<js::Persistent>(in.interpreter.heap(), js::Value::object(&port));
    in.post_task([&in, target] {
        auto* destination = static_cast<MessagePortObject*>(target->value().as_object());
        if (destination->closed || destination->detached || destination->pending.empty())
            return;
        std::shared_ptr<SerializedMessage const> const message = destination->pending.front();
        js::Interpreter::Roots const roots(in.interpreter);
        std::optional<Deserialized> const received = structured_deserialize(in, *message);
        destination->pending.pop_front();
        if (!received) {
            deliver_message_error(in, destination, in.url.serialize_origin(), js::Value::null());
            return;
        }
        deliver_message(in, destination, received->value, in.url.serialize_origin(), js::Value::null(), received->transferred);
    });
}

// Adds a message to a port's message queue, with a task to deliver it when
// the port has started; otherwise it waits for the port to start, here or in
// the realm the port is on its way to.
void enqueue_port_message(MessagePortObject& port, std::shared_ptr<SerializedMessage const> const& message)
{
    if (port.closed)
        return;
    port.pending.push_back(message);
    if (port.started && !port.detached)
        post_port_task(port);
}

void start_port(MessagePortObject& port)
{
    if (port.started || port.detached)
        return;
    port.started = true;
    for (std::size_t i = 0; i < port.pending.size(); ++i)
        post_port_task(port);
}

void install_message_channel(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;

    js::Object* port = define_interface(in, "MessagePort", in.prototype("EventTarget"));
    js::define_method(interpreter, *port, "postMessage", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        // The port's post message steps (HTML §9.5.3): the transfer list may
        // not hold this port, and a list holding the entangled port dooms the
        // message; the value is serialized now, in this realm, whether or not
        // anything will receive it.
        std::optional<MessagePortObject*> const found = this_port(interp, this_value);
        if (!found)
            return std::nullopt;
        MessagePortObject& source = **found;
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<std::vector<js::Value>> const transfer = transfer_or_options(internals, js::argument(args, 1));
        if (!transfer)
            return std::nullopt;
        if (source.detached)
            return js::Value::undefined();
        MessagePortObject* const target = source.entangled;
        if (target != nullptr)
            interp.root(js::Value::object(target));
        bool doomed = false;
        for (std::size_t i = 0; i < transfer->size(); ++i) {
            js::Object const* const entry = (*transfer)[i].as_object();
            if (entry == &source)
                return internals.throw_dom_exception("DataCloneError", "Port at index " + std::to_string(i) + " contains the source port.");
            if (target != nullptr && entry == target)
                doomed = true;
        }
        std::shared_ptr<SerializedMessage const> const message = structured_serialize(internals, js::argument(args, 0), *transfer);
        if (!message)
            return std::nullopt;
        if (target == nullptr || doomed)
            return js::Value::undefined();
        enqueue_port_message(*target, message);
        return js::Value::undefined();
    });
    js::define_method(interpreter, *port, "start", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<MessagePortObject*> const found = this_port(interp, this_value);
        if (!found)
            return std::nullopt;
        start_port(**found);
        return js::Value::undefined();
    });
    js::define_method(interpreter, *port, "close", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<MessagePortObject*> const found = this_port(interp, this_value);
        if (!found)
            return std::nullopt;
        if ((*found)->detached)
            return js::Value::undefined();
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
            start_port(**found);
            return js::Value::undefined();
        });
    static constexpr std::string_view port_events[] = { "messageerror" };
    define_event_handlers(in, *port, port_events);

    define_interface(in, "MessageChannel", nullptr,
        [](js::Interpreter& interp, Args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Heap::NoCollect const no_collect(interp.heap());
            auto* first = interp.heap().allocate<MessagePortObject>(internals.prototype("MessagePort"), *internals.realm_record);
            auto* second = interp.heap().allocate<MessagePortObject>(internals.prototype("MessagePort"), *internals.realm_record);
            first->entangled = second;
            second->entangled = first;
            js::Object* channel = interp.heap().allocate<PlainPlatformObject>(internals.prototype("MessageChannel"));
            channel->put(interp.key("port1"), js::Value::object(first), js::Enumerable);
            channel->put(interp.key("port2"), js::Value::object(second), js::Enumerable);
            return js::Value::object(channel);
        },
        0);

    // window.postMessage(message, targetOrigin, transfer) and (message,
    // options) (HTML §9.3.3): to the window the method is on, from the window
    // whose script called it, the incumbent realm's, as a task on the agent's
    // loop. "/" is the sender's origin, and so are one argument and options
    // without a target origin; a target origin the receiving document does
    // not have when the task runs delivers nothing. The message is serialized
    // at the call, so what cannot be cloned throws here and a transferred
    // buffer is detached at once, and it is deserialized into the receiving
    // realm when the task runs.
    js::define_method(interpreter, *interpreter.global(), "postMessage", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& target = internals_of(interp);
        js::RealmRecord* const incumbent = interp.incumbent_realm();
        Realm::Internals& sender = incumbent != nullptr && incumbent->host_defined != nullptr
            ? static_cast<Realm*>(incumbent->host_defined)->internals()
            : target;
        js::Interpreter::Roots const roots(interp);
        js::Value const data = js::argument(args, 0);
        js::Value const target_argument = js::argument(args, 1);
        std::optional<std::string> target_text;
        std::optional<std::vector<js::Value>> transfer = std::vector<js::Value> {};
        // WebIDL's overload resolution chooses by the number of arguments:
        // three or more are (message, targetOrigin, transfer), the second
        // converted to a string whatever it is; two are (message, options)
        // when the second is an object, null or undefined, and (message,
        // targetOrigin) otherwise; one is (message) with the default options.
        if (args.size() < 3 && (target_argument.is_object() || target_argument.is_nullish())) {
            // WindowPostMessageOptions: the inherited transfer member first.
            transfer = options_transfer(target, target_argument);
            if (!transfer)
                return std::nullopt;
            if (target_argument.is_object()) {
                std::optional<js::Value> const member = interp.get(*target_argument.as_object(), interp.key("targetOrigin"));
                if (!member)
                    return std::nullopt;
                if (!member->is_undefined()) {
                    target_text = target.to_utf8(*member);
                    if (!target_text)
                        return std::nullopt;
                }
            }
        } else {
            target_text = target.to_utf8(target_argument);
            if (!target_text)
                return std::nullopt;
            js::Value const transfer_argument = js::argument(args, 2);
            if (!transfer_argument.is_undefined()) {
                transfer = transfer_sequence(target, transfer_argument);
                if (!transfer)
                    return std::nullopt;
            }
        }
        std::string const sender_origin = sender.origin_url.serialize_origin();
        // The origin a delivery requires; none for "*".
        std::optional<std::string> required;
        if (!target_text || *target_text == "/") {
            required = sender_origin;
        } else if (*target_text != "*") {
            std::optional<net::Url> const parsed = net::parse_url(*target_text);
            if (!parsed)
                return target.throw_dom_exception("SyntaxError",
                    "Failed to execute 'postMessage' on 'Window': Invalid target origin '" + *target_text + "' in a call to 'postMessage'.");
            required = parsed->serialize_origin();
        }
        std::shared_ptr<SerializedMessage const> const message = structured_serialize(target, data, *transfer);
        if (!message)
            return std::nullopt;
        bool const to_itself = &sender == &target;
        Origin const sender_document_origin = document_origin(sender);
        auto flight = std::make_shared<MessageInFlight>(interp.heap(), message);
        auto source = std::make_shared<js::Persistent>(interp.heap(), js::Value::object(sender.window_proxy()));
        target.post_task([&target, flight, source, required, sender_origin, sender_document_origin, to_itself] {
            if (required) {
                // An opaque origin matches nothing but the window itself.
                std::string const own = target.origin_url.serialize_origin();
                bool const opaque = own == "null" || *required == "null";
                if (opaque ? !to_itself : *required != own)
                    return;
            }
            js::Object* const window = target.window_proxy();
            js::Interpreter::Roots const task_roots(target.interpreter);
            std::optional<Deserialized> const received = structured_deserialize(target, *flight->message);
            if (!received) {
                deliver_message_error(target, window, sender_origin, source->value(), sender_document_origin);
                return;
            }
            deliver_message(target, window, received->value, sender_origin, source->value(), received->transferred, sender_document_origin);
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
            js::Object* object = interp.heap().allocate<PlainPlatformObject>(internals.prototype("AbortController"));
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
