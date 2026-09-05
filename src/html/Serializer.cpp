#include "html/Serializer.h"

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

void serialize_into(std::string& out, dom::Node const& node, bool scripting);

void serialize_children_into(std::string& out, dom::Node const& node, bool scripting)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_html("template") && element.template_content()) {
            for (dom::Node const* child : element.template_content()->children())
                serialize_into(out, *child, scripting);
            return;
        }
    }
    for (dom::Node const* child : node.children())
        serialize_into(out, *child, scripting);
}

void serialize_into(std::string& out, dom::Node const& node, bool scripting)
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
        serialize_children_into(out, element, scripting);
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
    case dom::NodeType::DocumentType:
        out += "<!DOCTYPE ";
        out += static_cast<dom::DocumentType const&>(node).name;
        out += '>';
        return;
    case dom::NodeType::Document:
    case dom::NodeType::DocumentFragment:
        serialize_children_into(out, node, scripting);
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
    serialize_children_into(out, node, scripting);
    return out;
}

std::string serialize_node(dom::Node const& node, bool scripting)
{
    std::string out;
    serialize_into(out, node, scripting);
    return out;
}

std::string text_content(dom::Node const& node)
{
    std::string out;
    text_content_into(out, node);
    return out;
}

}
