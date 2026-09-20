#pragma once

// The reader's bookmarks: a tree of folders and bookmarks under two roots, as
// Firefox and Chrome keep them — the bar's, shown under the toolbar, and the
// rest. It is kept in the profile as bookmarks.json, and goes to and comes
// from other browsers as the Netscape bookmarks file every one of them reads
// and writes. Pure data: nothing here draws or fetches.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::ui {

struct BookmarkNode {
    std::uint64_t id = 0;
    bool folder = false;
    std::string title;
    std::string url; // a bookmark's address; empty for a folder
    std::int64_t added = 0; // seconds since the epoch; 0 when not known
    std::string icon; // the page's icon as PNG bytes; empty for none
    std::vector<BookmarkNode> children; // a folder's, in the reader's order
};

// When the bar is shown. An empty bar is not shown whatever this says:
// there is nothing on it.
enum class BookmarksBarMode : std::uint8_t {
    Always,
    NewTab, // on the new-tab page alone
    Never,
};

class Bookmarks {
public:
    static constexpr std::uint64_t bar_id = 1;
    static constexpr std::uint64_t other_id = 2;

    Bookmarks();

    BookmarkNode const& bar() const { return m_bar; }
    BookmarkNode const& other() const { return m_other; }
    BookmarkNode const* find(std::uint64_t id) const;
    // The first bookmark of an address, in tree order, the bar's first.
    BookmarkNode const* find_url(std::string_view url) const;
    // The folder a node is in; null for a root and for an id that is not there.
    BookmarkNode const* parent_of(std::uint64_t id) const;
    std::size_t count() const; // bookmarks, folders not counted

    // Into a folder, at a place among its children or at its end. 0 when the
    // folder is not one.
    std::uint64_t add_bookmark(std::uint64_t folder, std::string title, std::string url, std::int64_t added,
        std::optional<std::size_t> at = std::nullopt);
    std::uint64_t add_folder(std::uint64_t folder, std::string title, std::optional<std::size_t> at = std::nullopt);
    // A root stays; a folder goes with what is in it.
    bool remove(std::uint64_t id);
    bool rename(std::uint64_t id, std::string title);
    bool set_url(std::uint64_t id, std::string url);
    bool set_icon(std::uint64_t id, std::string png);
    // To a place in a folder: `at` counts the folder's children as they are
    // before the move. Not a root, and not a folder into itself or below.
    bool move(std::uint64_t id, std::uint64_t to_folder, std::size_t at);

    BookmarksBarMode bar_mode() const { return m_bar_mode; }
    void set_bar_mode(BookmarksBarMode mode);

    // Moves with every change: what the window's loop saves by.
    std::uint64_t changes() const { return m_changes; }

    std::string to_json() const;
    // nullopt when the text is not a bookmarks file; ids are given afresh.
    static std::optional<Bookmarks> from_json(std::string_view text);

    // The Netscape bookmarks file: <DT><A HREF ADD_DATE ICON> and <DT><H3>
    // folders in nested <DL>s, the bar's folder marked
    // PERSONAL_TOOLBAR_FOLDER.
    std::string to_netscape_html() const;
    // What a file holds is added — nothing here is replaced: the folder it
    // marks as its toolbar into the bar, the rest into the other bookmarks.
    // The number of bookmarks that came.
    std::size_t import_netscape_html(std::string_view html);

private:
    BookmarkNode* find_mutable(std::uint64_t id);
    BookmarkNode* parent_of_mutable(std::uint64_t id);
    void touch() { ++m_changes; }

    BookmarkNode m_bar;
    BookmarkNode m_other;
    BookmarksBarMode m_bar_mode = BookmarksBarMode::Always;
    std::uint64_t m_next_id = 3;
    std::uint64_t m_changes = 0;
};

}
