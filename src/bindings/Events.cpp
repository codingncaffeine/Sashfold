#include "bindings/Internal.h"
#include "bindings/NodeSupport.h"
#include "js/Runtime.h"

// Events (DOM §2): the dispatch algorithm over the capture, target and
// bubble phases, addEventListener and its options, the on<type> handlers
// (HTML §8.1.8) including the ones compiled from content attributes, and
// the Event interfaces a page constructs.

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

// HTML's GlobalEventHandlers (§8.1.8.2), with what the other specifications
// add to the set — the pointer, animation, transition, selection, scroll
// snap and content-visibility handlers — and the webkit-prefixed animation
// names every engine keeps for the pages that write them.
constexpr std::string_view global_event_handler_names[] = { "abort", "animationcancel", "animationend", "animationiteration",
    "animationstart", "auxclick", "beforeinput", "beforematch", "beforetoggle", "beforexrselect", "blur", "cancel", "canplay",
    "canplaythrough", "change", "click", "close", "command", "contentvisibilityautostatechange", "contextlost", "contextmenu",
    "contextrestored", "copy", "cuechange", "cut", "dblclick", "drag", "dragend", "dragenter", "dragleave", "dragover", "dragstart",
    "drop", "durationchange", "emptied", "ended", "error", "focus", "formdata", "gotpointercapture", "input", "invalid", "keydown",
    "keypress", "keyup", "load", "loadeddata", "loadedmetadata", "loadstart", "lostpointercapture", "mousedown", "mouseenter",
    "mouseleave", "mousemove", "mouseout", "mouseover", "mouseup", "mousewheel", "paste", "pause", "play", "playing", "pointercancel",
    "pointerdown", "pointerenter", "pointerleave", "pointermove", "pointerout", "pointerover", "pointerrawupdate", "pointerup",
    "progress", "ratechange", "reset", "resize", "scroll", "scrollend", "scrollsnapchange", "scrollsnapchanging",
    "securitypolicyviolation", "seeked", "seeking", "select", "selectionchange", "selectstart", "slotchange", "stalled", "submit",
    "suspend", "timeupdate", "toggle", "transitioncancel", "transitionend", "transitionrun", "transitionstart", "volumechange",
    "waiting", "webkitanimationend", "webkitanimationiteration", "webkitanimationstart", "webkittransitionend", "wheel" };

// HTML's WindowEventHandlers (§8.1.8.2): the window's own set, which the
// body and frameset elements forward to it.
constexpr std::string_view window_event_handler_names[] = { "afterprint", "beforeprint", "beforeunload", "hashchange",
    "languagechange", "message", "messageerror", "offline", "online", "pagehide", "pagereveal", "pageshow", "pageswap", "popstate",
    "rejectionhandled", "storage", "unhandledrejection", "unload" };

// The six GlobalEventHandlers a body's attribute gives the window as well.
constexpr std::string_view body_window_handler_names[] = { "blur", "error", "focus", "load", "resize", "scroll" };

// Event types whose body attribute handler belongs to the window (HTML
// §8.1.8.2).
bool is_window_event_type(std::string_view type)
{
    for (std::string_view const candidate : window_event_handler_names) {
        if (candidate == type)
            return true;
    }
    for (std::string_view const candidate : body_window_handler_names) {
        if (candidate == type)
            return true;
    }
    return false;
}

} // namespace

std::span<std::string_view const> global_event_handler_types() { return global_event_handler_names; }
std::span<std::string_view const> window_event_handler_types() { return window_event_handler_names; }

