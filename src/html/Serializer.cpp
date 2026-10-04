#include "html/Serializer.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace sashfold::html {

namespace {

// §13.3 "escaping a string": & always; the no-break space as &nbsp;; in
// attribute mode the double quote, otherwise < and >.
void escape_into(std::string& out, std::string_view text, bool attribute_mode)
{
    for (std::size_t i = 0; i < text.size(); ++i) {
        unsigned char const c = static_cast<unsigned char>(text[i]);
        if (c == '&') {
            out += "&amp;";
        } else if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA0) {
            out += "&nbsp;";
            ++i;
        } else if (attribute_mode && c == '"') {
            out += "&quot;";
        } else if (!attribute_mode && c == '<') {
            out += "&lt;";
        } else if (!attribute_mode && c == '>') {
            out += "&gt;";
        } else {
            out += static_cast<char>(c);
        }
    }
}

bool is_void_element(dom::Element const& element)
{
    if (!element.is_html())
        return false;
    static constexpr std::array<std::string_view, 16> names {
        "area", "base", "basefont", "bgsound", "br", "col", "embed", "hr", "img", "input", "keygen", "link",
        "meta", "param", "source", "track"
    };
    std::string_view const name = element.local_name();
    if (name == "wbr")
        return true;
    for (std::string_view const candidate : names) {
        if (candidate == name)
            return true;
    }
    return false;
}

// Whether the text children of `parent` are written as they are.
bool takes_raw_text(dom::Node const* parent, bool scripting)
{
    if (!parent || !parent->is_element())
        return false;
    auto const& element = static_cast<dom::Element const&>(*parent);
    if (!element.is_html())
        return false;
    std::string_view const name = element.local_name();
    if (name == "style" || name == "script" || name == "xmp" || name == "iframe" || name == "noembed"
        || name == "noframes" || name == "plaintext")
        return true;
    return scripting && name == "noscript";
}

// The attribute's serialized name (§13.3 step 2, "attribute mode"
// names): an attribute in a namespace is written with its prefix.
std::string attribute_name(dom::Attr const& attribute)
{
    if (attribute.namespace_uri.empty())
        return attribute.local_name;
    if (attribute.namespace_uri == dom::ns::xml)
        return "xml:" + attribute.local_name;
    if (attribute.namespace_uri == dom::ns::xmlns)
        return attribute.local_name == "xmlns" ? std::string("xmlns") : "xmlns:" + attribute.local_name;
    if (attribute.namespace_uri == dom::ns::xlink)
        return "xlink:" + attribute.local_name;
    return attribute.prefix.empty() ? attribute.local_name : attribute.prefix + ":" + attribute.local_name;
}

void serialize_into(std::string& out, dom::Node const& node, bool scripting, ShadowRootSerialization const* shadow);

void serialize_children_into(std::string& out, dom::Node const& node, bool scripting, ShadowRootSerialization const* shadow)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        // getHTML() writes a host's shadow root first, as the template the
        // parser would make it from again (§13.3, the shadow host step): a
        // serializable root when those were asked for, and any root named.
        if (dom::ShadowRoot const* const root = shadow != nullptr ? element.shadow_root() : nullptr;
            root != nullptr
            && ((shadow->serializable && root->serializable)
                || std::find(shadow->roots.begin(), shadow->roots.end(), root) != shadow->roots.end())) {
            out += "<template shadowrootmode=\"";
            out += root->mode == dom::ShadowRoot::Mode::Open ? "open" : "closed";
            out += '"';
            if (root->delegates_focus)
                out += " shadowrootdelegatesfocus=\"\"";
            if (root->serializable)
                out += " shadowrootserializable=\"\"";
            if (root->clonable)
                out += " shadowrootclonable=\"\"";
            out += '>';
            serialize_children_into(out, *root, scripting, shadow);
            out += "</template>";
        }
        if (element.is_html("template") && element.template_content()) {
            for (dom::Node const* child : element.template_content()->children())
                serialize_into(out, *child, scripting, shadow);
            return;
        }
    }
    for (dom::Node const* child : node.children())
        serialize_into(out, *child, scripting, shadow);
}

