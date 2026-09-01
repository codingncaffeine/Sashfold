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

}
