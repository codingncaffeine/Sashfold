#include "core/Bitmap.h"
#include "core/Png.h"
#include "css/StyleResolver.h"
#include "css/Stylesheets.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "net/Url.h"
#include "paint/Painter.h"
#include "text/FontManager.h"
#include "ui/PageImages.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Scores the engine on the Web Platform Tests' CSS reference tests. Every
// test under the listed directories that names a reference with
// <link rel="match"> or <link rel="mismatch"> is rendered at WPT's 800x600
// viewport, and so is each reference, from the same checkout with the
// same engine: a match must be pixel-identical (or within the test's own
// <meta name="fuzzy"> allowance), a mismatch must differ, and references
// may chain — the rules are the manifest's and wptrunner's, mirrored here.
// Pages resolve their references as if served from http://web-platform.test/
// so root-relative paths (the Ahem font, the shared references) land inside
// the checkout, and nothing touches the network. The committed baseline
// lists every passing test: a listed test that fails is a regression, a
// new pass is a ratchet to bless with --update.

using namespace sashfold;

namespace {

constexpr int viewport_width = 800;
constexpr int viewport_height = 600;
constexpr int max_chain_depth = 6;
constexpr std::size_t render_cache_size = 96;
constexpr std::string_view origin = "http://web-platform.test:8000";

std::optional<std::string> read_file(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::ostringstream stream;
    stream << file.rdbuf();
    return std::move(stream).str();
}

std::string lowercased(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string trimmed(std::string_view text)
{
    std::size_t start = 0;
    std::size_t end = text.size();
    auto const is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; };
    while (start < end && is_space(text[start]))
        ++start;
    while (end > start && is_space(text[end - 1]))
        --end;
    return std::string(text.substr(start, end - start));
}

std::vector<std::string> split(std::string_view text, char separator)
{
    std::vector<std::string> out;
    std::string current;
    for (char const c : text) {
        if (c == separator) {
            out.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    out.push_back(current);
    return out;
}

std::string percent_decode(std::string_view text)
{
    auto const hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

// --- Which files are tests: the manifest's rules --------------------------------------------

bool is_markup_extension(std::string const& extension)
{
    return extension == ".html" || extension == ".htm" || extension == ".xht" || extension == ".xhtml";
}

// reference_file_re: (^|[-_])(not)?ref[0-9]*([-_]|$) over the name without its extension.
bool name_matches_reference(std::string const& stem)
{
    for (std::size_t i = 0; i + 3 <= stem.size(); ++i) {
        if (stem.compare(i, 3, "ref") != 0)
            continue;
        bool boundary_before = i == 0 || stem[i - 1] == '-' || stem[i - 1] == '_';
        if (!boundary_before && i >= 3 && stem.compare(i - 3, 3, "not") == 0)
            boundary_before = i == 3 || stem[i - 4] == '-' || stem[i - 4] == '_';
        if (!boundary_before)
            continue;
        std::size_t j = i + 3;
        while (j < stem.size() && stem[j] >= '0' && stem[j] <= '9')
            ++j;
        if (j == stem.size() || stem[j] == '-' || stem[j] == '_')
            return true;
    }
    return false;
}

// The manifest's classification of a checkout-relative path (forward
// slashes): a reftest candidate, or not a test at all.
bool is_test_candidate(std::string const& rel)
{
    std::vector<std::string> const parts = split(rel, '/');
    if (parts.size() < 2)
        return false;
    std::string const& filename = parts.back();
    std::filesystem::path const name(filename);
    if (!is_markup_extension(lowercased(name.extension().string())))
        return false;
    if (filename.starts_with(".") || filename.starts_with("MANIFEST"))
        return false;
    if (parts[0] == "common")
        return false;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] == "resources" || parts[i] == "support" || parts[i] == "tools"
            || parts[i] == "reference" || parts[i] == "crashtests" || parts[i] == "print")
            return false;
    }
    if (rel.starts_with("css/CSS2/archive/") || rel.starts_with("css/common/"))
        return false;
    std::string const stem = name.stem().string();
    if (name_matches_reference(stem))
        return false;
    if (std::size_t const dash = stem.rfind('-'); dash != std::string::npos) {
        std::string const flag = stem.substr(dash + 1);
        if (flag == "manual" || flag == "visual" || flag == "crash" || flag == "print")
            return false;
    }
    return true;
}

// --- What a page declares: references and fuzziness -------------------------------------------

struct Link {
    bool match; // rel=match, else rel=mismatch
    std::string href;
};

struct FuzzyRange {
    long min_difference = 0;
    long max_difference = 0;
    long min_pixels = 0;
    long max_pixels = 0;
};

struct PageInfo {
    std::vector<Link> links;
    std::vector<std::pair<std::string, FuzzyRange>> fuzzy; // keyed by the reference's URL, or "" for all
    bool reftest_wait = false;
};

std::string attribute_of(dom::Element const& element, char const* name)
{
    dom::Attr const* attribute = element.find_attribute(name);
    return attribute ? attribute->value : std::string();
}

// "[a-]b" → a range; "b" alone is exactly b.
std::optional<std::pair<long, long>> parse_range(std::string_view text)
{
    std::string const value = trimmed(text);
    std::size_t const dash = value.find('-');
    auto const number = [](std::string const& digits) -> std::optional<long> {
        std::string const clean = trimmed(digits);
        if (clean.empty())
            return std::nullopt;
        for (char const c : clean) {
            if (c < '0' || c > '9')
                return std::nullopt;
        }
        return std::stol(clean);
    };
    if (dash == std::string::npos) {
        std::optional<long> const single = number(value);
        if (!single)
            return std::nullopt;
        return std::pair<long, long> { *single, *single };
    }
    std::optional<long> const low = number(value.substr(0, dash));
    std::optional<long> const high = number(value.substr(dash + 1));
    if (!low || !high)
        return std::nullopt;
    return std::pair<long, long> { *low, *high };
}

// The manifest's fuzzy grammar: "[reference:]maxDifference=[a-]b;totalPixels=[c-]d",
// the names optional in that order.
std::optional<std::pair<std::string, FuzzyRange>> parse_fuzzy(std::string const& content,
    net::Url const& page_url)
{
    std::string key;
    std::string value = content;
    if (std::size_t const colon = content.rfind(':'); colon != std::string::npos) {
        std::optional<net::Url> const reference = net::parse_url(trimmed(content.substr(0, colon)), &page_url);
        if (!reference)
            return std::nullopt;
        key = reference->serialize();
        value = content.substr(colon + 1);
    }
    std::vector<std::string> const parts = split(value, ';');
    if (parts.size() != 2)
        return std::nullopt;
    std::optional<std::pair<long, long>> difference;
    std::optional<std::pair<long, long>> pixels;
    std::deque<std::pair<long, long>> positional;
    for (std::string const& part : parts) {
        std::string name;
        std::string range = part;
        if (std::size_t const equals = part.find('='); equals != std::string::npos) {
            name = trimmed(part.substr(0, equals));
            range = part.substr(equals + 1);
        }
        std::optional<std::pair<long, long>> const parsed = parse_range(range);
        if (!parsed)
            return std::nullopt;
        if (name.empty())
            positional.push_back(*parsed);
        else if (name == "maxDifference" && !difference)
            difference = *parsed;
        else if (name == "totalPixels" && !pixels)
            pixels = *parsed;
        else
            return std::nullopt;
    }
    if (!difference) {
        if (positional.empty())
            return std::nullopt;
        difference = positional.front();
        positional.pop_front();
    }
    if (!pixels) {
        if (positional.empty())
            return std::nullopt;
        pixels = positional.front();
        positional.pop_front();
    }
    FuzzyRange const fuzzy { difference->first, difference->second, pixels->first, pixels->second };
    return std::pair<std::string, FuzzyRange> { key, fuzzy };
}

void gather_info(dom::Node const& node, net::Url const& page_url, PageInfo& info)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_html("link")) {
            std::string const rel = lowercased(trimmed(attribute_of(element, "rel")));
            std::string const href = trimmed(attribute_of(element, "href"));
            if ((rel == "match" || rel == "mismatch") && !href.empty())
                info.links.push_back(Link { rel == "match", href });
        } else if (element.is_html("meta")) {
            if (lowercased(trimmed(attribute_of(element, "name"))) == "fuzzy") {
                if (auto fuzzy = parse_fuzzy(attribute_of(element, "content"), page_url))
                    info.fuzzy.push_back(std::move(*fuzzy));
            }
        } else if (element.is_html("html")) {
            for (std::string const& token : split(attribute_of(element, "class"), ' ')) {
                if (token == "reftest-wait")
                    info.reftest_wait = true;
            }
        }
    }
    for (dom::Node const* child : node.children())
        gather_info(*child, page_url, info);
}