void serialize_into(std::string& out, dom::Node const& node, bool scripting, ShadowRootSerialization const* shadow)
{
    switch (node.type()) {
    case dom::NodeType::Element: {
        auto const& element = static_cast<dom::Element const&>(node);
        // An element in the HTML, MathML or SVG namespace is written by its
        // local name alone; anything else by its qualified name.
        out += '<';
        out += element.local_name();
        for (dom::Attr const& attribute : element.attributes()) {
            out += ' ';
            out += attribute_name(attribute);
            out += "=\"";
            escape_into(out, attribute.value, true);
            out += '"';
        }
        out += '>';
        if (is_void_element(element))
            return;
        // The parser drops one newline after these start tags, so one is
        // written back to make the round trip exact (§13.3 step 3).
        if (element.is_html("pre") || element.is_html("textarea") || element.is_html("listing")) {
            if (dom::Node const* first = element.children().empty() ? nullptr : element.children().front()) {
                if (first->is_text() && static_cast<dom::Text const*>(first)->data.starts_with('\n'))
                    out += '\n';
            }
        }
        serialize_children_into(out, element, scripting, shadow);
        out += "</";
        out += element.local_name();
        out += '>';
        return;
    }
    case dom::NodeType::Text: {
        auto const& text = static_cast<dom::Text const&>(node);
        if (takes_raw_text(node.parent(), scripting))
            out += text.data;
        else
            escape_into(out, text.data, false);
        return;
    }
    case dom::NodeType::Comment:
        out += "<!--";
        out += static_cast<dom::Comment const&>(node).data;
        out += "-->";
        return;
    case dom::NodeType::ProcessingInstruction: {
        auto const& instruction = static_cast<dom::ProcessingInstruction const&>(node);
        out += "<?";
        out += instruction.target;
        out += ' ';
        out += instruction.data;
        out += '>';
        return;
    }
    case dom::NodeType::DocumentType:
        out += "<!DOCTYPE ";
        out += static_cast<dom::DocumentType const&>(node).name;
        out += '>';
        return;
    case dom::NodeType::Document:
    case dom::NodeType::DocumentFragment:
        serialize_children_into(out, node, scripting, shadow);
        return;
    }
}

void text_content_into(std::string& out, dom::Node const& node)
{
    if (node.is_text()) {
        out += static_cast<dom::Text const&>(node).data;
        return;
    }
    for (dom::Node const* child : node.children())
        text_content_into(out, *child);
}

} // namespace

std::string serialize_children(dom::Node const& node, bool scripting)
{
    std::string out;
    serialize_children_into(out, node, scripting, nullptr);
    return out;
}

std::string serialize_children(dom::Node const& node, ShadowRootSerialization const& shadow, bool scripting)
{
    std::string out;
    serialize_children_into(out, node, scripting, &shadow);
    return out;
}

std::string serialize_node(dom::Node const& node, bool scripting)
{
    std::string out;
    serialize_into(out, node, scripting, nullptr);
    return out;
}

std::string text_content(dom::Node const& node)
{
    std::string out;
    text_content_into(out, node);
    return out;
}

// --- XML serialization (DOM Parsing and Serialization §3.2) -------------------

namespace {

// §3.2.1.1.2 "serializing an attribute value": & < > and the quote, and the
// white space an XML parser would fold away.
void escape_xml_into(std::string& out, std::string_view text, bool attribute_mode)
{
    for (char const c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += attribute_mode ? "&quot;" : "\"";
            break;
        case '\t':
            out += attribute_mode ? "&#9;" : "\t";
            break;
        case '\n':
            out += attribute_mode ? "&#10;" : "\n";
            break;
        case '\r':
            out += attribute_mode ? "&#13;" : "\r";
            break;
        default:
            out += c;
        }
    }
}

// The prefixes in scope, each with the namespace it stands for; a later
// entry hides an earlier one of the same prefix.
using PrefixScope = std::vector<std::pair<std::string, std::string>>;

std::string const* namespace_of_prefix(PrefixScope const& scope, std::string_view prefix)
{
    for (std::size_t i = scope.size(); i-- > 0;) {
        if (scope[i].first == prefix)
            return &scope[i].second;
    }
    return nullptr;
}

void serialize_xml_into(std::string& out, dom::Node const& node, std::string_view inherited_namespace, PrefixScope scope, int& generated);

void serialize_xml_children_into(std::string& out, dom::Node const& node, std::string_view inherited_namespace, PrefixScope const& scope,
    int& generated)
{
    for (dom::Node const* child : node.children())
        serialize_xml_into(out, *child, inherited_namespace, scope, generated);
}

