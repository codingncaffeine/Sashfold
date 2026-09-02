#include "css/Stylesheets.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "css/Parser.h"
#include "dom/Dom.h"
#include "html/Encoding.h"

#include <algorithm>
#include <set>

namespace sashfold::css {

namespace {

constexpr int max_import_depth = 4;
constexpr std::size_t max_sheets = 64;

std::string lowercased(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text)
        out += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
    return out;
}

// Whitespace- and comma-separated tokens, lowercased.
std::vector<std::string> tokens_of(std::string_view text)
{
    std::vector<std::string> out;
    std::string current;
    for (char const c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ',') {
            if (!current.empty())
                out.push_back(lowercased(current));
            current.clear();
            if (c == ',')
                out.push_back(",");
        } else {
            current += c;
        }
    }
    if (!current.empty())
        out.push_back(lowercased(current));
    return out;
}

// The charset parameter of a Content-Type value, unquoted; empty when none.
std::string charset_parameter(std::string_view content_type)
{
    std::string const lower = lowercased(content_type);
    std::size_t at = lower.find("charset=");
    if (at == std::string::npos)
        return {};
    at += 8;
    std::size_t end = at;
    while (end < lower.size() && lower[end] != ';' && lower[end] != ' ' && lower[end] != '\t')
        ++end;
    std::string value = lower.substr(at, end - at);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    return value;
}

std::string attribute(dom::Element const& element, char const* name)
{
    dom::Attr const* attr = element.find_attribute(name);
    return attr ? attr->value : std::string();
}

struct Collector {
    SheetFetcher const& fetch;
    std::vector<SheetSource>& out;
    std::set<std::string> visited;

    void add_fetched(net::Url const& url, int depth)
    {
        std::string const key = url.serialize(true);
        if (visited.contains(key) || out.size() >= max_sheets || !fetch)
            return;
        visited.insert(key);
        std::optional<FetchedSheet> const fetched = fetch(url);
        if (!fetched)
            return;
        add_text(decode_stylesheet(fetched->bytes, fetched->content_type), url, depth);
    }

    void add_text(std::string text, std::optional<net::Url> const& url, int depth)
    {
        if (depth < max_import_depth) {
            for (std::string const& href : import_urls(text)) {
                if (std::optional<net::Url> const target = net::parse_url(href, url ? &*url : nullptr))
                    add_fetched(*target, depth + 1);
            }
        }
        out.push_back(SheetSource { std::move(text), url });
    }
};

void walk(dom::Node const& node, net::Url const* base, Collector& collector)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_html("style")) {
            if (!media_list_applies(attribute(element, "media")))
                return;
            std::string text;
            for (dom::Node const* child : element.children()) {
                if (child->is_text())
                    text += static_cast<dom::Text const*>(child)->data;
            }
            collector.add_text(std::move(text),
                base ? std::optional<net::Url>(*base) : std::nullopt, 0);
            return;
        }
        if (element.is_html("link")) {
            bool is_stylesheet = false;
            bool is_alternate = false;
            for (std::string const& token : tokens_of(attribute(element, "rel"))) {
                is_stylesheet = is_stylesheet || token == "stylesheet";
                is_alternate = is_alternate || token == "alternate";
            }
            if (!is_stylesheet || is_alternate || element.find_attribute("disabled"))
                return;
            std::string const type = lowercased(attribute(element, "type"));
            if (!type.empty() && type != "text/css")
                return;
            if (!media_list_applies(attribute(element, "media")))
                return;
            std::string const href = attribute(element, "href");
            if (href.empty())
                return;
            if (std::optional<net::Url> const target = net::parse_url(href, base))
                collector.add_fetched(*target, 0);
            return;
        }
    }
    for (dom::Node const* child : node.children())
        walk(*child, base, collector);
}

} // namespace

std::vector<SheetSource> collect_stylesheets(dom::Document const& document, net::Url const* base,
    SheetFetcher const& fetch)
{
    std::vector<SheetSource> sheets;
    Collector collector { fetch, sheets, {} };
    walk(document, base, collector);
    return sheets;
}