namespace {

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
    if (!element && in.is_window(target) && is_window_event_type(type)) {
        // The body's onload="…" is the window's handler.
        for (dom::Node* child : in.document->children()) {
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
    // A document with no window — one a parser or createHTMLDocument made,
    // one a frame has gone on from — has scripting disabled (HTML §8.1.3.4):
    // a handler attribute of its elements is never compiled, let alone run.
    if (element && !from_body) {
        NodeWrapper const* const wrapper = in.wrapper_of(js::Value::object(target));
        if (wrapper != nullptr && wrapper->realm().internals().document != &element->document())
            element = nullptr;
    }
    // Compiles an attribute's text into the map's entry for this type
    // (§8.1.8.1 "getting the current value of the event handler"). A
    // handler the page's policy refuses, or that does not compile, is
    // remembered with a null function: asked once, not at every dispatch.
    auto const compile = [&](std::string const& source, bool for_window, bool another_body) {
        EventHandler handler;
        handler.from_attribute = true;
        handler.source = source;
        handler.from_another_body = another_body;
        handler.function = js::Value::null();
        if (in.scripts_sandboxed() || in.inline_refused(net::InlineKind::ScriptAttribute, {}, source)) {
            ++in.stats.scripts_refused;
        } else {
            std::u16string const body = js::utf16_from_utf8(source);
            // The window's onerror, which the body's attribute is, takes
            // the error's five parts by name (HTML §8.1.8.1).
            bool const window_onerror = for_window && key == "error";
            std::optional<js::Value> compiled
                = in.interpreter.compile_function(window_onerror ? u"event, source, lineno, colno, error" : u"event", body);
            if (compiled) {
                handler.function = *compiled;
            } else {
                js::Value const thrown = in.interpreter.take_exception();
                in.report_uncaught(thrown, for_window ? "<body on" + key + ">" : "on" + key + " attribute");
            }
        }
        (*map)[key] = handler;
    };
    if (element) {
        dom::Attr const* attribute = element->find_attribute("on" + key);
        auto it = map->find(key);
        if (attribute) {
            // The attribute's text, compiled when it is new, changed, or
            // stored raw by window_handler_attribute_written.
            if (it == map->end()
                || (it->second.from_attribute && (it->second.source != attribute->value || it->second.function.is_undefined())))
                compile(attribute->value, from_body, false);
        } else if (it != map->end() && it->second.from_attribute && !it->second.from_another_body) {
            map->erase(it);
        }
    }
    if (auto const raw = map->find(key);
        raw != map->end() && raw->second.from_attribute && raw->second.from_another_body && raw->second.function.is_undefined()) {
        // The attribute of a body other than the document's, stored raw as
        // it was set: the window's handler all the same (§8.1.8.2).
        std::string const source = raw->second.source;
        compile(source, true, true);
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
    // An ErrorEvent named error at a window or a worker's scope: the handler
    // is OnErrorEventHandler, called with the event's five parts, and true
    // from it is what cancels (HTML §8.1.8.1, special error event handling).
    bool const special = event.is_error_event && event.type == "error" && in.is_window(target);
    std::vector<js::Value> arguments { event_value };
    if (special) {
        arguments = { interpreter.root(in.string(event.message)), interpreter.root(in.string(event.filename)),
            js::Value::number(event.lineno), js::Value::number(event.colno), event.detail_value };
    }
    Realm::Internals::Entry const entry(in);
    js::Outcome const outcome = interpreter.call_outcome(handler, js::Value::object(target), arguments);
    if (!outcome.ok) {
        if (!interpreter.terminated())
            in.report_uncaught(outcome.value, "on" + event.type + " handler");
        return;
    }
    if (special) {
        if (outcome.value.is_boolean() && outcome.value.as_boolean())
            event.default_prevented = true;
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
    // The list may change while listeners run: remember the ids and find
    // each again, skipping any that has been removed since. The ids are
    // taken before the handler runs, so a listener the handler adds waits
    // for the next event, as one added by a listener does (DOM §2.9, the
    // listeners are cloned before any is invoked).
    std::vector<ListenerEntry>* list = in.listeners_of(target);
    std::vector<std::uint64_t> ids;
    if (list != nullptr) {
        for (ListenerEntry const& entry : *list) {
            if (entry.type == event.type && entry.listener.capture == capture)
                ids.push_back(entry.listener.id);
        }
    }
    if (!capture)
        call_handler(in, target, event);
    if (event.stop_immediate || list == nullptr)
        return;
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
            } else {
                // DOM §2.10 "inner invoke": a handleEvent that cannot be
                // called is a TypeError, reported like any listener's.
                (void)interpreter.throw_type_error("The listener's handleEvent is not callable.");
                js::Value const thrown = interpreter.take_exception();
                in.report_uncaught(thrown, event.type + " listener");
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

// The EventTarget a method's `this` names (WebIDL §3.7.6), with the realm
// whose listeners are its: undefined or null is the realm's window, which a
// bare addEventListener(…) reaches; a WindowProxy is the window it stands
// for, which only a script of that window's origin may reach; any other
// object is itself, when it is an EventTarget at all.
struct ThisTarget {
    Realm::Internals* internals;
    js::Object* object;
};

std::optional<ThisTarget> event_target_of(js::Interpreter& interpreter, js::Value const& this_value)
{
    if (this_value.is_nullish())
        return ThisTarget { &internals_of(interpreter), interpreter.global_this() };
    if (WindowProxyObject* const proxy = as_window_proxy(this_value)) {
        if (!is_platform_object_same_origin(interpreter, proxy->record())) {
            throw_security_error(interpreter);
            return std::nullopt;
        }
        return ThisTarget { &proxy->internals(), proxy };
    }
    if (!this_value.is_object() || !internals_of(interpreter).listeners_of(this_value.as_object())) {
        interpreter.throw_type_error("Illegal invocation");
        return std::nullopt;
    }
    return ThisTarget { &internals_of(interpreter), this_value.as_object() };
}

Native add_event_listener(js::Interpreter& interpreter, js::Value const& this_value, Args args)
{
    std::optional<ThisTarget> const target = event_target_of(interpreter, this_value);
    if (!target)
        return std::nullopt;
    Realm::Internals& in = *target->internals;
    std::vector<ListenerEntry>* list = in.listeners_of(target->object);
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
    std::optional<ThisTarget> const target = event_target_of(interpreter, this_value);
    if (!target)
        return std::nullopt;
    Realm::Internals& in = *target->internals;
    std::vector<ListenerEntry>* list = in.listeners_of(target->object);
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
    std::optional<ThisTarget> const target = event_target_of(interpreter, this_value);
    if (!target)
        return std::nullopt;
    js::Value const event_value = js::argument(args, 0);
    EventObject* event = event_value.is_object() ? dynamic_cast<EventObject*>(event_value.as_object()) : nullptr;
    if (!event)
        return interpreter.throw_type_error("parameter 1 is not of type 'Event'");
    if (event->dispatching || !event->initialized)
        return internals_of(interpreter).throw_dom_exception("InvalidStateError", "The event is already being dispatched");
    event->is_trusted = false;
    return js::Value::boolean(target->internals->dispatch(*event, target->object));
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
        if (interface == "ProgressEvent") {
            std::optional<bool> const computable = init_flag(interpreter, init, "lengthComputable", false);
            std::optional<double> const loaded = init_number(interpreter, init, "loaded", 0);
            std::optional<double> const total = init_number(interpreter, init, "total", 0);
            if (!computable || !loaded || !total)
                return std::nullopt;
            event->length_computable = *computable;
            event->loaded = *loaded;
            event->total = *total;
        }
        if (interface == "ErrorEvent") {
            std::optional<std::string> message = init_string(interpreter, init, "message");
            std::optional<std::string> filename = init_string(interpreter, init, "filename");
            std::optional<double> const lineno = init_number(interpreter, init, "lineno", 0);
            std::optional<double> const colno = init_number(interpreter, init, "colno", 0);
            // The error is any value, undefined when the dictionary has none.
            std::optional<js::Value> const error = init.is_object() ? interpreter.get(init, "error") : js::Value::undefined();
            if (!message || !filename || !lineno || !colno || !error)
                return std::nullopt;
            event->message = std::move(*message);
            event->filename = std::move(*filename);
            event->lineno = to_unsigned_long(*lineno);
            event->colno = to_unsigned_long(*colno);
            event->detail_value = *error;
        }
        if (interface == "AnimationEvent" || interface == "TransitionEvent") {
            // The dictionary's members in their order: the name (or the
            // property), the seconds elapsed, the pseudo-element.
            std::optional<js::Value> const animation = init_value(interpreter, init, "animation");
            if (!animation)
                return std::nullopt;
            event->detail_value = *animation;
            std::optional<std::string> name;
            std::optional<double> elapsed;
            if (interface == "AnimationEvent") {
                if (!(name = init_string(interpreter, init, "animationName")))
                    return std::nullopt;
                if (!(elapsed = init_number(interpreter, init, "elapsedTime", 0)))
                    return std::nullopt;
            } else {
                if (!(elapsed = init_number(interpreter, init, "elapsedTime", 0)))
                    return std::nullopt;
                if (!(name = init_string(interpreter, init, "propertyName")))
                    return std::nullopt;
            }
            std::optional<std::string> pseudo = init_string(interpreter, init, "pseudoElement");
            if (!pseudo)
                return std::nullopt;
            event->animation_name = std::move(*name);
            event->elapsed_time = *elapsed;
            event->pseudo_element = std::move(*pseudo);
        }
        if (interface == "ToggleEvent") {
            std::optional<std::string> old_state = init_string(interpreter, init, "oldState");
            std::optional<std::string> new_state = init_string(interpreter, init, "newState");
            if (!old_state || !new_state)
                return std::nullopt;
            event->old_state = std::move(*old_state);
            event->new_state = std::move(*new_state);
        }
        if (interface == "MessageEvent") {
            std::optional<js::Value> const data = init_value(interpreter, init, "data");
            std::optional<std::string> origin = init_string(interpreter, init, "origin");
            std::optional<js::Value> const source = init_value(interpreter, init, "source");
            std::optional<js::Value> const ports = init_value(interpreter, init, "ports");
            if (!data || !origin || !source || !ports)
                return std::nullopt;
            event->detail_value = data->is_undefined() ? js::Value::null() : *data;
            event->origin = std::move(*origin);
            event->source_value = source->is_nullish() ? js::Value::null() : *source;
            event->ports = ports->is_nullish() ? js::Value::undefined() : *ports;
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
Native read_event(js::Interpreter& interpreter, js::Value const& this_value, Read const& read)
{
    std::optional<EventObject*> const event = this_event(interpreter, this_value);
    if (!event)
        return std::nullopt;
    return read(internals_of(interpreter), **event);
}

template<typename Read>
void event_getter(Realm::Internals& in, js::Object& prototype, std::string_view name, Read read)
{
    if constexpr (Stateless<Read>) {
        define_getter(in, prototype, name,
            [](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native { return read_event(interpreter, this_value, Read {}); });
    } else {
        define_getter(in, prototype, name,
            [read](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native { return read_event(interpreter, this_value, read); });
    }
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
    std::pair<int, int> const scroll = in.hooks.scroll_position ? in.hooks.scroll_position(*in.document) : std::pair<int, int> { 0, 0 };
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
    event->is_error_event = interface == "ErrorEvent";
    return event;
}

std::vector<ListenerEntry>* Realm::Internals::listeners_of(js::Object* target)
{
    if (is_window(target))
        return &window_listeners;
    if (auto* event_target = dynamic_cast<EventTargetObject*>(target))
        return &event_target->listeners;
    return nullptr;
}

HandlerMap* Realm::Internals::handlers_of(js::Object* target)
{
    if (is_window(target))
        return &window_handlers;
    if (auto* event_target = dynamic_cast<EventTargetObject*>(target))
        return &event_target->handlers;
    return nullptr;
}

bool Realm::Internals::dispatch(EventObject& event, js::Object* target)
{
    // §2.9 "dispatch". The path runs from the target up — a node's parent,
    // the slot it is assigned to, a shadow root's host when the event is
    // composed — to the document and the window; every wrapper on it is
    // rooted while listeners run.
    js::Interpreter::Roots const roots(interpreter);
    interpreter.root(js::Value::object(&event));
    interpreter.root(js::Value::object(target));
    ++stats.events_dispatched;
    event.dispatching = true;
    event.target = js::Value::object(target);
    event.stop_propagation = false;
    event.stop_immediate = false;

    auto const node_of = [this](js::Object* object) -> dom::Node* {
        NodeWrapper* const wrapper = object != nullptr ? wrapper_of(js::Value::object(object)) : nullptr;
        return wrapper != nullptr ? &wrapper->node() : nullptr;
    };
    auto const in_shadow_tree = [](dom::Node const* node) { return node != nullptr && node->root().is_shadow_root(); };
    auto const holds = [](dom::Node const& ancestor, dom::Node const& node) {
        for (dom::Node const* at = &node; at != nullptr; at = at->parent_or_host()) {
            if (at == &ancestor)
                return true;
        }
        return false;
    };
    // §2.5 "retarget" A against B: A as B may see it — out of every shadow
    // tree B is not itself inside.
    auto const retarget = [&](js::Value const& a, dom::Node const* b) -> js::Value {
        dom::Node* node = a.is_object() ? node_of(a.as_object()) : nullptr;
        if (node == nullptr)
            return a;
        dom::Node* const given = node;
        for (;;) {
            dom::Node& root = node->root();
            if (!root.is_shadow_root() || (b != nullptr && holds(root, *b)))
                break;
            node = &static_cast<dom::ShadowRoot&>(root).host();
        }
        return node == given ? a : interpreter.root(js::Value::object(wrap(*node)));
    };

    std::vector<EventObject::PathEntry> path;
    auto const append = [&](js::Object* invocation_target, js::Object* shadow_adjusted_target, js::Value const& related, bool slot_in_closed_tree) {
        EventObject::PathEntry entry;
        entry.invocation_target = invocation_target;
        entry.shadow_adjusted_target = shadow_adjusted_target;
        entry.related_target = related;
        dom::Node* const node = node_of(invocation_target);
        entry.in_shadow_tree = in_shadow_tree(node);
        entry.root_of_closed_tree
            = node != nullptr && node->is_shadow_root() && static_cast<dom::ShadowRoot*>(node)->mode == dom::ShadowRoot::Mode::Closed;
        entry.slot_in_closed_tree = slot_in_closed_tree;
        path.push_back(entry);
    };

    js::Value const given_related = event.related_target;
    dom::Node* const target_node = node_of(target);
    js::Value related = retarget(given_related, target_node);
    bool clear_targets = false;
    // An event whose related target is its target, once retargeted, goes
    // nowhere: the pointer moved within one shadow tree, as its host sees it.
    bool const dispatched = !(related.is_object() && related.as_object() == target)
        || (given_related.is_object() && given_related.as_object() == target);
    if (dispatched) {
        append(target, target, related, false);
        if (target_node != nullptr) {
            // §2.9 "get the parent": a node's assigned slot or its parent,
            // a shadow root's host unless the event is not composed and
            // began in that root's tree, the document's window except for
            // load. Answered as the node, when it is one, and its object.
            dom::Node const* const first_root = &target_node->root();
            auto const parent_of = [&](dom::Node& node) -> std::pair<dom::Node*, js::Object*> {
                if (dom::Element* const slot = node.assigned_slot())
                    return { slot, wrap(*slot) };
                if (node.type() == dom::NodeType::Document)
                    return { nullptr, &node == document && event.type != "load" ? window_proxy() : nullptr };
                if (node.is_shadow_root()) {
                    if (!event.composed && &node == first_root)
                        return { nullptr, nullptr };
                    dom::Element& host = static_cast<dom::ShadowRoot&>(node).host();
                    return { &host, wrap(host) };
                }
                dom::Node* const parent = node.parent();
                return { parent, parent != nullptr ? wrap(*parent) : nullptr };
            };
            dom::Node* adjusted = target_node;
            dom::Node* slottable = target_node->assigned_slot() != nullptr ? target_node : nullptr;
            bool slot_in_closed_tree = false;
            std::pair<dom::Node*, js::Object*> parent = parent_of(*target_node);
            while (parent.second != nullptr) {
                interpreter.root(js::Value::object(parent.second));
                if (slottable != nullptr) {
                    slottable = nullptr;
                    if (parent.first != nullptr) {
                        dom::Node const& root = parent.first->root();
                        slot_in_closed_tree = root.is_shadow_root() && static_cast<dom::ShadowRoot const&>(root).mode == dom::ShadowRoot::Mode::Closed;
                    }
                }
                if (parent.first != nullptr && parent.first->assigned_slot() != nullptr)
                    slottable = parent.first;
                related = retarget(given_related, parent.first);
                if (parent.first == nullptr || holds(adjusted->root(), *parent.first)) {
                    append(parent.second, nullptr, related, slot_in_closed_tree);
                } else if (related.is_object() && related.as_object() == parent.second) {
                    break;
                } else {
                    adjusted = parent.first;
                    append(parent.second, parent.second, related, slot_in_closed_tree);
                }
                parent = parent.first != nullptr ? parent_of(*parent.first) : std::pair<dom::Node*, js::Object*> { nullptr, nullptr };
                slot_in_closed_tree = false;
            }
            for (std::size_t i = path.size(); i-- > 0;) {
                if (path[i].shadow_adjusted_target == nullptr)
                    continue;
                clear_targets = in_shadow_tree(node_of(path[i].shadow_adjusted_target))
                    || (path[i].related_target.is_object() && in_shadow_tree(node_of(path[i].related_target.as_object())));
                break;
            }
        } else if (auto* event_target = dynamic_cast<EventTargetObject*>(target)) {
            // A target that is no node names its parent itself.
            for (js::Object* parent = event_target->event_parent(); parent != nullptr && path.size() < 64;) {
                interpreter.root(js::Value::object(parent));
                append(parent, nullptr, related, false);
                auto* const next = dynamic_cast<EventTargetObject*>(parent);
                parent = next != nullptr ? next->event_parent() : nullptr;
            }
        }
    }
    event.path = path;
    js::Value const previous_event = current_event;
    // A listener at a place on the path sees the target and the related
    // target as that place may: the nearest shadow-adjusted target at or
    // below it. window.event is not told to a listener inside a shadow tree.
    auto const arrive = [&](std::size_t at) {
        for (std::size_t i = at + 1; i-- > 0;) {
            if (path[i].shadow_adjusted_target != nullptr) {
                event.target = js::Value::object(path[i].shadow_adjusted_target);
                break;
            }
        }
        event.related_target = path[at].related_target;
        current_event = path[at].in_shadow_tree ? js::Value::undefined() : js::Value::object(&event);
    };
    for (std::size_t i = path.size(); i-- > 0 && !event.stop_propagation;) {
        arrive(i);
        invoke(*this, path[i].invocation_target, event,
            path[i].shadow_adjusted_target != nullptr ? EventObject::Phase::AtTarget : EventObject::Phase::Capturing, true);
    }
    for (std::size_t i = 0; i < path.size() && !event.stop_propagation; ++i) {
        bool const at_target = path[i].shadow_adjusted_target != nullptr;
        if (!at_target && !event.bubbles)
            continue;
        arrive(i);
        invoke(*this, path[i].invocation_target, event, at_target ? EventObject::Phase::AtTarget : EventObject::Phase::Bubbling, false);
    }
    current_event = previous_event;
    event.phase = EventObject::Phase::None;
    event.current_target = js::Value::null();
    event.path.clear();
    event.dispatching = false;
    event.stop_propagation = false;
    event.stop_immediate = false;
    // What began inside a shadow tree is not left on the event for whoever
    // holds it afterwards (§2.9 step 9, "clear targets").
    event.related_target = given_related;
    if (clear_targets) {
        event.target = js::Value::null();
        event.related_target = js::Value::null();
    }
    return !event.default_prevented;
}

// --- Handler accessors ---------------------------------------------------------------------

void details_open_written(Realm::Internals& in, dom::Element& element, bool was_open)
{
    bool const is_open = element.find_attribute("open") != nullptr;
    if (was_open == is_open)
        return; // the value changed; the state did not
    js::Object* const wrapper = in.wrap(element);
    std::string const new_state = is_open ? "open" : "closed";
    if (auto const queued = in.toggle_tasks.find(wrapper); queued != in.toggle_tasks.end()) {
        queued->second = new_state;
        return;
    }
    in.toggle_tasks.emplace(wrapper, new_state);
    auto held = std::make_shared<js::Persistent>(in.interpreter.heap(), js::Value::object(wrapper));
    std::string const old_state = was_open ? "open" : "closed";
    in.post_task([&in, held, old_state] {
        js::Object* const target = held->value().as_object();
        auto const queued = in.toggle_tasks.find(target);
        if (queued == in.toggle_tasks.end())
            return;
        std::string const new_state_now = queued->second;
        in.toggle_tasks.erase(queued);
        Realm::Internals::Entry const entry(in);
        js::Interpreter::Roots const roots(in.interpreter);
        EventObject* event = in.new_event("ToggleEvent", "toggle", false, false);
        in.interpreter.root(js::Value::object(event));
        event->is_trusted = true;
        event->old_state = old_state;
        event->new_state = new_state_now;
        in.dispatch(*event, target);
    });
}

bool notify_rejection(Realm::Internals& in, js::PromiseObject& promise, std::string_view type)
{
    js::Interpreter& interp = in.interpreter;
    Realm::Internals::Entry const entry(in);
    js::Interpreter::Roots const roots(interp);
    interp.root(js::Value::object(&promise));
    // unhandledrejection is cancelable — canceled, nothing is printed;
    // rejectionhandled is not (HTML §8.1.7.3).
    EventObject* event = in.new_event("PromiseRejectionEvent", type, false, type == "unhandledrejection");
    interp.root(js::Value::object(event));
    event->is_trusted = true;
    event->promise_value = js::Value::object(&promise);
    event->detail_value = promise.result();
    return !in.dispatch(*event, in.window_proxy());
}

js::Value event_handler_of(Realm::Internals& in, js::Object* target, std::string_view type)
{
    return handler_value(in, target, type);
}

// A body's or frameset's on<type> content attribute for a window event is
// the window's handler (HTML §8.1.8.2, "determining the target of an event
// handler"): set, it is stored raw in the window's map and compiled when
// first read; removed, it goes. (The document's own body is read lazily by
// handler_value as well, so a parser-set attribute needs no hook.)
void window_handler_attribute_written(Realm::Internals& in, dom::Element& element, std::string_view local_name)
{
    if (!local_name.starts_with("on") || &element.document() != in.document)
        return;
    if (!element.is_html("body") && !element.is_html("frameset"))
        return;
    std::string_view const type = local_name.substr(2);
    if (!is_window_event_type(type))
        return;
    std::string const key(type);
    if (dom::Attr const* const attribute = element.find_attribute(local_name)) {
        EventHandler handler;
        handler.from_attribute = true;
        handler.source = attribute->value;
        handler.from_another_body = body_element(*in.document) != &element;
        in.window_handlers[key] = handler;
    } else if (auto const it = in.window_handlers.find(key); it != in.window_handlers.end() && it->second.from_attribute) {
        in.window_handlers.erase(it);
    }
}

namespace {

// The event type an on<type> accessor serves: one getter and one setter are
// every handler property's, each called as the function made for its
// property, whose key says which.
std::string handler_type_called(js::Interpreter& interpreter)
{
    js::NativeFunction const* const accessor = interpreter.active_native();
    std::string const name = accessor != nullptr ? member_name(*accessor) : std::string();
    return name.size() > 2 ? name.substr(2) : std::string();
}

Native event_handler_getter(js::Interpreter& interpreter, js::Value const& this_value, Args)
{
    js::NativeFunction const* const accessor = interpreter.active_native();
    std::string const type = handler_type_called(interpreter);
    Realm::Internals& internals = internals_of(interpreter);
    if (!this_value.is_object() || !internals.handlers_of(this_value.as_object())) {
        bool const lenient = accessor != nullptr && accessor->spec() != nullptr && (accessor->spec()->flags & member_flag::lenient_this) != 0;
        return lenient ? Native(js::Value::undefined()) : interpreter.throw_type_error("Illegal invocation");
    }
    return handler_value(internals, this_value.as_object(), type);
}

Native event_handler_setter(js::Interpreter& interpreter, js::Value const& this_value, Args args)
{
    std::string const type = handler_type_called(interpreter);
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
    (*map)[type] = handler;
    return js::Value::undefined();
}

}

void define_event_handlers(Realm::Internals& in, js::Object& target, std::span<std::string_view const> types)
{
    std::string name;
    for (std::string_view const type : types) {
        // onmouseenter and onmouseleave are [LegacyLenientThis] (HTML
        // §8.1.8.2.1), and Chromium treats onreadystatechange so too: read
        // on the wrong object they are undefined, written they do nothing.
        MemberKind const kind = type == "mouseenter" || type == "mouseleave" || type == "readystatechange" ? MemberKind::LenientThis : MemberKind::Plain;
        name.assign("on");
        name.append(type);
        define_plain_attribute(in.interpreter, target, name, event_handler_getter, event_handler_setter, kind);
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
    define_operation(interpreter, *event_target, "addEventListener", 2, add_event_listener);
    define_operation(interpreter, *event_target, "removeEventListener", 2, remove_event_listener);
    define_operation(interpreter, *event_target, "dispatchEvent", 1, dispatch_event_native);
    // The window reaches these through its prototype chain; a bare
    // addEventListener(…) calls them with an undefined `this`, which is the
    // realm's window (see event_target_of).

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
    define_operation(interpreter, *event, "preventDefault", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        if ((*e)->cancelable && !(*e)->in_passive_listener)
            (*e)->default_prevented = true;
        return js::Value::undefined();
    });
    define_operation(interpreter, *event, "stopPropagation", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        (*e)->stop_propagation = true;
        return js::Value::undefined();
    });
    define_operation(interpreter, *event, "stopImmediatePropagation", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        (*e)->stop_propagation = true;
        (*e)->stop_immediate = true;
        return js::Value::undefined();
    });
    define_operation(interpreter, *event, "composedPath", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<EventObject*> const e = this_event(interp, this_value);
        if (!e)
            return std::nullopt;
        // The event's path as the current target may see it (DOM §2.9
        // composedPath()): everything on it but what lies inside a closed
        // shadow tree the current target is not itself in. A listener on
        // an ancestor learns from its first entry where the event began.
        js::Interpreter::Roots const roots(interp);
        js::ArrayObject* composed = interp.new_array();
        interp.root(js::Value::object(composed));
        std::vector<EventObject::PathEntry> const& path = (*e)->path;
        if (path.empty() || !(*e)->current_target.is_object())
            return js::Value::object(composed);
        js::Object* const current = (*e)->current_target.as_object();
        std::size_t current_index = 0;
        int current_level = 0;
        for (std::size_t i = path.size(); i-- > 0;) {
            if (path[i].root_of_closed_tree)
                ++current_level;
            if (path[i].invocation_target == current) {
                current_index = i;
                break;
            }
            if (path[i].slot_in_closed_tree)
                --current_level;
        }
        std::vector<js::Object*> before; // nearest to the current target first
        int level = current_level;
        int max_level = current_level;
        for (std::size_t i = current_index; i-- > 0;) {
            if (path[i].root_of_closed_tree)
                ++level;
            if (level <= max_level)
                before.push_back(path[i].invocation_target);
            if (path[i].slot_in_closed_tree) {
                --level;
                max_level = std::min(max_level, level);
            }
        }
        for (std::size_t i = before.size(); i-- > 0;)
            composed->push(js::Value::object(before[i]));
        composed->push(js::Value::object(current));
        level = current_level;
        max_level = current_level;
        for (std::size_t i = current_index + 1; i < path.size(); ++i) {
            if (path[i].slot_in_closed_tree)
                ++level;
            if (level <= max_level)
                composed->push(js::Value::object(path[i].invocation_target));
            if (path[i].root_of_closed_tree) {
                --level;
                max_level = std::min(max_level, level);
            }
        }
        return js::Value::object(composed);
    });
    define_operation(interpreter, *event, "initEvent", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
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
    define_operation(interpreter, *custom_event, "initCustomEvent", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
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
    event_getter(in, *ui_event, "view", [](Realm::Internals& internals, EventObject&) { return js::Value::object(internals.window_proxy()); });
    event_getter(in, *ui_event, "detail", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.detail); });
    event_getter(in, *ui_event, "which", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.key_code ? e.key_code : e.button + 1); });
    define_operation(interpreter, *ui_event, "initUIEvent", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
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
        int const scroll = internals.hooks.scroll_position ? internals.hooks.scroll_position(*internals.document).first : 0;
        return js::Value::number(e.client_x + scroll);
    });
    event_getter(in, *mouse_event, "pageY", [](Realm::Internals& internals, EventObject& e) {
        int const scroll = internals.hooks.scroll_position ? internals.hooks.scroll_position(*internals.document).second : 0;
        return js::Value::number(e.client_y + scroll);
    });
    event_getter(in, *mouse_event, "offsetX", [](Realm::Internals& internals, EventObject& e) { return js::Value::number(offset_in_target(internals, e).first); });
    event_getter(in, *mouse_event, "offsetY", [](Realm::Internals& internals, EventObject& e) { return js::Value::number(offset_in_target(internals, e).second); });
    event_getter(in, *mouse_event, "movementX", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.movement_x); });
    event_getter(in, *mouse_event, "movementY", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.movement_y); });
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
    define_operation(interpreter, *mouse_event, "getModifierState", 1, modifier_state);
    define_operation(interpreter, *mouse_event, "initMouseEvent", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
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
    define_operation(interpreter, *keyboard_event, "getModifierState", 1, modifier_state);
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

    // PromiseRejectionEvent (HTML §8.1.7.4): the promise is a required
    // member of its init, and any value there is resolved to one.
    js::Object* rejection_event = define_interface(
        in, "PromiseRejectionEvent", event,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            if (args.size() < 2)
                return too_few_arguments(interp, "PromiseRejectionEvent", "PromiseRejectionEvent", 2, args.size());
            std::optional<std::string> const type = internals.to_utf8(args[0]);
            if (!type)
                return std::nullopt;
            js::Value const init = args[1];
            if (!init.is_object())
                return interp.throw_type_error("Failed to construct 'PromiseRejectionEvent': parameter 2 is not of type 'PromiseRejectionEventInit'.");
            std::optional<bool> const bubbles = init_flag(interp, init, "bubbles", false);
            std::optional<bool> const cancelable = init_flag(interp, init, "cancelable", false);
            std::optional<bool> const composed = init_flag(interp, init, "composed", false);
            if (!bubbles || !cancelable || !composed)
                return std::nullopt;
            js::Interpreter::Roots const roots(interp);
            interp.root(init);
            std::optional<js::Value> const given = interp.get(init, "promise");
            if (!given)
                return std::nullopt;
            if (given->is_undefined())
                return interp.throw_type_error("Failed to construct 'PromiseRejectionEvent': Failed to read the 'promise' property from 'PromiseRejectionEventInit': Required member is undefined.");
            interp.root(*given);
            std::optional<js::Value> const promise = js::promise_resolve(interp, js::Value::object(interp.intrinsics().promise_constructor), *given);
            if (!promise)
                return std::nullopt;
            interp.root(*promise);
            std::optional<js::Value> const reason = interp.get(init, "reason");
            if (!reason)
                return std::nullopt;
            interp.root(*reason);
            EventObject* made = internals.new_event("PromiseRejectionEvent", *type, *bubbles, *cancelable);
            made->composed = *composed;
            made->promise_value = *promise;
            made->detail_value = *reason;
            return js::Value::object(made);
        },
        2);
    event_getter(in, *rejection_event, "promise", [](Realm::Internals&, EventObject& e) { return e.promise_value; });
    event_getter(in, *rejection_event, "reason", [](Realm::Internals&, EventObject& e) { return e.detail_value; });

    // ToggleEvent (HTML §2.6.6): a details element opened or closed.
    js::Object* toggle_event = define_interface(in, "ToggleEvent", event, event_constructor("ToggleEvent"), 1);
    event_getter(in, *toggle_event, "oldState", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.old_state); });
    event_getter(in, *toggle_event, "newState", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.new_state); });

    // css-transitions-1 §6.1 and css-animations-1 §4.1: what a transition or a
    // CSS animation says of itself; the name is the property or the animation.
    js::Object* transition_event = define_interface(in, "TransitionEvent", event, event_constructor("TransitionEvent"), 1);
    event_getter(in, *transition_event, "animation", [](Realm::Internals&, EventObject& e) { return e.detail_value.is_undefined() ? js::Value::null() : e.detail_value; });
    event_getter(in, *transition_event, "propertyName", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.animation_name); });
    event_getter(in, *transition_event, "elapsedTime", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.elapsed_time); });
    event_getter(in, *transition_event, "pseudoElement", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.pseudo_element); });
    js::Object* animation_event = define_interface(in, "AnimationEvent", event, event_constructor("AnimationEvent"), 1);
    event_getter(in, *animation_event, "animation", [](Realm::Internals&, EventObject& e) { return e.detail_value.is_undefined() ? js::Value::null() : e.detail_value; });
    event_getter(in, *animation_event, "animationName", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.animation_name); });
    event_getter(in, *animation_event, "elapsedTime", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.elapsed_time); });
    event_getter(in, *animation_event, "pseudoElement", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.pseudo_element); });
    js::Object* progress_event = define_interface(in, "ProgressEvent", event, event_constructor("ProgressEvent"), 1);
    event_getter(in, *progress_event, "lengthComputable", [](Realm::Internals&, EventObject& e) { return js::Value::boolean(e.length_computable); });
    event_getter(in, *progress_event, "loaded", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.loaded); });
    event_getter(in, *progress_event, "total", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.total); });
    define_interface(in, "HashChangeEvent", event, event_constructor("HashChangeEvent"), 1);

    // ErrorEvent (HTML §8.1.4.3): an uncaught exception as its global is told
    // of it, and as a Worker is told of its worker's.
    js::Object* error_event = define_interface(in, "ErrorEvent", event, event_constructor("ErrorEvent"), 1);
    event_getter(in, *error_event, "message", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.message); });
    event_getter(in, *error_event, "filename", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.filename); });
    event_getter(in, *error_event, "lineno", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.lineno); });
    event_getter(in, *error_event, "colno", [](Realm::Internals&, EventObject& e) { return js::Value::number(e.colno); });
    event_getter(in, *error_event, "error", [](Realm::Internals&, EventObject& e) { return e.detail_value; });

    // MessageEvent (HTML §9.4.1): what a MessagePort or window.postMessage delivers.
    js::Object* message_event = define_interface(in, "MessageEvent", event, event_constructor("MessageEvent"), 1);
    event_getter(in, *message_event, "data", [](Realm::Internals&, EventObject& e) { return e.detail_value.is_undefined() ? js::Value::null() : e.detail_value; });
    event_getter(in, *message_event, "origin", [](Realm::Internals& internals, EventObject& e) { return internals.string(e.origin); });
    event_getter(in, *message_event, "lastEventId", [](Realm::Internals& internals, EventObject&) { return internals.string(""); });
    event_getter(in, *message_event, "source", [](Realm::Internals&, EventObject& e) { return e.source_value.is_undefined() ? js::Value::null() : e.source_value; });
    event_getter(in, *message_event, "ports", [](Realm::Internals& internals, EventObject& e) {
        return e.ports.is_nullish() ? js::Value::object(internals.interpreter.new_array()) : e.ports;
    });
}

}
