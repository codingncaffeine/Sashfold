#include "ui/BookmarksPage.h"

#include "core/Base64.h"
#include "ui/InternalPages.h"

#include <span>
#include <utility>

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

// A value for a query: what is not a letter, a digit or one of -._~ as %xx.
std::string query_encoded(std::string_view text)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (char const ch : text) {
        auto const c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            out += ch;
        } else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
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
    return html_escape(text);
}

std::string trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r'))
        text.remove_suffix(1);
    return std::string(text);
}

std::size_t bookmarks_in(BookmarkNode const& node)
{
    std::size_t count = node.folder ? 0 : 1;
    for (BookmarkNode const& child : node.children)
        count += bookmarks_in(child);
    return count;
}

std::string counted(std::size_t count, char const* one, char const* many)
{
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// The page's own drawings: a folder, a sheet for a page with no icon of its
// own, a lens, a star.
constexpr std::string_view folder_icon
    = R"(<svg class="ico" width="16" height="16" viewBox="0 0 16 16"><path d="M1.5 4.5h4.6l1.4 1.5h7v7.5h-13z" fill="#e8edf6" stroke="#5a6f96" stroke-width="1.2"/></svg>)";
constexpr std::string_view page_icon
    = R"(<svg class="ico" width="16" height="16" viewBox="0 0 16 16"><path d="M3.5 1.5h6l3 3v10h-9z" fill="#ffffff" stroke="#8a919e" stroke-width="1.2"/><path d="M9.5 1.5v3h3" fill="none" stroke="#8a919e" stroke-width="1.2"/></svg>)";
constexpr std::string_view transfer_icon
    = R"(<svg class="ico" width="16" height="16" viewBox="0 0 16 16"><path d="M4.5 2v9M1.8 8.3l2.7 2.7 2.7-2.7M11.5 14V5M8.8 7.7l2.7-2.7 2.7 2.7" fill="none" stroke="#5a6f96" stroke-width="1.3"/></svg>)";
constexpr std::string_view star_icon
    = R"(<svg width="56" height="56" viewBox="0 0 16 16"><path d="M8 2.2l1.7 3.9 4.2.4-3.2 2.8 1 4.1L8 11.2l-3.7 2.2 1-4.1L2.1 6.5l4.2-.4z" fill="#eef1f6" stroke="#c3c9d4" stroke-width="0.7"/></svg>)";

std::string view_query(BookmarksRequest const& view)
{
    if (view.transfer)
        return "view=transfer";
    std::string query;
    if (view.in)
        query = "in=" + std::to_string(*view.in);
    if (view.search && !view.search->empty())
        query += (query.empty() ? "" : "&") + std::string("q=") + query_encoded(*view.search);
    return query;
}

// An address of the page that asks for something and keeps the view.
std::string asking(BookmarksRequest const& view, std::string const& what)
{
    std::string const kept = view_query(view);
    return "about:bookmarks?" + what + (kept.empty() ? "" : "&" + kept);
}

std::string hidden_view(BookmarksRequest const& view)
{
    std::string out;
    if (view.transfer)
        return "<input type=\"hidden\" name=\"view\" value=\"transfer\">";
    if (view.in)
        out += "<input type=\"hidden\" name=\"in\" value=\"" + std::to_string(*view.in) + "\">";
    if (view.search && !view.search->empty())
        out += "<input type=\"hidden\" name=\"q\" value=\"" + escaped(*view.search) + "\">";
    return out;
}

void folders_in(BookmarkNode const& folder, int depth, std::vector<std::pair<BookmarkNode const*, int>>& out)
{
    out.emplace_back(&folder, depth);
    for (BookmarkNode const& child : folder.children) {
        if (child.folder)
            folders_in(child, depth + 1, out);
    }
}

std::string icon_of(BookmarkNode const& node)
{
    if (node.folder)
        return std::string(folder_icon);
    if (node.icon.empty())
        return std::string(page_icon);
    return "<img class=\"ico\" alt=\"\" width=\"16\" height=\"16\" src=\"data:image/png;base64,"
        + base64_encode(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(node.icon.data()), node.icon.size())) + "\">";
}

