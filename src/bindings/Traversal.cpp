#include "bindings/NodeSupport.h"

// NodeFilter, TreeWalker and NodeIterator (DOM §6): walking a subtree in
// tree order past what a filter leaves out. The algorithms are the
// standard's, step for step. A walker's place is one node; an iterator's
// is a node and a side of it, kept with the document so that removing the
// node moves the place out of what is removed (dom/Dom.cpp).

#include "dom/Dom.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace sashfold::bindings {

namespace {

constexpr std::uint16_t filter_accept = 1;
constexpr std::uint16_t filter_reject = 2;
constexpr std::uint16_t filter_skip = 3;

dom::Node* first_child(dom::Node const& node)
{
    return node.children().empty() ? nullptr : node.children().front();
}

dom::Node* next_sibling(dom::Node const& node)
{
    dom::Node const* const parent = node.parent();
    if (!parent)
        return nullptr;
    std::uint32_t const at = node.index();
    return at + 1 < parent->children().size() ? parent->children()[at + 1] : nullptr;
}

// A node's bit in whatToShow: one shifted by its nodeType less one.
std::uint32_t show_bit(dom::Node const& node)
{
    int type = 0;
    switch (node.type()) {
    case dom::NodeType::Element: type = 1; break;
    case dom::NodeType::Text: type = static_cast<dom::Text const&>(node).cdata_section ? 4 : 3; break;
    case dom::NodeType::ProcessingInstruction: type = 7; break;
    case dom::NodeType::Comment: type = 8; break;
    case dom::NodeType::Document: type = 9; break;
    case dom::NodeType::DocumentType: type = 10; break;
    case dom::NodeType::DocumentFragment: type = 11; break;
    }
    return type == 0 ? 0u : 1u << (type - 1);
}

// What a walker and an iterator share: the root, what to show, the filter,
// and the flag that keeps a filter from walking the walker it is filtering
// for.
class TraversalObject : public js::Object {
public:
    TraversalObject(js::Object* prototype, dom::Node& root_node, std::uint32_t show, js::Value filter_value)
        : Object(prototype, Class::Host)
        , what_to_show(show)
        , filter(std::move(filter_value))
    {
        place.root = &root_node;
        place.reference = &root_node;
    }

    // The root and the node stood at; an iterator's is kept by the document.
    dom::IteratorPlace place;
    std::uint32_t what_to_show;
    js::Value filter;
    bool active = false;

    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(filter);
        if (place.root)
            tracer.visit(place.root->wrapper);
        if (place.reference)
            tracer.visit(place.reference->wrapper);
    }
};

class TreeWalkerObject final : public TraversalObject {
public:
    using TraversalObject::TraversalObject;
};

class NodeIteratorObject final : public TraversalObject {
public:
    NodeIteratorObject(js::Object* prototype, dom::Node& root_node, std::uint32_t show, js::Value filter_value)
        : TraversalObject(prototype, root_node, show, std::move(filter_value))
    {
        dom::hold_place(place);
    }
    ~NodeIteratorObject() override { dom::release_place(place); }
};

// "Filter a node" (DOM §6): nullopt when the filter threw.
std::optional<std::uint16_t> filter_node(Realm::Internals& in, TraversalObject& traversal, dom::Node& node)
{
    if (traversal.active) {
        in.throw_dom_exception("InvalidStateError", "The filter is already running for this traversal.");
        return std::nullopt;
    }
    if ((traversal.what_to_show & show_bit(node)) == 0)
        return filter_skip;
    if (traversal.filter.is_nullish())
        return filter_accept;
    js::Interpreter& interpreter = in.interpreter;
    js::Interpreter::Roots const roots(interpreter);
    // A callback interface: the function itself, or the acceptNode of an
    // object that is not one, called on that object.
    js::Value callee = traversal.filter;
    js::Value this_value = js::Value::undefined();
    if (!js::Interpreter::is_callable(callee)) {
        std::optional<js::Value> const method = traversal.filter.as_object()->get(
            interpreter, interpreter.key("acceptNode"), traversal.filter);
        if (!method)
            return std::nullopt;
        if (!js::Interpreter::is_callable(*method)) {
            interpreter.throw_type_error("The filter's acceptNode is not a function");
            return std::nullopt;
        }
        callee = *method;
        this_value = traversal.filter;
    }
    js::Value const argument = interpreter.root(js::Value::object(in.wrap(node)));
    traversal.active = true;
    std::optional<js::Value> const result = interpreter.call(callee, this_value, std::span<js::Value const>(&argument, 1));
    traversal.active = false;
    if (!result)
        return std::nullopt;
    std::optional<std::uint32_t> const number = interpreter.to_uint32(*result);
    if (!number)
        return std::nullopt;
    return static_cast<std::uint16_t>(*number & 0xFFFFu);
}

