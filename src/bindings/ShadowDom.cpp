// Shadow trees as a script sees them (DOM §4.8 and §4.2.2, HTML §4.12.4):
// attachShadow and the ShadowRoot it makes, the slot element's assigned
// nodes, and the members of Element, Text and Node that answer about them.
// The trees themselves, and the slot algorithms, are the DOM's (dom/Dom.h).

#include "bindings/NodeSupport.h"
#include "html/Serializer.h"

#include <algorithm>

namespace sashfold::bindings {

namespace {

// A dictionary's member: undefined for a dictionary that is not an object.
std::optional<js::Value> member_of(js::Interpreter& interp, js::Value const& dictionary, std::string_view name)
{
    if (!dictionary.is_object())
        return js::Value::undefined();
    return interp.get(dictionary, name);
}

std::optional<bool> flag_of(js::Interpreter& interp, js::Value const& dictionary, std::string_view name)
{
    std::optional<js::Value> const value = member_of(interp, dictionary, name);
    if (!value)
        return std::nullopt;
    return js::Interpreter::to_boolean(*value);
}

dom::ShadowRoot* shadow_root_of(dom::Node& node)
{
    return node.is_shadow_root() ? static_cast<dom::ShadowRoot*>(&node) : nullptr;
}

// A ShadowRoot member that reads the root: a plain function, as the
// reader it is given captures nothing.
template<typename Read>
void shadow_getter(Realm::Internals& in, js::Object& prototype, std::string_view name, Read)
{
    node_getter(in, prototype, name, [](Realm::Internals& internals, dom::Node& n) -> Native {
        dom::ShadowRoot* const shadow = shadow_root_of(n);
        if (shadow == nullptr)
            return internals.interpreter.throw_type_error("Illegal invocation");
        return Read {}(internals, *shadow);
    });
}

// assignedSlot (DOM §4.2.2.3 "find a slot", with the open flag): the slot
// of an open tree; a closed tree's is not told.
Native assigned_slot_of(Realm::Internals& internals, dom::Node& n)
{
    dom::Element* const slot = n.assigned_slot();
    if (slot == nullptr)
        return js::Value::null();
    dom::Node const& root = slot->root();
    if (!root.is_shadow_root() || static_cast<dom::ShadowRoot const&>(root).mode != dom::ShadowRoot::Mode::Open)
        return js::Value::null();
    return js::Value::object(internals.wrap(*slot));
}

js::Value array_of(Realm::Internals& in, std::vector<dom::Node*> const& nodes, bool elements_only)
{
    js::Interpreter::Roots const roots(in.interpreter);
    js::ArrayObject* const array = in.interpreter.new_array();
    in.interpreter.root(js::Value::object(array));
    for (dom::Node* const node : nodes) {
        if (!elements_only || node->is_element())
            array->push(js::Value::object(in.wrap(*node)));
    }
    return js::Value::object(array);
}

// Element.attachShadow(init) (DOM §4.9 "attach a shadow root").
Native attach_shadow(Realm::Internals& in, dom::Element& element, Args args)
{
    js::Interpreter& interp = in.interpreter;
    js::Value const init = js::argument(args, 0);
    if (!init.is_object())
        return interp.throw_type_error("Failed to execute 'attachShadow' on 'Element': the argument is not a ShadowRootInit dictionary");
    std::optional<js::Value> const mode_value = member_of(interp, init, "mode");
    if (!mode_value)
        return std::nullopt;
    std::optional<std::string> const mode = mode_value->is_undefined() ? std::optional<std::string>(std::string()) : in.to_utf8(*mode_value);
    if (!mode)
        return std::nullopt;
    if (*mode != "open" && *mode != "closed")
        return interp.throw_type_error("Failed to execute 'attachShadow' on 'Element': a ShadowRootInit's mode is \"open\" or \"closed\"");
    std::optional<bool> const delegates_focus = flag_of(interp, init, "delegatesFocus");
    if (!delegates_focus)
        return std::nullopt;
    std::optional<js::Value> const assignment_value = member_of(interp, init, "slotAssignment");
    if (!assignment_value)
        return std::nullopt;
    std::optional<std::string> const assignment
        = assignment_value->is_undefined() ? std::optional<std::string>("named") : in.to_utf8(*assignment_value);
    if (!assignment)
        return std::nullopt;
    if (*assignment != "named" && *assignment != "manual")
        return interp.throw_type_error("Failed to execute 'attachShadow' on 'Element': a ShadowRootInit's slotAssignment is \"named\" or \"manual\"");
    std::optional<bool> const clonable = flag_of(interp, init, "clonable");
    if (!clonable)
        return std::nullopt;
    std::optional<bool> const serializable = flag_of(interp, init, "serializable");
    if (!serializable)
        return std::nullopt;

    // The one registry there is here is each window's own: it cannot be
    // given to an element of another document (DOM §4.9, the registry
    // steps of attachShadow).
    std::optional<js::Value> const registry = member_of(interp, init, "customElementRegistry");
    if (!registry)
        return std::nullopt;
    if (registry->is_object() && &element.document() != in.document) {
        std::optional<js::Value> const own = interp.get(*interp.global(), interp.key("customElements"));
        if (own && own->is_object() && own->as_object() == registry->as_object())
            return in.throw_dom_exception("NotSupportedError", "Failed to execute 'attachShadow' on 'Element': the registry is another document's.");
    }

    if (!element.is_html() || !dom::is_valid_shadow_host_name(element.local_name()))
        return in.throw_dom_exception("NotSupportedError", "Failed to execute 'attachShadow' on 'Element': This element does not support attachShadow");
    if (custom_element_disables_shadow(in, element))
        return in.throw_dom_exception("NotSupportedError", "Failed to execute 'attachShadow' on 'Element': attachShadow() is disabled by disabledFeatures static field.");
    dom::ShadowRoot::Mode const wanted = *mode == "open" ? dom::ShadowRoot::Mode::Open : dom::ShadowRoot::Mode::Closed;
    if (dom::ShadowRoot* const current = element.shadow_root()) {
        // A root the parser made from a template stands in for the one the
        // element's own script asks for, emptied; any other is the element's
        // one root already.
        if (!current->declarative || current->mode != wanted)
            return in.throw_dom_exception("NotSupportedError", "Failed to execute 'attachShadow' on 'Element': Shadow root cannot be created on a host which already hosts a shadow tree.");
        std::vector<dom::Node*> const children = current->children();
        for (dom::Node* const child : children)
            remove_node(in, *child);
        current->declarative = false;
        return js::Value::object(in.wrap(*current));
    }
    dom::ShadowRoot& shadow = element.attach_shadow();
    shadow.mode = wanted;
    shadow.delegates_focus = *delegates_focus;
    shadow.slot_assignment = *assignment == "manual" ? dom::ShadowRoot::SlotAssignment::Manual : dom::ShadowRoot::SlotAssignment::Named;
    shadow.clonable = *clonable;
    shadow.serializable = *serializable;
    shadow.available_to_element_internals = element.custom_defined();
    in.realm.note_mutation();
    return js::Value::object(in.wrap(shadow));
}

// getHTML(options) (HTML §8.5.3): the node's markup with the shadow roots
// the options ask for written as declarative templates.
Native get_html(Realm::Internals& in, dom::Node& node, Args args)
{
    js::Interpreter& interp = in.interpreter;
    js::Value const options = js::argument(args, 0);
    html::ShadowRootSerialization shadow;
    std::optional<bool> const serializable = flag_of(interp, options, "serializableShadowRoots");
    if (!serializable)
        return std::nullopt;
    shadow.serializable = *serializable;
    std::optional<js::Value> const roots = member_of(interp, options, "shadowRoots");
    if (!roots)
        return std::nullopt;
    if (roots->is_object()) {
        std::optional<js::Value> const length = interp.get(*roots, "length");
        if (!length)
            return std::nullopt;
        std::optional<double> const count = interp.to_number(*length);
        if (!count)
            return std::nullopt;
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(std::max(0.0, *count)); ++i) {
            std::optional<js::Value> const entry = interp.get(*roots, js::PropertyKey::index(i));
            if (!entry)
                return std::nullopt;
            NodeWrapper* const wrapper = in.wrapper_of(*entry);
            if (wrapper == nullptr || !wrapper->node().is_shadow_root())
                return interp.throw_type_error("Failed to execute 'getHTML': shadowRoots holds a value that is not a ShadowRoot");
            shadow.roots.push_back(static_cast<dom::ShadowRoot const*>(&wrapper->node()));
        }
    }
    dom::Node const& from = node.is_element() ? content_container(static_cast<dom::Element&>(node)) : node;
    // A template's contents are not its element: the host step looks at the element.
    return in.string(html::serialize_children(node.is_element() && &from == &node ? node : from, shadow));
}

// setHTMLUnsafe(html) (HTML §8.5.2 "unsafely set HTML"): the markup parsed
// with declarative shadow roots allowed, in place of the node's children.
Native set_html_unsafe(Realm::Internals& in, dom::Node& target, dom::Element& context, Args args)
{
    std::optional<std::string> const markup = in.to_utf8(js::argument(args, 0));
    if (!markup)
        return std::nullopt;
    replace_children_with_markup(in, target, context, *markup, true);
    return js::Value::undefined();
}

// assignedNodes(options) and assignedElements(options) (HTML §4.12.4).
Native assigned(Realm::Internals& in, dom::Element& slot, Args args, bool elements_only)
{
    std::optional<bool> const flatten = flag_of(in.interpreter, js::argument(args, 0), "flatten");
    if (!flatten)
        return std::nullopt;
    if (*flatten)
        return array_of(in, dom::find_flattened_slottables(slot), elements_only);
    return array_of(in, slot.assigned_nodes(), elements_only);
}

}

