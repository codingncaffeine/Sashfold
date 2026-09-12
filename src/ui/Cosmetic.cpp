#include "ui/Cosmetic.h"

#include "dom/Dom.h"

#include <unordered_set>

namespace sashfold::ui {

namespace {

bool is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
        || static_cast<unsigned char>(c) >= 0x80;
}

// Every id and class name the page's elements carry.
struct PageNames {
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> classes;

    void collect(dom::Node const& node)
    {
        if (node.is_element()) {
            auto const& element = static_cast<dom::Element const&>(node);
            for (dom::Attr const& attribute : element.attributes()) {
                if (!attribute.prefix.empty())
                    continue;
                if (attribute.local_name == "id") {
                    ids.insert(attribute.value);
                } else if (attribute.local_name == "class") {
                    std::string_view text = attribute.value;
                    while (!text.empty()) {
                        std::size_t const start = text.find_first_not_of(" \t\n\r\f");
                        if (start == std::string_view::npos)
                            break;
                        std::size_t const end = text.find_first_of(" \t\n\r\f", start);
                        classes.insert(std::string(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start)));
                        if (end == std::string_view::npos)
                            break;
                        text = text.substr(end);
                    }
                }
            }
        }
        for (dom::Node const* child : node.children())
            collect(*child);
    }
};

// Whether the selector can match something on this page: it names no id
// or class at all, or one of those it names is present. The names are
// read off the selector text — `#x` and `.y` outside brackets and
// strings — which is a filter, not a parse: a selector this lets through
// still has to match for real.
bool can_apply(std::string_view selector, PageNames const& names)
{
    bool named = false;
    int bracket = 0;
    char quote = 0;
    for (std::size_t i = 0; i < selector.size(); ++i) {
        char const c = selector[i];
        if (quote) {
            if (c == '\\')
                ++i;
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '[' || c == '(') {
            ++bracket;
            continue;
        }
        if (c == ']' || c == ')') {
            bracket = bracket > 0 ? bracket - 1 : 0;
            continue;
        }
        if (bracket > 0 || (c != '#' && c != '.'))
            continue;
        std::size_t end = i + 1;
        while (end < selector.size() && is_name_char(selector[end]))
            ++end;
        if (end == i + 1)
            continue;
        std::string const name(selector.substr(i + 1, end - i - 1));
        named = true;
        if (c == '#' ? names.ids.contains(name) : names.classes.contains(name))
            return true;
        i = end - 1;
    }
    return !named;
}

} // namespace

std::vector<std::string> cosmetic_selectors(net::Blocklists const& lists, net::Url const& page,
    dom::Document const& document)
{
    std::vector<std::string> out;
    if (lists.cosmetic_count() == 0)
        return out;
    std::vector<std::string> const named = lists.hidden_selectors(page.host);
    if (named.empty())
        return out;
    PageNames names;
    names.collect(document);
    for (std::string const& selector : named) {
        if (can_apply(selector, names))
            out.push_back(selector);
    }
    return out;
}

std::optional<css::SheetSource> cosmetic_sheet(net::Blocklists const& lists, net::Url const& page,
    dom::Document const& document)
{
    std::vector<std::string> const selectors = cosmetic_selectors(lists, page, document);
    if (selectors.empty())
        return std::nullopt;
    std::string text;
    // One rule per selector: a selector the parser rejects then drops only
    // its own rule, as the specification has it, not the whole sheet.
    for (std::string const& selector : selectors) {
        text += selector;
        text += " { display: none !important; }\n";
    }
    return css::SheetSource { std::move(text), std::nullopt };
}

}
