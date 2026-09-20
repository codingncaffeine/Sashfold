#include "bindings/NodeSupport.h"

// The Node, Element and CharacterData interfaces (DOM §4), the tree
// algorithms behind them, NodeList and HTMLCollection, and DOMRect. The
// HTML element interfaces and Document are in their own files.

#include "core/Unicode.h"
#include "css/Parser.h"
#include "html/Serializer.h"
#include "html/TreeBuilder.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::bindings {

// --- Tree helpers -----------------------------------------------------------------------

dom::Node* next_sibling_of(dom::Node const& node)
{
    dom::Node const* parent = node.parent();
    if (!parent)
        return nullptr;
    auto const& siblings = parent->children();
    auto const it = std::find(siblings.begin(), siblings.end(), &node);
    if (it == siblings.end() || it + 1 == siblings.end())
        return nullptr;
    return *(it + 1);
}

dom::Element* first_element_child(dom::Node const& node)
{
    for (dom::Node* child : node.children()) {
        if (child->is_element())
            return static_cast<dom::Element*>(child);
    }
    return nullptr;
}

dom::Element* last_element_child(dom::Node const& node)
{
    auto const& children = node.children();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if ((*it)->is_element())
            return static_cast<dom::Element*>(*it);
    }
    return nullptr;
}

dom::Element* next_element_sibling(dom::Node const& node)
{
    for (dom::Node* sibling = next_sibling_of(node); sibling; sibling = next_sibling_of(*sibling)) {
        if (sibling->is_element())
            return static_cast<dom::Element*>(sibling);
    }
    return nullptr;
}

dom::Element* previous_element_sibling(dom::Node const& node)
{
    for (dom::Node* sibling = node.previous_sibling(); sibling; sibling = sibling->previous_sibling()) {
        if (sibling->is_element())
            return static_cast<dom::Element*>(sibling);
    }
    return nullptr;
}

std::vector<dom::Node*> element_children(dom::Node const& node)
{
    std::vector<dom::Node*> out;
    for (dom::Node* child : node.children()) {
        if (child->is_element())
            out.push_back(child);
    }
    return out;
}

bool is_inclusive_ancestor(dom::Node const& ancestor, dom::Node const& node)
{
    for (dom::Node const* current = &node; current; current = current->parent()) {
        if (current == &ancestor)
            return true;
    }
    return false;
}

bool precedes_in_tree_order(dom::Node const& a, dom::Node const& b)
{
    std::vector<dom::Node const*> chain_a;
    for (dom::Node const* n = &a; n; n = n->parent())
        chain_a.push_back(n);
    std::vector<dom::Node const*> chain_b;
    for (dom::Node const* n = &b; n; n = n->parent())
        chain_b.push_back(n);
    // From the roots down to the first place the chains differ.
    std::size_t ia = chain_a.size();
    std::size_t ib = chain_b.size();
    while (ia > 0 && ib > 0 && chain_a[ia - 1] == chain_b[ib - 1]) {
        --ia;
        --ib;
    }
    if (ia == 0)
        return true; // a is an ancestor of b
    if (ib == 0)
        return false;
    dom::Node const* parent = chain_a[ia];
    auto const& siblings = parent->children();
    auto const pos_a = std::find(siblings.begin(), siblings.end(), chain_a[ia - 1]);
    auto const pos_b = std::find(siblings.begin(), siblings.end(), chain_b[ib - 1]);
    return pos_a < pos_b;
}

void collect_descendants(dom::Node& node, std::vector<dom::Node*>& out)
{
    for (dom::Node* child : node.children()) {
        out.push_back(child);
        collect_descendants(*child, out);
    }
}

dom::Node& content_container(dom::Element& element)
{
    if (element.is_html("template")) {
        if (!element.template_content())
            element.set_template_content(element.document().create<dom::DocumentFragment>());
        return *element.template_content();
    }
    return element;
}

dom::Element* element_by_id(dom::Node& root, std::string_view id)
{
    for (dom::Node* child : root.children()) {
        if (child->is_element()) {
            auto* element = static_cast<dom::Element*>(child);
            if (dom::Attr const* attribute = element->find_attribute("id")) {
                if (attribute->value == id)
                    return element;
            }
        }
        if (dom::Element* found = element_by_id(*child, id))
            return found;
    }
    return nullptr;
}

dom::Element* document_element(dom::Document& document)
{
    return first_element_child(document);
}

dom::Element* head_element(dom::Document& document)
{
    dom::Element* html = document_element(document);
    if (!html || !html->is_html("html"))
        return nullptr;
    for (dom::Node* child : html->children()) {
        if (child->is_element() && static_cast<dom::Element*>(child)->is_html("head"))
            return static_cast<dom::Element*>(child);
    }
    return nullptr;
}

dom::Element* body_element(dom::Document& document)
{
    dom::Element* html = document_element(document);
    if (!html || !html->is_html("html"))
        return nullptr;
    for (dom::Node* child : html->children()) {
        if (!child->is_element())
            continue;
        auto* element = static_cast<dom::Element*>(child);
        if (element->is_html("body") || element->is_html("frameset"))
            return element;
    }
    return nullptr;
}

std::string tag_name_of(dom::Element const& element)
{
    return element.is_html() ? ascii_upper(element.local_name()) : element.local_name();
}

std::string node_name_of(dom::Node const& node)
{
    switch (node.type()) {
    case dom::NodeType::Element:
        return tag_name_of(static_cast<dom::Element const&>(node));
    case dom::NodeType::Text:
        return static_cast<dom::Text const&>(node).cdata_section ? "#cdata-section" : "#text";
    case dom::NodeType::Comment:
        return "#comment";
    case dom::NodeType::ProcessingInstruction:
        return static_cast<dom::ProcessingInstruction const&>(node).target;
    case dom::NodeType::Document:
        return "#document";
    case dom::NodeType::DocumentFragment:
        return "#document-fragment";
    case dom::NodeType::DocumentType:
        return static_cast<dom::DocumentType const&>(node).name;
    }
    return "";
}

// --- Mutation ---------------------------------------------------------------------------

namespace {

void collect_scripts(dom::Node& node, std::vector<dom::Element*>& out)
{
    if (node.is_element()) {
        auto& element = static_cast<dom::Element&>(node);
        if (element.is_html("script"))
            out.push_back(&element);
    }
    for (dom::Node* child : node.children())
        collect_scripts(*child, out);
}

void insert_one(Realm::Internals& in, dom::Node& parent, dom::Node& node, dom::Node* reference)
{
    // A node moved out of a tree takes its frames down with it (the removing
    // steps), before it is adopted and inserted.
    if (node.parent())
        in.frames_removed(node);
    in.adopt_into(parent.document(), node);
    parent.insert_before(node, reference);
    in.realm.note_mutation();
    // A script element inserted into the document runs (§4.12.1, the
    // insertion steps), unless it was already started — a fragment's are;
    // an iframe inserted navigates, in a task after the script.
    if (parent.is_connected()) {
        std::vector<dom::Element*> scripts;
        collect_scripts(node, scripts);
        for (dom::Element* script : scripts)
            in.realm.run_inserted_script(*script);
        in.frames_inserted(node);
        custom_elements_inserted(in, node);
    }
}

} // namespace

Native ensure_pre_insertion_validity(Realm::Internals& in, dom::Node& parent, dom::Node& node, dom::Node* child, bool replacing)
{
    dom::NodeType const parent_type = parent.type();
    if (parent_type != dom::NodeType::Document && parent_type != dom::NodeType::DocumentFragment && parent_type != dom::NodeType::Element)
        return in.throw_dom_exception("HierarchyRequestError", "This node type does not support this method");
    if (is_inclusive_ancestor(node, parent))
        return in.throw_dom_exception("HierarchyRequestError", "The new child element contains the parent");
    if (child && child->parent() != &parent)
        return in.throw_dom_exception("NotFoundError", "The node before which the new node is to be inserted is not a child of this node");
    if (node.type() == dom::NodeType::Document)
        return in.throw_dom_exception("HierarchyRequestError", "Nodes of type '#document' may not be inserted inside nodes of this type");
    if (node.type() == dom::NodeType::Text && parent_type == dom::NodeType::Document)
        return in.throw_dom_exception("HierarchyRequestError", "Nodes of type '#text' may not be inserted inside nodes of type '#document'");
    if (node.type() == dom::NodeType::DocumentType && parent_type != dom::NodeType::Document)
        return in.throw_dom_exception("HierarchyRequestError", "Nodes of type 'DocumentType' may not be inserted inside nodes of this type");
    // A document holds one element and one doctype, the doctype first.
    if (parent_type == dom::NodeType::Document) {
        auto const& siblings = parent.children();
        auto const is_element = [](dom::Node const* candidate) { return candidate->is_element(); };
        auto const is_doctype = [](dom::Node const* candidate) { return candidate->type() == dom::NodeType::DocumentType; };
        // The child a replacement takes the place of does not count.
        auto const other_than_replaced = [&](dom::Node const* candidate) { return !(replacing && candidate == child); };
        bool const has_element = std::any_of(siblings.begin(), siblings.end(),
            [&](dom::Node const* candidate) { return is_element(candidate) && other_than_replaced(candidate); });
        auto const child_at = child ? std::find(siblings.begin(), siblings.end(), child) : siblings.end();
        bool const doctype_after_child = child && child_at != siblings.end() && std::any_of(child_at + 1, siblings.end(), is_doctype);
        bool const child_is_doctype = !replacing && child && child->type() == dom::NodeType::DocumentType;
        bool refused = false;
        switch (node.type()) {
        case dom::NodeType::DocumentFragment: {
            auto const& inserted = node.children();
            auto const elements = std::count_if(inserted.begin(), inserted.end(), is_element);
            bool const text = std::any_of(inserted.begin(), inserted.end(), [](dom::Node const* candidate) { return candidate->is_text(); });
            refused = elements > 1 || text || (elements == 1 && (has_element || child_is_doctype || doctype_after_child));
            break;
        }
        case dom::NodeType::Element:
            refused = has_element || child_is_doctype || doctype_after_child;
            break;
        case dom::NodeType::DocumentType:
            refused = std::any_of(siblings.begin(), siblings.end(),
                          [&](dom::Node const* candidate) { return is_doctype(candidate) && other_than_replaced(candidate); })
                || (child && std::any_of(siblings.begin(), child_at, is_element)) || (!child && has_element);
            break;
        default:
            break;
        }
        if (refused)
            return in.throw_dom_exception("HierarchyRequestError", "A document holds one element and one doctype, the doctype first");
    }
    return js::Value::undefined();
}

