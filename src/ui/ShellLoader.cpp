#include "ui/ShellLoader.h"

#include "core/Ascii.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <vector>

namespace sashfold::ui {

namespace {

std::string percent_decode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()
            && is_ascii_hex_digit(static_cast<unsigned char>(text[i + 1]))
            && is_ascii_hex_digit(static_cast<unsigned char>(text[i + 2]))) {
            unsigned const value = hex_digit_value(static_cast<unsigned char>(text[i + 1])) * 16u
                + hex_digit_value(static_cast<unsigned char>(text[i + 2]));
            out += static_cast<char>(value);
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::string content_type_for(std::string const& path)
{
    std::size_t const dot = path.rfind('.');
    std::string extension = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (char& c : extension)
        c = static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
    if (extension == "html" || extension == "htm" || extension == "xhtml")
        return "text/html; charset=utf-8";
    if (extension == "txt" || extension == "md" || extension == "log" || extension == "text")
        return "text/plain; charset=utf-8";
    if (extension == "css")
        return "text/css; charset=utf-8";
    if (extension == "json")
        return "application/json";
    if (extension == "js")
        return "text/javascript";
    if (extension == "png")
        return "image/png";
    if (extension == "jpg" || extension == "jpeg")
        return "image/jpeg";
    if (extension == "gif")
        return "image/gif";
    if (extension == "bmp")
        return "image/bmp";
    if (extension == "ico")
        return "image/x-icon";
    if (extension == "svg")
        return "image/svg+xml";
    return "application/octet-stream";
}

net::FetchResult load_file(net::Url const& url)
{
    std::string path = percent_decode(url.serialize_path());
    // file:///C:/dir/page.html carries a leading slash the filesystem does not.
    if (path.size() >= 3 && path[0] == '/' && is_ascii_alpha(static_cast<unsigned char>(path[1]))
        && path[2] == ':')
        path.erase(0, 1);
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return { std::nullopt, "cannot read " + path };
    std::ostringstream stream;
    stream << file.rdbuf();
    std::string const text = std::move(stream).str();

    net::FetchResponse response;
    response.status = 200;
    response.status_text = "OK";
    response.headers.push_back({ "Content-Type", content_type_for(path) });
    response.body.assign(text.begin(), text.end());
    response.final_url = url;
    return { std::move(response), "" };
}

} // namespace

std::optional<std::string> ShellLoader::refusal(net::Url const& url, net::Url const* first_party,
    net::ResourceKind kind, net::RequestGuard const& guard, bool redirected)
{
    if (!m_blocklists.empty()) {
        std::optional<net::Blocklists::Block> const block
            = m_blocklists.blocks(net::FilterRequest { &url, first_party, kind });
        if (block) {
            ++m_blocked;
            return (block->nefarious ? "kept off by " : "blocked by ") + block->list + ": " + block->rule;
        }
    }
    if (guard.refusal)
        return guard.refusal(url, redirected);
    return std::nullopt;
}

std::function<std::optional<std::string>(net::Url&)> ShellLoader::hop_refusal(net::Url const* first_party,
    net::ResourceKind kind, net::RequestGuard const& guard)
{
    // The lists and the guard outlive the fetch they are asked during.
    return [this, first_party, kind, &guard](net::Url& next) -> std::optional<std::string> {
        if (guard.upgrade_insecure)
            next = net::upgraded_insecure(next);
        return refusal(next, first_party, kind, guard, true);
    };
}

net::FetchResult ShellLoader::load(net::Url const& url, std::string const& referrer,
    bool bypass_cache)
{
    net::RequestGuard const none;
    if (std::optional<std::string> refused = refusal(url, nullptr, net::ResourceKind::Document, none, false))
        return { std::nullopt, std::move(*refused) };
    if (url.scheme == "file")
        return load_file(url);
    net::FetchOptions options;
    options.cookie_jar = &m_cookies;
    options.first_party = nullptr; // a navigation is its own first party
    options.referrer = referrer;
    options.cache = bypass_cache ? nullptr : &m_cache;
    options.pool = &m_pool;
    // A navigation redirected onto a listed site is refused where it lands.
    options.hop_refusal = hop_refusal(nullptr, net::ResourceKind::Document, none);
    return net::fetch(url, options);
}

net::FetchResult ShellLoader::load_subresource(net::Url const& requested, net::Url const& first_party,
    std::string const& referrer, net::ResourceKind kind, net::RequestGuard const& guard)
{
    net::Url const url = guard.upgrade_insecure ? net::upgraded_insecure(requested) : requested;
    if (std::optional<std::string> refused = refusal(url, &first_party, kind, guard, false))
        return { std::nullopt, std::move(*refused) };
    if (url.scheme == "file") {
        // A local page may reference local files, and so may the shell's
        // own about: pages (the new-tab page shows the theme's pictures);
        // a remote one may not.
        if (first_party.scheme != "file" && first_party.scheme != "about")
            return { std::nullopt, "a web page cannot read local files" };
        return load_file(url);
    }
    net::FetchOptions options;
    options.cookie_jar = &m_cookies;
    options.first_party = &first_party;
    options.referrer = referrer;
    options.cache = &m_cache;
    options.pool = &m_pool;
    options.hop_refusal = hop_refusal(&first_party, kind, guard);
    return net::fetch(url, options);
}

net::FetchResult ShellLoader::load_resource(net::Url const& requested, net::Url const& first_party,
    std::string const& referrer, net::ResourceRequest const& request, net::RequestGuard const& guard)
{
    net::ResourceKind const kind = request.destination == "script" ? net::ResourceKind::Script : net::ResourceKind::Xhr;
    net::Url const url = guard.upgrade_insecure ? net::upgraded_insecure(requested) : requested;
    if (std::optional<std::string> refused = refusal(url, &first_party, kind, guard, false))
        return { std::nullopt, std::move(*refused) };
    if (url.scheme == "file") {
        if (first_party.scheme != "file")
            return { std::nullopt, "a web page cannot read local files" };
        if (request.method != "GET" && request.method != "HEAD")
            return { std::nullopt, "a local file takes no " + request.method };
        return load_file(url);
    }
    net::FetchOptions options;
    options.cookie_jar = request.credentials ? &m_cookies : nullptr;
    options.first_party = &first_party;
    options.referrer = referrer;
    options.cache = &m_cache;
    options.pool = &m_pool;
    options.method = request.method;
    options.headers = request.headers;
    options.body = request.body;
    options.follow_redirects = request.follow_redirects;
    options.hop_refusal = hop_refusal(&first_party, kind, guard);
    return net::fetch(url, options);
}

std::string ShellLoader::cookies_for(net::Url const& url)
{
    using namespace std::chrono;
    // The page is its own first party: what document.cookie sees is what
    // a navigation to the page would send.
    return m_cookies.cookie_header(url, nullptr, duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

void ShellLoader::set_cookie(net::Url const& url, std::string_view set_cookie_line)
{
    using namespace std::chrono;
    std::vector<net::Header> const headers { net::Header { "Set-Cookie", std::string(set_cookie_line) } };
    m_cookies.store(url, nullptr, headers, duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

}