std::string decode_stylesheet(std::vector<std::uint8_t> const& bytes, std::string_view content_type)
{
    std::string_view view(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    std::optional<html::Encoding> encoding;
    if (view.starts_with("\xEF\xBB\xBF")) {
        encoding = html::Encoding::Utf8;
        view.remove_prefix(3);
    } else if (view.starts_with("\xFE\xFF")) {
        encoding = html::Encoding::Utf16Be;
        view.remove_prefix(2);
    } else if (view.starts_with("\xFF\xFE")) {
        encoding = html::Encoding::Utf16Le;
        view.remove_prefix(2);
    }
    if (!encoding) {
        if (std::string const label = charset_parameter(content_type); !label.empty())
            encoding = html::encoding_from_label(label);
    }
    if (!encoding && view.starts_with("@charset \"")) {
        std::size_t const end = view.find('"', 10);
        if (end != std::string_view::npos && end < 10 + 128 && view.substr(end, 2) == "\";")
            encoding = html::encoding_from_label(view.substr(10, end - 10));
    }
    std::u32string decoded = html::decode(view, encoding.value_or(html::Encoding::Utf8));
    if (!decoded.empty() && decoded[0] == 0xFEFF)
        decoded.erase(0, 1);
    return to_utf8(decoded);
}

std::vector<std::string> import_urls(std::string_view sheet_text)
{
    std::vector<std::string> urls;
    Stylesheet const sheet = parse_stylesheet(sheet_text);
    for (Rule const& rule : sheet.rules) {
        if (!rule.is_at_rule())
            break; // imports precede every other rule
        AtRule const& at = rule.at_rule();
        std::string const name = lowercased(at.name);
        if (name == "charset" || name == "layer")
            continue;
        if (name != "import")
            break;
        std::optional<std::string> url;
        std::string conditions;
        bool first = true;
        for (ComponentValue const& value : at.prelude) {
            if (value.is_token(Token::Type::Whitespace))
                continue;
            if (first) {
                first = false;
                if (value.is_token(Token::Type::String) || value.is_token(Token::Type::Url)) {
                    url = value.token().value;
                } else if (value.is_function() && lowercased(value.function().name) == "url") {
                    for (ComponentValue const& inner : value.function().values) {
                        if (inner.is_token(Token::Type::String) || inner.is_token(Token::Type::Ident)) {
                            url = inner.token().value;
                            break;
                        }
                    }
                }
                continue;
            }
            // What follows is the media list (layer() and supports() are
            // rare enough to read as media words here).
            if (value.is_token(Token::Type::Ident))
                conditions += value.token().value + " ";
            else if (value.is_token(Token::Type::Comma))
                conditions += ", ";
            else if (value.is_block())
                conditions += "(feature) ";
        }
        if (url && media_list_applies(conditions))
            urls.push_back(*url);
    }
    return urls;
}

bool media_list_applies(std::string_view media)
{
    std::vector<std::string> const tokens = tokens_of(media);
    if (tokens.empty())
        return true;
    // One query per comma; the list applies when any query does.
    bool any = false;
    bool query_seen = false;
    bool negated = false;
    std::optional<bool> type_applies; // the query's media type, when it names one
    auto const finish = [&] {
        if (!query_seen)
            return;
        bool applies = type_applies.value_or(true);
        if (negated)
            applies = !applies;
        any = any || applies;
        query_seen = false;
        negated = false;
        type_applies.reset();
    };
    int depth = 0; // inside a (feature): its words are not media types
    for (std::string const& token : tokens) {
        if (token == ",") {
            finish();
            continue;
        }
        query_seen = true;
        auto const opens = static_cast<int>(std::count(token.begin(), token.end(), '('));
        auto const closes = static_cast<int>(std::count(token.begin(), token.end(), ')'));
        if (depth > 0 || opens > 0) {
            depth = std::max(0, depth + opens - closes);
            continue; // a feature: taken as satisfied
        }
        if (token == "only" || token == "and")
            continue;
        if (token == "not") {
            negated = true;
            continue;
        }
        if (!type_applies)
            type_applies = token == "all" || token == "screen";
    }
    finish();
    return any;
}

}