// The form a bookmark or a folder is edited in, in its place in the list.
std::string edit_form(Bookmarks const& bookmarks, BookmarkNode const& node, BookmarksRequest const& view)
{
    std::vector<std::pair<BookmarkNode const*, int>> folders;
    folders_in(bookmarks.bar(), 0, folders);
    folders_in(bookmarks.other(), 0, folders);
    BookmarkNode const* const parent = bookmarks.parent_of(node.id);
    std::string out = "<form class=\"item editing\" action=\"about:bookmarks\"><input type=\"hidden\" name=\"save\" value=\""
        + std::to_string(node.id) + "\">" + hidden_view(view) + "<div class=\"fields\">";
    out += "<label>Name<input name=\"title\" value=\"" + escaped(node.title) + "\"></label>";
    if (!node.folder)
        out += "<label class=\"long\">Address<input name=\"url\" value=\"" + escaped(node.url) + "\"></label>";
    out += "<label>Folder<select name=\"folder\">";
    for (auto const& [folder, depth] : folders) {
        // A folder does not go into itself or below itself: those are not offered.
        bool below = false;
        for (BookmarkNode const* up = folder; up && !below; up = bookmarks.parent_of(up->id))
            below = up->id == node.id;
        if (below)
            continue;
        out += "<option value=\"" + std::to_string(folder->id) + "\"" + (parent && folder->id == parent->id ? " selected" : "") + ">";
        for (int i = 0; i < depth; ++i)
            out += "\xC2\xA0\xC2\xA0\xC2\xA0";
        out += escaped(folder->title) + "</option>";
    }
    out += "</select></label></div><div class=\"buttons\"><button class=\"primary\">Save</button><a class=\"button\" href=\"about:bookmarks"
        + (view_query(view).empty() ? std::string() : "?" + view_query(view)) + "\">Cancel</a></div></form>\n";
    return out;
}

// One row of a list: the icon, the name over the address (or over where it
// is kept, for something a search found), and what can be done to it.
std::string item_row(Bookmarks const& bookmarks, BookmarkNode const& node, BookmarksRequest const& view,
    std::vector<BookmarkNode const*> const* found_under)
{
    if (view.edit && *view.edit == node.id)
        return edit_form(bookmarks, node, view);
    std::string const id = std::to_string(node.id);
    std::string out = "<div class=\"item\">" + icon_of(node) + "<div class=\"what\">";
    if (node.folder) {
        out += "<a class=\"name\" href=\"about:bookmarks?in=" + id + "\">" + escaped(node.title.empty() ? "(no name)" : node.title) + "</a>";
        out += "<div class=\"sub\">" + counted(bookmarks_in(node), "bookmark", "bookmarks") + "</div>";
    } else {
        out += "<a class=\"name\" href=\"" + escaped(node.url) + "\">" + escaped(node.title.empty() ? node.url : node.title) + "</a>";
        out += "<div class=\"sub\">" + escaped(node.url) + "</div>";
    }
    if (found_under) {
        std::string path;
        for (BookmarkNode const* const folder : *found_under)
            path += (path.empty() ? "" : " \xE2\x80\xBA ") + folder->title;
        out += "<div class=\"sub path\">in " + escaped(path) + "</div>";
    }
    out += "</div><div class=\"do\"><a href=\"" + escaped(asking(view, "edit=" + id)) + "\">Edit</a>";
    if (!found_under) {
        out += "<a href=\"" + escaped(asking(view, "up=" + id)) + "\">Up</a><a href=\"" + escaped(asking(view, "down=" + id)) + "\">Down</a>";
    }
    out += "<a class=\"danger\" href=\"" + escaped(asking(view, "remove=" + id)) + "\">Remove</a></div></div>\n";
    return out;
}

