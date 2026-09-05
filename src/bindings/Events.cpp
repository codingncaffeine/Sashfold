#include "bindings/Internal.h"

// Events (DOM §2): the dispatch algorithm over the capture, target and
// bubble phases, addEventListener and its options, the on<type> handlers
// (HTML §8.1.8) including the ones compiled from content attributes, and
// the Event interfaces a page constructs.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

// Event types whose body attribute handler belongs to the window (HTML
// §8.1.8.2, the WindowEventHandlers set).
bool is_window_event_type(std::string_view type)
{
    static constexpr std::string_view types[] = {
        "afterprint", "beforeprint", "beforeunload", "hashchange", "languagechange", "message", "messageerror",
        "offline", "online", "pagehide", "pageshow", "popstate", "rejectionhandled", "storage",
        "unhandledrejection", "unload", "blur", "error", "focus", "load", "resize", "scroll"
    };
    for (std::string_view const candidate : types) {
        if (candidate == type)
            return true;
    }
    return false;
}

std::optional<EventObject*> this_event(js::Interpreter& interpreter, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* event = dynamic_cast<EventObject*>(this_value.as_object()))
            return event;
    }
    return interpreter.throw_type_error("Illegal invocation");
}

// The element behind an event target, when it is one.
dom::Element* element_of(Realm::Internals& in, js::Object* target)
{
    NodeWrapper* wrapper = in.wrapper_of(js::Value::object(target));
    if (!wrapper || !wrapper->node().is_element())
        return nullptr;
    return static_cast<dom::Element*>(&wrapper->node());
}

// The on<type> handler of a target, the content attribute compiled when
// it is the current one (§8.1.8.1 "getting the current value of the event
// handler"). Null when there is none.
js::Value handler_value(Realm::Internals& in, js::Object* target, std::string_view type)
{
    HandlerMap* map = in.handlers_of(target);
    if (!map)
        return js::Value::null();
    std::string const key(type);
    dom::Element* element = element_of(in, target);
    bool from_body = false;
    if (!element && target == in.interpreter.global() && is_window_event_type(type)) {
        // The body's onload="…" is the window's handler.
        for (dom::Node* child : in.document.children()) {
            if (!child->is_element())
                continue;
            for (dom::Node* grandchild : child->children()) {
                if (grandchild->is_element() && static_cast<dom::Element*>(grandchild)->is_html("body")) {
                    element = static_cast<dom::Element*>(grandchild);
                    from_body = true;
                }
            }
        }
    }
    if (element) {
        dom::Attr const* attribute = element->find_attribute("on" + key);
        auto it = map->find(key);
        if (attribute) {
            if (it == map->end() || (it->second.from_attribute && it->second.source != attribute->value)) {
                std::u16string const body = js::utf16_from_utf8(attribute->value);
                std::optional<js::Value> compiled = in.interpreter.compile_function(u"event", body);
                EventHandler handler;
                handler.from_attribute = true;
                handler.source = attribute->value;
                if (compiled) {
                    handler.function = *compiled;
                } else {
                    js::Value const thrown = in.interpreter.take_exception();
                    in.report_uncaught(thrown, from_body ? "<body on" + key + ">" : "on" + key + " attribute");
                }
                (*map)[key] = handler;
            }
        } else if (it != map->end() && it->second.from_attribute) {
            map->erase(it);
        }
    }
    auto const it = map->find(key);
    if (it == map->end() || !js::Interpreter::is_callable(it->second.function))
        return js::Value::null();
    return it->second.function;
}

// Calls the target's handler for the event, if it has one; a `false`
// return cancels the event (§8.1.8.1 step 5, except for error events).
void call_handler(Realm::Internals& in, js::Object* target, EventObject& event)
{
    js::Value const handler = handler_value(in, target, event.type);
    if (!handler.is_object())
        return;
    js::Interpreter& interpreter = in.interpreter;
    js::Interpreter::Roots const roots(interpreter);
    interpreter.root(handler);
    js::Value const event_value = js::Value::object(&event);
    js::Value const arguments[1] = { event_value };
    Realm::Internals::Entry const entry(in);
    js::Outcome const outcome = interpreter.call_outcome(handler, js::Value::object(target), arguments);
    if (!outcome.ok) {
        if (!interpreter.terminated())
            in.report_uncaught(outcome.value, "on" + event.type + " handler");
        return;
    }
    if (event.type != "error" && outcome.value.is_boolean() && !outcome.value.as_boolean() && event.cancelable)
        event.default_prevented = true;
}

