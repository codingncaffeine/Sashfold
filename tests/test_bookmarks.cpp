#include "Test.h"

#include "ui/Bookmarks.h"
#include "ui/BookmarksPage.h"

#include <optional>
#include <string>
#include <vector>

// The bookmarks tree: its two roots, adding, moving and removing, the file it
// is kept in, and the Netscape bookmarks file it trades with other browsers.

using namespace sashfold;
using namespace sashfold::ui;

namespace {

std::string titles_of(BookmarkNode const& folder)
{
    std::string out;
    for (BookmarkNode const& child : folder.children) {
        if (!out.empty())
            out += " ";
        out += child.folder ? "[" + child.title + ": " + titles_of(child) + "]" : child.title;
    }
    return out;
}

}

int main()
{
    // Two roots, nothing in them, and nothing to save yet.
    {
        Bookmarks b;
        CHECK_EQ(b.count(), std::size_t { 0 });
        CHECK(b.bar().folder && b.other().folder);
        CHECK_EQ(b.changes(), std::uint64_t { 0 });
        CHECK(b.bar_mode() == BookmarksBarMode::Always);
        CHECK(b.find(Bookmarks::bar_id) == &b.bar());
        CHECK(b.parent_of(Bookmarks::bar_id) == nullptr);
    }

    // Adding, finding, and places among a folder's children.
    {
        Bookmarks b;
        std::uint64_t const a = b.add_bookmark(Bookmarks::bar_id, "A", "https://a.example/", 100);
        std::uint64_t const c = b.add_bookmark(Bookmarks::bar_id, "C", "https://c.example/", 300);
        std::uint64_t const bb = b.add_bookmark(Bookmarks::bar_id, "B", "https://b.example/", 200, 1);
        CHECK(a != 0 && bb != 0 && c != 0 && a != bb && bb != c);
        CHECK_EQ(titles_of(b.bar()), std::string("A B C"));
        CHECK_EQ(b.count(), std::size_t { 3 });
        CHECK_EQ(b.changes(), std::uint64_t { 3 });
        CHECK(b.find_url("https://b.example/") == b.find(bb));
        CHECK(b.find_url("https://nowhere.example/") == nullptr);
        CHECK(b.parent_of(bb) == &b.bar());
        // Not into what is not a folder, not without an address.
        CHECK_EQ(b.add_bookmark(a, "X", "https://x.example/", 0), std::uint64_t { 0 });
        CHECK_EQ(b.add_bookmark(Bookmarks::bar_id, "X", "", 0), std::uint64_t { 0 });
        CHECK_EQ(b.add_bookmark(9999, "X", "https://x.example/", 0), std::uint64_t { 0 });

        // Moving within a folder: `at` counts the children as they stand.
        CHECK(b.move(a, Bookmarks::bar_id, 2)); // A from 0 to before C
        CHECK_EQ(titles_of(b.bar()), std::string("B A C"));
        CHECK(b.move(c, Bookmarks::bar_id, 0));
        CHECK_EQ(titles_of(b.bar()), std::string("C B A"));
        CHECK(b.move(c, Bookmarks::bar_id, 3)); // to the end
        CHECK_EQ(titles_of(b.bar()), std::string("B A C"));

        // Into a folder, and a folder never into itself or below itself.
        std::uint64_t const folder = b.add_folder(Bookmarks::bar_id, "News");
        std::uint64_t const inner = b.add_folder(folder, "Inner");
        CHECK(b.move(a, folder, 0));
        CHECK(b.move(bb, inner, 0));
        CHECK_EQ(titles_of(b.bar()), std::string("C [News: A [Inner: B]]"));
        CHECK(!b.move(folder, folder, 0));
        CHECK(!b.move(folder, inner, 0));
        CHECK(!b.move(Bookmarks::bar_id, Bookmarks::other_id, 0)); // a root stays where it is
        CHECK(b.move(folder, Bookmarks::other_id, 0));
        CHECK_EQ(titles_of(b.bar()), std::string("C"));
        CHECK_EQ(titles_of(b.other()), std::string("[News: A [Inner: B]]"));

        // Renaming, a new address, and removing: a folder goes with what is in it.
        CHECK(b.rename(c, "Sea"));
        CHECK(b.set_url(c, "https://sea.example/"));
        CHECK(!b.set_url(folder, "https://x.example/"));
        CHECK(!b.rename(Bookmarks::bar_id, "Mine"));
        CHECK(b.find_url("https://sea.example/") != nullptr);
        CHECK(b.remove(folder));
        CHECK_EQ(b.count(), std::size_t { 1 });
        CHECK(!b.remove(Bookmarks::other_id));
        CHECK(!b.remove(424242));
    }

    // The file it is kept in: there and back the same, the bar's setting with it.
    {
        Bookmarks b;
        b.add_bookmark(Bookmarks::bar_id, "Quote \"q\" \\ tab\t", "https://a.example/?x=1&y=2", 1700000000);
        std::uint64_t const folder = b.add_folder(Bookmarks::bar_id, "Folder");
        std::uint64_t const deep = b.add_bookmark(folder, "Deep", "https://deep.example/", 5);
        b.set_icon(deep, std::string("\x89PNG\r\n\x1a\n\0\0icon", 14));
        b.add_bookmark(Bookmarks::other_id, "Elsewhere", "https://else.example/", 0);
        b.set_bar_mode(BookmarksBarMode::NewTab);
        std::string const text = b.to_json();
        std::optional<Bookmarks> const back = Bookmarks::from_json(text);
        CHECK(back.has_value());
        if (back) {
            CHECK_EQ(titles_of(back->bar()), std::string("Quote \"q\" \\ tab\t [Folder: Deep]"));
            CHECK_EQ(titles_of(back->other()), std::string("Elsewhere"));
            CHECK(back->bar_mode() == BookmarksBarMode::NewTab);
            CHECK_EQ(back->changes(), std::uint64_t { 0 }); // as read: nothing to save
            BookmarkNode const* const found = back->find_url("https://deep.example/");
            CHECK(found != nullptr);
            if (found) {
                CHECK_EQ(found->added, std::int64_t { 5 });
                CHECK_EQ(found->icon, std::string("\x89PNG\r\n\x1a\n\0\0icon", 14));
            }
            CHECK_EQ(back->find_url("https://a.example/?x=1&y=2")->added, std::int64_t { 1700000000 });
            CHECK_EQ(back->to_json(), text); // and written again, the same text
        }
        CHECK(!Bookmarks::from_json("not json").has_value());
        CHECK(!Bookmarks::from_json("[1, 2]").has_value());
        CHECK(!Bookmarks::from_json("{\"bar\": 3}").has_value());
        // What is not a bookmark is passed over; the rest is kept.
        std::optional<Bookmarks> const partial = Bookmarks::from_json(
            "{\"bar\": [ 7, {\"title\": \"no address\"}, {\"title\": \"Kept\", \"url\": \"https://k.example/\", \"more\": true} ]}");
        CHECK(partial.has_value());
        if (partial)
            CHECK_EQ(titles_of(partial->bar()), std::string("Kept"));
    }

    // The Netscape file: written, and read into an empty tree the same.
    {
        Bookmarks b;
        b.add_bookmark(Bookmarks::bar_id, "A & B <c> \"q\"", "https://a.example/?x=1&y=2", 111);
        std::uint64_t const folder = b.add_folder(Bookmarks::bar_id, "Folder");
        std::uint64_t const deep = b.add_bookmark(folder, "Deep", "https://deep.example/", 222);
        b.set_icon(deep, std::string("\x89PNG-icon", 9));
        std::uint64_t const other_folder = b.add_folder(Bookmarks::other_id, "Reading");
        b.add_bookmark(other_folder, "Later", "https://later.example/", 333);
        b.add_bookmark(Bookmarks::other_id, "Loose", "https://loose.example/", 444);
        std::string const html = b.to_netscape_html();
        CHECK(html.starts_with("<!DOCTYPE NETSCAPE-Bookmark-file-1>"));
        CHECK(html.find("PERSONAL_TOOLBAR_FOLDER=\"true\"") != std::string::npos);
        CHECK(html.find("A &amp; B &lt;c&gt; &quot;q&quot;") != std::string::npos);

        Bookmarks fresh;
        CHECK_EQ(fresh.import_netscape_html(html), std::size_t { 4 });
        CHECK_EQ(titles_of(fresh.bar()), std::string("A & B <c> \"q\" [Folder: Deep]"));
        CHECK_EQ(titles_of(fresh.other()), std::string("[Reading: Later] Loose"));
        BookmarkNode const* const found = fresh.find_url("https://deep.example/");
        CHECK(found != nullptr);
        if (found) {
            CHECK_EQ(found->added, std::int64_t { 222 });
            CHECK_EQ(found->icon, std::string("\x89PNG-icon", 9));
        }
        CHECK(fresh.find_url("https://a.example/?x=1&y=2") != nullptr);
        // Importing adds: a second time doubles, and replaces nothing.
        CHECK_EQ(fresh.import_netscape_html(html), std::size_t { 4 });
        CHECK_EQ(fresh.count(), std::size_t { 8 });
    }

    // A file as another browser writes it: lower case, a comment, an unquoted
    // attribute, a query that is no bookmark, numeric references.
    {
        std::string const html = "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<!-- a <DL> in a comment -->\n<title>Bookmarks</title>\n<h1>Bookmarks Menu</h1>\n"
                                 "<dl><p>\n"
                                 "<dt><a href=\"place:sort=8&maxResults=10\">Most Visited</a>\n"
                                 "<dt><h3 add_date=\"5\" personal_toolbar_folder=\"true\">Bookmarks Toolbar</h3>\n<dl><p>\n"
                                 "<dt><a href=https://plain.example/ add_date=7>Caf&#233; &#x263A;</a>\n"
                                 "</dl><p>\n"
                                 "<dt><h3>Menu folder</h3>\n<dl><p>\n<dt><a href=\"https://m.example/\">M</a>\n</dl><p>\n"
                                 "</dl>\n";
        Bookmarks b;
        CHECK_EQ(b.import_netscape_html(html), std::size_t { 2 });
        CHECK_EQ(titles_of(b.bar()), std::string("Caf\xC3\xA9 \xE2\x98\xBA"));
        CHECK_EQ(titles_of(b.other()), std::string("[Menu folder: M]"));
        CHECK_EQ(b.find_url("https://plain.example/")->added, std::int64_t { 7 });
        // An address may hold a > inside its quotes: the tag does not end there.
        Bookmarks quoted;
        CHECK_EQ(quoted.import_netscape_html("<DL><p><DT><A HREF=\"data:text/html,<title>T</title>x\" ADD_DATE=\"9\">Quoted</A></DL>"), std::size_t { 1 });
        CHECK(quoted.find_url("data:text/html,<title>T</title>x") != nullptr);
        CHECK_EQ(titles_of(quoted.other()), std::string("Quoted"));
        // Nothing that is not the format: nothing comes, nothing breaks.
        Bookmarks none;
        CHECK_EQ(none.import_netscape_html("<html><body><p>hello</body></html>"), std::size_t { 0 });
        CHECK_EQ(none.import_netscape_html("<a href=\"https://unclosed"), std::size_t { 0 });
        CHECK_EQ(none.count(), std::size_t { 0 });
    }

    // Sorted by name: folders first, letters' case aside, what is inside a
    // folder left as it is; a bookmark with no name goes by its address.
    {
        Bookmarks b;
        b.add_bookmark(Bookmarks::bar_id, "zebra", "https://z.example/", 0);
        std::uint64_t const folder = b.add_folder(Bookmarks::bar_id, "Mid");
        b.add_bookmark(folder, "second", "https://2.example/", 0);
        b.add_bookmark(folder, "first", "https://1.example/", 0);
        b.add_bookmark(Bookmarks::bar_id, "Apple", "https://a.example/", 0);
        b.add_bookmark(Bookmarks::bar_id, "", "https://b.example/", 0);
        std::uint64_t const before = b.changes();
        CHECK(b.sort_by_name(Bookmarks::bar_id));
        CHECK_EQ(titles_of(b.bar()), std::string("[Mid: second first] Apple  zebra"));
        CHECK(b.changes() != before);
        CHECK(!b.sort_by_name(9999));
        CHECK(!b.sort_by_name(b.bar().children.back().id)); // a bookmark is no folder
    }

    // What was removed last comes back where it stood, once, with what was
    // in it; into the other bookmarks when its folder has gone since.
    {
        Bookmarks b;
        std::uint64_t const one = b.add_bookmark(Bookmarks::bar_id, "One", "https://1.example/", 0);
        std::uint64_t const folder = b.add_folder(Bookmarks::bar_id, "Folder");
        b.add_bookmark(folder, "Inside", "https://in.example/", 0);
        b.add_bookmark(Bookmarks::bar_id, "Three", "https://3.example/", 0);
        CHECK(!b.can_undo_remove());
        CHECK(!b.undo_remove());
        CHECK(b.remove(folder));
        CHECK(b.can_undo_remove());
        CHECK_EQ(b.removed_name(), std::string("Folder"));
        CHECK_EQ(titles_of(b.bar()), std::string("One Three"));
        CHECK(b.undo_remove());
        CHECK_EQ(titles_of(b.bar()), std::string("One [Folder: Inside] Three"));
        CHECK(!b.can_undo_remove());
        CHECK(!b.undo_remove());
        // Its id is the one it had: an address that names it still does.
        CHECK(b.find(folder) != nullptr && b.find(folder)->folder);
        // Only the last removal is kept.
        CHECK(b.remove(one));
        std::uint64_t const inside = b.find(folder)->children.front().id;
        CHECK(b.remove(inside));
        CHECK_EQ(b.removed_name(), std::string("Inside"));
        // Its folder goes too: what comes back goes to the other bookmarks.
        CHECK(b.remove(folder));
        CHECK_EQ(b.removed_name(), std::string("Folder"));
        Bookmarks c;
        std::uint64_t const home = c.add_folder(Bookmarks::bar_id, "Home");
        std::uint64_t const child = c.add_bookmark(home, "Child", "https://c.example/", 0);
        CHECK(c.remove(child));
        // A removal of the folder would take the place of the child's; take
        // the folder away by moving on without one: a copy restored.
        Bookmarks d = c;
        CHECK(d.undo_remove());
        CHECK_EQ(titles_of(d.bar()), std::string("[Home: Child]"));
    }

    // A search: every bookmark whose name or address holds the words,
    // whatever the letters' case, with the folders it is under.
    {
        Bookmarks b;
        std::uint64_t const outer = b.add_folder(Bookmarks::bar_id, "Outer");
        std::uint64_t const inner = b.add_folder(outer, "Inner");
        std::uint64_t const deep = b.add_bookmark(inner, "Deep One", "https://deep.example/", 0);
        b.add_bookmark(Bookmarks::other_id, "Elsewhere", "https://DEEP.example/else", 0);
        b.add_bookmark(Bookmarks::other_id, "Unrelated", "https://un.example/", 0);
        std::vector<Bookmarks::Found> const found = b.search("deep");
        CHECK_EQ(found.size(), std::size_t { 2 });
        if (found.size() == 2) {
            CHECK_EQ(found[0].node->title, std::string("Deep One"));
            CHECK_EQ(found[0].path.size(), std::size_t { 3 });
            if (found[0].path.size() == 3) {
                CHECK(found[0].path[0] == &b.bar());
                CHECK_EQ(found[0].path[1]->title, std::string("Outer"));
                CHECK_EQ(found[0].path[2]->title, std::string("Inner"));
            }
            CHECK_EQ(found[1].node->title, std::string("Elsewhere"));
        }
        CHECK(b.search("nothing like it").empty());
        CHECK(b.search("").empty());
        // A folder's name is not a bookmark's.
        CHECK(b.search("Outer").empty());
        std::vector<BookmarkNode const*> const path = b.path_of(deep);
        CHECK_EQ(path.size(), std::size_t { 3 });
        CHECK(b.path_of(Bookmarks::bar_id).empty());
        CHECK(b.path_of(9999).empty());
    }

    // A dated copy put in the tree's place: one more change to it, and when
    // the bar is shown stays the reader's setting.
    {
        Bookmarks now;
        now.add_bookmark(Bookmarks::bar_id, "Now", "https://now.example/", 0);
        now.set_bar_mode(BookmarksBarMode::Never);
        std::uint64_t const removed = now.add_bookmark(Bookmarks::bar_id, "Gone", "https://gone.example/", 0);
        now.remove(removed);
        Bookmarks then;
        then.add_bookmark(Bookmarks::other_id, "Then", "https://then.example/", 0);
        std::uint64_t const before = now.changes();
        now.replace_with(then);
        CHECK_EQ(titles_of(now.bar()), std::string());
        CHECK_EQ(titles_of(now.other()), std::string("Then"));
        CHECK(now.bar_mode() == BookmarksBarMode::Never);
        CHECK_EQ(now.changes(), before + 1);
        // What was removed from the tree that went is not put into this one.
        CHECK(!now.can_undo_remove());
    }

    // The page's address: what it looks at, what it asks to be done, and the
    // address of the view alone, which says nothing of what was asked.
    {
        BookmarksRequest const removal = bookmarks_request_of(std::optional<std::string>("remove=9&in=7"));
        CHECK(removal.in == std::optional<std::uint64_t>(7));
        CHECK(removal.remove == std::optional<std::uint64_t>(9));
        CHECK(removal.acts());
        CHECK_EQ(bookmarks_view_address(removal), std::string("about:bookmarks?in=7"));
        BookmarksRequest const search = bookmarks_request_of(std::optional<std::string>("q=two+words"));
        CHECK(search.search == std::optional<std::string>("two words"));
        CHECK(!search.acts());
        CHECK_EQ(bookmarks_view_address(search), std::string("about:bookmarks?q=two%20words"));
        // Looking — a folder, a search, the form open on a bookmark — does nothing.
        CHECK(!bookmarks_request_of(std::optional<std::string>("in=3&edit=4")).acts());
        CHECK(!bookmarks_request_of(std::optional<std::string>("view=transfer")).acts());
        CHECK(!bookmarks_request_of(std::nullopt).acts());
        // Everything else is an act.
        for (char const* query : { "undo=1", "up=3", "down=3", "sort=1", "save=3&title=x", "newfolder=1&foldername=x", "add=1&url=x",
                 "openall=1", "bar=never", "importfrom=/x", "import=/x", "restore=bookmarks-2026-01-01.json", "export=1" })
            CHECK(bookmarks_request_of(std::optional<std::string>(query)).acts());
        // A restore not yet confirmed shows the view it is asked in.
        BookmarksRequest const restore = bookmarks_request_of(std::optional<std::string>("restore=bookmarks-2026-01-01.json"));
        CHECK(restore.transfer && !restore.sure);
        CHECK_EQ(bookmarks_view_address(restore), std::string("about:bookmarks?view=transfer"));
        CHECK(bookmarks_request_of(std::optional<std::string>("restore=x&sure=1")).sure);
    }

    // The page itself: its style is an element and none of its rules are
    // words; a stranger's title is words, whatever it is made of; the folder
    // asked for is the one listed, and a search says where what it found is.
    {
        Bookmarks b;
        std::uint64_t const folder = b.add_folder(Bookmarks::bar_id, "Reading <b>list</b>");
        b.add_bookmark(folder, "Inside & out", "https://inside.example/?a=1&b=2", 0);
        b.add_bookmark(Bookmarks::other_id, "Elsewhere", "https://else.example/", 0);
        BookmarksRequest view;
        std::string const bar = bookmarks_page(b, view, {}, {});
        CHECK(bar.find("<style>") != std::string::npos);
        std::size_t const body = bar.find("<body>");
        CHECK(body != std::string::npos);
        CHECK(bar.find("border-radius", body == std::string::npos ? 0 : body) == std::string::npos);
        CHECK(bar.find("Reading &lt;b&gt;list&lt;/b&gt;") != std::string::npos);
        CHECK(bar.find("<b>list</b>") == std::string::npos);
        CHECK(bar.find("Inside &amp; out") == std::string::npos); // it is in the folder, not on the bar
        view.in = folder;
        std::string const inside = bookmarks_page(b, view, {}, {});
        CHECK(inside.find("Inside &amp; out") != std::string::npos);
        CHECK(inside.find("https://inside.example/?a=1&amp;b=2") != std::string::npos);
        // What is asked of a row keeps the folder that is shown.
        CHECK(inside.find("&amp;in=" + std::to_string(folder)) != std::string::npos);
        BookmarksRequest looking;
        looking.search = "ELSE";
        std::string const found = bookmarks_page(b, looking, {}, {});
        CHECK(found.find("1 bookmark found") != std::string::npos);
        CHECK(found.find("Other bookmarks") != std::string::npos);
        // A notice, with the link under it when there is one.
        BookmarksNotice notice;
        notice.words = "Removed: <one>";
        notice.link_words = "Undo";
        notice.link_address = "about:bookmarks?undo=1";
        std::string const told = bookmarks_page(b, {}, notice, {});
        CHECK(told.find("Removed: &lt;one&gt;") != std::string::npos);
        CHECK(told.find("href=\"about:bookmarks?undo=1\"") != std::string::npos);
        // The view bookmarks come in by lists what it is handed, and only there.
        BookmarksSurroundings around;
        around.sources.push_back({ "/home/x/.config/chromium/Default/Bookmarks", "Chromium", "Default", "", 12 });
        around.backups.push_back({ "bookmarks-2026-01-01.json", "2026-01-01", 3 });
        BookmarksRequest transfer;
        transfer.transfer = true;
        std::string const moving = bookmarks_page(b, transfer, {}, around);
        CHECK(moving.find("Chromium") != std::string::npos);
        CHECK(moving.find("12 bookmarks") != std::string::npos);
        CHECK(moving.find("importfrom=%2Fhome%2Fx%2F.config%2Fchromium%2FDefault%2FBookmarks") != std::string::npos);
        CHECK(moving.find("restore=bookmarks-2026-01-01.json") != std::string::npos);
        CHECK(moving.find("sure=1") == std::string::npos);
        transfer.restore = "bookmarks-2026-01-01.json";
        std::string const asking = bookmarks_page(b, transfer, {}, around);
        CHECK(asking.find("sure=1") != std::string::npos);
        CHECK(bookmarks_page(b, {}, {}, around).find("Chromium") == std::string::npos);
    }

    return test::report("bookmarks");
}
