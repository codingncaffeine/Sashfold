#pragma once

// The document tree the HTML parser builds. Ownership model for this stage:
// the Document owns every node (a node soup of unique_ptrs); tree structure
// is raw parent/children links, so reparenting — which tree construction does
// constantly (foster parenting, the adoption agency) — is pointer surgery,
// never an ownership move. The JS-facing lifetime model is a separate, later
// decision (see the plan's DOM-lifetime ADR note).

#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
};

enum class QuirksMode {
    No,
    Limited,
    Yes,
};

class Document;

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

    bool is_element() const { return m_type == NodeType::Element; }
    bool is_text() const { return m_type == NodeType::Text; }

private:
    Document* m_document;
    NodeType m_type;
    Node* m_parent = nullptr;
    std::vector<Node*> m_children;
};

struct Attr {
    std::string local_name;
    std::string value;
    std::string prefix; // "" for ordinary attributes
    std::string namespace_uri; // "" for none

    // The name as the html5lib tree format prints and sorts it.
    std::string display_name() const
    {
        return prefix.empty() ? local_name : prefix + " " + local_name;
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

    std::vector<Attr>& attributes() { return m_attributes; }
    std::vector<Attr> const& attributes() const { return m_attributes; }
    Attr const* find_attribute(std::string_view name) const;
    bool has_attribute(std::string_view name) const { return find_attribute(name) != nullptr; }

    // <template> only: its parsed contents live in a separate fragment.
    Node* template_content() const { return m_template_content; }
    void set_template_content(Node* content) { m_template_content = content; }

private:
    std::string m_namespace_uri;
    std::string m_local_name;
    std::vector<Attr> m_attributes;
    Node* m_template_content = nullptr;
};

class Text : public Node {
public:
    explicit Text(Document& document)
        : Node(document, NodeType::Text)
    {
    }
    std::string data; // UTF-8 (WTF-8 internally)
};

class Comment : public Node {
public:
    explicit Comment(Document& document)
        : Node(document, NodeType::Comment)
    {
    }
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

class Document : public Node {
public:
    Document()
        : Node(*this, NodeType::Document)
    {
    }

    QuirksMode quirks_mode = QuirksMode::No;

    template<typename T, typename... Args>
    T* create(Args&&... args)
    {
        auto node = std::make_unique<T>(*this, std::forward<Args>(args)...);
        T* raw = node.get();
        m_nodes.push_back(std::move(node));
        return raw;
    }

private:
    std::vector<std::unique_ptr<Node>> m_nodes;
};

// Deep-copies a subtree; the clone's nodes are owned by `document`.
Node* clone_subtree(Node const& node, Document& document);

}