void install_shadow_dom(Realm::Internals& in)
{
    js::Object& element = *in.prototype("Element");
    js::Object& node = *in.prototype("Node");
    js::Object& shadow_root = *in.prototype("ShadowRoot");

    element_method(in, element, "attachShadow", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        return attach_shadow(internals, e, args);
    });
    // An open root is the element's to show; a closed one only its maker has.
    element_getter(in, element, "shadowRoot", [](Realm::Internals& internals, dom::Element& e) -> Native {
        dom::ShadowRoot* const shadow = e.shadow_root();
        if (shadow == nullptr || shadow->mode != dom::ShadowRoot::Mode::Open)
            return js::Value::null();
        return js::Value::object(internals.wrap(*shadow));
    });
    element_getter(in, element, "assignedSlot", [](Realm::Internals& internals, dom::Element& e) -> Native { return assigned_slot_of(internals, e); });
    if (js::Object* const text = in.prototype("Text"))
        node_getter(in, *text, "assignedSlot", [](Realm::Internals& internals, dom::Node& n) -> Native { return assigned_slot_of(internals, n); });

    element_method(in, element, "getHTML", 0, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native { return get_html(internals, e, args); });
    element_method(in, element, "setHTMLUnsafe", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        return set_html_unsafe(internals, content_container(e), e, args);
    });

    // getRootNode(options): the root of the node's tree, or with
    // `composed` the root past every shadow root above it.
    node_method(in, node, "getRootNode", 0, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<bool> const composed = flag_of(internals.interpreter, js::argument(args, 0), "composed");
        if (!composed)
            return std::nullopt;
        return js::Value::object(internals.wrap(*composed ? n.shadow_including_root() : n.root()));
    });

    // ShadowRoot.
    shadow_getter(in, shadow_root, "mode", [](Realm::Internals& internals, dom::ShadowRoot& shadow) -> Native {
        return internals.string(shadow.mode == dom::ShadowRoot::Mode::Open ? "open" : "closed");
    });
    shadow_getter(in, shadow_root, "delegatesFocus", [](Realm::Internals&, dom::ShadowRoot& shadow) -> Native { return js::Value::boolean(shadow.delegates_focus); });
    shadow_getter(in, shadow_root, "slotAssignment", [](Realm::Internals& internals, dom::ShadowRoot& shadow) -> Native {
        return internals.string(shadow.slot_assignment == dom::ShadowRoot::SlotAssignment::Manual ? "manual" : "named");
    });
    shadow_getter(in, shadow_root, "clonable", [](Realm::Internals&, dom::ShadowRoot& shadow) -> Native { return js::Value::boolean(shadow.clonable); });
    shadow_getter(in, shadow_root, "serializable", [](Realm::Internals&, dom::ShadowRoot& shadow) -> Native { return js::Value::boolean(shadow.serializable); });
    shadow_getter(in, shadow_root, "host", [](Realm::Internals& internals, dom::ShadowRoot& shadow) -> Native {
        return js::Value::object(internals.wrap(shadow.host()));
    });
    // innerHTML: the tree's markup, and markup parsed as the host's would
    // be to take the tree's place.
    node_accessor(
        in, shadow_root, "innerHTML",
        [](Realm::Internals& internals, dom::Node& n) -> Native {
            if (!n.is_shadow_root())
                return internals.interpreter.throw_type_error("Illegal invocation");
            return internals.string(html::serialize_children(n));
        },
        [](Realm::Internals& internals, dom::Node& n, js::Value const& value) -> Native {
            dom::ShadowRoot* const shadow = shadow_root_of(n);
            if (shadow == nullptr)
                return internals.interpreter.throw_type_error("Illegal invocation");
            std::optional<std::string> const markup = value.is_nullish() ? std::optional<std::string>("") : internals.to_utf8(value);
            if (!markup)
                return std::nullopt;
            replace_children_with_markup(internals, *shadow, shadow->host(), *markup);
            return js::Value::undefined();
        });
    node_method(in, shadow_root, "getHTML", 0, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        if (!n.is_shadow_root())
            return internals.interpreter.throw_type_error("Illegal invocation");
        return get_html(internals, n, args);
    });
    node_method(in, shadow_root, "setHTMLUnsafe", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        dom::ShadowRoot* const shadow = shadow_root_of(n);
        if (shadow == nullptr)
            return internals.interpreter.throw_type_error("Illegal invocation");
        return set_html_unsafe(internals, *shadow, shadow->host(), args);
    });
    // The members a tree shares with a document (DocumentOrShadowRoot)
    // that answer from states only a document has here.
    for (std::string_view const name : { "fullscreenElement", "pictureInPictureElement", "pointerLockElement" })
        node_getter(in, shadow_root, name, [](Realm::Internals&, dom::Node&) -> Native { return js::Value::null(); });
    // activeElement: the focused element as this tree sees it — itself
    // when it is in the tree, the host that holds it when it is deeper.
    shadow_getter(in, shadow_root, "activeElement", [](Realm::Internals& internals, dom::ShadowRoot& shadow) -> Native {
        dom::Element const* const focused = shadow.document().focused();
        for (dom::Node const* at = focused; at != nullptr;) {
            dom::Node const& root = at->root();
            if (&root == &shadow)
                return js::Value::object(internals.wrap(const_cast<dom::Node&>(*at)));
            if (!root.is_shadow_root())
                break;
            at = &static_cast<dom::ShadowRoot const&>(root).host();
        }
        return js::Value::null();
    });

    // The slot element.
    if (js::Object* const slot = in.prototype("HTMLSlotElement")) {
        element_method(in, *slot, "assignedNodes", 0, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
            return assigned(internals, e, args, false);
        });
        element_method(in, *slot, "assignedElements", 0, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
            return assigned(internals, e, args, true);
        });
        // assign(...nodes): the nodes a slot takes where its root assigns
        // manually. A node named to this slot is taken from any other.
        element_method(in, *slot, "assign", 0, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
            std::vector<dom::Node*> nodes;
            for (std::size_t i = 0; i < args.size(); ++i) {
                NodeWrapper* const wrapper = internals.wrapper_of(args[i]);
                dom::Node* const given = wrapper != nullptr ? &wrapper->node() : nullptr;
                if (given == nullptr || !(given->is_element() || given->is_text()))
                    return internals.interpreter.throw_type_error("Failed to execute 'assign' on 'HTMLSlotElement': an argument is not an Element or a Text node");
                if (std::find(nodes.begin(), nodes.end(), given) == nodes.end())
                    nodes.push_back(given);
            }
            dom::Node& root = e.root();
            if (root.is_shadow_root()) {
                for (dom::Element* const other : static_cast<dom::ShadowRoot&>(root).slots()) {
                    if (other == &e)
                        continue;
                    std::vector<dom::Node*> kept = other->manually_assigned_nodes();
                    std::erase_if(kept, [&nodes](dom::Node* held) { return std::find(nodes.begin(), nodes.end(), held) != nodes.end(); });
                    if (kept.size() != other->manually_assigned_nodes().size())
                        other->set_manually_assigned_nodes(std::move(kept));
                }
            }
            e.set_manually_assigned_nodes(std::move(nodes));
            dom::assign_slottables_for_tree(e.root());
            internals.realm.note_mutation();
            return js::Value::undefined();
        });
    }
}

}
