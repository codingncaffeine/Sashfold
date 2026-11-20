#pragma once

// The accessibility tree of a document: what assistive technology reads of
// a page. Built from the semantics of a11y/Semantics.h on first demand and
// kept current from then on; a page nobody inspects never makes one, and
// pays nothing (Tree::instances counts every tree ever made).
//
// THE MODEL. One Node per element, text run, generated text, list marker
// or frame document that the tree includes, under a root for the document
// (role RootWebArea). Which nodes are included is fixed and documented on
// Tree::update below. Every node has an id (NodeId) that is never reused
// in the process: it names the same node for as long as the node lives.
//
// THREADS. A Tree belongs to the page's own thread: it reads the DOM, its
// styles and its layout, and every member is called there. Nothing it does
// waits on another thread. What another thread (the platform bridge) may
// touch is the Bridge at the end of this file, and only that:
//   - events go out through a queue: the tree posts each change to its
//     Observer on the page's thread; the Bridge, an Observer, copies it
//     into a locked queue and calls the wake function it was given (which
//     must not block either). The bridge's thread takes the events when it
//     likes; a slow reader lets the queue grow (it is capped, the oldest
//     dropped and a "tree changed" event put in their place), and never
//     slows the page;
//   - reads go to a snapshot: after each update the tree publishes an
//     immutable copy of every node (nodes that did not change are shared
//     with the copy before), which the bridge's thread reads without a lock
//     on the page;
//   - what must run on the page (an action, setting a value, focus, a
//     question the snapshot cannot answer such as a hit test against the
//     current layout) goes in as a Query: a function the bridge posts; the
//     page's thread runs the posted queries when it serves the bridge
//     (Tree::serve, called from its loop), and the query sends its answer
//     back however it likes (the bridge's own queue, a promise).

#include "a11y/Roles.h"
#include "a11y/Semantics.h"
#include "dom/Dom.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sashfold::a11y {

using NodeId = std::uint64_t; // 0 is no node

// The states of a node (Core-AAM §5.6, the states ARIA and the host
// language give), each a bit of States.
enum class State : std::uint8_t {
    Focusable,
    Focused,
    Disabled,
    Checkable,
    Checked,
    Mixed, // aria-checked or aria-pressed "mixed", an indeterminate checkbox
    Pressable, // a toggle button: aria-pressed present
    Pressed,
    Selectable,
    Selected,
    Expandable,
    Expanded,
    Required,
    Invalid,
    ReadOnly,
    Editable,
    Multiline,
    Multiselectable,
    Horizontal,
    Vertical,
    Modal,
    Busy,
    Visited,
    Linked,
    HasPopup,
    Default, // a form's default button
    Offscreen,
    Invisible, // visibility: hidden content kept in the tree
    Count_,
};
using States = std::uint64_t;
constexpr States state_bit(State s) { return States { 1 } << static_cast<unsigned>(s); }
std::string_view state_name(State);

// A relation between two nodes (Core-AAM §5.7), by ARIA's name for it and
// the reverse ones the platforms want.
enum class Relation : std::uint8_t {
    LabelledBy,
    LabelFor,
    DescribedBy,
    DescriptionFor,
    Details,
    DetailsFor,
    Controls,
    ControlledBy,
    FlowsTo,
    FlowsFrom,
    ErrorMessage,
    ErrorFor,
    ActiveDescendant,
    Owns,
    OwnedBy,
    Count_,
};
std::string_view relation_name(Relation);

// One node of the tree, as published: plain data, copied whole.
struct NodeData {
    NodeId id = 0;
    NodeId parent = 0;
    std::vector<NodeId> children;
    Role role = Role::Unknown;
    // What the node stands for: an element, a text node, the document; for
    // generated content and markers, the element they belong to. The page's
    // thread may follow it; the bridge's never does.
    dom::Node const* dom = nullptr;
    enum class Kind : std::uint8_t { Document, Element, Text, Generated, Marker, LineBreak } kind = Kind::Element;
    std::string tag; // the element's local name, for the platform's attributes
    std::string name;
    std::string description;
    Semantics::NameSource name_from = Semantics::NameSource::None;
    States states = 0;
    // Properties (Core-AAM §5.5); empty or 0 when not set.
    int level = 0;
    int pos_in_set = 0;
    int set_size = 0;
    std::string role_description;
    std::string key_shortcuts;
    std::string placeholder;
    std::string autocomplete;
    std::string has_popup; // "menu", "listbox", "tree", "grid", "dialog", "true"
    std::string current; // aria-current's token, when not false
    std::string live; // "polite", "assertive"; "off" stated; empty: not a live region
    std::string relevant; // aria-relevant of the live region it is in
    bool atomic = false;
    std::string invalid; // "true", "grammar", "spelling"; empty: valid
    std::string sort; // "ascending", "descending", "other"
    std::string url; // a link's target, resolved
    std::string language;
    // The range value interface: present on range widgets.
    std::optional<double> value_now;
    std::optional<double> value_min;
    std::optional<double> value_max;
    std::optional<double> value_step;
    std::string value_text; // aria-valuetext, or the value of a text field or a combobox
    // The text interface: the node's text (a text leaf's rendered text, a
    // field's value), and for fields the selection, in UTF-16 code units.
    std::string text;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> text_selection;
    std::optional<std::uint32_t> caret;
    // Tables (Core-AAM §5.8): on a table, its row and column counts; on a
    // row, its index; on a cell, its indices, spans and header cells.
    int row_count = 0;
    int column_count = 0;
    int row_index = -1;
    int column_index = -1;
    int row_span = 1;
    int column_span = 1;
    std::vector<NodeId> header_cells;
    // Relations, each to the nodes it names.
    std::vector<std::pair<Relation, std::vector<NodeId>>> relations;
    // Where it is in page coordinates (CSS px); nothing for a node with no box.
    std::optional<Rect> bounds;
    // The actions it takes (Tree::do_action), by name: "click", "press",
    // "jump", "check", "uncheck", "toggle", "select", "expand", "collapse",
    // "activate", "focus", "scroll-into-view".
    std::vector<std::string> actions;
};