// The allowance for comparing this page with the reference: the entry
// keyed by that reference, else the unkeyed one, else none.
std::optional<FuzzyRange> fuzzy_for(PageInfo const& info, std::string const& reference_url)
{
    for (auto const& [key, range] : info.fuzzy) {
        if (key == reference_url)
            return range;
    }
    for (auto const& [key, range] : info.fuzzy) {
        if (key.empty())
            return range;
    }
    return std::nullopt;
}

// --- Rendering pages from the checkout -------------------------------------------------------

struct Rendered {
    std::shared_ptr<Bitmap const> bitmap;
    PageInfo info;
    net::Url url;
};

class Renderer {
public:
    explicit Renderer(std::filesystem::path root)
        : m_root(std::move(root))
    {
    }

    std::size_t renders = 0;

    // The checkout-relative path a URL names, when it names one inside it.
    std::optional<std::string> rel_path_for(net::Url const& url) const
    {
        if (url.scheme != "http" && url.scheme != "https")
            return std::nullopt;
        if (url.host != "web-platform.test" && !url.host.ends_with(".web-platform.test"))
            return std::nullopt;
        std::string path = percent_decode(url.serialize_path());
        if (path.empty() || path[0] != '/' || path.find("..") != std::string::npos)
            return std::nullopt;
        return path.substr(1);
    }

