#include "ui/Bookmarks.h"

#include "core/Base64.h"
#include "core/Json.h"

#include <algorithm>
#include <cstdio>
#include <span>
#include <utility>

namespace sashfold::ui {

namespace {

constexpr std::size_t max_depth = 64;
constexpr std::size_t max_nodes = 200000;

BookmarkNode const* find_in(BookmarkNode const& node, std::uint64_t id)
{
    if (node.id == id)
        return &node;
    for (BookmarkNode const& child : node.children) {
        if (BookmarkNode const* const found = find_in(child, id))
            return found;
    }
    return nullptr;
}

BookmarkNode const* find_url_in(BookmarkNode const& node, std::string_view url)
{
    if (!node.folder && node.url == url)
        return &node;
    for (BookmarkNode const& child : node.children) {
        if (BookmarkNode const* const found = find_url_in(child, url))
            return found;
    }
    return nullptr;
}

BookmarkNode const* parent_in(BookmarkNode const& node, std::uint64_t id)
{
    for (BookmarkNode const& child : node.children) {
        if (child.id == id)
            return &node;
        if (BookmarkNode const* const found = parent_in(child, id))
            return found;
    }
    return nullptr;
}

std::size_t count_in(BookmarkNode const& node)
{
    std::size_t count = node.folder ? 0 : 1;
    for (BookmarkNode const& child : node.children)
        count += count_in(child);
    return count;
}

std::string json_quoted(std::string_view text)
{
    std::string out = "\"";
    for (char const ch : text) {
        auto const c = static_cast<unsigned char>(ch);
        if (c == '"')
            out += "\\\"";
        else if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (c < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
            out += buffer;
        } else {
            out += ch;
        }
    }
    out += '"';
    return out;
}

std::string base64_of(std::string const& bytes)
{
    return base64_encode(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(bytes.data()), bytes.size()));
}

void write_nodes(std::string& out, std::vector<BookmarkNode> const& nodes, int depth)
{
    std::string const indent(static_cast<std::size_t>(depth) * 2, ' ');
    out += "[";
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        BookmarkNode const& node = nodes[i];
        out += i == 0 ? "\n" : ",\n";
        out += indent + "  { \"title\": " + json_quoted(node.title);
        if (node.added != 0)
            out += ", \"added\": " + std::to_string(node.added);
        if (node.folder) {
            out += ", \"children\": ";
            write_nodes(out, node.children, depth + 2);
        } else {
            out += ", \"url\": " + json_quoted(node.url);
            if (!node.icon.empty())
                out += ", \"icon\": " + json_quoted(base64_of(node.icon));
        }
        out += " }";
    }
    out += nodes.empty() ? "]" : "\n" + indent + "]";
}

