#pragma once

// about:themes — the reader's themes and the two stores. The themes they
// have, to put on; a gallery of Firefox's themes, read from the add-ons
// site's public search API (names, previews, how many use each, and the
// address of each theme's .xpi), so that the store need not render in this
// engine for its themes to be had; and a box for a Chrome Web Store theme's
// address, since that store publishes no way to list it — a theme's .crx is
// fetched by its id. "Put on" is a plain link to the theme's file: it
// arrives as a download, and a download that is a browser theme is converted
// and put on (Browser::set_user_themes_directory).
//
// Nothing here fetches anything: the shell does, through its loader, when
// the reader opens the page — never before.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::ui {

struct GalleryTheme {
    std::string name;
    std::string author;
    std::string preview_url; // a picture of it; may be empty
    std::string file_url; // its .xpi
    long users = 0;
};

// What the page was asked for: about:themes?q=<words>&sort=<users|rating|created>&page=<n>,
// or ?use=<n> (put on the reader's n-th theme), or ?crx=<a Chrome Web Store
// address or id>.
struct GalleryQuery {
    std::string words;
    std::string sort = "users";
    int page = 1;
    std::optional<std::size_t> use;
    std::optional<std::string> crx;
};
GalleryQuery gallery_query_of(std::optional<std::string> const& url_query);
// The page's address for the same words, order and page — and nothing to
// do on the way: where the page is drawn again once that has been done.
std::string gallery_address(GalleryQuery const& query);

// The add-ons site's search for themes, as a URL under `api` (its search
// endpoint, ending in a slash).
std::string amo_search_url(std::string_view api, GalleryQuery const& query);

// What that search answered: its themes, and how many pages there are.
// Relative addresses in it are taken against `source`. Nothing for an answer
// that is not the API's.
struct GalleryAnswer {
    std::vector<GalleryTheme> themes;
    int pages = 1;
};
std::optional<GalleryAnswer> parse_amo_search(std::string_view json, std::string_view source);

// A Chrome Web Store item's id — thirty-two letters a to p — out of its
// address or given alone; and where that item's .crx is fetched from.
std::optional<std::string> chrome_theme_id(std::string_view text);
std::string chrome_crx_url(std::string_view id);

struct ThemesPage {
    struct Own {
        std::string name;
        bool current = false;
    };
    std::vector<Own> own; // the themes on offer, in the menu's order
    GalleryQuery query;
    std::optional<GalleryAnswer> gallery;
    std::string gallery_error; // why there is none, when there is none
    std::string notice; // something just done, said at the top
    bool can_adopt = true; // whether a downloaded theme has somewhere to go
};
std::string themes_page(ThemesPage const& page);

}
