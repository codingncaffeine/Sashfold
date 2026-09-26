#include "html/PreloadScanner.h"

#include <cstddef>
#include <utility>

namespace sashfold::html {

namespace {

char lowered(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool equals_ci(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowered(a[i]) != lowered(b[i]))
            return false;
    }
    return true;
}

// Whether a space-separated list of tokens holds one, letters' case aside.
bool has_token(std::string_view list, std::string_view token)
{
    std::size_t at = 0;
    while (at < list.size()) {
        while (at < list.size() && is_space(list[at]))
            ++at;
        std::size_t const from = at;
        while (at < list.size() && !is_space(list[at]))
            ++at;
        if (equals_ci(list.substr(from, at - from), token))
            return true;
    }
    return false;
}

std::string trimmed(std::string_view text)
{
    while (!text.empty() && is_space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && is_space(text.back()))
        text.remove_suffix(1);
    return std::string(text);
}

// The few character references a URL in an attribute is written with.
std::string unescaped(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '&') {
            std::string_view const rest = text.substr(i);
            if (rest.starts_with("&amp;")) {
                out += '&';
                i += 4;
                continue;
            }
            if (rest.starts_with("&#38;")) {
                out += '&';
                i += 4;
                continue;
            }
            if (rest.starts_with("&#x26;")) {
                out += '&';
                i += 5;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

struct Attribute {
    std::string name; // lowercase
    std::string value;
};

// A script's type that means "run this as script": none, a JavaScript MIME
// type, or module.
bool runs_as_script(std::string_view type)
{
    std::string const t = trimmed(type);
    if (t.empty() || equals_ci(t, "module"))
        return true;
    for (char const* known : { "text/javascript", "application/javascript", "text/ecmascript", "application/ecmascript",
             "application/x-javascript", "text/jscript", "text/livescript", "text/x-javascript", "text/x-ecmascript",
             "application/x-ecmascript", "text/javascript1.0", "text/javascript1.1", "text/javascript1.2",
             "text/javascript1.3", "text/javascript1.4", "text/javascript1.5" }) {
        if (equals_ci(t, known))
            return true;
    }
    return false;
}

}

PreloadScan scan_for_preloads(std::string_view html)
{
    PreloadScan scan;
    std::size_t at = 0;
    // The elements whose content shows nothing of the page to the parser:
    // their text is passed over up to their end tag.
    auto const skip_past_end_tag = [&](std::string_view name) {
        while (at < html.size()) {
            std::size_t const open = html.find("</", at);
            if (open == std::string_view::npos) {
                at = html.size();
                return;
            }
            std::string_view const after = html.substr(open + 2);
            if (after.size() >= name.size() && equals_ci(after.substr(0, name.size()), name)
                && (after.size() == name.size() || is_space(after[name.size()]) || after[name.size()] == '>'
                    || after[name.size()] == '/')) {
                std::size_t const close = html.find('>', open);
                at = close == std::string_view::npos ? html.size() : close + 1;
                return;
            }
            at = open + 2;
        }
    };
    while (at < html.size()) {
        std::size_t const open = html.find('<', at);
        if (open == std::string_view::npos || open + 1 >= html.size())
            break;
        at = open + 1;
        if (html.substr(at).starts_with("!--")) {
            std::size_t const end = html.find("-->", at + 3);
            at = end == std::string_view::npos ? html.size() : end + 3;
            continue;
        }
        char const first = html[at];
        if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z')))
            continue; // an end tag, a doctype, a stray <
        std::string tag;
        while (at < html.size() && !is_space(html[at]) && html[at] != '>' && html[at] != '/')
            tag += lowered(html[at++]);
        std::vector<Attribute> attributes;
        while (at < html.size() && html[at] != '>') {
            while (at < html.size() && (is_space(html[at]) || html[at] == '/'))
                ++at;
            if (at >= html.size() || html[at] == '>')
                break;
            Attribute attribute;
            while (at < html.size() && !is_space(html[at]) && html[at] != '=' && html[at] != '>' && html[at] != '/')
                attribute.name += lowered(html[at++]);
            while (at < html.size() && is_space(html[at]))
                ++at;
            if (at < html.size() && html[at] == '=') {
                ++at;
                while (at < html.size() && is_space(html[at]))
                    ++at;
                if (at < html.size() && (html[at] == '"' || html[at] == '\'')) {
                    char const quote = html[at++];
                    std::size_t const end = html.find(quote, at);
                    std::size_t const stop = end == std::string_view::npos ? html.size() : end;
                    attribute.value = std::string(html.substr(at, stop - at));
                    at = stop == html.size() ? stop : stop + 1;
                } else {
                    while (at < html.size() && !is_space(html[at]) && html[at] != '>')
                        attribute.value += html[at++];
                }
            }
            if (!attribute.name.empty())
                attributes.push_back(std::move(attribute));
        }
        if (at < html.size())
            ++at; // the >
        auto const value_of = [&](std::string_view name) -> std::string const* {
            for (Attribute const& attribute : attributes) {
                if (attribute.name == name)
                    return &attribute.value;
            }
            return nullptr;
        };
        if (tag == "base") {
            if (std::string const* const href = value_of("href"); href && scan.base_href.empty())
                scan.base_href = trimmed(unescaped(*href));
        } else if (tag == "meta") {
            if (std::string const* const equiv = value_of("http-equiv"); equiv && equals_ci(trimmed(*equiv), "content-security-policy")) {
                scan.meta_policy = true;
                if (std::string const* const content = value_of("content"))
                    scan.meta_policies.push_back(unescaped(*content));
            }
        } else if (tag == "link") {
            std::string const* const rel = value_of("rel");
            std::string const* const href = value_of("href");
            if (rel && href && !trimmed(*href).empty() && !value_of("disabled")) {
                std::string const* const media = value_of("media");
                bool const for_print = media && equals_ci(trimmed(*media), "print");
                if (has_token(*rel, "stylesheet") && !has_token(*rel, "alternate") && !for_print)
                    scan.resources.push_back({ Preload::Kind::Stylesheet, trimmed(unescaped(*href)) });
            }
        } else if (tag == "script") {
            std::string const* const src = value_of("src");
            std::string const* const type = value_of("type");
            if (src && !trimmed(*src).empty() && !value_of("nomodule") && runs_as_script(type ? *type : std::string_view()))
                scan.resources.push_back({ Preload::Kind::Script, trimmed(unescaped(*src)) });
            skip_past_end_tag("script");
        } else if (tag == "style" || tag == "noscript" || tag == "template" || tag == "textarea" || tag == "title"
            || tag == "xmp" || tag == "iframe" || tag == "noembed" || tag == "noframes") {
            skip_past_end_tag(tag);
        }
    }
    return scan;
}

}