std::string html_escaped(std::string_view text)
{
    std::string out;
    for (char const c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

void write_netscape(std::string& out, std::vector<BookmarkNode> const& nodes, int depth)
{
    std::string const indent(static_cast<std::size_t>(depth) * 4, ' ');
    for (BookmarkNode const& node : nodes) {
        if (node.folder) {
            out += indent + "<DT><H3 ADD_DATE=\"" + std::to_string(node.added) + "\">" + html_escaped(node.title) + "</H3>\n";
            out += indent + "<DL><p>\n";
            write_netscape(out, node.children, depth + 1);
            out += indent + "</DL><p>\n";
        } else {
            out += indent + "<DT><A HREF=\"" + html_escaped(node.url) + "\" ADD_DATE=\"" + std::to_string(node.added) + "\"";
            if (!node.icon.empty())
                out += " ICON=\"data:image/png;base64," + base64_of(node.icon) + "\"";
            out += ">" + html_escaped(node.title) + "</A>\n";
        }
    }
}

// The few references a bookmarks file's words and addresses hold.
std::string html_unescaped(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        std::size_t const end = text.find(';', i);
        if (end == std::string_view::npos || end - i > 10) {
            out += '&';
            continue;
        }
        std::string_view const name = text.substr(i + 1, end - i - 1);
        if (name == "amp")
            out += '&';
        else if (name == "lt")
            out += '<';
        else if (name == "gt")
            out += '>';
        else if (name == "quot")
            out += '"';
        else if (name == "apos")
            out += '\'';
        else if (name.size() > 1 && name[0] == '#') {
            unsigned long code = 0;
            bool const hex = name[1] == 'x' || name[1] == 'X';
            for (std::size_t k = hex ? 2 : 1; k < name.size(); ++k) {
                char const c = name[k];
                int const digit = c >= '0' && c <= '9' ? c - '0'
                    : hex && c >= 'a' && c <= 'f'      ? c - 'a' + 10
                    : hex && c >= 'A' && c <= 'F'      ? c - 'A' + 10
                                                       : -1;
                if (digit < 0 || code > 0x10FFFF) {
                    code = 0x110000;
                    break;
                }
                code = code * (hex ? 16 : 10) + static_cast<unsigned long>(digit);
            }
            if (code == 0 || code > 0x10FFFF) {
                out += '&';
                continue;
            }
            // UTF-8.
            if (code < 0x80) {
                out += static_cast<char>(code);
            } else if (code < 0x800) {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else if (code < 0x10000) {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (code >> 18));
                out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
        } else {
            out += '&';
            continue;
        }
        i = end;
    }
    return out;
}

char lowered(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// An attribute's value out of a tag's text, by its name in any case.
std::optional<std::string> attribute_of(std::string_view tag, std::string_view name)
{
    for (std::size_t i = 0; i + name.size() < tag.size(); ++i) {
        bool const boundary = i == 0 || tag[i - 1] == ' ' || tag[i - 1] == '\t' || tag[i - 1] == '\n' || tag[i - 1] == '\r';
        if (!boundary)
            continue;
        bool same = true;
        for (std::size_t k = 0; k < name.size() && same; ++k)
            same = lowered(tag[i + k]) == lowered(name[k]);
        if (!same)
            continue;
        std::size_t at = i + name.size();
        while (at < tag.size() && (tag[at] == ' ' || tag[at] == '\t'))
            ++at;
        if (at >= tag.size() || tag[at] != '=')
            continue;
        ++at;
        while (at < tag.size() && (tag[at] == ' ' || tag[at] == '\t'))
            ++at;
        if (at >= tag.size())
            return std::string();
        if (tag[at] == '"' || tag[at] == '\'') {
            char const quote = tag[at];
            std::size_t const end = tag.find(quote, at + 1);
            if (end == std::string_view::npos)
                return std::nullopt;
            return html_unescaped(tag.substr(at + 1, end - at - 1));
        }
        std::size_t end = at;
        while (end < tag.size() && tag[end] != ' ' && tag[end] != '\t' && tag[end] != '\n' && tag[end] != '\r')
            ++end;
        return html_unescaped(tag.substr(at, end - at));
    }
    return std::nullopt;
}

bool tag_is(std::string_view tag, std::string_view name)
{
    if (tag.size() < name.size())
        return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (lowered(tag[i]) != lowered(name[i]))
            return false;
    }
    return tag.size() == name.size() || tag[name.size()] == ' ' || tag[name.size()] == '\t' || tag[name.size()] == '\n'
        || tag[name.size()] == '\r' || tag[name.size()] == '/';
}

std::int64_t number_of(std::optional<std::string> const& text)
{
    if (!text)
        return 0;
    std::int64_t value = 0;
    for (char const c : *text) {
        if (c < '0' || c > '9' || value > 99999999999999LL)
            return 0;
        value = value * 10 + (c - '0');
    }
    return value;
}

}

Bookmarks::Bookmarks()
{
    m_bar.id = bar_id;
    m_bar.folder = true;
    m_bar.title = "Bookmarks bar";
    m_other.id = other_id;
    m_other.folder = true;
    m_other.title = "Other bookmarks";
}

BookmarkNode const* Bookmarks::find(std::uint64_t id) const
{
    if (BookmarkNode const* const found = find_in(m_bar, id))
        return found;
    return find_in(m_other, id);
}

BookmarkNode* Bookmarks::find_mutable(std::uint64_t id)
{
    return const_cast<BookmarkNode*>(find(id));
}

BookmarkNode const* Bookmarks::find_url(std::string_view url) const
{
    if (BookmarkNode const* const found = find_url_in(m_bar, url))
        return found;
    return find_url_in(m_other, url);
}

BookmarkNode const* Bookmarks::parent_of(std::uint64_t id) const
{
    if (BookmarkNode const* const found = parent_in(m_bar, id))
        return found;
    return parent_in(m_other, id);
}

BookmarkNode* Bookmarks::parent_of_mutable(std::uint64_t id)
{
    return const_cast<BookmarkNode*>(parent_of(id));
}

std::size_t Bookmarks::count() const
{
    return count_in(m_bar) + count_in(m_other);
}

std::uint64_t Bookmarks::add_bookmark(std::uint64_t folder, std::string title, std::string url, std::int64_t added,
    std::optional<std::size_t> at)
{
    BookmarkNode* const parent = find_mutable(folder);
    if (!parent || !parent->folder || url.empty())
        return 0;
    BookmarkNode node;
    node.id = m_next_id++;
    node.title = std::move(title);
    node.url = std::move(url);
    node.added = added;
    std::size_t const place = std::min(at.value_or(parent->children.size()), parent->children.size());
    std::uint64_t const id = node.id;
    parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(place), std::move(node));
    touch();
    return id;
}

