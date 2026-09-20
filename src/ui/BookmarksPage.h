#pragma once

// about:bookmarks: the place the reader's bookmarks are managed, laid out the
// way a bookmarks manager is in Firefox and Chrome — the folders down the
// left, the folder chosen on the right as a list of what is in it, each with
// its icon, its name, its address and what can be done to it; a search over
// all of them; the form one is edited in, in its place in the list; and a
// view of its own for bringing bookmarks in — from another browser on this
// machine, from a bookmarks file — writing them out, and the dated copies
// kept of them. The page acts through its own address: a link or a form of
// it names what is asked, the shell applies that and draws the page again.
// Pure: nothing here touches a file or the network; what the shell found on
// the machine is handed in.

#include "ui/Bookmarks.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::ui {

// What an address of the page asks for, read from its query: what is looked
// at, and what is to be done.
struct BookmarksRequest {
    // The view: the folder shown (the bar's, unsaid), what a search found,
    // or the view bookmarks come in and go out by; and the bookmark or
    // folder whose form is open in the list.
    std::optional<std::uint64_t> in;
    std::optional<std::string> search;
    bool transfer = false;
    std::optional<std::uint64_t> edit;

    std::optional<std::uint64_t> remove;
    bool undo = false;
    std::optional<std::uint64_t> up; // one place earlier in its folder
    std::optional<std::uint64_t> down;
    std::optional<std::uint64_t> sort; // a folder, by name
    std::optional<std::uint64_t> save; // the form, sent: title, url and folder below
    std::optional<std::string> title;
    std::optional<std::string> url;
    std::optional<std::uint64_t> folder;
    std::optional<std::uint64_t> new_folder_in; // with `title`
    std::optional<std::uint64_t> add_in; // a bookmark written by hand: `title` and `url`
    std::optional<std::uint64_t> open_all; // a folder's bookmarks, each in a tab
    std::optional<std::string> bar; // "always", "new-tab" or "never"
    std::optional<std::string> import_from; // a source the shell found, by its key
    std::optional<std::string> import_path; // a bookmarks file, by the path typed
    std::optional<std::string> restore; // a dated copy, by its key
    bool sure = false; // a restore, confirmed
    bool export_file = false;

    // Whether it changes anything, opens anything or asks for a file: such
    // an address is not one to come back to, and the entry keeps the view's.
    bool acts() const
    {
        return remove || undo || up || down || sort || save || new_folder_in || add_in || open_all || bar || import_from
            || import_path || restore || export_file;
    }
};

BookmarksRequest bookmarks_request_of(std::optional<std::string> const& url_query);

// The address of what a request looks at, with nothing it asks to be done.
std::string bookmarks_view_address(BookmarksRequest const& request);

// Applies what a request asks of the tree itself — everything but files and
// tabs, which are the shell's — and says what was done, in words for the top
// of the page; empty when nothing of the tree's was asked.
std::string apply_bookmarks_request(BookmarksRequest const& request, Bookmarks& bookmarks);

// A place bookmarks can be brought in from, found on this machine: another
// browser's own store, or a bookmarks file lying in one of the usual folders.
struct BookmarkSource {
    std::string key; // what the page's link names it by: the file that is read
    std::string browser; // "Firefox", "Chrome", … ; empty for a bookmarks file
    std::string detail; // the profile's name, or the file's name and folder
    std::string when; // the file's date
    std::size_t count = 0; // the bookmarks it holds
};

// A dated copy of the reader's own bookmarks, kept in the profile.
struct BookmarkBackup {
    std::string key; // its file's name
    std::string when;
    std::size_t count = 0;
};

// What the shell found, for the view bookmarks come in and go out by.
struct BookmarksSurroundings {
    std::vector<BookmarkSource> sources;
    std::vector<BookmarkBackup> backups;
};

// The page, for what the request looks at. A notice may end in a link the
// shell wants under it — the way back from a removal — given as its words
// and its address.
struct BookmarksNotice {
    std::string words;
    std::string link_words;
    std::string link_address;
};

std::string bookmarks_page(Bookmarks const& bookmarks, BookmarksRequest const& view, BookmarksNotice const& notice,
    BookmarksSurroundings const& surroundings);

}