js::Value node_or_null(Realm::Internals& in, dom::Node* node)
{
    return node ? js::Value::object(in.wrap(*node)) : js::Value::null();
}

// --- TreeWalker ---------------------------------------------------------------

Native walker_parent(Realm::Internals& in, TreeWalkerObject& walker)
{
    dom::Node* node = walker.place.reference;
    while (node && node != walker.place.root) {
        node = node->parent();
        if (!node)
            break;
        std::optional<std::uint16_t> const result = filter_node(in, walker, *node);
        if (!result)
            return std::nullopt;
        if (*result == filter_accept) {
            walker.place.reference = node;
            return node_or_null(in, node);
        }
    }
    return js::Value::null();
}

Native walker_children(Realm::Internals& in, TreeWalkerObject& walker, bool first)
{
    dom::Node* node = first ? first_child(*walker.place.reference) : walker.place.reference->last_child();
    while (node) {
        std::optional<std::uint16_t> const result = filter_node(in, walker, *node);
        if (!result)
            return std::nullopt;
        if (*result == filter_accept) {
            walker.place.reference = node;
            return node_or_null(in, node);
        }
        if (*result == filter_skip) {
            if (dom::Node* const child = first ? first_child(*node) : node->last_child()) {
                node = child;
                continue;
            }
        }
        while (node) {
            if (dom::Node* const sibling = first ? next_sibling(*node) : node->previous_sibling()) {
                node = sibling;
                break;
            }
            dom::Node* const parent = node->parent();
            if (!parent || parent == walker.place.root || parent == walker.place.reference)
                return js::Value::null();
            node = parent;
        }
    }
    return js::Value::null();
}

Native walker_siblings(Realm::Internals& in, TreeWalkerObject& walker, bool next)
{
    dom::Node* node = walker.place.reference;
    if (node == walker.place.root)
        return js::Value::null();
    while (true) {
        dom::Node* sibling = next ? next_sibling(*node) : node->previous_sibling();
        while (sibling) {
            node = sibling;
            std::optional<std::uint16_t> const result = filter_node(in, walker, *node);
            if (!result)
                return std::nullopt;
            if (*result == filter_accept) {
                walker.place.reference = node;
                return node_or_null(in, node);
            }
            sibling = next ? first_child(*node) : node->last_child();
            if (*result == filter_reject || !sibling)
                sibling = next ? next_sibling(*node) : node->previous_sibling();
        }
        node = node->parent();
        if (!node || node == walker.place.root)
            return js::Value::null();
        std::optional<std::uint16_t> const result = filter_node(in, walker, *node);
        if (!result)
            return std::nullopt;
        if (*result == filter_accept)
            return js::Value::null();
    }
}

Native walker_previous(Realm::Internals& in, TreeWalkerObject& walker)
{
    dom::Node* node = walker.place.reference;
    while (node != walker.place.root) {
        dom::Node* sibling = node->previous_sibling();
        while (sibling) {
            node = sibling;
            std::optional<std::uint16_t> result = filter_node(in, walker, *node);
            if (!result)
                return std::nullopt;
            while (*result != filter_reject && node->last_child()) {
                node = node->last_child();
                result = filter_node(in, walker, *node);
                if (!result)
                    return std::nullopt;
            }
            if (*result == filter_accept) {
                walker.place.reference = node;
                return node_or_null(in, node);
            }
            sibling = node->previous_sibling();
        }
        if (node == walker.place.root || !node->parent())
            return js::Value::null();
        node = node->parent();
        std::optional<std::uint16_t> const result = filter_node(in, walker, *node);
        if (!result)
            return std::nullopt;
        if (*result == filter_accept) {
            walker.place.reference = node;
            return node_or_null(in, node);
        }
    }
    return js::Value::null();
}

