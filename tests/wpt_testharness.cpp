#include "bindings/LayoutOracle.h"
#include "bindings/Realm.h"
#include "css/Stylesheets.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "net/Url.h"
#include "text/FontManager.h"
#include "ui/Frames.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Scores the engine on the Web Platform Tests' testharness.js tests. Every
// test under the listed directories that loads /resources/testharness.js —
// a page, or a .any.js or .window.js script the suite's server would wrap
// in one — is loaded as if served from http://web-platform.test:8000/, its
// scripts run, and its event loop pumped on a virtual clock until the
// harness reports; each subtest's verdict is the harness's own, handed to
// the runner by the testharnessreport.js it serves in the suite's place.
// The unit scored is the subtest, as the suite's dashboards count them. The
// committed baseline lists every passing subtest: one that stops passing is
// a regression, a new pass is a ratchet to bless with --update.
//
// Everything comes from the checkout: any host under web-platform.test or
// not-web-platform.test on any port is the same tree, so a cross-origin test
// finds its other origin there; the server's substitutions ({{host}},
// {{domains[www]}}, {{ports[http][0]}}, {{location[path]}}, {{GET[name]}})
// are applied to .sub. files, and a .headers file's headers travel with the
// response. Nothing touches the network, and no server-side handler runs, so
// a test that needs one ends as the harness times it out.

using namespace sashfold;

namespace {

constexpr int viewport_width = 800;
constexpr int viewport_height = 600;
constexpr std::string_view origin = "http://web-platform.test:8000";
constexpr std::string_view host_name = "web-platform.test";
constexpr std::string_view alt_host_name = "not-web-platform.test";
constexpr auto test_deadline = std::chrono::seconds(5);
constexpr int max_pumps = 20000;

// Layout runs on the process's one font manager, so the tests' layout
// questions take turns; their scripts run side by side.
std::mutex layout_mutex;
constexpr std::string_view results_marker = "__sashfold_results\n";

// The suite's completion callback, in the runner's hands: every result goes
// to the console as one line per subtest, the fields escaped.
constexpr std::string_view report_script = R"JS(
setup({ output: false });
add_completion_callback(function (tests, status) {
    function field(text) {
        return String(text == null ? "" : text).replace(/\\/g, "\\\\").replace(/\t/g, "\\t").replace(/\n/g, "\\n").replace(/\r/g, "\\r");
    }
    var lines = ["harness\t" + status.status + "\t" + field(status.message)];
    for (var i = 0; i < tests.length; i++)
        lines.push("test\t" + tests[i].status + "\t" + field(tests[i].name) + "\t" + field(tests[i].message));
    console.log("__sashfold_results\n" + lines.join("\n"));
});
)JS";

std::optional<std::string> read_file(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::ostringstream stream;
    stream << file.rdbuf();
    return std::move(stream).str();
}

std::vector<std::string> read_lines(std::filesystem::path const& path)
{
    std::vector<std::string> lines;
    std::optional<std::string> const text = read_file(path);
    if (!text)
        return lines;
    std::string current;
    for (char const c : *text) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r')
                current.pop_back();
            if (!current.empty() && current[0] != '#')
                lines.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty() && current[0] != '#')
        lines.push_back(current);
    return lines;
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

// The report's field escaping, undone; and done again for the baseline.
std::string unescape_field(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out += text[i];
            continue;
        }
        char const next = text[++i];
        out += next == 't' ? '\t' : next == 'n' ? '\n' : next == 'r' ? '\r' : next;
    }
    return out;
}

std::string escape_field(std::string_view text)
{
    std::string out;
    for (char const c : text) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\t')
            out += "\\t";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else
            out += c;
    }
    return out;
}

std::string json_escaped(std::string_view text)
{
    std::string out;
    for (char const c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (c == '\n')
            out += "\\n";
        else if (c == '\t')
            out += "\\t";
        else if (static_cast<unsigned char>(c) < 0x20)
            out += ' ';
        else
            out += c;
    }
    return out;
}

