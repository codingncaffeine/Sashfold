#include "ui/BookmarksPage.h"

#include "core/Base64.h"
#include "ui/InternalPages.h"

#include <span>
#include <utility>
#include <vector>

namespace sashfold::ui {

namespace {

// application/x-www-form-urlencoded, one value: + is a space, %xx a byte.
std::string form_decoded(std::string_view text)
{
    auto const hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
        } else if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::optional<std::uint64_t> id_of(std::string const& text)
{
    if (text.empty() || text.size() > 18)
        return std::nullopt;
    std::uint64_t value = 0;
    for (char const c : text) {
        if (c < '0' || c > '9')
            return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value;
}

std::string escaped(std::string_view text)
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

std::string trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r'))
        text.remove_suffix(1);
    return std::string(text);
}

void folders_in(BookmarkNode const& folder, int depth, std::vector<std::pair<BookmarkNode const*, int>>& out)
{
    out.emplace_back(&folder, depth);
    for (BookmarkNode const& child : folder.children) {
        if (child.folder)
            folders_in(child, depth + 1, out);
    }
}

void write_folder(std::string& out, BookmarkNode const& folder)
{
    if (folder.children.empty()) {
        out += "<p class=\"none\">Nothing here yet.</p>\n";
        return;
    }
    out += "<ul>\n";
    for (BookmarkNode const& node : folder.children) {
        std::string const id = std::to_string(node.id);
        std::string const actions = " <span class=\"do\"><a href=\"about:bookmarks?edit=" + id + "\">Edit</a> <a href=\"about:bookmarks?up=" + id
            + "\">Up</a> <a href=\"about:bookmarks?down=" + id + "\">Down</a> <a href=\"about:bookmarks?remove=" + id + "\">Remove</a></span>";
        if (node.folder) {
            out += "<li class=\"folder\"><span class=\"name\">" + escaped(node.title.empty() ? "(no name)" : node.title) + "</span>" + actions + "\n";
            write_folder(out, node);
            out += "</li>\n";
            continue;
        }
        out += "<li>";
        if (!node.icon.empty()) {
            out += "<img alt=\"\" width=\"16\" height=\"16\" src=\"data:image/png;base64,"
                + base64_encode(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(node.icon.data()), node.icon.size()))
                + "\"> ";
        }
        out += "<a class=\"name\" href=\"" + escaped(node.url) + "\">" + escaped(node.title.empty() ? node.url : node.title) + "</a> <span class=\"where\">"
            + escaped(node.url) + "</span>" + actions + "</li>\n";
    }
    out += "</ul>\n";
}

}

BookmarksRequest bookmarks_request_of(std::optional<std::string> const& url_query)
{
    BookmarksRequest request;
    if (!url_query)
        return request;
    std::string_view rest = *url_query;
    while (!rest.empty()) {
        std::size_t const amp = rest.find('&');
        std::string_view const pair = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
        std::size_t const equals = pair.find('=');
        std::string const name = form_decoded(pair.substr(0, equals));
        std::string const value = equals == std::string_view::npos ? std::string() : form_decoded(pair.substr(equals + 1));
        if (name == "remove")
            request.remove = id_of(value);
        else if (name == "up")
            request.up = id_of(value);
        else if (name == "down")
            request.down = id_of(value);
        else if (name == "edit")
            request.edit = id_of(value);
        else if (name == "save")
            request.save = id_of(value);
        else if (name == "title")
            request.title = value;
        else if (name == "foldername")
            request.title = value; // a new folder's, from its own form
        else if (name == "url")
            request.url = value;
        else if (name == "folder")
            request.folder = id_of(value);
        else if (name == "newfolder")
            request.new_folder_in = id_of(value);
        else if (name == "bar")
            request.bar = value;
        else if (name == "import")
            request.import_path = trimmed(value);
        else if (name == "export")
            request.export_file = true;
    }
    return request;
}

std::string apply_bookmarks_request(BookmarksRequest const& request, Bookmarks& bookmarks)
{
    if (request.remove) {
        BookmarkNode const* const node = bookmarks.find(*request.remove);
        std::string const name = node ? (node->title.empty() ? node->url : node->title) : std::string();
        return bookmarks.remove(*request.remove) ? "Removed: " + name : "That one is not there any more.";
    }
    if (request.up || request.down) {
        std::uint64_t const id = request.up ? *request.up : *request.down;
        BookmarkNode const* const parent = bookmarks.parent_of(id);
        if (!parent)
            return "That one is not there any more.";
        std::size_t at = 0;
        while (at < parent->children.size() && parent->children[at].id != id)
            ++at;
        if (request.up) {
            if (at == 0)
                return "It is the first in its folder already.";
            bookmarks.move(id, parent->id, at - 1);
        } else {
            if (at + 1 >= parent->children.size())
                return "It is the last in its folder already.";
            bookmarks.move(id, parent->id, at + 2); // past the one after it, as the places stand
        }
        return "Moved.";
    }
    if (request.save) {
        BookmarkNode const* const node = bookmarks.find(*request.save);
        if (!node)
            return "That one is not there any more.";
        bool const folder = node->folder;
        if (request.title)
            bookmarks.rename(*request.save, trimmed(*request.title));
        if (!folder && request.url && !trimmed(*request.url).empty())
            bookmarks.set_url(*request.save, trimmed(*request.url));
        if (request.folder) {
            BookmarkNode const* const parent = bookmarks.parent_of(*request.save);
            BookmarkNode const* const target = bookmarks.find(*request.folder);
            if (parent && target && target->folder && target->id != parent->id
                && !bookmarks.move(*request.save, target->id, target->children.size()))
                return "A folder cannot go into itself.";
        }
        return "Saved.";
    }
    if (request.new_folder_in) {
        std::string const name = request.title ? trimmed(*request.title) : std::string();
        if (name.empty())
            return "A folder needs a name.";
        return bookmarks.add_folder(*request.new_folder_in, name) != 0 ? "Folder made: " + name : "There is no such folder to put it in.";
    }
    if (request.bar) {
        if (*request.bar == "always")
            bookmarks.set_bar_mode(BookmarksBarMode::Always);
        else if (*request.bar == "new-tab")
            bookmarks.set_bar_mode(BookmarksBarMode::NewTab);
        else if (*request.bar == "never")
            bookmarks.set_bar_mode(BookmarksBarMode::Never);
        else
            return {};
        return "The bar is shown: " + std::string(*request.bar == "always" ? "always" : *request.bar == "new-tab" ? "on the new-tab page" : "never") + ".";
    }
    return {};
}

