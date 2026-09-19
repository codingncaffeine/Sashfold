// about:themes, the parts of it that fetch nothing: what its address asks
// for, the catalogue's search address, the catalogue's answer read, a Chrome
// Web Store id found, and the page written — every name a stranger chose
// escaped on its way into it.

#include "ui/ThemeGallery.h"
#include "Test.h"

#include <fstream>
#include <iterator>
#include <string>

using namespace sashfold;
using namespace sashfold::ui;

int main(int argc, char** argv)
{
    std::string const fixtures = argc > 1 ? argv[1] : "tests/fixtures/themes";

    // --- What the page's address asks for -----------------------------------------
    {
        GalleryQuery const plain = gallery_query_of(std::nullopt);
        CHECK_EQ(plain.words, std::string());
        CHECK_EQ(plain.sort, std::string("users"));
        CHECK_EQ(plain.page, 1);
        CHECK(!plain.use && !plain.crx);
        GalleryQuery const asked = gallery_query_of(std::string("q=space+cats%21&sort=rating&page=3"));
        CHECK_EQ(asked.words, std::string("space cats!"));
        CHECK_EQ(asked.sort, std::string("rating"));
        CHECK_EQ(asked.page, 3);
        // A sort the catalogue has no such order for, a page that is no number.
        GalleryQuery const odd = gallery_query_of(std::string("sort=evil&page=-4&use=2x"));
        CHECK_EQ(odd.sort, std::string("users"));
        CHECK_EQ(odd.page, 1);
        CHECK(!odd.use);
        GalleryQuery const use = gallery_query_of(std::string("use=5"));
        CHECK(use.use && *use.use == 5);
        GalleryQuery const crx = gallery_query_of(std::string("crx=https%3A%2F%2Fchromewebstore.google.com%2Fdetail%2Fa-theme%2Faaaabbbbccccddddeeeeffffgggghhhh"));
        CHECK(crx.crx && *crx.crx == "https://chromewebstore.google.com/detail/a-theme/aaaabbbbccccddddeeeeffffgggghhhh");
    }

    // --- The page's own address, for drawing it again -------------------------------
    {
        CHECK_EQ(gallery_address(GalleryQuery {}), std::string("about:themes"));
        GalleryQuery again = gallery_query_of(std::string("q=space+cats&sort=rating&page=3&use=2&crx=x"));
        // What was to be done on the way is not done twice.
        CHECK_EQ(gallery_address(again), std::string("about:themes?sort=rating&q=space%20cats&page=3"));
    }

    // --- The catalogue's search address ---------------------------------------------
    {
        GalleryQuery query;
        CHECK_EQ(amo_search_url("https://x.example/api/", query),
            std::string("https://x.example/api/?type=statictheme&app=firefox&sort=users&page_size=24&page=1"));
        query.words = "space cats & dogs";
        query.sort = "created";
        query.page = 2;
        CHECK_EQ(amo_search_url("https://x.example/api/", query),
            std::string("https://x.example/api/?type=statictheme&app=firefox&sort=created&page_size=24&page=2&q=space%20cats%20%26%20dogs"));
    }

    // --- Its answer -------------------------------------------------------------------
    {
        std::ifstream file(fixtures + "/amo-search.json", std::ios::binary);
        std::string const json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        CHECK(!json.empty());
        std::optional<GalleryAnswer> const answer = parse_amo_search(json, "https://cdn.example/api/v5/search/?x=1");
        CHECK(answer.has_value());
        if (answer) {
            CHECK_EQ(answer->pages, 3);
            // The third has no file to put on, and is left out.
            CHECK_EQ(answer->themes.size(), std::size_t { 2 });
            if (answer->themes.size() == 2) {
                GalleryTheme const& fox = answer->themes[0];
                CHECK_EQ(fox.name, std::string("Sample Fox")); // the English of its names
                CHECK_EQ(fox.author, std::string("A Fox"));
                CHECK_EQ(fox.users, 1234567L);
                // Addresses are taken against where the answer came from.
                CHECK_EQ(fox.file_url, std::string("https://cdn.example/api/v5/search/firefox-sample.xpi"));
                CHECK_EQ(fox.preview_url, std::string("https://cdn.example/api/v5/search/firefox-sample/header.png"));
                GalleryTheme const& other = answer->themes[1];
                CHECK_EQ(other.name, std::string("Chrome <b>d'essai</b>")); // no English: the first there is
                CHECK_EQ(other.author, std::string());
                CHECK_EQ(other.preview_url, std::string());
                CHECK_EQ(other.file_url, std::string("https://cdn.example/api/v5/search/chrome-sample.crx")); // the older shape: a list of files
            }
        }
        CHECK(!parse_amo_search("{\"detail\": \"Not found.\"}", "https://x.example/"));
        CHECK(!parse_amo_search("<html>", "https://x.example/"));
    }

    // --- A Chrome Web Store id ------------------------------------------------------
    {
        std::string const id = "aaaabbbbccccddddeeeeffffgggghhhh";
        CHECK(chrome_theme_id(id) == id);
        CHECK(chrome_theme_id("https://chromewebstore.google.com/detail/a-theme/" + id + "?hl=en") == id);
        CHECK(chrome_theme_id("  " + id + "  ") == id);
        CHECK(!chrome_theme_id("https://chromewebstore.google.com/category/themes"));
        CHECK(!chrome_theme_id(id + "a")); // thirty-three letters are no id
        CHECK(!chrome_theme_id(id.substr(1))); // nor thirty-one
        CHECK(!chrome_theme_id("aaaabbbbccccddddeeeeffffgggghhhz")); // nor a letter past p
        CHECK(chrome_crx_url(id).find("id%3D" + id + "%26") != std::string::npos);
        CHECK(chrome_crx_url(id).starts_with("https://clients2.google.com/service/update2/crx?"));
    }

    // --- The page ---------------------------------------------------------------------
    {
        ThemesPage page;
        page.own = { { "Sashfold", true }, { "Nord & Co", false } };
        page.query.words = "\"quoted\"";
        page.query.sort = "rating";
        page.query.page = 2;
        page.gallery = GalleryAnswer { { GalleryTheme { "A <script>", "B&B", "https://p.example/a.png", "https://f.example/a.xpi?x=1&y=2", 1234567 } }, 3 };
        std::string const html = themes_page(page);
        CHECK(html.find("<title>Themes</title>") != std::string::npos);
        // The one in use is said, the others are links that put them on.
        CHECK(html.find("<b>Sashfold \xe2\x9c\x93</b>") != std::string::npos);
        CHECK(html.find("<a href=\"about:themes?use=1\">Nord &amp; Co</a>") != std::string::npos);
        // A stranger's words are words: nothing of theirs is markup here.
        CHECK(html.find("A &lt;script&gt;") != std::string::npos);
        CHECK(html.find("<script>") == std::string::npos);
        CHECK(html.find("by B&amp;B") != std::string::npos);
        CHECK(html.find("1,234,567 users") != std::string::npos);
        CHECK(html.find("<a class=puton href=\"https://f.example/a.xpi?x=1&amp;y=2\">Put on</a>") != std::string::npos);
        CHECK(html.find("value=\"&quot;quoted&quot;\"") != std::string::npos);
        // The sort in use is said; the others, and the pages either side, keep the words.
        CHECK(html.find("<b>Top rated</b>") != std::string::npos);
        CHECK(html.find("about:themes?sort=users&q=%22quoted%22") != std::string::npos);
        CHECK(html.find("about:themes?sort=rating&q=%22quoted%22&page=1") != std::string::npos);
        CHECK(html.find("about:themes?sort=rating&q=%22quoted%22&page=3") != std::string::npos);
        CHECK(html.find("Page 2 of 3") != std::string::npos);
        // Both stores are named, and Chrome's has its box.
        CHECK(html.find("https://addons.mozilla.org/en-US/firefox/themes/") != std::string::npos);
        CHECK(html.find("https://chromewebstore.google.com/category/themes") != std::string::npos);
        CHECK(html.find("name=crx") != std::string::npos);

        ThemesPage none;
        none.gallery_error = "could not connect <now>";
        none.can_adopt = false;
        std::string const bare = themes_page(none);
        CHECK(bare.find("The gallery could not be read: could not connect &lt;now&gt;.") != std::string::npos);
        CHECK(bare.find("no profile folder") != std::string::npos);
        CHECK(bare.find("None yet.") != std::string::npos);
        ThemesPage empty;
        empty.gallery = GalleryAnswer {};
        CHECK(themes_page(empty).find("No themes by those words.") != std::string::npos);
    }

    return sashfold::test::report("theme_gallery");
}