// Invokes the listeners of one target for the phase (§2.9.5 "inner invoke").
void invoke(Realm::Internals& in, js::Object* target, EventObject& event, EventObject::Phase phase, bool capture)
{
    event.current_target = js::Value::object(target);
    event.phase = phase;
    if (!capture)
        call_handler(in, target, event);
    if (event.stop_immediate)
        return;
    std::vector<ListenerEntry>* list = in.listeners_of(target);
    if (!list)
        return;
    // The list may change while listeners run: remember the ids and find
    // each again, skipping any that has been removed since.
    std::vector<std::uint64_t> ids;
    for (ListenerEntry const& entry : *list) {
        if (entry.type == event.type && entry.listener.capture == capture)
            ids.push_back(entry.listener.id);
    }
    js::Interpreter& interpreter = in.interpreter;
    for (std::uint64_t const id : ids) {
        if (event.stop_immediate)
            break;
        list = in.listeners_of(target);
        auto const it = std::find_if(list->begin(), list->end(),
            [id](ListenerEntry const& entry) { return entry.listener.id == id; });
        if (it == list->end())
            continue;
        js::Interpreter::Roots const roots(interpreter);
        js::Value const callback = interpreter.root(it->listener.callback);
        bool const passive = it->listener.passive;
        if (it->listener.once)
            list->erase(it);
        event.in_passive_listener = passive;
        js::Value const arguments[1] = { js::Value::object(&event) };
        if (js::Interpreter::is_callable(callback)) {
            in.call_reporting(callback, js::Value::object(target), arguments, event.type + " listener");
        } else if (callback.is_object()) {
            std::optional<js::Value> const handle_event = interpreter.get(callback, "handleEvent");
            if (!handle_event) {
                js::Value const thrown = interpreter.take_exception();
                in.report_uncaught(thrown, event.type + " listener");
            } else if (js::Interpreter::is_callable(*handle_event)) {
                in.call_reporting(*handle_event, callback, arguments, event.type + " listener");
            }
        }
        event.in_passive_listener = false;
        if (interpreter.terminated())
            break;
    }
}

// The listener options: a boolean is `capture`; an object carries the
// three flags (§2.7).
struct ListenerOptions {
    bool capture = false;
    bool once = false;
    bool passive = false;
};

std::optional<ListenerOptions> parse_options(js::Interpreter& interpreter, js::Value const& options)
{
    ListenerOptions parsed;
    if (options.is_object()) {
        for (auto const& [name, flag] : { std::pair { "capture", &parsed.capture }, std::pair { "once", &parsed.once },
                 std::pair { "passive", &parsed.passive } }) {
            std::optional<js::Value> const value = interpreter.get(options, name);
            if (!value)
                return std::nullopt;
            *flag = js::Interpreter::to_boolean(*value);
        }
    } else {
        parsed.capture = js::Interpreter::to_boolean(options);
    }
    return parsed;
}

Native add_event_listener(js::Interpreter& interpreter, js::Value const& this_value, Args args)
{
    Realm::Internals& in = internals_of(interpreter);
    if (!this_value.is_object())
        return interpreter.throw_type_error("Illegal invocation");
    std::vector<ListenerEntry>* list = in.listeners_of(this_value.as_object());
    if (!list)
        return interpreter.throw_type_error("Illegal invocation");
    std::optional<std::string> const type = in.to_utf8(js::argument(args, 0));
    if (!type)
        return std::nullopt;
    js::Value const callback = js::argument(args, 1);
    if (!callback.is_object())
        return js::Value::undefined(); // null is allowed and does nothing
    std::optional<ListenerOptions> const options = parse_options(interpreter, js::argument(args, 2));
    if (!options)
        return std::nullopt;
    for (ListenerEntry const& entry : *list) {
        if (entry.type == *type && entry.listener.callback == callback && entry.listener.capture == options->capture)
            return js::Value::undefined(); // already registered
    }
    Listener listener;
    listener.callback = callback;
    listener.id = in.next_listener_id++;
    listener.capture = options->capture;
    listener.once = options->once;
    listener.passive = options->passive;
    list->push_back(ListenerEntry { *type, listener });
    return js::Value::undefined();
}

Native remove_event_listener(js::Interpreter& interpreter, js::Value const& this_value, Args args)
{
    Realm::Internals& in = internals_of(interpreter);
    if (!this_value.is_object())
        return interpreter.throw_type_error("Illegal invocation");
    std::vector<ListenerEntry>* list = in.listeners_of(this_value.as_object());
    if (!list)
        return interpreter.throw_type_error("Illegal invocation");
    std::optional<std::string> const type = in.to_utf8(js::argument(args, 0));
    if (!type)
        return std::nullopt;
    js::Value const callback = js::argument(args, 1);
    std::optional<ListenerOptions> const options = parse_options(interpreter, js::argument(args, 2));
    if (!options)
        return std::nullopt;
    auto const it = std::find_if(list->begin(), list->end(), [&](ListenerEntry const& entry) {
        return entry.type == *type && entry.listener.callback == callback && entry.listener.capture == options->capture;
    });
    if (it != list->end())
        list->erase(it);
    return js::Value::undefined();
}