// What happened in the tree (the platform bridge turns each into its
// platform's event: AT-SPI's object:children-changed, property-change,
// state-changed, text-changed, text-caret-moved, focus, announcement).
struct Event {
    enum class Type : std::uint8_t {
        ChildrenChanged, // `node`'s children are other than they were
        NameChanged,
        DescriptionChanged,
        RoleChanged,
        StateChanged, // `state` turned to `on`
        ValueChanged,
        TextInserted, // `text` at `offset`
        TextRemoved,
        CaretMoved, // to `offset`
        TextSelectionChanged,
        SelectionChanged, // the selected children of a listbox, tree, grid, tablist
        Focus, // `node` has the focus now
        BoundsChanged,
        Announcement, // a live region spoke: `text`, with `politeness`
        LoadComplete, // the document finished loading
        TreeChanged, // too much changed to say (the queue overflowed): read again
    };
    Event() = default;
    Event(Type type_, NodeId node_)
        : type(type_)
        , node(node_)
    {
    }
    Type type = Type::ChildrenChanged;
    NodeId node = 0;
    State state = State::Count_;
    bool on = false;
    std::uint32_t offset = 0;
    std::string text;
    std::string politeness; // "polite" or "assertive"
};
std::string_view event_name(Event::Type);

// The platform bridge's side: told every event, on the page's thread, in
// the order they happened. Must not block.
class Observer {
public:
    virtual ~Observer() = default;
    virtual void on_event(Event const&) = 0;
    // A new snapshot was published (Tree::snapshot), before the events of
    // the update that made it.
    virtual void on_published() { }
};

// What the tree asks the page to do for an action: the host carries it out
// as the reader's own input would (a click with its default action, focus,
// a value typed), on the page's thread.
class ActionSink {
public:
    virtual ~ActionSink() = default;
    virtual void click(dom::Element&) { }
    virtual void focus(dom::Element&) { }
    virtual void set_value(dom::Element&, std::string const&) { }
    virtual void scroll_into_view(dom::Element&) { }
    virtual void set_text_selection(dom::Element&, std::uint32_t, std::uint32_t) { }
};

// Every node as published at one moment: immutable, shared, read on any
// thread.
struct Snapshot {
    NodeId root = 0;
    std::uint64_t generation = 0;
    std::unordered_map<NodeId, std::shared_ptr<NodeData const>> nodes;
    NodeData const* find(NodeId id) const
    {
        auto const found = nodes.find(id);
        return found == nodes.end() ? nullptr : found->second.get();
    }
};

class Tree {
public:
    // Made for a document, reading its styles and layout through `source`
    // (which must outlive it). Nothing is built until it is first asked.
    Tree(dom::Document& document, PageSource* source);
    ~Tree();
    Tree(Tree const&) = delete;
    Tree& operator=(Tree const&) = delete;

    // How many trees were ever made in this process: what proves that a
    // page nobody inspects pays nothing.
    static std::uint64_t instances();

    Semantics& semantics() { return m_semantics; }
    dom::Document& document() { return *m_document; }

    void set_observer(Observer* observer) { m_observer = observer; }
    void set_action_sink(ActionSink* sink) { m_sink = sink; }