std::uint64_t Bookmarks::add_folder(std::uint64_t folder, std::string title, std::optional<std::size_t> at)
{
    BookmarkNode* const parent = find_mutable(folder);
    if (!parent || !parent->folder)
        return 0;
    BookmarkNode node;
    node.id = m_next_id++;
    node.folder = true;
    node.title = std::move(title);
    std::size_t const place = std::min(at.value_or(parent->children.size()), parent->children.size());
    std::uint64_t const id = node.id;
    parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(place), std::move(node));
    touch();
    return id;
}

bool Bookmarks::remove(std::uint64_t id)
{
    BookmarkNode* const parent = parent_of_mutable(id);
    if (!parent)
        return false;
    std::erase_if(parent->children, [id](BookmarkNode const& child) { return child.id == id; });
    touch();
    return true;
}

bool Bookmarks::rename(std::uint64_t id, std::string title)
{
    BookmarkNode* const node = find_mutable(id);
    if (!node || id == bar_id || id == other_id)
        return false;
    node->title = std::move(title);
    touch();
    return true;
}

bool Bookmarks::set_url(std::uint64_t id, std::string url)
{
    BookmarkNode* const node = find_mutable(id);
    if (!node || node->folder || url.empty())
        return false;
    node->url = std::move(url);
    touch();
    return true;
}

bool Bookmarks::set_icon(std::uint64_t id, std::string png)
{
    BookmarkNode* const node = find_mutable(id);
    if (!node || node->folder)
        return false;
    if (node->icon == png)
        return true;
    node->icon = std::move(png);
    touch();
    return true;
}

bool Bookmarks::move(std::uint64_t id, std::uint64_t to_folder, std::size_t at)
{
    BookmarkNode* const from = parent_of_mutable(id);
    BookmarkNode const* const moving = find(id);
    BookmarkNode const* const target = find(to_folder);
    if (!from || !moving || !target || !target->folder)
        return false;
    // Not into itself, nor into anything below itself.
    if (find_in(*moving, to_folder))
        return false;
    auto const where = std::find_if(from->children.begin(), from->children.end(),
        [id](BookmarkNode const& child) { return child.id == id; });
    std::size_t const was = static_cast<std::size_t>(where - from->children.begin());
    BookmarkNode node = std::move(*where);
    from->children.erase(where);
    // The target is looked up again: taking the node out may have moved it.
    BookmarkNode* const into = find_mutable(to_folder);
    std::size_t place = at;
    if (into == from && was < at)
        --place; // `at` counted the node itself
    place = std::min(place, into->children.size());
    into->children.insert(into->children.begin() + static_cast<std::ptrdiff_t>(place), std::move(node));
    touch();
    return true;
}

