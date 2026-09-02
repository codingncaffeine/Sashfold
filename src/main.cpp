#include "core/Bitmap.h"
#include "core/Png.h"
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
#include "platform/Window.h"
#include "text/Face.h"
#include "text/FontManager.h"
#include "text/SashfoldMono.h"
#include "text/TrueType.h"
#include "ui/Browser.h"
#include "ui/InternalPages.h"
#include "ui/PageImages.h"
#include "ui/Script.h"
#include "ui/ShellLoader.h"
#include "ui/Theme.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace sashfold;

namespace {

int usage(char const* program)
{
    std::cerr << "usage: " << program << " [url] [--theme <file.json>] [--downloads <dir>]\n"
              << "       " << program << " --script <file> [--update-goldens] [--width N] [--height N]\n"
              << "       " << program << " --render <file.html|url> [-o out.png] [--width N] [--height N]\n"
              << "                 [--max-height N] [--thumbnail small.png [--thumbnail-width N]]\n"
              << "                 [--report out.json]\n"
              << "       " << program << " --bench <file.html|url> [--runs N] [--width N]\n"
              << "       " << program << " --fetch <url>\n"
              << "       " << program << " --dump-dom <file.html>\n"
              << "       " << program << " --font-sampler <output.png> [--font <file.ttf>]\n"
              << "       " << program << " --font-info <file.ttf|file.ttc>\n"
              << "       " << program << " --font-list\n"
              << "       any mode: --fonts system|builtin   (system, except --script)\n"
              << "       " << program << " --smoke [-o output.png]\n"
              << "\n"
              << "  With a URL or nothing, opens the browser window.\n"
              << "  --theme applies a theme file to the window and to --script; the default is\n"
              << "          themes/default.json beside the executable or its parent, reloaded\n"
              << "          whenever the file changes while the window is open.\n"
              << "  --downloads is where downloads are saved (the window defaults to your\n"
              << "          Downloads folder; --script saves nothing unless told where).\n"
              << "  --script replays a shell script headlessly and checks its assertions.\n"
              << "  --render lays out the page (local file or live URL) and writes a PNG; a load\n"
              << "          that fails renders the page the window would show. --max-height caps the\n"
              << "          picture, --thumbnail draws the viewport's top small, --report writes a\n"
              << "          JSON account of the load and the render.\n"
              << "  --bench times parse, style, layout and paint of a page, several runs.\n"
              << "  --fetch prints the response head through the fetch choke point.\n"
              << "  --dump-dom parses the file and prints the document tree.\n"
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
                  << (font->has_cff() ? ", CFF outlines (declined)" : "") << "\n"
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
};

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

// Loads a --render / --bench input through the shell's loader.
std::optional<LoadedPage> load_page(std::string const& source)
{
    std::optional<net::Url> const url = input_url(source);
    if (!url)
        return std::nullopt;
    auto loader = std::make_unique<ui::ShellLoader>();
    net::FetchResult result = loader->load(*url, "", false);
    if (!result.response) {
        std::cerr << "error: " << result.error << "\n";
        return std::nullopt;
    }
    if (url->scheme != "file")
        std::cerr << "fetched " << result.response->final_url.serialize() << " ("
                  << result.response->status << ", " << result.response->body.size() << " bytes)\n";
    return LoadedPage { std::string(result.response->body.begin(), result.response->body.end()),
        result.response->final_url, std::move(loader) };
}

// Why a subresource did not arrive: the fetch error, or the status.
std::string describe_failure(net::FetchResult const& result)
{
    if (!result.response)
        return result.error;
    return "status " + std::to_string(result.response->status);
}

// The page's stylesheets, fetched through its own session; a failure is
// named on stderr and counted when a counter is given.
css::SheetFetcher sheet_fetcher(LoadedPage const& page, int* failures = nullptr)
{
    return [&page, failures](net::Url const& url) -> std::optional<css::FetchedSheet> {
        net::FetchResult result = page.loader->load_subresource(url, page.url, "");
        if (!result.response || result.response->status != 200) {
            std::cerr << "stylesheet " << url.serialize() << ": " << describe_failure(result) << "\n";
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
        net::FetchResult result = page.loader->load_subresource(url, page.url, "");
        if (!result.response || result.response->status != 200) {
            std::cerr << "image " << url.serialize() << ": " << describe_failure(result) << "\n";
            if (failures)
                ++*failures;
            return std::nullopt;
        }
        return std::move(result.response->body);
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
    load.page.loader = std::make_unique<ui::ShellLoader>();
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
struct RenderExtras {
    std::string report; // a JSON account of the load and the render
    std::string thumbnail; // a small PNG of the viewport's top
    int thumbnail_width = 320;
    int max_height = 0; // a cap on the picture's height; 0 keeps the page's
};

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
            std::string const name = lowercase_ascii(value.function().name);
            if (name.ends_with("gradient"))
                ++census["gradients"];
            else if (name == "counter" || name == "counters")
                ++census["counters"];
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
    bool has_url = false;
    for (css::ComponentValue const& value : declaration.value) {
        if (value.is_token(css::Token::Type::Ident) && first_ident.empty())
            first_ident = lowercase_ascii(value.token().value);
        if (value.is_token(css::Token::Type::Url) || (value.is_function() && lowercase_ascii(value.function().name) == "url"))
            has_url = true;
    }
    if (name.starts_with("--"))
        return; // custom properties are written; what they hold is counted where it is used
    if (name == "position" || name == "z-index")
        return; // positioning and z-index are written
    if (name == "display" && first_ident == "inline-block")
        return; // inline-block is written
    if (name == "display" && (first_ident.starts_with("table") || first_ident == "inline-table"))
        ++census["tables"];
    else if (name == "display" && (first_ident == "grid" || first_ident == "inline-grid"))
        ++census["grid"];
    else if (name.starts_with("grid-") || name == "grid")
        ++census["grid"];
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
        ++census["vertical-align"];
    else if (name == "background-image" || (name == "background" && has_url))
        ++census["background-images"];
    else if (name.starts_with("border") && name.find("radius") != std::string::npos)
        ++census["border-radius"];
    else if (name == "box-shadow" || name == "text-shadow")
        ++census["shadows"];
    else if (name == "filter" || name == "backdrop-filter" || name == "clip-path" || name == "mask")
        ++census["effects"];
    else if (name == "letter-spacing" || name == "word-spacing" || name == "text-transform" || name == "text-overflow")
        ++census["text-properties"];
    else if (name == "columns" || name == "column-count" || name == "column-width")
        ++census["multi-column"];
    else if (name == "object-fit" || name == "aspect-ratio")
        ++census["sizing"];
    else if (name == "outline" || name.starts_with("outline-"))
        ++census["outline"];
    else if (name == "direction" || name == "writing-mode")
        ++census["direction"];
    else if (name == "counter-reset" || name == "counter-increment")
        ++census["counters"];
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
            if (name == "font-face") {
                for (css::Rule const& child : rule.at_rule().child_rules) {
                    if (!child.is_nested_declarations())
                        continue;
                    for (css::Declaration const& declaration : child.nested_declarations().declarations) {
                        if (lowercase_ascii(declaration.name) != "src")
                            continue;
                        for (css::ComponentValue const& value : declaration.value) {
                            std::string text;
                            if (value.is_token(css::Token::Type::Url) || value.is_token(css::Token::Type::String))
                                text = lowercase_ascii(value.token().value);
                            else if (value.is_function())
                                for (css::ComponentValue const& inner : value.function().values)
                                    if (inner.is_token(css::Token::Type::String))
                                        text = lowercase_ascii(inner.token().value);
                            if (text.find("woff") != std::string::npos) {
                                ++census["web-fonts"];
                                break;
                            }
                        }
                    }
                }
                continue;
            }
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
        static_cast<float>(viewport_height) };
    auto const t0 = clock::now();
    auto document = html::parse_document_bytes(loaded.bytes);
    auto const t1 = clock::now();
    int sheet_failures = 0;
    int image_failures = 0;
    std::vector<css::SheetSource> const sheets = css::collect_stylesheets(*document, &loaded.url,
        sheet_fetcher(loaded, &sheet_failures), media);
    std::vector<text::PageFont> const fonts
        = css::collect_page_fonts(sheets, sheet_fetcher(loaded, &sheet_failures), media);
    text::FontManager::instance().set_page_fonts(fonts);
    auto const t2 = clock::now();
    css::StyleMap const styles = css::resolve_styles(*document, sheets, media);
    auto const t3 = clock::now();
    layout::ImageMap const images = ui::collect_images(*document, &loaded.url,
        image_fetcher(loaded, &image_failures), media);
    auto const t4 = clock::now();
    layout::LayoutResult const page = layout::layout_document(*document, styles,
        static_cast<float>(viewport_width), &images, nullptr, static_cast<float>(viewport_height));
    auto const t5 = clock::now();

    int height = std::max(1, static_cast<int>(page.page_height + 0.5f));
    if (extras.max_height > 0)
        height = std::min(height, extras.max_height);
    Bitmap canvas(viewport_width, height, page.canvas_background);
    paint::paint_page(canvas, page);
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
            << "  \"connections\": { \"opened\": " << connections.opened << ", \"reused\": "
            << connections.reused << ", \"retried\": " << connections.retried << " },\n";
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
int bench(std::string const& input, int runs, int viewport_width, int viewport_height)
{
    using clock = std::chrono::steady_clock;
    using ms = std::chrono::duration<double, std::milli>;
    auto const page_started = clock::now();
    std::optional<LoadedPage> const loaded = load_page(input);
    if (!loaded)
        return 1;
    double const page_ms = ms(clock::now() - page_started).count();
    css::MediaContext const media { static_cast<float>(viewport_width),
        static_cast<float>(viewport_height) };
    // The sheets are fetched once, outside the timed runs: the network is
    // not what the phases measure — what it cost is reported on its own line.
    auto const sheets_started = clock::now();
    std::vector<css::SheetSource> const sheets = [&] {
        auto const first = html::parse_document_bytes(loaded->bytes);
        return css::collect_stylesheets(*first, &loaded->url, sheet_fetcher(*loaded), media);
    }();
    text::FontManager::instance().set_page_fonts(
        css::collect_page_fonts(sheets, sheet_fetcher(*loaded), media));
    double const sheets_ms = ms(clock::now() - sheets_started).count();
    std::size_t image_count = 0;
    double images_ms = 0;
    struct Sample {
        double parse = 0;
        double sheets = 0;
        double style = 0;
        double layout = 0;
        double paint = 0;
    };
    std::vector<Sample> samples;
    float page_height = 0;
    std::size_t rule_count = 0;
    std::size_t universal_count = 0;
    for (int run = 0; run < runs; ++run) {
        Sample sample;
        auto const t0 = clock::now();
        auto document = html::parse_document_bytes(loaded->bytes);
        auto const t1 = clock::now();
        css::StyleSet const style_set(sheets, media);
        auto const t1b = clock::now();
        css::StyleMap const styles = css::resolve_styles(*document, style_set);
        auto const t2 = clock::now();
        rule_count = style_set.rule_count();
        universal_count = style_set.universal_count();
        // Images are fetched here on every run, so the first run pays the
        // network and the rest the session cache; neither counts as a phase.
        layout::ImageMap const images = ui::collect_images(*document, &loaded->url, image_fetcher(*loaded), media);
        image_count = images.size();
        auto const t2b = clock::now();
        if (run == 0)
            images_ms = ms(t2b - t2).count();
        layout::LayoutResult const page = layout::layout_document(*document, styles,
            static_cast<float>(viewport_width), &images, nullptr, static_cast<float>(viewport_height));
        auto const t3 = clock::now();
        Bitmap canvas(viewport_width, 1000, page.canvas_background);
        paint::paint_page(canvas, page);
        auto const t4 = clock::now();
        sample.parse = ms(t1 - t0).count();
        sample.sheets = ms(t1b - t1).count();
        sample.style = ms(t2 - t1b).count();
        sample.layout = ms(t3 - t2b).count();
        sample.paint = ms(t4 - t3).count();
        samples.push_back(sample);
        page_height = page.page_height;
    }
    auto const report = [&](char const* name, double Sample::*member) {
        std::vector<double> values;
        for (Sample const& sample : samples)
            values.push_back(sample.*member);
        std::sort(values.begin(), values.end());
        std::printf("  %-7s min %8.2f ms   median %8.2f ms\n", name, values.front(),
            values[values.size() / 2]);
    };
    std::printf("bench: %zu bytes, %zu sheet(s) with %zu rules (%zu universal), %zu image(s), "
                "%d run(s), viewport %d px wide, page %d px tall\n",
        loaded->bytes.size(), sheets.size(), rule_count, universal_count, image_count, runs,
        viewport_width, static_cast<int>(page_height + 0.5f));
    net::ConnectionPool::Stats const& connections = loaded->loader->pool().stats();
    std::printf("  network page %.0f ms, sheets %.0f ms, images %.0f ms (first run); "
                "connections opened %zu, reused %zu, retried %zu\n",
        page_ms, sheets_ms, images_ms, connections.opened, connections.reused,
        connections.retried);
    report("parse", &Sample::parse);
    report("sheets", &Sample::sheets);
    report("style", &Sample::style);
    report("layout", &Sample::layout);
    report("paint", &Sample::paint);
    std::vector<double> totals;
    for (Sample const& sample : samples)
        totals.push_back(sample.parse + sample.sheets + sample.style + sample.layout + sample.paint);
    std::sort(totals.begin(), totals.end());
    std::printf("  %-7s min %8.2f ms   median %8.2f ms\n", "total", totals.front(),
        totals[totals.size() / 2]);
    return 0;
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
std::string default_theme_path(char const* program)
{
    std::error_code error;
    std::filesystem::path const exe = std::filesystem::absolute(program, error);
    if (error)
        return {};
    std::filesystem::path const dir = exe.parent_path();
    for (std::filesystem::path const& base : { dir, dir.parent_path() }) {
        std::filesystem::path const candidate = base / "themes" / "default.json";
        if (std::filesystem::exists(candidate, error))
            return candidate.string();
    }
    return {};
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

int run_script_mode(std::string const& script, bool update_goldens, int width, int height,
    std::string const& theme_path, std::string const& downloads)
{
    ui::ShellLoader loader;
    ui::Browser browser(loader, load_theme(theme_path), width, height);
    platform::use_process_clipboard(true); // a script never touches the real clipboard
    browser.set_downloads_directory(downloads);
    ui::ScriptResult const result = ui::run_script(browser, script, update_goldens, std::cout);
    return result.ok() ? 0 : 1;
}

int run_window(std::string const& start_url, std::string const& theme_path,
    std::string const& downloads)
{
    std::unique_ptr<platform::Window> window = platform::Window::create("Sashfold", 1100, 760);
    if (!window) {
        std::cerr << "error: no window backend on this OS yet (the Wayland and AppKit shells are not written);\n"
                     "       --render, --fetch, and --script work everywhere\n";
        return 1;
    }
    ui::ShellLoader loader;
    ui::Browser browser(loader, load_theme(theme_path), window->width(), window->height());
    browser.set_downloads_directory(downloads);
    browser.navigate(start_url.empty() ? "about:sashfold" : start_url);

    std::error_code error;
    std::filesystem::file_time_type theme_stamp;
    if (!theme_path.empty())
        theme_stamp = std::filesystem::last_write_time(theme_path, error);
    auto last_theme_check = std::chrono::steady_clock::now();
    std::string last_title;

    bool running = true;
    while (running) {
        platform::WindowEvent event;
        while (window->poll(event)) {
            using Kind = platform::WindowEvent::Kind;
            switch (event.kind) {
            case Kind::Close: running = false; break;
            case Kind::Resize: browser.resize(event.width, event.height); break;
            case Kind::MouseMove: browser.mouse_move(event.x, event.y); break;
            case Kind::MouseDown: browser.mouse_down(event.x, event.y, event.button); break;
            case Kind::MouseUp: browser.mouse_up(event.x, event.y, event.button); break;
            case Kind::Wheel: browser.wheel(event.x, event.y, event.wheel); break;
            case Kind::KeyDown: browser.key_down(event.key); break;
            case Kind::Text: browser.text_input(event.text); break;
            case Kind::None: break;
            }
        }
        if (!running)
            break;

        if (browser.has_pending_load()) {
            window->present(browser.frame()); // the "Loading" frame, before the synchronous fetch
            browser.tick();
        }
        if (browser.needs_paint())
            window->present(browser.frame());
        std::string const title = browser.window_title();
        if (title != last_title) {
            window->set_title(title);
            last_title = title;
        }
        window->set_cursor(browser.cursor());

        // Themes are data: edit the file and the window follows.
        if (!theme_path.empty()) {
            auto const now = std::chrono::steady_clock::now();
            if (now - last_theme_check > std::chrono::milliseconds(500)) {
                last_theme_check = now;
                std::filesystem::file_time_type const stamp
                    = std::filesystem::last_write_time(theme_path, error);
                if (!error && stamp != theme_stamp) {
                    theme_stamp = stamp;
                    browser.set_theme(load_theme(theme_path));
                }
            }
        }
        if (!browser.has_pending_load())
            window->wait(theme_path.empty() ? -1 : 500);
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
    std::string start_url;
    std::string theme_path = default_theme_path(argv[0]);
    std::optional<std::string> downloads;
    std::string font_path;
    std::string fonts_mode; // "system" or "builtin"; empty picks per mode
    int width = 0;
    int height = 0;
    int runs = 5;
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
        } else if (arg == "--downloads") {
            std::string directory;
            if (!value_after(i, directory))
                return usage(argv[0]);
            downloads = directory;
        } else if (arg == "--script" || arg == "--render" || arg == "--fetch" || arg == "--dump-dom"
            || arg == "--font-sampler" || arg == "--font-info" || arg == "--bench") {
            mode = arg;
            if (!value_after(i, input))
                return usage(argv[0]);
        } else if (arg == "--font") {
            if (!value_after(i, font_path))
                return usage(argv[0]);
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
        } else if (arg == "--runs") {
            std::string text;
            if (!value_after(i, text))
                return usage(argv[0]);
            runs = std::clamp(std::atoi(text.c_str()), 1, 1000);
        } else if (arg == "--width" || arg == "--height") {
            std::string text;
            if (!value_after(i, text))
                return usage(argv[0]);
            (arg == "--width" ? width : height) = std::max(64, std::atoi(text.c_str()));
        } else if (arg == "-o" || arg == "--output") {
            if (!value_after(i, output))
                return usage(argv[0]);
        } else if (arg == "--report") {
            if (!value_after(i, extras.report))
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

    if (mode == "--script")
        return run_script_mode(input, update_goldens, width ? width : 1024, height ? height : 720,
            theme_path, downloads.value_or(""));
    if (mode == "--render")
        return render_page(input, output, width ? width : 800, height ? height : 720, extras);
    if (mode == "--bench")
        return bench(input, runs, width ? width : 800, height ? height : 720);
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
    return run_window(start_url, theme_path, downloads.value_or(default_downloads_directory()));
}