std::string html_escaped(std::string_view text)
{
    std::string out;
    for (char const c : text) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else if (c == '"')
            out += "&quot;";
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

// --- The server's substitutions -----------------------------------------------------------

// The suite's server configuration, as far as tests read it through {{…}}:
// its host, the subdomains it answers on, the second host of another site,
// and its ports by scheme.
std::string domain_for(std::string_view subdomain, std::string_view base)
{
    if (subdomain.empty())
        return std::string(base);
    // The two non-ASCII subdomains the suite's server configures, as the
    // URL parser would write them.
    if (subdomain == "élève")
        return "xn--lve-6lad." + std::string(base);
    if (subdomain == "天気の良い日")
        return "xn--n8j6ds53lwwkrqhv28a." + std::string(base);
    return std::string(subdomain) + "." + std::string(base);
}

std::string port_for(std::string_view scheme, std::size_t index)
{
    static std::map<std::string, std::vector<int>, std::less<>> const ports = {
        { "http", { 8000, 8001 } }, { "http-private", { 8002 } }, { "http-public", { 8003 } },
        { "https", { 8443, 8444 } }, { "https-private", { 8445 } }, { "https-public", { 8446 } },
        { "ws", { 8880, 8881 } }, { "wss", { 8888, 8889 } }, { "h2", { 9000 } }, { "webtransport-h3", { 11000 } },
    };
    auto const it = ports.find(scheme);
    if (it == ports.end() || index >= it->second.size())
        return "";
    return std::to_string(it->second[index]);
}

// One {{…}} expression, for the request at `url`; nullopt leaves it as it is.
std::optional<std::string> substitution(std::string_view expression, net::Url const& url,
    std::map<std::string, std::string>& variables, int& uuids)
{
    std::string const text = trimmed(expression);
    if (text.starts_with("$")) {
        std::size_t const colon = text.find(':');
        if (colon == std::string::npos) {
            auto const it = variables.find(text.substr(1));
            return it == variables.end() ? std::optional<std::string>(std::nullopt) : std::optional<std::string>(it->second);
        }
        std::optional<std::string> const value = substitution(text.substr(colon + 1), url, variables, uuids);
        if (value)
            variables[text.substr(1, colon - 1)] = *value;
        return value;
    }
    // name, name[index], name[index][index]
    std::string name = text;
    std::vector<std::string> indexes;
    if (std::size_t const bracket = text.find('['); bracket != std::string::npos) {
        name = text.substr(0, bracket);
        std::size_t at = bracket;
        while (at < text.size() && text[at] == '[') {
            std::size_t const close = text.find(']', at);
            if (close == std::string::npos)
                return std::nullopt;
            indexes.push_back(text.substr(at + 1, close - at - 1));
            at = close + 1;
        }
    }
    auto const index_at = [&](std::size_t i) { return i < indexes.size() ? indexes[i] : std::string(); };
    if (name == "host")
        return std::string(host_name);
    if (name == "domains")
        return domain_for(index_at(0), host_name);
    if (name == "hosts") {
        std::string const base = index_at(0) == "alt" ? std::string(alt_host_name) : std::string(host_name);
        return domain_for(index_at(1), base);
    }
    if (name == "ports") {
        std::size_t const index = indexes.size() > 1 ? static_cast<std::size_t>(std::atoi(indexes[1].c_str())) : 0;
        return port_for(index_at(0), index);
    }
    if (name == "location") {
        std::string const field = index_at(0);
        if (field == "scheme")
            return url.scheme;
        if (field == "host")
            return url.host_with_port();
        if (field == "hostname")
            return url.serialize_host();
        if (field == "port")
            return url.port_string();
        if (field == "path")
            return url.serialize_path();
        if (field == "query")
            return url.query ? *url.query : std::string();
        if (field == "server")
            return url.scheme + "://" + url.host_with_port();
        return std::nullopt;
    }
    if (name == "GET") {
        std::string const wanted = index_at(0);
        for (std::string const& pair : split(url.query ? *url.query : std::string(), '&')) {
            std::size_t const equals = pair.find('=');
            if (pair.substr(0, equals) == wanted)
                return equals == std::string::npos ? std::string() : percent_decode(pair.substr(equals + 1));
        }
        return std::string();
    }
    if (name == "url_base")
        return std::string("/");
    if (name.starts_with("uuid(")) {
        std::string const number = std::to_string(++uuids);
        return "00000000-0000-4000-8000-" + std::string(12 - std::min<std::size_t>(12, number.size()), '0') + number;
    }
    if (name.starts_with("header_or_default(")) {
        std::size_t const comma = name.find(',');
        std::size_t const close = name.rfind(')');
        if (comma == std::string::npos || close == std::string::npos || close < comma)
            return std::nullopt;
        return trimmed(name.substr(comma + 1, close - comma - 1));
    }
    if (name.starts_with("header("))
        return std::string();
    return std::nullopt;
}

// The `sub` pipe the server applies to every .sub. file.
std::string substitute(std::string const& text, net::Url const& url)
{
    std::map<std::string, std::string> variables;
    int uuids = 0;
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t const open = text.find("{{", at);
        if (open == std::string::npos) {
            out.append(text, at, std::string::npos);
            break;
        }
        std::size_t const close = text.find("}}", open + 2);
        if (close == std::string::npos) {
            out.append(text, at, std::string::npos);
            break;
        }
        out.append(text, at, open - at);
        std::optional<std::string> const value = substitution(text.substr(open + 2, close - open - 2), url, variables, uuids);
        if (value)
            out += *value;
        else
            out.append(text, open, close + 2 - open);
        at = close + 2;
    }
    return out;
}

// --- Serving the checkout ------------------------------------------------------------------

struct Served {
    std::string body;
    std::string content_type;
    std::vector<net::Header> headers;
};

std::string content_type_for(std::string const& rel)
{
    std::string const path = lowercased(rel);
    auto const ends = [&](std::string_view suffix) { return path.ends_with(suffix); };
    if (ends(".html") || ends(".htm"))
        return "text/html";
    if (ends(".xhtml") || ends(".xht"))
        return "application/xhtml+xml";
    if (ends(".js") || ends(".mjs"))
        return "text/javascript";
    if (ends(".css"))
        return "text/css";
    if (ends(".json"))
        return "application/json";
    if (ends(".txt"))
        return "text/plain";
    if (ends(".svg"))
        return "image/svg+xml";
    if (ends(".png"))
        return "image/png";
    if (ends(".gif"))
        return "image/gif";
    if (ends(".jpg") || ends(".jpeg"))
        return "image/jpeg";
    if (ends(".xml"))
        return "text/xml";
    return "application/octet-stream";
}