Native pre_insert(Realm::Internals& in, dom::Node& parent, dom::Node& node, dom::Node* child)
{
    if (!ensure_pre_insertion_validity(in, parent, node, child))
        return std::nullopt;
    dom::Node* reference = child;
    if (reference == &node)
        reference = next_sibling_of(node);
    // Rooted: an inserted script may run below, and a fragment's wrapper is
    // reachable from nothing else once its children have moved.
    js::Interpreter::Roots const roots(in.interpreter);
    js::Value const result = in.interpreter.root(js::Value::object(in.wrap(node)));
    if (node.type() == dom::NodeType::DocumentFragment) {
        std::vector<dom::Node*> const children = node.children();
        for (dom::Node* fragment_child : children)
            insert_one(in, parent, *fragment_child, reference);
        if (!children.empty())
            in.realm.note_mutation();
    } else {
        insert_one(in, parent, node, reference);
    }
    return result;
}

void remove_node(Realm::Internals& in, dom::Node& node)
{
    if (!node.parent())
        return;
    bool const was_connected = node.is_connected();
    in.frames_removed(node);
    node.remove();
    if (was_connected)
        custom_elements_removed(in, node);
    in.realm.note_mutation();
}

std::vector<dom::Node*> parse_markup(Realm::Internals& in, dom::Element& context, std::string_view markup, bool scripts_started)
{
    html::FragmentParseResult result = html::parse_fragment(decode_utf8(markup), context.namespace_uri(),
        context.local_name(), true);
    std::vector<dom::Node*> children;
    if (!result.root)
        return children;
    children = result.root->children();
    for (dom::Node* child : children) {
        // Scripts created by the fragment parser are already started and
        // never run (§13.4), unless the caller unmarks them.
        if (scripts_started) {
            std::vector<dom::Element*> scripts;
            collect_scripts(*child, scripts);
            for (dom::Element* script : scripts)
                in.started_scripts.insert(script);
        }
        context.document().adopt(*child); // detaches from the parse root too
    }
    return children;
}

void replace_children_with_markup(Realm::Internals& in, dom::Node& parent, dom::Element& context, std::string_view markup)
{
    std::vector<dom::Node*> const children = parse_markup(in, context, markup);
    std::vector<dom::Node*> const old = parent.children();
    bool const was_connected = parent.is_connected();
    for (dom::Node* child : old) {
        in.frames_removed(*child);
        child->remove();
        if (was_connected)
            custom_elements_removed(in, *child);
    }
    for (dom::Node* child : children)
        parent.append_child(*child);
    if (parent.is_connected()) {
        for (dom::Node* child : children) {
            in.frames_inserted(*child);
            custom_elements_inserted(in, *child);
        }
    }
    in.realm.note_mutation();
}

void replace_children_with_text(Realm::Internals& in, dom::Node& parent, std::string_view text)
{
    std::vector<dom::Node*> const old = parent.children();
    for (dom::Node* child : old) {
        in.frames_removed(*child);
        child->remove();
    }
    if (!text.empty()) {
        dom::Text* node = parent.document().create<dom::Text>();
        node->data = std::string(text);
        parent.append_child(*node);
    }
    in.realm.note_mutation();
}

void copy_started_scripts(Realm::Internals& in, dom::Node const& source, dom::Node const& clone)
{
    if (source.is_element() && clone.is_element()) {
        auto const& element = static_cast<dom::Element const&>(source);
        if ((element.is_html("script") || element.is_svg("script")) && !in.started_scripts.contains(static_cast<dom::Element const*>(&clone))) {
            // The realm that ran it is the one of the document it was in.
            Realm::Internals const* const home = in.realm_of(source.document());
            if (in.started_scripts.contains(&element) || (home != nullptr && home->started_scripts.contains(&element)))
                in.started_scripts.insert(static_cast<dom::Element const*>(&clone));
        }
    }
    std::vector<dom::Node*> const& from = source.children();
    std::vector<dom::Node*> const& to = clone.children();
    for (std::size_t i = 0; i < from.size() && i < to.size(); ++i)
        copy_started_scripts(in, *from[i], *to[i]);
}

namespace {

dom::Node* clone_node_alone(Realm::Internals& in, dom::Node const& node, bool deep);

}

dom::Node* clone_node(Realm::Internals& in, dom::Node const& node, bool deep)
{
    dom::Node* const clone = clone_node_alone(in, node, deep);
    if (clone != nullptr)
        copy_started_scripts(in, node, *clone);
    return clone;
}

namespace {

dom::Node* clone_node_alone(Realm::Internals& in, dom::Node const& node, bool deep)
{
    dom::Document& document = node.document();
    if (node.type() == dom::NodeType::Document) {
        auto const& source = static_cast<dom::Document const&>(node);
        auto clone = std::make_unique<dom::Document>();
        clone->quirks_mode = source.quirks_mode;
        clone->xml = source.xml;
        clone->content_type = &source == in.document ? in.document_content_type : source.content_type;
        if (deep) {
            for (dom::Node const* child : node.children())
                clone->append_child(*dom::clone_subtree(*child, *clone));
        }
        dom::Document* raw = clone.get();
        in.extra_documents.push_back(std::move(clone));
        return raw;
    }
    if (deep)
        return dom::clone_subtree(node, document);
    switch (node.type()) {
    case dom::NodeType::Element: {
        auto const& element = static_cast<dom::Element const&>(node);
        dom::Element* clone = document.create<dom::Element>(element.namespace_uri(), element.local_name());
        clone->attributes() = element.attributes();
        return clone;
    }
    case dom::NodeType::Text:
    case dom::NodeType::Comment:
    case dom::NodeType::ProcessingInstruction:
    case dom::NodeType::DocumentType:
        return dom::clone_subtree(node, document); // no children to leave out
    case dom::NodeType::DocumentFragment:
    case dom::NodeType::Document:
        break;
    }
    return document.create<dom::DocumentFragment>();
}

} // namespace

// --- Selectors ----------------------------------------------------------------------------

std::optional<css::SelectorList> parse_selector(Realm::Internals& in, std::string_view text)
{
    std::optional<css::SelectorList> list = css::parse_selector_list(css::parse_component_value_list(text));
    if (!list || list->selectors.empty()) {
        in.throw_dom_exception("SyntaxError", "'" + std::string(text) + "' is not a valid selector");
        return std::nullopt;
    }
    return list;
}

std::vector<dom::Node*> query_all(dom::Node& root, css::SelectorList const& list, bool first_only)
{
    std::vector<dom::Node*> out;
    std::vector<dom::Node*> descendants;
    collect_descendants(root, descendants);
    for (dom::Node* node : descendants) {
        if (!node->is_element())
            continue;
        if (css::matches(list, *static_cast<dom::Element*>(node))) {
            out.push_back(node);
            if (first_only)
                break;
        }
    }
    return out;
}

// --- Rects ---------------------------------------------------------------------------------

std::optional<LayoutBox> client_box(Realm::Internals& in, dom::Element const& element)
{
    if (!in.hooks.layout_box)
        return std::nullopt;
    std::optional<LayoutBox> box = in.hooks.layout_box(element);
    if (!box)
        return std::nullopt;
    if (in.hooks.scroll_position) {
        std::pair<int, int> const scroll = in.hooks.scroll_position(*in.document);
        box->x -= static_cast<float>(scroll.first);
        box->y -= static_cast<float>(scroll.second);
    }
    return box;
}

js::Value make_rect(Realm::Internals& in, LayoutBox const& box)
{
    return make_rect(in, static_cast<double>(box.x), static_cast<double>(box.y), static_cast<double>(box.width), static_cast<double>(box.height));
}

js::Value make_rect(Realm::Internals& in, double x, double y, double width, double height)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());
    js::Object* rect = interpreter.new_object(in.prototype("DOMRect"));
    rect->put(interpreter.key("x"), js::Value::number(x));
    rect->put(interpreter.key("y"), js::Value::number(y));
    rect->put(interpreter.key("width"), js::Value::number(width));
    rect->put(interpreter.key("height"), js::Value::number(height));
    return js::Value::object(rect);
}

// --- Argument helpers ---------------------------------------------------------------------

// A node argument, or a TypeError naming the parameter.
std::optional<dom::Node*> node_argument(Realm::Internals& in, Args args, std::size_t index, std::string_view method)
{
    js::Value const value = js::argument(args, index);
    NodeWrapper* wrapper = in.wrapper_of(value);
    if (!wrapper)
        return in.interpreter.throw_type_error("Failed to execute '" + std::string(method) + "': parameter "
            + std::to_string(index + 1) + " is not of type 'Node'");
    return &wrapper->node();
}