    // Brings the tree up to date with the document, its styles, its layout,
    // the focus and the controls' values, and posts an event for each
    // change since the last update. Cheap when nothing changed (it compares
    // the document's version numbers); otherwise it walks the flat tree
    // once and computes again only what can have changed: the elements the
    // DOM's change marks name (and below an element whose own attributes
    // changed, everything), the names that read what changed (content,
    // aria-labelledby), and every state when styles or layout moved.
    //
    // Which nodes are in the tree:
    //   - the document's root; each element and text node in the flat tree
    //     (shadow trees and slots resolved), aria-owns moving an owned
    //     element under its owner, after the owner's own children;
    //   - not what is hidden (display: none, aria-hidden="true" on it or
    //     above, inert, not rendered: a closed <details>' content, a
    //     dialog that is not open), nor what an aria-modal dialog that is
    //     shown leaves outside it; visibility: hidden leaves its node out
    //     but keeps what inside it is visible again;
    //   - an element is left out and its children take its place when it
    //     is generic or presentational (role none, or a role HTML gives no
    //     node: slot, display: contents without a role), has no name, is
    //     not focusable, is no live region and is no aria-owns owner; the
    //     body and html elements are always left out so (the root stands
    //     for the document);
    //   - the descendants of a role with presentational children (button,
    //     checkbox, image, slider...) are left out but their text, which
    //     becomes text leaves of the node itself;
    //   - a text field (input, textarea) is a leaf: its value is its text;
    //   - a text node with rendered text is a text leaf (StaticText); white
    //     space that renders nothing is no node; <br> is a LineBreak leaf;
    //   - ::before and ::after content and a list item's marker are leaves
    //     of their element (Kind Generated, Marker);
    //   - a frame's document (PageSource::frame_document) is the frame's
    //     child, with a RootWebArea of its own.
    void update();
    // Whether anything changed since the last update (without updating).
    bool stale() const;

    NodeId root();
    NodeData const* node(NodeId id);
    // The node an element or text node stands as; 0 when it is not in the
    // tree.
    NodeId id_of(dom::Node const& node);
    // The node for an element even when it is left out of the tree
    // (the testdriver's question): its own data, computed now.
    std::optional<NodeData> data_for(dom::Element const& element);

    // The deepest node at a page point (CSS px), what a pointer there
    // touches: the element under it, then the text leaf whose boxes hold it.
    NodeId hit_test(float x, float y);
    // The boxes of a text leaf's characters from and to (UTF-16 code units),
    // in page coordinates: one per line it spans.
    std::vector<Rect> text_bounds(NodeId id, std::uint32_t from, std::uint32_t to);
    // The leaf's text split as its inline text boxes are laid out: each run
    // with its character range and box.
    struct InlineTextBox {
        std::uint32_t start = 0;
        std::uint32_t end = 0;
        Rect bounds;
    };
    std::vector<InlineTextBox> inline_text_boxes(NodeId id);

    // Carries out a node's action by name (one of NodeData::actions), through
    // the ActionSink; false when the node has no such action.
    bool do_action(NodeId id, std::string_view action);
    bool set_value(NodeId id, std::string const& value);
    bool focus(NodeId id);
    bool set_text_selection(NodeId id, std::uint32_t start, std::uint32_t end);

    // The text dump: one line per node, indented by depth, with its role,
    // name, the states and properties set, and its bounds. The instrument
    // --dump-a11y prints.
    std::string dump(bool with_bounds = true);

    // The document finished loading: posts LoadComplete.
    void load_complete();

    // The snapshot published at the last update (null before the first).
    std::shared_ptr<Snapshot const> snapshot() const;

    // Queries from another thread: post() may be called on any thread and
    // calls the page's wake function (set by the page's host: it must not
    // block, and should make the page's loop call serve() soon); serve()
    // runs what was posted, on the page's thread.
    using Query = std::function<void(Tree&)>;
    void set_page_wake(std::function<void()> wake);
    void post(Query query);
    void serve();

    // The tree's own state (Tree.cpp).
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
    dom::Document* m_document;
    Semantics m_semantics;
    Observer* m_observer = nullptr;
    ActionSink* m_sink = nullptr;
};

// The platform bridge's thread-safe end: an Observer that queues the
// events for another thread, and the place that thread reads the latest
// snapshot from and posts its queries to.
class Bridge final : public Observer {
public:
    // `wake` is called on the page's thread after events were queued, and
    // must not block: it tells the bridge's thread to look (a write to a
    // pipe, an event loop's wakeup). `capacity` caps the queue.
    explicit Bridge(Tree& tree, std::function<void()> wake = {}, std::size_t capacity = 4096);
    ~Bridge() override;

    // Page thread: the tree's observer. The snapshot is taken over as the
    // tree publishes it; the events are queued.
    void on_event(Event const&) override;
    void on_published() override;

    // Page thread, before the tree goes: the bridge lets go of it. The
    // bridge's thread may hold the Bridge for longer (a shared pointer):
    // from then on its snapshot stays the last one, and a query posted is
    // dropped — never run against a tree that is gone.
    void detach();

    // Any thread.
    std::vector<Event> take_events();
    std::shared_ptr<Snapshot const> snapshot() const;
    // False when the tree is gone and the query was dropped.
    bool post(Tree::Query query);
    bool detached() const;
    std::size_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }

private:
    Tree* m_tree;
    std::function<void()> m_wake;
    std::size_t m_capacity;
    mutable std::mutex m_lock;
    std::deque<Event> m_events;
    std::shared_ptr<Snapshot const> m_snapshot;
    std::atomic<std::size_t> m_dropped { 0 };
};

}
