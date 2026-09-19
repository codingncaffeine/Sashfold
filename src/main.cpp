#include "bindings/LayoutOracle.h"
#include "bindings/Realm.h"
#include "core/Ascii.h"
#include "core/Bitmap.h"
#include "core/Json.h"
#include "core/Png.h"
#include "core/Unicode.h"
#include "css/Parser.h"
#include "css/StyleResolver.h"
#include "css/Stylesheets.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "html/TreeDump.h"
#include "layout/Layout.h"
#include "net/Http.h"
#include "paint/Painter.h"
#include "platform/Clipboard.h"
#include "platform/Memory.h"
#include "platform/Window.h"
#include "text/Face.h"
#include "text/FontManager.h"
#include "text/SashfoldMono.h"
#include "text/TrueType.h"
#include "ui/Browser.h"
#include "ui/Cosmetic.h"
#include "ui/Frames.h"
#include "ui/InternalPages.h"
#include "ui/PageImages.h"
#include "ui/Script.h"
#include "ui/ShellLoader.h"
#include "ui/Theme.h"
#include "ui/ThemeImport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace sashfold;

namespace {

int usage(char const* program)
{
    std::cerr << "usage: " << program << " [url] [--theme <file.json>] [--blocklists <dir>] [--downloads <dir>] [--profile <dir>]\n"
              << "                 [--exit-after ms] [--timings out.json]   (a headless run under a compositor with no screen)\n"
              << "       " << program << " --script <file> [--update-goldens] [--width N] [--height N]\n"
              << "       " << program << " --render <file.html|url> [-o out.png] [--width N] [--height N]\n"
              << "                 [--max-height N] [--thumbnail small.png [--thumbnail-width N]]\n"
              << "                 [--report out.json] [--gaps out.tsv] [--dump-layout] [--no-scripts] [--script-time ms]\n"
              << "       " << program << " --bench <file.html|url> [--runs N] [--width N] [--height N] [--report out.json]\n"
              << "       " << program << " --fetch <url>\n"
              << "       " << program << " --dump-dom <file.html>\n"
              << "       " << program << " --font-sampler <output.png> [--font <file.ttf>]\n"
              << "       " << program << " --font-info <file.ttf|file.ttc>\n"
              << "       " << program << " --font-list\n"
              << "       " << program << " --import-theme <folder|manifest.json|file.xpi|file.crx|file.zip> [-o <dir>]\n"
              << "       any mode: --fonts system|builtin   (system, except --script)\n"
              << "       any mode: --add-font <file.ttf>    (repeatable)\n"
              << "       " << program << " --smoke [-o output.png]\n"
              << "\n"
              << "  With a URL or nothing, opens the browser window.\n"
              << "  --theme applies a theme file to the window and to --script; the default is\n"
              << "          themes/default.json beside the executable or its parent, reloaded\n"
              << "          whenever the file changes while the window is open.\n"
              << "  --import-theme converts a Firefox or Chrome theme into a theme of ours, in\n"
              << "          -o's folder or the themes folder of the profile, where the window\n"
              << "          finds it; a theme dropped into that folder is converted the same way.\n"
              << "  --blocklists is the folder of content-blocking lists (filters/*.txt in\n"
              << "          Adblock syntax, nefarious/*.txt sites to keep off), read at start;\n"
              << "          the default is blocklists/ beside the executable or its parent.\n"
              << "  --downloads is where downloads are saved (the window defaults to your\n"
              << "          Downloads folder; --script saves nothing unless told where).\n"
              << "  --profile is the folder the window keeps its session in (the tabs of the\n"
              << "          last run, brought back on the next start): $XDG_CONFIG_HOME/sashfold\n"
              << "          or ~/.config/sashfold on Linux and macOS, %APPDATA%\\Sashfold on Windows.\n"
              << "  --script replays a shell script headlessly and checks its assertions.\n"
              << "  --render lays out the page (local file or live URL) and writes a PNG; a load\n"
              << "          that fails renders the page the window would show. --max-height caps the\n"
              << "          picture, --thumbnail draws the viewport's top small, --report writes a\n"
              << "          JSON account of the load and the render, --gaps counts what the page\n"
              << "          wrote that the engine dropped (unknown properties, skipped at-rules,\n"
              << "          selectors that do not parse, uncaught script errors, custom and\n"
              << "          unknown elements) as kind, item and hits, --dump-layout prints every\n"
              << "          laid-out box and text run with its position and size. The page's\n"
              << "          scripts run first, their timers on a virtual clock given --script-time\n"
              << "          milliseconds (3000); --no-scripts renders the markup alone.\n"
              << "  --bench runs a session on a page the way the window does and times it: first paint, pixels,\n"
              << "          every phase of the shell.s work, a scroll frame, the memory; one JSON, medians over runs.\n"
              << "  --fetch prints the response head through the fetch choke point.\n"
              << "  --dump-dom parses the file and prints the document tree.\n"
              << "  --add-font installs a font file for the page as if the machine had it,\n"
              << "          which is how a render is taken in the font a test is scored in.\n"
              << "  --font-sampler draws the Sashfold Mono QA sheet.\n"
              << "  --smoke renders the paint smoke scene.\n";
    return 2;
}

int font_info(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "error: cannot read " << path << "\n";
        return 1;
    }
    std::vector<std::uint8_t> const bytes((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    std::size_t const faces = text::TrueTypeFont::face_count(bytes);
    if (faces == 0) {
        std::cerr << "error: not a TrueType font: " << path << "\n";
        return 1;
    }
    for (std::size_t face = 0; face < faces; ++face) {
        std::optional<text::TrueTypeFont> const font = text::TrueTypeFont::parse(bytes, face);
        if (!font) {
            std::cout << "face " << face << ": malformed\n";
            continue;
        }
        std::size_t outlined = 0;
        std::size_t empty = 0;
        std::size_t malformed = 0;
        std::size_t points = 0;
        for (std::uint32_t glyph = 0; glyph < font->glyph_count(); ++glyph) {
            auto const outline = font->outline(static_cast<std::uint16_t>(glyph));
            if (!outline)
                ++malformed;
            else if (outline->points.empty())
                ++empty;
            else {
                ++outlined;
                points += outline->points.size();
            }
        }
        std::uint16_t const a = font->glyph_index(U'A');
        std::cout << "face " << face << ": " << font->family_name() << " / "
                  << font->subfamily_name() << "\n"
                  << "  units/em " << font->units_per_em() << ", ascender " << font->ascender()
                  << ", descender " << font->descender() << ", line gap " << font->line_gap()
                  << ", x-height " << font->x_height() << ", cap height " << font->cap_height()
                  << "\n"
                  << "  weight " << font->weight_class() << (font->is_italic() ? ", italic" : "")
                  << (font->has_cff() ? ", CFF outlines" : "") << "\n"
                  << "  glyphs " << font->glyph_count() << ": " << outlined << " with outlines, "
                  << empty << " empty, " << malformed << " malformed; " << points << " points\n"
                  << "  cmap: " << font->mapped_code_points() << " code points; 'A' -> glyph " << a
                  << ", advance " << font->advance_width(a) << ", lsb "
                  << font->left_side_bearing(a) << "\n";
    }
    return 0;
}

// Every face the font manager finds on this machine, and how long the
// catalogue took to build.
int font_list()
{
    auto const started = std::chrono::steady_clock::now();
    std::vector<text::FaceInfo> const& faces = text::FontManager::instance().catalogue();
    auto const elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started);
    for (text::FaceInfo const& face : faces) {
        std::cout << face.family << " / " << face.subfamily << "  (" << face.weight_class
                  << (face.italic ? ", italic" : "") << ")  " << face.path;
        if (face.face_index != 0)
            std::cout << " #" << face.face_index;
        std::cout << "\n";
    }
    std::cout << faces.size() << " faces catalogued in " << elapsed.count() << " ms\n";
    return 0;
}

int fetch_url(std::string const& input)
{
    auto const url = net::parse_url(input);
    if (!url) {
        std::cerr << "error: unparseable URL " << input << "\n";
        return 1;
    }
    net::FetchResult result = net::fetch(*url);
    if (!result.response) {
        std::cerr << "error: " << result.error << "\n";
        return 1;
    }
    std::cout << result.response->status << " " << result.response->status_text << "  ("
              << result.response->final_url.serialize() << ")\n";
    for (net::Header const& header : result.response->headers)
        std::cout << header.name << ": " << header.value << "\n";
    std::cout << "\n[" << result.response->body.size() << " bytes of body]\n";
    return 0;
}

struct LoadedPage {
    std::string bytes;
    net::Url url; // where it landed: the base for the page's references
    std::unique_ptr<ui::ShellLoader> loader; // fetches the page's stylesheets with the same session
    // The page's Content Security Policy, from its headers and then its
    // <meta> elements; every fetch and inline block is judged by it.
    std::unique_ptr<net::ContentSecurityPolicy> policy;
};

// The page's policy from its response headers, its violations named on
// stderr as the console lines they would be.
std::unique_ptr<net::ContentSecurityPolicy> page_policy(net::Url const& url, std::vector<net::Header> const* headers)
{
    auto policy = std::make_unique<net::ContentSecurityPolicy>(url);
    policy->set_reporter([](std::string_view message) { std::cerr << "console.error: " << message << "\n"; });
    if (headers) {
        for (net::Header const& header : *headers) {
            if (ascii_ci_equals(header.name, "content-security-policy"))
                policy->add_header(header.value, false);
            else if (ascii_ci_equals(header.name, "content-security-policy-report-only"))
                policy->add_header(header.value, true);
        }
    }
    return policy;
}

// The policy's say on the page's <style> elements and style attributes,
// for the collector and the resolver.
css::InlineSheetCheck inline_sheet_check(net::ContentSecurityPolicy& policy)
{
    return [&policy](dom::Element const& style, std::string_view text) {
        dom::Attr const* const nonce = style.find_attribute("nonce");
        return !policy.inline_refusal(net::InlineKind::Style, nonce ? nonce->value : std::string(), text);
    };
}

css::StyleAttributeCheck style_attribute_check(net::ContentSecurityPolicy& policy)
{
    return [&policy](dom::Element const&, std::string_view text) {
        return !policy.inline_refusal(net::InlineKind::StyleAttribute, {}, text);
    };
}

// The blocklists folder the render and bench modes' loaders read, set from
// the command line before either runs; empty reads none.
std::string render_blocklists_path;

net::Blocklists load_blocklists(std::string const& path);
ui::Theme load_theme(std::string const& path);

std::unique_ptr<ui::ShellLoader> make_render_loader()
{
    auto loader = std::make_unique<ui::ShellLoader>();
    loader->set_blocklists(load_blocklists(render_blocklists_path));
    return loader;
}

// A --render / --bench input as a URL: one as typed, anything else as a
// local file.
std::optional<net::Url> input_url(std::string const& source)
{
    std::optional<net::Url> url;
    if (source.starts_with("http://") || source.starts_with("https://") || source.starts_with("file:")
        || source.starts_with("data:")) {
        url = net::parse_url(source);
    } else {
        std::error_code error;
        std::string generic = std::filesystem::absolute(source, error).generic_string();
        if (!generic.starts_with("/"))
            generic = "/" + generic;
        url = net::parse_url("file://" + generic);
    }
    if (!url)
        std::cerr << "error: unparseable input " << source << "\n";
    return url;
}

// Why a subresource did not arrive: the fetch error, or the status.
std::string describe_failure(net::FetchResult const& result)
{
    if (!result.response)
        return result.error;
    return "status " + std::to_string(result.response->status);
}

// The page's stylesheets — or, with the kind said, its fonts — fetched
// through its own session under its policy's guard; a failure is named on
// stderr and counted when a counter is given.
css::SheetFetcher sheet_fetcher(LoadedPage const& page, int* failures = nullptr,
    net::ResourceKind kind = net::ResourceKind::Stylesheet)
{
    return [&page, failures, kind](net::Url const& url, std::string_view nonce) -> std::optional<css::FetchedSheet> {
        net::RequestGuard const guard = page.policy ? page.policy->guard(kind, std::string(nonce)) : net::RequestGuard {};
        net::FetchResult result = page.loader->load_subresource(url, page.url, "", kind, guard);
        if (!result.response || result.response->status != 200) {
            std::cerr << (kind == net::ResourceKind::Font ? "font " : "stylesheet ") << url.serialize() << ": "
                      << describe_failure(result) << "\n";
            if (failures)
                ++*failures;
            return std::nullopt;
        }
        std::string const* type = net::find_header(result.response->headers, "content-type");
        return css::FetchedSheet { std::move(result.response->body), type ? *type : "" };
    };
}

// The page's images, fetched through its own session.
ui::ImageFetcher image_fetcher(LoadedPage const& page, int* failures = nullptr)
{
    return [&page, failures](net::Url const& url) -> std::optional<std::vector<std::uint8_t>> {
        net::RequestGuard const guard = page.policy ? page.policy->guard(net::ResourceKind::Image) : net::RequestGuard {};
        net::FetchResult result = page.loader->load_subresource(url, page.url, "", net::ResourceKind::Image, guard);
        if (!result.response || result.response->status != 200) {
            std::cerr << "image " << url.serialize() << ": " << describe_failure(result) << "\n";
            if (failures)
                ++*failures;
            return std::nullopt;
        }
        return std::move(result.response->body);
    };
}

// The page's frames and what their documents fetch, through its own
// session: a frame's document, an object's or an embed's whatever its status,
// which goes with it, as a window shows a server's error page in a frame, and
// anything else only when it arrived.
ui::FrameFetcher frame_fetcher(LoadedPage const& page)
{
    return [&page](net::Url const& url, net::Url const&, net::ResourceKind kind,
               net::RequestGuard const& guard) -> std::optional<ui::FrameResponse> {
        net::FetchResult result = page.loader->load_subresource(url, page.url, "", kind, guard);
        bool const document = kind == net::ResourceKind::Subdocument || kind == net::ResourceKind::Object;
        if (!result.response || (!document && result.response->status != 200)) {
            std::cerr << (document ? "frame " : "frame resource ") << url.serialize() << ": "
                      << describe_failure(result) << "\n";
            return std::nullopt;
        }
        std::string const* const type = net::find_header(result.response->headers, "content-type");
        return ui::FrameResponse { std::move(result.response->body), type ? *type : "", result.response->final_url,
            std::move(result.response->headers), result.response->status };
    };
}

bool starts_with_ci(std::string_view text, std::string_view lowercase_prefix)
{
    if (text.size() < lowercase_prefix.size())
        return false;
    for (std::size_t i = 0; i < lowercase_prefix.size(); ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != lowercase_prefix[i])
            return false;
    }
    return true;
}