void serialize_xml_into(std::string& out, dom::Node const& node, std::string_view inherited_namespace, PrefixScope scope, int& generated)
{
    switch (node.type()) {
    case dom::NodeType::Element: {
        auto const& element = static_cast<dom::Element const&>(node);
        // An element here has no prefix of its own: it is written by its
        // local name, and declares its namespace as the default one where
        // that is not the default already.
        std::string const& own_namespace = element.namespace_uri();
        out += '<';
        out += element.local_name();
        bool const declares_default = own_namespace != inherited_namespace;
        if (declares_default) {
            out += " xmlns=\"";
            escape_xml_into(out, own_namespace, true);
            out += '"';
        }
        // The prefix declarations first: they are in scope for the
        // attributes beside them.
        for (dom::Attr const& attribute : element.attributes()) {
            if (attribute.namespace_uri == dom::ns::xmlns && attribute.prefix == "xmlns")
                scope.push_back({ attribute.local_name, attribute.value });
        }
        for (dom::Attr const& attribute : element.attributes()) {
            std::string prefix;
            if (attribute.namespace_uri == dom::ns::xmlns) {
                // The default namespace is the element's own, said above or
                // inherited; a prefix declaration is written as it stands.
                if (attribute.prefix.empty() || attribute.value == dom::ns::xml)
                    continue;
                prefix = "xmlns";
            } else if (attribute.namespace_uri == dom::ns::xml) {
                prefix = "xml";
            } else if (!attribute.namespace_uri.empty()) {
                // The attribute's own prefix when it names this namespace
                // in scope, or can be declared to; one made up otherwise.
                std::string const* const bound = attribute.prefix.empty() ? nullptr : namespace_of_prefix(scope, attribute.prefix);
                if (bound != nullptr && *bound == attribute.namespace_uri) {
                    prefix = attribute.prefix;
                } else {
                    prefix = bound == nullptr && !attribute.prefix.empty() ? attribute.prefix : "ns" + std::to_string(++generated);
                    scope.push_back({ prefix, attribute.namespace_uri });
                    out += " xmlns:" + prefix + "=\"";
                    escape_xml_into(out, attribute.namespace_uri, true);
                    out += '"';
                }
            }
            out += ' ';
            if (!prefix.empty())
                out += prefix + ":";
            out += attribute.local_name;
            out += "=\"";
            escape_xml_into(out, attribute.value, true);
            out += '"';
        }
        dom::Node const* const content = element.is_html("template") ? element.template_content() : nullptr;
        bool const childless = (content != nullptr ? content : &node)->children().empty();
        // An HTML element that never has content is closed in its tag; any
        // other HTML element keeps its end tag, so that an HTML parser
        // reading the text back sees the same element.
        if (childless && element.is_html()) {
            if (is_void_element(element)) {
                out += " />";
                return;
            }
        } else if (childless) {
            out += "/>";
            return;
        }
        out += '>';
        serialize_xml_children_into(out, content != nullptr ? *content : node, own_namespace, scope, generated);
        out += "</";
        out += element.local_name();
        out += '>';
        return;
    }
    case dom::NodeType::Text:
        escape_xml_into(out, static_cast<dom::Text const&>(node).data, false);
        return;
    case dom::NodeType::Comment:
        out += "<!--";
        out += static_cast<dom::Comment const&>(node).data;
        out += "-->";
        return;
    case dom::NodeType::ProcessingInstruction: {
        auto const& instruction = static_cast<dom::ProcessingInstruction const&>(node);
        out += "<?";
        out += instruction.target;
        out += ' ';
        out += instruction.data;
        out += "?>";
        return;
    }
    case dom::NodeType::DocumentType: {
        auto const& doctype = static_cast<dom::DocumentType const&>(node);
        out += "<!DOCTYPE ";
        out += doctype.name;
        if (!doctype.public_identifier.empty())
            out += " PUBLIC \"" + doctype.public_identifier + "\"";
        if (!doctype.system_identifier.empty())
            out += (doctype.public_identifier.empty() ? " SYSTEM \"" : " \"") + doctype.system_identifier + "\"";
        out += '>';
        return;
    }
    case dom::NodeType::Document:
    case dom::NodeType::DocumentFragment:
        serialize_xml_children_into(out, node, inherited_namespace, scope, generated);
        return;
    }
}

}

std::string serialize_xml(dom::Node const& node)
{
    std::string out;
    int generated = 0;
    serialize_xml_into(out, node, {}, PrefixScope { { "xml", std::string(dom::ns::xml) } }, generated);
    return out;
}

}
