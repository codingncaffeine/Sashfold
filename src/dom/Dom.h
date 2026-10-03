#pragma once

// The document tree the HTML parser builds. Ownership model for this stage:
// the Document owns every node (a node soup of unique_ptrs); tree structure
// is raw parent/children links, so reparenting — which tree construction does
// constantly (foster parenting, the adoption agency) — is pointer surgery,
// never an ownership move. The JS-facing lifetime model is a separate, later
// decision (see the plan's DOM-lifetime ADR note).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sashfold::js {
class Object;
}

namespace sashfold::dom {

namespace ns {
inline constexpr std::string_view html = "http://www.w3.org/1999/xhtml";
inline constexpr std::string_view svg = "http://www.w3.org/2000/svg";
inline constexpr std::string_view mathml = "http://www.w3.org/1998/Math/MathML";
inline constexpr std::string_view xlink = "http://www.w3.org/1999/xlink";
inline constexpr std::string_view xml = "http://www.w3.org/XML/1998/namespace";
inline constexpr std::string_view xmlns = "http://www.w3.org/2000/xmlns/";
}

enum class NodeType {
    Document,
    DocumentType,
    DocumentFragment,
    Element,
    Text,
    Comment,
    ProcessingInstruction,
};

enum class QuirksMode {
    No,
    Limited,
    Yes,
};

class Document;
class Node;

// A range's boundary points (DOM §5): nodes and offsets, in UTF-16 code units
// into a node with data and in children otherwise. The documents of its nodes
// keep a range, so their tree's own insertions and removals move a live one,
// and a document that ends first lets go of it: both boundaries become null.
struct Range {
    Node* start_node = nullptr;
    std::uint32_t start_offset = 0;
    Node* end_node = nullptr;
    std::uint32_t end_offset = 0;
    bool live = true; // a StaticRange is kept, never moved
};

// Where a NodeIterator stands (DOM §6.1): the node it last gave, or is about
// to give, and which side of it the iterator is on. Kept with the document
// of its root so that removing a node can move the place out of what is
// removed — the iterator's pre-removing steps — as a live range's
// boundaries are moved.
struct IteratorPlace {
    Node* root = nullptr;
    Node* reference = nullptr;
    bool before_reference = true;
    // Where a traversal in flight has got to, while the filter is asked
    // about that node: the reference stays the last node given until the
    // filter accepts, and a removal the filter makes moves this one too.
    // Null between traversals.
    Node* candidate = nullptr;
    bool before_candidate = true;
};

class Node {
public:
    Node(Document& document, NodeType type)
        : m_document(&document)
        , m_type(type)
    {
    }
    virtual ~Node() = default;

    NodeType type() const { return m_type; }
    Document& document() const { return *m_document; }

    Node* parent() const { return m_parent; }
    std::vector<Node*> const& children() const { return m_children; }
    Node* last_child() const { return m_children.empty() ? nullptr : m_children.back(); }

    // Detaches from any current parent first.
    void append_child(Node& child);
    void insert_before(Node& child, Node* reference); // nullptr reference == append
    void remove(); // detach this node from its parent

    Node* previous_sibling() const;
    // The position among the parent's children; 0 without a parent.
    std::uint32_t index() const;

    bool is_element() const { return m_type == NodeType::Element; }
    bool is_text() const { return m_type == NodeType::Text; }

    // Whether this node is in its document's tree (DOM §4.2.1 "connected").
    bool is_connected() const;
    // The node at the top of the tree this one is in: the document, or the
    // root of a detached subtree.
    Node& root();
    Node const& root() const;

    // The one script object standing for this node, cached here for the
    // node's lifetime (ADR 0001 §1); null until a script first reaches it.
    // The bindings own it: the script heap's collector decides when it goes,
    // and clears this slot when it does.
    js::Object* wrapper = nullptr;

    // What changed here since styles were last computed from the tree, each
    // as the document's style clock stood when it happened (0: never). A
    // resolver that last read the tree at clock c recomputes what carries a
    // stamp of c or later, so two of them over one document (the shell's,
    // and the one a script's questions go to) each see every change since
    // their own last look, and nothing is ever cleared.
    //   self: this element's own attributes or state changed;
    //   children: a child was inserted or removed, or a child's text changed;
    //   subtree: this node was inserted, and everything in it is new;
    //   descendants: some node below carries one of the three.
    struct StyleMarks {
        std::uint32_t self = 0;
        std::uint32_t children = 0;
        std::uint32_t subtree = 0;
        std::uint32_t descendants = 0;
    };
    StyleMarks const& style_marks() const { return m_style_marks; }
    void mark_style_self();
    void mark_style_children();
    void mark_style_subtree();
    // This node's text changed: its parent's children are marked, since
    // :empty and dir=auto read the text an element holds.
    void mark_style_data();

private:
    friend class Document;
    void mark_style_ancestors(std::uint32_t clock);
    Document* m_document;
    NodeType m_type;
    Node* m_parent = nullptr;
    std::vector<Node*> m_children;
    StyleMarks m_style_marks;
};

struct Attr {
    std::string local_name;
    std::string value;
    std::string prefix; // "" for ordinary attributes
    std::string namespace_uri; // "" for none
    // A style attribute written through element.style rather than by
    // markup or setAttribute: a page's style policy governs the latter
    // two and lets this one through, as the CSSOM is not inline style.
    bool from_cssom = false;