std::string host_of(net::Url const& url)
{
    return url.has_host() && !url.host.empty() ? url.serialize_host() : url.serialize();
}

// The document's <title>, its whitespace collapsed.
std::string document_title(dom::Node const& node)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_html("title")) {
            std::string text;
            for (dom::Node const* child : element.children()) {
                if (child->is_text())
                    text += static_cast<dom::Text const*>(child)->data;
            }
            std::string out;
            bool pending_space = false;
            for (char const c : text) {
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
                    pending_space = !out.empty();
                    continue;
                }
                if (pending_space) {
                    out += ' ';
                    pending_space = false;
                }
                out += c;
            }
            return out;
        }
    }
    for (dom::Node const* child : node.children()) {
        std::string title = document_title(*child);
        if (!title.empty())
            return title;
    }
    return {};
}

// A --render load with the shell's answer to whatever went wrong: a fetch
// that failed renders the error page (the certificate page when validation
// failed), a type the engine cannot show renders the text or the
// unsupported-content page — so a picture always results, of what the
// window would show.
struct RenderLoad {
    LoadedPage page;
    std::string outcome = "document"; // document, text, unsupported, certificate-error, error
    int status = 0; // the HTTP status; 0 when nothing answered
    std::string error; // the loader's, when nothing answered
    std::string content_type;
    std::size_t bytes = 0; // of the response body
    double fetch_ms = 0;
};

std::optional<RenderLoad> load_for_render(std::string const& source)
{
    using clock = std::chrono::steady_clock;
    std::optional<net::Url> const url = input_url(source);
    if (!url)
        return std::nullopt;
    RenderLoad load;
    load.page.url = *url;
    load.page.loader = make_render_loader();
    load.page.policy = page_policy(*url, nullptr);
    auto const started = clock::now();
    net::FetchResult result = load.page.loader->load(*url, "", false);
    load.fetch_ms = std::chrono::duration<double, std::milli>(clock::now() - started).count();
    if (!result.response) {
        std::string const host = host_of(*url);
        load.error = result.error;
        if (result.error.find("certificate validation failed") != std::string::npos) {
            load.outcome = "certificate-error";
            load.page.bytes = ui::certificate_error_page(host, url->serialize());
        } else {
            load.outcome = "error";
            load.page.bytes
                = ui::error_page("Sashfold can't reach " + host, result.error, url->serialize());
        }
        std::cerr << "error: " << result.error << "\n";
        return load;
    }
    net::FetchResponse& response = *result.response;
    load.status = response.status;
    load.page.url = response.final_url;
    load.page.policy = page_policy(response.final_url, &response.headers);
    load.bytes = response.body.size();
    if (std::string const* const type = net::find_header(response.headers, "content-type"))
        load.content_type = *type;
    if (url->scheme != "file")
        std::cerr << "fetched " << response.final_url.serialize() << " (" << response.status << ", "
                  << response.body.size() << " bytes)\n";
    std::string_view const type = load.content_type;
    if (type.empty() || starts_with_ci(type, "text/html") || starts_with_ci(type, "application/xhtml")) {
        load.page.bytes.assign(response.body.begin(), response.body.end());
    } else if (starts_with_ci(type, "text/")) {
        load.outcome = "text";
        load.page.bytes = ui::text_page(response.final_url.serialize(), response.body);
    } else {
        load.outcome = "unsupported";
        load.page.bytes = ui::unsupported_content_page(response.final_url.serialize(),
            load.content_type, response.body.size());
    }
    return load;
}

// What --render leaves beside the picture when asked.
// The loader's account of a page's network, by what was fetched, as the
// reports write it: every kind's fetches, what the cache answered, what
// failed, the exchanges and the bytes on the wire, and the milliseconds
// of each step, summed.
std::string census_json(ui::ShellLoader::Census const& network)
{
    auto const kind_json = [](ui::ShellLoader::Census::Kind const& kind) {
        net::FetchTiming const& t = kind.timing;
        std::ostringstream text;
        text << "{ \"fetches\": " << kind.fetches << ", \"cached\": " << kind.cached << ", \"failed\": " << kind.failed
             << ", \"requests\": " << t.requests << ", \"reused\": " << t.reused << ", \"bytes\": " << t.bytes
             << ", \"ms\": { \"resolve\": " << static_cast<long>(t.resolve_ms + 0.5)
             << ", \"connect\": " << static_cast<long>(t.connect_ms + 0.5)
             << ", \"tls\": " << static_cast<long>(t.tls_ms + 0.5)
             << ", \"first_byte\": " << static_cast<long>(t.first_byte_ms + 0.5)
             << ", \"body\": " << static_cast<long>(t.body_ms + 0.5)
             << ", \"total\": " << static_cast<long>(t.total_ms + 0.5) << " } }";
        return text.str();
    };
    std::ostringstream out;
    out << "{ \"total\": " << kind_json(network.total()) << ",\n    \"by_kind\": {\n"
        << "      \"document\": " << kind_json(network.document) << ",\n"
        << "      \"subdocument\": " << kind_json(network.subdocument) << ",\n"
        << "      \"stylesheet\": " << kind_json(network.stylesheet) << ",\n"
        << "      \"script\": " << kind_json(network.script) << ",\n"
        << "      \"image\": " << kind_json(network.image) << ",\n"
        << "      \"font\": " << kind_json(network.font) << ",\n"
        << "      \"xhr\": " << kind_json(network.xhr) << ",\n"
        << "      \"other\": " << kind_json(network.other) << "\n"
        << "    } }";
    return out.str();
}

struct RenderExtras {
    std::string report; // a JSON account of the load and the render
    std::string thumbnail; // a small PNG of the viewport's top
    int thumbnail_width = 320;
    int max_height = 0; // a cap on the picture's height; 0 keeps the page's
    bool dump_layout = false; // print the fragment tree after layout
    bool scripts = true; // run the page's scripts before laying it out
    double script_time_ms = 3000; // how much virtual time the page's timers get
    std::string gaps; // a census of what the page wrote that the engine dropped
};

// --gaps: what a page wrote that the engine dropped, counted while it loads
// and renders: the properties, at-rules and selectors the style system
// skipped, uncaught script errors, custom elements (never upgraded) and tags
// HTML does not define. Written as `kind \t item \t hits`, one row each.
struct GapCensus {
    std::mutex lock;
    std::map<std::pair<std::string, std::string>, long> hits;

    void note(std::string_view kind, std::string_view item)
    {
        std::lock_guard<std::mutex> const guard(lock);
        ++hits[{ std::string(kind), std::string(item) }];
    }
};

// The elements HTML defines, with the obsolete ones it still parses and
// styles; applet, bgsound, blink, isindex, keygen, multicol, nextid and
// spacer are HTMLUnknownElement in the standard and so are left out.
bool html_defines(std::string_view name)
{
    static constexpr std::string_view const names[] = {
        "a", "abbr", "address", "area", "article", "aside", "audio", "b", "base", "bdi", "bdo", "blockquote",
        "body", "br", "button", "canvas", "caption", "cite", "code", "col", "colgroup", "data", "datalist", "dd",
        "del", "details", "dfn", "dialog", "div", "dl", "dt", "em", "embed", "fieldset", "figcaption", "figure",
        "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hgroup", "hr", "html", "i",
        "iframe", "img", "input", "ins", "kbd", "label", "legend", "li", "link", "main", "map", "mark", "menu",
        "meta", "meter", "nav", "noscript", "object", "ol", "optgroup", "option", "output", "p", "picture", "pre",
        "progress", "q", "rp", "rt", "ruby", "s", "samp", "script", "search", "section", "select", "selectedcontent",
        "slot", "small", "source", "span", "strong", "style", "sub", "summary", "sup", "table", "tbody", "td",
        "template", "textarea", "tfoot", "th", "thead", "time", "title", "tr", "track", "u", "ul", "var", "video",
        "wbr", "acronym", "basefont", "big", "center", "dir", "font", "frame", "frameset", "listing", "marquee",
        "menuitem", "nobr", "noembed", "noframes", "param", "plaintext", "rb", "rtc", "strike", "tt", "xmp",
    };
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}