void side_folder(std::string& out, BookmarkNode const& folder, int depth, std::uint64_t shown, bool any_shown)
{
    bool const here = any_shown && folder.id == shown;
    out += "<a class=\"place" + std::string(here ? " here" : "") + "\" style=\"padding-left: " + std::to_string(12 + depth * 18)
        + "px\" href=\"about:bookmarks?in=" + std::to_string(folder.id) + "\">" + std::string(folder_icon) + "<span class=\"label\">"
        + escaped(folder.title.empty() ? "(no name)" : folder.title) + "</span><span class=\"count\">" + std::to_string(bookmarks_in(folder))
        + "</span></a>\n";
    for (BookmarkNode const& child : folder.children) {
        if (child.folder)
            side_folder(out, child, depth + 1, shown, any_shown);
    }
}

std::string transfer_view(BookmarksRequest const& view, BookmarksSurroundings const& surroundings)
{
    std::string out = "<div class=\"head\"><h2>Import and backup</h2></div>\n";

    out += "<div class=\"section\"><h3>From another browser</h3>"
           "<p class=\"explain\">Bookmarks of the browsers found on this computer, read straight from where they keep them. "
           "What is brought in is added to yours: a browser's toolbar goes onto your bar, the rest into Other bookmarks, "
           "and nothing of yours is replaced.</p>\n";
    bool any_browser = false;
    for (BookmarkSource const& source : surroundings.sources) {
        if (source.browser.empty())
            continue;
        any_browser = true;
        out += "<div class=\"item\">" + std::string(transfer_icon) + "<div class=\"what\"><span class=\"name\">" + escaped(source.browser)
            + "</span><div class=\"sub\">" + escaped(source.detail) + " \xC2\xB7 " + counted(source.count, "bookmark", "bookmarks")
            + (source.when.empty() ? std::string() : " \xC2\xB7 " + escaped(source.when)) + "</div></div><div class=\"do\"><a class=\"button\" href=\""
            + escaped(asking(view, "importfrom=" + query_encoded(source.key))) + "\">Import</a></div></div>\n";
    }
    if (!any_browser)
        out += "<p class=\"none\">No other browser's bookmarks were found on this computer.</p>\n";
    out += "</div>\n";

    out += "<div class=\"section\"><h3>From a bookmarks file</h3>"
           "<p class=\"explain\">Every browser can write its bookmarks out as a bookmarks file (\"Export bookmarks to HTML\"). "
           "Those found in your downloads, desktop, documents and home folders are listed; any other can be named by its path.</p>\n";
    bool any_file = false;
    for (BookmarkSource const& source : surroundings.sources) {
        if (!source.browser.empty())
            continue;
        any_file = true;
        out += "<div class=\"item\">" + std::string(page_icon) + "<div class=\"what\"><span class=\"name\">" + escaped(source.detail)
            + "</span><div class=\"sub\">" + counted(source.count, "bookmark", "bookmarks")
            + (source.when.empty() ? std::string() : " \xC2\xB7 " + escaped(source.when)) + "</div></div><div class=\"do\"><a class=\"button\" href=\""
            + escaped(asking(view, "importfrom=" + query_encoded(source.key))) + "\">Import</a></div></div>\n";
    }
    if (!any_file)
        out += "<p class=\"none\">No bookmarks file was found in those folders.</p>\n";
    out += "<form class=\"line\" action=\"about:bookmarks\">" + hidden_view(view)
        + "<input class=\"wide\" name=\"import\" placeholder=\"The path of a bookmarks file, e.g. /home/you/bookmarks.html\"><button>Import this file</button></form></div>\n";

    out += "<div class=\"section\"><h3>Export</h3>"
           "<p class=\"explain\">Your bookmarks written out as a bookmarks file that Firefox, Chrome and every other browser reads, "
           "into your downloads folder, named with today's date.</p>\n<p><a class=\"button primary\" href=\"" + escaped(asking(view, "export=1"))
        + "\">Export bookmarks to HTML</a></p></div>\n";

    out += "<div class=\"section\"><h3>Backups</h3>"
           "<p class=\"explain\">A copy of your bookmarks is kept for each day they change, the last ten of them. "
           "Restoring one replaces your bookmarks with it; how they stand now is kept as a copy first.</p>\n";
    if (surroundings.backups.empty())
        out += "<p class=\"none\">No copy has been kept yet.</p>\n";
    for (BookmarkBackup const& backup : surroundings.backups) {
        bool const asked = view.restore && *view.restore == backup.key && !view.sure;
        out += "<div class=\"item" + std::string(asked ? " asked" : "") + "\">" + std::string(page_icon) + "<div class=\"what\"><span class=\"name\">"
            + escaped(backup.when) + "</span><div class=\"sub\">" + counted(backup.count, "bookmark", "bookmarks") + "</div></div><div class=\"do\">";
        if (asked) {
            out += "<span class=\"sure\">Replace your bookmarks with this copy?</span><a class=\"button danger\" href=\""
                + escaped(asking(view, "restore=" + query_encoded(backup.key) + "&sure=1")) + "\">Yes, restore it</a><a class=\"button\" href=\"about:bookmarks?view=transfer\">No</a>";
        } else {
            out += "<a class=\"button\" href=\"" + escaped(asking(view, "restore=" + query_encoded(backup.key))) + "\">Restore</a>";
        }
        out += "</div></div>\n";
    }
    out += "</div>\n";
    return out;
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
        if (name == "in")
            request.in = id_of(value);
        else if (name == "q")
            request.search = trimmed(value);
        else if (name == "view")
            request.transfer = value == "transfer";
        else if (name == "edit")
            request.edit = id_of(value);
        else if (name == "remove")
            request.remove = id_of(value);
        else if (name == "undo")
            request.undo = true;
        else if (name == "up")
            request.up = id_of(value);
        else if (name == "down")
            request.down = id_of(value);
        else if (name == "sort")
            request.sort = id_of(value);
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
        else if (name == "add")
            request.add_in = id_of(value);
        else if (name == "openall")
            request.open_all = id_of(value);
        else if (name == "bar")
            request.bar = value;
        else if (name == "importfrom")
            request.import_from = value;
        else if (name == "import")
            request.import_path = trimmed(value);
        else if (name == "restore")
            request.restore = value;
        else if (name == "sure")
            request.sure = true;
        else if (name == "export")
            request.export_file = true;
    }
    if (request.search && request.search->empty())
        request.search.reset();
    // A restore not yet confirmed only asks the page to ask.
    if (request.restore && !request.sure) {
        request.transfer = true;
    }
    return request;
}

