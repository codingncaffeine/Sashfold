#include "bindings/Internal.h"

// The Fullscreen API (WHATWG Fullscreen): an element asks to be shown over
// the whole screen, the host — the shell — agrees or not (it puts its
// chrome away and asks the window for the whole output), and the document
// names the element until the page, the reader (Esc) or the element's
// removal ends it. One element at a time: a second request takes the place
// of the first, as the top layer's last entry does. Not here yet: a frame's
// element (refused, as a frame without allowfullscreen is), ::backdrop, and
// the top layer's escape from its ancestors' clips — the UA sheet's
// :fullscreen rule places the element over the viewport.

namespace sashfold::bindings {

namespace {

dom::Element* element_of(js::Object* wrapper)
{
    if (wrapper == nullptr)
        return nullptr;
    dom::Node& node = static_cast<NodeWrapper*>(wrapper)->node();
    return node.is_element() ? &static_cast<dom::Element&>(node) : nullptr;
}

void fire(Realm::Internals& in, js::Object* target, std::string_view type)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(target));
    EventObject* event = in.new_event("Event", type, true, false);
    in.interpreter.root(js::Value::object(event));
    event->is_trusted = true;
    in.dispatch(*event, target);
}

// What a queued fullscreenchange keeps alive until its task runs: the
// element's wrapper, and the resolve function of the page's promise when
// there is one. One allocation, handed whole to the task.
struct Settle {
    Settle(js::Heap& heap, js::Object* wrapper)
        : target(heap, js::Value::object(wrapper))
    {
    }
    js::Persistent target;
    std::optional<js::Persistent> resolve;
};

// In a task of its own: the event at the element (bubbling to the
// document), then the promise the page was given settled.
void queue_event(Realm::Internals& in, js::Object* target, std::string_view type, std::optional<js::PromiseCapability> const& capability)
{
    auto settle = std::make_shared<Settle>(in.interpreter.heap(), target);
    if (capability)
        settle->resolve.emplace(in.interpreter.heap(), capability->resolve);
    in.post_task([&in, settle = std::move(settle), name = std::string(type)] {
        Realm::Internals::Entry const entry(in);
        fire(in, settle->target.value().as_object(), name);
        if (settle->resolve)
            static_cast<void>(in.interpreter.call(settle->resolve->value(), js::Value::undefined(), {}));
    });
}

}

Native request_fullscreen(Realm::Internals& in, dom::Element& element)
{
    js::Interpreter& interp = in.interpreter;
    js::Object* const wrapper = in.wrap(element);
    // Asked for by an element of this window's document, in the document,
    // while the reader has just acted on the page, and granted by the shell.
    bool const allowed = element.is_connected() && &element.document() == in.document && in.parent_realm == nullptr
        && (!in.hooks.user_activation || in.hooks.user_activation()) && in.hooks.request_fullscreen
        && in.hooks.request_fullscreen(true);
    if (!allowed) {
        auto held = std::make_shared<js::Persistent>(interp.heap(), js::Value::object(wrapper));
        in.post_task([&in, held] {
            Realm::Internals::Entry const entry(in);
            fire(in, held->value().as_object(), "fullscreenerror");
        });
        interp.throw_type_error("Fullscreen request denied");
        return rejected_promise(interp, interp.take_exception());
    }
    std::optional<js::PromiseCapability> const capability = js::new_promise_capability(interp, js::Value::object(interp.intrinsics().promise_constructor));
    if (!capability)
        return std::nullopt;
    in.fullscreen_wrapper = wrapper;
    in.mutations++; // :fullscreen matches another element
    queue_event(in, wrapper, "fullscreenchange", capability);
    return capability->promise;
}

void exit_fullscreen(Realm::Internals& in, std::optional<js::PromiseCapability> const& capability)
{
    js::Object* const wrapper = in.fullscreen_wrapper;
    if (wrapper == nullptr)
        return;
    in.fullscreen_wrapper = nullptr;
    in.mutations++;
    if (in.hooks.request_fullscreen)
        in.hooks.request_fullscreen(false);
    // At the element while it is in the document; at the document when the
    // element's leaving it is what ended full screen.
    dom::Element* const element = element_of(wrapper);
    js::Object* const target = element && element->is_connected() ? wrapper : in.wrap(*in.document);
    queue_event(in, target, "fullscreenchange", capability);
}

Native exit_fullscreen_promise(Realm::Internals& in)
{
    js::Interpreter& interp = in.interpreter;
    if (in.fullscreen_wrapper == nullptr) {
        interp.throw_type_error("Not in full screen");
        return rejected_promise(interp, interp.take_exception());
    }
    std::optional<js::PromiseCapability> const capability = js::new_promise_capability(interp, js::Value::object(interp.intrinsics().promise_constructor));
    if (!capability)
        return std::nullopt;
    exit_fullscreen(in, capability);
    return capability->promise;
}

js::Value fullscreen_element(Realm::Internals& in)
{
    dom::Element* const element = element_of(in.fullscreen_wrapper);
    return element && element->is_connected() ? js::Value::object(in.fullscreen_wrapper) : js::Value::null();
}

bool fullscreen_enabled(Realm::Internals const& in) { return in.parent_realm == nullptr && static_cast<bool>(in.hooks.request_fullscreen); }

void check_fullscreen_element(Realm::Internals& in)
{
    dom::Element* const element = element_of(in.fullscreen_wrapper);
    if (in.fullscreen_wrapper != nullptr && (element == nullptr || !element->is_connected()))
        exit_fullscreen(in, std::nullopt);
}

void trace_fullscreen(Realm::Internals const& in, js::Tracer& tracer)
{
    if (in.fullscreen_wrapper)
        tracer.visit(in.fullscreen_wrapper);
}

}