Native dispatch_event_native(js::Interpreter& interpreter, js::Value const& this_value, Args args)
{
    Realm::Internals& in = internals_of(interpreter);
    if (!this_value.is_object() || !in.listeners_of(this_value.as_object()))
        return interpreter.throw_type_error("Illegal invocation");
    js::Value const event_value = js::argument(args, 0);
    EventObject* event = event_value.is_object() ? dynamic_cast<EventObject*>(event_value.as_object()) : nullptr;
    if (!event)
        return interpreter.throw_type_error("parameter 1 is not of type 'Event'");
    if (event->dispatching || !event->initialized)
        return in.throw_dom_exception("InvalidStateError", "The event is already being dispatched");
    event->is_trusted = false;
    return js::Value::boolean(in.dispatch(*event, this_value.as_object()));
}

// Reads the common members of an EventInit dictionary.
std::optional<bool> init_flag(js::Interpreter& interpreter, js::Value const& init, std::string_view name, bool fallback)
{
    if (!init.is_object())
        return fallback;
    std::optional<js::Value> const value = interpreter.get(init, name);
    if (!value)
        return std::nullopt;
    return value->is_undefined() ? fallback : js::Interpreter::to_boolean(*value);
}

std::optional<double> init_number(js::Interpreter& interpreter, js::Value const& init, std::string_view name, double fallback)
{
    if (!init.is_object())
        return fallback;
    std::optional<js::Value> const value = interpreter.get(init, name);
    if (!value)
        return std::nullopt;
    if (value->is_undefined())
        return fallback;
    return interpreter.to_number(*value);
}

std::optional<std::string> init_string(js::Interpreter& interpreter, js::Value const& init, std::string_view name)
{
    if (!init.is_object())
        return std::string();
    std::optional<js::Value> const value = interpreter.get(init, name);
    if (!value)
        return std::nullopt;
    if (value->is_undefined())
        return std::string();
    return internals_of(interpreter).to_utf8(*value);
}

std::optional<js::Value> init_value(js::Interpreter& interpreter, js::Value const& init, std::string_view name)
{
    if (!init.is_object())
        return js::Value::null();
    std::optional<js::Value> const value = interpreter.get(init, name);
    if (!value)
        return std::nullopt;
    return value->is_undefined() ? js::Value::null() : *value;
}

// `new Event(type, init)` for every interface: the fields the dictionary
// names are read by the interface's constructor.
js::NativeFunction::ConstructCallback event_constructor(std::string interface)
{
    return [interface](js::Interpreter& interpreter, Args args, js::Object*) -> Native {
        Realm::Internals& in = internals_of(interpreter);
        if (args.empty())
            return interpreter.throw_type_error("Failed to construct '" + interface + "': 1 argument required");
        std::optional<std::string> const type = in.to_utf8(args[0]);
        if (!type)
            return std::nullopt;
        js::Value const init = js::argument(args, 1);
        if (!init.is_undefined() && !init.is_object())
            return interpreter.throw_type_error("Failed to construct '" + interface + "': parameter 2 is not a dictionary");
        std::optional<bool> const bubbles = init_flag(interpreter, init, "bubbles", false);
        std::optional<bool> const cancelable = init_flag(interpreter, init, "cancelable", false);
        std::optional<bool> const composed = init_flag(interpreter, init, "composed", false);
        if (!bubbles || !cancelable || !composed)
            return std::nullopt;
        js::Interpreter::Roots const roots(interpreter);
        interpreter.root(init);
        EventObject* event = in.new_event(interface, *type, *bubbles, *cancelable);
        interpreter.root(js::Value::object(event));
        event->composed = *composed;
        if (interface == "CustomEvent") {
            std::optional<js::Value> const detail = init_value(interpreter, init, "detail");
            if (!detail)
                return std::nullopt;
            event->detail_value = *detail;
        }
        if (interface == "UIEvent" || interface == "MouseEvent" || interface == "KeyboardEvent" || interface == "InputEvent"
            || interface == "FocusEvent" || interface == "PointerEvent" || interface == "WheelEvent") {
            std::optional<double> const detail = init_number(interpreter, init, "detail", 0);
            if (!detail)
                return std::nullopt;
            event->detail = static_cast<int>(*detail);
        }
        if (interface == "MouseEvent" || interface == "PointerEvent" || interface == "WheelEvent" || interface == "KeyboardEvent") {
            for (auto const& [name, flag] : { std::pair { "ctrlKey", &event->ctrl_key }, std::pair { "shiftKey", &event->shift_key },
                     std::pair { "altKey", &event->alt_key }, std::pair { "metaKey", &event->meta_key } }) {
                std::optional<bool> const value = init_flag(interpreter, init, name, false);
                if (!value)
                    return std::nullopt;
                *flag = *value;
            }
        }
        if (interface == "MouseEvent" || interface == "PointerEvent" || interface == "WheelEvent") {
            for (auto const& [name, field] : { std::pair { "clientX", &event->client_x }, std::pair { "clientY", &event->client_y },
                     std::pair { "screenX", &event->screen_x }, std::pair { "screenY", &event->screen_y },
                     std::pair { "button", &event->button }, std::pair { "buttons", &event->buttons } }) {
                std::optional<double> const value = init_number(interpreter, init, name, 0);
                if (!value)
                    return std::nullopt;
                *field = static_cast<int>(*value);
            }
            std::optional<js::Value> const related = init_value(interpreter, init, "relatedTarget");
            if (!related)
                return std::nullopt;
            event->related_target = *related;
        }
        if (interface == "WheelEvent") {
            std::optional<double> const dx = init_number(interpreter, init, "deltaX", 0);
            std::optional<double> const dy = init_number(interpreter, init, "deltaY", 0);
            if (!dx || !dy)
                return std::nullopt;
            event->delta_x = *dx;
            event->delta_y = *dy;
        }
        if (interface == "KeyboardEvent") {
            std::optional<std::string> key = init_string(interpreter, init, "key");
            std::optional<std::string> code = init_string(interpreter, init, "code");
            std::optional<double> const key_code = init_number(interpreter, init, "keyCode", 0);
            std::optional<bool> const repeat = init_flag(interpreter, init, "repeat", false);
            if (!key || !code || !key_code || !repeat)
                return std::nullopt;
            event->key = std::move(*key);
            event->code = std::move(*code);
            event->key_code = static_cast<int>(*key_code);
            event->repeat = *repeat;
        }
        if (interface == "InputEvent") {
            std::optional<std::string> data = init_string(interpreter, init, "data");
            std::optional<std::string> input_type = init_string(interpreter, init, "inputType");
            if (!data || !input_type)
                return std::nullopt;
            event->data = std::move(*data);
            event->input_type = std::move(*input_type);
        }
        if (interface == "FocusEvent") {
            std::optional<js::Value> const related = init_value(interpreter, init, "relatedTarget");
            if (!related)
                return std::nullopt;
            event->related_target = *related;
        }
        if (interface == "PopStateEvent") {
            std::optional<js::Value> const state = init_value(interpreter, init, "state");
            if (!state)
                return std::nullopt;
            event->detail_value = *state;
        }
        return js::Value::object(event);
    };
}

