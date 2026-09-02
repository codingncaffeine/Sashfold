#include "Test.h"

#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "net/Url.h"
#include "ui/Reader.h"

#include <memory>
#include <string>
#include <string_view>

// Reader mode: the article container wins over the furniture around it,
// the title comes from its heading, links and pictures come out absolute,
// scripts and the negatively named boxes come out not at all, wrappers
// give way to their children, and a page with no paragraphs falls back to
// its body.

using namespace sashfold;

namespace {

bool contains(std::string const& text, std::string_view needle)
{
    return text.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    net::Url const base = *net::parse_url("https://example.org/dir/page.html");

    // --- The article wins, the furniture does not ------------------------------
    {
        constexpr std::string_view html = R"HTML(<!doctype html><html><head><title>Article - Site</title>
<script>var x = 1;</script><style>p { color: red }</style></head><body>
<nav><a href="/">Home</a> <a href="/about">About</a></nav>
<div class="sidebar"><p>Sidebar text that should not be in the reader view, with plenty of words to score, and commas, commas, commas, commas.</p></div>
<article>
<h1>The article heading</h1>
<p>First paragraph of the article, long enough to count as content, with a few commas, and some more words to pass the threshold.</p>
<p>Second paragraph continues the article with <span class="x">a span</span>, <a href="/x">a link</a>, and <em>emphasis</em>, all long enough to score well in the extraction.</p>
<div><p>Third paragraph, again long enough, keeps the article the strongest candidate on the page.</p></div>
<figure><img src="pic.png" alt="A picture" width="200"><figcaption>Caption</figcaption></figure>
<script>alert(1)</script>
</article>
<div id="comments"><p>A comment that is long enough to look like content, with commas, but sits in the comments section, so it should be dropped.</p></div>
<footer><p>Footer text with enough length and commas, commas, to look like a paragraph, but it is the footer.</p></footer>
</body></html>)HTML";
        auto const document = html::parse_document(html);
        ui::Article const article = ui::extract_article(*document, base);
        CHECK_EQ(article.title, "The article heading");
        CHECK(contains(article.html, "<p>First paragraph"));
        CHECK(contains(article.html, "<p>Third paragraph"));
        CHECK(contains(article.html, "a span, <a href=\"https://example.org/x\">a link</a>, and <em>emphasis</em>"));
        CHECK(contains(article.html, "<img src=\"https://example.org/dir/pic.png\" alt=\"A picture\" width=\"200\">"));
        CHECK(contains(article.html, "<figure>") && contains(article.html, "<figcaption>Caption</figcaption>"));
        CHECK(!contains(article.html, "<span")); // wrappers give way to their children
        CHECK(!contains(article.html, "<div")); // and so do divs
        CHECK(!contains(article.html, "<h1>")); // the heading is the page's title
        CHECK(!contains(article.html, "Sidebar"));
        CHECK(!contains(article.html, "comment"));
        CHECK(!contains(article.html, "Footer"));
        CHECK(!contains(article.html, "Home"));
        CHECK(!contains(article.html, "alert"));
        CHECK(!contains(article.html, "class="));

        std::string const page = ui::reader_page(*document, base);
        CHECK(contains(page, "<title>The article heading</title>"));
        CHECK(contains(page, "<h1>The article heading</h1>"));
        CHECK(contains(page, "<a href=\"https://example.org/dir/page.html\">example.org</a>"));
        CHECK(contains(page, "<p>First paragraph"));
    }

    // --- A page with no paragraphs falls back to its body and <title> --------
    {
        auto const document = html::parse_document(
            "<!doctype html><title>Just a title</title><body>Just some text <b>here</b>.</body>");
        ui::Article const article = ui::extract_article(*document, base);
        CHECK_EQ(article.title, "Just a title");
        CHECK(contains(article.html, "Just some text <b>here</b>."));
    }

    // --- Text is escaped on the way out -------------------------------------------
    {
        auto const document = html::parse_document(
            "<!doctype html><body><p>Fish &amp; chips are &lt;b&gt;good&lt;/b&gt;, said the sign, with commas, commas, and more commas here.</p></body>");
        ui::Article const article = ui::extract_article(*document, base);
        CHECK(contains(article.html, "Fish &amp; chips are &lt;b&gt;good&lt;/b&gt;"));
    }

    return sashfold::test::report("reader");
}
