#pragma once

// Focus (HTML §6.6): which elements are focusable areas, which of those Tab
// reaches, and the order it reaches them in — the flattened tabindex-ordered
// focus navigation scopes of a document, through shadow trees, slots and the
// documents of frames. Pure functions over the flat tree: what they need of
// style and layout (whether an element is being rendered, inert, a scroll
// container with something to scroll) and of the frames (an iframe's active
// document, a document's container) the host gives through an Environment.
// The shell's Tab key, a script's focus(), a press on the page and the test
// runners all ask the same rules here.

#include <functional>
#include <optional>
#include <vector>

namespace sashfold::dom {
class Document;
class Element;
class Node;
}

namespace sashfold::dom::focus {

// What style and layout say of one element, for the rules below.
struct ElementFacts {
    bool display_none = false; // display: none (the element and its flat-tree descendants have no boxes)
    bool display_contents = false; // display: contents (no box of its own; its children have theirs)
    bool visible = true; // visibility: visible (not hidden, not collapse)
    bool inert = false;
    // A scroll container (overflow scroll or auto on either axis) whose
    // content overflows it, so the reader has something to scroll.
    bool scrollable = false;
    // An editing host by a rule the markup does not show: the body of a
    // document in design mode (HTML §6.8.2).
    bool editing_host = false;
};

struct Environment {
    // Null: the element's own markup decides (the `hidden` attribute is
    // display: none, a style attribute is not read), nothing scrolls.
    std::function<ElementFacts(Element const&)> facts;
    // A navigable container's active document (an iframe's), when the host
    // has one live for it; null otherwise.
    std::function<Document const*(Element const&)> content_document;
    // The navigable container a document is shown in, null for the top.
    std::function<Element const*(Document const&)> container_of;
};

// The tabindex attribute's value by the rules for parsing integers (HTML
// §2.3.4.1): nullopt when the attribute is absent or is not an integer.
std::optional<long> tabindex_value(Element const& element);

// An editing host (HTML §6.8.1): contenteditable in the true or
// plaintext-only state on an element whose parent is not editable.
bool is_editing_host(Element const& element);

// Being rendered (HTML §15.1) as focus asks it: the element has a box — no
// flat-tree ancestor or itself display: none, itself not display: contents,
// not inside a closed <details> but for its summary, and visible.
bool is_rendered_for_focus(Element const& element, Environment const& env);

// A focusable area (HTML §6.6.2): what focus() and a press can focus —
// links, form controls that are not disabled, the first summary of a
// details, iframes, media with controls, editing hosts, any element with a
// tabindex, and a scroll container with overflow that holds no focusable
// area of its own — when it is connected, rendered and not inert.
bool is_focusable_area(Element const& element, Environment const& env);

// Whether Tab reaches the element (§6.6.3 "sequentially focusable"): a
// focusable area whose tabindex is not negative (a delegatesFocus host is
// never a stop itself: its contents are).
bool is_sequentially_focusable(Element const& element, Environment const& env);

// The flattened tabindex-ordered focus navigation scope of a document
// (§6.6.3): its focusable areas in the order Tab visits them — positive
// tabindex values first, ascending, ties in flat-tree order, then the rest
// in flat-tree order — each shadow host's and slot's own scope ordered alike
// and put in after the owner, a scope owner with a negative tabindex taking
// its whole scope out. An iframe is an entry; its document is not entered.
std::vector<Element const*> navigation_order(Document const& document, Environment const& env);

// One step of sequential focus navigation inside a single document (§6.6.5
// "sequential navigation search algorithm"): the entry after (backward:
// before) `from` — an entry of the order, or any other node, a starting
// point, then taken by its place in the flat tree — or the first (last)
// when `from` is null. Radio buttons of one group are one stop: the checked
// one, else the first met in the direction of travel, and none while the
// focus is already in that group. Null when the document has no more.
Element const* next_in_document(Document const& document, Node const* from, bool backward, Environment const& env);

// Sequential navigation across documents: from `from` (in any document of
// the frame tree; null starts at `top`'s edge), the next stop. An iframe in
// the order is entered — its first (last) stop — and a frame's document
// once done gives way to the entry after (before) its container; a frame
// whose document has no stop is a stop itself. Null when `top` has no more:
// the focus leaves the page, to the window's own controls.
Element const* sequential_next(Document const& top, Node const* from, bool backward, Environment const& env);

// The focusable area a shadow host with delegatesFocus gives its focus to
// (§6.6.4 "focus delegate"): the first focusable area of its shadow tree in
// tree order, a nested delegating host giving its own delegate; an
// [autofocus] one comes first. Null when there is none.
Element const* focus_delegate(Element const& host, Environment const& env);

// What a press on `node` focuses (§6.6.4 "click focusable"): the nearest
// flat-tree inclusive ancestor that is a focusable area (a delegatesFocus
// host: its delegate unless the focus is already inside it); null when none
// is, and the press then focuses the viewport.
Element const* click_focus_target(Node const& node, Environment const& env);

}
