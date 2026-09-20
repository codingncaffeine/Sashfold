#include "Test.h"

#include "ui/Bookmarks.h"

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

    return test::report("bookmarks");
}