    // The name as the html5lib tree format prints and sorts it.
    std::string display_name() const
    {
        return prefix.empty() ? local_name : prefix + " " + local_name;
    }
    // The qualified name (DOM §4.9.2): the local name, after the prefix and a
    // colon when there is a prefix.
    std::string qualified_name() const
    {
        return prefix.empty() ? local_name : prefix + ":" + local_name;
    }
    bool has_qualified_name(std::string_view name) const
    {
        if (prefix.empty())
            return local_name == name;
        return name.size() == prefix.size() + 1 + local_name.size() && name.starts_with(prefix) && name[prefix.size()] == ':'
            && name.ends_with(local_name);
    }
};

class Element : public Node {
public:
    Element(Document& document, std::string namespace_uri, std::string local_name)
        : Node(document, NodeType::Element)
        , m_namespace_uri(std::move(namespace_uri))
        , m_local_name(std::move(local_name))
    {
    }

    std::string const& namespace_uri() const { return m_namespace_uri; }
    std::string const& local_name() const { return m_local_name; }

    bool is_html() const { return m_namespace_uri == ns::html; }
    bool is_html(std::string_view name) const { return is_html() && m_local_name == name; }
    bool is_svg(std::string_view name) const { return m_namespace_uri == ns::svg && m_local_name == name; }
    bool is_mathml(std::string_view name) const { return m_namespace_uri == ns::mathml && m_local_name == name; }

    // Whoever takes the attributes to change them has changed this element's
    // style inputs, so the mutable view marks the element as it is handed
    // out; a reader takes the const one.
    std::vector<Attr>& attributes()
    {
        mark_style_self();
        return m_attributes;
    }
    std::vector<Attr> const& attributes() const { return m_attributes; }
    Attr const* find_attribute(std::string_view name) const;
    bool has_attribute(std::string_view name) const { return find_attribute(name) != nullptr; }

    // <template> only: its parsed contents live in a separate fragment.
    Node* template_content() const { return m_template_content; }
    void set_template_content(Node* content) { m_template_content = content; }

    // A custom element its definition has made (HTML §4.13.6, "custom"):
    // what :defined asks of an element with a custom element's name, which
    // is undefined until then. The bindings say it, and mark the change.
    bool custom_defined() const { return m_custom_defined; }
    void set_custom_defined(bool defined)
    {
        if (defined != m_custom_defined) {
            m_custom_defined = defined;
            mark_style_self();
        }
    }

private:
    std::string m_namespace_uri;
    std::string m_local_name;
    std::vector<Attr> m_attributes;
    Node* m_template_content = nullptr;
    bool m_custom_defined = false;
};

class Text : public Node {
public:
    explicit Text(Document& document)
        : Node(document, NodeType::Text)
    {
    }
    std::string data; // UTF-8 (WTF-8 internally)
    // A CDATASection (DOM §4.12) is a Text node in all but its name and
    // number, and in not being an exclusive Text node: normalize() leaves it.
    bool cdata_section = false;
};

class Comment : public Node {
public:
    explicit Comment(Document& document)
        : Node(document, NodeType::Comment)
    {
    }
    std::string data;
};

class ProcessingInstruction : public Node {
public:
    explicit ProcessingInstruction(Document& document)
        : Node(document, NodeType::ProcessingInstruction)
    {
    }
    std::string target;
    std::string data;
};

class DocumentType : public Node {
public:
    explicit DocumentType(Document& document)
        : Node(document, NodeType::DocumentType)
    {
    }
    std::string name;
    std::string public_identifier;
    std::string system_identifier;
};

class DocumentFragment : public Node {
public:
    explicit DocumentFragment(Document& document)
        : Node(document, NodeType::DocumentFragment)
    {
    }
};

// A stylesheet as the CSS object model has it (CSSOM §6.1), shared between
// the script's CSSStyleSheet and the document the sheets are collected
// from: once a script changes the sheet through the object model, its text
// stands for it in place of what its element carries — for as long as the
// element still carries `source`, the text the object model read it from
// (a script that rewrites a <style> element's text replaces the sheet).
// `disabled` leaves it out of the cascade; `version` moves with every
// change, so that the sheets are collected again. The text is written only
// when it is asked for, by `write`: a page inserting rules one by one, as
// the style-in-script libraries do, asks for it once per collection.
struct ScriptedSheet {
    bool changed = false;
    bool disabled = false;
    std::uint64_t version = 0;
    std::string source;
    std::function<std::string()> write;

