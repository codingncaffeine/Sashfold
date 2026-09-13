#include "Test.h"

#include "core/Bitmap.h"
#include "css/StyleResolver.h"
#include "css/Stylesheets.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "net/Csp.h"
#include "net/Url.h"
#include "paint/Painter.h"
#include "text/FontManager.h"
#include "ui/Frames.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Frames drawn into the page. Where a frame's document comes from — src,
// srcdoc, a data: URL — each filling the frame's content box at the frame's
// size, with the frame's own background showing through a document that
// gives none; a page that frames itself shown once inside itself and no
// deeper; ten frames deep and a hundred to a page. What a document says
// about being framed (X-Frame-Options, frame-ancestors over it), the page's
// frame-src over what is fetched, a fetched document's own policy over what
// it fetches, and the page's policy kept by srcdoc and data: documents. A
// frame drawn before taken as it was until its size changes; and the
// border an iframe has, and frameborder taking it away.

using namespace sashfold;

namespace {

bool same(Color a, Color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

struct File {
    std::string body;
    std::vector<net::Header> headers;
    std::string type;
};

File html_file(std::string body, std::vector<net::Header> headers = {})
{
    return File { std::move(body), std::move(headers), "text/html" };
}

// Serves the files a test names and counts what is asked of it. A request
// the guard it comes with refuses is counted apart and never served, as the
// shell's loader never makes it.
struct Server {
    std::map<std::string, File> files;
    std::function<std::optional<File>(std::string const& url)> generate;
    std::map<std::string, int> fetches;
    std::map<std::string, int> refusals;

    ui::FrameFetcher fetcher()
    {
        return [this](net::Url const& url, net::Url const&, net::ResourceKind,
                   net::RequestGuard const& guard) -> std::optional<ui::FrameResponse> {
            std::string const key = url.serialize();
            if (guard.refusal && guard.refusal(url, false)) {
                ++refusals[key];
                return std::nullopt;
            }
            ++fetches[key];
            std::optional<File> file;
            if (auto const it = files.find(key); it != files.end())
                file = it->second;
            else if (generate)
                file = generate(key);
            if (!file)
                return std::nullopt;
            return ui::FrameResponse { std::vector<std::uint8_t>(file->body.begin(), file->body.end()), file->type, url,
                file->headers };
        };
    }
};

// The page at https://example.test/page.html, 400 px wide, with its frames
// drawn and the whole painted.
Bitmap render(std::string const& markup, Server& server, net::ContentSecurityPolicy* policy = nullptr)
{
    dom::Document document;
    html::parse_document_bytes_into(document, markup);
    net::Url const base = *net::parse_url("https://example.test/page.html");
    css::MediaContext const media { 400, 900, 1 };
    std::vector<css::SheetSource> const sheets = css::collect_stylesheets(document, &base, {}, media);
    css::StyleMap const styles = css::resolve_styles(document, sheets, media, &base);
    layout::LayoutResult page = layout::layout_document(document, styles, 400, nullptr, nullptr, 900);
    ui::draw_frames(base, page, server.fetcher(), 1.0f, policy);
    Bitmap canvas(400, 900, page.canvas_background);
    paint::paint_page(canvas, page);
    return canvas;
}

}

int main()
{
    text::FontManager::instance().set_system_fonts(false);
    Color const lime = Color::rgb(0, 255, 0);
    Color const red = Color::rgb(255, 0, 0);
    Color const yellow = Color::rgb(255, 255, 0);
    Color const white = Color::rgb(255, 255, 255);
    Color const magenta = Color::rgb(255, 0, 255);
    Color const blue = Color::rgb(0, 0, 255);
    Color const navy = Color::rgb(0, 0, 128);
    Color const cyan = Color::rgb(0, 255, 255);
    Color const gray = Color::rgb(128, 128, 128);
    std::string const green_page
        = "<!doctype html><body style='margin:0'><div style='width:100%;height:100vh;background:#00ff00'></div>";
    std::string const red_page = "<!doctype html><body style='margin:0;background:#ff0000'>";
    // Frames 100 by 60, one under another.
    std::string const head
        = "<!doctype html><style>body { margin: 0 } iframe { display: block; border: 0; width: 100px; height: 60px }</style>";

    {
        Server server;
        server.files["https://example.test/green.html"] = html_file(green_page);
        server.files["https://example.test/clear.html"] = html_file("<!doctype html><body style='margin:0'>");
        server.files["https://example.test/self.html"] = html_file("<!doctype html><body style='margin:0;background:#0000ff'>"
                                                                   "<iframe src='self.html' style='display:block;border:0;margin-left:20px;"
                                                                   "width:60px;height:30px;background:#000080'></iframe>");
        Bitmap const canvas = render(head
                + "<iframe src='green.html'></iframe>"
                  "<iframe srcdoc=\"<body style='margin:0;background:#ff0000'>\"></iframe>"
                  "<iframe src=\"data:text/html,<body style='margin:0'><div style='width:50vw;height:30px;background:%23ffff00'></div>\"></iframe>"
                  "<iframe src='clear.html' style='background:#ff00ff'></iframe>"
                  "<iframe src='self.html'></iframe>"
                  "<iframe src='missing.html' style='background:#00ffff'></iframe>"
                  "<iframe src='green.html' style='display:none'></iframe>",
            server);
        CHECK(same(canvas.pixel(50, 30), lime)); // src: 100vh is the frame's 60px
        CHECK(same(canvas.pixel(50, 90), red)); // srcdoc
        CHECK(same(canvas.pixel(25, 135), yellow)); // data: 50vw is half the frame's 100px
        CHECK(same(canvas.pixel(75, 135), white)); // and past it the page shows through
        CHECK(same(canvas.pixel(50, 210), magenta)); // a document with no color of its own: the frame's
        CHECK(same(canvas.pixel(10, 250), blue)); // a page that frames itself
        CHECK(same(canvas.pixel(30, 250), blue)); // shows itself inside once,
        CHECK(same(canvas.pixel(65, 250), navy)); // and that copy's frame only its background
        CHECK(same(canvas.pixel(50, 330), cyan)); // nothing to show
        CHECK_EQ(server.fetches["https://example.test/green.html"], 1); // the frame not displayed asks for nothing
        CHECK_EQ(server.fetches["https://example.test/self.html"], 2);
        CHECK_EQ(server.fetches["https://example.test/missing.html"], 1);
        CHECK_EQ(server.fetches.size(), std::size_t { 4 });
    }

    // What a document says about being framed, and its policy over what it
    // fetches.
    {
        Server server;
        server.files["https://example.test/deny.html"] = html_file(red_page, { { "X-Frame-Options", "DENY" } });
        server.files["https://example.test/sameorigin.html"] = html_file(green_page, { { "X-Frame-Options", "SAMEORIGIN" } });
        server.files["https://other.test/sameorigin.html"] = html_file(red_page, { { "x-frame-options", "sameorigin" } });
        server.files["https://example.test/conflict.html"]
            = html_file(red_page, { { "X-Frame-Options", "SAMEORIGIN" }, { "X-Frame-Options", "ALLOWALL" } });
        server.files["https://example.test/csp-none.html"] = html_file(red_page,
            { { "Content-Security-Policy", "frame-ancestors 'none'" }, { "X-Frame-Options", "ALLOWALL" } });
        server.files["https://example.test/csp-self.html"] = html_file(green_page,
            { { "X-Frame-Options", "DENY" }, { "Content-Security-Policy", "frame-ancestors 'self'" } });
        server.files["https://example.test/styled.html"]
            = html_file("<!doctype html><link rel=stylesheet href='red.css'><style>html { background: #ff0000 }</style>",
                { { "Content-Security-Policy", "style-src 'none'" } });
        server.files["https://example.test/red.css"] = File { "html { background: #ff0000 }", {}, "text/css" };
        Bitmap const canvas = render(head + "<style>iframe { background: #808080 }</style>"
                + "<iframe src='deny.html'></iframe>"
                  "<iframe src='sameorigin.html'></iframe>"
                  "<iframe src='https://other.test/sameorigin.html'></iframe>"
                  "<iframe src='conflict.html'></iframe>"
                  "<iframe src='csp-none.html'></iframe>"
                  "<iframe src='csp-self.html'></iframe>"
                  "<iframe src='styled.html'></iframe>",
            server);
        CHECK(same(canvas.pixel(50, 30), gray)); // X-Frame-Options: DENY
        CHECK(same(canvas.pixel(50, 90), lime)); // SAMEORIGIN, in a page of its own origin
        CHECK(same(canvas.pixel(50, 150), gray)); // and in a page of another
        CHECK(same(canvas.pixel(50, 210), gray)); // values at odds refuse
        CHECK(same(canvas.pixel(50, 270), gray)); // frame-ancestors 'none', whatever X-Frame-Options says
        CHECK(same(canvas.pixel(50, 330), lime)); // frame-ancestors 'self' allows, and X-Frame-Options goes unread
        CHECK(same(canvas.pixel(50, 390), gray)); // its own style-src refused its sheet and its <style>
        CHECK_EQ(server.refusals["https://example.test/red.css"], 1);
        CHECK_EQ(server.fetches.count("https://example.test/red.css"), std::size_t { 0 });
    }

    // The page's policy: frame-src over what is fetched, and everything else
    // in it over the srcdoc and data: documents, which keep it.
    {
        Server server;
        server.files["https://example.test/green.html"] = html_file(green_page);
        server.files["https://other.test/green.html"] = html_file(green_page);
        net::ContentSecurityPolicy policy(*net::parse_url("https://example.test/page.html"));
        policy.add_header("frame-src 'self' data:; style-src 'self'", false);
        Bitmap const canvas = render(head + "<style>iframe { background: #808080 }</style>"
                + "<iframe src='green.html'></iframe>"
                  "<iframe src='https://other.test/green.html'></iframe>"
                  "<iframe srcdoc='<style>html { background: #ff0000 }</style>'></iframe>"
                  "<iframe srcdoc='<body bgcolor=#00ff00>'></iframe>"
                  "<iframe src='data:text/html,<style>html{background:%23ff0000}</style>'></iframe>"
                  "<iframe src='data:text/html,<body bgcolor=%2300ff00>'></iframe>",
            server, &policy);
        CHECK(same(canvas.pixel(50, 30), lime)); // frame-src 'self'
        CHECK(same(canvas.pixel(50, 90), gray)); // another origin, never asked for
        CHECK_EQ(server.refusals["https://other.test/green.html"], 1);
        CHECK_EQ(server.fetches.count("https://other.test/green.html"), std::size_t { 0 });
        CHECK(same(canvas.pixel(50, 150), gray)); // an srcdoc document keeps the page's style-src
        CHECK(same(canvas.pixel(50, 210), lime)); // and is drawn under it
        CHECK(same(canvas.pixel(50, 270), gray)); // so does a data: document
        CHECK(same(canvas.pixel(50, 330), lime));
    }

    // Ten frames deep, and a hundred frames to a page.
    {
        Server server;
        server.generate = [](std::string const& url) -> std::optional<File> {
            std::string const prefix = "https://example.test/deep/";
            if (!url.starts_with(prefix))
                return std::nullopt;
            int const n = std::stoi(url.substr(prefix.size()));
            return html_file("<!doctype html><iframe src='" + std::to_string(n + 1) + ".html'></iframe>");
        };
        render(head + "<iframe src='deep/1.html'></iframe>", server);
        CHECK_EQ(server.fetches.size(), std::size_t { 10 });
        CHECK_EQ(server.fetches["https://example.test/deep/10.html"], 1);
    }
    {
        Server server;
        std::string markup = head;
        for (int i = 0; i < 101; ++i)
            markup += "<iframe src='many/" + std::to_string(i) + ".html'></iframe>";
        render(markup, server);
        CHECK_EQ(server.fetches.size(), std::size_t { 100 });
    }

    // A frame drawn before is taken as it was, until its size changes.
    {
        Server server;
        server.files["https://example.test/green.html"] = html_file(green_page);
        dom::Document document;
        html::parse_document_bytes_into(document,
            "<!doctype html><style>body { margin: 0 } iframe { display: block; border: 0; width: 50%; height: 60px }</style>"
            "<iframe src='green.html'></iframe>");
        net::Url const base = *net::parse_url("https://example.test/page.html");
        ui::DrawnFrames drawn;
        auto const draw_at = [&](int width) {
            css::MediaContext const media { static_cast<float>(width), 600, 1 };
            css::StyleMap const styles
                = css::resolve_styles(document, css::collect_stylesheets(document, &base, {}, media), media, &base);
            layout::LayoutResult laid = layout::layout_document(document, styles, static_cast<float>(width), nullptr, nullptr, 600);
            ui::draw_frames(base, laid, server.fetcher(), 1.0f, nullptr, &drawn);
            Bitmap canvas(width, 600, laid.canvas_background);
            paint::paint_page(canvas, laid);
            return canvas;
        };
        CHECK(same(draw_at(400).pixel(100, 30), lime));
        CHECK(same(draw_at(400).pixel(100, 30), lime));
        CHECK_EQ(server.fetches["https://example.test/green.html"], 1);
        CHECK(same(draw_at(300).pixel(100, 30), lime));
        CHECK_EQ(server.fetches["https://example.test/green.html"], 2);
        CHECK_EQ(drawn.size(), std::size_t { 1 });
    }

    // An iframe is framed by a border of its own (HTML's rendering section:
    // 2px inset), which frameborder=0, or a frameborder that is no number,
    // takes away.
    {
        dom::Document plain;
        html::parse_document_bytes_into(plain,
            "<!doctype html><iframe id=plain></iframe><iframe id=zero frameborder=0></iframe>"
            "<iframe id=no frameborder=no></iframe><iframe id=one frameborder=1></iframe>");
        css::StyleMap const styles = css::resolve_styles(plain);
        std::map<std::string, float> borders;
        for (auto const& [element, computed] : styles) {
            dom::Attr const* const id = element->find_attribute("id");
            if (id && element->is_html("iframe")) {
                borders[id->value] = computed.border_left.width + computed.border_top.width + computed.border_right.width
                    + computed.border_bottom.width;
            }
        }
        CHECK_EQ(borders["plain"], 8.0f);
        CHECK_EQ(borders["zero"], 0.0f);
        CHECK_EQ(borders["no"], 0.0f);
        CHECK_EQ(borders["one"], 8.0f);
    }

    return test::report("frames");
}
