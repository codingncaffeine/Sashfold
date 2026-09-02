#include "Test.h"

#include "css/StyleResolver.h"
#include "css/Stylesheets.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "net/Http.h"
#include "net/Url.h"
#include "ui/ShellLoader.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// External stylesheets: <link rel=stylesheet> and @import fetched in
// cascade order through a fetcher the caller supplies, relative references
// resolved against the document, cycles and depth bounded, print sheets
// and alternates left alone; bytes decoded by the CSS charset rules; the
// cascade honoring the order the sheets arrived in. Then the real shell
// loader over data: URLs, so no network is needed.

using namespace sashfold;

namespace {

struct FakeFetcher {
    std::map<std::string, std::string> sheets; // url -> text
    std::vector<std::string> requested;

    std::optional<css::FetchedSheet> operator()(net::Url const& url)
    {
        requested.push_back(url.serialize());
        auto const it = sheets.find(url.serialize());
        if (it == sheets.end())
            return std::nullopt;
        return css::FetchedSheet { std::vector<std::uint8_t>(it->second.begin(), it->second.end()),
            "text/css" };
    }
};

dom::Element const* find_by_id(dom::Node const& node, std::string_view id)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (dom::Attr const* attribute = element.find_attribute("id"); attribute
            && attribute->value == id)
            return &element;
    }
    for (dom::Node const* child : node.children()) {
        if (dom::Element const* found = find_by_id(*child, id))
            return found;
    }
    return nullptr;
}

int red_of(css::StyleMap const& styles, dom::Document const& document, std::string_view id)
{
    dom::Element const* element = find_by_id(document, id);
    if (!element)
        return -1;
    auto const it = styles.find(element);
    return it == styles.end() ? -1 : it->second.color.r;
}

std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

} // namespace