// The `// META:` lines at the top of a .any.js or .window.js script.
std::vector<std::pair<std::string, std::string>> script_metadata(std::string const& source)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t at = 0;
    while (at < source.size()) {
        std::size_t const end = source.find('\n', at);
        std::string const line = trimmed(source.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (!line.starts_with("//"))
            break;
        if (line.starts_with("// META:")) {
            std::string const rest = trimmed(line.substr(8));
            std::size_t const equals = rest.find('=');
            if (equals != std::string::npos)
                out.emplace_back(trimmed(rest.substr(0, equals)), trimmed(rest.substr(equals + 1)));
        }
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return out;
}

// Whether a .any.js script has a window variant: it does unless a global
// line names other globals only.
bool wants_window(std::string const& source)
{
    for (auto const& [key, value] : script_metadata(source)) {
        if (key != "global")
            continue;
        for (std::string const& global : split(value, ',')) {
            std::string const name = trimmed(global);
            if (name == "window")
                return true;
        }
        return false;
    }
    return true;
}

class Server {
public:
    explicit Server(std::filesystem::path root)
        : m_root(std::move(root))
    {
    }

    // The checkout-relative path a URL names, when it names one inside it:
    // every host of the suite's two sites is the same tree.
    std::optional<std::string> rel_path_for(net::Url const& url) const
    {
        if (url.scheme != "http" && url.scheme != "https")
            return std::nullopt;
        if (url.host != host_name && !url.host.ends_with(std::string(".") + std::string(host_name))
            && url.host != alt_host_name && !url.host.ends_with(std::string(".") + std::string(alt_host_name)))
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

    std::optional<Served> serve(net::Url const& url) const
    {
        std::optional<std::string> const rel = rel_path_for(url);
        if (!rel)
            return std::nullopt;
        if (*rel == "resources/testharnessreport.js")
            return Served { std::string(report_script), "text/javascript", {} };
        if (rel->ends_with(".any.html"))
            return wrapper(*rel, rel->substr(0, rel->size() - 9) + ".any.js", true, url);
        if (rel->ends_with(".window.html"))
            return wrapper(*rel, rel->substr(0, rel->size() - 12) + ".window.js", false, url);
        // A directory is answered with a listing of its entries, as the
        // suite's server answers one, rather than with an empty body: a
        // frame pointed at another site's root has a document of that origin.
        std::error_code error;
        if (std::filesystem::is_directory(m_root / *rel, error))
            return Served { directory_listing(*rel), "text/html", {} };
        std::optional<std::string> body = read_file(m_root / *rel);
        if (!body)
            return std::nullopt;
        if (rel->find(".sub.") != std::string::npos)
            body = substitute(*body, url);
        Served served { std::move(*body), content_type_for(*rel), headers_for(*rel, url) };
        if (std::string const* type = net::find_header(served.headers, "Content-Type"))
            served.content_type = *type;
        return served;
    }

    std::filesystem::path const& root() const { return m_root; }

private:
    // The page the suite's server makes for a directory: a link to each
    // entry, the subdirectories marked with a slash.
    std::string directory_listing(std::string const& rel) const
    {
        auto const escaped = [](std::string const& text) {
            std::string out;
            for (char const c : text) {
                if (c == '&')
                    out += "&amp;";
                else if (c == '<')
                    out += "&lt;";
                else if (c == '>')
                    out += "&gt;";
                else if (c == '"')
                    out += "&quot;";
                else
                    out += c;
            }
            return out;
        };
        std::vector<std::string> names;
        std::error_code error;
        for (auto const& entry : std::filesystem::directory_iterator(m_root / rel, error))
            names.push_back(entry.path().filename().string() + (entry.is_directory(error) ? "/" : ""));
        std::sort(names.begin(), names.end());
        std::string const path = "/" + rel + (rel.empty() || rel.ends_with('/') ? "" : "/");
        std::string page = "<!DOCTYPE html>\n<title>Directory listing for " + escaped(path) + "</title>\n<h1>Directory listing for "
            + escaped(path) + "</h1>\n<ul>\n";
        if (!rel.empty())
            page += "<li class=\"dir\"><a href=\"..\">..</a></li>\n";
        for (std::string const& name : names)
            page += "<li class=\"" + std::string(name.ends_with('/') ? "dir" : "file") + "\"><a href=\"" + escaped(name) + "\">" + escaped(name) + "</a></li>\n";
        return page + "</ul>\n";
    }

    // A .headers file beside a resource: one header per line.
    std::vector<net::Header> headers_for(std::string const& rel, net::Url const& url) const
    {
        std::vector<net::Header> headers;
        std::optional<std::string> text = read_file(m_root / (rel + ".headers"));
        if (!text)
            return headers;
        if (rel.find(".sub.") != std::string::npos)
            text = substitute(*text, url);
        for (std::string const& line : split(*text, '\n')) {
            std::size_t const colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            headers.push_back(net::Header { trimmed(line.substr(0, colon)), trimmed(line.substr(colon + 1)) });
        }
        return headers;
    }

    // The page the server generates around a .any.js or .window.js script.
    std::optional<Served> wrapper(std::string const& rel, std::string const& script_rel, bool any, net::Url const& url) const
    {
        std::optional<std::string> const source = read_file(m_root / script_rel);
        if (!source)
            return std::nullopt;
        std::string meta;
        std::string scripts;
        for (auto const& [key, value] : script_metadata(*source)) {
            if (key == "timeout" && value == "long")
                meta += "<meta name=\"timeout\" content=\"long\">\n";
            else if (key == "title")
                meta += "<title>" + html_escaped(value) + "</title>\n";
            else if (key == "script")
                scripts += "<script src=\"" + html_escaped(value) + "\"></script>\n";
        }
        std::string body = "<!doctype html>\n<meta charset=utf-8>\n" + meta;
        if (any)
            body += "<script>\nself.GLOBAL = {\n  isWindow: function() { return true; },\n  isWorker: function() { return false; },\n"
                    "  isShadowRealm: function() { return false; },\n};\n</script>\n";
        body += "<script src=\"/resources/testharness.js\"></script>\n<script src=\"/resources/testharnessreport.js\"></script>\n"
            + scripts + "<div id=log></div>\n<script src=\"/" + html_escaped(script_rel) + url.search() + "\"></script>\n";
        return Served { std::move(body), "text/html", headers_for(rel, url) };
    }

    std::filesystem::path m_root;
};

// --- Which files are tests -----------------------------------------------------------------

bool excluded_path(std::vector<std::string> const& parts)
{
    if (parts.size() < 2 || parts[0] == "common" || parts[0] == "resources")
        return true;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] == "resources" || parts[i] == "support" || parts[i] == "tools" || parts[i] == "crashtests")
            return true;
    }
    return false;
}