void census_elements(dom::Node const& node, GapCensus& census)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_html()) {
            std::string const& name = element.local_name();
            if (name.find('-') != std::string::npos)
                census.note("html custom element", name);
            else if (!html_defines(name))
                census.note("html unknown element", name);
        }
    }
    for (dom::Node const* child : node.children())
        census_elements(*child, census);
}

bool write_gaps(std::string const& path, GapCensus const& census)
{
    std::ofstream out(path, std::ios::binary);
    for (auto const& [key, count] : census.hits) {
        std::string item;
        for (char const c : key.second)
            item += c == '\t' || c == '\n' || c == '\r' ? ' ' : c;
        out << key.first << '\t' << item << '\t' << count << '\n';
    }
    return static_cast<bool>(out);
}

// The fragment tree as text, one box per line — the instrument for a
// layout question: what box is where, how big, on which baseline.
void dump_fragments(layout::Fragment const& fragment, int depth)
{
    std::string const indent(static_cast<std::size_t>(depth) * 2, ' ');
    std::string name = "anonymous";
    if (fragment.element) {
        name = fragment.element->local_name();
        if (dom::Attr const* id = fragment.element->find_attribute("id"))
            name += "#" + id->value;
        if (dom::Attr const* class_attribute = fragment.element->find_attribute("class"))
            name += "." + class_attribute->value;
    }
    std::string flags;
    if (fragment.floating)
        flags += " float";
    if (fragment.positioned)
        flags += fragment.out_of_flow ? " out-of-flow" : " positioned";
    if (fragment.stacking_context)
        flags += " stacking-context";
    if (fragment.image)
        flags += " image";
    if (fragment.control)
        flags += " control";
    if (fragment.scroll_range_x > 0 || fragment.scroll_range_y > 0) {
        std::ostringstream range;
        range << " scrolls " << fragment.scroll_range_x << "x" << fragment.scroll_range_y;
        flags += range.str();
    }
    std::cout << indent << name << " @ " << fragment.x << "," << fragment.y << " " << fragment.width << "x"
              << fragment.height;
    if (fragment.last_baseline)
        std::cout << " baseline " << *fragment.last_baseline;
    std::cout << flags << "\n";
    for (layout::TextRun const& run : fragment.runs) {
        std::cout << indent << "  \"" << to_utf8(run.text) << "\" @ " << run.x << "," << run.baseline_y
                  << " w " << run.width << "\n";
    }
    for (layout::Fragment const& child : fragment.children)
        dump_fragments(child, depth + 1);
}

std::string json_string(std::string_view text)
{
    std::string out = "\"";
    for (unsigned char const c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                out += buffer;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

std::string utc_now()
{
    std::time_t const now = std::time(nullptr);
    char buffer[32] = {};
    if (std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now)) == 0)
        return "";
    return buffer;
}

void count_text(layout::Fragment const& fragment, std::size_t& runs, std::size_t& characters)
{
    runs += fragment.runs.size();
    for (layout::TextRun const& run : fragment.runs)
        characters += run.text.size();
    for (layout::Fragment const& child : fragment.children)
        count_text(child, runs, characters);
}

long whole_ms(std::chrono::steady_clock::duration duration)
{
    return static_cast<long>(std::chrono::duration<double, std::milli>(duration).count() + 0.5);
}

// What a page's stylesheets ask for that the engine does not do yet, as
// declaration counts per feature — the report's account of why a render
// may look wrong, and the ranking of what to write next by pages asking.
using FeatureCensus = std::map<std::string, long>;

std::string lowercase_ascii(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text)
        out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    return out;
}

void census_values(std::vector<css::ComponentValue> const& values, FeatureCensus& census)
{
    for (css::ComponentValue const& value : values) {
        if (value.is_function()) {
            census_values(value.function().values, census);
        } else if (value.is_block()) {
            census_values(value.block().values, census);
        }
    }
}

void census_declaration(css::Declaration const& declaration, FeatureCensus& census)
{
    std::string const name = lowercase_ascii(declaration.name);
    std::string first_ident;
    for (css::ComponentValue const& value : declaration.value) {
        if (value.is_token(css::Token::Type::Ident) && first_ident.empty())
            first_ident = lowercase_ascii(value.token().value);
    }
    if (name.starts_with("--"))
        return; // custom properties are written; what they hold is counted where it is used
    if (name == "position" || name == "z-index")
        return; // positioning and z-index are written
    if (name == "display" && first_ident == "inline-block")
        return; // inline-block is written
    else if (name == "display" && first_ident == "contents")
        ++census["display-contents"];
    else if (name == "rotate" || name == "scale")
        ++census["transforms"];
    else if (name == "transform") {
        // Translations are drawn; the rest of a transform list is not.
        for (css::ComponentValue const& value : declaration.value) {
            if (!value.is_function())
                continue;
            std::string const function = lowercase_ascii(value.function().name);
            if (!function.starts_with("translate")) {
                ++census["transforms"];
                break;
            }
        }
    }
    else if (name.starts_with("animation") || name.starts_with("transition"))
        ++census["animations"];
    else if (name == "vertical-align")
        return; // vertical-align is written
    else if (name.starts_with("border") && name.find("radius") != std::string::npos)
        return; // rounded corners are drawn
    else if (name == "box-shadow" || name == "text-shadow")
        ++census["shadows"];
    else if (name == "filter" || name == "backdrop-filter" || name == "clip-path" || name == "mask")
        ++census["effects"];
    else if (name == "text-transform")
        return; // text-transform is written
    else if (name == "text-overflow")
        ++census["text-properties"];
    else if (name == "columns" || name == "column-count" || name == "column-width")
        ++census["multi-column"];
    else if (name == "object-fit" || name == "aspect-ratio")
        ++census["sizing"];
    else if (name == "outline" || name.starts_with("outline-"))
        ++census["outline"];
    else if (name == "direction" || name == "writing-mode")
        return; // both are written, and the bidirectional algorithm under them
    if (name != "src")
        census_values(declaration.value, census);
}

void census_rules(std::vector<css::Rule> const& rules, FeatureCensus& census)
{
    for (css::Rule const& rule : rules) {
        if (rule.is_qualified()) {
            for (css::Declaration const& declaration : rule.qualified().declarations)
                census_declaration(declaration, census);
            census_rules(rule.qualified().child_rules, census);
        } else if (rule.is_at_rule()) {
            std::string const name = lowercase_ascii(rule.at_rule().name);
            // A font face's descriptors are no page feature to count: every
            // format its sources name is read.
            if (name == "font-face")
                continue;
            if (name == "supports" || name == "layer" || name == "container" || name == "keyframes" || name == "scope")
                ++census["at-rules"];
            census_rules(rule.at_rule().child_rules, census);
        } else if (rule.is_nested_declarations()) {
            for (css::Declaration const& declaration : rule.nested_declarations().declarations)
                census_declaration(declaration, census);
        }
    }
}

FeatureCensus feature_census(std::vector<css::SheetSource> const& sheets)
{
    FeatureCensus census;
    for (css::SheetSource const& sheet : sheets)
        census_rules(css::parse_stylesheet(sheet.text).rules, census);
    return census;
}

// --scale: the display the headless modes render for, in device px per CSS
// px; the viewport they are given is in device px, as a window's is.
float g_device_scale = 1.0f;

