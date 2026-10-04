#include "dom/Dom.h"

#include <algorithm>

namespace sashfold::dom {

namespace {

bool is_inclusive_ancestor_of(Node const& ancestor, Node const& node)
{
    for (Node const* current = &node; current; current = current->parent()) {
        if (current == &ancestor)
            return true;
    }
    return false;
}

void mark_slots_of(Element& host);

bool is_slottable(Node const& node)
{
    return node.is_element() || node.is_text();
}

// Whether the node or anything below it in its own tree is a <slot>.
bool contains_slot(Node const& node)
{
    if (node.is_element() && static_cast<Element const&>(node).is_slot())
        return true;
    for (Node const* child : node.children()) {
        if (contains_slot(*child))
            return true;
    }
    return false;
}

bool is_style_element(Node const& node)
{
    return node.is_element() && (static_cast<Element const&>(node).is_html("style") || static_cast<Element const&>(node).is_svg("style"));
}

// Whether a node coming to or leaving `parent` changes the sheets of the
// tree: a style element in it, or the text of one.
bool changes_sheets(Node const& parent, Node const& node)
{
    if (is_style_element(parent))
        return true;
    if (is_style_element(node))
        return true;
    for (Node const* child : node.children()) {
        if (changes_sheets(node, *child))
            return true;
    }
    return false;
}

std::string_view attribute_value(Element const& element, std::string_view name)
{
    Attr const* const attribute = element.find_attribute(name);
    return attribute ? std::string_view(attribute->value) : std::string_view();
}

// A host's children are siblings whichever slot each is drawn in: when
// one comes or goes, the slots that show any of them are marked, so that
// a style walk comes to the others (`a + b`, :nth-child()).
void mark_slots_of(Element& host)
{
    for (Element* const slot : host.shadow_root()->slots()) {
        if (!slot->assigned_nodes().empty())
            slot->mark_style_children();
    }
}

// The slot steps of "insert" (DOM §4.2.3, steps 7.4 to 7.6) for one node
// now among `parent`'s children.
void slots_after_insertion(Node& parent, Node& node)
{
    if (parent.is_element() && static_cast<Element&>(parent).shadow_root() != nullptr && is_slottable(node)) {
        assign_a_slot(node);
        mark_slots_of(static_cast<Element&>(parent));
    }
    Node& root = parent.root();
    if (!root.is_shadow_root())
        return;
    // The tree's sheets changed: its root says so, as a new subtree does,
    // and the styles of everything in it are computed again.
    if (changes_sheets(parent, node))
        root.mark_style_subtree();
    if (parent.is_element() && static_cast<Element&>(parent).is_slot() && static_cast<Element&>(parent).assigned_nodes().empty())
        parent.document().signal_slot_change(static_cast<Element&>(parent));
    if (contains_slot(node)) {
        static_cast<ShadowRoot&>(root).slots_changed();
        assign_slottables_for_tree(root);
    }
}

// The slot steps of "remove" (DOM §4.2.3, steps 11 to 13) for a node that
// has just left `parent`, having been assigned to `slot` (or to none).
void slots_after_removal(Node& parent, Node& node, Element* slot)
{
    if (slot != nullptr)
        assign_slottables(*slot);
    if (parent.is_element() && static_cast<Element&>(parent).shadow_root() != nullptr)
        mark_slots_of(static_cast<Element&>(parent));
    Node& root = parent.root();
    if (root.is_shadow_root() && changes_sheets(parent, node))
        root.mark_style_subtree();
    if (root.is_shadow_root() && parent.is_element() && static_cast<Element&>(parent).is_slot()
        && static_cast<Element&>(parent).assigned_nodes().empty())
        parent.document().signal_slot_change(static_cast<Element&>(parent));
    if (contains_slot(node)) {
        if (root.is_shadow_root()) {
            static_cast<ShadowRoot&>(root).slots_changed();
            assign_slottables_for_tree(root);
        }
        assign_slottables_for_tree(node);
    }
}

}

void Node::append_child(Node& child)
{
    insert_before(child, nullptr);
}

Node* Node::style_parent() const
{
    if (m_assigned_slot != nullptr)
        return m_assigned_slot;
    return parent_or_host();
}

// A stamp goes up the ancestors as `descendants` until it meets one that
// already has it: that one's own ancestors were given it when it was. Up
// the way a style walk comes down: through the slot a node is assigned to,
// and out of a shadow tree to its host.
void Node::mark_style_ancestors(std::uint32_t clock)
{
    for (Node* node = style_parent(); node && node->m_style_marks.descendants != clock; node = node->style_parent())
        node->m_style_marks.descendants = clock;
}

void Node::mark_style_self()
{
    std::uint32_t const clock = m_document->style_clock();
    if (m_style_marks.self == clock)
        return;
    m_style_marks.self = clock;
    mark_style_ancestors(clock);
    // A shadow host's child is a sibling of the host's other children
    // wherever each is drawn: the slots that show them are marked, so that
    // a style walk comes to the ones this change may reach.
    if (m_parent != nullptr && m_document->has_shadow_trees() && m_parent->is_element()
        && static_cast<Element*>(m_parent)->shadow_root() != nullptr)
        mark_slots_of(*static_cast<Element*>(m_parent));
}

void Node::mark_style_children()
{
    std::uint32_t const clock = m_document->style_clock();
    if (m_style_marks.children == clock)
        return;
    m_style_marks.children = clock;
    mark_style_ancestors(clock);
}

void Node::mark_style_subtree()
{
    std::uint32_t const clock = m_document->style_clock();
    if (m_style_marks.subtree == clock)
        return;
    m_style_marks.subtree = clock;
    mark_style_ancestors(clock);
}

void Node::mark_style_data()
{
    if (!m_parent)
        return;
    m_parent->mark_style_children();
    // The text of a style element in a shadow tree is that tree's sheet.
    if (m_document->has_shadow_trees() && is_style_element(*m_parent)) {
        if (Node& root = m_parent->root(); root.is_shadow_root())
            root.mark_style_subtree();
    }
}

std::uint32_t Document::advance_style_clock() const
{
    return ++m_style_clock;
}

void Document::note_style_removal(Element const& element)
{
    // The removals are kept for resolvers that look now and then, and for
    // none at all while no one looks (a page nobody draws); past this many
    // they are let go of, and a resolver that has not looked since then
    // computes everything.
    constexpr std::size_t kept_removals = std::size_t(1) << 18;
    if (m_style_removals.size() >= kept_removals) {
        m_style_removals.clear();
        m_style_removals.shrink_to_fit();
        m_style_removals_from = m_style_clock + 1;
    }
    m_style_removals.push_back({ m_style_clock, &element });
    // A state held by what left the tree goes with it: it may be adopted
    // by another document, and freed there.
    for (Element const** held : { &m_hovered, &m_active, &m_focused, &m_target, &m_fullscreen }) {
        if (*held && holds(*held, element))
            *held = nullptr;
    }
}

namespace {

// An element and the elements it sits in, innermost first.
std::vector<Element const*> element_chain(Element const* element)
{
    std::vector<Element const*> chain;
    for (Node const* node = element; node; node = node->style_parent()) {
        if (node->is_element())
            chain.push_back(static_cast<Element const*>(node));
    }
    return chain;
}

// Marks the elements in one chain and not the other: the ones whose
// state turned when the state moved from one element to another.
void mark_chain_difference(Element const* from, Element const* to)
{
    std::vector<Element const*> const left = element_chain(from);
    std::vector<Element const*> const entered = element_chain(to);
    std::size_t shared = 0;
    while (shared < left.size() && shared < entered.size()
        && left[left.size() - 1 - shared] == entered[entered.size() - 1 - shared])
        ++shared;
    for (std::size_t i = 0; i + shared < left.size(); ++i)
        const_cast<Element*>(left[i])->mark_style_self();
    for (std::size_t i = 0; i + shared < entered.size(); ++i)
        const_cast<Element*>(entered[i])->mark_style_self();
}

}

void Document::set_chain_state(Element const*& held, Element const* element)
{
    if (held == element)
        return;
    note_state_change();
    mark_chain_difference(held, element);
    held = element;
}

void Document::set_one_state(Element const*& held, Element const* element)
{
    if (held == element)
        return;
    note_state_change();
    if (held)
        const_cast<Element*>(held)->mark_style_self();
    if (element)
        const_cast<Element*>(element)->mark_style_self();
    held = element;
}

void Document::set_focused(Element const* element, bool visible)
{
    note_state_change();
    if (m_focused == element) {
        if (m_focus_visible != visible && element)
            const_cast<Element*>(element)->mark_style_self();
        m_focus_visible = visible;
        return;
    }
    mark_chain_difference(m_focused, element);
    // The focused element itself turns too when it stays in both chains
    // (focus moved to one of its descendants): :focus left it. And the
    // one focused now, when it was in the old chain already (focus moved
    // from a descendant up to it): :focus came to it.
    if (m_focused)
        const_cast<Element*>(m_focused)->mark_style_self();
    if (element)
        const_cast<Element*>(element)->mark_style_self();
    m_focused = element;
    m_focus_visible = visible;
}

bool Document::holds(Element const* held, Element const& element)
{
    for (Node const* node = held; node; node = node->style_parent()) {
        if (node == &element)
            return true;
    }
    return false;
}

void Node::insert_before(Node& child, Node* reference)
{
    child.remove();
    child.m_parent = this;
    child.mark_style_subtree();
    mark_style_children();
    if (!reference) {
        m_children.push_back(&child);
    } else {
        auto const it = std::find(m_children.begin(), m_children.end(), reference);
        // The insertion steps for live ranges (DOM §4.2.3 "insert"): a boundary
        // in this node past the reference child moves one on.
        auto const index = static_cast<std::uint32_t>(it - m_children.begin());
        for (Range* range : m_document->ranges()) {
            if (!range->live)
                continue;
            if (range->start_node == this && range->start_offset > index)
                ++range->start_offset;
            if (range->end_node == this && range->end_offset > index)
                ++range->end_offset;
        }
        m_children.insert(it, &child);
    }
    if (m_document->has_shadow_trees())
        slots_after_insertion(*this, child);
}

void Node::remove()
{
    if (!m_parent)
        return;
    auto& siblings = m_parent->m_children;
    // The removing steps for live ranges (DOM §4.2.3 "remove"): a boundary
    // inside this node moves to where the node stood, one past it moves back.
    if (!m_document->ranges().empty()) {
        std::uint32_t const at = index();
        for (Range* range : m_document->ranges()) {
            if (!range->live)
                continue;
            if (range->start_node && is_inclusive_ancestor_of(*this, *range->start_node)) {
                range->start_node = m_parent;
                range->start_offset = at;
            }
            if (range->end_node && is_inclusive_ancestor_of(*this, *range->end_node)) {
                range->end_node = m_parent;
                range->end_offset = at;
            }
            if (range->start_node == m_parent && range->start_offset > at)
                --range->start_offset;
            if (range->end_node == m_parent && range->end_offset > at)
                --range->end_offset;
        }
    }
    // The pre-removing steps of the iterators (DOM §6.1): a place inside
    // what is going moves to the node after it, when the iterator has not
    // given its reference yet and there is one within its root, and to
    // the node before it otherwise.
    auto const move_out = [this](Node const& root, Node*& stands_at, bool& before_it) {
        if (!stands_at || !is_inclusive_ancestor_of(*this, *stands_at))
            return;
        if (before_it) {
            Node* next = nullptr;
            for (Node* node = this; node && node != &root && !next; node = node->parent()) {
                if (!node->parent())
                    break;
                std::vector<Node*> const& around = node->parent()->children();
                std::uint32_t const at = node->index();
                if (at + 1 < around.size())
                    next = around[at + 1];
            }
            if (next) {
                stands_at = next;
                return;
            }
            before_it = false;
        }
        Node* before = previous_sibling();
        if (!before) {
            stands_at = m_parent;
            return;
        }
        while (before->last_child())
            before = before->last_child();
        stands_at = before;
    };
    for (IteratorPlace* place : m_document->places()) {
        // What holds the root was never among what the iterator walks:
        // taking it away takes the whole walk with it and moves nothing.
        if (!place->root || is_inclusive_ancestor_of(*this, *place->root))
            continue;
        move_out(*place->root, place->reference, place->before_reference);
        move_out(*place->root, place->candidate, place->before_candidate);
    }
    // Styles are kept only for what is in the document's tree, so only a
    // removal from it has styles to let go of: every element that goes is
    // written down, and the parent is marked for the siblings it leaves.
    // The shadow trees of the hosts among them go with them.
    if (is_connected()) {
        std::vector<Node const*> pending { this };
        while (!pending.empty()) {
            Node const* current = pending.back();
            pending.pop_back();
            if (current->is_element()) {
                m_document->note_style_removal(static_cast<Element const&>(*current));
                if (ShadowRoot const* const shadow = static_cast<Element const&>(*current).shadow_root())
                    pending.push_back(shadow);
            }
            for (Node const* child : current->m_children)
                pending.push_back(child);
        }
    }
    m_parent->mark_style_children();
    siblings.erase(std::remove(siblings.begin(), siblings.end(), this), siblings.end());
    Node* const left = m_parent;
    Element* const slot = m_assigned_slot;
    m_parent = nullptr;
    if (m_document->has_shadow_trees())
        slots_after_removal(*left, *this, slot);
}

std::uint32_t Node::index() const
{
    if (!m_parent)
        return 0;
    auto const& siblings = m_parent->m_children;
    return static_cast<std::uint32_t>(std::find(siblings.begin(), siblings.end(), this) - siblings.begin());
}

Node* Node::previous_sibling() const
{
    if (!m_parent)
        return nullptr;
    auto const& siblings = m_parent->m_children;
    auto const it = std::find(siblings.begin(), siblings.end(), this);
    if (it == siblings.begin())
        return nullptr;
    return *(it - 1);
}

bool Node::is_connected() const
{
    return &shadow_including_root() == m_document;
}

Node& Node::root()
{
    Node* node = this;
    while (node->m_parent)
        node = node->m_parent;
    return *node;
}

Node const& Node::root() const
{
    Node const* node = this;
    while (node->m_parent)
        node = node->m_parent;
    return *node;
}

Node* Node::parent_or_host() const
{
    if (m_parent != nullptr)
        return m_parent;
    return m_is_shadow_root ? &static_cast<ShadowRoot const*>(this)->host() : nullptr;
}

Node& Node::shadow_including_root()
{
    Node* node = this;
    while (Node* const up = node->parent_or_host())
        node = up;
    return *node;
}

Node const& Node::shadow_including_root() const
{
    Node const* node = this;
    while (Node const* const up = node->parent_or_host())
        node = up;
    return *node;
}

void Document::adopt(Node& node)
{
    if (node.m_document == this)
        return;
    node.remove();
    Document& old = *node.m_document;
    // Every node of the subtree moves, the template contents and the
    // shadow trees of the hosts among them included.
    std::vector<Node*> pending { &node };
    while (!pending.empty()) {
        Node* current = pending.back();
        pending.pop_back();
        for (Node* child : current->m_children)
            pending.push_back(child);
        if (current->is_element()) {
            if (Node* content = static_cast<Element*>(current)->template_content())
                pending.push_back(content);
            if (static_cast<Element*>(current)->is_html("base"))
                ++m_base_elements;
            if (ShadowRoot* const shadow = static_cast<Element*>(current)->shadow_root()) {
                pending.push_back(shadow);
                ++m_shadow_roots;
            }
        }
        auto const it = std::find_if(old.m_nodes.begin(), old.m_nodes.end(),
            [current](std::unique_ptr<Node> const& owned) { return owned.get() == current; });
        if (it != old.m_nodes.end()) {
            m_nodes.push_back(std::move(*it));
            old.m_nodes.erase(it);
        }
        current->m_document = this;
        // The old document's stamps mean nothing on this one's clock; the
        // insertion that follows marks the whole subtree as new anyway.
        current->m_style_marks = {};
    }
    // A range with a boundary among the moved nodes is this document's to keep
    // now, and stays the old one's only while a boundary is still its.
    std::vector<Range*> const ranges = old.m_ranges;
    for (Range* range : ranges) {
        auto const holds = [range](Document const& document) {
            return (range->start_node && range->start_node->m_document == &document)
                || (range->end_node && range->end_node->m_document == &document);
        };
        if (!holds(*this))
            continue;
        if (std::find(m_ranges.begin(), m_ranges.end(), range) == m_ranges.end())
            m_ranges.push_back(range);
        if (!holds(old))
            std::erase(old.m_ranges, range);
    }
}

// The first attribute whose qualified name is `name`, in any namespace (DOM
// §4.9 "get an attribute by name").
Attr const* Element::find_attribute(std::string_view name) const
{
    for (Attr const& attribute : m_attributes) {
        if (attribute.has_qualified_name(name))
            return &attribute;
    }
    return nullptr;
}

bool is_valid_custom_element_name(std::string_view name)
{
    if (name.empty() || name.front() < 'a' || name.front() > 'z')
        return false;
    if (name.find('-') == std::string_view::npos)
        return false;
    for (char const c : name) {
        if (c >= 'A' && c <= 'Z')
            return false;
        bool const ordinary = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
        // The rest of what the production allows is non-ASCII, which is
        // taken as it comes; the ASCII it forbids is what is checked here.
        if (!ordinary && static_cast<unsigned char>(c) < 0x80)
            return false;
    }
    static constexpr std::string_view taken[] = { "annotation-xml", "color-profile", "font-face", "font-face-src", "font-face-uri",
        "font-face-format", "font-face-name", "missing-glyph" };
    return std::find(std::begin(taken), std::end(taken), name) == std::end(taken);
}

bool is_valid_shadow_host_name(std::string_view name)
{
    static constexpr std::string_view names[] = { "article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3",
        "h4", "h5", "h6", "header", "main", "nav", "p", "section", "span" };
    return std::find(std::begin(names), std::end(names), name) != std::end(names) || is_valid_custom_element_name(name);
}

// --- Shadow trees and slots -------------------------------------------------

ShadowRoot::ShadowRoot(Document& document, Element& host)
    : DocumentFragment(document)
    , m_host(&host)
{
    m_is_shadow_root = true;
    ++document.m_shadow_roots;
}

std::vector<Element*> const& ShadowRoot::slots() const
{
    if (m_slots_stale) {
        m_slots.clear();
        std::vector<Node*> pending(children().rbegin(), children().rend());
        while (!pending.empty()) {
            Node* const current = pending.back();
            pending.pop_back();
            if (current->is_element() && static_cast<Element*>(current)->is_slot())
                m_slots.push_back(static_cast<Element*>(current));
            for (auto child = current->children().rbegin(); child != current->children().rend(); ++child)
                pending.push_back(*child);
        }
        m_slots_stale = false;
    }
    return m_slots;
}

Element::Slotting& Element::slotting()
{
    if (!m_slotting)
        m_slotting = std::make_unique<Slotting>();
    return *m_slotting;
}

ShadowRoot& Element::attach_shadow()
{
    Slotting& held = slotting();
    if (held.shadow_root == nullptr) {
        held.shadow_root = document().create<ShadowRoot>(*this);
        // The children are no longer what is drawn in this element's place:
        // none is, until a slot of the new tree takes it.
        mark_style_children();
        for (Node* const child : children())
            document().forget_styles_in(*child);
    }
    return *held.shadow_root;
}

std::vector<Node*> const& Element::assigned_nodes() const
{
    static std::vector<Node*> const none;
    return m_slotting ? m_slotting->assigned_nodes : none;
}

std::vector<Node*> const& Element::manually_assigned_nodes() const
{
    static std::vector<Node*> const none;
    return m_slotting ? m_slotting->manually_assigned_nodes : none;
}

void Element::set_manually_assigned_nodes(std::vector<Node*> nodes)
{
    slotting().manually_assigned_nodes = std::move(nodes);
}

void Document::forget_styles_in(Node& node)
{
    if (!node.is_connected())
        return;
    // Everything drawn under the node goes with it: what is in it, the
    // shadow trees of the hosts among that, and what the slots among it
    // were assigned — nodes that live elsewhere in the tree and were
    // drawn here.
    std::vector<Node const*> pending { &node };
    while (!pending.empty()) {
        Node const* const current = pending.back();
        pending.pop_back();
        if (current->is_element()) {
            auto const& element = static_cast<Element const&>(*current);
            note_style_removal(element);
            if (ShadowRoot const* const shadow = element.shadow_root())
                pending.push_back(shadow);
            for (Node const* const assigned : element.assigned_nodes())
                pending.push_back(assigned);
        }
        for (Node const* const child : current->children())
            pending.push_back(child);
    }
}

void Document::signal_slot_change(Element& slot)
{
    // A document no script watches (a parser's, a template's) tells no one.
    if (!on_slot_signal)
        return;
    if (std::find(signal_slots.begin(), signal_slots.end(), &slot) == signal_slots.end())
        signal_slots.push_back(&slot);
    on_slot_signal();
}

Element* find_slot(Node const& slottable, bool open_only)
{
    Node const* const parent = slottable.parent();
    if (parent == nullptr || !parent->is_element())
        return nullptr;
    ShadowRoot const* const shadow = static_cast<Element const*>(parent)->shadow_root();
    if (shadow == nullptr || (open_only && shadow->mode != ShadowRoot::Mode::Open))
        return nullptr;
    if (shadow->slot_assignment == ShadowRoot::SlotAssignment::Manual) {
        for (Element* const slot : shadow->slots()) {
            std::vector<Node*> const& named = slot->manually_assigned_nodes();
            if (std::find(named.begin(), named.end(), &slottable) != named.end())
                return slot;
        }
        return nullptr;
    }
    std::string_view const name = slottable.is_element() ? attribute_value(static_cast<Element const&>(slottable), "slot") : std::string_view();
    for (Element* const slot : shadow->slots()) {
        if (attribute_value(*slot, "name") == name)
            return slot;
    }
    return nullptr;
}

std::vector<Node*> find_slottables(Element const& slot)
{
    std::vector<Node*> result;
    Node const& root = slot.root();
    if (!root.is_shadow_root())
        return result;
    auto const& shadow = static_cast<ShadowRoot const&>(root);
    Element& host = shadow.host();
    if (shadow.slot_assignment == ShadowRoot::SlotAssignment::Manual) {
        for (Node* const slottable : slot.manually_assigned_nodes()) {
            if (slottable->parent() == &host)
                result.push_back(slottable);
        }
        return result;
    }
    for (Node* const child : host.children()) {
        if (is_slottable(*child) && find_slot(*child) == &slot)
            result.push_back(child);
    }
    return result;
}

std::vector<Node*> find_flattened_slottables(Element const& slot)
{
    std::vector<Node*> result;
    if (!slot.root().is_shadow_root())
        return result;
    std::vector<Node*> slottables = find_slottables(slot);
    if (slottables.empty()) {
        for (Node* const child : slot.children()) {
            if (is_slottable(*child))
                slottables.push_back(child);
        }
    }
    for (Node* const node : slottables) {
        if (node->is_element() && static_cast<Element*>(node)->is_slot() && node->root().is_shadow_root()) {
            std::vector<Node*> const inner = find_flattened_slottables(*static_cast<Element*>(node));
            result.insert(result.end(), inner.begin(), inner.end());
        } else {
            result.push_back(node);
        }
    }
    return result;
}

void assign_slottables(Element& slot)
{
    std::vector<Node*> slottables = find_slottables(slot);
    std::vector<Node*> const& old = slot.assigned_nodes();
    if (slottables == old)
        return;
    slot.document().signal_slot_change(slot);
    // What the slot shows changed, and so did where each node that came
    // or went is drawn and what it inherits.
    slot.mark_style_children();
    for (Node* const was : old) {
        if (was->m_assigned_slot == &slot && std::find(slottables.begin(), slottables.end(), was) == slottables.end()) {
            was->m_assigned_slot = nullptr;
            was->mark_style_subtree();
            // Not drawn any more, unless another slot takes it.
            slot.document().forget_styles_in(*was);
        }
    }
    for (Node* const now : slottables) {
        if (now->m_assigned_slot != &slot) {
            // Drawn somewhere else from now on, or — when this slot is not
            // drawn itself — nowhere: what was kept for it goes, and a
            // style walk that comes to it finds it new.
            slot.document().forget_styles_in(*now);
            now->m_assigned_slot = &slot;
            now->mark_style_subtree();
        }
    }
    // A slot that showed its own children shows them no longer, or does again.
    for (Node* const child : slot.children()) {
        child->mark_style_subtree();
        if (old.empty() && !slottables.empty())
            slot.document().forget_styles_in(*child);
    }
    slot.slotting().assigned_nodes = std::move(slottables);
}

void assign_slottables_for_tree(Node& root)
{
    if (root.is_shadow_root()) {
        for (Element* const slot : static_cast<ShadowRoot&>(root).slots())
            assign_slottables(*slot);
        return;
    }
    // A tree that is not a shadow tree: its slots take nothing, and let go
    // of what they had.
    std::vector<Node*> pending { &root };
    while (!pending.empty()) {
        Node* const current = pending.back();
        pending.pop_back();
        if (current->is_element() && static_cast<Element*>(current)->is_slot() && !static_cast<Element*>(current)->assigned_nodes().empty())
            assign_slottables(*static_cast<Element*>(current));
        for (Node* const child : current->children())
            pending.push_back(child);
    }
}

void assign_a_slot(Node& slottable)
{
    if (Element* const slot = find_slot(slottable))
        assign_slottables(*slot);
}

void slot_attribute_changed(Element& element)
{
    if (!element.document().has_shadow_trees())
        return;
    if (Element* const slot = element.assigned_slot())
        assign_slottables(*slot);
    assign_a_slot(element);
}

void slot_name_changed(Element& slot)
{
    if (!slot.document().has_shadow_trees())
        return;
    Node& root = slot.root();
    if (root.is_shadow_root()) {
        static_cast<ShadowRoot&>(root).slots_changed();
        assign_slottables_for_tree(root);
    }
}

std::vector<Node*> const& flat_children(Node const& node)
{
    if (node.is_element()) {
        auto const& element = static_cast<Element const&>(node);
        if (ShadowRoot const* const shadow = element.shadow_root())
            return shadow->children();
        if (!element.assigned_nodes().empty())
            return element.assigned_nodes();
    }
    return node.children();
}

Node* flat_parent(Node const& node)
{
    if (Element* const slot = node.assigned_slot())
        return slot;
    Node* const parent = node.parent();
    if (parent == nullptr)
        return nullptr;
    if (parent->is_shadow_root())
        return &static_cast<ShadowRoot*>(parent)->host();
    if (parent->is_element()) {
        auto const& element = static_cast<Element const&>(*parent);
        // A host's child that no slot takes, and a slot's own child while
        // the slot shows what it was assigned, are not drawn.
        if (element.shadow_root() != nullptr || !element.assigned_nodes().empty())
            return nullptr;
    }
    return parent;
}

void clone_shadow_root(Element const& source, Element& clone)
{
    ShadowRoot const* const shadow = source.shadow_root();
    if (shadow == nullptr || !shadow->clonable || clone.shadow_root() != nullptr)
        return;
    ShadowRoot& copy = clone.attach_shadow();
    copy.mode = shadow->mode;
    copy.slot_assignment = shadow->slot_assignment;
    copy.delegates_focus = shadow->delegates_focus;
    copy.clonable = true;
    copy.serializable = shadow->serializable;
    copy.declarative = shadow->declarative;
    for (Node const* child : shadow->children())
        copy.append_child(*clone_subtree(*child, clone.document()));
}

Node* clone_subtree(Node const& node, Document& document)
{
    switch (node.type()) {
    case NodeType::Element: {
        auto const& element = static_cast<Element const&>(node);
        Element* clone = document.create<Element>(element.namespace_uri(), element.local_name());
        clone->attributes() = element.attributes();
        if (Node* content = element.template_content())
            clone->set_template_content(clone_subtree(*content, document));
        clone_shadow_root(element, *clone);
        for (Node const* child : node.children())
            clone->append_child(*clone_subtree(*child, document));
        return clone;
    }
    case NodeType::Text: {
        auto const& text = static_cast<Text const&>(node);
        Text* clone = document.create<Text>();
        clone->data = text.data;
        clone->cdata_section = text.cdata_section;
        return clone;
    }
    case NodeType::Comment: {
        Comment* clone = document.create<Comment>();
        clone->data = static_cast<Comment const&>(node).data;
        return clone;
    }
    case NodeType::ProcessingInstruction: {
        auto const& instruction = static_cast<ProcessingInstruction const&>(node);
        ProcessingInstruction* clone = document.create<ProcessingInstruction>();
        clone->target = instruction.target;
        clone->data = instruction.data;
        return clone;
    }
    case NodeType::DocumentFragment: {
        DocumentFragment* clone = document.create<DocumentFragment>();
        for (Node const* child : node.children())
            clone->append_child(*clone_subtree(*child, document));
        return clone;
    }
    case NodeType::DocumentType: {
        auto const& doctype = static_cast<DocumentType const&>(node);
        DocumentType* clone = document.create<DocumentType>();
        clone->name = doctype.name;
        clone->public_identifier = doctype.public_identifier;
        clone->system_identifier = doctype.system_identifier;
        return clone;
    }
    case NodeType::Document:
        break; // a document is never cloned as a subtree
    }
    return document.create<DocumentFragment>();
}

Document::~Document()
{
    // A node lives as long as its document, so a range still holding one of
    // this document's nodes lets go of both its boundaries.
    std::vector<Range*> const ranges = m_ranges;
    for (Range* range : ranges)
        release_range(*range);
    m_ranges.clear();
    // And an iterator's place among them holds nothing from here on.
    for (IteratorPlace* place : m_places) {
        place->root = nullptr;
        place->reference = nullptr;
        place->candidate = nullptr;
    }
    m_places.clear();
}

void hold_place(IteratorPlace& place)
{
    if (place.root)
        place.root->document().m_places.push_back(&place);
}

void release_place(IteratorPlace& place)
{
    if (place.root)
        std::erase(place.root->document().m_places, &place);
    place.root = nullptr;
    place.reference = nullptr;
    place.candidate = nullptr;
}

void set_range(Range& range, Node* start_node, std::uint32_t start_offset, Node* end_node, std::uint32_t end_offset)
{
    auto const document_of = [](Node const* node) { return node ? &node->document() : nullptr; };
    Document* const old_start = document_of(range.start_node);
    Document* const old_end = document_of(range.end_node);
    range.start_node = start_node;
    range.start_offset = start_offset;
    range.end_node = end_node;
    range.end_offset = end_offset;
    Document* const new_start = document_of(start_node);
    Document* const new_end = document_of(end_node);
    for (Document* old : { old_start, old_end }) {
        if (old && old != new_start && old != new_end)
            std::erase(old->m_ranges, &range);
    }
    for (Document* now : { new_start, new_end }) {
        if (now && std::find(now->m_ranges.begin(), now->m_ranges.end(), &range) == now->m_ranges.end())
            now->m_ranges.push_back(&range);
    }
}

void release_range(Range& range)
{
    for (Node* node : { range.start_node, range.end_node }) {
        if (node)
            std::erase(node->document().m_ranges, &range);
    }
    range.start_node = nullptr;
    range.start_offset = 0;
    range.end_node = nullptr;
    range.end_offset = 0;
}

void ranges_data_replaced(Node& node, std::uint32_t offset, std::uint32_t count, std::uint32_t inserted)
{
    auto const shift = [&](Node const* boundary, std::uint32_t& at) {
        if (boundary != &node)
            return;
        if (at > offset && at <= offset + count)
            at = offset;
        else if (at > offset + count)
            at = at + inserted - count;
    };
    for (Range* range : node.document().ranges()) {
        if (!range->live)
            continue;
        shift(range->start_node, range->start_offset);
        shift(range->end_node, range->end_offset);
    }
}

void ranges_text_split(Node& node, std::uint32_t offset, Node& new_node)
{
    Node const* const parent = node.parent();
    std::uint32_t const after = node.index() + 1;
    auto const shift = [&](Node*& boundary, std::uint32_t& at) {
        if (boundary == &node && at > offset) {
            boundary = &new_node;
            at -= offset;
        } else if (parent && boundary == parent && at == after) {
            ++at;
        }
    };
    for (Range* range : node.document().ranges()) {
        if (!range->live)
            continue;
        shift(range->start_node, range->start_offset);
        shift(range->end_node, range->end_offset);
    }
}

void ranges_text_merged(Node& into, Node& merged, std::uint32_t length)
{
    Node const* const parent = merged.parent();
    std::uint32_t const index = merged.index();
    auto const shift = [&](Node*& boundary, std::uint32_t& at) {
        if (boundary == &merged) {
            boundary = &into;
            at += length;
        } else if (parent && boundary == parent && at == index) {
            boundary = &into;
            at = length;
        }
    };
    for (Range* range : into.document().ranges()) {
        if (!range->live)
            continue;
        shift(range->start_node, range->start_offset);
        shift(range->end_node, range->end_offset);
    }
}

}