int main()
{
    // --- Collection: order, resolution, conditions, cycles -----------------------
    net::Url const base = *net::parse_url("https://example.test/dir/page.html");
    FakeFetcher fetcher;
    fetcher.sheets["https://example.test/dir/a.css"]
        = "@import \"b.css\";\n@import url(sub/c.css) print;\np { color: rgb(1, 0, 0) }";
    fetcher.sheets["https://example.test/dir/b.css"] = "@import 'a.css';\np { color: rgb(2, 0, 0) }";
    fetcher.sheets["https://example.test/dir/sub/c.css"] = "p { color: rgb(3, 0, 0) }";
    fetcher.sheets["https://example.test/abs.css"] = "p { color: rgb(4, 0, 0) }";
    fetcher.sheets["https://cdn.test/x.css"] = "p { color: rgb(5, 0, 0) }";
    auto const document = html::parse_document(std::string_view(R"(<!doctype html>
<html><head>
  <style>p { color: rgb(9, 0, 0) }</style>
  <link rel="stylesheet" href="a.css">
  <link rel="alternate stylesheet" href="/abs.css">
  <link rel="StyleSheet" href="/abs.css" media="print">
  <link rel="stylesheet" href="https://cdn.test/missing.css">
  <link rel="stylesheet">
  <link rel="stylesheet" href="/abs.css" disabled>
  <link rel="icon" href="/favicon.ico">
  <link rel="stylesheet" href="/abs.css" type="text/plain">
  <style media="print">p { color: rgb(8, 0, 0) }</style>
  <style>p { color: rgb(7, 0, 0) }</style>
  <link rel="stylesheet" href="https://cdn.test/x.css" type="text/css" media="screen, print">
</head><body><p id="p">text</p></body></html>)"));
    std::vector<css::SheetSource> const sheets = css::collect_stylesheets(*document, &base,
        [&](net::Url const& url) { return fetcher(url); });
    // In order: the first <style>, b (imported by a, whose import of a is a
    // cycle), a itself, the last <style>, then x. Print, alternate,
    // disabled, wrongly typed and missing sheets are absent.
    if (CHECK_EQ(sheets.size(), 5u)) {
        CHECK(sheets[0].text.find("rgb(9") != std::string::npos);
        CHECK(sheets[0].url && sheets[0].url->serialize() == base.serialize());
        CHECK(sheets[1].text.find("rgb(2") != std::string::npos);
        CHECK(sheets[1].url && sheets[1].url->serialize() == "https://example.test/dir/b.css");
        CHECK(sheets[2].text.find("rgb(1") != std::string::npos);
        CHECK(sheets[3].text.find("rgb(7") != std::string::npos);
        CHECK(sheets[4].text.find("rgb(5") != std::string::npos);
    }
    // Each URL asked for once: a, b, missing, x; never c (print) or abs.
    CHECK_EQ(fetcher.requested.size(), 4u);
    auto const asked = [&](std::string const& url) {
        return std::find(fetcher.requested.begin(), fetcher.requested.end(), url)
            != fetcher.requested.end();
    };
    CHECK(asked("https://example.test/dir/a.css"));
    CHECK(asked("https://example.test/dir/b.css"));
    CHECK(asked("https://cdn.test/missing.css"));
    CHECK(asked("https://cdn.test/x.css"));
    CHECK(!asked("https://example.test/dir/sub/c.css"));
    CHECK(!asked("https://example.test/abs.css"));

    // The cascade takes the sheets in that order: the last one wins.
    CHECK_EQ(red_of(css::resolve_styles(*document, sheets), *document, "p"), 5);
    // Without a fetcher only the <style> elements count.
    CHECK_EQ(red_of(css::resolve_styles(*document), *document, "p"), 7);
    CHECK(css::collect_stylesheets(*document, nullptr, {}).size() == 2);

    // Imports stop a few levels down.
    FakeFetcher deep;
    for (int level = 0; level < 10; ++level) {
        deep.sheets["https://deep.test/" + std::to_string(level) + ".css"]
            = "@import \"" + std::to_string(level + 1) + ".css\"; p { color: rgb(" + std::to_string(level) + ", 0, 0) }";
    }
    auto const deep_document = html::parse_document(std::string_view(
        R"(<link rel="stylesheet" href="https://deep.test/0.css"><p id="p">x</p>)"));
    std::vector<css::SheetSource> const deep_sheets = css::collect_stylesheets(*deep_document, nullptr,
        [&](net::Url const& url) { return deep(url); });
    CHECK_EQ(deep_sheets.size(), 5u); // levels 4, 3, 2, 1, 0
    CHECK(!deep_sheets.empty() && deep_sheets.back().text.find("rgb(0,") != std::string::npos);

    // --- @import parsing ----------------------------------------------------------------
    std::vector<std::string> const imports = css::import_urls(
        "@charset \"utf-8\";\n@import url(\"a.css\");\n@import 'b.css' screen;\n"
        "@import url(c.css) print;\n@import url(d.css) (min-width: 1px);\np {}\n@import \"late.css\";");
    if (CHECK_EQ(imports.size(), 3u)) {
        CHECK_EQ(imports[0], std::string("a.css"));
        CHECK_EQ(imports[1], std::string("b.css"));
        CHECK_EQ(imports[2], std::string("d.css"));
    }

    // --- Decoding ----------------------------------------------------------------------------
    CHECK_EQ(css::decode_stylesheet(bytes_of("\xEF\xBB\xBFp{}"), ""), std::string("p{}"));
    CHECK_EQ(css::decode_stylesheet(bytes_of(std::string_view("\xFF\xFEp\0{\0}\0", 8)), ""),
        std::string("p{}"));
    CHECK_EQ(css::decode_stylesheet(bytes_of("a\xE9"), "text/css; charset=windows-1252"),
        std::string("a\xC3\xA9"));
    CHECK_EQ(css::decode_stylesheet(bytes_of("a\xE9"), "text/css; charset=\"ISO-8859-1\""),
        std::string("a\xC3\xA9"));
    CHECK_EQ(css::decode_stylesheet(bytes_of("@charset \"windows-1252\";\xE9"), ""),
        std::string("@charset \"windows-1252\";\xC3\xA9"));
    CHECK_EQ(css::decode_stylesheet(bytes_of("\xC3\xA9"), "text/css"), std::string("\xC3\xA9"));
    CHECK_EQ(css::decode_stylesheet(bytes_of("\xE9"), ""), std::string("\xEF\xBF\xBD")); // U+FFFD

    // --- Media queries --------------------------------------------------------------------------
    css::MediaContext const wide { 800, 600 };
    css::MediaContext const narrow { 400, 700 };
    auto const wide_says = [&](char const* query) { return css::media_query_matches(query, wide); };
    auto const narrow_says = [&](char const* query) { return css::media_query_matches(query, narrow); };
    CHECK(wide_says(""));
    CHECK(wide_says("screen"));
    CHECK(wide_says("all"));
    CHECK(wide_says("Screen, print"));
    CHECK(wide_says("only screen and (min-width: 1px)"));
    CHECK(wide_says("not print"));
    CHECK(!wide_says("print"));
    CHECK(!wide_says("print, speech"));
    CHECK(!wide_says("not screen"));
    CHECK(!wide_says("tty"));
    // Widths, both syntaxes, and the em unit.
    CHECK(wide_says("(min-width: 600px)"));
    CHECK(!narrow_says("(min-width: 600px)"));
    CHECK(!wide_says("(max-width: 599px)"));
    CHECK(narrow_says("(max-width: 599px)"));
    CHECK(wide_says("(max-width: 50em)"));
    CHECK(wide_says("(width >= 800px)"));
    CHECK(!wide_says("(width > 800px)"));
    CHECK(wide_says("(600px <= width <= 900px)"));
    CHECK(!narrow_says("(600px <= width <= 900px)"));
    CHECK(wide_says("screen and (min-width: 600px) and (max-width: 900px)"));
    CHECK(!wide_says("screen and (min-width: 600px) and (max-width: 700px)"));
    CHECK(wide_says("(min-width: 900px), (min-height: 500px)")); // a list: any query
    CHECK(wide_says("((min-width: 900px) or (min-height: 500px))"));
    CHECK(!wide_says("not all and (min-width: 600px)"));
    CHECK(narrow_says("not all and (min-width: 600px)"));
    // Orientation, ratio, resolution, the preference and pointer features.
    CHECK(wide_says("(orientation: landscape)"));
    CHECK(narrow_says("(orientation: portrait)"));
    CHECK(wide_says("(min-aspect-ratio: 4/3)"));
    CHECK(!narrow_says("(min-aspect-ratio: 4/3)"));
    CHECK(wide_says("(min-resolution: 1dppx)"));
    CHECK(!wide_says("(min-resolution: 2dppx)"));
    CHECK(wide_says("(-webkit-min-device-pixel-ratio: 1)"));
    CHECK(!wide_says("(-webkit-min-device-pixel-ratio: 1.5)"));
    CHECK(wide_says("(prefers-color-scheme: light)"));
    CHECK(!wide_says("(prefers-color-scheme: dark)"));
    CHECK(wide_says("(prefers-reduced-motion: no-preference)"));
    CHECK(!wide_says("(prefers-reduced-motion: reduce)"));
    CHECK(wide_says("(hover: hover) and (pointer: fine)"));
    CHECK(!wide_says("(hover: none)"));
    CHECK(wide_says("(scripting: none)"));
    CHECK(wide_says("(color)"));
    CHECK(!wide_says("(monochrome)"));
    CHECK(wide_says("not all and (monochrome)"));
    // Unknown features make their query false, even negated; garbage too.
    CHECK(!wide_says("(made-up-feature: 3)"));
    CHECK(!wide_says("not all and (made-up-feature: 3)"));
    CHECK(!wide_says("screen and"));
    CHECK(wide_says("screen and (made-up: 1), screen")); // the other query carries the list
    // The <link media> and @import gates see the same evaluator.
    CHECK(css::import_urls("@import url(a.css) (min-width: 600px);", narrow).empty());
    CHECK_EQ(css::import_urls("@import url(a.css) layer(base) screen and (min-width: 600px);", wide).size(), 1u);

    // --- @media blocks in the cascade ---------------------------------------------------------
    auto const responsive = html::parse_document(std::string_view(R"(<!doctype html>
<html><head><style>
  @media screen { @media (min-width: 1px) { p { color: rgb(9, 0, 0) } } }
  @media print { p { color: rgb(6, 0, 0) } }
  @media not all and (monochrome) { p { color: rgb(8, 0, 0) } }
  @media screen and (max-width: 500px) { p { color: rgb(7, 0, 0) } }
  @media (min-width: 600px) { p { color: rgb(5, 0, 0) } }
  @supports (display: grid) { p { color: rgb(4, 0, 0) } }
</style></head><body><p id="p">x</p></body></html>)"));
    std::vector<css::SheetSource> const responsive_sheets
        = css::collect_stylesheets(*responsive, nullptr, {});
    CHECK_EQ(red_of(css::resolve_styles(*responsive, responsive_sheets, wide), *responsive, "p"), 5);
    CHECK_EQ(red_of(css::resolve_styles(*responsive, responsive_sheets, narrow), *responsive, "p"), 7);
    // A compiled set resolves the same, as often as asked, and counts the
    // rules the context admitted (the UA sheet's included).
    css::StyleSet const wide_set(responsive_sheets, wide);
    css::StyleSet const narrow_set(responsive_sheets, narrow);
    CHECK_EQ(red_of(css::resolve_styles(*responsive, wide_set), *responsive, "p"), 5);
    CHECK_EQ(red_of(css::resolve_styles(*responsive, wide_set), *responsive, "p"), 5);
    CHECK_EQ(red_of(css::resolve_styles(*responsive, narrow_set), *responsive, "p"), 7);
    CHECK_EQ(wide_set.rule_count(), narrow_set.rule_count());
    CHECK(wide_set.rule_count() > 3);
    CHECK_EQ(wide_set.media().width, 800.0f);
    auto const link_media = html::parse_document(std::string_view(
        R"HTML(<style media="(max-width: 500px)">p { color: rgb(3, 0, 0) }</style><p id="p">x</p>)HTML"));
    CHECK_EQ(css::collect_stylesheets(*link_media, nullptr, {}, wide).size(), 0u);
    CHECK_EQ(css::collect_stylesheets(*link_media, nullptr, {}, narrow).size(), 1u);
    // An XHTML page's CDATA-wrapped sheet, parsed as HTML: the markers are
    // dropped so the sheet is not swallowed into a bracket block.
    auto const cdata = html::parse_document(std::string_view(
        "<style>\n  <![CDATA[\n  p { color: rgb(9, 0, 0) }\n  ]]>\n</style><p id=\"p\">x</p>"));
    CHECK_EQ(red_of(css::resolve_styles(*cdata), *cdata, "p"), 9);
    auto const plain_brackets = html::parse_document(std::string_view(
        "<style>p { color: rgb(9, 0, 0) } ]]></style><p id=\"p\">x</p>"));
    CHECK_EQ(red_of(css::resolve_styles(*plain_brackets), *plain_brackets, "p"), 9);

    // --- The shell loader, over data: URLs -----------------------------------------------------
    ui::ShellLoader loader;
    net::Url const page = *net::parse_url(
        "data:text/html,<link rel=stylesheet href=\"data:text/css,p{color:rgb(6,0,0)}\"><p id=q>q</p>");
    net::FetchResult const result = loader.load(page, "", false);
    if (CHECK(result.response.has_value())) {
        auto const loaded = html::parse_document_bytes(
            std::string(result.response->body.begin(), result.response->body.end()));
        std::vector<css::SheetSource> const loaded_sheets = css::collect_stylesheets(*loaded,
            &result.response->final_url, [&](net::Url const& url) -> std::optional<css::FetchedSheet> {
                net::FetchResult sub = loader.load_subresource(url, result.response->final_url, "");
                if (!sub.response || sub.response->status != 200)
                    return std::nullopt;
                std::string const* type = net::find_header(sub.response->headers, "content-type");
                return css::FetchedSheet { std::move(sub.response->body), type ? *type : "" };
            });
        CHECK_EQ(loaded_sheets.size(), 1u);
        CHECK_EQ(red_of(css::resolve_styles(*loaded, loaded_sheets), *loaded, "q"), 6);
    }

    return test::report("stylesheets");
}
