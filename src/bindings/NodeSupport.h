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

// --- Names --------------------------------------------------------------------------

// DOM §1.4 "validate and extract": a namespace, prefix and local name from a
// qualified name, for an element's name when `element`, else an attribute's;
// or the name of the DOMException to throw when they are not valid.
struct ExtractedName {
    std::string namespace_uri; // "" for null
    std::string prefix; // "" for null
    std::string local_name;
    std::string_view error; // "" when valid
};
ExtractedName validate_and_extract(std::string namespace_uri, std::string const& qualified_name, bool element = false);

// --- Arguments and character data -----------------------------------------------------

// A node argument, or a TypeError naming the parameter.
std::optional<dom::Node*> node_argument(Realm::Internals&, Args, std::size_t index, std::string_view method);
// Text (CDATA sections too), Comment and ProcessingInstruction: the nodes
// with data (DOM §4.10), and that data.
bool is_character_data(dom::Node const&);
std::string const& character_data(dom::Node const&);
std::u16string data_units(dom::Node const&);
// DOM §4.10 "replace data": `count` code units at `offset`, which the caller
// has checked, replaced by `data`, and the live ranges in the node moved to
// match. Counts as a mutation.
void replace_data(Realm::Internals&, dom::Node&, std::size_t offset, std::size_t count, std::u16string_view data);
// The whole of the data replaced: the data, nodeValue and textContent setters.
void set_data(Realm::Internals&, dom::Node&, std::u16string_view units);
// DOM §4.11 "split a Text node" at an offset no longer than its length: the
// new node, inserted after it when it has a parent.
dom::Text* split_text(Realm::Internals&, dom::Text&, std::size_t offset);
// A new live range at (container, 0) (Range.cpp).
js::Value new_range(Realm::Internals&, dom::Node& container);
// document.createTreeWalker and createNodeIterator (Traversal.cpp): the
// root, whatToShow and the filter, as the arguments give them.
Native new_tree_walker(Realm::Internals&, Args);
Native new_node_iterator(Realm::Internals&, Args);

// --- Mutation (each counts as one) --------------------------------------------------

// DOM §4.2.3 "ensure pre-insertion validity", or with `replacing` the checks
// replacing `child` makes before anything is removed: undefined, or the
// thrown HierarchyRequestError or NotFoundError.
Native ensure_pre_insertion_validity(Realm::Internals&, dom::Node& parent, dom::Node& node, dom::Node* child, bool replacing = false);
// DOM §4.2.3 pre-insert: validity, adoption, a fragment's children moved
// one by one, and inserted scripts prepared. Returns the wrapper of `node`,
// or the thrown exception.
Native pre_insert(Realm::Internals&, dom::Node& parent, dom::Node& node, dom::Node* child);
void remove_node(Realm::Internals&, dom::Node& node);
// Replaces the children of `parent` with the children parsed from `markup`
// in the context of `context` (innerHTML).
void replace_children_with_markup(Realm::Internals&, dom::Node& parent, dom::Element& context, std::string_view markup);
// Parses `markup` in `context` and returns the children, adopted into
// context's document and detached; scripts among them never run, unless
// `scripts_started` is false, when they run once inserted.
std::vector<dom::Node*> parse_markup(Realm::Internals&, dom::Element& context, std::string_view markup, bool scripts_started = true);
void replace_children_with_text(Realm::Internals&, dom::Node& parent, std::string_view text);
dom::Node* clone_node(Realm::Internals&, dom::Node const& node, bool deep);
// The cloning steps of a script element (HTML §4.12.1): a copy of one that
// had started has started too, and so never runs. For a clone made of
// `source`, subtree for subtree.
void copy_started_scripts(Realm::Internals&, dom::Node const& source, dom::Node const& clone);

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

template<typename Read, typename Write>
void document_accessor(Realm::Internals& in, js::Object& prototype, std::string_view name, Read read, Write write)
{
    define_getter(
        in, prototype, name,
        [read](js::Interpreter& interpreter, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Document*> const document = this_document(interpreter, this_value);
            if (!document)
                return std::nullopt;
            return read(internals_of(interpreter), **document);
        },
        [write](js::Interpreter& interpreter, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Document*> const document = this_document(interpreter, this_value);
            if (!document)
                return std::nullopt;
            return write(internals_of(interpreter), **document, js::argument(args, 0));
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