namespace {

// A node, or null/undefined.
std::optional<dom::Node*> node_or_null_argument(Realm::Internals& in, Args args, std::size_t index, std::string_view method)
{
    js::Value const value = js::argument(args, index);
    if (value.is_nullish())
        return nullptr;
    return node_argument(in, args, index, method);
}

// The (Node or DOMString)... arguments of the ParentNode and ChildNode
// methods, strings becoming Text nodes, gathered into one fragment.
std::optional<dom::Node*> nodes_argument(Realm::Internals& in, dom::Document& document, Args args)
{
    if (args.size() == 1) {
        if (NodeWrapper* wrapper = in.wrapper_of(args[0]))
            return &wrapper->node();
    }
    dom::DocumentFragment* fragment = document.create<dom::DocumentFragment>();
    for (js::Value const& value : args) {
        if (NodeWrapper* wrapper = in.wrapper_of(value)) {
            if (wrapper->node().parent())
                in.frames_removed(wrapper->node());
            in.adopt_into(document, wrapper->node());
            fragment->append_child(wrapper->node());
            continue;
        }
        std::optional<std::string> text = in.to_utf8(value);
        if (!text)
            return std::nullopt;
        dom::Text* node = document.create<dom::Text>();
        node->data = std::move(*text);
        fragment->append_child(*node);
    }
    return fragment;
}

std::optional<std::string> string_argument(Realm::Internals& in, Args args, std::size_t index)
{
    return in.to_utf8(js::argument(args, index));
}

// The Attr objects of an element as a NamedNodeMap-shaped array.
js::Value attribute_map(Realm::Internals& in, dom::Element& element)
{
    js::Interpreter& interpreter = in.interpreter;
    // Several cells per attribute, none reachable until the map holds them:
    // no collection while they are made.
    js::Heap::NoCollect const guard(interpreter.heap());
    js::ArrayObject* map = interpreter.new_array();
    map->set_prototype(in.prototype("NamedNodeMap"));
    map->host_data = &element;
    for (dom::Attr const& attribute : element.attributes()) {
        js::Object* attr = interpreter.heap().allocate<PlainPlatformObject>(in.prototype("Attr"));
        map->push(js::Value::object(attr));
        std::string const name = attribute.qualified_name();
        attr->put(interpreter.key("name"), in.string(name), js::Enumerable);
        attr->put(interpreter.key("localName"), in.string(attribute.local_name), js::Enumerable);
        attr->put(interpreter.key("value"), in.string(attribute.value), js::Enumerable | js::Writable);
        attr->put(interpreter.key("nodeName"), in.string(name), js::Enumerable);
        attr->put(interpreter.key("nodeValue"), in.string(attribute.value), js::Enumerable | js::Writable);
        attr->put(interpreter.key("textContent"), in.string(attribute.value), js::Enumerable | js::Writable);
        attr->put(interpreter.key("namespaceURI"), attribute.namespace_uri.empty() ? js::Value::null() : in.string(attribute.namespace_uri), js::Enumerable);
        attr->put(interpreter.key("prefix"), attribute.prefix.empty() ? js::Value::null() : in.string(attribute.prefix), js::Enumerable);
        attr->put(interpreter.key("specified"), js::Value::boolean(true), js::Enumerable);
        attr->put(interpreter.key("nodeType"), js::Value::number(2), js::Enumerable);
        attr->put(interpreter.key("ownerElement"), js::Value::object(in.wrap(element)), js::Enumerable);
        // WebIDL's named properties: the attribute reachable by its name
        // (jQuery 1.x reads `attributes[name].expando` in its support
        // tests), unless the map or its prototype already has that name,
        // and never as an index. Not enumerable, like every named property.
        bool const looks_like_index = !name.empty() && name.find_first_not_of("0123456789") == std::string::npos;
        if (!looks_like_index) {
            js::PropertyKey const named = interpreter.key(name);
            if (!map->has_property(named))
                map->put(named, js::Value::object(attr), js::Configurable);
        }
    }
    return js::Value::object(map);
}

bool is_valid_attribute_name(std::string_view name)
{
    if (name.empty())
        return false;
    for (char const c : name) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == '/' || c == '>' || c == '<' || c == '='
            || c == '"' || c == '\'' || c == '\0')
            return false;
    }
    return true;
}

// A valid namespace prefix and a valid attribute local name (DOM §1.4): at
// least one code unit, and none of them ASCII whitespace, U+0000, "/" or ">",
// nor "=" in a local name.
bool is_valid_name_part(std::string_view part, bool local_name)
{
    if (part.empty())
        return false;
    for (char const c : part) {
        if (c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ' || c == '\0' || c == '/' || c == '>' || (local_name && c == '='))
            return false;
    }
    return true;
}

// A valid element local name (DOM §1.4): one that starts with an ASCII letter
// may hold anything but ASCII whitespace, U+0000, "/" and ">"; any other
// starts with ":", "_" or a non-ASCII code point and goes on in letters,
// digits, "-", ".", ":", "_" and non-ASCII code points.
bool is_valid_element_local_name(std::string_view name)
{
    if (name.empty())
        return false;
    auto const ascii_alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    auto const non_ascii = [](char c) { return static_cast<unsigned char>(c) >= 0x80; };
    if (ascii_alpha(name[0]))
        return is_valid_name_part(name, false);
    if (name[0] != ':' && name[0] != '_' && !non_ascii(name[0]))
        return false;
    for (char const c : name.substr(1)) {
        if (!ascii_alpha(c) && !(c >= '0' && c <= '9') && c != '-' && c != '.' && c != ':' && c != '_' && !non_ascii(c))
            return false;
    }
    return true;
}

} // namespace

ExtractedName validate_and_extract(std::string namespace_uri, std::string const& qualified_name, bool element)
{
    ExtractedName out { std::move(namespace_uri), "", qualified_name, "" };
    std::size_t const colon = qualified_name.find(':');
    if (colon != std::string::npos) {
        out.prefix = qualified_name.substr(0, colon);
        out.local_name = qualified_name.substr(colon + 1);
        if (!is_valid_name_part(out.prefix, false)) {
            out.error = "InvalidCharacterError";
            return out;
        }
    }
    bool const xmlns_name = qualified_name == "xmlns" || out.prefix == "xmlns";
    if (!(element ? is_valid_element_local_name(out.local_name) : is_valid_name_part(out.local_name, true)))
        out.error = "InvalidCharacterError";
    else if (!out.prefix.empty() && out.namespace_uri.empty())
        out.error = "NamespaceError";
    else if (out.prefix == "xml" && out.namespace_uri != dom::ns::xml)
        out.error = "NamespaceError";
    // The name or prefix xmlns belongs to the XMLNS namespace, and that
    // namespace to nothing else.
    else if (xmlns_name != (out.namespace_uri == dom::ns::xmlns))
        out.error = "NamespaceError";
    return out;
}

namespace {

// A namespace argument: null and the empty string are no namespace, "".
std::optional<std::string> namespace_argument(Realm::Internals& in, Args args, std::size_t index)
{
    js::Value const value = js::argument(args, index);
    return value.is_nullish() ? std::optional<std::string>("") : in.to_utf8(value);
}

// The insertAdjacent* positions (§4.9 "insert adjacent").
std::optional<std::pair<dom::Node*, dom::Node*>> adjacent_position(Realm::Internals& in, dom::Element& element,
    std::string_view where, std::string_view method)
{
    std::string const position = ascii_lower(where);
    if (position == "beforebegin") {
        if (!element.parent())
            return std::pair<dom::Node*, dom::Node*> { nullptr, nullptr };
        return std::pair<dom::Node*, dom::Node*> { element.parent(), &element };
    }
    if (position == "afterbegin")
        return std::pair<dom::Node*, dom::Node*> { &element, element.children().empty() ? nullptr : element.children().front() };
    if (position == "beforeend")
        return std::pair<dom::Node*, dom::Node*> { &element, nullptr };
    if (position == "afterend") {
        if (!element.parent())
            return std::pair<dom::Node*, dom::Node*> { nullptr, nullptr };
        return std::pair<dom::Node*, dom::Node*> { element.parent(), next_sibling_of(element) };
    }
    in.throw_dom_exception("SyntaxError", "Failed to execute '" + std::string(method) + "': The value provided ('"
        + std::string(where) + "') is not one of 'beforeBegin', 'afterBegin', 'beforeEnd', or 'afterEnd'.");
    return std::nullopt;
}

} // namespace

bool is_character_data(dom::Node const& node)
{
    return node.is_text() || node.type() == dom::NodeType::Comment || node.type() == dom::NodeType::ProcessingInstruction;
}

std::string const& character_data(dom::Node const& node)
{
    if (node.is_text())
        return static_cast<dom::Text const&>(node).data;
    if (node.type() == dom::NodeType::ProcessingInstruction)
        return static_cast<dom::ProcessingInstruction const&>(node).data;
    return static_cast<dom::Comment const&>(node).data;
}

std::u16string data_units(dom::Node const& node)
{
    return js::utf16_from_utf8(character_data(node));
}

void replace_data(Realm::Internals& in, dom::Node& node, std::size_t offset, std::size_t count, std::u16string_view data)
{
    std::u16string units = data_units(node);
    count = std::min(count, units.size() - offset);
    units.replace(offset, count, data);
    std::string utf8 = js::utf8_from_utf16(units);
    if (node.is_text())
        static_cast<dom::Text&>(node).data = std::move(utf8);
    else if (node.type() == dom::NodeType::ProcessingInstruction)
        static_cast<dom::ProcessingInstruction&>(node).data = std::move(utf8);
    else
        static_cast<dom::Comment&>(node).data = std::move(utf8);
    dom::ranges_data_replaced(node, static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(data.size()));
    in.realm.note_mutation();
}

void set_data(Realm::Internals& in, dom::Node& node, std::u16string_view units)
{
    replace_data(in, node, 0, data_units(node).size(), units);
}

dom::Text* split_text(Realm::Internals& in, dom::Text& node, std::size_t offset)
{
    std::u16string const units = data_units(node);
    dom::Text* rest = node.document().create<dom::Text>();
    rest->cdata_section = node.cdata_section;
    rest->data = js::utf8_from_utf16(std::u16string_view(units).substr(offset));
    if (dom::Node* parent = node.parent()) {
        parent->insert_before(*rest, next_sibling_of(node));
        dom::ranges_text_split(node, static_cast<std::uint32_t>(offset), *rest);
    }
    replace_data(in, node, offset, units.size() - offset, u"");
    return rest;
}