void Bookmarks::set_bar_mode(BookmarksBarMode mode)
{
    if (m_bar_mode == mode)
        return;
    m_bar_mode = mode;
    touch();
}

std::string Bookmarks::to_json() const
{
    std::string out = "{\n  \"version\": 1,\n  \"bar-shown\": ";
    out += m_bar_mode == BookmarksBarMode::Always ? "\"always\"" : m_bar_mode == BookmarksBarMode::NewTab ? "\"new-tab\"" : "\"never\"";
    out += ",\n  \"bar\": ";
    write_nodes(out, m_bar.children, 1);
    out += ",\n  \"other\": ";
    write_nodes(out, m_other.children, 1);
    out += "\n}\n";
    return out;
}

std::optional<Bookmarks> Bookmarks::from_json(std::string_view text)
{
    std::optional<JsonValue> const parsed = JsonValue::parse(text);
    if (!parsed || !parsed->is_object())
        return std::nullopt;
    JsonValue const* const bar = parsed->get("bar");
    JsonValue const* const other = parsed->get("other");
    if ((bar && !bar->is_array()) || (other && !other->is_array()) || (!bar && !other))
        return std::nullopt;
    Bookmarks out;
    std::size_t nodes = 0;
    auto const read = [&](auto const& self, JsonValue const& list, BookmarkNode& into, std::size_t depth) -> void {
        if (depth > max_depth)
            return;
        for (JsonValue const& value : list.as_array()) {
            if (!value.is_object() || ++nodes > max_nodes)
                continue;
            BookmarkNode node;
            node.id = out.m_next_id++;
            if (JsonValue const* const title = value.get("title"); title && title->is_string())
                node.title = title->as_string();
            if (JsonValue const* const added = value.get("added"); added && added->is_number() && added->as_number() > 0)
                node.added = static_cast<std::int64_t>(added->as_number());
            if (JsonValue const* const children = value.get("children"); children && children->is_array()) {
                node.folder = true;
                self(self, *children, node, depth + 1);
            } else {
                JsonValue const* const url = value.get("url");
                if (!url || !url->is_string() || url->as_string().empty())
                    continue;
                node.url = url->as_string();
                if (JsonValue const* const icon = value.get("icon"); icon && icon->is_string()) {
                    if (std::optional<std::vector<std::uint8_t>> const bytes = base64_decode(icon->as_string()))
                        node.icon.assign(bytes->begin(), bytes->end());
                }
            }
            into.children.push_back(std::move(node));
        }
    };
    if (bar)
        read(read, *bar, out.m_bar, 0);
    if (other)
        read(read, *other, out.m_other, 0);
    if (JsonValue const* const shown = parsed->get("bar-shown"); shown && shown->is_string()) {
        if (shown->as_string() == "new-tab")
            out.m_bar_mode = BookmarksBarMode::NewTab;
        else if (shown->as_string() == "never")
            out.m_bar_mode = BookmarksBarMode::Never;
    }
    out.m_changes = 0;
    return out;
}

std::string Bookmarks::to_netscape_html() const
{
    std::string out = "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
                      "<!-- This is an automatically generated file.\n"
                      "     It will be read and overwritten.\n"
                      "     DO NOT EDIT! -->\n"
                      "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
                      "<TITLE>Bookmarks</TITLE>\n"
                      "<H1>Bookmarks</H1>\n"
                      "<DL><p>\n";
    out += "    <DT><H3 ADD_DATE=\"0\" PERSONAL_TOOLBAR_FOLDER=\"true\">" + html_escaped(m_bar.title) + "</H3>\n";
    out += "    <DL><p>\n";
    write_netscape(out, m_bar.children, 2);
    out += "    </DL><p>\n";
    write_netscape(out, m_other.children, 1);
    out += "</DL><p>\n";
    return out;
}