// A read-only accessor over an EventObject field.
template<typename Read>
void event_getter(Realm::Internals& in, js::Object& prototype, std::string_view name, Read read)
{
    define_getter(in, prototype, name, [read](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native {
        std::optional<EventObject*> const event = this_event(interpreter, this_value);
        if (!event)
            return std::nullopt;
        return read(internals_of(interpreter), **event);
    });
}

// A pointer position relative to the target's box, for offsetX/offsetY.
std::pair<double, double> offset_in_target(Realm::Internals& in, EventObject const& event)
{
    dom::Element* element = event.target.is_object() ? element_of(in, event.target.as_object()) : nullptr;
    if (!element || !in.hooks.layout_box)
        return { event.client_x, event.client_y };
    std::optional<LayoutBox> const box = in.hooks.layout_box(*element);
    if (!box)
        return { event.client_x, event.client_y };
    std::pair<int, int> const scroll = in.hooks.scroll_position ? in.hooks.scroll_position() : std::pair<int, int> { 0, 0 };
    return { event.client_x + scroll.first - box->x, event.client_y + scroll.second - box->y };
}

} // namespace

// --- Internals ---------------------------------------------------------------------------------

EventObject* Realm::Internals::new_event(std::string_view interface, std::string_view type, bool bubbles, bool cancelable)
{
    js::Object* proto = prototype(interface);
    if (!proto)
        proto = prototype("Event");
    EventObject* event = interpreter.heap().allocate<EventObject>(proto);
    event->type = std::string(type);
    event->bubbles = bubbles;
    event->cancelable = cancelable;
    event->initialized = true;
    event->time_stamp = now() - time_origin;
    return event;
}

std::vector<ListenerEntry>* Realm::Internals::listeners_of(js::Object* target)
{
    if (target == interpreter.global())
        return &window_listeners;
    if (auto* event_target = dynamic_cast<EventTargetObject*>(target))
        return &event_target->listeners;
    return nullptr;
}

HandlerMap* Realm::Internals::handlers_of(js::Object* target)
{
    if (target == interpreter.global())
        return &window_handlers;
    if (auto* event_target = dynamic_cast<EventTargetObject*>(target))
        return &event_target->handlers;
    return nullptr;
}

