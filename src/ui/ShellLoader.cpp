#include "ui/ShellLoader.h"

#include "core/Ascii.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace sashfold::ui {

namespace {

// A page's requests on stderr under SASHFOLD_NET_TRACE=1: the method, the
// status or the error, the address and the size — what a stream that
// starts failing looks like from here.
bool net_tracing()
{
    static bool const wanted = [] {
        char const* const value = std::getenv("SASHFOLD_NET_TRACE");
        return value != nullptr && value[0] == '1';
    }();
    return wanted;
}

// SASHFOLD_NET_TRACE_DIR=<dir>: what each traced exchange sent and got
// back, kept as <n>.req and <n>.res beside its whole address in <n>.url,
// the trace line naming <n>. A protocol that fails inside its bodies
// (a token refused, a message the page answered wrongly) shows nothing
// in the line itself.
std::string const& trace_directory()
{
    static std::string const directory = [] {
        char const* const value = std::getenv("SASHFOLD_NET_TRACE_DIR");
        return std::string(value != nullptr ? value : "");
    }();
    return directory;
}

void keep_bytes(std::string const& path, std::vector<std::uint8_t> const& bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void trace_request(std::string const& method, net::Url const& url, std::vector<std::uint8_t> const& sent,
    net::FetchResult const& result)
{
    std::string const address = url.serialize();
    std::string const shown = address.size() > 200 ? address.substr(0, 200) + "…" : address;
    std::string label;
    if (!trace_directory().empty()) {
        static std::atomic<unsigned> count { 0 };
        unsigned const number = ++count;
        label = "#" + std::to_string(number) + " ";
        std::error_code ignored;
        std::filesystem::create_directories(trace_directory(), ignored);
        std::string const stem = trace_directory() + "/" + std::to_string(number);
        std::ofstream(stem + ".url", std::ios::binary) << method << ' ' << address << '\n';
        if (!sent.empty())
            keep_bytes(stem + ".req", sent);
        if (result.response)
            keep_bytes(stem + ".res", result.response->body);
    }
    if (result.response) {
        std::cerr << "net: " << label << method << " " << result.response->status << " " << shown << " ("
                  << result.response->body.size() << " bytes" << (result.response->from_cache ? ", cached" : "") << ")\n";
        // A small answer from a media server is a message, not media: shown
        // whole, so what the server said can be read.
        if (address.find("videoplayback") != std::string::npos && result.response->body.size() <= 512) {
            std::string hex;
            for (std::uint8_t const byte : result.response->body) {
                hex += "0123456789abcdef"[byte >> 4];
                hex += "0123456789abcdef"[byte & 15];
            }
            std::cerr << "net: body " << hex << "\n";
        }
    } else
        std::cerr << "net: " << label << method << " failed " << shown << ": " << result.error << "\n";
}

}

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

net::CookieJar& ShellLoader::cookies(std::string_view container)
{
    if (container.empty())
        return m_cookies;
    // A jar, once made, stays where it is: only the finding of it is locked.
    std::lock_guard<std::mutex> const lock(m_mutex);
    return m_container_jars[std::string(container)];
}

net::FetchResult ShellLoader::submit(net::Url const& url, std::string const& referrer, PostedForm const& form,
    std::string_view container)
{
    net::RequestGuard const none;
    if (std::optional<std::string> refused = refusal(url, nullptr, net::ResourceKind::Document, none, false))
        return { std::nullopt, std::move(*refused) };
    if (url.scheme != "http" && url.scheme != "https")
        return { std::nullopt, "a form can be posted to a web address only" };
    net::FetchOptions options;
    options.cookie_jar = &cookies(container);
    options.first_party = nullptr; // a navigation is its own first party
    options.referrer = referrer;
    options.cache = nullptr; // what a POST brings back is never the cache's
    options.pool = &m_pool;
    options.method = "POST";
    options.headers.push_back({ "Content-Type", form.content_type });
    options.headers.push_back({ "Origin", form.origin.empty() ? std::string("null") : form.origin });
    options.body = form.body;
    options.hop_refusal = hop_refusal(nullptr, net::ResourceKind::Document, none);
    return noted(net::ResourceKind::Document, net::fetch(url, options));
}

std::shared_ptr<net::FetchTicket> ShellLoader::submit_ahead(net::Url const& url, std::string const& referrer,
    PostedForm const& form, std::string_view container)
{
    if (url.scheme != "http" && url.scheme != "https")
        return nullptr;
    return m_fetches.submit([this, url, referrer, form, held = std::string(container)] {
        return submit(url, referrer, form, held);
    });
}

std::vector<std::string> ShellLoader::container_names() const
{
    std::vector<std::string> names;
    std::lock_guard<std::mutex> const lock(m_mutex);
    for (auto const& [name, jar] : m_container_jars)
        names.push_back(name);
    return names;
}

net::FetchResult ShellLoader::load(net::Url const& url, std::string const& referrer,
    bool bypass_cache, std::string_view container)
{
    net::RequestGuard const none;
    if (std::optional<std::string> refused = refusal(url, nullptr, net::ResourceKind::Document, none, false))
        return { std::nullopt, std::move(*refused) };
    if (url.scheme == "file")
        return load_file(url);
    net::FetchOptions options;
    options.cookie_jar = &cookies(container);
    options.first_party = nullptr; // a navigation is its own first party
    options.referrer = referrer;
    options.cache = bypass_cache ? nullptr : &m_cache;
    options.pool = &m_pool;
    // A navigation redirected onto a listed site is refused where it lands.
    options.hop_refusal = hop_refusal(nullptr, net::ResourceKind::Document, none);
    return noted(net::ResourceKind::Document, net::fetch(url, options));
}

void ShellLoader::Census::Kind::add(Kind const& more)
{
    fetches += more.fetches;
    cached += more.cached;
    failed += more.failed;
    timing.add(more.timing);
}

ShellLoader::Census::Kind& ShellLoader::Census::of(net::ResourceKind kind)
{
    switch (kind) {
    case net::ResourceKind::Document:
        return document;
    case net::ResourceKind::Subdocument:
    case net::ResourceKind::Object:
        return subdocument;
    case net::ResourceKind::Stylesheet:
        return stylesheet;
    case net::ResourceKind::Script:
    case net::ResourceKind::Worker:
        return script;
    case net::ResourceKind::Image:
        return image;
    case net::ResourceKind::Font:
        return font;
    case net::ResourceKind::Xhr:
        return xhr;
    case net::ResourceKind::Media:
    case net::ResourceKind::Other:
        break;
    }
    return other;
}

ShellLoader::Census::Kind const& ShellLoader::Census::of(net::ResourceKind kind) const
{
    return const_cast<Census&>(*this).of(kind);
}

ShellLoader::Census::Kind ShellLoader::Census::total() const
{
    Kind sum;
    for (Kind const* kind : { &document, &subdocument, &stylesheet, &script, &image, &font, &xhr, &other })
        sum.add(*kind);
    return sum;
}

net::FetchResult ShellLoader::noted(net::ResourceKind kind, net::FetchResult result)
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    Census::Kind& entry = m_census.of(kind);
    ++entry.fetches;
    if (!result.response)
        ++entry.failed;
    else if (result.response->from_cache)
        ++entry.cached;
    entry.timing.add(result.timing);
    return result;
}

