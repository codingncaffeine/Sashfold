#include "bindings/Internal.h"
#include "bindings/NodeSupport.h"

#include <algorithm>
#include <string>
#include <vector>

// MutationObserver (DOM §4.3): a page asks to be told what changed in a
// part of the tree, and hears about it in one batch at the end of the turn
// rather than as each change happens.
//
// It is not only a convenience for pages. The component libraries the
// modern web is built with watch their own children with one, to know what
// has been put inside them and where it should go; a browser whose
// observer never delivers leaves those components holding nothing. An
// observer that takes registrations and says nothing is worse than none at
// all, because a page cannot tell it apart from a tree that never changed.

namespace sashfold::bindings {

namespace {

struct Registration {
    dom::Node* target = nullptr;
    bool child_list = false;
    bool attributes = false;
    bool character_data = false;
    bool subtree = false;
    bool attribute_old_value = false;
    bool character_data_old_value = false;
    bool has_attribute_filter = false;
    std::vector<std::string> attribute_filter;
};

class MutationObserverObject final : public js::Object {
public:
    MutationObserverObject(js::Object* prototype, js::Value the_callback)
        : Object(prototype, Class::Host)
        , callback(the_callback)
    {
    }
    js::Value callback;
    std::vector<Registration> registrations;
    std::vector<js::Value> records;

    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(callback);
        for (js::Value const& record : records)
            tracer.visit(record);
    }
    std::size_t size_in_bytes() const override
    {
        return Object::size_in_bytes() + registrations.size() * sizeof(Registration) + records.size() * sizeof(js::Value);
    }
};

// The records a callback is handed, as a plain array.
js::Value record_array(Realm::Internals& in, std::vector<js::Value> const& records)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Interpreter::Roots const roots(interpreter);
    js::ArrayObject* list = interpreter.new_array();
    interpreter.root(js::Value::object(list));
    for (js::Value const& record : records)
        list->push(record);
    return js::Value::object(list);
}

std::optional<MutationObserverObject*> this_observer(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* found = dynamic_cast<MutationObserverObject*>(this_value.as_object()))
            return found;
    }
    return interp.throw_type_error("Illegal invocation");
}

// Whether a registration is watching the node the change happened to: its
// own target, or a node beneath it when it asked for the subtree.
bool watches(Registration const& registration, dom::Node& node)
{
    if (registration.target == &node)
        return true;
    if (!registration.subtree)
        return false;
    for (dom::Node* ancestor = node.parent(); ancestor != nullptr; ancestor = ancestor->parent()) {
        if (ancestor == registration.target)
            return true;
    }
    return false;
}

// Everything the observers are owed is delivered together, in a microtask,
// once per turn: a page sees one batch of what changed, not one call per
// change.
void arrange_delivery(Realm::Internals& in)
{
    if (in.mutation_delivery_pending)
        return;
    in.mutation_delivery_pending = true;
    js::Interpreter& interpreter = in.interpreter;
    js::Interpreter::Roots const roots(interpreter);
    js::NativeFunction* deliver = interpreter.new_native("deliver mutations", 0,
        [](js::Interpreter& interp, js::Value const&, Args) -> Native {
            Realm::Internals& internals = internals_of(interp);
            internals.mutation_delivery_pending = false;
            // The list is copied first: a callback may make or drop
            // observers, and must not move the one being walked.
            std::vector<js::Object*> const observers = internals.mutation_observers;
            for (js::Object* const object : observers) {
                auto* const observer = static_cast<MutationObserverObject*>(object);
                if (observer->records.empty())
                    continue;
                js::Interpreter::Roots const inner(interp);
                interp.root(js::Value::object(observer));
                // The array is made while the records are still the
                // observer's, which is what holds them: taking them out
                // first would leave them for the next collection.
                js::Value const list = interp.root(record_array(internals, observer->records));
                observer->records.clear();
                js::Value const arguments[] = { list, js::Value::object(observer) };
                internals.call_reporting(observer->callback, js::Value::object(observer), arguments, "MutationObserver callback");
            }
            return js::Value::undefined();
        });
    interpreter.enqueue_microtask(js::Value::object(deliver), {});
}