    static std::optional<net::Url> url_for(std::string const& rel)
    {
        return net::parse_url(std::string(origin) + "/" + rel);
    }

    // The page rendered at the viewport, with what it declares; null when
    // the file is not there. Recent pages stay cached: references repeat.
    std::shared_ptr<Rendered const> render(std::string const& rel)
    {
        if (auto const it = m_cache.find(rel); it != m_cache.end())
            return it->second;
        std::shared_ptr<Rendered const> const rendered = render_uncached(rel);
        m_cache.emplace(rel, rendered);
        m_order.push_back(rel);
        while (m_order.size() > render_cache_size) {
            m_cache.erase(m_order.front());
            m_order.pop_front();
        }
        return rendered;
    }

private:
    std::shared_ptr<Rendered const> render_uncached(std::string const& rel)
    {
        std::optional<net::Url> const url = url_for(rel);
        std::optional<std::string> const source = read_file(m_root / rel);
        if (!url || !source)
            return nullptr;
        if (std::getenv("SASHFOLD_WPT_TRACE"))
            std::cerr << "  render " << rel << "\n";
        ++renders;
        css::MediaContext const media { static_cast<float>(viewport_width),
            static_cast<float>(viewport_height) };
        auto document = html::parse_document_bytes(*source);
        auto rendered = std::make_shared<Rendered>();
        rendered->url = *url;
        gather_info(*document, *url, rendered->info);

        auto const fetch_sheet = [&](net::Url const& target) -> std::optional<css::FetchedSheet> {
            std::optional<std::string> const bytes = read_url(target);
            if (!bytes)
                return std::nullopt;
            std::string const path = lowercased(target.serialize_path());
            return css::FetchedSheet { std::vector<std::uint8_t>(bytes->begin(), bytes->end()),
                path.ends_with(".css") ? "text/css" : "" };
        };
        auto const fetch_image = [&](net::Url const& target) -> std::optional<std::vector<std::uint8_t>> {
            std::optional<std::string> const bytes = read_url(target);
            if (!bytes)
                return std::nullopt;
            return std::vector<std::uint8_t>(bytes->begin(), bytes->end());
        };
        std::vector<css::SheetSource> const sheets
            = css::collect_stylesheets(*document, &*url, fetch_sheet, media);
        text::FontManager::instance().set_page_fonts(css::collect_page_fonts(sheets, fetch_sheet, media));
        css::StyleMap const styles = css::resolve_styles(*document, sheets, media);
        layout::ImageMap const images = ui::collect_images(*document, &*url, fetch_image, media);
        // The pictures the stylesheets ask for. The painter draws them from a
        // map of its own, and without it every background-image in the suite
        // is a blank box.
        layout::BackgroundImages const backgrounds
            = ui::collect_background_images(styles, fetch_image);
        layout::LayoutResult const page = layout::layout_document(*document, styles,
            static_cast<float>(viewport_width), &images, nullptr, static_cast<float>(viewport_height));
        auto canvas = std::make_shared<Bitmap>(viewport_width, viewport_height, page.canvas_background);
        paint::paint_page(*canvas, page, 0, 0, &backgrounds);
        rendered->bitmap = std::move(canvas);
        return rendered;
    }