std::string bookmarks_page(Bookmarks const& bookmarks, std::string_view notice, std::optional<std::uint64_t> editing)
{
    std::string body = "<h1>Bookmarks</h1>\n";
    if (!notice.empty())
        body += "<p class=\"notice\">" + escaped(notice) + "</p>\n";

    std::vector<std::pair<BookmarkNode const*, int>> folders;
    folders_in(bookmarks.bar(), 0, folders);
    folders_in(bookmarks.other(), 0, folders);

    if (BookmarkNode const* const node = editing ? bookmarks.find(*editing) : nullptr;
        node && node->id != Bookmarks::bar_id && node->id != Bookmarks::other_id) {
        BookmarkNode const* const parent = bookmarks.parent_of(node->id);
        body += "<form class=\"edit\" action=\"about:bookmarks\"><input type=\"hidden\" name=\"save\" value=\"" + std::to_string(node->id) + "\">\n";
        body += "<label>Name <input id=\"edit-title\" name=\"title\" value=\"" + escaped(node->title) + "\"></label>\n";
        if (!node->folder)
            body += "<label>Address <input id=\"edit-url\" name=\"url\" value=\"" + escaped(node->url) + "\"></label>\n";
        body += "<label>Folder <select name=\"folder\">";
        for (auto const& [folder, depth] : folders) {
            body += "<option value=\"" + std::to_string(folder->id) + "\"" + (parent && folder->id == parent->id ? " selected" : "") + ">";
            for (int i = 0; i < depth; ++i)
                body += "\xC2\xA0\xC2\xA0";
            body += escaped(folder->title) + "</option>";
        }
        body += "</select></label>\n<button>Save</button> <a href=\"about:bookmarks\">Leave it as it is</a></form>\n";
    }

    BookmarksBarMode const mode = bookmarks.bar_mode();
    auto const choice = [&](char const* value, char const* words, BookmarksBarMode is) {
        return mode == is ? std::string("<b>") + words + "</b>"
                          : std::string("<a href=\"about:bookmarks?bar=") + value + "\">" + words + "</a>";
    };
    body += "<p class=\"bar\">Show the bar: " + choice("always", "always", BookmarksBarMode::Always) + " \xC2\xB7 "
        + choice("new-tab", "on the new-tab page", BookmarksBarMode::NewTab) + " \xC2\xB7 " + choice("never", "never", BookmarksBarMode::Never)
        + " <span class=\"where\">(Ctrl+Shift+B)</span></p>\n";

    auto const root = [&](BookmarkNode const& folder) {
        body += "<h2>" + escaped(folder.title) + "</h2>\n";
        write_folder(body, folder);
        body += "<form class=\"new\" action=\"about:bookmarks\"><input type=\"hidden\" name=\"newfolder\" value=\"" + std::to_string(folder.id)
            + "\"><input id=\"folder-name-" + std::to_string(folder.id) + "\" name=\"foldername\" placeholder=\"A new folder's name\"> <button>New folder</button></form>\n";
    };
    root(bookmarks.bar());
    root(bookmarks.other());

    body += "<h2>From and to other browsers</h2>\n"
            "<p>Every browser writes its bookmarks out as a bookmarks file (\"Export bookmarks to HTML\") and reads one in.</p>\n"
            "<form class=\"new\" action=\"about:bookmarks\"><input id=\"import-path\" name=\"import\" placeholder=\"/home/you/bookmarks.html\" size=\"40\"> <button>Bring in that file</button></form>\n"
            "<p><a href=\"about:bookmarks?export=1\">Write mine out as a bookmarks file</a> <span class=\"where\">(into the downloads folder)</span></p>\n";

    static constexpr std::string_view style
        = "ul{list-style:none;margin:0 0 0 1.2em;padding:0}li{margin:.35em 0}li.folder>.name{font-weight:bold}"
          ".where{color:#777;font-size:85%;margin-left:.4em}.do{font-size:85%;margin-left:.8em}.do a{margin-right:.5em}"
          ".none{color:#777;margin-left:1.2em}.notice{background:#e8f0fe;padding:.5em .8em;border-radius:6px}"
          "form.edit{background:#f4f4f4;padding:.8em;border-radius:6px}form.edit label{display:block;margin:.3em 0}"
          "form.edit input{width:30em}form.new{margin:.4em 0 1.2em 1.2em}img{vertical-align:middle}";
    return internal_page("Bookmarks", body, style);
}

}
