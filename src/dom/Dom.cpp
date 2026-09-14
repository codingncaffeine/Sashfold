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

void Node::insert_before(Node& child, Node* reference)
{
    child.remove();
    child.m_parent = this;
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
        }
        auto const it = std::find_if(old.m_nodes.begin(), old.m_nodes.end(),
            [current](std::unique_ptr<Node> const& owned) { return owned.get() == current; });
        if (it != old.m_nodes.end()) {
            m_nodes.push_back(std::move(*it));
            old.m_nodes.erase(it);
        }
        current->m_document = this;
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