Native walker_next(Realm::Internals& in, TreeWalkerObject& walker)
{
    dom::Node* node = walker.place.reference;
    std::uint16_t result = filter_accept;
    while (true) {
        while (result != filter_reject && first_child(*node)) {
            node = first_child(*node);
            std::optional<std::uint16_t> const answer = filter_node(in, walker, *node);
            if (!answer)
                return std::nullopt;
            result = *answer;
            if (result == filter_accept) {
                walker.place.reference = node;
                return node_or_null(in, node);
            }
        }
        dom::Node* sibling = nullptr;
        for (dom::Node* temporary = node; temporary; temporary = temporary->parent()) {
            if (temporary == walker.place.root)
                return js::Value::null();
            sibling = next_sibling(*temporary);
            if (sibling)
                break;
        }
        if (!sibling)
            return js::Value::null();
        node = sibling;
        std::optional<std::uint16_t> const answer = filter_node(in, walker, *node);
        if (!answer)
            return std::nullopt;
        result = *answer;
        if (result == filter_accept) {
            walker.place.reference = node;
            return node_or_null(in, node);
        }
    }
}

// --- NodeIterator -------------------------------------------------------------

// The node after one in tree order within a root, and the one before it.
dom::Node* following_within(dom::Node& node, dom::Node const& root)
{
    if (dom::Node* const child = first_child(node))
        return child;
    for (dom::Node* current = &node; current && current != &root; current = current->parent()) {
        if (dom::Node* const sibling = next_sibling(*current))
            return sibling;
    }
    return nullptr;
}

dom::Node* preceding_within(dom::Node& node, dom::Node const& root)
{
    if (&node == &root)
        return nullptr;
    dom::Node* before = node.previous_sibling();
    if (!before)
        return node.parent();
    while (before->last_child())
        before = before->last_child();
    return before;
}

Native iterator_traverse(Realm::Internals& in, NodeIteratorObject& iterator, bool next)
{
    if (!iterator.place.reference || !iterator.place.root)
        return js::Value::null();
    // While a node's filter runs the place keeps it as a candidate, so a
    // filter that removes the node — or something around it — moves the
    // walk in flight as any removal moves the reference; the reference
    // itself stays the last node given until the filter accepts, and the
    // node filtered is the one given back. A walk that ends with nothing
    // accepted leaves the reference where it was.
    iterator.place.candidate = iterator.place.reference;
    iterator.place.before_candidate = iterator.place.before_reference;
    while (true) {
        dom::Node* node = iterator.place.candidate;
        bool before = iterator.place.before_candidate;
        if (next) {
            if (!before)
                node = following_within(*node, *iterator.place.root);
            else
                before = false;
        } else {
            if (before)
                node = preceding_within(*node, *iterator.place.root);
            else
                before = true;
        }
        if (!node) {
            iterator.place.candidate = nullptr;
            return js::Value::null();
        }
        iterator.place.candidate = node;
        iterator.place.before_candidate = before;
        std::optional<std::uint16_t> const result = filter_node(in, iterator, *node);
        if (!result) {
            iterator.place.candidate = nullptr;
            return std::nullopt;
        }
        // The filter may have run a script that took the root's document
        // away from under the iterator.
        if (!iterator.place.root || !iterator.place.candidate) {
            iterator.place.candidate = nullptr;
            return js::Value::null();
        }
        if (*result == filter_accept) {
            iterator.place.reference = iterator.place.candidate;
            iterator.place.before_reference = iterator.place.before_candidate;
            iterator.place.candidate = nullptr;
            return node_or_null(in, node);
        }
    }
}

template<typename T>
T* this_traversal(js::Interpreter& interpreter, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* object = dynamic_cast<T*>(this_value.as_object()))
            return object;
    }
    interpreter.throw_type_error("Illegal invocation");
    return nullptr;
}