// The test's id — the path the suite's manifest names it by — for a file
// under the checkout, or nothing for a file that is not a testharness test.
std::optional<std::string> test_id_for(std::filesystem::path const& root, std::string const& rel)
{
    std::vector<std::string> const parts = split(rel, '/');
    if (excluded_path(parts))
        return std::nullopt;
    std::string const& filename = parts.back();
    if (filename.starts_with(".") || filename.starts_with("MANIFEST"))
        return std::nullopt;
    std::string const lower = lowercased(filename);
    if (lower.ends_with(".any.js")) {
        std::optional<std::string> const source = read_file(root / rel);
        if (!source || !wants_window(*source))
            return std::nullopt;
        return rel.substr(0, rel.size() - 7) + ".any.html";
    }
    if (lower.ends_with(".window.js"))
        return rel.substr(0, rel.size() - 10) + ".window.html";
    if (!(lower.ends_with(".html") || lower.ends_with(".htm") || lower.ends_with(".xhtml") || lower.ends_with(".xht")))
        return std::nullopt;
    std::string const stem = std::filesystem::path(filename).stem().string();
    if (stem.ends_with("-manual") || stem.ends_with("-ref"))
        return std::nullopt;
    std::optional<std::string> const source = read_file(root / rel);
    if (!source || source->find("testharness.js") == std::string::npos)
        return std::nullopt;
    return rel;
}

// --- Running one test ---------------------------------------------------------------------

struct SubtestResult {
    std::string name;
    int status = 3; // the harness's: 0 pass, 1 fail, 2 timeout, 3 not run, 4 precondition failed
    std::string message;
};

struct TestResult {
    bool completed = false; // the harness reported
    int harness_status = 0; // 0 ok, 1 error, 2 timeout, 3 precondition failed
    std::string harness_message; // or why it never reported
    std::vector<SubtestResult> subtests;
    double seconds = 0;
};

std::string status_name(int status)
{
    switch (status) {
    case 0: return "PASS";
    case 1: return "FAIL";
    case 2: return "TIMEOUT";
    case 3: return "NOTRUN";
    default: return "PRECONDITION_FAILED";
    }
}

void parse_report(std::string_view report, TestResult& result)
{
    for (std::string const& line : split(report, '\n')) {
        std::vector<std::string> const fields = split(line, '\t');
        if (fields.size() >= 3 && fields[0] == "harness") {
            result.completed = true;
            result.harness_status = std::atoi(fields[1].c_str());
            result.harness_message = unescape_field(fields[2]);
        } else if (fields.size() >= 4 && fields[0] == "test") {
            result.subtests.push_back(SubtestResult { unescape_field(fields[2]), std::atoi(fields[1].c_str()), unescape_field(fields[3]) });
        }
    }
}

