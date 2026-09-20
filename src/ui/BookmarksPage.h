#pragma once

// about:bookmarks: the reader's bookmarks as a page of the browser's own —
// every folder and bookmark in order, each with what can be done to it, the
// form one is edited in, when the bar is shown, and the way in and out for
// the bookmarks file other browsers read and write. The page acts through
// its own address: a link or a form of it names what is asked, the shell
// applies that to the tree and draws the page again. Pure: nothing here
// touches a file or the network.

#include "ui/Bookmarks.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sashfold::ui {

// What an address of the page asks for, read from its query.
struct BookmarksRequest {
    std::optional<std::uint64_t> remove;
    std::optional<std::uint64_t> up; // one place earlier in its folder
    std::optional<std::uint64_t> down;
    std::optional<std::uint64_t> edit; // show the form for this one
    std::optional<std::uint64_t> save; // the form, sent: title, url and folder below
    std::optional<std::string> title;
    std::optional<std::string> url;
    std::optional<std::uint64_t> folder;
    std::optional<std::uint64_t> new_folder_in; // with `title`
    std::optional<std::string> bar; // "always", "new-tab" or "never"
    std::optional<std::string> import_path; // a bookmarks file on this machine
    bool export_file = false;

    // Whether it changes anything, or asks for a file: such an address is
    // not one to come back to, and the entry keeps the plain one.
    bool acts() const
    {
        return remove || up || down || save || new_folder_in || bar || import_path || export_file;
    }
};

BookmarksRequest bookmarks_request_of(std::optional<std::string> const& url_query);

// Applies what a request asks of the tree itself — everything but the
// files, which are the shell's to read and write — and says what was done,
// in words for the top of the page; empty when nothing was asked.
std::string apply_bookmarks_request(BookmarksRequest const& request, Bookmarks& bookmarks);

// The page. `editing` shows the form for one bookmark or folder.
std::string bookmarks_page(Bookmarks const& bookmarks, std::string_view notice, std::optional<std::uint64_t> editing);

}
