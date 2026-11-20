#pragma once

// The accessibility semantics of one element at a time, computed from the
// tree and its styles: its role (WAI-ARIA 1.2 §7.1 and §8.1 over HTML-AAM's
// and SVG-AAM's element mappings), its accessible name and description
// (Accessible Name and Description Computation 1.2), whether it is hidden,
// focusable, disabled, and the element references its ARIA relations name.
// The accessibility tree (a11y/Tree.h) is built from these answers; the test
// driver asks them directly.

#include "a11y/Roles.h"
#include "dom/Dom.h"
#include "dom/Forms.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sashfold::css {
struct ComputedStyle;
}

namespace sashfold::a11y {

// A rectangle in CSS px, in page coordinates (from the top left of the
// document's initial containing block, as getBoundingClientRect plus the
// page's scroll).
struct Rect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    bool operator==(Rect const&) const = default;
};

// What the model reads of the page beyond its tree: the styles and the
// layout, which their owner keeps up to date with the tree. Every member
// answers on the page's own thread. A source without styles (`style`
// answering null for every element) treats every element as rendered.
class PageSource {
public:
    virtual ~PageSource() = default;
    // The element's computed style; null when it has none known.
    virtual css::ComputedStyle const* style(dom::Element const&) = 0;
    // The element's border box, through its transforms (what
    // getBoundingClientRect gives, plus the page scroll); nothing for an
    // element that has no box.
    virtual std::optional<Rect> box(dom::Element const&) { return std::nullopt; }
    // The boxes of part of a Text node's data, UTF-16 offsets from and to.
    virtual std::vector<Rect> text_boxes(dom::Node const&, std::uint32_t, std::uint32_t) { return {}; }
    // The element under a page point, what a pointer there lands on.
    virtual dom::Element const* element_at(float, float) { return nullptr; }
    // The document shown in a frame (iframe, frame, object) when it is
    // reachable from here; null otherwise.
    virtual dom::Document* frame_document(dom::Element const&) { return nullptr; }
    // Whether a link's URL is in the reader's history (:visited).
    virtual bool visited(dom::Element const&) { return false; }
    // A text control's selection as the host keeps it (the caret is its
    // focus end), in code points of its value; nothing when unknown.
    virtual std::optional<dom::forms::TextSelection> text_selection(dom::Element const&) { return std::nullopt; }
    // The viewport's size, CSS px, for the offscreen state.
    virtual std::optional<Rect> viewport() { return std::nullopt; }
    // A number that moves whenever anything the answers above depend on
    // changes (a restyle, a relayout): what the live tree compares.
    virtual std::uint64_t generation() const { return 0; }
};

// ASCII whitespace (Infra §4.6) collapsed to single spaces and trimmed.
std::string collapse_whitespace(std::string_view text);
// The role attribute's tokens (ARIA 1.2 §7.1: ASCII-whitespace-separated).
std::vector<std::string_view> role_tokens(std::string_view value);

class Semantics {
public:
    explicit Semantics(PageSource* source = nullptr);

    PageSource* source() const { return m_source; }
    void set_source(PageSource* source) { m_source = source; }

    // --- Roles ------------------------------------------------------------
    // The role the element is exposed with (Core-AAM's "Computed Role"):
    // the first valid, allowed token of its role attribute, else its host
    // language's implicit role; `none` resolved (a focusable element or one
    // with a global ARIA attribute keeps its implicit role; children
    // presentational, required owned elements of a presentational parent
    // inherit it), and HTML-AAM's minimum role applied.
    Role role(dom::Element const&);
    // The role the element's host language gives it, ignoring its role
    // attribute (HTML-AAM §3.4, SVG-AAM §4, MathML).
    Role implicit_role(dom::Element const&);
    // The explicit role (the first valid token), before `none` is resolved.
    std::optional<Role> explicit_role(dom::Element const&);
    // The string WebDriver's Get Computed Role answers with.
    std::string computed_role_string(dom::Element const&);

    // --- Names and descriptions ---------------------------------------------
    std::string name(dom::Element const&);
    std::string description(dom::Element const&);
    // Where the name came from, for the platform's name-source property and
    // for the description to know what the name already used.
    enum class NameSource : std::uint8_t { None, LabelledBy, Label, Attribute, Contents, Title, Placeholder, Caption, Alt, Value };
    NameSource name_source(dom::Element const&);