std::string bookmarks_view_address(BookmarksRequest const& request)
{
    std::string const query = view_query(request);
    return query.empty() ? "about:bookmarks" : "about:bookmarks?" + query;
}

std::string apply_bookmarks_request(BookmarksRequest const& request, Bookmarks& bookmarks)
{
    if (request.remove) {
        BookmarkNode const* const node = bookmarks.find(*request.remove);
        std::string const name = node ? (node->title.empty() ? node->url : node->title) : std::string();
        return bookmarks.remove(*request.remove) ? "Removed: " + name : "That one is not there any more.";
    }
    if (request.undo) {
        std::string const name = bookmarks.removed_name();
        return bookmarks.undo_remove() ? "Put back: " + name : "There is nothing to put back.";
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
    if (request.sort) {
        BookmarkNode const* const folder = bookmarks.find(*request.sort);
        return bookmarks.sort_by_name(*request.sort) ? "Sorted by name: " + folder->title : "There is no such folder.";
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
    if (request.add_in) {
        std::string const address = request.url ? trimmed(*request.url) : std::string();
        if (address.empty())
            return "A bookmark needs an address.";
        std::string name = request.title ? trimmed(*request.title) : std::string();
        if (name.empty())
            name = address;
        return bookmarks.add_bookmark(*request.add_in, name, address, 0) != 0 ? "Bookmark added: " + name
                                                                                : "There is no such folder to put it in.";
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
        return "The bookmarks bar is shown " + std::string(*request.bar == "always" ? "always." : *request.bar == "new-tab" ? "on the new-tab page only." : "never.");
    }
    return {};
}

std::string bookmarks_page(Bookmarks const& bookmarks, BookmarksRequest const& view, BookmarksNotice const& notice,
    BookmarksSurroundings const& surroundings)
{
    // The folder shown: the one asked for, when it is one; the bar's otherwise.
    BookmarkNode const* shown = view.in ? bookmarks.find(*view.in) : nullptr;
    if (!shown || !shown->folder)
        shown = &bookmarks.bar();
    bool const searching = view.search.has_value();
    bool const folder_view = !searching && !view.transfer;

    std::string body = "<div class=\"top\"><h1>Bookmarks</h1><form class=\"search\" action=\"about:bookmarks\">"
                       "<input name=\"q\" placeholder=\"Search bookmarks\" value=\""
        + escaped(view.search.value_or("")) + "\"><button>Search</button></form></div>\n";
    if (!notice.words.empty()) {
        body += "<p class=\"notice\">" + escaped(notice.words);
        if (!notice.link_words.empty())
            body += " <a href=\"" + escaped(notice.link_address) + "\">" + escaped(notice.link_words) + "</a>";
        body += "</p>\n";
    }

    body += "<div class=\"panes\">\n<div class=\"side\"><div class=\"card places\">\n";
    side_folder(body, bookmarks.bar(), 0, shown->id, folder_view);
    side_folder(body, bookmarks.other(), 0, shown->id, folder_view);
    body += "<div class=\"rule\"></div><a class=\"place" + std::string(view.transfer ? " here" : "")
        + "\" style=\"padding-left: 12px\" href=\"about:bookmarks?view=transfer\">" + std::string(transfer_icon)
        + "<span class=\"label\">Import and backup</span></a>\n</div>\n";

    BookmarksBarMode const mode = bookmarks.bar_mode();
    auto const choice = [&](char const* value, char const* words, BookmarksBarMode is) {
        return mode == is ? std::string("<b>") + words + "</b>"
                          : "<a href=\"" + escaped(asking(view, std::string("bar=") + value)) + "\">" + words + "</a>";
    };
    body += "<div class=\"card pad\"><h3>Bookmarks bar</h3><div class=\"choices\">" + choice("always", "Always show", BookmarksBarMode::Always)
        + choice("new-tab", "On the new-tab page", BookmarksBarMode::NewTab) + choice("never", "Never", BookmarksBarMode::Never)
        + "</div><p class=\"hint\">Ctrl+Shift+B shows and hides it. Ctrl+D, or the star in the toolbar, bookmarks the page in front.</p></div>\n</div>\n";

    body += "<div class=\"main card\">\n";
    if (view.transfer) {
        body += transfer_view(view, surroundings);
    } else if (searching) {
        std::vector<Bookmarks::Found> const found = bookmarks.search(*view.search);
        body += "<div class=\"head\"><h2>\xE2\x80\x9C" + escaped(*view.search) + "\xE2\x80\x9D</h2><span class=\"tools\"><span class=\"muted\">"
            + counted(found.size(), "bookmark", "bookmarks") + " found</span><a href=\"about:bookmarks\">Clear the search</a></span></div>\n";
        if (found.empty())
            body += "<div class=\"empty\">" + std::string(star_icon) + "<p>No bookmark's name or address holds those words.</p></div>\n";
        for (Bookmarks::Found const& one : found)
            body += item_row(bookmarks, *one.node, view, &one.path);
    } else {
        std::string path;
        for (BookmarkNode const* const folder : bookmarks.path_of(shown->id))
            path += "<a href=\"about:bookmarks?in=" + std::to_string(folder->id) + "\">" + escaped(folder->title) + "</a> \xE2\x80\xBA ";
        body += "<div class=\"head\"><h2>" + std::string(folder_icon) + " " + escaped(shown->title) + "</h2><span class=\"tools\">";
        if (shown->children.size() > 1)
            body += "<a href=\"" + escaped(asking(view, "sort=" + std::to_string(shown->id))) + "\">Sort by name</a>";
        if (bookmarks_in(*shown) > 0)
            body += "<a href=\"" + escaped(asking(view, "openall=" + std::to_string(shown->id))) + "\">Open all in tabs</a>";
        body += "</span></div>\n";
        if (!path.empty())
            body += "<div class=\"crumbs\">" + path + escaped(shown->title) + "</div>\n";
        if (shown->children.empty()) {
            body += "<div class=\"empty\">" + std::string(star_icon)
                + "<p>Nothing is kept here yet.</p><p class=\"muted\">Ctrl+D, or the star in the toolbar, bookmarks the page in front.</p></div>\n";
        }
        for (BookmarkNode const& node : shown->children)
            body += item_row(bookmarks, node, view, nullptr);
        std::string const id = std::to_string(shown->id);
        body += "<div class=\"foot\"><form class=\"line\" action=\"about:bookmarks\"><input type=\"hidden\" name=\"newfolder\" value=\"" + id + "\">"
            + hidden_view(view) + "<input name=\"foldername\" placeholder=\"A new folder's name\"><button>New folder</button></form>"
            + "<form class=\"line\" action=\"about:bookmarks\"><input type=\"hidden\" name=\"add\" value=\"" + id + "\">" + hidden_view(view)
            + "<input name=\"title\" placeholder=\"Name\"><input class=\"wide\" name=\"url\" placeholder=\"Address\"><button>Add bookmark</button></form></div>\n";
    }
    body += "</div>\n</div>\n";

    // A <style> element, as internal_page asks for one: what is handed over
    // goes into the head as it is.
    static constexpr std::string_view style = R"(<style>
body { padding: 36px 48px; font-family: sans-serif }
.top { display: flex; flex-wrap: wrap; gap: 12px; align-items: center; justify-content: space-between; margin: 0 0 20px 0 }
.top h1 { margin: 0 }
.search { display: flex; gap: 8px }
.search input { width: 320px }
input, select { box-sizing: border-box; padding: 7px 10px; font-size: 15px; border: 1px solid #c9ccd3; border-radius: 6px; background-color: #ffffff; color: #1d1f24 }
button, a.button { display: inline-block; box-sizing: border-box; padding: 7px 14px; font-size: 14px; border: 1px solid #c9ccd3; border-radius: 6px; background-color: #ffffff; color: #1d1f24; text-decoration: none }
button:hover, a.button:hover { background-color: #f0f2f6 }
button.primary, a.button.primary { background-color: #1f5fbf; border-color: #1f5fbf; color: #ffffff }
a.button.danger { border-color: #c4463a; color: #a5342a }
.notice { background-color: #eef6ee; border: 1px solid #b7d8b7; border-radius: 8px; padding: 10px 16px; margin: 0 0 20px 0 }
.panes { display: flex; gap: 24px; align-items: flex-start }
.side { width: 290px; flex: none }
.main { flex: 1; min-width: 0 }
.card { background-color: #ffffff; border: 1px solid #d9dbe0; border-radius: 10px; margin: 0 0 20px 0; overflow: hidden }
.card.pad { padding: 16px }
h3 { font-size: 14px; margin: 0 0 10px 0; color: #1d1f24 }
.places { padding: 8px }
.place { display: flex; align-items: center; gap: 8px; padding: 8px 12px 8px 12px; border-radius: 6px; color: #1d1f24; text-decoration: none; font-size: 15px }
.place:hover { background-color: #f0f2f6 }
.place.here { background-color: #e6eefb; color: #174a99; font-weight: bold }
.place .label { flex: 1; min-width: 0 }
.place .count { color: #5d6470; font-size: 13px; font-weight: normal }
.rule { height: 1px; background-color: #ececf0; margin: 8px 4px 8px 4px }
.choices a, .choices b { display: block; padding: 7px 10px; margin: 0 0 4px 0; border: 1px solid #d9dbe0; border-radius: 6px; font-size: 14px; font-weight: normal; text-decoration: none; color: #1d1f24 }
.choices a:hover { background-color: #f0f2f6 }
.choices b { border-color: #1f5fbf; color: #174a99; background-color: #e6eefb }
.hint { font-size: 13px; color: #5d6470; margin: 10px 0 0 0 }
.head { display: flex; align-items: center; justify-content: space-between; padding: 16px 20px 14px 20px; border-bottom: 1px solid #ececf0 }
.head h2 { flex: 1; min-width: 0; margin: 0; font-size: 19px; white-space: nowrap; overflow: hidden }
.tools { flex: none; white-space: nowrap }
.head .ico { margin: 0 4px 0 0 }
.tools a, .tools .muted { margin: 0 0 0 18px; font-size: 14px }
.crumbs { padding: 10px 20px 0 20px; font-size: 13px; color: #5d6470 }
.ico { flex: none; vertical-align: middle }
.item { display: flex; align-items: center; gap: 12px; padding: 10px 20px 10px 20px; border-bottom: 1px solid #f0f1f4 }
.item:hover { background-color: #f7f8fb }
.item.asked { background-color: #fdf3f2 }
.what { flex: 1; min-width: 0 }
.name { display: block; font-size: 15px; color: #1d1f24; text-decoration: none; white-space: nowrap; overflow: hidden }
a.name:hover { color: #1f5fbf; text-decoration: underline }
.sub { font-size: 13px; color: #5d6470; white-space: nowrap; overflow: hidden }
.sub.path { color: #8a919e }
.do { flex: none }
.do a { margin: 0 0 0 14px; font-size: 13px; color: #5d6470; text-decoration: none }
.do a:hover { color: #1f5fbf; text-decoration: underline }
.do a.danger:hover { color: #a5342a }
.do a.button { color: #1d1f24; font-size: 14px }
.do a.button.danger { color: #a5342a }
.sure { font-size: 13px; color: #a5342a }
.item.editing { display: block; background-color: #f7f8fb }
.fields { display: flex; gap: 12px }
.fields label { flex: 1; min-width: 0; font-size: 13px; color: #5d6470 }
.fields label.long { flex: 2 }
.fields input, .fields select { display: block; width: 100%; margin: 4px 0 0 0 }
.buttons { margin: 12px 0 0 0 }
.buttons a.button { margin: 0 0 0 8px }
.empty { padding: 44px 20px 40px 20px; text-align: center; color: #1d1f24 }
.empty p { margin: 10px 0 0 0 }
.foot { padding: 16px 20px 16px 20px; background-color: #fafbfc }
.line { display: flex; flex-wrap: wrap; gap: 8px; margin: 0 0 10px 0 }
.line input { width: 220px }
.line input.wide { flex: 1 1 180px; min-width: 180px; width: auto }
.section { padding: 18px 20px 6px 20px; border-bottom: 1px solid #ececf0 }
.section .item { padding: 10px 0 10px 0 }
.section .line { margin: 12px 0 12px 0 }
.explain { font-size: 14px; color: #5d6470; margin: 0 0 8px 0 }
.none { font-size: 14px; color: #5d6470; margin: 0 0 12px 0 }
@media (max-width: 960px) {
body { padding: 24px 20px }
.panes { gap: 16px }
.side { width: 230px }
.search input { width: 240px }
}
</style>)";
    return internal_page("Bookmarks", body, style);
}

}