namespace {

// The GlobalEventHandlers set every element and the document expose.
constexpr std::string_view global_event_types[] = {
    "abort", "animationend", "animationiteration", "animationstart", "auxclick", "beforeinput", "blur", "cancel", "canplay",
    "canplaythrough", "change", "click", "close", "contextmenu", "copy", "cuechange", "cut", "dblclick", "drag", "dragend",
    "dragenter", "dragleave", "dragover", "dragstart", "drop", "durationchange", "emptied", "ended", "error", "focus",
    "focusin", "focusout", "formdata", "input", "invalid", "keydown", "keypress", "keyup", "load", "loadeddata",
    "loadedmetadata", "loadstart", "mousedown", "mouseenter", "mouseleave", "mousemove", "mouseout", "mouseover",
    "mouseup", "paste", "pause", "play", "playing", "pointercancel", "pointerdown", "pointerenter", "pointerleave",
    "pointermove", "pointerout", "pointerover", "pointerup", "progress", "ratechange", "reset", "resize", "scroll",
    "scrollend", "securitypolicyviolation", "seeked", "seeking", "select", "selectionchange", "selectstart",
    "slotchange", "stalled", "submit", "suspend", "timeupdate", "toggle", "touchcancel", "touchend", "touchmove",
    "touchstart", "transitioncancel", "transitionend", "transitionrun", "transitionstart", "volumechange", "waiting",
    "wheel"
};

// --- Node ----------------------------------------------------------------------------------

void install_node(Realm::Internals& in, js::Object& node)
{
    js::Interpreter& interpreter = in.interpreter;
    node_getter(in, node, "nodeType", [](Realm::Internals&, dom::Node& n) -> Native {
        switch (n.type()) {
        case dom::NodeType::Element: return js::Value::number(1);
        case dom::NodeType::Text: return js::Value::number(static_cast<dom::Text&>(n).cdata_section ? 4 : 3);
        case dom::NodeType::ProcessingInstruction: return js::Value::number(7);
        case dom::NodeType::Comment: return js::Value::number(8);
        case dom::NodeType::Document: return js::Value::number(9);
        case dom::NodeType::DocumentType: return js::Value::number(10);
        case dom::NodeType::DocumentFragment: return js::Value::number(11);
        }
        return js::Value::number(0);
    });
    node_getter(in, node, "nodeName", [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.string(node_name_of(n)); });
    node_accessor(
        in, node, "nodeValue",
        [](Realm::Internals& internals, dom::Node& n) -> Native {
            if (is_character_data(n))
                return internals.string(character_data(n));
            return js::Value::null();
        },
        [](Realm::Internals& internals, dom::Node& n, js::Value const& value) -> Native {
            if (!is_character_data(n))
                return js::Value::undefined();
            std::optional<std::string> text = value.is_nullish() ? std::optional<std::string>("") : internals.to_utf8(value);
            if (!text)
                return std::nullopt;
            set_data(internals, n, js::utf16_from_utf8(*text));
            return js::Value::undefined();
        });
    node_accessor(
        in, node, "textContent",
        [](Realm::Internals& internals, dom::Node& n) -> Native {
            switch (n.type()) {
            case dom::NodeType::Document:
            case dom::NodeType::DocumentType:
                return js::Value::null();
            case dom::NodeType::Text:
            case dom::NodeType::Comment:
            case dom::NodeType::ProcessingInstruction:
                return internals.string(character_data(n));
            case dom::NodeType::Element:
            case dom::NodeType::DocumentFragment:
                break;
            }
            return internals.string(html::text_content(n.is_element() ? content_container(static_cast<dom::Element&>(n)) : n));
        },
        [](Realm::Internals& internals, dom::Node& n, js::Value const& value) -> Native {
            std::optional<std::string> text = value.is_nullish() ? std::optional<std::string>("") : internals.to_utf8(value);
            if (!text)
                return std::nullopt;
            switch (n.type()) {
            case dom::NodeType::Text:
            case dom::NodeType::Comment:
            case dom::NodeType::ProcessingInstruction:
                set_data(internals, n, js::utf16_from_utf8(*text));
                break;
            case dom::NodeType::Element:
                replace_children_with_text(internals, content_container(static_cast<dom::Element&>(n)), *text);
                break;
            case dom::NodeType::DocumentFragment:
                replace_children_with_text(internals, n, *text);
                break;
            case dom::NodeType::Document:
            case dom::NodeType::DocumentType:
                break;
            }
            return js::Value::undefined();
        });
    node_getter(in, node, "parentNode", [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.realm.wrap_or_null(n.parent()); });
    node_getter(in, node, "parentElement", [](Realm::Internals& internals, dom::Node& n) -> Native {
        dom::Node* parent = n.parent();
        return internals.realm.wrap_or_null(parent && parent->is_element() ? parent : nullptr);
    });
    node_getter(in, node, "childNodes", [](Realm::Internals& internals, dom::Node& n) -> Native {
        dom::Node& container = n.is_element() ? content_container(static_cast<dom::Element&>(n)) : n;
        return node_list(internals, container.children());
    });
    node_getter(in, node, "firstChild", [](Realm::Internals& internals, dom::Node& n) -> Native {
        return internals.realm.wrap_or_null(n.children().empty() ? nullptr : n.children().front());
    });
    node_getter(in, node, "lastChild", [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.realm.wrap_or_null(n.last_child()); });
    node_getter(in, node, "previousSibling", [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.realm.wrap_or_null(n.previous_sibling()); });
    node_getter(in, node, "nextSibling", [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.realm.wrap_or_null(next_sibling_of(n)); });
    node_getter(in, node, "ownerDocument", [](Realm::Internals& internals, dom::Node& n) -> Native {
        if (n.type() == dom::NodeType::Document)
            return js::Value::null();
        return js::Value::object(internals.wrap(n.document()));
    });
    node_getter(in, node, "isConnected", [](Realm::Internals& internals, dom::Node& n) -> Native {
        return js::Value::boolean(&n.root() == internals.document || (n.root().type() == dom::NodeType::Document && &n.root() != internals.document));
    });
    node_getter(in, node, "baseURI", [](Realm::Internals& internals, dom::Node&) -> Native { return internals.string(internals.base_url().serialize()); });
    node_method(in, node, "hasChildNodes", 0, [](Realm::Internals&, dom::Node& n, Args) -> Native { return js::Value::boolean(!n.children().empty()); });
    node_method(in, node, "getRootNode", 0, [](Realm::Internals& internals, dom::Node& n, Args) -> Native { return js::Value::object(internals.wrap(n.root())); });
    node_method(in, node, "appendChild", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<dom::Node*> const child = node_argument(internals, args, 0, "appendChild");
        if (!child)
            return std::nullopt;
        return pre_insert(internals, n.is_element() ? content_container(static_cast<dom::Element&>(n)) : n, **child, nullptr);
    });
    node_method(in, node, "insertBefore", 2, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<dom::Node*> const child = node_argument(internals, args, 0, "insertBefore");
        if (!child)
            return std::nullopt;
        std::optional<dom::Node*> const reference = node_or_null_argument(internals, args, 1, "insertBefore");
        if (!reference)
            return std::nullopt;
        return pre_insert(internals, n, **child, *reference);
    });
    node_method(in, node, "removeChild", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<dom::Node*> const child = node_argument(internals, args, 0, "removeChild");
        if (!child)
            return std::nullopt;
        if ((*child)->parent() != &n)
            return internals.throw_dom_exception("NotFoundError", "The node to be removed is not a child of this node");
        js::Value const result = js::Value::object(internals.wrap(**child));
        remove_node(internals, **child);
        return result;
    });
    node_method(in, node, "replaceChild", 2, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<dom::Node*> const replacement = node_argument(internals, args, 0, "replaceChild");
        std::optional<dom::Node*> const child = node_argument(internals, args, 1, "replaceChild");
        if (!replacement || !child)
            return std::nullopt;
        // Everything is checked before the child is removed (DOM §4.2.3
        // "replace").
        if (!ensure_pre_insertion_validity(internals, n, **replacement, *child, true))
            return std::nullopt;
        js::Interpreter::Roots const roots(internals.interpreter);
        js::Value const result = internals.interpreter.root(js::Value::object(internals.wrap(**child)));
        dom::Node* const reference = next_sibling_of(**child) == *replacement ? next_sibling_of(**replacement) : next_sibling_of(**child);
        remove_node(internals, **child);
        if (!pre_insert(internals, n, **replacement, reference))
            return std::nullopt;
        return result;
    });
    node_method(in, node, "cloneNode", 0, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        dom::Node* clone = clone_node(internals, n, js::Interpreter::to_boolean(js::argument(args, 0)));
        return js::Value::object(internals.wrap(*clone));
    });
    node_method(in, node, "contains", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<dom::Node*> const other = node_or_null_argument(internals, args, 0, "contains");
        if (!other)
            return std::nullopt;
        return js::Value::boolean(*other && is_inclusive_ancestor(n, **other));
    });
    node_method(in, node, "isSameNode", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        return js::Value::boolean(internals.realm.node_of(js::argument(args, 0)) == &n);
    });
    node_method(in, node, "isEqualNode", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        dom::Node* other = internals.realm.node_of(js::argument(args, 0));
        if (!other)
            return js::Value::boolean(false);
        return js::Value::boolean(other->type() == n.type() && node_name_of(*other) == node_name_of(n)
            && html::serialize_node(*other) == html::serialize_node(n));
    });
    node_method(in, node, "compareDocumentPosition", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<dom::Node*> const other = node_argument(internals, args, 0, "compareDocumentPosition");
        if (!other)
            return std::nullopt;
        dom::Node& o = **other;
        if (&o == &n)
            return js::Value::number(0);
        // Disconnected and implementation-specific, and preceding or following
        // by the roots' addresses, so the answer for a pair of trees is the
        // same for all their nodes and the other way round from the other one.
        if (&o.root() != &n.root())
            return js::Value::number(1 | 32 | (std::less<dom::Node const*> {}(&o.root(), &n.root()) ? 2 : 4));
        if (is_inclusive_ancestor(o, n))
            return js::Value::number(8 | 2); // contains, preceding
        if (is_inclusive_ancestor(n, o))
            return js::Value::number(16 | 4); // contained by, following
        return js::Value::number(precedes_in_tree_order(o, n) ? 2 : 4);
    });
    node_method(in, node, "normalize", 0, [](Realm::Internals& internals, dom::Node& n, Args) -> Native {
        // Only exclusive Text nodes are folded: a CDATA section stays (DOM
        // §4.4). An empty one goes; any other takes in the data of those
        // after it, and the live ranges in them.
        auto const exclusive_text = [](dom::Node const& candidate) {
            return candidate.is_text() && !static_cast<dom::Text const&>(candidate).cdata_section;
        };
        std::vector<dom::Node*> descendants;
        collect_descendants(n, descendants);
        for (dom::Node* node_ptr : descendants) {
            if (!exclusive_text(*node_ptr) || !node_ptr->parent())
                continue;
            auto& text = static_cast<dom::Text&>(*node_ptr);
            std::size_t length = data_units(text).size();
            if (length == 0) {
                remove_node(internals, text);
                continue;
            }
            std::vector<dom::Node*> following;
            std::u16string folded;
            for (dom::Node* next = next_sibling_of(text); next && exclusive_text(*next); next = next_sibling_of(*next)) {
                following.push_back(next);
                folded += data_units(*next);
            }
            if (following.empty())
                continue;
            replace_data(internals, text, length, 0, folded);
            for (dom::Node* merged : following) {
                dom::ranges_text_merged(text, *merged, static_cast<std::uint32_t>(length));
                length += data_units(*merged).size();
            }
            for (dom::Node* merged : following)
                remove_node(internals, *merged);
        }
        internals.realm.note_mutation();
        return js::Value::undefined();
    });
    node_method(in, node, "lookupNamespaceURI", 1, [](Realm::Internals&, dom::Node&, Args) -> Native { return js::Value::null(); });
    node_method(in, node, "isDefaultNamespace", 1, [](Realm::Internals&, dom::Node&, Args) -> Native { return js::Value::boolean(true); });
    for (auto const& [name, value] : { std::pair { "ELEMENT_NODE", 1 }, std::pair { "ATTRIBUTE_NODE", 2 }, std::pair { "TEXT_NODE", 3 },
             std::pair { "CDATA_SECTION_NODE", 4 }, std::pair { "ENTITY_REFERENCE_NODE", 5 }, std::pair { "ENTITY_NODE", 6 },
             std::pair { "PROCESSING_INSTRUCTION_NODE", 7 }, std::pair { "COMMENT_NODE", 8 }, std::pair { "DOCUMENT_NODE", 9 },
             std::pair { "DOCUMENT_TYPE_NODE", 10 }, std::pair { "DOCUMENT_FRAGMENT_NODE", 11 }, std::pair { "NOTATION_NODE", 12 },
             std::pair { "DOCUMENT_POSITION_DISCONNECTED", 1 }, std::pair { "DOCUMENT_POSITION_PRECEDING", 2 },
             std::pair { "DOCUMENT_POSITION_FOLLOWING", 4 }, std::pair { "DOCUMENT_POSITION_CONTAINS", 8 },
             std::pair { "DOCUMENT_POSITION_CONTAINED_BY", 16 }, std::pair { "DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC", 32 } }) {
        node.put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        std::optional<js::Value> const constructor = node.get(interpreter, interpreter.key("constructor"), js::Value::object(&node));
        if (constructor && constructor->is_object())
            constructor->as_object()->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }
}

