#include "dom/Dom.h"

#include <algorithm>

namespace sashfold::dom {

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
    m_children.insert(it, &child);
}

void Node::remove()
{
    if (!m_parent)
        return;
    auto& siblings = m_parent->m_children;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), this), siblings.end());
    m_parent = nullptr;
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

Attr const* Element::find_attribute(std::string_view name) const
{
    for (Attr const& attribute : m_attributes) {
        if (attribute.local_name == name && attribute.prefix.empty())
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
        Text* clone = document.create<Text>();
        clone->data = static_cast<Text const&>(node).data;
        return clone;
    }
    case NodeType::Comment: {
        Comment* clone = document.create<Comment>();
        clone->data = static_cast<Comment const&>(node).data;
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

}