// One record, and a copy of it for every observer that asked to hear about
// this change.
void queue_record(Realm::Internals& in, dom::Node& node, std::string_view type,
    std::vector<dom::Node*> const& added, std::vector<dom::Node*> const& removed, dom::Node* previous, dom::Node* next,
    std::string_view attribute_name, std::string_view attribute_namespace, std::optional<std::string> const& old_value)
{
    if (in.mutation_observers.empty())
        return;
    js::Interpreter& interpreter = in.interpreter;
    bool made_any = false;
    for (js::Object* const object : in.mutation_observers) {
        auto* const observer = static_cast<MutationObserverObject*>(object);
        for (Registration const& registration : observer->registrations) {
            if (!watches(registration, node))
                continue;
            if (type == "childList" && !registration.child_list)
                continue;
            if (type == "attributes") {
                if (!registration.attributes)
                    continue;
                if (registration.has_attribute_filter
                    && std::find(registration.attribute_filter.begin(), registration.attribute_filter.end(), attribute_name)
                        == registration.attribute_filter.end())
                    continue;
            }
            if (type == "characterData" && !registration.character_data)
                continue;

            js::Interpreter::Roots const roots(interpreter);
            interpreter.root(js::Value::object(observer));
            js::Heap::NoCollect const no_collect(interpreter.heap());
            js::Object* record = interpreter.heap().allocate<PlainPlatformObject>(in.prototype("MutationRecord"));
            record->put(interpreter.key("type"), in.string(type), js::Enumerable);
            record->put(interpreter.key("target"), js::Value::object(in.wrap(node)), js::Enumerable);
            record->put(interpreter.key("addedNodes"), node_list(in, added), js::Enumerable);
            record->put(interpreter.key("removedNodes"), node_list(in, removed), js::Enumerable);
            record->put(interpreter.key("previousSibling"),
                previous != nullptr ? js::Value::object(in.wrap(*previous)) : js::Value::null(), js::Enumerable);
            record->put(interpreter.key("nextSibling"), next != nullptr ? js::Value::object(in.wrap(*next)) : js::Value::null(),
                js::Enumerable);
            record->put(interpreter.key("attributeName"),
                attribute_name.empty() ? js::Value::null() : in.string(attribute_name), js::Enumerable);
            record->put(interpreter.key("attributeNamespace"),
                attribute_namespace.empty() ? js::Value::null() : in.string(attribute_namespace), js::Enumerable);
            // The value before the change, for an observer that asked for it.
            bool const wanted = (type == "attributes" && registration.attribute_old_value)
                || (type == "characterData" && registration.character_data_old_value);
            record->put(interpreter.key("oldValue"), wanted && old_value ? in.string(*old_value) : js::Value::null(), js::Enumerable);
            observer->records.push_back(js::Value::object(record));
            made_any = true;
            break; // one record per observer, however many registrations match
        }
    }
    if (made_any)
        arrange_delivery(in);
}

}

// --- What the tree tells it -------------------------------------------------------------

void mutation_children_changed(Realm::Internals& in, dom::Node& parent, std::vector<dom::Node*> const& added,
    std::vector<dom::Node*> const& removed, dom::Node* previous, dom::Node* next)
{
    if (added.empty() && removed.empty())
        return;
    queue_record(in, parent, "childList", added, removed, previous, next, {}, {}, std::nullopt);
}

void mutation_attribute_changed(Realm::Internals& in, dom::Element& element, std::string_view namespace_uri,
    std::string_view local_name, std::optional<std::string> const& old_value)
{
    queue_record(in, element, "attributes", {}, {}, nullptr, nullptr, local_name, namespace_uri, old_value);
}

void mutation_character_data_changed(Realm::Internals& in, dom::Node& node, std::optional<std::string> const& old_value)
{
    queue_record(in, node, "characterData", {}, {}, nullptr, nullptr, {}, {}, old_value);
}

void trace_mutation_observers(Realm::Internals const& in, js::Tracer& tracer)
{
    for (js::Object* const observer : in.mutation_observers)
        tracer.visit(observer);
}