    std::optional<std::string> read_url(net::Url const& target) const
    {
        std::optional<std::string> const rel = rel_path_for(target);
        if (!rel)
            return std::nullopt;
        return read_file(m_root / *rel);
    }

    std::filesystem::path m_root;
    std::unordered_map<std::string, std::shared_ptr<Rendered const>> m_cache;
    std::deque<std::string> m_order;
};

// --- Comparison ---------------------------------------------------------------------------------

struct Difference {
    long max_channel = 0; // the largest difference in any color channel
    long pixels = 0; // how many pixels differ at all
};

// Over the color channels, as wptrunner compares (alpha is not looked at).
Difference compare(Bitmap const& a, Bitmap const& b)
{
    Difference difference;
    std::vector<std::uint8_t> const& pa = a.pixels();
    std::vector<std::uint8_t> const& pb = b.pixels();
    std::size_t const count = std::min(pa.size(), pb.size()) / 4;
    for (std::size_t i = 0; i < count; ++i) {
        long worst = 0;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            long const delta = std::abs(static_cast<long>(pa[i * 4 + channel]) - static_cast<long>(pb[i * 4 + channel]));
            worst = std::max(worst, delta);
        }
        if (worst > 0) {
            ++difference.pixels;
            difference.max_channel = std::max(difference.max_channel, worst);
        }
    }
    return difference;
}

// wptrunner's check_pass, for one comparison.
bool equal_within(Difference const& difference, std::optional<FuzzyRange> const& fuzzy)
{
    if (!fuzzy
        || (fuzzy->min_difference == 0 && fuzzy->max_difference == 0 && fuzzy->min_pixels == 0
            && fuzzy->max_pixels == 0))
        return difference.pixels == 0 && difference.max_channel == 0;
    return (difference.pixels == 0 && fuzzy->min_pixels == 0)
        || (difference.max_channel == 0 && fuzzy->min_difference == 0)
        || (fuzzy->min_difference <= difference.max_channel && difference.max_channel <= fuzzy->max_difference
            && fuzzy->min_pixels <= difference.pixels && difference.pixels <= fuzzy->max_pixels);
}

struct Outcome {
    bool pass = false;
    std::string reason;
    std::shared_ptr<Bitmap const> left; // the last pair compared, for --dump
    std::shared_ptr<Bitmap const> right;
    std::string right_rel;
};

// The depth-first walk of the reference graph wptrunner performs: a test
// passes when some path of holding comparisons reaches a reference with
// no references of its own.
Outcome evaluate(Renderer& renderer, std::string const& test_rel)
{
    Outcome outcome;
    std::shared_ptr<Rendered const> const test = renderer.render(test_rel);
    if (!test) {
        outcome.reason = "unreadable";
        return outcome;
    }
    if (test->info.links.empty()) {
        outcome.reason = "no reference";
        return outcome;
    }
    struct Step {
        std::shared_ptr<Rendered const> left;
        std::string right_rel;
        bool match;
        int depth;
    };
    std::vector<Step> stack;
    auto const push_links = [&](std::shared_ptr<Rendered const> const& left, int depth) {
        std::vector<Link> const& links = left->info.links;
        for (std::size_t i = links.size(); i-- > 0;) {
            std::optional<net::Url> const target = net::parse_url(links[i].href, &left->url);
            std::optional<std::string> const rel = target ? renderer.rel_path_for(*target) : std::nullopt;
            stack.push_back(Step { left, rel.value_or(std::string()), links[i].match, depth });
        }
    };
    push_links(test, 1);
    while (!stack.empty()) {
        Step const step = stack.back();
        stack.pop_back();
        if (step.right_rel.empty()) {
            outcome.reason = "reference outside the checkout";
            continue;
        }
        std::shared_ptr<Rendered const> const right = renderer.render(step.right_rel);
        if (!right) {
            outcome.reason = "missing reference " + step.right_rel;
            continue;
        }
        Difference const difference = compare(*step.left->bitmap, *right->bitmap);
        bool const equal = equal_within(difference, fuzzy_for(step.left->info, right->url.serialize()));
        outcome.left = step.left->bitmap;
        outcome.right = right->bitmap;
        outcome.right_rel = step.right_rel;
        if (step.match != equal) {
            outcome.reason = (step.match ? "differs from " : "matches ") + step.right_rel + " ("
                + std::to_string(difference.pixels) + " px, max " + std::to_string(difference.max_channel) + ")";
            continue;
        }
        if (right->info.links.empty() || step.depth >= max_chain_depth) {
            outcome.pass = true;
            outcome.reason.clear();
            return outcome;
        }
        push_links(right, step.depth + 1);
    }
    if (outcome.reason.empty())
        outcome.reason = "no reference held";
    return outcome;
}