// --- ParentNode and ChildNode mixins --------------------------------------------------------

void install_parent_node(Realm::Internals& in, js::Object& proto)
{
    node_getter(in, proto, "children", [](Realm::Internals& internals, dom::Node& n) -> Native {
        dom::Node& container = n.is_element() ? content_container(static_cast<dom::Element&>(n)) : n;
        js::Interpreter::Roots const roots(internals.interpreter);
        js::Value const list = internals.interpreter.root(node_list(internals, element_children(container)));
        list.as_object()->set_prototype(internals.prototype("HTMLCollection"));
        return list;
    });
    node_getter(in, proto, "childElementCount", [](Realm::Internals&, dom::Node& n) -> Native {
        return js::Value::number(static_cast<double>(element_children(n).size()));
    });
    node_getter(in, proto, "firstElementChild", [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.realm.wrap_or_null(first_element_child(n)); });
    node_getter(in, proto, "lastElementChild", [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.realm.wrap_or_null(last_element_child(n)); });
    node_method(in, proto, "append", 0, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<dom::Node*> const nodes = nodes_argument(internals, n.document(), args);
        if (!nodes)
            return std::nullopt;
        if (!pre_insert(internals, n, **nodes, nullptr))
            return std::nullopt;
        return js::Value::undefined();
    });
    node_method(in, proto, "prepend", 0, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<dom::Node*> const nodes = nodes_argument(internals, n.document(), args);
        if (!nodes)
            return std::nullopt;
        if (!pre_insert(internals, n, **nodes, n.children().empty() ? nullptr : n.children().front()))
            return std::nullopt;
        return js::Value::undefined();
    });
    node_method(in, proto, "replaceChildren", 0, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<dom::Node*> const nodes = nodes_argument(internals, n.document(), args);
        if (!nodes)
            return std::nullopt;
        std::vector<dom::Node*> const old = n.children();
        for (dom::Node* child : old) {
            internals.frames_removed(*child);
            child->remove();
        }
        if (!pre_insert(internals, n, **nodes, nullptr))
            return std::nullopt;
        return js::Value::undefined();
    });
    node_method(in, proto, "querySelector", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<std::string> const text = string_argument(internals, args, 0);
        if (!text)
            return std::nullopt;
        std::optional<css::SelectorList> const list = parse_selector(internals, *text);
        if (!list)
            return std::nullopt;
        std::vector<dom::Node*> const found = query_all(n, *list, true);
        return internals.realm.wrap_or_null(found.empty() ? nullptr : found.front());
    });
    node_method(in, proto, "querySelectorAll", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<std::string> const text = string_argument(internals, args, 0);
        if (!text)
            return std::nullopt;
        std::optional<css::SelectorList> const list = parse_selector(internals, *text);
        if (!list)
            return std::nullopt;
        return node_list(internals, query_all(n, *list, false));
    });
    node_method(in, proto, "getElementsByTagName", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<std::string> const name = string_argument(internals, args, 0);
        if (!name)
            return std::nullopt;
        std::string const lower = ascii_lower(*name);
        std::vector<dom::Node*> descendants;
        collect_descendants(n, descendants);
        std::vector<dom::Node*> found;
        for (dom::Node* d : descendants) {
            if (!d->is_element())
                continue;
            auto& element = static_cast<dom::Element&>(*d);
            if (*name == "*" || (element.is_html() ? element.local_name() == lower : element.local_name() == *name))
                found.push_back(d);
        }
        js::Interpreter::Roots const roots(internals.interpreter);
        js::Value const list = internals.interpreter.root(node_list(internals, found));
        list.as_object()->set_prototype(internals.prototype("HTMLCollection"));
        return list;
    });
    node_method(in, proto, "getElementsByTagNameNS", 2, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<std::string> const name = string_argument(internals, args, 1);
        if (!name)
            return std::nullopt;
        std::vector<dom::Node*> descendants;
        collect_descendants(n, descendants);
        std::vector<dom::Node*> found;
        for (dom::Node* d : descendants) {
            if (d->is_element() && (*name == "*" || static_cast<dom::Element&>(*d).local_name() == *name))
                found.push_back(d);
        }
        return node_list(internals, found);
    });
    node_method(in, proto, "getElementsByClassName", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<std::string> const names = string_argument(internals, args, 0);
        if (!names)
            return std::nullopt;
        std::vector<std::string> const wanted = split_tokens(*names);
        std::vector<dom::Node*> descendants;
        collect_descendants(n, descendants);
        std::vector<dom::Node*> found;
        if (!wanted.empty()) {
            for (dom::Node* d : descendants) {
                if (!d->is_element())
                    continue;
                std::vector<std::string> const classes = split_tokens(attribute_or_empty(static_cast<dom::Element&>(*d), "class"));
                bool all = true;
                for (std::string const& w : wanted) {
                    if (std::find(classes.begin(), classes.end(), w) == classes.end()) {
                        all = false;
                        break;
                    }
                }
                if (all)
                    found.push_back(d);
            }
        }
        js::Interpreter::Roots const roots(internals.interpreter);
        js::Value const list = internals.interpreter.root(node_list(internals, found));
        list.as_object()->set_prototype(internals.prototype("HTMLCollection"));
        return list;
    });
}

void install_child_node(Realm::Internals& in, js::Object& proto)
{
    node_method(in, proto, "remove", 0, [](Realm::Internals& internals, dom::Node& n, Args) -> Native {
        remove_node(internals, n);
        return js::Value::undefined();
    });
    node_method(in, proto, "before", 0, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        dom::Node* parent = n.parent();
        if (!parent)
            return js::Value::undefined();
        std::optional<dom::Node*> const nodes = nodes_argument(internals, n.document(), args);
        if (!nodes)
            return std::nullopt;
        // The reference is the first preceding sibling not among the nodes.
        dom::Node* reference = &n;
        if (!pre_insert(internals, *parent, **nodes, reference))
            return std::nullopt;
        return js::Value::undefined();
    });
    node_method(in, proto, "after", 0, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        dom::Node* parent = n.parent();
        if (!parent)
            return js::Value::undefined();
        std::optional<dom::Node*> const nodes = nodes_argument(internals, n.document(), args);
        if (!nodes)
            return std::nullopt;
        if (!pre_insert(internals, *parent, **nodes, next_sibling_of(n)))
            return std::nullopt;
        return js::Value::undefined();
    });
    node_method(in, proto, "replaceWith", 0, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        dom::Node* parent = n.parent();
        if (!parent)
            return js::Value::undefined();
        std::optional<dom::Node*> const nodes = nodes_argument(internals, n.document(), args);
        if (!nodes)
            return std::nullopt;
        dom::Node* reference = next_sibling_of(n);
        remove_node(internals, n);
        if (!pre_insert(internals, *parent, **nodes, reference))
            return std::nullopt;
        return js::Value::undefined();
    });
}

// --- Element ----------------------------------------------------------------------------------