void install_mutation_observer(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    define_interface(in, "MutationRecord", nullptr);
    js::Object* proto = define_interface(in, "MutationObserver", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            js::Value const callback = js::argument(args, 0);
            if (!js::Interpreter::is_callable(callback))
                return interp.throw_type_error("Failed to construct 'MutationObserver': parameter 1 is not of type 'Function'.");
            Realm::Internals& internals = internals_of(interp);
            return js::Value::object(interp.heap().allocate<MutationObserverObject>(internals.prototype("MutationObserver"), callback));
        },
        1);

    js::define_method(interpreter, *proto, "observe", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<MutationObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        std::optional<dom::Node*> const target = this_node(interp, js::argument(args, 0));
        if (!target)
            return std::nullopt;
        Registration registration;
        registration.target = *target;
        js::Value const options = js::argument(args, 1);
        // Whether the page named the option at all, which the rules below
        // turn on as much as its value: `{ attributeOldValue: false }` asks
        // for attributes, because it spoke of them (DOM §4.3.1).
        bool said_attributes = false;
        bool said_character_data = false;
        bool said_attribute_old_value = false;
        bool said_character_data_old_value = false;
        auto const flag = [&](char const* name, bool& into, bool* said = nullptr) -> bool {
            if (!options.is_object())
                return true;
            std::optional<js::Value> const found = interp.get(*options.as_object(), interp.key(name));
            if (!found)
                return false;
            if (found->is_undefined())
                return true;
            into = js::Interpreter::to_boolean(*found);
            if (said != nullptr)
                *said = true;
            return true;
        };
        if (!flag("childList", registration.child_list) || !flag("subtree", registration.subtree)
            || !flag("attributes", registration.attributes, &said_attributes)
            || !flag("characterData", registration.character_data, &said_character_data)
            || !flag("attributeOldValue", registration.attribute_old_value, &said_attribute_old_value)
            || !flag("characterDataOldValue", registration.character_data_old_value, &said_character_data_old_value))
            return std::nullopt;
        bool said_attribute_filter = false;
        if (options.is_object()) {
            std::optional<js::Value> const filter = interp.get(*options.as_object(), interp.key("attributeFilter"));
            if (!filter)
                return std::nullopt;
            if (!filter->is_undefined())
                said_attribute_filter = true;
            if (filter->is_object()) {
                registration.has_attribute_filter = true;
                std::optional<js::Value> const length = interp.get(*filter->as_object(), interp.key("length"));
                if (!length)
                    return std::nullopt;
                std::optional<double> const count = interp.to_number(*length);
                if (!count)
                    return std::nullopt;
                for (std::size_t i = 0; i < static_cast<std::size_t>(std::max(0.0, *count)); ++i) {
                    std::optional<js::Value> const entry = interp.get(*filter->as_object(), js::PropertyKey::index(i));
                    if (!entry)
                        return std::nullopt;
                    std::optional<std::string> const text = internals.to_utf8(*entry);
                    if (!text)
                        return std::nullopt;
                    registration.attribute_filter.push_back(*text);
                }
            }
        }
        // Naming the old value or a filter asks for attributes themselves,
        // unless the page said otherwise; and having said otherwise while
        // asking for either is a contradiction, not a default.
        if ((said_attribute_old_value || said_attribute_filter) && !said_attributes)
            registration.attributes = true;
        if (said_character_data_old_value && !said_character_data)
            registration.character_data = true;
        if (!registration.child_list && !registration.attributes && !registration.character_data) {
            return interp.throw_type_error(
                "Failed to execute 'observe' on 'MutationObserver': the options must ask for childList, attributes or characterData.");
        }
        if (!registration.attributes && (registration.attribute_old_value || said_attribute_filter)) {
            return interp.throw_type_error(
                "Failed to execute 'observe' on 'MutationObserver': attributes were asked about but not asked for.");
        }
        if (!registration.character_data && registration.character_data_old_value) {
            return interp.throw_type_error(
                "Failed to execute 'observe' on 'MutationObserver': character data was asked about but not asked for.");
        }
        // Observing the same node again replaces what was asked before.
        auto& registrations = (*observer)->registrations;
        std::erase_if(registrations, [&](Registration const& already) { return already.target == *target; });
        registrations.push_back(std::move(registration));
        if (std::find(internals.mutation_observers.begin(), internals.mutation_observers.end(), *observer)
            == internals.mutation_observers.end())
            internals.mutation_observers.push_back(*observer);
        return js::Value::undefined();
    });

    js::define_method(interpreter, *proto, "disconnect", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<MutationObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        (*observer)->registrations.clear();
        (*observer)->records.clear();
        std::erase(internals.mutation_observers, static_cast<js::Object*>(*observer));
        return js::Value::undefined();
    });

    js::define_method(interpreter, *proto, "takeRecords", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<MutationObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        // Made while the records are still the observer's, which is what
        // holds them against a collection.
        js::Value const list = record_array(internals, (*observer)->records);
        (*observer)->records.clear();
        return list;
    });
}

}