// --- The run ---------------------------------------------------------------------------------------

struct DirectoryScore {
    std::string path;
    long passed = 0;
    long total = 0;
};

std::vector<std::string> read_lines(std::filesystem::path const& path)
{
    std::vector<std::string> lines;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        std::string const clean = trimmed(line);
        if (!clean.empty() && clean[0] != '#')
            lines.push_back(clean);
    }
    return lines;
}

std::string json_escaped(std::string const& text)
{
    std::string out;
    for (char const c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

std::string html_escaped(std::string const& text)
{
    std::string out;
    for (char const c : text) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out += c;
    }
    return out;
}

std::string today()
{
    std::time_t const now = std::time(nullptr);
    char buffer[16] = {};
    if (std::strftime(buffer, sizeof buffer, "%Y-%m-%d", std::gmtime(&now)) == 0)
        return "";
    return buffer;
}

std::string percent_of(long passed, long total)
{
    if (total == 0)
        return "0.0%";
    char buffer[16] = {};
    std::snprintf(buffer, sizeof buffer, "%.1f%%", 100.0 * static_cast<double>(passed) / static_cast<double>(total));
    return buffer;
}

void write_json(std::filesystem::path const& path, std::vector<DirectoryScore> const& scores,
    long passed, long total, std::string const& revision)
{
    std::ofstream out(path, std::ios::binary);
    out << "{\n  \"revision\": \"" << json_escaped(revision) << "\",\n  \"date\": \"" << today()
        << "\",\n  \"viewport\": [" << viewport_width << ", " << viewport_height << "],\n  \"passed\": " << passed
        << ",\n  \"total\": " << total << ",\n  \"directories\": [\n";
    for (std::size_t i = 0; i < scores.size(); ++i) {
        out << "    { \"path\": \"" << json_escaped(scores[i].path) << "\", \"passed\": " << scores[i].passed
            << ", \"total\": " << scores[i].total << " }" << (i + 1 < scores.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

// The public page: the number, then a row per directory. Hand-written
// markup in the landing page's manner, no scripts.
void write_html(std::filesystem::path const& path, std::vector<DirectoryScore> const& scores,
    long passed, long total, std::string const& revision)
{
    std::ofstream out(path, std::ios::binary);
    out << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
           "<title>Sashfold — Web Platform Tests</title>\n"
           "<meta name=\"description\" content=\"Sashfold's pass rate on the Web Platform Tests' CSS reference tests, by directory.\">\n"
           "<link rel=\"icon\" type=\"image/png\" href=\"icon-160.png\">\n"
           "<link rel=\"canonical\" href=\"https://sashfold.com/wpt.html\">\n"
           "<style>\n"
           "  :root { --bg: #f7f9fc; --panel: #ffffff; --ink: #16233c; --muted: #4e5d78; --navy: #16305e;\n"
           "          --blue: #1e63c4; --sky: #37b6e8; --accent: #f49c3c; --line: #d8e1ee; --code-bg: #eef3fa; }\n"
           "  @media (prefers-color-scheme: dark) {\n"
           "    :root { --bg: #0d1524; --panel: #131e33; --ink: #e6edf7; --muted: #9fb0c9; --navy: #a8c4ef;\n"
           "            --blue: #5b9bea; --sky: #4cc3ef; --accent: #f5aa55; --line: #24334f; --code-bg: #1a2740; }\n"
           "  }\n"
           "  * { box-sizing: border-box; }\n"
           "  body { margin: 0; background: var(--bg); color: var(--ink);\n"
           "         font: 17px/1.65 system-ui, \"Segoe UI\", Roboto, Helvetica, Arial, sans-serif; }\n"
           "  main { max-width: 880px; margin: 0 auto; padding: 0 20px 64px; }\n"
           "  a { color: var(--blue); }\n"
           "  a:hover { color: var(--sky); }\n"
           "  h1, h2 { color: var(--navy); line-height: 1.2; }\n"
           "  h1 { margin-top: 48px; }\n"
           "  code { font-family: ui-monospace, \"Cascadia Code\", Consolas, monospace; background: var(--code-bg);\n"
           "         padding: 1px 6px; border-radius: 4px; font-size: 0.92em; }\n"
           "  .stat { display: inline-block; background: var(--panel); border: 1px solid var(--line);\n"
           "          border-radius: 10px; padding: 14px 20px; margin: 8px 0 20px; }\n"
           "  .stat .big { font-size: 1.8rem; font-weight: 700; color: var(--blue); display: block; }\n"
           "  .stat .what { color: var(--muted); font-size: 0.92rem; }\n"
           "  table { border-collapse: collapse; width: 100%; margin: 20px 0; }\n"
           "  th, td { text-align: left; padding: 8px 10px; border-bottom: 1px solid var(--line); }\n"
           "  th { color: var(--muted); font-weight: 600; font-size: 0.92rem; }\n"
           "  td.num { text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }\n"
           "  .bar { background: var(--code-bg); border-radius: 4px; height: 10px; width: 100%; min-width: 120px; }\n"
           "  .bar span { display: block; height: 10px; border-radius: 4px; background: var(--blue); }\n"
           "  p.muted { color: var(--muted); font-size: 0.95rem; }\n"
           "</style>\n</head>\n<body>\n<main>\n"
           "<p><a href=\"./\">← sashfold.com</a></p>\n"
           "<h1>Web Platform Tests</h1>\n"
           "<p>Sashfold's pass rate on the CSS <em>reference tests</em> of the <a href=\"https://github.com/web-platform-tests/wpt\">Web Platform Tests</a> — "
           "the tests that need no scripting: a test page and a reference page must render to the same pixels. "
           "Every test and every reference is rendered by Sashfold itself, headless, at the suite's 800×600 viewport, "
           "with the built-in face and the fonts the tests bring along. The comparison follows the suite's own rules "
           "(pixel-identical unless a test declares a fuzziness allowance; references may chain).</p>\n";
    out << "<div class=\"stat\"><span class=\"big\">" << passed << " / " << total << "</span><span class=\"what\">CSS reference tests passing — "
        << percent_of(passed, total) << "</span></div>\n";
    out << "<table>\n<thead><tr><th>Directory</th><th></th><th>Passing</th><th>Rate</th></tr></thead>\n<tbody>\n";
    for (DirectoryScore const& score : scores) {
        int const width = score.total == 0 ? 0 : static_cast<int>(100 * score.passed / score.total);
        out << "<tr><td><code>" << html_escaped(score.path) << "</code></td><td><div class=\"bar\"><span style=\"width: "
            << width << "%\"></span></div></td><td class=\"num\">" << score.passed << " / " << score.total
            << "</td><td class=\"num\">" << percent_of(score.passed, score.total) << "</td></tr>\n";
    }
    out << "</tbody>\n</table>\n";
    out << "<p class=\"muted\">Scored " << today() << " against WPT revision <code>" << html_escaped(revision.substr(0, 12))
        << "</code>. The count is every reference test the suite's manifest rules find under these directories, "
           "including the ones that wait on scripts (Sashfold runs none yet, so those fail). XHTML test files are parsed "
           "by the HTML parser. The number is enforced in CI: a baseline names every passing test, and a test that stops "
           "passing fails the build. What the engine cannot do, it does not score.</p>\n"
           "</main>\n</body>\n</html>\n";
}

void usage(char const* program)
{
    std::cerr << "usage: " << program
              << " <wpt-checkout> <directories-file> <baseline-file> [--update] [--only <text>]\n"
                 "       [--json <file>] [--html <file>] [--revision <file>] [--dump <dir>] [--print <n>]\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        usage(argv[0]);
        return 2;
    }
    std::filesystem::path const root = argv[1];
    std::filesystem::path const directories_file = argv[2];
    std::filesystem::path const baseline_file = argv[3];
    bool update = false;
    std::string only;
    std::string json_path;
    std::string html_path;
    std::string revision_file;
    std::string dump_dir;
    int max_printed = 20;
    if (char const* env = std::getenv("SASHFOLD_PRINT_FAILURES"))
        max_printed = std::atoi(env);
    for (int i = 4; i < argc; ++i) {
        std::string const arg = argv[i];
        auto const value = [&](std::string& into) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                std::exit(2);
            }
            into = argv[++i];
        };
        if (arg == "--update")
            update = true;
        else if (arg == "--only")
            value(only);
        else if (arg == "--json")
            value(json_path);
        else if (arg == "--html")
            value(html_path);
        else if (arg == "--revision")
            value(revision_file);
        else if (arg == "--dump")
            value(dump_dir);
        else if (arg == "--print") {
            std::string text;
            value(text);
            max_printed = std::atoi(text.c_str());
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    std::vector<std::string> const directories = read_lines(directories_file);
    if (directories.empty()) {
        std::cerr << "no directories listed in " << directories_file << "\n";
        return 2;
    }
    std::string revision;
    if (!revision_file.empty()) {
        std::vector<std::string> const lines = read_lines(revision_file);
        if (!lines.empty())
            revision = lines.front();
    }

    // The tests: the reftest candidates by the manifest's naming rules that
    // declare a reference. Every file is read once here; the parse waits
    // for the run.
    auto const started = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, std::size_t>> tests; // rel path, directory index
    for (std::size_t d = 0; d < directories.size(); ++d) {
        std::filesystem::path const directory = root / directories[d];
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) {
            std::cerr << "not a directory: " << directory.string() << " (run tools/wpt-fetch.sh)\n";
            return 2;
        }
        for (auto const& entry : std::filesystem::recursive_directory_iterator(directory, error)) {
            if (!entry.is_regular_file())
                continue;
            std::string const rel = entry.path().lexically_relative(root).generic_string();
            if (!is_test_candidate(rel))
                continue;
            if (!only.empty() && rel.find(only) == std::string::npos)
                continue;
            std::optional<std::string> const source = read_file(entry.path());
            if (!source || source->find("match") == std::string::npos)
                continue; // no rel=match or rel=mismatch anywhere: not a reftest
            std::optional<net::Url> const url = Renderer::url_for(rel);
            if (!url)
                continue;
            PageInfo info;
            gather_info(*html::parse_document_bytes(*source), *url, info);
            if (info.links.empty())
                continue;
            tests.emplace_back(rel, d);
        }
    }
    std::sort(tests.begin(), tests.end());
    if (tests.empty()) {
        std::cerr << "no reference tests found\n";
        return 2;
    }
    auto const discovered = std::chrono::steady_clock::now();
    std::cout << tests.size() << " reference tests under " << directories.size() << " directories ("
              << static_cast<long>(std::chrono::duration<double>(discovered - started).count()) << " s to find)\n";

    text::FontManager::instance().set_system_fonts(false);
    // Ahem is the suite's measuring stick: an em square filled edge to edge,
    // an 800/200 ascent and descent per 1000 units, one em of advance for
    // every glyph. A test that names it without declaring it is written
    // against those numbers and expects it installed, which is how the
    // suite's own runner provisions it.
    std::filesystem::path const ahem = root / "fonts" / "Ahem.ttf";
    if (std::filesystem::exists(ahem)) {
        text::FontManager::instance().add_font_file(ahem.string());
    } else {
        std::cerr << "warning: " << ahem.string()
                  << " is missing; tests written against Ahem cannot be scored\n";
    }
    Renderer renderer(root);
    std::vector<DirectoryScore> scores;
    for (std::string const& directory : directories)
        scores.push_back(DirectoryScore { directory, 0, 0 });
    std::set<std::string> passing;
    std::vector<std::pair<std::string, std::string>> failures; // rel path, reason
    std::size_t done = 0;
    for (auto const& [rel, d] : tests) {
        Outcome const outcome = evaluate(renderer, rel);
        ++scores[d].total;
        if (outcome.pass) {
            ++scores[d].passed;
            passing.insert(rel);
        } else {
            failures.emplace_back(rel, outcome.reason);
            if (!dump_dir.empty() && outcome.left && outcome.right) {
                std::filesystem::path const dump(dump_dir);
                std::error_code error;
                std::filesystem::create_directories(dump, error);
                std::string name = rel;
                std::replace(name.begin(), name.end(), '/', '_');
                write_png((dump / (name + ".test.png")).string(), *outcome.left);
                write_png((dump / (name + ".ref.png")).string(), *outcome.right);
            }
        }
        if (++done % 1000 == 0)
            std::cerr << "  " << done << " / " << tests.size() << "\n";
    }
    auto const elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - discovered).count();

    long passed = 0;
    long total = 0;
    for (DirectoryScore const& score : scores) {
        std::printf("  %-28s %6ld / %6ld  %6s\n", score.path.c_str(), score.passed, score.total,
            percent_of(score.passed, score.total).c_str());
        passed += score.passed;
        total += score.total;
    }
    std::printf("  %-28s %6ld / %6ld  %6s\n", "TOTAL", passed, total, percent_of(passed, total).c_str());
    std::printf("%zu renders in %.1f s\n", renderer.renders, elapsed);

    int printed = 0;
    for (auto const& [rel, reason] : failures) {
        if (printed++ >= max_printed)
            break;
        std::cout << "FAIL " << rel << ": " << reason << "\n";
    }
    if (printed < static_cast<int>(failures.size()))
        std::cout << "(" << failures.size() - static_cast<std::size_t>(printed)
                  << " more failures; SASHFOLD_PRINT_FAILURES=n or --print n shows them)\n";

    if (!json_path.empty())
        write_json(json_path, scores, passed, total, revision);
    if (!html_path.empty())
        write_html(html_path, scores, passed, total, revision);

    // The baseline: every test that passed when it was last blessed. With
    // --only the run is partial, so the verdict is informational.
    std::set<std::string> const baseline = [&] {
        std::vector<std::string> const lines = read_lines(baseline_file);
        return std::set<std::string>(lines.begin(), lines.end());
    }();
    std::set<std::string> ran;
    for (auto const& [rel, d] : tests)
        ran.insert(rel);
    std::vector<std::string> regressions;
    std::vector<std::string> new_passes;
    for (std::string const& rel : baseline) {
        if (ran.contains(rel) && !passing.contains(rel))
            regressions.push_back(rel);
    }
    for (std::string const& rel : passing) {
        if (!baseline.contains(rel))
            new_passes.push_back(rel);
    }
    long gone = 0;
    for (std::string const& rel : baseline) {
        if (!ran.contains(rel))
            ++gone;
    }

    if (update) {
        if (!only.empty()) {
            std::cerr << "--update needs the whole run, not --only\n";
            return 2;
        }
        std::ofstream out(baseline_file, std::ios::binary);
        out << "# WPT CSS reference tests passing (" << passed << " of " << total << ") at WPT revision "
            << (revision.empty() ? std::string("(unrecorded)") : revision) << ",\n"
            << "# viewport " << viewport_width << "x" << viewport_height
            << ". One test per line; a listed test that fails is a regression.\n"
            << "# Re-bless with: wpt_reftest <checkout> <directories> <this file> --update\n";
        for (std::string const& rel : passing)
            out << rel << "\n";
        std::cout << "blessed " << passing.size() << " passing tests into " << baseline_file.string() << "\n";
        return 0;
    }

    if (!only.empty()) {
        std::cout << "(partial run: the baseline is not enforced)\n";
        return 0;
    }
    if (!regressions.empty()) {
        std::cerr << "REGRESSION: " << regressions.size() << " test(s) in the baseline no longer pass:\n";
        for (std::string const& rel : regressions)
            std::cerr << "  " << rel << "\n";
        return 1;
    }
    if (gone > 0)
        std::cout << gone << " baseline test(s) are no longer in the checkout (a different revision?)\n";
    if (!new_passes.empty())
        std::cout << "RATCHET: " << new_passes.size() << " new pass(es) — bless them with --update\n";
    std::cout << "baseline held: " << (baseline.size() - static_cast<std::size_t>(gone)) << " tests still pass\n";
    return 0;
}