template<typename T, typename Body>
void traversal_method(Realm::Internals& in, js::Object& prototype, std::string_view name, Body body)
{
    define_operation(in.interpreter, prototype, name, 0,
        [body](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native {
            T* const object = this_traversal<T>(interpreter, this_value);
            if (!object)
                return std::nullopt;
            return body(internals_of(interpreter), *object);
        });
}

template<typename T, typename Read>
void traversal_getter(Realm::Internals& in, js::Object& prototype, std::string_view name, Read read)
{
    define_getter(in, prototype, name, [read](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native {
        T* const object = this_traversal<T>(interpreter, this_value);
        if (!object)
            return std::nullopt;
        return read(internals_of(interpreter), *object);
    });
}

// The arguments createTreeWalker and createNodeIterator share: the root, a
// whatToShow that is everything when left out, and a filter that is null, a
// function, or an object to call acceptNode on.
struct Asked {
    dom::Node* root = nullptr;
    std::uint32_t what_to_show = 0xFFFFFFFFu;
    js::Value filter;
};

std::optional<Asked> asked_for(Realm::Internals& in, Args args)
{
    Asked asked;
    asked.root = in.realm.node_of(js::argument(args, 0));
    if (!asked.root) {
        in.interpreter.throw_type_error("parameter 1 is not of type 'Node'");
        return std::nullopt;
    }
    if (js::Value const show = js::argument(args, 1); !show.is_undefined()) {
        std::optional<std::uint32_t> const number = in.interpreter.to_uint32(show);
        if (!number)
            return std::nullopt;
        asked.what_to_show = *number;
    }
    asked.filter = js::argument(args, 2);
    if (asked.filter.is_undefined())
        asked.filter = js::Value::null();
    if (!asked.filter.is_null() && !asked.filter.is_object()) {
        in.interpreter.throw_type_error("parameter 3 is not an object");
        return std::nullopt;
    }
    return asked;
}

} // namespace

Native new_tree_walker(Realm::Internals& in, Args args)
{
    std::optional<Asked> const asked = asked_for(in, args);
    if (!asked)
        return std::nullopt;
    return js::Value::object(in.interpreter.heap().allocate<TreeWalkerObject>(
        in.prototype("TreeWalker"), *asked->root, asked->what_to_show, asked->filter));
}

Native new_node_iterator(Realm::Internals& in, Args args)
{
    std::optional<Asked> const asked = asked_for(in, args);
    if (!asked)
        return std::nullopt;
    return js::Value::object(in.interpreter.heap().allocate<NodeIteratorObject>(
        in.prototype("NodeIterator"), *asked->root, asked->what_to_show, asked->filter));
}

void install_traversal(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());

    // NodeFilter is a callback interface: an object to read the constants
    // from, which nothing is an instance of.
    js::Object* const node_filter = define_interface(in, "NodeFilter", nullptr);
    std::optional<js::Value> const filter_constructor
        = node_filter->get(interpreter, interpreter.key("constructor"), js::Value::object(node_filter));
    for (auto const& [name, value] : { std::pair<char const*, double> { "FILTER_ACCEPT", 1 }, { "FILTER_REJECT", 2 },
             { "FILTER_SKIP", 3 }, { "SHOW_ALL", 4294967295.0 }, { "SHOW_ELEMENT", 0x1 }, { "SHOW_ATTRIBUTE", 0x2 },
             { "SHOW_TEXT", 0x4 }, { "SHOW_CDATA_SECTION", 0x8 }, { "SHOW_ENTITY_REFERENCE", 0x10 },
             { "SHOW_ENTITY", 0x20 }, { "SHOW_PROCESSING_INSTRUCTION", 0x40 }, { "SHOW_COMMENT", 0x80 },
             { "SHOW_DOCUMENT", 0x100 }, { "SHOW_DOCUMENT_TYPE", 0x200 }, { "SHOW_DOCUMENT_FRAGMENT", 0x400 },
             { "SHOW_NOTATION", 0x800 } }) {
        node_filter->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        if (filter_constructor && filter_constructor->is_object())
            filter_constructor->as_object()->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }

    js::Object* const walker = define_interface(in, "TreeWalker", nullptr);
    traversal_getter<TreeWalkerObject>(in, *walker, "root", [](Realm::Internals& internals, TreeWalkerObject& w) -> Native {
        return node_or_null(internals, w.place.root);
    });
    traversal_getter<TreeWalkerObject>(in, *walker, "whatToShow", [](Realm::Internals&, TreeWalkerObject& w) -> Native {
        return js::Value::number(static_cast<double>(w.what_to_show));
    });
    traversal_getter<TreeWalkerObject>(in, *walker, "filter", [](Realm::Internals&, TreeWalkerObject& w) -> Native {
        return w.filter;
    });
    define_getter(
        in, *walker, "currentNode",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            TreeWalkerObject* const w = this_traversal<TreeWalkerObject>(interp, this_value);
            if (!w)
                return std::nullopt;
            return node_or_null(internals_of(interp), w->place.reference);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            TreeWalkerObject* const w = this_traversal<TreeWalkerObject>(interp, this_value);
            if (!w)
                return std::nullopt;
            dom::Node* const node = internals_of(interp).realm.node_of(js::argument(args, 0));
            if (!node)
                return interp.throw_type_error("The value is not of type 'Node'");
            w->place.reference = node;
            return js::Value::undefined();
        });
    traversal_method<TreeWalkerObject>(in, *walker, "parentNode", walker_parent);
    traversal_method<TreeWalkerObject>(in, *walker, "firstChild",
        [](Realm::Internals& internals, TreeWalkerObject& w) { return walker_children(internals, w, true); });
    traversal_method<TreeWalkerObject>(in, *walker, "lastChild",
        [](Realm::Internals& internals, TreeWalkerObject& w) { return walker_children(internals, w, false); });
    traversal_method<TreeWalkerObject>(in, *walker, "previousSibling",
        [](Realm::Internals& internals, TreeWalkerObject& w) { return walker_siblings(internals, w, false); });
    traversal_method<TreeWalkerObject>(in, *walker, "nextSibling",
        [](Realm::Internals& internals, TreeWalkerObject& w) { return walker_siblings(internals, w, true); });
    traversal_method<TreeWalkerObject>(in, *walker, "previousNode", walker_previous);
    traversal_method<TreeWalkerObject>(in, *walker, "nextNode", walker_next);

    js::Object* const iterator = define_interface(in, "NodeIterator", nullptr);
    traversal_getter<NodeIteratorObject>(in, *iterator, "root", [](Realm::Internals& internals, NodeIteratorObject& i) -> Native {
        return node_or_null(internals, i.place.root);
    });
    traversal_getter<NodeIteratorObject>(in, *iterator, "referenceNode", [](Realm::Internals& internals, NodeIteratorObject& i) -> Native {
        return node_or_null(internals, i.place.reference);
    });
    traversal_getter<NodeIteratorObject>(in, *iterator, "pointerBeforeReferenceNode", [](Realm::Internals&, NodeIteratorObject& i) -> Native {
        return js::Value::boolean(i.place.before_reference);
    });
    traversal_getter<NodeIteratorObject>(in, *iterator, "whatToShow", [](Realm::Internals&, NodeIteratorObject& i) -> Native {
        return js::Value::number(static_cast<double>(i.what_to_show));
    });
    traversal_getter<NodeIteratorObject>(in, *iterator, "filter", [](Realm::Internals&, NodeIteratorObject& i) -> Native {
        return i.filter;
    });
    traversal_method<NodeIteratorObject>(in, *iterator, "nextNode",
        [](Realm::Internals& internals, NodeIteratorObject& i) { return iterator_traverse(internals, i, true); });
    traversal_method<NodeIteratorObject>(in, *iterator, "previousNode",
        [](Realm::Internals& internals, NodeIteratorObject& i) { return iterator_traverse(internals, i, false); });
    // detach() does nothing, and has since the iterators stopped needing it.
    traversal_method<NodeIteratorObject>(in, *iterator, "detach",
        [](Realm::Internals&, NodeIteratorObject&) -> Native { return js::Value::undefined(); });
}

}