    // --- Visibility, focus, state helpers ---------------------------------
    // Not rendered: no box (display: none here or above, not connected, in
    // a template) or visibility: hidden. A text node is so when its parent is.
    bool not_rendered(dom::Node const&);
    // Hidden as AccName and Core-AAM mean it: not rendered, or aria-hidden
    // true on it or an ancestor (in the flat tree).
    bool hidden(dom::Node const&);
    // Whether aria-hidden="true" is on the node or a flat-tree ancestor.
    bool aria_hidden(dom::Node const&);
    // Rendered but for visibility: hidden (a box that keeps its room).
    bool visibility_hidden_only(dom::Element const&);
    // Whether the element is inert (the `inert` attribute, or a modal
    // dialog's background, as `interactivity: inert` computes).
    bool inert(dom::Element const&);
    // A focusable area (HTML §6.6.2), the tabindex attribute's negative
    // values included; rendered and not inert.
    bool focusable(dom::Element const&);
    // Disabled natively (HTML §4.10.18.5, fieldsets included) or by
    // aria-disabled on it or an ancestor.
    bool disabled(dom::Element const&);
    // The element with that ID in the element's tree (a shadow tree is a
    // tree of its own), first in tree order.
    dom::Element* element_by_id(dom::Node const& scope, std::string_view id);
    // The elements an IDREFS attribute names, in order, the ones that do not
    // resolve left out; and an element-reference attribute set from script
    // (ariaLabelledByElements) when the attribute's own value is absent.
    std::vector<dom::Element*> idrefs(dom::Element const&, std::string_view attribute);
    dom::Element* idref(dom::Element const&, std::string_view attribute);
    // The element whose aria-owns claims this one (the first in tree order
    // that does, and only when that does not make a cycle); null when none.
    dom::Element* aria_owner(dom::Element const&);
    // The children that make up a node's content for naming and for the
    // tree: its flat-tree children with those another element owns left
    // out, then the elements its aria-owns names.
    std::vector<dom::Node*> content_children(dom::Node const&);
    // The flat-tree parent, through aria-owns: the owner of an owned element.
    dom::Node* content_parent(dom::Node const&);
    // The text a text node shows: white space collapsed as its style says,
    // text-transform applied.
    std::string rendered_text(dom::Text const&);

    // An attribute's value, the ElementInternals defaults (ARIA §10.2,
    // "default semantics") standing in when the element has none.
    std::optional<std::string_view> aria(dom::Element const&, std::string_view attribute);
    // aria-* true/false tokens: true when the value is "true" (ASCII case-
    // insensitive), false for "false", nothing for absent or other values.
    std::optional<bool> aria_bool(dom::Element const&, std::string_view attribute);

    // The element-reference attributes a script set (ARIAMixin's
    // ariaLabelledByElements and the like): the bindings keep them, and
    // say so here, keyed by element and content attribute name.
    using ElementReferences = std::function<std::optional<std::vector<dom::Element*>>(dom::Element const&, std::string_view attribute)>;
    void set_element_references(ElementReferences references) { m_references = std::move(references); }
    // ElementInternals' role and ARIA properties (default semantics).
    using InternalsDefaults = std::function<std::optional<std::string>(dom::Element const&, std::string_view attribute)>;
    void set_internals_defaults(InternalsDefaults defaults) { m_internals = std::move(defaults); }

    // Forgets every cached answer (IDs, ownership): call after the tree or
    // its attributes changed. The caches also check the document's versions.
    void invalidate();

    css::ComputedStyle const* style(dom::Element const& element);

private:
    struct NameContext;
    std::string text_alternative(dom::Node const& node, NameContext& context, bool is_root);
    std::string name_from_content(dom::Element const& element, NameContext& context);
    std::optional<std::string> host_language_label(dom::Element const& element, NameContext& context, bool is_root, NameSource* source);
    std::string embedded_control_value(dom::Element const& element, Role role, NameContext& context);
    std::string label_elements_text(dom::Element const& control, NameContext& context);
    std::string subtree_text(dom::Node const& node, NameContext& context);
    std::string pseudo_text(dom::Element const& element, bool before);
    bool has_author_name(dom::Element const& element);
    bool presentational_conflict(dom::Element const& element);
    Role role_uncached(dom::Element const& element);
    Role html_role(dom::Element const& element);
    Role svg_role(dom::Element const& element);
    bool inherits_presentation(dom::Element const& element, Role implicit);
    void refresh_caches(dom::Document const& document);
    void build_owners(dom::Document const& document);

    PageSource* m_source = nullptr;
    ElementReferences m_references;
    InternalsDefaults m_internals;
    // Caches, valid for the document versions they were made at.
    dom::Document const* m_cached_document = nullptr;
    std::uint64_t m_tree_version = ~std::uint64_t { 0 };
    std::uint64_t m_attribute_version = ~std::uint64_t { 0 };
    std::uint64_t m_source_generation = ~std::uint64_t { 0 };
    std::uint64_t m_state_version = ~std::uint64_t { 0 };
    std::unordered_map<dom::Node const*, std::unordered_map<std::string, dom::Element*>> m_ids;
    std::unordered_map<dom::Element const*, std::vector<dom::Element const*>> m_labels;
    bool m_labels_built = false;
    // What the internals hook answered, kept while the views on it are used.
    std::deque<std::string> m_internals_strings;
    NameSource m_last_source = NameSource::None;
    std::unordered_map<dom::Element const*, dom::Element*> m_owner_of;
    std::unordered_map<dom::Element const*, std::vector<dom::Element*>> m_owned;
    bool m_owners_built = false;
    bool m_building_owners = false;
    std::unordered_map<dom::Element const*, Role> m_roles;
    std::unordered_set<dom::Element const*> m_roles_in_progress;
};

}