void install_element(Realm::Internals& in, js::Object& element)
{
    install_parent_node(in, element);
    install_child_node(in, element);
    element_getter(in, element, "tagName", [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(tag_name_of(e)); });
    element_getter(in, element, "localName", [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(e.local_name()); });
    element_getter(in, element, "namespaceURI", [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(e.namespace_uri()); });
    element_getter(in, element, "prefix", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::null(); });
    reflect_string(in, element, "id", "id");
    reflect_string(in, element, "className", "class");
    reflect_string(in, element, "slot", "slot");
    element_getter(in, element, "classList", [](Realm::Internals& internals, dom::Element& e) -> Native { return make_token_list(internals, e, "class"); });
    element_getter(in, element, "part", [](Realm::Internals& internals, dom::Element& e) -> Native { return make_token_list(internals, e, "part"); });
    element_getter(in, element, "attributes", [](Realm::Internals& internals, dom::Element& e) -> Native { return attribute_map(internals, e); });
    element_getter(in, element, "shadowRoot", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::null(); });
    element_getter(in, element, "assignedSlot", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::null(); });
    element_method(in, element, "hasAttributes", 0, [](Realm::Internals&, dom::Element& e, Args) -> Native { return js::Value::boolean(!e.attributes().empty()); });
    element_method(in, element, "getAttributeNames", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        js::Interpreter::Roots const roots(internals.interpreter);
        js::ArrayObject* names = internals.interpreter.new_array();
        internals.interpreter.root(js::Value::object(names));
        for (dom::Attr const& attribute : e.attributes())
            names->push(internals.string(attribute.qualified_name()));
        return js::Value::object(names);
    });
    // The attribute methods (DOM §4.9): those without NS take a qualified
    // name, the first attribute with it in any namespace; the NS forms a
    // namespace and a local name.
    element_method(in, element, "getAttribute", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const name = string_argument(internals, args, 0);
        if (!name)
            return std::nullopt;
        dom::Attr const* attribute = e.find_attribute(e.is_html() ? ascii_lower(*name) : *name);
        return attribute ? internals.string(attribute->value) : js::Value::null();
    });
    element_method(in, element, "getAttributeNS", 2, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const namespace_uri = namespace_argument(internals, args, 0);
        if (!namespace_uri)
            return std::nullopt;
        std::optional<std::string> const name = string_argument(internals, args, 1);
        if (!name)
            return std::nullopt;
        for (dom::Attr const& attribute : e.attributes()) {
            if (attribute.namespace_uri == *namespace_uri && attribute.local_name == *name)
                return internals.string(attribute.value);
        }
        return js::Value::null();
    });
    auto const set_attribute_native = [](Realm::Internals& internals, dom::Element& e, Args args, std::size_t name_index) -> Native {
        std::optional<std::string> const name = string_argument(internals, args, name_index);
        std::optional<std::string> value = string_argument(internals, args, name_index + 1);
        if (!name || !value)
            return std::nullopt;
        if (!is_valid_attribute_name(*name))
            return internals.throw_dom_exception("InvalidCharacterError", "'" + *name + "' is not a valid attribute name");
        set_attribute(internals, e, e.is_html() ? ascii_lower(*name) : *name, std::move(*value));
        return js::Value::undefined();
    };
    element_method(in, element, "setAttribute", 2, [set_attribute_native](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        return set_attribute_native(internals, e, args, 0);
    });
    // The qualified name validated and split, the attribute of that namespace
    // and local name takes the value, or one is appended with the prefix.
    element_method(in, element, "setAttributeNS", 3, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> namespace_uri = namespace_argument(internals, args, 0);
        if (!namespace_uri)
            return std::nullopt;
        std::optional<std::string> const qualified_name = string_argument(internals, args, 1);
        if (!qualified_name)
            return std::nullopt;
        std::optional<std::string> value = string_argument(internals, args, 2);
        if (!value)
            return std::nullopt;
        ExtractedName const name = validate_and_extract(std::move(*namespace_uri), *qualified_name);
        if (!name.error.empty())
            return internals.throw_dom_exception(name.error, "'" + *qualified_name + "' is not a valid attribute name in the namespace '" + name.namespace_uri + "'");
        set_attribute_ns(internals, e, name.namespace_uri, name.prefix, name.local_name, std::move(*value));
        return js::Value::undefined();
    });
    element_method(in, element, "removeAttribute", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const name = string_argument(internals, args, 0);
        if (!name)
            return std::nullopt;
        remove_attribute(internals, e, e.is_html() ? ascii_lower(*name) : *name);
        return js::Value::undefined();
    });
    element_method(in, element, "removeAttributeNS", 2, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const namespace_uri = namespace_argument(internals, args, 0);
        if (!namespace_uri)
            return std::nullopt;
        std::optional<std::string> const name = string_argument(internals, args, 1);
        if (!name)
            return std::nullopt;
        remove_attribute_ns(internals, e, *namespace_uri, *name);
        return js::Value::undefined();
    });
    element_method(in, element, "hasAttribute", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const name = string_argument(internals, args, 0);
        if (!name)
            return std::nullopt;
        return js::Value::boolean(e.has_attribute(e.is_html() ? ascii_lower(*name) : *name));
    });
    element_method(in, element, "hasAttributeNS", 2, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const namespace_uri = namespace_argument(internals, args, 0);
        if (!namespace_uri)
            return std::nullopt;
        std::optional<std::string> const name = string_argument(internals, args, 1);
        if (!name)
            return std::nullopt;
        for (dom::Attr const& attribute : e.attributes()) {
            if (attribute.namespace_uri == *namespace_uri && attribute.local_name == *name)
                return js::Value::boolean(true);
        }
        return js::Value::boolean(false);
    });
    element_method(in, element, "toggleAttribute", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const name = string_argument(internals, args, 0);
        if (!name)
            return std::nullopt;
        if (!is_valid_attribute_name(*name))
            return internals.throw_dom_exception("InvalidCharacterError", "'" + *name + "' is not a valid attribute name");
        std::string const lower = e.is_html() ? ascii_lower(*name) : *name;
        bool const present = e.has_attribute(lower);
        js::Value const force = js::argument(args, 1);
        bool const want = force.is_undefined() ? !present : js::Interpreter::to_boolean(force);
        if (want && !present)
            set_attribute(internals, e, lower, "");
        else if (!want && present)
            remove_attribute(internals, e, lower);
        return js::Value::boolean(want);
    });
    element_method(in, element, "getAttributeNode", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const name = string_argument(internals, args, 0);
        if (!name)
            return std::nullopt;
        std::string const lower = e.is_html() ? ascii_lower(*name) : *name;
        js::Interpreter::Roots const roots(internals.interpreter);
        js::Value const map = internals.interpreter.root(attribute_map(internals, e));
        auto* array = static_cast<js::ArrayObject*>(map.as_object());
        for (std::size_t i = 0; i < e.attributes().size(); ++i) {
            if (e.attributes()[i].has_qualified_name(lower))
                return array->element(static_cast<std::uint32_t>(i));
        }
        return js::Value::null();
    });
    element_accessor(
        in, element, "innerHTML",
        [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(html::serialize_children(content_container(e))); },
        [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
            std::optional<std::string> const markup = value.is_nullish() ? std::optional<std::string>("") : internals.to_utf8(value);
            if (!markup)
                return std::nullopt;
            replace_children_with_markup(internals, content_container(e), e, *markup);
            return js::Value::undefined();
        });
    element_accessor(
        in, element, "outerHTML",
        [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(html::serialize_node(e)); },
        [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
            std::optional<std::string> const markup = value.is_nullish() ? std::optional<std::string>("") : internals.to_utf8(value);
            if (!markup)
                return std::nullopt;
            dom::Node* parent = e.parent();
            if (!parent)
                return js::Value::undefined();
            if (parent->type() == dom::NodeType::Document)
                return internals.throw_dom_exception("NoModificationAllowedError", "This element has no parent element");
            dom::Element* context = parent->is_element() ? static_cast<dom::Element*>(parent) : body_element(*internals.document);
            if (!context)
                context = internals.document->create<dom::Element>(std::string(dom::ns::html), "body");
            std::vector<dom::Node*> const children = parse_markup(internals, *context, *markup);
            dom::Node* reference = next_sibling_of(e);
            internals.frames_removed(e);
            e.remove();
            for (dom::Node* child : children)
                insert_one(internals, *parent, *child, reference);
            internals.realm.note_mutation();
            return js::Value::undefined();
        });
    element_method(in, element, "insertAdjacentHTML", 2, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const where = string_argument(internals, args, 0);
        std::optional<std::string> const markup = string_argument(internals, args, 1);
        if (!where || !markup)
            return std::nullopt;
        auto const position = adjacent_position(internals, e, *where, "insertAdjacentHTML");
        if (!position)
            return std::nullopt;
        dom::Node* const parent = position->first;
        if (!parent)
            return js::Value::undefined();
        if (parent->type() == dom::NodeType::Document)
            return internals.throw_dom_exception("NoModificationAllowedError", "The element has no parent");
        dom::Element* context = parent->is_element() ? static_cast<dom::Element*>(parent) : &e;
        std::vector<dom::Node*> const children = parse_markup(internals, *context, *markup);
        for (dom::Node* child : children)
            insert_one(internals, *parent, *child, position->second);
        return js::Value::undefined();
    });
    element_method(in, element, "insertAdjacentElement", 2, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const where = string_argument(internals, args, 0);
        if (!where)
            return std::nullopt;
        std::optional<dom::Node*> const node = node_argument(internals, args, 1, "insertAdjacentElement");
        if (!node)
            return std::nullopt;
        auto const position = adjacent_position(internals, e, *where, "insertAdjacentElement");
        if (!position)
            return std::nullopt;
        if (!position->first)
            return js::Value::null();
        return pre_insert(internals, *position->first, **node, position->second);
    });
    element_method(in, element, "insertAdjacentText", 2, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const where = string_argument(internals, args, 0);
        std::optional<std::string> text = string_argument(internals, args, 1);
        if (!where || !text)
            return std::nullopt;
        auto const position = adjacent_position(internals, e, *where, "insertAdjacentText");
        if (!position)
            return std::nullopt;
        if (!position->first)
            return js::Value::undefined();
        dom::Text* node = e.document().create<dom::Text>();
        node->data = std::move(*text);
        if (!pre_insert(internals, *position->first, *node, position->second))
            return std::nullopt;
        return js::Value::undefined();
    });
    element_getter(in, element, "previousElementSibling", [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.realm.wrap_or_null(previous_element_sibling(e)); });
    element_getter(in, element, "nextElementSibling", [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.realm.wrap_or_null(next_element_sibling(e)); });
    auto const matches_native = [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const text = string_argument(internals, args, 0);
        if (!text)
            return std::nullopt;
        std::optional<css::SelectorList> const list = parse_selector(internals, *text);
        if (!list)
            return std::nullopt;
        return js::Value::boolean(css::matches(*list, e));
    };
    element_method(in, element, "matches", 1, matches_native);
    element_method(in, element, "webkitMatchesSelector", 1, matches_native);
    element_method(in, element, "msMatchesSelector", 1, matches_native);
    element_method(in, element, "closest", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        std::optional<std::string> const text = string_argument(internals, args, 0);
        if (!text)
            return std::nullopt;
        std::optional<css::SelectorList> const list = parse_selector(internals, *text);
        if (!list)
            return std::nullopt;
        for (dom::Node* node = &e; node && node->is_element(); node = node->parent()) {
            if (css::matches(*list, *static_cast<dom::Element*>(node)))
                return js::Value::object(internals.wrap(*node));
        }
        return js::Value::null();
    });
    element_method(in, element, "getBoundingClientRect", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        std::optional<LayoutBox> const box = client_box(internals, e);
        if (!box)
            return make_rect(internals, 0, 0, 0, 0);
        return make_rect(internals, *box);
    });
    element_method(in, element, "getClientRects", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        js::Interpreter::Roots const roots(internals.interpreter);
        js::ArrayObject* rects = internals.interpreter.new_array();
        internals.interpreter.root(js::Value::object(rects));
        if (std::optional<LayoutBox> const box = client_box(internals, e))
            rects->push(make_rect(internals, *box));
        return js::Value::object(rects);
    });
    element_method(in, element, "checkVisibility", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        return js::Value::boolean(internals.hooks.layout_box ? internals.hooks.layout_box(e).has_value() : true);
    });
    auto const box_metric = [](Realm::Internals& internals, dom::Element& e, int which) -> Native {
        // 0 width, 1 height, 2 top, 3 left: the box as laid out, page coordinates.
        if (!internals.hooks.layout_box)
            return js::Value::number(0);
        std::optional<LayoutBox> const box = internals.hooks.layout_box(e);
        if (!box)
            return js::Value::number(0);
        switch (which) {
        case 0: return js::Value::number(std::round(static_cast<double>(box->width)));
        case 1: return js::Value::number(std::round(static_cast<double>(box->height)));
        case 2: return js::Value::number(std::round(static_cast<double>(box->y)));
        default: return js::Value::number(std::round(static_cast<double>(box->x)));
        }
    };
    // clientWidth and clientHeight (CSSOM View §6): the root element's are
    // the viewport's; any other box's are its padding box's, which is its
    // border box without the borders. clientTop and clientLeft are those
    // borders. The style's lengths are the engine's px; the box is CSS px.
    auto const client_metric = [](Realm::Internals& internals, dom::Element& e, int which) -> Native {
        // 0 width, 1 height, 2 top, 3 left.
        bool const root = e.parent() != nullptr && e.parent()->type() == dom::NodeType::Document;
        if (root && which < 2) {
            if (internals.hooks.refresh_viewport)
                internals.hooks.refresh_viewport();
            return js::Value::number(static_cast<double>(which == 0 ? internals.hooks.viewport_width : internals.hooks.viewport_height));
        }
        if (!internals.hooks.layout_box)
            return js::Value::number(0);
        std::optional<LayoutBox> const box = internals.hooks.layout_box(e);
        if (!box)
            return js::Value::number(0);
        css::ComputedStyle const* const style = internals.hooks.computed_style ? internals.hooks.computed_style(e) : nullptr;
        float const scale = internals.hooks.device_scale > 0 ? internals.hooks.device_scale : 1.0f;
        float const left = style ? style->border_left.width / scale : 0.0f;
        float const right = style ? style->border_right.width / scale : 0.0f;
        float const top = style ? style->border_top.width / scale : 0.0f;
        float const bottom = style ? style->border_bottom.width / scale : 0.0f;
        switch (which) {
        case 0: return js::Value::number(std::round(static_cast<double>(std::max(0.0f, box->width - left - right))));
        case 1: return js::Value::number(std::round(static_cast<double>(std::max(0.0f, box->height - top - bottom))));
        case 2: return js::Value::number(std::round(static_cast<double>(top)));
        default: return js::Value::number(std::round(static_cast<double>(left)));
        }
    };
    element_getter(in, element, "clientWidth", [client_metric](Realm::Internals& internals, dom::Element& e) -> Native { return client_metric(internals, e, 0); });
    element_getter(in, element, "clientHeight", [client_metric](Realm::Internals& internals, dom::Element& e) -> Native { return client_metric(internals, e, 1); });
    element_getter(in, element, "clientTop", [client_metric](Realm::Internals& internals, dom::Element& e) -> Native { return client_metric(internals, e, 2); });
    element_getter(in, element, "clientLeft", [client_metric](Realm::Internals& internals, dom::Element& e) -> Native { return client_metric(internals, e, 3); });
    element_getter(in, element, "scrollWidth", [box_metric](Realm::Internals& internals, dom::Element& e) -> Native { return box_metric(internals, e, 0); });
    element_getter(in, element, "scrollHeight", [box_metric](Realm::Internals& internals, dom::Element& e) -> Native { return box_metric(internals, e, 1); });
    element_accessor(
        in, element, "scrollTop", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::number(0); },
        [](Realm::Internals&, dom::Element&, js::Value const&) -> Native { return js::Value::undefined(); });
    element_accessor(
        in, element, "scrollLeft", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::number(0); },
        [](Realm::Internals&, dom::Element&, js::Value const&) -> Native { return js::Value::undefined(); });
    for (std::string_view const name : { "scrollIntoView", "scrollIntoViewIfNeeded", "scrollTo", "scroll", "scrollBy", "releasePointerCapture",
             "setPointerCapture", "requestPointerLock" })
        element_method(in, element, name, 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::undefined(); });
    element_method(in, element, "hasPointerCapture", 1, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::boolean(false); });
    element_method(in, element, "attachShadow", 1, [](Realm::Internals& internals, dom::Element&, Args) -> Native {
        return internals.throw_dom_exception("NotSupportedError", "Shadow trees are not supported");
    });
    element_method(in, element, "getAnimations", 0, [](Realm::Internals& internals, dom::Element&, Args) -> Native {
        return js::Value::object(internals.interpreter.new_array());
    });
    // requestFullscreen() is a promise, refused while the shell has no full screen.
    element_method(in, element, "requestFullscreen", 0, [](Realm::Internals& internals, dom::Element&, Args) -> Native {
        internals.interpreter.throw_type_error("Fullscreen request denied");
        return rejected_promise(internals.interpreter, internals.interpreter.take_exception());
    });
}