TestResult run_test(Server const& server, std::string const& id)
{
    TestResult result;
    auto const started = std::chrono::steady_clock::now();
    std::optional<net::Url> const url = Server::url_for(id);
    std::optional<Served> const page = url ? server.serve(*url) : std::nullopt;
    if (!page) {
        result.harness_message = "the test could not be served";
        return result;
    }
    char const* const trace_env = std::getenv("SASHFOLD_WPT_TRACE");
    bool const trace = trace_env != nullptr;
    bool const deep = trace && std::atoi(trace_env) >= 2; // the event loop and every fetch too
    if (trace)
        std::cerr << "  run " << id << "\n";
    css::MediaContext const media { static_cast<float>(viewport_width), static_cast<float>(viewport_height) };
    auto const fetch_sheet = [&](net::Url const& target, std::string_view) -> std::optional<css::FetchedSheet> {
        std::optional<Served> const served = server.serve(target);
        if (!served)
            return std::nullopt;
        return css::FetchedSheet { std::vector<std::uint8_t>(served->body.begin(), served->body.end()), served->content_type };
    };
    ui::FrameFetcher const frame_fetcher = [&](net::Url const& target, net::Url const&, net::ResourceKind,
                                              net::RequestGuard const&) -> std::optional<ui::FrameResponse> {
        // A .py file is a program the suite's server runs, which this runner
        // cannot: a frame asking for one has no answer, as when a server
        // cannot be reached, rather than a document of the program's text.
        if (target.serialize_path().ends_with(".py"))
            return std::nullopt;
        std::optional<Served> served = server.serve(target);
        if (!served) {
            // A file the suite's server does not have is its 404 answer, a
            // document of the URL's origin like any other; a host it does not
            // serve has no answer at all.
            if (!server.rel_path_for(target))
                return std::nullopt;
            served = Served { R"({"error": {"code": 404, "message": null}})", "application/json", {} };
        }
        return ui::FrameResponse { std::vector<std::uint8_t>(served->body.begin(), served->body.end()),
            served->content_type, target, std::move(served->headers) };
    };
    auto document = std::make_unique<dom::Document>();
    bindings::LayoutOracle oracle(*document, *url, fetch_sheet, media);
    double clock = 0;
    std::optional<std::string> report;
    bool stopped = false;
    bindings::HostHooks hooks;
    hooks.fetch_script = [&](net::Url const& target, net::RequestGuard const&) -> std::optional<std::string> {
        std::optional<Served> const served = server.serve(target);
        if (deep)
            std::cerr << "    " << id << " ~ script " << target.serialize() << (served ? " served" : " not found") << "\n";
        if (!served)
            return std::nullopt;
        return served->body;
    };
    hooks.fetch_resource = [&](net::Url const& target, net::ResourceRequest const& request,
                               net::RequestGuard const&) -> net::FetchResult {
        if (deep)
            std::cerr << "    " << id << " ~ fetch " << request.method << " " << target.serialize() << "\n";
        if (request.method != "GET" && request.method != "HEAD")
            return { std::nullopt, "the runner serves files only" };
        std::optional<Served> served = server.serve(target);
        if (!served)
            return { std::nullopt, "not found" };
        net::FetchResponse response;
        response.status = 200;
        response.status_text = "OK";
        response.headers = std::move(served->headers);
        if (!net::find_header(response.headers, "Content-Type"))
            response.headers.push_back(net::Header { "Content-Type", served->content_type });
        if (request.method == "GET")
            response.body.assign(served->body.begin(), served->body.end());
        response.final_url = target;
        return { std::move(response), "" };
    };
    hooks.now = [&clock] { return clock; };
    hooks.should_stop = [&] {
        if (std::chrono::steady_clock::now() - started > test_deadline)
            stopped = true;
        return stopped;
    };
    oracle.install(hooks);
    if (hooks.layout_box) {
        hooks.layout_box = [inner = std::move(hooks.layout_box)](dom::Element const& element) {
            std::lock_guard<std::mutex> const lock(layout_mutex);
            return inner(element);
        };
    }
    if (hooks.computed_style) {
        hooks.computed_style = [inner = std::move(hooks.computed_style)](dom::Element const& element) {
            std::lock_guard<std::mutex> const lock(layout_mutex);
            return inner(element);
        };
    }
    hooks.console = [&](std::string_view level, std::string_view message) {
        if (level == "log" && message.starts_with(results_marker)) {
            report = std::string(message.substr(results_marker.size()));
            return;
        }
        if (trace)
            std::cerr << "    " << id << " console." << level << ": " << message << "\n";
        if (!report && level == "error" && result.harness_message.empty())
            result.harness_message = std::string(message);
    };
    if (deep)
        hooks.trace = [&](std::string_view message) { std::cerr << "    " << id << " ~ " << message << "\n"; };
    hooks.viewport_width = media.width;
    hooks.viewport_height = media.height;
    hooks.user_agent = "Mozilla/5.0 (X11; Linux x86_64) Sashfold/0.0 wpt";
    hooks.frame_document = [&](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const& ancestors, std::optional<net::Url> const& target) {
        return ui::frame_document_for(iframe, base, policy, ancestors, target, frame_fetcher);
    };
    auto realm = std::make_unique<bindings::Realm>(*document, *url, std::move(hooks));
    oracle.set_realm(realm.get());
    html::parse_document_bytes_into(*document, page->body, realm.get());
    realm->document_parsed();
    // The event loop on a virtual clock: the tasks and due timers, then the
    // clock moved to the next timer, until the harness reports, nothing is
    // pending, or the wall-clock deadline passes.
    for (int pump = 0; pump < max_pumps && !report && !stopped; ++pump) {
        bool const ran = realm->run_pending();
        if (report || stopped || realm->interpreter().terminated())
            break;
        if (std::chrono::steady_clock::now() - started > test_deadline) {
            stopped = true;
            break;
        }
        if (ran)
            continue;
        std::optional<double> const due = realm->next_timer_due();
        if (!due)
            break;
        clock = std::max(clock, *due);
    }
    if (report) {
        parse_report(*report, result);
    } else if (result.harness_message.empty()) {
        result.harness_message = stopped ? "the deadline passed before the harness reported"
                                         : "the harness never reported: nothing left to run";
    }
    realm.reset();
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (trace)
        std::cerr << "  done " << id << " " << result.seconds << " s\n";
    return result;
}

// --- Scores and reports ------------------------------------------------------------------

struct DirectoryScore {
    std::string path;
    long passed = 0; // subtests
    long total = 0;
    long files = 0;
    long completed = 0; // files the harness reported on
};

