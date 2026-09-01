#include "html/TreeDump.h"

#include <algorithm>

namespace sashfold::html {

namespace {

void indent(std::string& out, int depth)
{
    out += "| ";
    for (int i = 0; i < depth; ++i)
        out += "  ";
}

std::string qualified_name(dom::Element const& element)
{
    if (element.namespace_uri() == dom::ns::svg)
        return "svg " + element.local_name();
    if (element.namespace_uri() == dom::ns::mathml)
        return "math " + element.local_name();
    return element.local_name();
}

void dump_node(std::string& out, dom::Node const& node, int depth)
{
    switch (node.type()) {
    case dom::NodeType::Document:
    case dom::NodeType::DocumentFragment:
        for (dom::Node const* child : node.children())
            dump_node(out, *child, depth);
        return;
    case dom::NodeType::DocumentType: {
        auto const& doctype = static_cast<dom::DocumentType const&>(node);
        indent(out, depth);
        out += "<!DOCTYPE " + doctype.name;
        if (!doctype.public_identifier.empty() || !doctype.system_identifier.empty())
            out += " \"" + doctype.public_identifier + "\" \"" + doctype.system_identifier + "\"";
        out += ">\n";
        return;
    }
    case dom::NodeType::Comment: {
        indent(out, depth);
        out += "<!-- " + static_cast<dom::Comment const&>(node).data + " -->\n";
        return;
    }
    case dom::NodeType::Text: {
        indent(out, depth);
        out += "\"" + static_cast<dom::Text const&>(node).data + "\"\n";
        return;
    }
    case dom::NodeType::Element: {
        auto const& element = static_cast<dom::Element const&>(node);
        indent(out, depth);
        out += "<" + qualified_name(element) + ">\n";

        std::vector<dom::Attr const*> attributes;
        attributes.reserve(element.attributes().size());
        for (dom::Attr const& attribute : element.attributes())
            attributes.push_back(&attribute);
        std::sort(attributes.begin(), attributes.end(), [](dom::Attr const* a, dom::Attr const* b) {
            return a->display_name() < b->display_name();
        });
        for (dom::Attr const* attribute : attributes) {
            indent(out, depth + 1);
            out += attribute->display_name() + "=\"" + attribute->value + "\"\n";
        }

        if (element.template_content()) {
            indent(out, depth + 1);
            out += "content\n";
            for (dom::Node const* child : element.template_content()->children())
                dump_node(out, *child, depth + 2);
        }
        for (dom::Node const* child : element.children())
            dump_node(out, *child, depth + 1);
        return;
    }
    }
}

}

std::string dump_document(dom::Document const& document)
{
    std::string out;
    dump_node(out, document, 0);
    return out;
}

std::string dump_children(dom::Node const& node)
{
    std::string out;
    for (dom::Node const* child : node.children())
        dump_node(out, *child, 0);
    return out;
}

}