    std::string const& text()
    {
        if (m_stale && write) {
            m_text = write();
            m_stale = false;
        }
        return m_text;
    }
    // A change through the object model: written again when next asked.
    void touch()
    {
        changed = true;
        m_stale = true;
        ++version;
    }

private:
    std::string m_text;
    bool m_stale = false;
};

class Document : public Node {
public:
    Document()
        : Node(*this, NodeType::Document)
    {
    }
    ~Document() override;

    // The sheets the object model has a say in: those of <style> and
    // <link> elements, by element (a document's nodes live as long as it
    // does), and those adopted onto the document or onto a shadow root,
    // by the root, in the order adopted.
    std::vector<std::pair<Element const*, std::shared_ptr<ScriptedSheet>>> scripted_sheets;
    std::vector<std::pair<Node const*, std::vector<std::shared_ptr<ScriptedSheet>>>> adopted_sheets;
    std::shared_ptr<ScriptedSheet> scripted_sheet(Element const& element) const
    {
        for (auto const& [owner, sheet] : scripted_sheets) {
            if (owner == &element)
                return sheet;
        }
        return nullptr;
    }
    std::vector<std::shared_ptr<ScriptedSheet>> const* adopted(Node const& root) const
    {
        for (auto const& [owner, sheets] : adopted_sheets) {
            if (owner == &root)
                return &sheets;
        }
        return nullptr;
    }

    // The states selectors ask after that the tree does not hold
    // (selectors-4 §9, §10, §13): the element under the pointer, the one
    // being pressed, the one with the focus and whether that focus is shown,
    // the URL's target, the one shown full screen. Each setter marks the
    // elements whose state it turned — for hover and press the element and
    // its ancestors, for focus the focused one and the ancestors that hold
    // it (:focus-within) — so a restyle computes those and what they reach.
    Element const* hovered() const { return m_hovered; }
    Element const* active() const { return m_active; }
    Element const* focused() const { return m_focused; }
    bool focus_visible() const { return m_focus_visible; }
    Element const* target() const { return m_target; }
    Element const* fullscreen() const { return m_fullscreen; }
    void set_hovered(Element const* element) { set_chain_state(m_hovered, element); }
    void set_active(Element const* element) { set_chain_state(m_active, element); }
    void set_focused(Element const* element, bool visible);
    void set_target(Element const* element) { set_one_state(m_target, element); }
    void set_fullscreen(Element const* element) { set_one_state(m_fullscreen, element); }
    // Whether the element or one inside it is hovered, pressed, focused.
    bool holds_hover(Element const& element) const { return holds(m_hovered, element); }
    bool holds_active(Element const& element) const { return holds(m_active, element); }
    bool holds_focus(Element const& element) const { return holds(m_focused, element); }
    // Moves with every change of those states, and with a control's
    // checkedness or value that the host changed: a host that restyles only
    // after a script watches this as well.
    std::uint64_t state_version() const { return m_state_version; }
    void note_state_change() { ++m_state_version; }
    // A form control's live checkedness, which the host keeps (what the
    // reader toggled, what a script set); nothing for one the host has no
    // say over, whose checked attribute then answers. Whoever changes it
    // marks the element.
    std::function<std::optional<bool>(Element const&)> live_checked;
    // The same for a text control's value (what was typed), which
    // :placeholder-shown and the validity states read.
    std::function<std::optional<std::string>(Element const&)> live_value;

    QuirksMode quirks_mode = QuirksMode::No;
    // Whether this is an XML document rather than an HTML one (DOM §4.5), and
    // the content type a document made by script reports; a loaded
    // document's content type is its realm's.
    bool xml = false;
    // Made by `new Document()`: a Document to script, neither an
    // HTMLDocument nor an XMLDocument (DOM §4.5.1).
    bool plain = false;
    std::string content_type = "text/html";

    template<typename T, typename... Args>
    T* create(Args&&... args)
    {
        auto node = std::make_unique<T>(*this, std::forward<Args>(args)...);
        T* raw = node.get();
        m_nodes.push_back(std::move(node));
        if constexpr (std::is_base_of_v<Element, T>) {
            if (raw->is_html("base"))
                ++m_base_elements;
        }
        return raw;
    }