void write_json(std::filesystem::path const& path, std::vector<DirectoryScore> const& scores,
    long passed, long total, long files, long completed, std::string const& revision)
{
    std::ofstream out(path, std::ios::binary);
    out << "{\n  \"revision\": \"" << json_escaped(revision) << "\",\n  \"date\": \"" << today()
        << "\",\n  \"passed\": " << passed << ",\n  \"total\": " << total << ",\n  \"files\": " << files
        << ",\n  \"completed\": " << completed << ",\n  \"directories\": [\n";
    for (std::size_t i = 0; i < scores.size(); ++i) {
        out << "    { \"path\": \"" << json_escaped(scores[i].path) << "\", \"passed\": " << scores[i].passed
            << ", \"total\": " << scores[i].total << ", \"files\": " << scores[i].files << ", \"completed\": "
            << scores[i].completed << " }" << (i + 1 < scores.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

void write_html(std::filesystem::path const& path, std::vector<DirectoryScore> const& scores,
    long passed, long total, long files, long completed, std::string const& revision)
{
    std::ofstream out(path, std::ios::binary);
    out << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
           "<title>Sashfold — Web Platform Tests, scripted</title>\n"
           "<meta name=\"description\" content=\"Sashfold's pass rate on the Web Platform Tests' testharness.js tests, by directory.\">\n"
           "<link rel=\"icon\" type=\"image/png\" href=\"icon-160.png\">\n"
           "<link rel=\"canonical\" href=\"https://sashfold.com/wpt-harness.html\">\n"
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
           "          border-radius: 10px; padding: 14px 20px; margin: 8px 12px 20px 0; }\n"
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
           "<h1>Web Platform Tests, scripted</h1>\n"
           "<p>Sashfold's pass rate on the <em>testharness.js</em> tests of the <a href=\"https://github.com/web-platform-tests/wpt\">Web Platform Tests</a> — "
           "the tests written as scripts, each file holding many subtests that assert what the DOM, the window and the "
           "event loop do. Every test is loaded by Sashfold itself, headless, from the suite's own files, its scripts run "
           "and its event loop pumped until the harness reports; the verdicts are the harness's own. The unit counted is "
           "the subtest, as the suite's dashboards count them.</p>\n";
    out << "<div class=\"stat\"><span class=\"big\">" << passed << " / " << total << "</span><span class=\"what\">subtests passing — "
        << percent_of(passed, total) << "</span></div>\n";
    out << "<div class=\"stat\"><span class=\"big\">" << completed << " / " << files << "</span><span class=\"what\">test files the harness reported on</span></div>\n";
    out << "<table>\n<thead><tr><th>Directory</th><th></th><th>Subtests</th><th>Rate</th><th>Files</th></tr></thead>\n<tbody>\n";
    for (DirectoryScore const& score : scores) {
        int const width = score.total == 0 ? 0 : static_cast<int>(100 * score.passed / score.total);
        out << "<tr><td><code>" << html_escaped(score.path) << "</code></td><td><div class=\"bar\"><span style=\"width: "
            << width << "%\"></span></div></td><td class=\"num\">" << score.passed << " / " << score.total
            << "</td><td class=\"num\">" << percent_of(score.passed, score.total) << "</td><td class=\"num\">"
            << score.completed << " / " << score.files << "</td></tr>\n";
    }
    out << "</tbody>\n</table>\n";
    out << "<p class=\"muted\">Scored " << today() << " against WPT revision <code>" << html_escaped(revision.substr(0, 12))
        << "</code>. The subtests counted are those the harness reported: a test file whose harness never reports "
           "— a script the engine cannot run, a wait on a server the runner does not have — contributes none, and the "
           "files column says how many did. Every host of the suite is served from the same checkout, nothing touches "
           "the network, and no server-side handler runs. The number is enforced in CI: a baseline names every passing "
           "subtest, and one that stops passing fails the build.</p>\n"
           "</main>\n</body>\n</html>\n";
}

void usage(char const* program)
{
    std::cerr << "usage: " << program
              << " <wpt-checkout> <directories-file> <baseline-file> [--update] [--only <text>]\n"
                 "       [--json <file>] [--html <file>] [--revision <file>] [--print <n>] [--jobs <n>]\n"
                 "       [--hang <seconds>] [--messages <n>] [--files <n>] [--accept-losses]\n";
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
    int max_printed = 20;
    if (char const* env = std::getenv("SASHFOLD_PRINT_FAILURES"))
        max_printed = std::atoi(env);
    int jobs = static_cast<int>(std::max(1u, std::thread::hardware_concurrency() / 2));
    long hang_seconds = 60;
    int messages = 0;
    int files_ranked = 0;
    bool accept_losses = false;
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
        else if (arg == "--print") {
            std::string text;
            value(text);
            max_printed = std::atoi(text.c_str());
        } else if (arg == "--jobs") {
            std::string text;
            value(text);
            jobs = std::max(1, std::atoi(text.c_str()));
        } else if (arg == "--hang") {
            std::string text;
            value(text);
            hang_seconds = std::max(1L, std::atol(text.c_str()));
        } else if (arg == "--messages") {
            std::string text;
            value(text);
            messages = std::atoi(text.c_str());
        } else if (arg == "--files") {
            std::string text;
            value(text);
            files_ranked = std::atoi(text.c_str());
        } else if (arg == "--accept-losses") {
            accept_losses = true;
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

    auto const started = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, std::size_t>> tests; // id, directory index
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
            if (!only.empty() && rel.find(only) == std::string::npos)
                continue;
            if (std::optional<std::string> id = test_id_for(root, rel))
                tests.emplace_back(std::move(*id), d);
        }
    }
    std::sort(tests.begin(), tests.end());
    if (tests.empty()) {
        std::cerr << "no testharness tests found\n";
        return 2;
    }
    auto const discovered = std::chrono::steady_clock::now();
    std::cout << tests.size() << " testharness tests under " << directories.size() << " directories ("
              << static_cast<long>(std::chrono::duration<double>(discovered - started).count()) << " s to find)\n";

    text::FontManager::instance().set_system_fonts(false);
    std::filesystem::path const ahem = root / "fonts" / "Ahem.ttf";
    if (std::filesystem::exists(ahem))
        text::FontManager::instance().add_font_file(ahem.string());

    Server const server(root);
    std::vector<DirectoryScore> scores;
    for (std::string const& directory : directories)
        scores.push_back(DirectoryScore { directory });
    std::set<std::string> passing; // "id\tescaped subtest name"
    std::set<std::string> files_seen;
    std::vector<std::pair<std::string, std::string>> incomplete; // id, why
    std::vector<std::pair<std::string, std::string>> failures; // "id › name", message
    // The tests run `jobs` at a time, each in a realm of its own; the
    // results are read off in order afterwards. A watchdog names any test
    // running past the hang limit and aborts: the script deadline cannot stop
    // a hang in the engine's own code, and a silent kill names nothing.
    std::vector<TestResult> results(tests.size());
    {
        std::atomic<std::size_t> next { 0 };
        std::atomic<std::size_t> finished { 0 };
        std::vector<std::atomic<long>> running(static_cast<std::size_t>(jobs)); // test index + 1, 0 = idle
        std::vector<std::atomic<long>> since(static_cast<std::size_t>(jobs)); // seconds into the run
        auto const run_started = std::chrono::steady_clock::now();
        auto const seconds_now = [&] {
            return static_cast<long>(std::chrono::duration<double>(std::chrono::steady_clock::now() - run_started).count());
        };
        std::atomic<bool> all_done { false };
        std::thread watchdog([&] {
            while (!all_done) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                for (std::size_t w = 0; w < running.size(); ++w) {
                    long const index = running[w];
                    if (index > 0 && seconds_now() - since[w] > hang_seconds) {
                        std::cerr << "HANG: " << tests[static_cast<std::size_t>(index - 1)].first << " has run for more than "
                                  << hang_seconds << " s; the engine is stuck outside script (run it alone under a debugger)\n";
                        std::abort();
                    }
                }
            }
        });
        std::vector<std::thread> workers;
        for (int w = 0; w < jobs; ++w) {
            workers.emplace_back([&, w] {
                for (;;) {
                    std::size_t const i = next.fetch_add(1);
                    if (i >= tests.size())
                        return;
                    since[static_cast<std::size_t>(w)] = seconds_now();
                    running[static_cast<std::size_t>(w)] = static_cast<long>(i) + 1;
                    results[i] = run_test(server, tests[i].first);
                    running[static_cast<std::size_t>(w)] = 0;
                    std::size_t const count = finished.fetch_add(1) + 1;
                    if (count % 200 == 0)
                        std::cerr << "  " << count << " / " << tests.size() << "\n";
                }
            });
        }
        for (std::thread& worker : workers)
            worker.join();
        all_done = true;
        watchdog.join();
    }
    for (std::size_t i = 0; i < tests.size(); ++i) {
        std::string const& id = tests[i].first;
        TestResult const& result = results[i];
        DirectoryScore& score = scores[tests[i].second];
        ++score.files;
        files_seen.insert(id);
        if (result.completed)
            ++score.completed;
        else
            incomplete.emplace_back(id, result.harness_message);
        for (SubtestResult const& subtest : result.subtests) {
            ++score.total;
            if (subtest.status == 0) {
                ++score.passed;
                passing.insert(id + "\t" + escape_field(subtest.name));
            } else {
                failures.emplace_back(id + " \xe2\x80\xba " + subtest.name, status_name(subtest.status) + (subtest.message.empty() ? "" : ": " + subtest.message));
            }
        }
        if (result.completed && result.harness_status != 0)
            failures.emplace_back(id + " \xe2\x80\xba (harness)", (result.harness_status == 1 ? "ERROR: " : result.harness_status == 2 ? "TIMEOUT: " : "PRECONDITION FAILED: ") + result.harness_message);
    }
    auto const elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - discovered).count();

    long passed = 0;
    long total = 0;
    long files = 0;
    long completed = 0;
    for (DirectoryScore const& score : scores) {
        std::printf("  %-52s %6ld / %6ld  %6s   files %4ld / %4ld\n", score.path.c_str(), score.passed, score.total,
            percent_of(score.passed, score.total).c_str(), score.completed, score.files);
        passed += score.passed;
        total += score.total;
        files += score.files;
        completed += score.completed;
    }
    std::printf("wpt testharness: %ld / %ld subtests (%s) in %ld files, %ld reported, %.0f s\n", passed, total,
        percent_of(passed, total).c_str(), files, completed, elapsed);
    // The failure messages that recur most: the missing piece worth the most
    // subtests is at the top of this list.
    if (messages > 0) {
        std::map<std::string, long> counts;
        for (auto const& [name, message] : failures)
            ++counts[message];
        std::vector<std::pair<long, std::string>> common;
        for (auto const& [message, count] : counts)
            common.emplace_back(count, message);
        std::sort(common.begin(), common.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        for (std::size_t i = 0; i < common.size() && i < static_cast<std::size_t>(messages); ++i)
            std::printf("  %6ld  %s\n", common[i].first, common[i].second.substr(0, 160).c_str());
    }
    // The files that fail the most subtests, each with the message that
    // recurs most in it: where one missing piece stops a whole file.
    if (files_ranked > 0) {
        std::map<std::string, std::pair<long, std::map<std::string, long>>> by_file;
        for (auto const& [name, message] : failures) {
            std::string const file = name.substr(0, name.find(" \xe2\x80\xba "));
            auto& entry = by_file[file];
            ++entry.first;
            ++entry.second[message];
        }
        std::vector<std::pair<long, std::string>> ranked;
        for (auto const& [file, entry] : by_file)
            ranked.emplace_back(entry.first, file);
        std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        for (std::size_t i = 0; i < ranked.size() && i < static_cast<std::size_t>(files_ranked); ++i) {
            auto const& entry = by_file[ranked[i].second];
            std::string top;
            long top_count = 0;
            for (auto const& [message, count] : entry.second) {
                if (count > top_count) {
                    top = message;
                    top_count = count;
                }
            }
            std::printf("  %5ld  %s \xe2\x80\x94 %s\n", ranked[i].first, ranked[i].second.c_str(), top.substr(0, 110).c_str());
        }
    }
    // The slowest tests, named: a test at the deadline is a hang to look at.
    std::vector<std::pair<double, std::string>> slowest;
    for (std::size_t i = 0; i < tests.size(); ++i)
        slowest.emplace_back(results[i].seconds, tests[i].first);
    std::sort(slowest.begin(), slowest.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < slowest.size() && i < 10 && slowest[i].first >= 1.0; ++i)
        std::printf("  slow %5.1f s  %s\n", slowest[i].first, slowest[i].second.c_str());
    int printed = 0;
    for (auto const& [id, why] : incomplete) {
        if (printed++ >= max_printed)
            break;
        std::cout << "  NO REPORT " << id << ": " << why << "\n";
    }
    for (auto const& [name, message] : failures) {
        if (printed++ >= max_printed)
            break;
        std::cout << "  FAIL " << name << ": " << message << "\n";
    }
    if (printed > max_printed)
        std::cout << "  (" << (incomplete.size() + failures.size() - static_cast<std::size_t>(max_printed))
                  << " more; SASHFOLD_PRINT_FAILURES=n or --print n shows them)\n";
    if (!json_path.empty())
        write_json(json_path, scores, passed, total, files, completed, revision);
    if (!html_path.empty())
        write_html(html_path, scores, passed, total, files, completed, revision);

    // The baseline: every subtest that passed when it was last blessed. With
    // --only the run is partial, so the verdict is informational.
    std::set<std::string> const baseline = [&] {
        std::vector<std::string> const lines = read_lines(baseline_file);
        return std::set<std::string>(lines.begin(), lines.end());
    }();
    std::vector<std::string> regressions;
    long gone = 0;
    for (std::string const& line : baseline) {
        if (passing.contains(line))
            continue;
        std::string const id = line.substr(0, line.find('\t'));
        if (!files_seen.contains(id) && (only.empty() || !std::filesystem::exists(root / id)))
            ++gone;
        else if (only.empty() || id.find(only) != std::string::npos)
            regressions.push_back(line);
    }
    std::vector<std::string> new_passes;
    for (std::string const& line : passing) {
        if (!baseline.contains(line))
            new_passes.push_back(line);
    }
    if (update) {
        if (!only.empty()) {
            std::cerr << "--update needs the whole run, not --only\n";
            return 2;
        }
        // A bless never drops a passing subtest unread: the losses are
        // named, and go only when accepted by name of the flag.
        if (!regressions.empty() && !accept_losses) {
            std::cerr << "REFUSED: --update would drop " << regressions.size()
                      << " subtest(s) that passed before; read them, and --accept-losses if they are to go:\n";
            for (std::string const& line : regressions)
                std::cerr << "  lost " << line << "\n";
            return 1;
        }
        for (std::size_t i = 0; i < new_passes.size() && i < 40; ++i)
            std::cout << "  won " << new_passes[i] << "\n";
        if (new_passes.size() > 40)
            std::cout << "  (" << (new_passes.size() - 40) << " more won)\n";
        for (std::string const& line : regressions)
            std::cout << "  lost " << line << "\n";
        std::ofstream out(baseline_file, std::ios::binary);
        out << "# Every testharness subtest that passes: one per line, the test's path, a tab, the subtest's\n"
               "# name (tabs and newlines escaped). A listed subtest that fails is a regression.\n"
               "# Re-bless with: wpt_testharness <checkout> <directories> <this file> --update\n";
        for (std::string const& line : passing)
            out << line << "\n";
        std::cout << "blessed " << passing.size() << " passing subtests into " << baseline_file.string() << " ("
                  << new_passes.size() << " won, " << regressions.size() << " lost)\n";
        return 0;
    }
    if (!only.empty()) {
        std::cout << "(partial run: the baseline is not enforced)\n";
        return 0;
    }
    if (!regressions.empty()) {
        std::cerr << "REGRESSION: " << regressions.size() << " subtest(s) in the baseline no longer pass:\n";
        for (std::string const& line : regressions)
            std::cerr << "  " << line << "\n";
        return 1;
    }
    if (gone > 0)
        std::cout << gone << " baseline subtest(s) are no longer in the checkout (a different revision?)\n";
    if (!new_passes.empty())
        std::cout << "RATCHET: " << new_passes.size() << " new pass(es) — bless them with --update\n";
    std::cout << "baseline held: " << (baseline.size() - static_cast<std::size_t>(gone)) << " subtests still pass\n";
    return 0;
}
