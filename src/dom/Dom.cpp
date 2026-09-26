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

}

void Node::append_child(Node& child)
{
    insert_before(child, nullptr);
}

// A stamp goes up the ancestors as `descendants` until it meets one that
// already has it: that one's own ancestors were given it when it was.
void Node::mark_style_ancestors(std::uint32_t clock)
{
    for (Node* node = m_parent; node && node->m_style_marks.descendants != clock; node = node->m_parent)
        node->m_style_marks.descendants = clock;
}

void Node::mark_style_self()
{
    std::uint32_t const clock = m_document->style_clock();
    if (m_style_marks.self == clock)
        return;
    m_style_marks.self = clock;
    mark_style_ancestors(clock);
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
    if (m_parent)
        m_parent->mark_style_children();
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
}

void Node::insert_before(Node& child, Node* reference)
{
    child.remove();
    child.m_parent = this;
    child.mark_style_subtree();
    mark_style_children();
    if (!reference) {
        m_children.push_back(&child);
        return;
    }
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
    if (is_connected()) {
        std::vector<Node const*> pending { this };
        while (!pending.empty()) {
            Node const* current = pending.back();
            pending.pop_back();
            if (current->is_element())
                m_document->note_style_removal(static_cast<Element const&>(*current));
            for (Node const* child : current->m_children)
                pending.push_back(child);
        }
    }
    m_parent->mark_style_children();
    siblings.erase(std::remove(siblings.begin(), siblings.end(), this), siblings.end());
    m_parent = nullptr;
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
    return &root() == m_document;
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

void Document::adopt(Node& node)
{
    if (node.m_document == this)
        return;
    node.remove();
    Document& old = *node.m_document;
    // Every node of the subtree moves, the template contents included.
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

Node* clone_subtree(Node const& node, Document& document)
{
    switch (node.type()) {
    case NodeType::Element: {
        auto const& element = static_cast<Element const&>(node);
        Element* clone = document.create<Element>(element.namespace_uri(), element.local_name());
        clone->attributes() = element.attributes();
        if (Node* content = element.template_content())
            clone->set_template_content(clone_subtree(*content, document));
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