// --- CharacterData, Text, Comment ------------------------------------------------------------

void install_character_data(Realm::Internals& in, js::Object& character_data, js::Object& text)
{
    install_child_node(in, character_data);
    node_accessor(
        in, character_data, "data",
        [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.string(js::utf8_from_utf16(data_units(n))); },
        [](Realm::Internals& internals, dom::Node& n, js::Value const& value) -> Native {
            std::optional<std::string> const data = value.is_nullish() ? std::optional<std::string>("") : internals.to_utf8(value);
            if (!data)
                return std::nullopt;
            set_data(internals, n, js::utf16_from_utf8(*data));
            return js::Value::undefined();
        });
    node_getter(in, character_data, "length", [](Realm::Internals&, dom::Node& n) -> Native {
        return js::Value::number(static_cast<double>(data_units(n).size()));
    });
    node_getter(in, character_data, "previousElementSibling", [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.realm.wrap_or_null(previous_element_sibling(n)); });
    node_getter(in, character_data, "nextElementSibling", [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.realm.wrap_or_null(next_element_sibling(n)); });
    node_method(in, character_data, "substringData", 2, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<double> const offset = internals.interpreter.to_number(js::argument(args, 0));
        std::optional<double> const count = internals.interpreter.to_number(js::argument(args, 1));
        if (!offset || !count)
            return std::nullopt;
        std::u16string const units = data_units(n);
        auto const start = static_cast<std::size_t>(std::max(0.0, *offset));
        if (start > units.size())
            return internals.throw_dom_exception("IndexSizeError", "The offset is larger than the node's length");
        auto const length = static_cast<std::size_t>(std::max(0.0, *count));
        return internals.string(js::utf8_from_utf16(std::u16string_view(units).substr(start, length)));
    });
    node_method(in, character_data, "appendData", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<std::string> const data = string_argument(internals, args, 0);
        if (!data)
            return std::nullopt;
        replace_data(internals, n, data_units(n).size(), 0, js::utf16_from_utf8(*data));
        return js::Value::undefined();
    });
    // The offset and count are WebIDL unsigned longs.
    auto const replace = [](Realm::Internals& internals, dom::Node& n, double offset, double count, std::string_view data) -> Native {
        std::uint32_t const start = to_unsigned_long(offset);
        if (start > data_units(n).size())
            return internals.throw_dom_exception("IndexSizeError", "The offset is larger than the node's length");
        replace_data(internals, n, start, to_unsigned_long(count), js::utf16_from_utf8(data));
        return js::Value::undefined();
    };
    node_method(in, character_data, "insertData", 2, [replace](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<double> const offset = internals.interpreter.to_number(js::argument(args, 0));
        std::optional<std::string> const data = string_argument(internals, args, 1);
        if (!offset || !data)
            return std::nullopt;
        return replace(internals, n, *offset, 0, *data);
    });
    node_method(in, character_data, "deleteData", 2, [replace](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<double> const offset = internals.interpreter.to_number(js::argument(args, 0));
        std::optional<double> const count = internals.interpreter.to_number(js::argument(args, 1));
        if (!offset || !count)
            return std::nullopt;
        return replace(internals, n, *offset, *count, "");
    });
    node_method(in, character_data, "replaceData", 3, [replace](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<double> const offset = internals.interpreter.to_number(js::argument(args, 0));
        std::optional<double> const count = internals.interpreter.to_number(js::argument(args, 1));
        std::optional<std::string> const data = string_argument(internals, args, 2);
        if (!offset || !count || !data)
            return std::nullopt;
        return replace(internals, n, *offset, *count, *data);
    });
    node_getter(in, text, "wholeText", [](Realm::Internals& internals, dom::Node& n) -> Native {
        // This text node with its contiguous text siblings.
        dom::Node* first = &n;
        while (dom::Node* previous = first->previous_sibling()) {
            if (!previous->is_text())
                break;
            first = previous;
        }
        std::string whole;
        for (dom::Node* node = first; node && node->is_text(); node = next_sibling_of(*node))
            whole += static_cast<dom::Text&>(*node).data;
        return internals.string(whole);
    });
    node_method(in, text, "splitText", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<double> const offset = internals.interpreter.to_number(js::argument(args, 0));
        if (!offset)
            return std::nullopt;
        if (!n.is_text())
            return internals.interpreter.throw_type_error("Illegal invocation");
        std::uint32_t const at = to_unsigned_long(*offset);
        if (at > data_units(n).size())
            return internals.throw_dom_exception("IndexSizeError", "The offset is larger than the node's length");
        return js::Value::object(internals.wrap(*split_text(internals, static_cast<dom::Text&>(n), at)));
    });
}

// The attribute SVGURIReference's href reflects: href in no namespace, else
// the href in the XLink namespace SVG 2 keeps for old content, which is
// xlink:href whether the parser or setAttributeNS put it there. An attribute
// in no namespace named xlink:href is neither.
dom::Attr const* reflected_href(dom::Element const& element)
{
    dom::Attr const* xlink = nullptr;
    for (dom::Attr const& attribute : element.attributes()) {
        if (attribute.local_name != "href")
            continue;
        if (attribute.namespace_uri.empty())
            return &attribute;
        if (attribute.namespace_uri == dom::ns::xlink && xlink == nullptr)
            xlink = &attribute;
    }
    return xlink;
}

} // namespace

std::string const* svg_href(dom::Element const& element)
{
    dom::Attr const* const href = reflected_href(element);
    return href ? &href->value : nullptr;
}

namespace {

// An SVGAnimatedString (SVG 2 §4.4.7) over an element's href: baseVal reads
// and writes the attribute reflected, and animVal, with no animation running,
// reads what baseVal does.
class AnimatedStringObject final : public ElementBackedObject {
public:
    AnimatedStringObject(js::Object* prototype, NodeWrapper& the_wrapper)
        : ElementBackedObject(prototype, &the_wrapper)
    {
    }
};

std::optional<AnimatedStringObject*> this_animated_string(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* animated = dynamic_cast<AnimatedStringObject*>(this_value.as_object()))
            return animated;
    }
    return interp.throw_type_error("Illegal invocation");
}