    // Whether a base element was ever made for this document or moved into
    // it. Most documents have none, and for them the document base URL is
    // the document's own, with no walk of the tree to find that out.
    bool may_have_base() const { return m_base_elements != 0; }
    // How many there have been: a number that moves when the parser makes
    // one, for whoever keeps the base URL it last worked out.
    std::uint32_t base_elements_made() const { return m_base_elements; }

    // Moves `node` and its whole subtree into this document's arena from
    // the document that made them (DOM §4.2.4 "adopt"), so a node parsed or
    // created by another document can be inserted here and outlive it. A
    // node already of this document is left alone. The node is detached from
    // its old parent first.
    void adopt(Node& node);

    // Every node this document owns, in its tree or not: one made and never
    // inserted, a removed subtree, template contents.
    std::vector<std::unique_ptr<Node>> const& owned_nodes() const { return m_nodes; }

    // The ranges with a boundary among this document's nodes.
    std::vector<Range*> const& ranges() const { return m_ranges; }
    // The iterators' places whose root is one of this document's nodes.
    std::vector<IteratorPlace*> const& places() const { return m_places; }

    // The clock the nodes' style marks are stamped with. A resolver moves
    // it on as it reads the tree, and keeps the value it moved it to: what
    // changes after that carries that stamp or a later one. Moving the clock
    // changes nothing in the tree, so a reader of a const document may.
    std::uint32_t style_clock() const { return m_style_clock; }
    std::uint32_t advance_style_clock() const;
    // Everything must be computed again: a change no mark on a node can say,
    // such as the fonts the page is measured in. The stamp it was made at.
    void mark_style_everything() { m_style_everything = m_style_clock; }
    std::uint32_t style_everything_at() const { return m_style_everything; }
    // The elements taken out of the document's tree, each with the clock
    // when it went, so that a resolver can drop the styles it keeps for
    // them without ever reading the element (which another document may
    // have adopted and freed since). The list is let go of when it grows
    // long; a resolver that last looked before `style_removals_from` cannot
    // know what went, and computes everything.
    struct StyleRemoval {
        std::uint32_t at;
        Element const* element;
    };
    std::vector<StyleRemoval> const& style_removals() const { return m_style_removals; }
    std::uint32_t style_removals_from() const { return m_style_removals_from; }

private:
    friend class Node;
    friend void set_range(Range&, Node*, std::uint32_t, Node*, std::uint32_t);
    friend void release_range(Range&);
    friend void hold_place(IteratorPlace&);
    friend void release_place(IteratorPlace&);
    void note_style_removal(Element const& element);
    void set_chain_state(Element const*& held, Element const* element);
    void set_one_state(Element const*& held, Element const* element);
    static bool holds(Element const* held, Element const& element);
    Element const* m_hovered = nullptr;
    Element const* m_active = nullptr;
    Element const* m_focused = nullptr;
    bool m_focus_visible = false;
    Element const* m_target = nullptr;
    Element const* m_fullscreen = nullptr;
    std::uint64_t m_state_version = 0;
    std::vector<std::unique_ptr<Node>> m_nodes;
    std::vector<Range*> m_ranges;
    std::vector<IteratorPlace*> m_places;
    std::uint32_t m_base_elements = 0;
    mutable std::uint32_t m_style_clock = 1;
    std::uint32_t m_style_everything = 0;
    std::vector<StyleRemoval> m_style_removals;
    std::uint32_t m_style_removals_from = 0;
};

// Deep-copies a subtree; the clone's nodes are owned by `document`.
Node* clone_subtree(Node const& node, Document& document);

// Sets a range's boundaries, keeping the range with its nodes' documents.
void set_range(Range&, Node* start_node, std::uint32_t start_offset, Node* end_node, std::uint32_t end_offset);
// Takes a range from its documents; both boundaries become null.
void release_range(Range&);
// Keeps a place with its root's document for as long as it is held, and
// lets it go; a place whose document ended holds nothing.
void hold_place(IteratorPlace&);
void release_place(IteratorPlace&);
// The live range steps of the character data algorithms, in UTF-16 code
// units: `count` units at `offset` replaced by `inserted` of them (DOM §4.10
// "replace data"), a Text node split at `offset` once `new_node` follows it
// (§4.11 "split a Text node"), and normalize() folding `merged` into `into`
// at `length` (§4.4).
void ranges_data_replaced(Node&, std::uint32_t offset, std::uint32_t count, std::uint32_t inserted);
void ranges_text_split(Node&, std::uint32_t offset, Node& new_node);
void ranges_text_merged(Node& into, Node& merged, std::uint32_t length);

}
