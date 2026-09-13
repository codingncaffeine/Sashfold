#include "Test.h"

#include "bindings/LayoutOracle.h"
#include "bindings/Realm.h"
#include "core/Bitmap.h"
#include "core/Png.h"
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
#include "ui/PageImages.h"

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
// frame drawn before taken as it was until its size changes; the border an
// iframe has, and frameborder taking it away; what a frame's realm is
// answered with, and a frame that has a realm drawn from its live document.

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
    int status = 200;
};

File html_file(std::string body, std::vector<net::Header> headers = {})
{
    return File { std::move(body), std::move(headers), "text/html", 200 };
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
                file->headers, file->status };
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

    // What the host answers a frame's realm with: an srcdoc as text with its
    // parent's origin, a fetched document under its own URL, a document the
    // rules refuse as nothing, and a page named twice up the chain of
    // documents a frame is inside as nothing.
    {
        Server server;
        server.files["https://example.test/green.html"] = html_file(green_page);
        server.files["https://example.test/deny.html"] = html_file(green_page, { net::Header { "X-Frame-Options", "DENY" } });
        server.files["https://example.test/page.html"] = html_file(green_page);
        net::Url const base = *net::parse_url("https://example.test/page.html");
        std::vector<bindings::FrameAncestor> const page_only { bindings::FrameAncestor { base.serialize(true), base } };
        std::function<dom::Element*(dom::Node&)> const first_iframe = [&first_iframe](dom::Node& node) -> dom::Element* {
            if (node.is_element() && static_cast<dom::Element&>(node).is_html("iframe"))
                return &static_cast<dom::Element&>(node);
            for (dom::Node* const child : node.children()) {
                if (dom::Element* const found = first_iframe(*child))
                    return found;
            }
            return nullptr;
        };
        auto const answer_for = [&](std::string const& markup, std::vector<bindings::FrameAncestor> const& chain) {
            dom::Document document;
            html::parse_document_bytes_into(document, markup);
            dom::Element* const iframe = first_iframe(document);
            return iframe ? ui::frame_document_for(*iframe, base, nullptr, chain, std::nullopt, server.fetcher())
                          : std::optional<bindings::FrameDocument>();
        };
        std::optional<bindings::FrameDocument> const srcdoc = answer_for("<iframe srcdoc='<p>x</p>'></iframe>", page_only);
        CHECK(srcdoc && srcdoc->srcdoc && std::string(srcdoc->bytes.begin(), srcdoc->bytes.end()) == "<p>x</p>");
        CHECK(srcdoc && srcdoc->origin.serialize_origin() == "https://example.test" && srcdoc->url.serialize() == "about:srcdoc");
        std::optional<bindings::FrameDocument> const fetched = answer_for("<iframe src='green.html'></iframe>", page_only);
        CHECK(fetched && !fetched->srcdoc && fetched->url.serialize() == "https://example.test/green.html");
        CHECK(!answer_for("<iframe src='deny.html'></iframe>", page_only));
        CHECK(answer_for("<iframe src='page.html'></iframe>", page_only).has_value());
        std::vector<bindings::FrameAncestor> const twice { page_only[0], page_only[0] };
        CHECK(!answer_for("<iframe src='page.html'></iframe>", twice));
    }

    // A frame whose document has a realm of its own is drawn from that live
    // document: what the frame's script did shows, where a frame drawn without
    // a realm parses its markup with scripting off.
    {
        Server server;
        net::Url const base = *net::parse_url("https://example.test/page.html");
        std::string const markup = head
            + "<iframe srcdoc=\"<body style='margin:0'><script>document.body.style.background = '#00ff00'</script>\"></iframe>";
        auto const pixel_drawn = [&](bool with_realm) {
            dom::Document document;
            bindings::HostHooks hooks;
            hooks.frame_document = [&server](dom::Element const& iframe, net::Url const& frame_base, net::ContentSecurityPolicy* policy,
                                       std::vector<bindings::FrameAncestor> const& ancestors, std::optional<net::Url> const& target) {
                return ui::frame_document_for(iframe, frame_base, policy, ancestors, target, server.fetcher());
            };
            bindings::Realm realm(document, base, std::move(hooks));
            html::parse_document_bytes_into(document, markup, &realm);
            realm.document_parsed();
            css::MediaContext const media { 400, 900, 1 };
            css::StyleMap const styles
                = css::resolve_styles(document, css::collect_stylesheets(document, &base, {}, media), media, &base);
            layout::LayoutResult laid = layout::layout_document(document, styles, 400, nullptr, nullptr, 900);
            ui::draw_frames(base, laid, server.fetcher(), 1.0f, nullptr, nullptr, with_realm ? &realm : nullptr);
            Bitmap canvas(400, 900, laid.canvas_background);
            paint::paint_page(canvas, laid);
            return canvas.pixel(50, 30);
        };
        CHECK(same(pixel_drawn(true), lime));
        CHECK(!same(pixel_drawn(false), lime));
    }

    // The page's policy over what a frame's document is fetched for (CSP3
    // §6.8.1, the effective directive): object-src for an object's or an
    // embed's, frame-src for an iframe's, and neither for the other.
    {
        net::Url const base = *net::parse_url("https://example.test/page.html");
        std::string const address = "https://example.test/o.html";
        net::Url const target = *net::parse_url(address);
        std::vector<bindings::FrameAncestor> const page_only { bindings::FrameAncestor { base.serialize(true), base } };
        struct Asked {
            bool answered = false;
            int fetches = 0;
            int refusals = 0;
            int status = 0;
        };
        auto const ask = [&](std::string const& header, std::string const& markup, bool as_target, int file_status = 200) {
            Server server;
            server.files[address] = html_file(green_page);
            server.files[address].status = file_status;
            net::ContentSecurityPolicy policy(base);
            policy.add_header(header, false);
            dom::Document document;
            html::parse_document_bytes_into(document, markup);
            std::function<dom::Element*(dom::Node&)> const first_element = [&first_element](dom::Node& node) -> dom::Element* {
                for (dom::Node* const child : node.children()) {
                    if (child->is_element()) {
                        dom::Element& element = static_cast<dom::Element&>(*child);
                        if (element.is_html("object") || element.is_html("embed") || element.is_html("iframe"))
                            return &element;
                    }
                    if (dom::Element* const found = first_element(*child))
                        return found;
                }
                return nullptr;
            };
            Asked asked;
            if (dom::Element* const element = first_element(document)) {
                std::optional<bindings::FrameDocument> const answer = ui::frame_document_for(*element, base, &policy, page_only,
                    as_target ? std::optional<net::Url>(target) : std::nullopt, server.fetcher());
                asked.answered = answer.has_value();
                asked.status = answer ? answer->status : 0;
            }
            asked.fetches = server.fetches.count(address) ? server.fetches[address] : 0;
            asked.refusals = server.refusals.count(address) ? server.refusals[address] : 0;
            return asked;
        };
        Asked const object = ask("frame-src *; object-src 'none'", "<object data='https://example.test/o.html'></object>", true);
        CHECK(!object.answered && object.refusals == 1 && object.fetches == 0);
        Asked const embed = ask("frame-src *; object-src 'none'", "<embed src='https://example.test/o.html'>", true);
        CHECK(!embed.answered && embed.refusals == 1 && embed.fetches == 0);
        Asked const refused_iframe = ask("frame-src 'none'; object-src *", "<iframe src='https://example.test/o.html'></iframe>", false);
        CHECK(!refused_iframe.answered && refused_iframe.refusals == 1 && refused_iframe.fetches == 0);
        Asked const fetched_iframe = ask("frame-src *; object-src 'none'", "<iframe src='https://example.test/o.html'></iframe>", false);
        CHECK(fetched_iframe.answered && fetched_iframe.fetches == 1 && fetched_iframe.refusals == 0);
        // The status the document came with goes with it.
        Asked const missing = ask("frame-src *", "<iframe src='https://example.test/o.html'></iframe>", false, 404);
        CHECK(missing.answered && missing.status == 404 && fetched_iframe.status == 200);
    }

    // An object or an embed is laid out and drawn as what the realm of its
    // document decided it represents (HTML §15.4.1): an object showing its
    // fallback is an ordinary box around its children, and so is one whose
    // picture does not decode; an embed that represents nothing has no size
    // of its own, though a size written for it still holds; an object's
    // picture is its content; an object's document is drawn from its window.
    // A frame inside a frameset has a window but no box, so nothing is drawn
    // for it. Laid out without a realm, nothing is decided and an object is the
    // replaced box it always was.
    {
        Server server;
        server.files["https://example.test/green.html"] = html_file(green_page);
        std::vector<std::uint8_t> const png = encode_png(Bitmap(12, 8, blue));
        server.files["https://example.test/pic.png"] = File { std::string(png.begin(), png.end()), {}, "image/png", 200 };
        server.files["https://example.test/bad.png"] = File { "no picture here", {}, "image/png", 200 };
        net::Url const base = *net::parse_url("https://example.test/page.html");
        css::MediaContext const media { 400, 900, 1 };
        std::string const markup = "<!doctype html><style>body { margin: 0 } object, embed { display: block; border: 0 }</style>"
                                   "<object id=fallback data='missing.html'><div id=inside style='width:50px;height:20px'></div></object>"
                                   "<embed id=nothing>"
                                   "<embed id=sized width=40 height=30>"
                                   "<object id=picture data='pic.png'></object>"
                                   "<object id=broken data='bad.png'><div id=instead style='width:30px;height:10px'></div></object>"
                                   "<object id=shown data='green.html' style='width:100px;height:60px'></object>";
        struct Live {
            dom::Document document;
            std::unique_ptr<bindings::Realm> realm;
        };
        auto const open_live = [&](std::string const& page_markup) {
            auto live = std::make_unique<Live>();
            bindings::HostHooks hooks;
            hooks.frame_document = [&server](dom::Element const& element, net::Url const& frame_base, net::ContentSecurityPolicy* policy,
                                       std::vector<bindings::FrameAncestor> const& ancestors, std::optional<net::Url> const& target) {
                return ui::frame_document_for(element, frame_base, policy, ancestors, target, server.fetcher());
            };
            hooks.image_decodes = [](std::vector<std::uint8_t> const& bytes) { return ui::decode_image_bytes(bytes).has_value(); };
            live->realm = std::make_unique<bindings::Realm>(live->document, base, std::move(hooks));
            html::parse_document_bytes_into(live->document, page_markup, live->realm.get());
            live->realm->document_parsed();
            live->realm->run_pending();
            return live;
        };
        std::function<dom::Element*(dom::Node&, std::string_view)> const by_id
            = [&by_id](dom::Node& node, std::string_view id) -> dom::Element* {
            if (node.is_element()) {
                dom::Attr const* const attribute = static_cast<dom::Element&>(node).find_attribute("id");
                if (attribute && attribute->value == id)
                    return &static_cast<dom::Element&>(node);
            }
            for (dom::Node* const child : node.children()) {
                if (dom::Element* const found = by_id(*child, id))
                    return found;
            }
            return nullptr;
        };
        std::function<layout::Fragment const*(layout::Fragment const&, std::string_view)> const fragment_of
            = [&fragment_of](layout::Fragment const& fragment, std::string_view id) -> layout::Fragment const* {
            if (fragment.element) {
                dom::Attr const* const attribute = fragment.element->find_attribute("id");
                if (attribute && attribute->value == id)
                    return &fragment;
            }
            for (layout::Fragment const& child : fragment.children) {
                if (layout::Fragment const* const found = fragment_of(child, id))
                    return found;
            }
            return nullptr;
        };
        ui::ImageFetcher const fetch_image = [&server](net::Url const& url) -> std::optional<std::vector<std::uint8_t>> {
            std::optional<ui::FrameResponse> response
                = server.fetcher()(url, *net::parse_url("https://example.test/page.html"), net::ResourceKind::Image, {});
            if (!response)
                return std::nullopt;
            return std::move(response->bytes);
        };

        std::unique_ptr<Live> const live = open_live(markup);
        layout::EmbeddedStates const states = bindings::embedded_states(*live->realm);
        css::StyleMap const styles
            = css::resolve_styles(live->document, css::collect_stylesheets(live->document, &base, {}, media), media, &base);
        layout::ImageMap const images = ui::collect_images(live->document, &base, fetch_image, media, &states);
        layout::LayoutResult laid = layout::layout_document(live->document, styles, 400, &images, nullptr, 900, 1, &states);
        ui::draw_frames(base, laid, server.fetcher(), 1.0f, nullptr, nullptr, live->realm.get());
        Bitmap canvas(400, 900, laid.canvas_background);
        paint::paint_page(canvas, laid);

        layout::Fragment const* const inside = fragment_of(laid.root, "inside");
        CHECK(inside != nullptr && inside->width == 50.0f && inside->height == 20.0f);
        layout::Fragment const* const nothing = fragment_of(laid.root, "nothing");
        CHECK(nothing != nullptr && nothing->width == 0.0f && nothing->height == 0.0f);
        layout::Fragment const* const sized = fragment_of(laid.root, "sized");
        CHECK(sized != nullptr && sized->width == 40.0f && sized->height == 30.0f);
        layout::Fragment const* const picture = fragment_of(laid.root, "picture");
        CHECK(picture != nullptr && picture->image && picture->image->bitmap && picture->width == 12.0f && picture->height == 8.0f);
        layout::Fragment const* const instead = fragment_of(laid.root, "instead");
        CHECK(instead != nullptr && instead->width == 30.0f);
        layout::Fragment const* const shown = fragment_of(laid.root, "shown");
        CHECK(shown != nullptr && same(canvas.pixel(static_cast<int>(shown->x) + 50, static_cast<int>(shown->y) + 30), lime));

        // Without a realm nothing is decided: the object is replaced, 300 by 150.
        layout::LayoutResult const undecided = layout::layout_document(live->document, styles, 400, nullptr, nullptr, 900);
        layout::Fragment const* const replaced = fragment_of(undecided.root, "fallback");
        CHECK(replaced != nullptr && replaced->width == 300.0f && fragment_of(undecided.root, "inside") == nullptr);

        // What a script asks of the geometry follows the decisions: an embed not
        // decided yet has no size, and an object's fallback is laid out once it
        // has been decided, a task after the script that inserted it.
        bindings::LayoutOracle oracle(live->document, base, {}, media);
        oracle.set_realm(live->realm.get());
        live->realm->run("var late = document.createElement('object'); late.data = 'missing.html';"
                         " late.innerHTML = \"<div id=late style='width:50px;height:20px'></div>\"; document.body.appendChild(late);"
                         " var pending = document.createElement('embed'); pending.id = 'pending'; pending.src = 'green.html';"
                         " document.body.appendChild(pending);",
            "<test>");
        dom::Element* const late = by_id(live->document, "late");
        dom::Element* const pending = by_id(live->document, "pending");
        std::optional<bindings::LayoutBox> const late_before = late ? oracle.box(*late) : std::nullopt;
        std::optional<bindings::LayoutBox> const pending_before = pending ? oracle.box(*pending) : std::nullopt;
        CHECK(late != nullptr && !late_before);
        CHECK(pending_before && pending_before->width == 0.0f);
        live->realm->run_pending();
        std::optional<bindings::LayoutBox> const late_after = late ? oracle.box(*late) : std::nullopt;
        CHECK(late_after && late_after->width == 50.0f);

        // A frameset's frame: a window, no box, and nothing drawn.
        std::unique_ptr<Live> const framed = open_live("<!doctype html><frameset><frame id=f src='green.html'></frameset>");
        dom::Element* const frame = by_id(framed->document, "f");
        css::StyleMap const frame_styles = css::resolve_styles(framed->document);
        layout::LayoutResult frame_laid = layout::layout_document(framed->document, frame_styles, 400, nullptr, nullptr, 900);
        ui::draw_frames(base, frame_laid, server.fetcher(), 1.0f, nullptr, nullptr, framed->realm.get());
        CHECK(frame != nullptr && framed->realm->frame_realm(*frame) != nullptr && fragment_of(frame_laid.root, "f") == nullptr);
    }

    return test::report("frames");
}