// The SVG <a> element's interface, SVGAElement, as far as its href: what a
// hyperlink's origin is read from.
void install_svg_links(Realm::Internals& in, js::Object& svg_element)
{
    js::Object* animated_string = define_interface(in, "SVGAnimatedString", nullptr);
    js::NativeFunction::Callback const read = [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AnimatedStringObject*> const found = this_animated_string(interp, this_value);
        if (!found)
            return std::nullopt;
        dom::Element const* const element = (*found)->element();
        std::string const* const href = element ? svg_href(*element) : nullptr;
        return internals_of(interp).string(href ? *href : std::string());
    };
    define_getter(in, *animated_string, "baseVal", read,
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<AnimatedStringObject*> const found = this_animated_string(interp, this_value);
            if (!found)
                return std::nullopt;
            std::optional<std::string> value = internals_of(interp).to_utf8(js::argument(args, 0));
            if (!value)
                return std::nullopt;
            // The write goes to the attribute reflected — an xlink:href while
            // there is no href, else href, made in no namespace when there is
            // neither — as a mutation of the element's own document's realm.
            if (dom::Element* const element = (*found)->element()) {
                dom::Attr const* const reflected = reflected_href(*element);
                std::string const namespace_uri = reflected ? reflected->namespace_uri : std::string();
                set_attribute_ns((*found)->wrapper->realm().internals(), *element, namespace_uri, "", "href", std::move(*value));
            }
            return js::Value::undefined();
        });
    define_getter(in, *animated_string, "animVal", read);

    // href is [SameObject]: the one SVGAnimatedString is made the first time,
    // with the intrinsics of the element's realm, and its wrapper keeps it.
    js::Object* anchor = define_interface(in, "SVGAElement", &svg_element);
    element_getter(in, *anchor, "href", [](Realm::Internals& internals, dom::Element& e) -> Native {
        js::Interpreter::Roots const roots(internals.interpreter);
        NodeWrapper& wrapper = wrapper_for(internals, e);
        internals.interpreter.root(js::Value::object(&wrapper));
        if (js::Object* const kept = wrapper.same_object("href"))
            return js::Value::object(kept);
        js::Object* const animated = internals.interpreter.heap().allocate<AnimatedStringObject>(
            wrapper.realm().internals().prototype("SVGAnimatedString"), wrapper);
        wrapper.keep_same_object("href", animated);
        return js::Value::object(animated);
    });
}

} // namespace

// --- install_nodes ---------------------------------------------------------------------------------

void install_nodes(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());
    js::Object* event_target = in.prototype("EventTarget");

    js::Object* node = define_interface(in, "Node", event_target);
    install_node(in, *node);

    js::Object* element = define_interface(in, "Element", node);
    install_element(in, *element);
    define_event_handlers(in, *element, global_event_types);

    js::Object* html_element = define_interface(in, "HTMLElement", element);
    js::Object* svg_element = define_interface(in, "SVGElement", element);
    install_svg_links(in, *svg_element);
    define_interface(in, "MathMLElement", element);
    install_html_elements(in, *html_element);

    js::Object* character_data = define_interface(in, "CharacterData", node);
    js::Object* text = define_interface(in, "Text", character_data,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            std::optional<std::string> data = args.empty() ? std::optional<std::string>("") : internals.to_utf8(args[0]);
            if (!data)
                return std::nullopt;
            dom::Text* node_ptr = internals.document->create<dom::Text>();
            node_ptr->data = std::move(*data);
            return js::Value::object(internals.wrap(*node_ptr));
        });
    js::Object* comment = define_interface(in, "Comment", character_data,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            std::optional<std::string> data = args.empty() ? std::optional<std::string>("") : internals.to_utf8(args[0]);
            if (!data)
                return std::nullopt;
            dom::Comment* node_ptr = internals.document->create<dom::Comment>();
            node_ptr->data = std::move(*data);
            return js::Value::object(internals.wrap(*node_ptr));
        });
    (void)comment;
    install_character_data(in, *character_data, *text);
    // Neither has a constructor: a document makes them.
    define_interface(in, "CDATASection", text);
    js::Object* processing_instruction = define_interface(in, "ProcessingInstruction", character_data);
    node_getter(in, *processing_instruction, "target", [](Realm::Internals& internals, dom::Node& n) -> Native {
        if (n.type() != dom::NodeType::ProcessingInstruction)
            return internals.interpreter.throw_type_error("Illegal invocation");
        return internals.string(static_cast<dom::ProcessingInstruction&>(n).target);
    });

    js::Object* fragment = define_interface(in, "DocumentFragment", node,
        [](js::Interpreter& interp, Args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            return js::Value::object(internals.wrap(*internals.document->create<dom::DocumentFragment>()));
        });
    install_parent_node(in, *fragment);
    // ShadowRoot: the interface object alone. attachShadow refuses with
    // NotSupportedError, so no page gets one, but a library's
    // `node instanceof ShadowRoot` must be a question, not a ReferenceError.
    define_interface(in, "ShadowRoot", fragment);
    node_method(in, *fragment, "getElementById", 1, [](Realm::Internals& internals, dom::Node& n, Args args) -> Native {
        std::optional<std::string> const id = string_argument(internals, args, 0);
        if (!id)
            return std::nullopt;
        return internals.realm.wrap_or_null(element_by_id(n, *id));
    });

    js::Object* doctype = define_interface(in, "DocumentType", node);
    install_child_node(in, *doctype);
    node_getter(in, *doctype, "name", [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.string(static_cast<dom::DocumentType&>(n).name); });
    node_getter(in, *doctype, "publicId", [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.string(static_cast<dom::DocumentType&>(n).public_identifier); });
    node_getter(in, *doctype, "systemId", [](Realm::Internals& internals, dom::Node& n) -> Native { return internals.string(static_cast<dom::DocumentType&>(n).system_identifier); });

    install_document(in, *node);

    // NodeList and HTMLCollection are Arrays with their own prototypes over
    // Array.prototype: forEach, indexing and length come for free, item()
    // and namedItem() are added.
    js::Object* node_list_proto = define_interface(in, "NodeList", interpreter.intrinsics().array_prototype);
    js::Object* collection_proto = define_interface(in, "HTMLCollection", interpreter.intrinsics().array_prototype);
    js::Object* named_node_map = define_interface(in, "NamedNodeMap", interpreter.intrinsics().array_prototype);
    define_interface(in, "Attr", nullptr);
    auto const item = [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        if (!js::Interpreter::is_array(this_value))
            return interp.throw_type_error("Illegal invocation");
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        auto* array = static_cast<js::ArrayObject*>(this_value.as_object());
        std::uint32_t const position = to_unsigned_long(*index);
        if (position >= array->length())
            return js::Value::null();
        js::Value const element_value = array->element(position);
        return element_value.is_empty() ? js::Value::null() : element_value;
    };
    js::define_method(interpreter, *node_list_proto, "item", 1, item);
    js::define_method(interpreter, *collection_proto, "item", 1, item);
    js::define_method(interpreter, *named_node_map, "item", 1, item);
    js::define_method(interpreter, *collection_proto, "namedItem", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        if (!js::Interpreter::is_array(this_value))
            return interp.throw_type_error("Illegal invocation");
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        auto* array = static_cast<js::ArrayObject*>(this_value.as_object());
        for (std::uint32_t i = 0; i < array->length(); ++i) {
            dom::Node* node_ptr = internals.realm.node_of(array->element(i));
            if (!node_ptr || !node_ptr->is_element())
                continue;
            auto& e = static_cast<dom::Element&>(*node_ptr);
            if (attribute_or_empty(e, "id") == *name || attribute_or_empty(e, "name") == *name)
                return js::Value::object(internals.wrap(e));
        }
        return js::Value::null();
    });
    js::define_method(interpreter, *named_node_map, "getNamedItem", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        if (!js::Interpreter::is_array(this_value))
            return interp.throw_type_error("Illegal invocation");
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        auto* array = static_cast<js::ArrayObject*>(this_value.as_object());
        for (std::uint32_t i = 0; i < array->length(); ++i) {
            js::Value const attr = array->element(i);
            if (!attr.is_object())
                continue;
            std::optional<js::Value> const attr_name = interp.get(attr, "name");
            if (!attr_name)
                return std::nullopt;
            if (attr_name->is_string() && attr_name->as_string()->to_utf8() == *name)
                return attr;
        }
        return js::Value::null();
    });

    // DOMRect.
    js::Object* rect = define_interface(in, "DOMRect", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            double values[4] = { 0, 0, 0, 0 };
            for (std::size_t i = 0; i < 4 && i < args.size(); ++i) {
                std::optional<double> const number = interp.to_number(args[i]);
                if (!number)
                    return std::nullopt;
                values[i] = *number;
            }
            return make_rect(internals_of(interp), values[0], values[1], values[2], values[3]);
        },
        4);
    in.prototypes["DOMRectReadOnly"] = rect;
    auto const rect_side = [](std::string_view a, std::string_view b, bool sum, bool minimum) {
        return [a, b, sum, minimum](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<js::Value> const first = interp.get(this_value, a);
            std::optional<js::Value> const second = interp.get(this_value, b);
            if (!first || !second)
                return std::nullopt;
            std::optional<double> const x = interp.to_number(*first);
            std::optional<double> const y = interp.to_number(*second);
            if (!x || !y)
                return std::nullopt;
            if (!sum)
                return js::Value::number(minimum ? std::min(*x, *x + *y) : *x);
            return js::Value::number(std::max(*x, *x + *y));
        };
    };
    define_getter(in, *rect, "top", rect_side("y", "height", false, true));
    define_getter(in, *rect, "left", rect_side("x", "width", false, true));
    define_getter(in, *rect, "bottom", rect_side("y", "height", true, false));
    define_getter(in, *rect, "right", rect_side("x", "width", true, false));
    js::define_method(interpreter, *rect, "toJSON", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        js::Object* json = interp.new_object();
        interp.root(js::Value::object(json));
        for (std::string_view const name : { "x", "y", "width", "height", "top", "right", "bottom", "left" }) {
            std::optional<js::Value> const value = interp.get(this_value, name);
            if (!value)
                return std::nullopt;
            json->put(internals.interpreter.key(name), *value);
        }
        return js::Value::object(json);
    });
}

}
