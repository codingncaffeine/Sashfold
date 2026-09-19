#include "html/DocumentBase.h"

#include "dom/Dom.h"

#include <optional>
#include <vector>

namespace sashfold::html {

net::Url document_base_url(dom::Document const& document, net::Url const& fallback)
{
    // Most documents never held a base element: nothing to look for.
    if (!document.may_have_base())
        return fallback;
    std::vector<dom::Node const*> pending { &document };
    while (!pending.empty()) {
        dom::Node const* const node = pending.back();
        pending.pop_back();
        if (node->is_element()) {
            auto const& element = static_cast<dom::Element const&>(*node);
            if (element.is_html("base")) {
                if (dom::Attr const* const href = element.find_attribute("href")) {
                    std::optional<net::Url> const url = net::parse_url(href->value, &fallback);
                    if (!url || url->scheme == "data" || url->scheme == "javascript")
                        return fallback;
                    return *url;
                }
            }
        }
        // Tree order: the first child is looked at next.
        std::vector<dom::Node*> const& children = node->children();
        for (auto child = children.rbegin(); child != children.rend(); ++child)
            pending.push_back(*child);
    }
    return fallback;
}

}