net::FetchResult ShellLoader::load_subresource(net::Url const& requested, net::Url const& first_party,
    std::string const& referrer, net::ResourceKind kind, net::RequestGuard const& guard, std::string_view container)
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
    // Asked for ahead: what came, or what is on its way, is the answer. Its
    // redirects were judged by the lists alone — the page's policy was not
    // to hand then — so where it ended up is judged by the guard now.
    std::shared_ptr<net::FetchTicket> ahead;
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        if (auto const it = m_ahead.find(ahead_key(url, kind, container)); it != m_ahead.end()) {
            ahead = std::move(it->second.ticket);
            m_ahead.erase(it);
        }
    }
    if (ahead) {
        net::FetchResult result = ahead->take();
        if (result.response && guard.refusal && result.response->final_url.serialize() != url.serialize()) {
            if (std::optional<std::string> refused = guard.refusal(result.response->final_url, true))
                return { std::nullopt, std::move(*refused) };
        }
        return result;
    }
    return fetch_subresource(url, first_party, referrer, kind, guard, std::string(container));
}

net::FetchResult ShellLoader::fetch_subresource(net::Url const& url, net::Url const& first_party, std::string const& referrer,
    net::ResourceKind kind, net::RequestGuard const& guard, std::string const& container)
{
    net::FetchOptions options;
    options.cookie_jar = &cookies(container);
    options.first_party = &first_party;
    options.referrer = referrer;
    options.cache = &m_cache;
    options.pool = &m_pool;
    options.hop_refusal = hop_refusal(&first_party, kind, guard);
    net::FetchResult result = noted(kind, net::fetch(url, options));
    if (net_tracing())
        trace_request(options.method.empty() ? "GET" : options.method, url, options.body, result);
    return result;
}

