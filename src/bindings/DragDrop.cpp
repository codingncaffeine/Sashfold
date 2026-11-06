#include "bindings/NodeSupport.h"

// Drag and drop, the page's side (HTML §6.11.5 "drag-and-drop processing
// model"): one drag at a time, begun by dragstart at what is dragged, its
// data in one store that every event of the drag carries — writable in
// dragstart, its formats seen but not its data while the pointer moves
// (protected mode), read in drop. The shell decides where the pointer is
// and what a drop does by default; the page hears each step and may take
// it as its own.

#include "dom/Dom.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

// The dropEffect dragenter and dragover begin with (HTML §6.11.5 table):
// from effectAllowed, and when the page set none, from what is dragged.
std::string initial_drop_effect(std::string const& allowed, std::string const& kind)
{
    if (allowed == "none")
        return "none";
    if (allowed == "copy" || allowed == "copyLink" || allowed == "copyMove" || allowed == "all")
        return "copy";
    if (allowed == "link" || allowed == "linkMove")
        return "link";
    if (allowed == "move")
        return "move";
    // "uninitialized": a link links, anything else copies.
    return kind == "link" ? "link" : "copy";
}

js::Object* store_of(Realm::Internals const& in)
{
    if (!in.drag || !in.drag->store || !in.drag->store->value().is_object())
        return nullptr;
    return in.drag->store->value().as_object();
}

bool fire(Realm::Internals& in, dom::Node& target, std::string_view type, MouseInit const& init, js::Object& store, bool cancelable)
{
    EventObject* const event = in.new_event("DragEvent", type, true, cancelable);
    in.interpreter.root(js::Value::object(event));
    event->is_trusted = true;
    event->composed = true;
    event->client_x = init.client_x;
    event->client_y = init.client_y;
    event->screen_x = init.client_x;
    event->screen_y = init.client_y;
    event->button = init.button;
    event->buttons = init.buttons;
    event->detail = 0;
    event->ctrl_key = init.ctrl;
    event->shift_key = init.shift;
    event->alt_key = init.alt;
    event->meta_key = init.meta;
    event->related_target = init.related_target != nullptr ? js::Value::object(in.wrap(*init.related_target)) : js::Value::null();
    event->members.push_back(js::Value::object(&store));
    return in.dispatch(*event, in.wrap(target));
}

}

bool Realm::drag_start(dom::Element& source, MouseInit const& init, std::vector<std::pair<std::string, std::string>> data, std::string kind)
{
    Internals& in = *m_internals;
    Internals::Entry const entry(in);
    js::Interpreter::Roots const roots(in.interpreter);
    js::Object* const store = new_data_transfer(in, std::move(data), false);
    in.interpreter.root(js::Value::object(store));
    set_drag_store_mode(*store, false, false);
    set_data_transfer_effect(*store, true, "uninitialized");
    set_data_transfer_effect(*store, false, "none");
    Internals::Drag drag;
    drag.store = std::make_unique<js::Persistent>(in.interpreter.heap(), js::Value::object(store));
    drag.source = &source;
    drag.kind = std::move(kind);
    in.drag = std::move(drag);
    bool const proceed = fire(in, source, "dragstart", init, *store, true);
    // From here the store is protected until the drop.
    set_drag_store_mode(*store, false, true);
    if (!proceed || !in.drag) {
        in.drag.reset();
        return false;
    }
    return true;
}

bool Realm::drag_event(dom::Node& target, std::string_view type, MouseInit const& init, std::string const& operation)
{
    Internals& in = *m_internals;
    js::Object* const store = store_of(in);
    if (!store)
        return false;
    Internals::Entry const entry(in);
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(store));
    bool const drop = type == "drop";
    set_drag_store_mode(*store, drop, !drop);
    std::string effect = "none";
    if (type == "dragenter" || type == "dragover")
        effect = initial_drop_effect(data_transfer_effect(*store, true), in.drag->kind);
    else if (drop || type == "dragend")
        effect = operation;
    set_data_transfer_effect(*store, false, effect);
    bool const cancelable = type != "dragleave" && type != "dragend";
    bool const proceed = fire(in, target, type, init, *store, cancelable);
    set_drag_store_mode(*store, false, true);
    return proceed;
}

bool Realm::dragging() const
{
    return store_of(*m_internals) != nullptr;
}

std::string Realm::drag_drop_effect() const
{
    js::Object const* const store = store_of(*m_internals);
    return store ? data_transfer_effect(*store, false) : "none";
}

std::string Realm::drag_effect_allowed() const
{
    js::Object const* const store = store_of(*m_internals);
    return store ? data_transfer_effect(*store, true) : "none";
}

std::vector<std::pair<std::string, std::string>> Realm::drag_data() const
{
    js::Object const* const store = store_of(*m_internals);
    return store ? data_transfer_data(*store) : std::vector<std::pair<std::string, std::string>> {};
}

dom::Element* Realm::drag_source() const
{
    return m_internals->drag ? m_internals->drag->source : nullptr;
}

std::string Realm::drag_kind() const
{
    return m_internals->drag ? m_internals->drag->kind : std::string();
}

void Realm::drag_finished()
{
    m_internals->drag.reset();
}

}
