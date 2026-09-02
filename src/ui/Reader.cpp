#include "ui/Reader.h"

#include "core/Ascii.h"
#include "dom/Dom.h"
#include "ui/InternalPages.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sashfold::ui {

namespace {

bool is_html_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

std::string lowercase(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
    return text;
}

bool is_one_of(std::string_view name, std::initializer_list<std::string_view> names)
{
    for (std::string_view const candidate : names) {
        if (name == candidate)
            return true;
    }
    return false;
}

// Elements whose subtrees are never article content.
bool is_dropped_element(dom::Element const& element)
{
    if (!element.is_html())
        return true; // svg, mathml
    return is_one_of(element.local_name(),
        { "script", "style", "noscript", "template", "nav", "aside", "footer", "form", "iframe",
            "object", "embed", "canvas", "button", "input", "select", "textarea", "video", "audio",
            "dialog", "menu", "link", "meta", "head", "title" });
}

// Class and id names that mark the furniture around an article, and those
// that mark the article itself.
bool has_hint(dom::Element const& element, std::initializer_list<std::string_view> hints)
{
    for (char const* attribute : { "class", "id" }) {
        dom::Attr const* value = element.find_attribute(attribute);
        if (!value)
            continue;
        std::string const lowered = lowercase(value->value);
        for (std::string_view const hint : hints) {
            if (lowered.find(hint) != std::string::npos)
                return true;
        }
    }
    return false;
}

bool has_negative_hint(dom::Element const& element)
{
    return has_hint(element,
        { "comment", "sidebar", "footer", "footnote", "navbar", "navigation", "menu", "widget",
            "share", "social", "promo", "advert", "sponsor", "related", "breadcrumb", "cookie",
            "popup", "banner", "toolbar", "masthead", "skyscraper", "shoutbox", "-ad-", "outbrain" });
}

bool has_positive_hint(dom::Element const& element)
{
    return has_hint(element,
        { "article", "content", "entry", "hentry", "main", "post", "story", "text", "blog", "body" });
}

// The visible text under a node, script and style left out, whitespace
// collapsed as a line would show it.
void gather_visible_text(dom::Node const& node, std::string& out)
{
    for (dom::Node const* child : node.children()) {
        if (child->is_text()) {
            for (char const c : static_cast<dom::Text const*>(child)->data) {
                if (is_html_space(c)) {
                    if (!out.empty() && out.back() != ' ')
                        out += ' ';
                } else {
                    out += c;
                }
            }
        } else if (child->is_element()) {
            auto const& element = static_cast<dom::Element const&>(*child);
            if (is_one_of(element.local_name(), { "script", "style", "noscript", "template" }))
                continue;
            gather_visible_text(element, out);
        }
    }
}

std::string visible_text(dom::Node const& node)
{
    std::string out;
    gather_visible_text(node, out);
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// Text length inside links over all text: a nav bar or a tag cloud is
// mostly links, an article mostly not.
double link_density(dom::Element const& element)
{
    std::string const all = visible_text(element);
    if (all.empty())
        return 0;
    double linked = 0;
    std::vector<dom::Element const*> stack { &element };
    while (!stack.empty()) {
        dom::Element const* const current = stack.back();
        stack.pop_back();
        for (dom::Node const* child : current->children()) {
            if (!child->is_element())
                continue;
            auto const& child_element = static_cast<dom::Element const&>(*child);
            if (child_element.is_html("a"))
                linked += static_cast<double>(visible_text(child_element).size());
            else
                stack.push_back(&child_element);
        }
    }
    return linked / static_cast<double>(all.size());
}

dom::Element const* parent_element(dom::Node const& node)
{
    dom::Node const* const parent = node.parent();
    return parent && parent->is_element() ? static_cast<dom::Element const*>(parent) : nullptr;
}

dom::Element const* find_element(dom::Node const& node, std::string_view name)
{
    for (dom::Node const* child : node.children()) {
        if (!child->is_element())
            continue;
        auto const& element = static_cast<dom::Element const&>(*child);
        if (element.is_html(name))
            return &element;
        if (dom::Element const* const found = find_element(element, name))
            return found;
    }
    return nullptr;
}

bool inside_dropped(dom::Element const& element)
{
    for (dom::Element const* current = &element; current; current = parent_element(*current)) {
        if (is_dropped_element(*current) || has_negative_hint(*current))
            return true;
    }
    return false;
}

// Every paragraph scores its parent and, at half, its grandparent: a
// point for being one, a point per comma, one per hundred characters up to
// three. The container with the most, less its link density and with its
// hints, is the article.
struct Scorer {
    std::unordered_map<dom::Element const*, double> scores;

    void visit(dom::Node const& node)
    {
        for (dom::Node const* child : node.children()) {
            if (!child->is_element())
                continue;
            auto const& element = static_cast<dom::Element const&>(*child);
            if (is_dropped_element(element) || has_negative_hint(element))
                continue;
            if (is_one_of(element.local_name(), { "p", "pre", "blockquote", "td" })) {
                std::string const text = visible_text(element);
                if (text.size() >= 25) {
                    double points = 1 + static_cast<double>(std::count(text.begin(), text.end(), ','));
                    points += std::min(3.0, static_cast<double>(text.size()) / 100.0);
                    if (dom::Element const* const parent = parent_element(element)) {
                        scores[parent] += points;
                        if (dom::Element const* const grandparent = parent_element(*parent))
                            scores[grandparent] += points / 2;
                    }
                }
            }
            visit(element);
        }
    }

    dom::Element const* best() const
    {
        dom::Element const* winner = nullptr;
        double winning = 0;
        for (auto const& [element, raw] : scores) {
            double score = raw * (1 - link_density(*element));
            if (has_positive_hint(*element))
                score += 25;
            if (element->is_html("body"))
                score -= 5; // a real container beats the whole page at a tie
            if (!winner || score > winning) {
                winner = element;
                winning = score;
            }
        }
        return winner;
    }
};

// The content serialized through a whitelist: known elements keep their
// tags and nothing else, links and pictures keep their addresses made
// absolute, wrappers give way to their children, furniture is left out.
struct Serializer {
    net::Url const& base;
    std::string const& title;
    std::string out;

    void attribute(dom::Element const& element, char const* name, bool resolve)
    {
        dom::Attr const* value = element.find_attribute(name);
        if (!value || value->value.empty())
            return;
        std::string text = value->value;
        if (resolve) {
            std::optional<net::Url> const url = net::parse_url(text, &base);
            if (!url)
                return;
            text = url->serialize();
        }
        out += ' ';
        out += name;
        out += "=\"" + html_escape(text) + "\"";
    }

    void children_of(dom::Node const& node)
    {
        for (dom::Node const* child : node.children()) {
            if (child->is_text())
                out += html_escape(static_cast<dom::Text const*>(child)->data);
            else if (child->is_element())
                element(static_cast<dom::Element const&>(*child));
        }
    }

    void element(dom::Element const& element)
    {
        if (is_dropped_element(element) || has_negative_hint(element))
            return;
        std::string const& name = element.local_name();
        if (name == "h1" && visible_text(element) == title)
            return; // the article's own heading is the page's title already
        if (name == "a") {
            dom::Attr const* href = element.find_attribute("href");
            if (!href || !net::parse_url(href->value, &base)) {
                children_of(element);
                return;
            }
            out += "<a";
            attribute(element, "href", true);
            out += '>';
            children_of(element);
            out += "</a>";
            return;
        }
        if (name == "img") {
            if (!element.find_attribute("src"))
                return;
            out += "<img";
            attribute(element, "src", true);
            attribute(element, "alt", false);
            attribute(element, "width", false);
            attribute(element, "height", false);
            out += '>';
            return;
        }
        if (name == "br" || name == "hr") {
            out += "<" + name + ">";
            return;
        }
        static constexpr std::string_view kept[] = { "p", "h1", "h2", "h3", "h4", "h5", "h6", "ul",
            "ol", "li", "blockquote", "pre", "code", "em", "strong", "b", "i", "u", "s", "small", "sup",
            "sub", "table", "thead", "tbody", "tfoot", "tr", "td", "th", "caption", "figure",
            "figcaption", "dl", "dt", "dd", "cite", "q", "abbr", "mark", "time", "kbd", "samp", "var",
            "section", "article", "main", "header", "hgroup", "address", "del", "ins" };
        bool const keep = std::find(std::begin(kept), std::end(kept), name) != std::end(kept);
        if (keep)
            out += "<" + name + ">";
        children_of(element);
        if (keep)
            out += "</" + name + ">";
    }
};

std::string page_title(dom::Document const& document, dom::Element const* article)
{
    if (article) {
        if (dom::Element const* const heading = find_element(*article, "h1")) {
            std::string const text = visible_text(*heading);
            if (!text.empty())
                return text;
        }
    }
    if (dom::Element const* const heading = find_element(document, "h1")) {
        std::string const text = visible_text(*heading);
        if (!text.empty())
            return text;
    }
    if (dom::Element const* const title = find_element(document, "title"))
        return visible_text(*title);
    return "Reader";
}

} // namespace

Article extract_article(dom::Document const& document, net::Url const& base)
{
    Scorer scorer;
    scorer.visit(document);
    dom::Element const* container = scorer.best();
    if (container && inside_dropped(*container))
        container = nullptr;
    if (!container)
        container = find_element(document, "body");
    Article article;
    article.title = page_title(document, container);
    if (!container)
        return article;
    Serializer serializer { base, article.title, {} };
    serializer.children_of(*container);
    article.html = std::move(serializer.out);
    return article;
}

std::string reader_page(dom::Document const& document, net::Url const& url)
{
    Article const article = extract_article(document, url);
    // The source line names the host; a data: page has none and would
    // otherwise print itself.
    std::string const host = url.has_host() && !url.host.empty() ? url.serialize_host()
        : url.scheme == "data"                                     ? std::string("a data: URL")
                                                                   : url.serialize();
    std::string page = "<!doctype html><html><head><meta charset=\"utf-8\"><title>"
        + html_escape(article.title) + "</title><style>\n";
    page += "body { margin: 0; background: #faf7f1; color: #1c1b19; font-size: 18px; line-height: 1.6 }\n"
            "article { width: 640px; margin: 40px auto 80px }\n"
            "h1 { font-size: 32px; line-height: 1.25; margin: 0 0 8px }\n"
            "h2 { font-size: 24px; margin: 32px 0 8px }\n"
            "h3, h4, h5, h6 { font-size: 20px; margin: 24px 0 8px }\n"
            ".source { color: #6f6a60; font-size: 14px; margin: 0 0 28px }\n"
            ".source a { color: #6f6a60 }\n"
            "p, ul, ol, blockquote, pre, table, figure, dl { margin: 0 0 20px }\n"
            "blockquote { margin-left: 0; padding-left: 16px; border-left: 3px solid #d9d2c5; color: #4a463f }\n"
            "pre, code, kbd, samp { font-family: monospace; font-size: 15px }\n"
            "pre { background: #f0ece4; padding: 12px; white-space: pre-wrap }\n"
            "a { color: #1a5fb4 }\n"
            "img { display: block; margin: 8px 0 }\n"
            "figcaption { font-size: 14px; color: #6f6a60 }\n"
            "hr { border-top: 1px solid #d9d2c5; margin: 24px 0 }\n"
            "</style></head><body><article><h1>"
        + html_escape(article.title) + "</h1><p class=\"source\"><a href=\"" + html_escape(url.serialize())
        + "\">" + html_escape(host) + "</a></p>" + article.html + "</article></body></html>";
    return page;
}

}