std::string ShellLoader::ahead_key(net::Url const& url, net::ResourceKind kind, std::string_view container)
{
    return std::to_string(static_cast<unsigned>(kind)) + ' ' + std::string(container) + ' ' + url.serialize(true);
}

std::shared_ptr<net::FetchTicket> ShellLoader::prefetch(net::Url const& url, net::Url const& first_party,
    std::string const& referrer, net::ResourceKind kind, std::string_view container)
{
    // Only what goes over the network is worth a thread, and only what the
    // lists let through is asked for at all.
    if (url.scheme != "http" && url.scheme != "https")
        return nullptr;
    if (refusal(url, &first_party, kind, net::RequestGuard {}, false))
        return nullptr;
    std::int64_t const now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string const key = ahead_key(url, kind, container);
    // Asked for from one thread only — the window's — so what is looked for
    // here and not found is still not there when it is entered below.
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        // What nobody claimed within a minute is let go; and no more than a
        // few hundred are kept at once, the oldest going first.
        std::erase_if(m_ahead, [now](auto const& entry) { return now - entry.second.asked_at > 60; });
        while (m_ahead.size() >= 512) {
            auto oldest = m_ahead.begin();
            for (auto it = m_ahead.begin(); it != m_ahead.end(); ++it) {
                if (it->second.asked_at < oldest->second.asked_at)
                    oldest = it;
            }
            m_ahead.erase(oldest);
        }
        // On its way already: the same ticket.
        if (auto const it = m_ahead.find(key); it != m_ahead.end())
            return it->second.ticket;
    }
    // The work owns copies: the page that asked may be gone before it runs.
    std::shared_ptr<net::FetchTicket> ticket = m_fetches.submit(
        [this, url, first_party, referrer, kind, held = std::string(container)] {
            return fetch_subresource(url, first_party, referrer, kind, net::RequestGuard {}, held);
        });
    std::lock_guard<std::mutex> const lock(m_mutex);
    m_ahead[key] = Ahead { ticket, now };
    return ticket;
}

bool ShellLoader::ahead_pending(net::Url const& url, net::ResourceKind kind, std::string_view container)
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    auto const it = m_ahead.find(ahead_key(url, kind, container));
    return it != m_ahead.end() && it->second.ticket && !it->second.ticket->done();
}

std::shared_ptr<net::FetchTicket> ShellLoader::load_ahead(net::Url const& url, std::string const& referrer,
    bool bypass_cache, std::string_view container)
{
    if (url.scheme != "http" && url.scheme != "https")
        return nullptr;
    return m_fetches.submit([this, url, referrer, bypass_cache, held = std::string(container)] {
        return load(url, referrer, bypass_cache, held);
    });
}

net::FetchResult ShellLoader::load_resource(net::Url const& requested, net::Url const& first_party,
    std::string const& referrer, net::ResourceRequest const& request, net::RequestGuard const& guard,
    std::string_view container)
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
    options.cookie_jar = request.credentials ? &cookies(container) : nullptr;
    options.first_party = &first_party;
    options.referrer = referrer;
    options.cache = &m_cache;
    options.pool = &m_pool;
    options.method = request.method;
    options.headers = request.headers;
    options.body = request.body;
    options.follow_redirects = request.follow_redirects;
    options.hop_refusal = hop_refusal(&first_party, kind, guard);
    net::FetchResult result = noted(kind, net::fetch(url, options));
    if (net_tracing())
        trace_request(options.method.empty() ? "GET" : options.method, url, options.body, result);
    return result;
}

std::string ShellLoader::cookies_for(net::Url const& url, std::string_view container)
{
    using namespace std::chrono;
    // The page is its own first party: what document.cookie sees is what
    // a navigation to the page would send.
    return cookies(container).cookie_header(url, nullptr, duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

void ShellLoader::set_cookie(net::Url const& url, std::string_view set_cookie_line, std::string_view container)
{
    using namespace std::chrono;
    std::vector<net::Header> const headers { net::Header { "Set-Cookie", std::string(set_cookie_line) } };
    cookies(container).store(url, nullptr, headers, duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

}