std::size_t Bookmarks::import_netscape_html(std::string_view html)
{
    // A scan of tags in order, as every reader of this format does it: the
    // file is not well-formed HTML and is not meant to be. An <H3> names the
    // folder the next <DL> opens; </DL> closes it; an <A> is a bookmark in
    // the folder open at that point.
    std::vector<std::uint64_t> open; // the folders open, outermost first; 0 for one that is not kept
    std::optional<std::string> pending_title;
    bool pending_is_bar = false;
    std::int64_t pending_added = 0;
    std::size_t came = 0;
    std::size_t nodes = 0;
    std::size_t at = 0;
    while (at < html.size()) {
        std::size_t const open_at = html.find('<', at);
        if (open_at == std::string_view::npos)
            break;
        if (html.substr(open_at, 4) == "<!--") {
            std::size_t const end = html.find("-->", open_at + 4);
            at = end == std::string_view::npos ? html.size() : end + 3;
            continue;
        }
        // The tag ends at the first > that is not inside a quoted value: an
        // address may hold one.
        std::size_t close_at = std::string_view::npos;
        char quote = 0;
        for (std::size_t i = open_at + 1; i < html.size(); ++i) {
            char const c = html[i];
            if (quote != 0) {
                if (c == quote)
                    quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                close_at = i;
                break;
            }
        }
        if (close_at == std::string_view::npos)
            break;
        std::string_view const tag = html.substr(open_at + 1, close_at - open_at - 1);
        at = close_at + 1;
        auto const text_until = [&](std::string_view closing) {
            // The words up to the closing tag, which this format always writes.
            std::string lower;
            std::size_t end = at;
            while (end < html.size()) {
                std::size_t const next = html.find('<', end);
                if (next == std::string_view::npos) {
                    end = html.size();
                    break;
                }
                std::string_view const rest = html.substr(next + 1, closing.size());
                bool same = rest.size() == closing.size();
                for (std::size_t k = 0; k < closing.size() && same; ++k)
                    same = lowered(rest[k]) == closing[k];
                if (same) {
                    end = next;
                    break;
                }
                end = next + 1;
            }
            std::string const words = html_unescaped(html.substr(at, end - at));
            at = end;
            return words;
        };
        if (tag_is(tag, "h3")) {
            pending_is_bar = attribute_of(tag, "PERSONAL_TOOLBAR_FOLDER").value_or("") == "true";
            pending_added = number_of(attribute_of(tag, "ADD_DATE"));
            pending_title = text_until("/h3");
        } else if (tag_is(tag, "dl")) {
            if (open.size() >= max_depth) {
                open.push_back(0);
            } else if (open.empty() && !pending_title) {
                open.push_back(other_id); // the file's own outermost list
            } else if (pending_is_bar) {
                open.push_back(bar_id);
            } else {
                std::uint64_t const parent = open.empty() ? other_id : open.back();
                std::uint64_t made = 0;
                if (parent != 0 && ++nodes <= max_nodes) {
                    made = add_folder(parent, pending_title.value_or(""));
                    if (BookmarkNode* const node = find_mutable(made))
                        node->added = pending_added;
                }
                open.push_back(made);
            }
            pending_title.reset();
            pending_is_bar = false;
        } else if (tag_is(tag, "/dl")) {
            if (!open.empty())
                open.pop_back();
        } else if (tag_is(tag, "a")) {
            std::optional<std::string> const href = attribute_of(tag, "HREF");
            std::int64_t const added = number_of(attribute_of(tag, "ADD_DATE"));
            std::optional<std::string> const icon = attribute_of(tag, "ICON");
            std::string const title = text_until("/a");
            std::uint64_t const parent = open.empty() ? other_id : open.back();
            // A query another browser runs in place of a bookmark is not one here.
            if (!href || href->empty() || href->starts_with("place:") || parent == 0 || ++nodes > max_nodes)
                continue;
            std::uint64_t const made = add_bookmark(parent, title, *href, added);
            if (made == 0)
                continue;
            ++came;
            constexpr std::string_view png_prefix = "data:image/png;base64,";
            if (icon && icon->starts_with(png_prefix)) {
                if (std::optional<std::vector<std::uint8_t>> const bytes = base64_decode(std::string_view(*icon).substr(png_prefix.size())))
                    set_icon(made, std::string(bytes->begin(), bytes->end()));
            }
        }
    }
    return came;
}

}