bool Realm::Internals::dispatch(EventObject& event, js::Object* target)
{
    // §2.9 "dispatch". The path runs from the target up through its
    // ancestors to the document and the window; every wrapper on it is
    // rooted while listeners run.
    js::Interpreter::Roots const roots(interpreter);
    interpreter.root(js::Value::object(&event));
    interpreter.root(js::Value::object(target));
    ++stats.events_dispatched;
    event.dispatching = true;
    event.target = js::Value::object(target);
    event.stop_propagation = false;
    event.stop_immediate = false;
    std::vector<js::Object*> path;
    path.push_back(target);
    if (NodeWrapper* wrapper = wrapper_of(js::Value::object(target))) {
        dom::Node* node = &wrapper->node();
        for (dom::Node* ancestor = node->parent(); ancestor; ancestor = ancestor->parent()) {
            js::Object* ancestor_wrapper = wrap(*ancestor);
            interpreter.root(js::Value::object(ancestor_wrapper));
            path.push_back(ancestor_wrapper);
        }
        // The document's parent is the window, except for load (§2.9.1).
        if (&node->root() == &document && event.type != "load")
            path.push_back(interpreter.global());
    }
    js::Value const previous_event = current_event;
    current_event = js::Value::object(&event);
    for (std::size_t i = path.size(); i-- > 1 && !event.stop_propagation;)
        invoke(*this, path[i], event, EventObject::Phase::Capturing, true);
    if (!event.stop_propagation) {
        invoke(*this, path[0], event, EventObject::Phase::AtTarget, true);
        if (!event.stop_immediate)
            invoke(*this, path[0], event, EventObject::Phase::AtTarget, false);
    }
    if (event.bubbles) {
        for (std::size_t i = 1; i < path.size() && !event.stop_propagation; ++i)
            invoke(*this, path[i], event, EventObject::Phase::Bubbling, false);
    }
    current_event = previous_event;
    event.phase = EventObject::Phase::None;
    event.current_target = js::Value::null();
    event.dispatching = false;
    event.stop_propagation = false;
    event.stop_immediate = false;
    return !event.default_prevented;
}

// --- Handler accessors ---------------------------------------------------------------------

