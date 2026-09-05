#pragma once

// Shared by the node binding files: the tree algorithms (DOM §4.2) the
// natives call, and the small accessor templates that keep each native to
// its own logic.

#include "bindings/Internal.h"
#include "css/Selector.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::bindings {

// --- Tree helpers -----------------------------------------------------------------

dom::Node* next_sibling_of(dom::Node const& node);
dom::Element* first_element_child(dom::Node const& node);
dom::Element* last_element_child(dom::Node const& node);
dom::Element* next_element_sibling(dom::Node const& node);
dom::Element* previous_element_sibling(dom::Node const& node);
std::vector<dom::Node*> element_children(dom::Node const& node);
bool is_inclusive_ancestor(dom::Node const& ancestor, dom::Node const& node);
// True when `a` comes before `b` in tree order (both under one root).
bool precedes_in_tree_order(dom::Node const& a, dom::Node const& b);
// Every descendant in tree order, the node itself excluded.
void collect_descendants(dom::Node& node, std::vector<dom::Node*>& out);
// The children a template shows to scripts: its content fragment.
dom::Node& content_container(dom::Element& element);
dom::Element* element_by_id(dom::Node& root, std::string_view id);
dom::Element* body_element(dom::Document& document);
dom::Element* head_element(dom::Document& document);
dom::Element* document_element(dom::Document& document);
// The HTML tag name as tagName spells it: uppercase for HTML elements.
std::string tag_name_of(dom::Element const& element);
std::string node_name_of(dom::Node const& node);

// --- Mutation (each counts as one) --------------------------------------------------

// DOM §4.2.3 pre-insert: validity, adoption, a fragment's children moved
// one by one, and inserted scripts prepared. Returns the wrapper of `node`,
// or the thrown exception.
Native pre_insert(Realm::Internals&, dom::Node& parent, dom::Node& node, dom::Node* child);
void remove_node(Realm::Internals&, dom::Node& node);
// Replaces the children of `parent` with the children parsed from `markup`
// in the context of `context` (innerHTML).
void replace_children_with_markup(Realm::Internals&, dom::Node& parent, dom::Element& context, std::string_view markup);
// Parses `markup` in `context` and returns the children, adopted into
// context's document and detached; scripts among them will never run.
std::vector<dom::Node*> parse_markup(Realm::Internals&, dom::Element& context, std::string_view markup);
void replace_children_with_text(Realm::Internals&, dom::Node& parent, std::string_view text);
dom::Node* clone_node(Realm::Internals&, dom::Node const& node, bool deep);

// --- Selectors ----------------------------------------------------------------------

// Parses a selector string, throwing a SyntaxError DOMException when it is
// invalid. nullopt = thrown.
std::optional<css::SelectorList> parse_selector(Realm::Internals&, std::string_view text);
std::vector<dom::Node*> query_all(dom::Node& root, css::SelectorList const& list, bool first_only);

// --- Accessor templates --------------------------------------------------------------

template<typename Read>
void node_getter(Realm::Internals& in, js::Object& prototype, std::string_view name, Read read)
{
    define_getter(in, prototype, name, [read](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native {
        std::optional<dom::Node*> const node = this_node(interpreter, this_value);
        if (!node)
            return std::nullopt;
        return read(internals_of(interpreter), **node);
    });
}

template<typename Read, typename Write>
void node_accessor(Realm::Internals& in, js::Object& prototype, std::string_view name, Read read, Write write)
{
    define_getter(
        in, prototype, name,
        [read](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Node*> const node = this_node(interpreter, this_value);
            if (!node)
                return std::nullopt;
            return read(internals_of(interpreter), **node);
        },
        [write](js::Interpreter& interpreter, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Node*> const node = this_node(interpreter, this_value);
            if (!node)
                return std::nullopt;
            return write(internals_of(interpreter), **node, js::argument(args, 0));
        });
}

template<typename Body>
void node_method(Realm::Internals& in, js::Object& prototype, std::string_view name, int length, Body body)
{
    js::define_method(in.interpreter, prototype, name, length,
        [body](js::Interpreter& interpreter, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Node*> const node = this_node(interpreter, this_value);
            if (!node)
                return std::nullopt;
            return body(internals_of(interpreter), **node, args);
        });
}

template<typename Read>
void element_getter(Realm::Internals& in, js::Object& prototype, std::string_view name, Read read)
{
    define_getter(in, prototype, name, [read](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native {
        std::optional<dom::Element*> const element = this_element(interpreter, this_value);
        if (!element)
            return std::nullopt;
        return read(internals_of(interpreter), **element);
    });
}

template<typename Read, typename Write>
void element_accessor(Realm::Internals& in, js::Object& prototype, std::string_view name, Read read, Write write)
{
    define_getter(
        in, prototype, name,
        [read](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Element*> const element = this_element(interpreter, this_value);
            if (!element)
                return std::nullopt;
            return read(internals_of(interpreter), **element);
        },
        [write](js::Interpreter& interpreter, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Element*> const element = this_element(interpreter, this_value);
            if (!element)
                return std::nullopt;
            return write(internals_of(interpreter), **element, js::argument(args, 0));
        });
}

template<typename Body>
void element_method(Realm::Internals& in, js::Object& prototype, std::string_view name, int length, Body body)
{
    js::define_method(in.interpreter, prototype, name, length,
        [body](js::Interpreter& interpreter, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Element*> const element = this_element(interpreter, this_value);
            if (!element)
                return std::nullopt;
            return body(internals_of(interpreter), **element, args);
        });
}

template<typename Read>
void document_getter(Realm::Internals& in, js::Object& prototype, std::string_view name, Read read)
{
    define_getter(in, prototype, name, [read](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native {
        std::optional<dom::Document*> const document = this_document(interpreter, this_value);
        if (!document)
            return std::nullopt;
        return read(internals_of(interpreter), **document);
    });
}

template<typename Body>
void document_method(Realm::Internals& in, js::Object& prototype, std::string_view name, int length, Body body)
{
    js::define_method(in.interpreter, prototype, name, length,
        [body](js::Interpreter& interpreter, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Document*> const document = this_document(interpreter, this_value);
            if (!document)
                return std::nullopt;
            return body(internals_of(interpreter), **document, args);
        });
}

// A DOMRect for a box in client coordinates.
js::Value make_rect(Realm::Internals&, double x, double y, double width, double height);
js::Value make_rect(Realm::Internals&, LayoutBox const& box);
// An element's box as the client sees it (page box less the scroll).
std::optional<LayoutBox> client_box(Realm::Internals&, dom::Element const&);

// The second half of install_nodes: the HTML element interfaces (HtmlElements.cpp).
void install_html_elements(Realm::Internals&, js::Object& html_element_prototype);
// The Document interface (Document.cpp).
void install_document(Realm::Internals&, js::Object& node_prototype);

}