int render_page(std::string const& path, std::string const& output, int viewport_width,
    int viewport_height, RenderExtras const& extras)
{
    using clock = std::chrono::steady_clock;
    auto const started = clock::now();
    std::optional<RenderLoad> const load = load_for_render(path);
    if (!load)
        return 1;
    LoadedPage const& loaded = load->page;
    css::MediaContext const media { static_cast<float>(viewport_width),
        static_cast<float>(viewport_height), g_device_scale };
    auto const t0 = clock::now();
    int sheet_failures = 0;
    int image_failures = 0;
    GapCensus gap_census;
    bool const counting_gaps = !extras.gaps.empty();
    struct GapSinkReset {
        ~GapSinkReset() { css::set_gap_sink(nullptr); }
    } const gap_sink_reset;
    if (counting_gaps)
        css::set_gap_sink([&gap_census](std::string_view kind, std::string_view item) { gap_census.note(kind, item); });
    // The page is parsed with its scripts running. Timers run on a virtual
    // clock afterwards, up to the budget: what the page does in its first
    // seconds, without waiting for them. A script that asks for a box gets
    // the page laid out as it stands.
    auto document = std::make_unique<dom::Document>();
    bindings::LayoutOracle oracle(*document, loaded.url, sheet_fetcher(loaded), media);
    oracle.set_policy(loaded.policy.get());
    std::unique_ptr<bindings::Realm> realm;
    double script_clock = 0;
    if (extras.scripts) {
        bindings::HostHooks hooks;
        hooks.policy = loaded.policy.get();
        hooks.fetch_script = [&loaded](net::Url const& url, net::RequestGuard const& guard) -> std::optional<std::string> {
            net::FetchResult result = loaded.loader->load_subresource(url, loaded.url, "", net::ResourceKind::Script, guard);
            if (!result.response || result.response->status != 200) {
                std::cerr << "script " << url.serialize() << ": " << describe_failure(result) << "\n";
                return std::nullopt;
            }
            return std::string(result.response->body.begin(), result.response->body.end());
        };
        hooks.fetch_resource = [&loaded](net::Url const& url, net::ResourceRequest const& request, net::RequestGuard const& guard) {
            return loaded.loader->load_resource(url, loaded.url, "", request, guard);
        };
        hooks.now = [&script_clock] { return script_clock; };
        hooks.should_stop = [started] { return clock::now() - started > std::chrono::seconds(30); };
        oracle.install(hooks);
        hooks.console = [&gap_census, counting_gaps](std::string_view level, std::string_view message) {
            std::cerr << "console." << level << ": " << message << "\n";
            if (counting_gaps && level == "error" && message.starts_with("Uncaught "))
                gap_census.note("script error", message.substr(9));
        };
        hooks.viewport_width = media.width / g_device_scale;
        hooks.viewport_height = media.height / g_device_scale;
        hooks.device_scale = g_device_scale;
        hooks.user_agent = std::string(net::user_agent());
        hooks.image_decodes = [](std::vector<std::uint8_t> const& bytes) { return ui::decode_image_bytes(bytes).has_value(); };
        // A frame's document gets a realm of its own, by the framing rules the
        // frames are drawn by, unless the page's sandbox keeps scripts off.
        if (loaded.policy->sandbox_allows_scripts()) {
            hooks.frame_document = [&loaded](dom::Element const& iframe, net::Url const& base,
                                       net::ContentSecurityPolicy* policy, std::vector<bindings::FrameAncestor> const& ancestors,
                                       std::optional<net::Url> const& target) {
                return ui::frame_document_for(iframe, base, policy, ancestors, target, frame_fetcher(loaded));
            };
        }
        realm = std::make_unique<bindings::Realm>(*document, loaded.url, std::move(hooks));
        oracle.set_realm(realm.get());
    }
    // A document sandboxed without allow-scripts parses with scripting off.
    html::parse_document_bytes_into(*document, loaded.bytes,
        loaded.policy->sandbox_allows_scripts() ? realm.get() : nullptr);
    if (realm) {
        realm->document_parsed();
        for (int i = 0; i < 200 && realm->has_pending_timers(); ++i) {
            double const due = *realm->next_timer_due();
            if (due > extras.script_time_ms)
                break;
            script_clock = std::max(script_clock, due);
            realm->run_pending();
        }
    }
    auto const t1 = clock::now();
    bindings::adopt_meta_policies(*loaded.policy, *document);
    std::vector<css::SheetSource> sheets = css::collect_stylesheets(*document, &loaded.url,
        sheet_fetcher(loaded, &sheet_failures), media, inline_sheet_check(*loaded.policy));
    if (loaded.loader) {
        if (std::optional<css::SheetSource> hiding = ui::cosmetic_sheet(loaded.loader->blocklists(), loaded.url, *document))
            sheets.push_back(std::move(*hiding));
    }
    std::vector<text::PageFont> const fonts
        = css::collect_page_fonts(sheets, sheet_fetcher(loaded, &sheet_failures, net::ResourceKind::Font), media);
    text::FontManager::instance().set_page_fonts(fonts);
    auto const t2 = clock::now();
    css::StyleSet style_set(sheets, media, &loaded.url);
    style_set.set_style_attribute_check(style_attribute_check(*loaded.policy));
    css::StyleMap const styles = css::resolve_styles(*document, style_set);
    auto const t3 = clock::now();
    // The objects and embeds as the page's realm decided them; without scripts
    // nothing is decided, and each is the replaced box it always was.
    layout::EmbeddedStates const embedded = realm ? bindings::embedded_states(*realm) : layout::EmbeddedStates {};
    layout::ImageMap const images = ui::collect_images(*document, &loaded.url,
        image_fetcher(loaded, &image_failures), media, realm ? &embedded : nullptr);
    layout::BackgroundImages const backgrounds
        = ui::collect_background_images(styles, image_fetcher(loaded, &image_failures));
    auto const t4 = clock::now();
    layout::LayoutResult page = layout::layout_document(*document, styles,
        static_cast<float>(viewport_width), &images, nullptr, static_cast<float>(viewport_height), g_device_scale,
        realm ? &embedded : nullptr);
    // The page's frames: a frame's document under the page's frame-src, what
    // that document fetches under its own policy.
    ui::draw_frames(loaded.url, page, frame_fetcher(loaded), g_device_scale, loaded.policy.get(), nullptr, realm.get());
    text::FontManager::instance().set_page_fonts(fonts);
    auto const t5 = clock::now();
    if (extras.dump_layout) {
        std::cout << "layout " << viewport_width << "x" << viewport_height << ", page height "
                  << page.page_height << "\n";
        dump_fragments(page.root, 0);
    }

    int height = std::max(1, static_cast<int>(page.page_height + 0.5f));
    if (extras.max_height > 0)
        height = std::min(height, extras.max_height);
    Bitmap canvas(viewport_width, height, page.canvas_background);
    paint::paint_page(canvas, page, 0, 0, &backgrounds);
    auto const t6 = clock::now();
    if (!write_png(output, canvas)) {
        std::cerr << "error: could not write " << output << "\n";
        return 1;
    }
    std::cout << "wrote " << output << " (" << canvas.width() << "x" << canvas.height() << ")\n";

    if (!extras.thumbnail.empty()) {
        // The viewport's top, as the window would first show it, scaled
        // down; a page shorter than the viewport leaves its canvas color.
        Bitmap view(viewport_width, viewport_height, page.canvas_background);
        view.blit(canvas, 0, 0);
        int const thumb_width = std::max(16, extras.thumbnail_width);
        int const thumb_height = std::max(1, thumb_width * viewport_height / viewport_width);
        Bitmap thumb(thumb_width, thumb_height, page.canvas_background);
        thumb.draw_scaled(view, Rect { 0, 0, thumb_width, thumb_height });
        if (!write_png(extras.thumbnail, thumb)) {
            std::cerr << "error: could not write " << extras.thumbnail << "\n";
            return 1;
        }
    }
    if (!extras.report.empty()) {
        std::size_t runs = 0;
        std::size_t characters = 0;
        count_text(page.root, runs, characters);
        net::ConnectionPool::Stats const& connections = loaded.loader->pool().stats();
        std::ofstream out(extras.report, std::ios::binary);
        out << "{\n"
            << "  \"input\": " << json_string(path) << ",\n"
            << "  \"url\": " << json_string(loaded.url.serialize()) << ",\n"
            << "  \"outcome\": " << json_string(load->outcome) << ",\n"
            << "  \"status\": " << load->status << ",\n"
            << "  \"error\": " << json_string(load->error) << ",\n"
            << "  \"content_type\": " << json_string(load->content_type) << ",\n"
            << "  \"bytes\": " << load->bytes << ",\n"
            << "  \"title\": " << json_string(document_title(*document)) << ",\n"
            << "  \"rendered\": " << json_string(utc_now()) << ",\n"
            << "  \"viewport\": { \"width\": " << viewport_width << ", \"height\": " << viewport_height
            << " },\n"
            << "  \"page_height\": " << static_cast<int>(page.page_height + 0.5f) << ",\n"
            << "  \"picture\": { \"width\": " << canvas.width() << ", \"height\": " << canvas.height()
            << " },\n"
            << "  \"text\": { \"runs\": " << runs << ", \"characters\": " << characters << " },\n"
            << "  \"stylesheets\": { \"count\": " << sheets.size() << ", \"failed\": " << sheet_failures
            << " },\n"
            << "  \"images\": { \"count\": " << images.size() << ", \"failed\": " << image_failures
            << " },\n"
            << "  \"fonts\": " << fonts.size() << ",\n"
            << "  \"blocked\": " << loaded.loader->blocked_requests() << ",\n"
            << "  \"csp\": { \"policies\": " << loaded.policy->policies().size() << ", \"refused\": " << loaded.policy->refusals() << " },\n";
        if (realm) {
            bindings::ScriptStats const& scripts = realm->stats();
            out << "  \"scripts\": { \"run\": " << scripts.scripts_run << ", \"modules\": " << scripts.modules_run << ", \"failed\": " << scripts.scripts_failed
                << ", \"skipped\": " << scripts.scripts_skipped << ", \"external\": " << scripts.external_fetched
                << ", \"external_failed\": " << scripts.external_failed << ", \"timers\": " << scripts.timers_fired
                << ", \"events\": " << scripts.events_dispatched << ", \"errors\": " << scripts.uncaught_errors
                << ", \"ms\": " << static_cast<long>(scripts.script_ms + 0.5) << " },\n";
        }
        out
            << "  \"connections\": { \"opened\": " << connections.opened << ", \"reused\": "
            << connections.reused << ", \"retried\": " << connections.retried << " },\n";
        // Where the page's network time went, by what was fetched: the
        // loader's account of every fetch it made for this page.
        out << "  \"network\": " << census_json(loaded.loader->census()) << ",\n";
        FeatureCensus const census = feature_census(sheets);
        out << "  \"asks\": {";
        bool first_ask = true;
        for (auto const& [feature, count] : census) {
            out << (first_ask ? " " : ", ") << json_string(feature) << ": " << count;
            first_ask = false;
        }
        out << (first_ask ? "" : " ") << "},\n"
            << "  \"ms\": { \"fetch\": " << static_cast<long>(load->fetch_ms + 0.5)
            << ", \"parse\": " << whole_ms(t1 - t0) << ", \"stylesheets\": " << whole_ms(t2 - t1)
            << ", \"style\": " << whole_ms(t3 - t2) << ", \"images\": " << whole_ms(t4 - t3)
            << ", \"layout\": " << whole_ms(t5 - t4) << ", \"paint\": " << whole_ms(t6 - t5)
            << ", \"total\": " << whole_ms(t6 - started) << " }\n"
            << "}\n";
        if (!out) {
            std::cerr << "error: could not write " << extras.report << "\n";
            return 1;
        }
    }
    if (counting_gaps) {
        census_elements(*document, gap_census);
        if (!write_gaps(extras.gaps, gap_census)) {
            std::cerr << "error: could not write " << extras.gaps << "\n";
            return 1;
        }
    }
    return 0;
}

int dump_dom(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "error: cannot read " << path << "\n";
        return 1;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    auto document = html::parse_document_bytes(std::move(stream).str());
    std::cout << html::dump_document(*document);
    return 0;
}