void define_event_handlers(Realm::Internals& in, js::Object& target, std::span<std::string_view const> types)
{
    for (std::string_view const type : types) {
        std::string const type_name(type);
        define_getter(in, target, "on" + type_name,
            [type_name](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native {
                Realm::Internals& internals = internals_of(interpreter);
                if (!this_value.is_object() || !internals.handlers_of(this_value.as_object()))
                    return interpreter.throw_type_error("Illegal invocation");
                return handler_value(internals, this_value.as_object(), type_name);
            },
            [type_name](js::Interpreter& interpreter, js::Value const& this_value, Args args) -> Native {
                Realm::Internals& internals = internals_of(interpreter);
                if (!this_value.is_object())
                    return interpreter.throw_type_error("Illegal invocation");
                HandlerMap* map = internals.handlers_of(this_value.as_object());
                if (!map)
                    return interpreter.throw_type_error("Illegal invocation");
                js::Value const value = js::argument(args, 0);
                EventHandler handler;
                handler.from_attribute = false;
                if (value.is_object())
                    handler.function = value; // a callable, or an object the spec keeps and never calls
                (*map)[type_name] = handler;
                return js::Value::undefined();
            });
    }
}

// --- The interfaces -------------------------------------------------------------------------

void install_events(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());

    // EventTarget, constructible: a page makes its own event buses.
    js::Object* event_target = define_interface(in, "EventTarget", nullptr,
        [](js::Interpreter& interp, Args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            return js::Value::object(interp.heap().allocate<EventTargetObject>(internals.prototype("EventTarget")));
        });
    js::define_method(interpreter, *event_target, "addEventListener", 2, add_event_listener);
    js::define_method(interpreter, *event_target, "removeEventListener", 2, remove_event_listener);
    js::define_method(interpreter, *event_target, "dispatchEvent", 1, dispatch_event_native);
    // The window is the global object, made before any of this: it gets
    // the same methods by copy.
    js::define_method(interpreter, *interpreter.global(), "addEventListener", 2, add_event_listener);
    js::define_method(interpreter, *interpreter.global(), "removeEventListener", 2, remove_event_listener);
    js::define_method(interpreter, *interpreter.global(), "dispatchEvent", 1, dispatch_event_native);

    // Event.
    js::Object* event = define_interface(in, "Event", nullptr, event_constructor("Event"), 1);
    event_getter(in, *event, "type", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.type); });
    event_getter(in, *event, "target", [](Realm::Internals&, EventObject& e) { return e.target.is_undefined() ? js::Value::null() : e.target; });
    event_getter(in, *event, "srcElement", [](Realm::Internals&, EventObject& e) { return e.target.is_undefined() ? js::Value::null() : e.target; });
    event_getter(in, *event, "currentTarget", [](Realm::Internals&, EventObject& e) { return e.current_target.is_undefined() ? js::Value::null() : e.current_target; });
    event_getter(in, *event, "eventPhase", [](Realm::Internals&, EventObject& e) { return js::Value::number(static_cast<int>(e.phase)); });
    event_getter(in, *event, "bubbles", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.bubbles); });
    event_getter(in, *event, "cancelable", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.cancelable); });
    event_getter(in, *event, "composed", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.composed); });
    event_getter(in, *event, "defaultPrevented", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.default_prevented); });
    event_getter(in, *event, "isTrusted", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.is_trusted); });
    event_getter(in, *event, "timeStamp", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.time_stamp); });
    define_getter(in, *event, "returnValue",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<EventObject*> const e = this_event(interp, this_value);
            if (!e)
                return std::nullopt;
            return js::Value::boolean(!(*e)->default_prevented);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<EventObject*> const e = this_event(interp, this_value);
            if (!e)
                return std::nullopt;
            if (!js::Interpreter::to_boolean(js::argument(args, 0)) && (*e)->cancelable && !(*e)->in_passive_listener)
                (*e)->default_prevented = true;
            return js::Value::undefined();
        });
    define_getter(in, *event, "cancelBubble",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<EventObject*> const e = this_event(interp, this_value);
            if (!e)
                return std::nullopt;
            return js::Value::boolean((*e)->stop_propagation);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<EventObject*> const e = this_event(interp, this_value);
            if (!e)
                return std::nullopt;
            if (js::Interpreter::to_boolean(js::argument(args, 0)))
                (*e)->stop_propagation = true;
            return js::Value::undefined();
        });
    js::define_method(interpreter, *event, "preventDefault", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        if ((*e)->cancelable && !(*e)->in_passive_listener)
            (*e)->default_prevented = true;
        return js::Value::undefined();
    });
    js::define_method(interpreter, *event, "stopPropagation", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        (*e)->stop_propagation = true;
        return js::Value::undefined();
    });
    js::define_method(interpreter, *event, "stopImmediatePropagation", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        (*e)->stop_propagation = true;
        (*e)->stop_immediate = true;
        return js::Value::undefined();
    });
    js::define_method(interpreter, *event, "composedPath", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        js::ArrayObject* path = interp.new_array();
        interp.root(js::Value::object(path));
        if ((*e)->dispatching && (*e)->current_target.is_object()) {
            js::Object* current = (*e)->current_target.as_object();
            if (NodeWrapper* wrapper = internals.wrapper_of(js::Value::object(current))) {
                for (dom::Node* node = &wrapper->node(); node; node = node->parent())
                    path->push(js::Value::object(internals.wrap(*node)));
                if (&wrapper->node().root() == &internals.document && (*e)->type != "load")
                    path->push(js::Value::object(interp.global()));
            } else {
                path->push(js::Value::object(current));
            }
        }
        return js::Value::object(path);
    });
    js::define_method(interpreter, *event, "initEvent", 3, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        if ((*e)->dispatching)
            return js::Value::undefined();
        std::optional<std::string> type = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!type)
            return std::nullopt;
        (*e)->type = std::move(*type);
        (*e)->bubbles = js::Interpreter::to_boolean(js::argument(args, 1));
        (*e)->cancelable = js::Interpreter::to_boolean(js::argument(args, 2));
        (*e)->initialized = true;
        (*e)->default_prevented = false;
        (*e)->is_trusted = false;
        (*e)->target = js::Value::null();
        return js::Value::undefined();
    });
    for (auto const& [name, value] : { std::pair { "NONE", 0 }, std::pair { "CAPTURING_PHASE", 1 }, std::pair { "AT_TARGET", 2 },
             std::pair { "BUBBLING_PHASE", 3 } }) {
        event->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        std::optional<js::Value> const constructor = event->get(interpreter, interpreter.key("constructor"), js::Value::object(event));
        if (constructor && constructor->is_object())
            constructor->as_object()->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }

    // CustomEvent.
    js::Object* custom_event = define_interface(in, "CustomEvent", event, event_constructor("CustomEvent"), 1);
    event_getter(in, *custom_event, "detail", [](Realm::Internals&, EventObject& e) { return e.detail_value.is_undefined() ? js::Value::null() : e.detail_value; });
    js::define_method(interpreter, *custom_event, "initCustomEvent", 4, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        if ((*e)->dispatching)
            return js::Value::undefined();
        std::optional<std::string> type = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!type)
            return std::nullopt;
        (*e)->type = std::move(*type);
        (*e)->bubbles = js::Interpreter::to_boolean(js::argument(args, 1));
        (*e)->cancelable = js::Interpreter::to_boolean(js::argument(args, 2));
        (*e)->detail_value = js::argument(args, 3);
        (*e)->initialized = true;
        return js::Value::undefined();
    });

    // UIEvent and the events under it.
    js::Object* ui_event = define_interface(in, "UIEvent", event, event_constructor("UIEvent"), 1);
    event_getter(in, *ui_event, "view", [](Realm::Internals& internals, EventObject&) { return js::Value::object(internals.interpreter.global()); });
    event_getter(in, *ui_event, "detail", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.detail); });
    event_getter(in, *ui_event, "which", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.key_code ? e.key_code : e.button + 1); });
    js::define_method(interpreter, *ui_event, "initUIEvent", 5, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        std::optional<std::string> type = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!type)
            return std::nullopt;
        (*e)->type = std::move(*type);
        (*e)->bubbles = js::Interpreter::to_boolean(js::argument(args, 1));
        (*e)->cancelable = js::Interpreter::to_boolean(js::argument(args, 2));
        (*e)->initialized = true;
        return js::Value::undefined();
    });

    js::Object* mouse_event = define_interface(in, "MouseEvent", ui_event, event_constructor("MouseEvent"), 1);
    event_getter(in, *mouse_event, "clientX", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.client_x); });
    event_getter(in, *mouse_event, "clientY", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.client_y); });
    event_getter(in, *mouse_event, "x", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.client_x); });
    event_getter(in, *mouse_event, "y", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.client_y); });
    event_getter(in, *mouse_event, "screenX", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.screen_x); });
    event_getter(in, *mouse_event, "screenY", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.screen_y); });
    event_getter(in, *mouse_event, "pageX", [](Realm::Internals& internals, EventObject& e) {
        int const scroll = internals.hooks.scroll_position ? internals.hooks.scroll_position().first : 0;
        return js::Value::number(e.client_x + scroll);
    });
    event_getter(in, *mouse_event, "pageY", [](Realm::Internals& internals, EventObject& e) {
        int const scroll = internals.hooks.scroll_position ? internals.hooks.scroll_position().second : 0;
        return js::Value::number(e.client_y + scroll);
    });
    event_getter(in, *mouse_event, "offsetX", [](Realm::Internals& internals, EventObject& e) { return js::Value::number(offset_in_target(internals, e).first); });
    event_getter(in, *mouse_event, "offsetY", [](Realm::Internals& internals, EventObject& e) { return js::Value::number(offset_in_target(internals, e).second); });
    event_getter(in, *mouse_event, "movementX", [](Realm::Internals&, EventObject&) { return js::Value::number(0); });
    event_getter(in, *mouse_event, "movementY", [](Realm::Internals&, EventObject&) { return js::Value::number(0); });
    event_getter(in, *mouse_event, "button", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.button); });
    event_getter(in, *mouse_event, "buttons", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.buttons); });
    event_getter(in, *mouse_event, "relatedTarget", [](Realm::Internals&, EventObject& e) { return e.related_target.is_undefined() ? js::Value::null() : e.related_target; });
    for (js::Object* proto : { mouse_event }) {
        event_getter(in, *proto, "ctrlKey", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.ctrl_key); });
        event_getter(in, *proto, "shiftKey", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.shift_key); });
        event_getter(in, *proto, "altKey", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.alt_key); });
        event_getter(in, *proto, "metaKey", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.meta_key); });
    }
    auto const modifier_state = [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        std::optional<std::string> const key = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!key)
            return std::nullopt;
        if (*key == "Control")
            return js::Value::boolean((*e)->ctrl_key);
        if (*key == "Shift")
            return js::Value::boolean((*e)->shift_key);
        if (*key == "Alt")
            return js::Value::boolean((*e)->alt_key);
        if (*key == "Meta")
            return js::Value::boolean((*e)->meta_key);
        return js::Value::boolean(false);
    };
    js::define_method(interpreter, *mouse_event, "getModifierState", 1, modifier_state);
    js::define_method(interpreter, *mouse_event, "initMouseEvent", 15, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        std::optional<std::string> type = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!type)
            return std::nullopt;
        EventObject& ev = **e;
        ev.type = std::move(*type);
        ev.bubbles = js::Interpreter::to_boolean(js::argument(args, 1));
        ev.cancelable = js::Interpreter::to_boolean(js::argument(args, 2));
        auto const number_at = [&](std::size_t index) {
            std::optional<double> const value = interp.to_number(js::argument(args, index));
            return value ? static_cast<int>(*value) : 0;
        };
        ev.detail = number_at(4);
        ev.screen_x = number_at(5);
        ev.screen_y = number_at(6);
        ev.client_x = number_at(7);
        ev.client_y = number_at(8);
        ev.ctrl_key = js::Interpreter::to_boolean(js::argument(args, 9));
        ev.alt_key = js::Interpreter::to_boolean(js::argument(args, 10));
        ev.shift_key = js::Interpreter::to_boolean(js::argument(args, 11));
        ev.meta_key = js::Interpreter::to_boolean(js::argument(args, 12));
        ev.button = number_at(13);
        ev.related_target = js::argument(args, 14);
        ev.initialized = true;
        return js::Value::undefined();
    });

    js::Object* pointer_event = define_interface(in, "PointerEvent", mouse_event, event_constructor("PointerEvent"), 1);
    event_getter(in, *pointer_event, "pointerId", [](Realm::Internals&, EventObject&) { return js::Value::number(1); });
    event_getter(in, *pointer_event, "pointerType", [](Realm::Internals& internals, EventObject&) { return internals.string("mouse"); });
    event_getter(in, *pointer_event, "isPrimary", [](Realm::Internals&, EventObject&) { return js::Value::boolean(true); });
    event_getter(in, *pointer_event, "pressure", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.buttons ? 0.5 : 0); });
    event_getter(in, *pointer_event, "width", [](Realm::Internals&, EventObject&) { return js::Value::number(1); });
    event_getter(in, *pointer_event, "height", [](Realm::Internals&, EventObject&) { return js::Value::number(1); });

    js::Object* wheel_event = define_interface(in, "WheelEvent", mouse_event, event_constructor("WheelEvent"), 1);
    event_getter(in, *wheel_event, "deltaX", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.delta_x); });
    event_getter(in, *wheel_event, "deltaY", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.delta_y); });
    event_getter(in, *wheel_event, "deltaZ", [](Realm::Internals&, EventObject&) { return js::Value::number(0); });
    event_getter(in, *wheel_event, "deltaMode", [](Realm::Internals&, EventObject&) { return js::Value::number(0); });
    for (auto const& [name, value] : { std::pair { "DOM_DELTA_PIXEL", 0 }, std::pair { "DOM_DELTA_LINE", 1 }, std::pair { "DOM_DELTA_PAGE", 2 } })
        wheel_event->put(interpreter.key(name), js::Value::number(value), js::Enumerable);

    js::Object* keyboard_event = define_interface(in, "KeyboardEvent", ui_event, event_constructor("KeyboardEvent"), 1);
    event_getter(in, *keyboard_event, "key", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.key); });
    event_getter(in, *keyboard_event, "code", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.code); });
    event_getter(in, *keyboard_event, "keyCode", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.key_code); });
    event_getter(in, *keyboard_event, "charCode", [](Realm::Internals&, EventObject& e) {
        // The legacy charCode of a printable key on keypress: its code point.
        if (e.type != "keypress" || e.key.empty())
            return js::Value::number(0);
        std::u16string const units = js::utf16_from_utf8(e.key);
        return js::Value::number(units.size() == 1 ? units[0] : 0);
    });
    event_getter(in, *keyboard_event, "location", [](Realm::Internals&, EventObject&) { return js::Value::number(0); });
    event_getter(in, *keyboard_event, "repeat", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.repeat); });
    event_getter(in, *keyboard_event, "isComposing", [](Realm::Internals&, EventObject&) { return js::Value::boolean(false); });
    event_getter(in, *keyboard_event, "ctrlKey", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.ctrl_key); });
    event_getter(in, *keyboard_event, "shiftKey", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.shift_key); });
    event_getter(in, *keyboard_event, "altKey", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.alt_key); });
    event_getter(in, *keyboard_event, "metaKey", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.meta_key); });
    js::define_method(interpreter, *keyboard_event, "getModifierState", 1, modifier_state);
    for (auto const& [name, value] : { std::pair { "DOM_KEY_LOCATION_STANDARD", 0 }, std::pair { "DOM_KEY_LOCATION_LEFT", 1 },
             std::pair { "DOM_KEY_LOCATION_RIGHT", 2 }, std::pair { "DOM_KEY_LOCATION_NUMPAD", 3 } })
        keyboard_event->put(interpreter.key(name), js::Value::number(value), js::Enumerable);

    js::Object* input_event = define_interface(in, "InputEvent", ui_event, event_constructor("InputEvent"), 1);
    event_getter(in, *input_event, "data", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.data); });
    event_getter(in, *input_event, "inputType", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.input_type); });
    event_getter(in, *input_event, "isComposing", [](Realm::Internals&, EventObject&) { return js::Value::boolean(false); });
    event_getter(in, *input_event, "dataTransfer", [](Realm::Internals&, EventObject&) { return js::Value::null(); });

    js::Object* focus_event = define_interface(in, "FocusEvent", ui_event, event_constructor("FocusEvent"), 1);
    event_getter(in, *focus_event, "relatedTarget", [](Realm::Internals&, EventObject& e) { return e.related_target.is_undefined() ? js::Value::null() : e.related_target; });

    js::Object* pop_state_event = define_interface(in, "PopStateEvent", event, event_constructor("PopStateEvent"), 1);
    event_getter(in, *pop_state_event, "state", [](Realm::Internals&, EventObject& e) { return e.detail_value.is_undefined() ? js::Value::null() : e.detail_value; });

    define_interface(in, "TransitionEvent", event, event_constructor("TransitionEvent"), 1);
    define_interface(in, "AnimationEvent", event, event_constructor("AnimationEvent"), 1);
    define_interface(in, "ProgressEvent", event, event_constructor("ProgressEvent"), 1);
    define_interface(in, "HashChangeEvent", event, event_constructor("HashChangeEvent"), 1);
}

}