// Renders every glyph of Sashfold Mono at several sizes — the standing QA
// sheet for the face.
// The QA sheet for a face: the built-in one by default, or any TrueType
// file, drawn through the same Face interface the page renderer uses. Code
// points the file lacks fall back to the built-in face, so a gap shows as
// a Sashfold Mono glyph rather than nothing.
int font_sampler(std::string const& output, std::string const& font_path)
{
    std::unique_ptr<text::Face> loaded;
    if (!font_path.empty()) {
        std::ifstream file(font_path, std::ios::binary);
        std::vector<std::uint8_t> const bytes((std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        std::optional<text::TrueTypeFont> parsed = text::TrueTypeFont::parse(bytes);
        if (!parsed) {
            std::cerr << "error: cannot read " << font_path << " as a TrueType font\n";
            return 1;
        }
        loaded = text::make_truetype_face(std::move(*parsed));
    }
    text::Face const& builtin = text::builtin_face();
    text::Face const& font = loaded ? *loaded : builtin;
    auto const draw = [&](Bitmap& canvas, char32_t c, float x, float y, float size, bool bold,
                          bool italic) {
        std::uint32_t const glyph = font.glyph_index(c);
        if (glyph != 0) {
            font.draw_glyph(canvas, glyph, x, y, size, Color::rgb(20, 20, 24), bold, italic);
            return font.advance(glyph, size);
        }
        builtin.draw_glyph(canvas, c, x, y, size, Color::rgb(20, 20, 24), bold, italic);
        return builtin.advance(c, size);
    };
    Bitmap canvas(980, 760, Color::rgb(252, 252, 250));
    std::u32string const rows[] = {
        U"ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        U"abcdefghijklmnopqrstuvwxyz",
        U"0123456789 !\"#$%&'()*+,-./",
        U":;<=>?@[\\]^_`{|}~ •–—‘’“”…�",
        U"← → ↑ ↓ ↔ ↻ × ✓ † ‡ ™ chrome glyphs",
        U"café naïve Straße Ångström Ærø œuvre Łódź Đại þorn Việt ếch ÉCOLE ÑANDÚ",
        U"© ® · ° ± ÷ « » ‹ › ¡ ¿ £ € ¢ ¥ § ¶ µ ¬ ² ³ ¹ ½ ¼ ¾ ¤ ¦ ª º Ｆｕｌｌ",
        U"The quick brown fox jumps over the lazy dog.",
        U"int main() { return \"hi\"; } /* 0xFF */",
    };
    float y = 40;
    for (float size : { 16.0f, 24.0f }) {
        for (std::u32string const& row : rows) {
            float x = 16;
            for (char32_t const c : row)
                x += draw(canvas, c, x, y, size, false, false);
            y += size * 1.4f;
        }
        y += 12;
    }
    // Bold and italic rows.
    for (int variant = 0; variant < 3; ++variant) {
        std::u32string const sample = U"Weight and slant: Hamburgefonstiv 017";
        float x = 16;
        for (char32_t const c : sample)
            x += draw(canvas, c, x, y, 22.0f, variant == 1, variant == 2);
        y += 34;
    }
    if (!write_png(output, canvas)) {
        std::cerr << "error: could not write " << output << "\n";
        return 1;
    }
    std::cout << "wrote " << output << "\n";
    return 0;
}

// Exercises the paint path end to end: opaque fills, clipping at every edge,
// and alpha compositing over both opaque and transparent ground.
int smoke_scene(std::string const& output)
{
    Bitmap canvas(320, 200, Color::rgb(250, 250, 248));
    canvas.fill_rect(Rect { 0, 0, 320, 44 }, Color::rgb(32, 38, 52));
    canvas.fill_rect(Rect { 16, 68, 120, 90 }, Color::rgb(214, 84, 72));
    canvas.fill_rect(Rect { 96, 100, 120, 90 }, Color::rgba(60, 120, 216, 128));
    canvas.fill_rect(Rect { -20, 168, 80, 60 }, Color::rgb(96, 176, 120));
    canvas.fill_rect(Rect { 268, -10, 80, 40 }, Color::rgba(240, 200, 64, 200));
    canvas.fill_round_rect(Rect { 200, 60, 100, 60 }, 14, Color::rgb(60, 60, 70));
    if (!write_png(output, canvas)) {
        std::cerr << "error: could not write " << output << "\n";
        return 1;
    }
    std::cout << "wrote " << output << " (" << canvas.width() << "x" << canvas.height() << ")\n";
    return 0;
}

// The engine's stages timed separately, best and median of several runs,
// painting a viewport-sized slice the way the shell does each frame. The
// perf budgets are checked against these numbers.
// The shell's counters from one moment on: what it had done before is
// taken off, so a page's own work stands alone.
ui::Profile profile_since(ui::Profile const& now, ui::Profile const& base)
{
    ui::Profile d = now;
    d.restyles -= base.restyles;
    d.relayouts -= base.relayouts;
    d.paints -= base.paints;
    d.painted_pixels -= base.painted_pixels;
    d.sheets_ms -= base.sheets_ms;
    d.images_ms -= base.images_ms;
    d.restyle_ms -= base.restyle_ms;
    d.relayout_ms -= base.relayout_ms;
    d.frames_ms -= base.frames_ms;
    d.paint_ms -= base.paint_ms;
    return d;
}

// The shell's counters as the reports write them.
std::string profile_json(ui::Profile const& p)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << "{ \"restyles\": " << p.restyles << ", \"relayouts\": " << p.relayouts
        << ", \"paints\": " << p.paints << ", \"painted_pixels\": " << p.painted_pixels << ", \"ms\": { \"sheets\": " << p.sheets_ms
        << ", \"images\": " << p.images_ms << ", \"restyle\": " << p.restyle_ms << ", \"relayout\": " << p.relayout_ms
        << ", \"frames\": " << p.frames_ms << ", \"paint\": " << p.paint_ms << " } }";
    return out.str();
}

// --bench: a whole session on a page, the way the window runs it, timed —
// the moment the page is first on the frame, the moment its scripts have
// had their time and its pictures are on it too, what each phase of the
// shell's work cost, what a wheel notch costs to scroll and paint at the
// window's size, and the memory the page took — written as one JSON in
// the shape of --render's report, with the loader's account of the
// network beside it. Each run is a fresh session, since the cache is the
// loader's and a second run on it is not a first; the summary takes the
// median over the runs. The JSON goes to --report's file, else after the
// summary on stdout.
int bench(std::string const& input, int runs, int viewport_width, int viewport_height, std::string const& report_path,
    std::string const& theme_path, std::string const& blocklists_path, std::string const& downloads)
{
    using clock = std::chrono::steady_clock;
    using ms = std::chrono::duration<double, std::milli>;
    constexpr double script_time_ms = 3000; // the timers' virtual time, as --render gives it
    constexpr int scroll_notches = 20;
    std::optional<net::Url> const url = input_url(input);
    if (!url)
        return 1;
    struct Run {
        double first_paint_ms = 0;
        double pixels_ms = 0;
        bool scrolled = false;
        std::vector<double> scroll_ms; // a notch and the frame after it, each
        std::vector<double> scroll_paint_ms; // the frame alone
        ui::Profile profile;
        ui::ShellLoader::Census network;
        std::size_t rss_before = 0;
        std::size_t rss_after = 0;
        std::string title;
        std::string final_url;
        int status = 0;
    };
    auto const median = [](std::vector<double> values) {
        if (values.empty())
            return 0.0;
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    auto const largest = [](std::vector<double> const& values) {
        return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
    };
    net::Blocklists const lists = load_blocklists(blocklists_path);
    ui::Theme const theme = load_theme(theme_path);
    std::vector<Run> results;
    for (int i = 0; i < std::max(1, runs); ++i) {
        Run run;
        run.rss_before = platform::resident_set_bytes();
        ui::ShellLoader loader;
        loader.set_blocklists(lists);
        ui::Browser browser(loader, theme, viewport_width, viewport_height);
        browser.set_downloads_directory(downloads);
        double clock_ms = 0; // the pages' clock, virtual: the timers run when it says
        browser.set_clock([&clock_ms] { return clock_ms; });
        browser.frame(); // the new-tab page, before the clock starts
        ui::Profile const base = browser.profile();
        auto const settle = [&browser] {
            for (int round = 0; round < 4; ++round) {
                while (browser.has_pending_load())
                    browser.tick();
                browser.run_scripts();
                if (!browser.has_pending_load())
                    break;
            }
        };
        auto const started = clock::now();
        browser.open(*url);
        browser.tick(); // the navigation: the document, and whatever the shell fetches before it shows a page
        browser.frame();
        run.first_paint_ms = ms(clock::now() - started).count();
        settle();
        // The timers, as --render gives them: each due one runs in turn
        // until the page's virtual time is spent.
        for (int step = 0; step < 200; ++step) {
            std::optional<double> const due = browser.next_timer_ms();
            if (!due || *due > script_time_ms)
                break;
            clock_ms = std::max(clock_ms, *due);
            settle();
        }
        browser.frame();
        run.pixels_ms = ms(clock::now() - started).count();
        run.rss_after = platform::resident_set_bytes();
        // A wheel notch at the content's center, and the frame after it,
        // twenty times: what a reader's scroll costs today.
        Rect const content = browser.chrome_layout().content;
        int const cx = content.x + content.width / 2;
        int const cy = content.y + content.height / 2;
        for (int notch = 0; notch < scroll_notches; ++notch) {
            std::uint64_t const painted = browser.profile().paints;
            auto const t = clock::now();
            browser.wheel(cx, cy, -1);
            browser.frame();
            run.scroll_ms.push_back(ms(clock::now() - t).count());
            run.scroll_paint_ms.push_back(browser.profile().paints > painted ? browser.profile().last_paint_ms : 0.0);
        }
        run.scrolled = browser.scroll_y() > 0;
        run.profile = profile_since(browser.profile(), base);
        run.network = loader.census();
        run.title = browser.page_title();
        if (ui::HistoryEntry const* const entry = browser.current_entry()) {
            run.final_url = entry->final_url.serialize();
            run.status = entry->status;
        }
        results.push_back(std::move(run));
    }

    std::vector<double> first_paints;
    std::vector<double> pixels;
    std::vector<double> scroll_frames;
    std::vector<double> scroll_paints;
    std::vector<double> rss_deltas;
    std::vector<double> network_totals;
    for (Run const& run : results) {
        first_paints.push_back(run.first_paint_ms);
        pixels.push_back(run.pixels_ms);
        scroll_frames.push_back(median(run.scroll_ms));
        scroll_paints.push_back(median(run.scroll_paint_ms));
        rss_deltas.push_back(static_cast<double>(run.rss_after) - static_cast<double>(run.rss_before));
        network_totals.push_back(run.network.total().timing.total_ms);
    }
    Run const& first = results.front();
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    out << "{\n"
        << "  \"input\": " << json_string(input) << ",\n"
        << "  \"url\": " << json_string(first.final_url) << ",\n"
        << "  \"title\": " << json_string(first.title) << ",\n"
        << "  \"status\": " << first.status << ",\n"
        << "  \"rendered\": " << json_string(utc_now()) << ",\n"
        << "  \"viewport\": { \"width\": " << viewport_width << ", \"height\": " << viewport_height << " },\n"
        << "  \"runs\": " << results.size() << ",\n"
        << "  \"script_time_ms\": " << static_cast<long>(script_time_ms) << ",\n"
        << "  \"scroll_notches\": " << scroll_notches << ",\n"
        << "  \"median\": { \"first_paint_ms\": " << median(first_paints) << ", \"pixels_ms\": " << median(pixels)
        << ", \"scroll_frame_ms\": " << median(scroll_frames) << ", \"scroll_paint_ms\": " << median(scroll_paints)
        << ", \"network_ms\": " << median(network_totals) << ", \"rss_delta_bytes\": " << static_cast<long long>(median(rss_deltas))
        << " },\n"
        << "  \"run_details\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        Run const& run = results[i];
        out << "    { \"first_paint_ms\": " << run.first_paint_ms << ", \"pixels_ms\": " << run.pixels_ms
            << ", \"scrolled\": " << (run.scrolled ? "true" : "false")
            << ", \"scroll\": { \"frame_ms\": { \"median\": " << median(run.scroll_ms) << ", \"max\": " << largest(run.scroll_ms)
            << " }, \"paint_ms\": { \"median\": " << median(run.scroll_paint_ms) << ", \"max\": " << largest(run.scroll_paint_ms) << " } },\n"
            << "      \"shell\": " << profile_json(run.profile) << ",\n"
            << "      \"rss_bytes\": { \"before\": " << run.rss_before << ", \"after\": " << run.rss_after << " },\n"
            << "      \"network\": " << census_json(run.network) << " }" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    std::string const json = out.str();
    if (!report_path.empty()) {
        std::ofstream file(report_path, std::ios::binary);
        file << json;
        if (!file) {
            std::cerr << "error: could not write " << report_path << "\n";
            return 1;
        }
    }
    ui::ShellLoader::Census::Kind const network = first.network.total();
    std::printf("bench: %s at %dx%d, %zu run(s): first paint %.0f ms, pixels %.0f ms, scroll frame %.1f ms "
                "(paint %.1f ms), RSS %+.1f MB — medians\n",
        first.final_url.c_str(), viewport_width, viewport_height, results.size(), median(first_paints), median(pixels),
        median(scroll_frames), median(scroll_paints), median(rss_deltas) / (1024.0 * 1024.0));
    std::printf("  first run: network %.0f ms over %d request(s) (document %.0f, sheets %.0f, scripts %.0f, images %.0f, fonts %.0f, "
                "xhr %.0f); shell sheets %.0f ms, images %.0f, style %.0f (%llu), layout %.0f (%llu), frames %.0f, paint %.0f (%llu)\n",
        network.timing.total_ms, network.timing.requests, first.network.document.timing.total_ms,
        first.network.stylesheet.timing.total_ms, first.network.script.timing.total_ms, first.network.image.timing.total_ms,
        first.network.font.timing.total_ms, first.network.xhr.timing.total_ms, first.profile.sheets_ms, first.profile.images_ms,
        first.profile.restyle_ms, static_cast<unsigned long long>(first.profile.restyles), first.profile.relayout_ms,
        static_cast<unsigned long long>(first.profile.relayouts), first.profile.frames_ms, first.profile.paint_ms,
        static_cast<unsigned long long>(first.profile.paints));
    if (report_path.empty())
        std::fputs(json.c_str(), stdout);
    return 0;
}

// The profile folder the window keeps its session in: the platform's
// configuration directory ($XDG_CONFIG_HOME, else ~/.config, on Linux and
// macOS; %APPDATA% on Windows) plus the program's name, made when
// missing. Empty when neither a home nor the folder can be had.
std::string default_profile_directory()
{
    std::filesystem::path root;
#ifdef _WIN32
    if (char const* const appdata = std::getenv("APPDATA"); appdata && *appdata) {
        root = appdata;
    } else if (char const* const home = std::getenv("USERPROFILE"); home && *home) {
        root = std::filesystem::path(home) / "AppData" / "Roaming";
    } else {
        return {};
    }
    std::filesystem::path const directory = root / "Sashfold";
#else
    if (char const* const xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        root = xdg;
    } else if (char const* const home = std::getenv("HOME"); home && *home) {
        root = std::filesystem::path(home) / ".config";
    } else {
        return {};
    }
    std::filesystem::path const directory = root / "sashfold";
#endif
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return std::filesystem::is_directory(directory, error) ? directory.string() : std::string();
}

std::optional<std::string> read_text_file(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::ostringstream text;
    text << file.rdbuf();
    return std::move(text).str();
}

// Written whole or not at all: to a file beside it, then renamed over it.
bool write_text_file_atomically(std::filesystem::path const& path, std::string const& text)
{
    std::filesystem::path const temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << text;
        if (!file)
            return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

// The containers the window offers, from the profile's containers.json:
// [{"name": "Work", "color": "#f59e0b"}, …], in order. A profile without
// the file gets one with four containers to start from.
std::vector<ui::Browser::Container> containers_from(std::string_view text)
{
    std::vector<ui::Browser::Container> containers;
    std::optional<JsonValue> const parsed = JsonValue::parse(text);
    if (!parsed || !parsed->is_array())
        return containers;
    for (JsonValue const& value : parsed->as_array()) {
        JsonValue const* const name = value.is_object() ? value.get("name") : nullptr;
        if (!name || !name->is_string() || name->as_string().empty())
            continue;
        bool duplicate = false;
        for (ui::Browser::Container const& existing : containers)
            duplicate = duplicate || existing.name == name->as_string();
        if (duplicate)
            continue;
        ui::Browser::Container container;
        container.name = name->as_string();
        container.color = Color::rgb(0x8f, 0x96, 0xa3);
        if (JsonValue const* const color = value.get("color"); color && color->is_string()) {
            if (std::optional<Color> const parsed_color = ui::parse_theme_color(color->as_string()))
                container.color = *parsed_color;
        }
        containers.push_back(std::move(container));
    }
    return containers;
}

std::vector<ui::Browser::Container> load_containers(std::filesystem::path const& path)
{
    if (std::optional<std::string> const text = read_text_file(path))
        return containers_from(*text);
    static constexpr char const* defaults = "[\n"
                                            "  {\"name\": \"Personal\", \"color\": \"#3b82f6\"},\n"
                                            "  {\"name\": \"Work\", \"color\": \"#f59e0b\"},\n"
                                            "  {\"name\": \"Banking\", \"color\": \"#22c55e\"},\n"
                                            "  {\"name\": \"Shopping\", \"color\": \"#ec4899\"}\n"
                                            "]\n";
    write_text_file_atomically(path, defaults);
    return containers_from(defaults);
}

// A container's cookie file in the profile: cookies.txt for the default,
// cookies-<name>.txt for the rest, the name held to letters, digits,
// dashes and underscores.
std::filesystem::path cookie_file(std::filesystem::path const& profile, std::string_view container)
{
    if (container.empty())
        return profile / "cookies.txt";
    std::string safe;
    for (char const c : container)
        safe += is_ascii_alphanumeric(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
    return profile / ("cookies-" + safe + ".txt");
}

std::int64_t unix_seconds_now()
{
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// The user's Downloads folder, when the OS has the convention and it exists.
std::string default_downloads_directory()
{
#ifdef _WIN32
    char const* const home = std::getenv("USERPROFILE");
#else
    char const* const home = std::getenv("HOME");
#endif
    if (!home || !*home)
        return {};
    std::error_code error;
    std::filesystem::path const downloads = std::filesystem::path(home) / "Downloads";
    return std::filesystem::is_directory(downloads, error) ? downloads.string() : std::string();
}

// themes/default.json beside the executable, or beside its parent directory
// (a build tree inside the repository), else the built-in defaults.
// A file that ships beside the executable or one directory up (a build
// tree's parent is the repository): themes/default.json, assets/icon.png.
std::string shipped_file_path(char const* program, std::filesystem::path const& relative)
{
    std::error_code error;
    std::filesystem::path const exe = std::filesystem::absolute(program, error);
    if (error)
        return {};
    std::filesystem::path const dir = exe.parent_path();
    for (std::filesystem::path const& base : { dir, dir.parent_path() }) {
        std::filesystem::path const candidate = base / relative;
        if (std::filesystem::exists(candidate, error))
            return candidate.string();
    }
    return {};
}

std::string default_theme_path(char const* program)
{
    return shipped_file_path(program, std::filesystem::path("themes") / "default.json");
}

// The window's icon, for the OSes that take one from the client.
std::optional<Bitmap> load_window_icon(char const* program)
{
    std::string const path = shipped_file_path(program, std::filesystem::path("assets") / "icon.png");
    if (path.empty())
        return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> const bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return decode_png(bytes);
}

// The themes the palette offers: every .json beside the theme file that
// parses as one, by the name it gives itself, in name order.
std::vector<ui::Browser::ThemePreset> theme_presets_beside(std::string const& theme_path)
{
    std::vector<ui::Browser::ThemePreset> presets;
    if (theme_path.empty())
        return presets;
    std::error_code error;
    std::filesystem::path const directory = std::filesystem::path(theme_path).parent_path();
    for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(directory, error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".json")
            continue;
        std::vector<std::string> problems;
        std::optional<ui::Theme> const theme = ui::Theme::load(entry.path().string(), &problems);
        if (!theme || !problems.empty() || theme->name.empty())
            continue;
        presets.push_back({ theme->name, entry.path().string() });
    }
    std::sort(presets.begin(), presets.end(),
        [](ui::Browser::ThemePreset const& a, ui::Browser::ThemePreset const& b) { return a.name < b.name; });
    return presets;
}

// The reader's own themes: a folder in the profile. A theme file of ours
// put there is offered beside the shipped ones; a Firefox or a Chrome theme
// put there — its folder, its .xpi, its .crx — is converted into one, in
// `converted/` under it, and offered the same way.
std::string user_themes_directory(std::string const& profile)
{
    return profile.empty() ? std::string() : (std::filesystem::path(profile) / "themes").string();
}

// Converts what has been put in the folder and has no conversion as new as
// itself, and says what it did. Whether anything was converted.
bool convert_dropped_themes(std::string const& directory)
{
    if (directory.empty())
        return false;
    std::error_code error;
    std::filesystem::path const folder(directory);
    std::filesystem::path const converted = folder / "converted";
    bool any = false;
    for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.path() == converted || !ui::is_browser_theme_path(entry.path().string()))
            continue;
        // Done before, and the source not touched since: the stamp beside
        // the conversions holds the source's time.
        std::filesystem::path const stamp = converted / (entry.path().filename().string() + ".stamp");
        std::filesystem::file_time_type const changed = std::filesystem::last_write_time(entry.path(), error);
        std::optional<std::string> const stamped = read_text_file(stamp);
        std::string const now = std::to_string(changed.time_since_epoch().count());
        if (stamped && *stamped == now)
            continue;
        std::vector<std::string> problems;
        std::optional<ui::ImportedTheme> const theme = ui::import_browser_theme_from(entry.path().string(), &problems);
        std::optional<std::string> const written
            = theme ? ui::write_imported_theme(*theme, converted.string(), &problems) : std::nullopt;
        for (std::string const& problem : problems)
            std::cerr << problem << "\n";
        if (written) {
            std::cerr << "theme: " << entry.path().filename().string() << " converted: " << *written << "\n";
            any = true;
        }
        // Stamped either way: what cannot be converted is not tried again
        // every time the folder is looked at.
        std::filesystem::create_directories(converted, error);
        write_text_file_atomically(stamp, now);
    }
    return any;
}

// The themes offered: the shipped ones, and the reader's own — the theme
// files in their folder and the conversions under it.
std::vector<ui::Browser::ThemePreset> all_theme_presets(std::string const& theme_path, std::string const& user_directory)
{
    std::vector<ui::Browser::ThemePreset> presets = theme_presets_beside(theme_path);
    auto const offer = [&](std::filesystem::path const& file) {
        std::vector<std::string> problems;
        std::optional<ui::Theme> const theme = ui::Theme::load(file.string(), &problems);
        if (!theme || !problems.empty() || theme->name.empty())
            return;
        for (ui::Browser::ThemePreset const& preset : presets) {
            if (preset.name == theme->name)
                return; // the first of a name stands
        }
        presets.push_back({ theme->name, file.string() });
    };
    if (!user_directory.empty()) {
        std::error_code error;
        std::filesystem::path const folder(user_directory);
        for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(folder, error)) {
            if (entry.is_regular_file(error) && entry.path().extension() == ".json")
                offer(entry.path());
        }
        for (std::filesystem::directory_entry const& entry :
            std::filesystem::directory_iterator(folder / "converted", error)) {
            if (entry.is_directory(error))
                offer(entry.path() / "theme.json");
        }
    }
    std::sort(presets.begin(), presets.end(),
        [](ui::Browser::ThemePreset const& a, ui::Browser::ThemePreset const& b) { return a.name < b.name; });
    return presets;
}

// --import-theme: a Firefox or Chrome theme converted into `directory`, the
// theme file's path on stdout and what could not be carried over on stderr.
int import_theme(std::string const& path, std::string const& directory)
{
    std::vector<std::string> problems;
    std::optional<ui::ImportedTheme> const theme = ui::import_browser_theme_from(path, &problems);
    std::optional<std::string> const written
        = theme && !directory.empty() ? ui::write_imported_theme(*theme, directory, &problems) : std::nullopt;
    for (std::string const& problem : problems)
        std::cerr << problem << "\n";
    if (!written) {
        std::cerr << "error: no theme was converted\n";
        return 1;
    }
    for (std::string const& note : theme->notes)
        std::cerr << "not carried over: " << note << "\n";
    std::cout << *written << "\n";
    return 0;
}

ui::Theme load_theme(std::string const& path)
{
    if (path.empty())
        return ui::Theme {};
    std::vector<std::string> problems;
    std::optional<ui::Theme> const theme = ui::Theme::load(path, &problems);
    for (std::string const& problem : problems)
        std::cerr << problem << "\n";
    return theme.value_or(ui::Theme {});
}

// What kept a theme's pictures from being put on: a file that is not there
// or is no picture is left out, the theme stands, and this says which.
void report_theme_pictures(ui::Browser const& browser)
{
    for (std::string const& problem : browser.theme_problems())
        std::cerr << problem << "\n";
}

// The blocklists folder: `--blocklists`, else blocklists/ beside the
// executable or its parent (the repository's, which ships no lists).
std::string default_blocklists_path(char const* program)
{
    return shipped_file_path(program, std::filesystem::path("blocklists"));
}

net::Blocklists load_blocklists(std::string const& path)
{
    net::Blocklists lists;
    if (path.empty())
        return lists;
    std::vector<std::string> problems;
    lists.load_directory(path, &problems);
    for (std::string const& problem : problems)
        std::cerr << problem << "\n";
    return lists;
}

int run_script_mode(std::string const& script, bool update_goldens, int width, int height,
    std::string const& theme_path, std::string const& blocklists_path, std::string const& downloads)
{
    ui::ShellLoader loader;
    loader.set_blocklists(load_blocklists(blocklists_path));
    ui::Browser browser(loader, load_theme(theme_path), width, height);
    report_theme_pictures(browser);
    platform::use_process_clipboard(true); // a script never touches the real clipboard
    browser.set_downloads_directory(downloads);
    // The four containers a fresh profile gets, so a script can open tabs
    // in them and step through them the way the window does.
    browser.set_containers(containers_from(
        "[{\"name\": \"Personal\", \"color\": \"#3b82f6\"}, {\"name\": \"Work\", \"color\": \"#f59e0b\"}, "
        "{\"name\": \"Banking\", \"color\": \"#22c55e\"}, {\"name\": \"Shopping\", \"color\": \"#ec4899\"}]"));
    browser.set_theme_presets(theme_presets_beside(theme_path));
    ui::ScriptResult const result = ui::run_script(browser, script, update_goldens, std::cout);
    return result.ok() ? 0 : 1;
}

// A headless run of the window — under a compositor with no screen — ends
// itself after `exit_after_ms` and, with `timings_path`, writes what the
// real window measured: when the start page was first presented, when
// nothing was left to load, what every present cost, the shell's counters
// and the loader's account, in --bench's terms so the two compare.
int run_window(std::string const& start_url, std::string const& theme_path,
    std::string const& blocklists_path, std::string const& downloads, std::string const& profile,
    char const* program, int exit_after_ms, std::string const& timings_path)
{
    std::optional<Bitmap> const icon = load_window_icon(program);
    std::unique_ptr<platform::Window> window
        = platform::Window::create("Sashfold", 1100, 760, icon ? &*icon : nullptr);
    if (!window) {
        std::cerr << "error: could not open a window (the AppKit shell is not written; on Linux the\n"
                     "       Wayland display must be reachable); --render, --fetch, and --script work everywhere\n";
        return 1;
    }
    ui::ShellLoader loader;
    loader.set_blocklists(load_blocklists(blocklists_path));
    // The theme: the file the reader last chose from the palette, kept in
    // the profile's settings.json, else the shipped one; the palette's
    // presets are the files beside the shipped one either way.
    std::filesystem::path const profile_path = profile.empty() ? std::filesystem::path() : std::filesystem::path(profile);
    std::error_code error;
    std::string theme_file = theme_path;
    if (!profile_path.empty()) {
        if (std::optional<std::string> const settings = read_text_file(profile_path / "settings.json")) {
            if (std::optional<JsonValue> const parsed = JsonValue::parse(*settings); parsed && parsed->is_object()) {
                if (JsonValue const* const chosen = parsed->get("theme");
                    chosen && chosen->is_string() && std::filesystem::is_regular_file(chosen->as_string(), error))
                    theme_file = chosen->as_string();
            }
        }
    }
    ui::Browser browser(loader, load_theme(theme_file), window->width(), window->height());
    report_theme_pictures(browser);
    browser.set_scale(window->scale());
    browser.set_downloads_directory(downloads);
    // The reader's own themes join the shipped ones: what is in the
    // profile's themes folder, a Firefox or Chrome theme dropped there
    // converted first.
    std::string const user_themes = user_themes_directory(profile);
    if (!user_themes.empty())
        std::filesystem::create_directories(user_themes, error);
    convert_dropped_themes(user_themes);
    browser.set_theme_presets(all_theme_presets(theme_path, user_themes));
    // The cache lives in the profile too: what was fetched last time is
    // there, and a page that has not changed costs a conditional request.
    if (!profile_path.empty())
        loader.cache().set_directory((profile_path / "cache").string());

    // The profile: the containers offered, the cookie jars (the default's
    // and one per container), every page's localStorage, and the session
    // — the tabs of the last run come back, each page fetched when its tab
    // is shown, and a URL on the command line opens beside them. Each
    // file is written back whenever what it holds changes — at most once
    // a second, and once more at the end — whole or not at all, so a
    // crash loses a second of it at most.
    std::string saved_session;
    std::uint64_t saved_storage = 0;
    std::map<std::string, std::uint64_t> saved_cookies; // by container name; "" the default
    bool restored = false;
    if (!profile_path.empty()) {
        browser.set_containers(load_containers(profile_path / "containers.json"));
        std::int64_t const now = unix_seconds_now();
        std::vector<std::string> names { "" };
        for (ui::Browser::Container const& container : browser.containers())
            names.push_back(container.name);
        for (std::string const& name : names) {
            if (std::optional<std::string> const text = read_text_file(cookie_file(profile_path, name)))
                loader.cookies(name).load(*text, now);
            saved_cookies[name] = loader.cookies(name).changes();
        }
        if (std::optional<std::string> const text = read_text_file(profile_path / "storage.json"))
            browser.restore_storage(*text);
        saved_storage = browser.storage_changes();
        if (std::optional<std::string> const text = read_text_file(profile_path / "session.json")) {
            restored = browser.restore_session(*text);
            if (restored)
                saved_session = browser.session_json();
        }
    }
    if (!restored) {
        browser.navigate(start_url.empty() ? "about:sashfold" : start_url);
    } else if (!start_url.empty()) {
        browser.new_tab();
        browser.navigate(start_url);
    }
    // The timings: the clock starts with that navigation; every present is
    // clocked; the first present of the page and the moment nothing was
    // left to load are kept.
    using clock = std::chrono::steady_clock;
    using wall_ms = std::chrono::duration<double, std::milli>;
    auto const started = clock::now();
    double first_present_ms = 0;
    double loaded_ms = 0;
    std::vector<double> present_ms;
    ui::Profile const base_profile = browser.profile();
    auto const present = [&](Bitmap const& frame) {
        auto const t = clock::now();
        window->present(frame);
        present_ms.push_back(wall_ms(clock::now() - t).count());
    };
    auto last_profile_write = std::chrono::steady_clock::now();
    // Writes whatever of the profile changed; true when a write is still
    // owed because the last one was less than a second ago.
    auto const save_profile = [&](bool regardless) {
        if (profile_path.empty())
            return false;
        auto const now = std::chrono::steady_clock::now();
        bool const throttled = !regardless && now - last_profile_write < std::chrono::seconds(1);
        bool owed = false;
        if (std::string session = browser.session_json(); session != saved_session) {
            if (throttled) {
                owed = true;
            } else {
                if (write_text_file_atomically(profile_path / "session.json", session))
                    saved_session = std::move(session);
                last_profile_write = now;
            }
        }
        if (browser.storage_changes() != saved_storage) {
            if (throttled) {
                owed = true;
            } else {
                if (write_text_file_atomically(profile_path / "storage.json", browser.storage_json()))
                    saved_storage = browser.storage_changes();
                last_profile_write = now;
            }
        }
        std::vector<std::string> names { "" };
        for (std::string const& name : loader.container_names())
            names.push_back(name);
        for (std::string const& name : names) {
            net::CookieJar& jar = loader.cookies(name);
            auto const written = saved_cookies.find(name);
            if (written != saved_cookies.end() && written->second == jar.changes())
                continue;
            if (throttled) {
                owed = true;
            } else {
                if (write_text_file_atomically(cookie_file(profile_path, name), jar.serialize()))
                    saved_cookies[name] = jar.changes();
                last_profile_write = now;
            }
        }
        return owed;
    };

    std::filesystem::file_time_type theme_stamp;
    if (!theme_file.empty())
        theme_stamp = std::filesystem::last_write_time(theme_file, error);
    auto last_theme_check = std::chrono::steady_clock::now();
    // The themes folder is looked at too: a folder's time moves when
    // something is put in it or taken out.
    std::filesystem::file_time_type themes_stamp;
    if (!user_themes.empty())
        themes_stamp = std::filesystem::last_write_time(user_themes, error);
    auto last_themes_check = std::chrono::steady_clock::now();
    std::string last_title;
    std::optional<Rect> last_caret;

    bool running = true;
    while (running) {
        if (exit_after_ms > 0 && wall_ms(clock::now() - started).count() >= exit_after_ms)
            break;
        platform::WindowEvent event;
        while (window->poll(event)) {
            using Kind = platform::WindowEvent::Kind;
            switch (event.kind) {
            case Kind::Close: running = false; break;
            case Kind::Resize: browser.resize(event.width, event.height); break;
            case Kind::Scale: browser.set_scale(event.scale); break;
            case Kind::MouseMove: browser.mouse_move(event.x, event.y); break;
            case Kind::MouseDown: browser.mouse_down(event.x, event.y, event.button, event.modifiers); break;
            case Kind::MouseUp: browser.mouse_up(event.x, event.y, event.button); break;
            case Kind::Wheel: browser.wheel(event.x, event.y, event.wheel); break;
            case Kind::Scroll: browser.scroll_pixels(event.x, event.y, event.scroll_x, event.scroll_y); break;
            case Kind::KeyDown: browser.key_down(event.key); break;
            case Kind::Text: browser.text_input(event.text); break;
            case Kind::Preedit: browser.preedit(event.preedit); break;
            case Kind::Active: browser.set_window_active(event.active); break;
            case Kind::None: break;
            }
        }
        if (!running)
            break;
        // The input method follows the caret: told where it is whenever
        // that changes, and that there is none when no field has focus.
        if (std::optional<Rect> const caret = browser.text_input_area(); caret != last_caret) {
            window->set_text_input(caret);
            last_caret = caret;
        }
        // The frame: the shell's where the system draws none, and what the
        // reader asked of it through that frame.
        if (window->wants_client_decorations() != browser.window_controls())
            browser.set_window_controls(window->wants_client_decorations());
        {
            using Request = ui::Browser::WindowRequest;
            using Edge = platform::WindowEdge;
            switch (browser.take_window_request()) {
            case Request::None: break;
            case Request::Move: window->begin_move(); break;
            case Request::Minimize: window->minimize(); break;
            case Request::ToggleMaximize: window->toggle_maximize(); break;
            case Request::Close: running = false; break;
            case Request::ResizeTop: window->begin_resize(Edge::Top); break;
            case Request::ResizeBottom: window->begin_resize(Edge::Bottom); break;
            case Request::ResizeLeft: window->begin_resize(Edge::Left); break;
            case Request::ResizeRight: window->begin_resize(Edge::Right); break;
            case Request::ResizeTopLeft: window->begin_resize(Edge::TopLeft); break;
            case Request::ResizeTopRight: window->begin_resize(Edge::TopRight); break;
            case Request::ResizeBottomLeft: window->begin_resize(Edge::BottomLeft); break;
            case Request::ResizeBottomRight: window->begin_resize(Edge::BottomRight); break;
            }
            if (!running)
                break;
        }

        if (browser.has_pending_load()) {
            present(browser.frame()); // the "Loading" frame, before the synchronous fetch
            browser.tick();
        }
        browser.run_scripts(); // the pages' timers that came due
        if (browser.needs_paint())
            present(browser.frame());
        if (first_present_ms == 0 && !browser.has_pending_load() && !present_ms.empty()) {
            // The start page is on the frame, and nothing is left to load:
            // one moment today, two once pictures arrive after the page.
            first_present_ms = wall_ms(clock::now() - started).count();
            loaded_ms = first_present_ms;
        }
        std::string const title = browser.window_title();
        if (title != last_title) {
            window->set_title(title);
            last_title = title;
        }
        window->set_cursor(browser.cursor());

        // A theme chosen from the palette is on already; the window follows
        // that file from here on and keeps the choice for the next start.
        if (std::optional<std::string> const chosen = browser.take_theme_request()) {
            report_theme_pictures(browser);
            theme_file = *chosen;
            theme_stamp = std::filesystem::last_write_time(theme_file, error);
            if (!profile_path.empty())
                write_text_file_atomically(profile_path / "settings.json", "{\n  \"theme\": " + json_string(theme_file) + "\n}\n");
        }
        // Themes are data: edit the file and the window follows.
        if (!theme_file.empty()) {
            auto const now = std::chrono::steady_clock::now();
            if (now - last_theme_check > std::chrono::milliseconds(500)) {
                last_theme_check = now;
                std::filesystem::file_time_type const stamp
                    = std::filesystem::last_write_time(theme_file, error);
                if (!error && stamp != theme_stamp) {
                    theme_stamp = stamp;
                    browser.set_theme(load_theme(theme_file));
                    report_theme_pictures(browser);
                }
            }
        }
        // A theme put into the folder while the window is open is there to
        // choose the next time the list is opened.
        if (!user_themes.empty()) {
            auto const now = std::chrono::steady_clock::now();
            if (now - last_themes_check > std::chrono::milliseconds(2000)) {
                last_themes_check = now;
                std::filesystem::file_time_type const stamp = std::filesystem::last_write_time(user_themes, error);
                if (!error && stamp != themes_stamp) {
                    themes_stamp = stamp;
                    convert_dropped_themes(user_themes);
                    // Converting writes into the folder: its time as it is now.
                    themes_stamp = std::filesystem::last_write_time(user_themes, error);
                    browser.set_theme_presets(all_theme_presets(theme_path, user_themes));
                }
            }
        }
        bool const profile_owed = save_profile(false);
        if (!browser.has_pending_load()) {
            // Sleep until input, the theme check, the next page timer, or
            // the profile write that is owed.
            int timeout = theme_file.empty() ? (user_themes.empty() ? -1 : 2000) : 500;
            if (std::optional<double> const due = browser.next_timer_ms()) {
                int const ms = static_cast<int>(std::ceil(*due));
                timeout = timeout < 0 ? ms : std::min(timeout, ms);
            }
            if (profile_owed)
                timeout = timeout < 0 ? 1000 : std::min(timeout, 1000);
            if (exit_after_ms > 0) {
                int const left = std::max(1, exit_after_ms - static_cast<int>(wall_ms(clock::now() - started).count()));
                timeout = timeout < 0 ? left : std::min(timeout, left);
            }
            window->wait(timeout);
        }
    }
    save_profile(true);
    if (!timings_path.empty()) {
        std::vector<double> sorted = present_ms;
        std::sort(sorted.begin(), sorted.end());
        std::ofstream out(timings_path, std::ios::binary);
        out << std::fixed << std::setprecision(1) << "{\n"
            << "  \"url\": " << json_string(start_url) << ",\n"
            << "  \"first_present_ms\": " << first_present_ms << ",\n"
            << "  \"loaded_ms\": " << loaded_ms << ",\n"
            << "  \"ran_ms\": " << wall_ms(clock::now() - started).count() << ",\n"
            << "  \"presents\": " << present_ms.size() << ",\n"
            << "  \"present_ms\": { \"median\": " << (sorted.empty() ? 0.0 : sorted[sorted.size() / 2])
            << ", \"max\": " << (sorted.empty() ? 0.0 : sorted.back()) << " },\n"
            << "  \"shell\": " << profile_json(profile_since(browser.profile(), base_profile)) << ",\n"
            << "  \"network\": " << census_json(loader.census()) << "\n}\n";
        if (!out) {
            std::cerr << "error: could not write " << timings_path << "\n";
            return 1;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> const args(argv + 1, argv + argc);
    std::string mode;
    std::string input;
    std::string output = "sashfold-out.png";
    bool output_given = false;
    std::string start_url;
    std::string theme_path = default_theme_path(argv[0]);
    std::string blocklists_path = default_blocklists_path(argv[0]);
    std::optional<std::string> downloads;
    std::optional<std::string> profile;
    std::string font_path;
    // Files named on the command line, installed as if the machine had them:
    // what lets a render be taken in the same font world a test scores in.
    std::vector<std::string> added_fonts;
    std::string fonts_mode; // "system" or "builtin"; empty picks per mode
    int width = 0;
    int height = 0;
    int runs = 5;
    int exit_after_ms = 0; // the window ends itself after this many milliseconds; 0 never
    std::string timings_path; // where a headless window run writes what it measured
    bool update_goldens = false;
    RenderExtras extras;

    auto const value_after = [&](std::size_t& i, std::string& into) {
        if (i + 1 >= args.size()) {
            std::cerr << "error: " << args[i] << " needs a value\n";
            return false;
        }
        into = args[++i];
        return true;
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string const& arg = args[i];
        if (arg == "--theme") {
            if (!value_after(i, theme_path))
                return usage(argv[0]);
        } else if (arg == "--blocklists") {
            if (!value_after(i, blocklists_path))
                return usage(argv[0]);
        } else if (arg == "--downloads") {
            std::string directory;
            if (!value_after(i, directory))
                return usage(argv[0]);
            downloads = directory;
        } else if (arg == "--profile") {
            std::string directory;
            if (!value_after(i, directory))
                return usage(argv[0]);
            profile = directory;
        } else if (arg == "--script" || arg == "--render" || arg == "--fetch" || arg == "--dump-dom"
            || arg == "--font-sampler" || arg == "--font-info" || arg == "--bench" || arg == "--import-theme") {
            mode = arg;
            if (!value_after(i, input))
                return usage(argv[0]);
        } else if (arg == "--font") {
            if (!value_after(i, font_path))
                return usage(argv[0]);
        } else if (arg == "--add-font") {
            std::string file;
            if (!value_after(i, file))
                return usage(argv[0]);
            added_fonts.push_back(file);
        } else if (arg == "--fonts") {
            if (!value_after(i, fonts_mode))
                return usage(argv[0]);
            if (fonts_mode != "system" && fonts_mode != "builtin") {
                std::cerr << "error: --fonts takes system or builtin\n";
                return usage(argv[0]);
            }
        } else if (arg == "--smoke" || arg == "--font-list") {
            mode = arg;
        } else if (arg == "--update-goldens") {
            update_goldens = true;
        } else if (arg == "--dump-layout") {
            extras.dump_layout = true;
        } else if (arg == "--no-scripts") {
            extras.scripts = false;
        } else if (arg == "--script-time") {
            std::string text;
            if (!value_after(i, text))
                return usage(argv[0]);
            extras.script_time_ms = std::max(0, std::atoi(text.c_str()));
        } else if (arg == "--runs") {
            std::string text;
            if (!value_after(i, text))
                return usage(argv[0]);
            runs = std::clamp(std::atoi(text.c_str()), 1, 1000);
        } else if (arg == "--exit-after") {
            std::string text;
            if (!value_after(i, text))
                return usage(argv[0]);
            exit_after_ms = std::max(0, std::atoi(text.c_str()));
        } else if (arg == "--timings") {
            if (!value_after(i, timings_path))
                return usage(argv[0]);
        } else if (arg == "--width" || arg == "--height") {
            std::string text;
            if (!value_after(i, text))
                return usage(argv[0]);
            (arg == "--width" ? width : height) = std::max(64, std::atoi(text.c_str()));
        } else if (arg == "--scale") {
            std::string text;
            if (!value_after(i, text))
                return usage(argv[0]);
            double const factor = std::atof(text.c_str());
            if (!(factor >= 0.5 && factor <= 8))
                return usage(argv[0]);
            g_device_scale = static_cast<float>(factor);
        } else if (arg == "-o" || arg == "--output") {
            if (!value_after(i, output))
                return usage(argv[0]);
            output_given = true;
        } else if (arg == "--report") {
            if (!value_after(i, extras.report))
                return usage(argv[0]);
        } else if (arg == "--gaps") {
            if (!value_after(i, extras.gaps))
                return usage(argv[0]);
        } else if (arg == "--thumbnail") {
            if (!value_after(i, extras.thumbnail))
                return usage(argv[0]);
        } else if (arg == "--thumbnail-width" || arg == "--max-height") {
            std::string text;
            if (!value_after(i, text))
                return usage(argv[0]);
            (arg == "--max-height" ? extras.max_height : extras.thumbnail_width)
                = std::max(0, std::atoi(text.c_str()));
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else if (arg.starts_with("-")) {
            std::cerr << "error: unrecognised argument '" << arg << "'\n";
            return usage(argv[0]);
        } else {
            start_url = arg;
        }
    }

    // The machine's fonts serve the window, --render and --bench. The script
    // harness renders the built-in face alone unless told otherwise: its
    // goldens must match on every OS.
    bool const system_fonts = fonts_mode.empty() ? mode != "--script" : fonts_mode == "system";
    text::FontManager::instance().set_system_fonts(system_fonts);

    // A font file named here joins the catalogue as an installed face would,
    // after the system fonts are settled so that --fonts builtin --add-font
    // reaches exactly the faces asked for. A file that holds no outlined
    // face is an error rather than a silent nothing: a mistyped path would
    // otherwise lay the page out in a different font than the one intended,
    // which is the whole reason this flag exists.
    for (std::string const& file : added_fonts) {
        std::size_t outlined = 0;
        for (text::FaceInfo const& info : text::TrueTypeFont::scan_file(file))
            outlined += info.has_outlines ? 1 : 0;
        if (outlined == 0) {
            std::cerr << "error: --add-font " << file << ": no outlined face to install\n";
            return 1;
        }
        text::FontManager::instance().add_font_file(file);
    }

    render_blocklists_path = blocklists_path;
    if (mode == "--script")
        return run_script_mode(input, update_goldens, width ? width : 1024, height ? height : 720,
            theme_path, blocklists_path, downloads.value_or(""));
    if (mode == "--render")
        return render_page(input, output, width ? width : 800, height ? height : 720, extras);
    if (mode == "--bench")
        return bench(input, runs, width ? width : 1100, height ? height : 800, extras.report, theme_path, blocklists_path,
            downloads.value_or(""));
    if (mode == "--import-theme")
        return import_theme(input,
            output_given ? output : user_themes_directory(profile.value_or(default_profile_directory())));
    if (mode == "--fetch")
        return fetch_url(input);
    if (mode == "--dump-dom")
        return dump_dom(input);
    if (mode == "--font-sampler")
        return font_sampler(input, font_path);
    if (mode == "--font-info")
        return font_info(input);
    if (mode == "--font-list")
        return font_list();
    if (mode == "--smoke")
        return smoke_scene(output);
    return run_window(start_url, theme_path, blocklists_path, downloads.value_or(default_downloads_directory()),
        profile.value_or(default_profile_directory()), argv[0], exit_after_ms, timings_path);
}
