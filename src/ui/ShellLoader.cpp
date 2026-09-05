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

net::FetchResult ShellLoader::load(net::Url const& url, std::string const& referrer,
    bool bypass_cache)
{
    if (url.scheme == "file")
        return load_file(url);
    net::FetchOptions options;
    options.cookie_jar = &m_cookies;
    options.first_party = nullptr; // a navigation is its own first party
    options.referrer = referrer;
    options.cache = bypass_cache ? nullptr : &m_cache;
    options.pool = &m_pool;
    return net::fetch(url, options);
}

net::FetchResult ShellLoader::load_subresource(net::Url const& url, net::Url const& first_party,
    std::string const& referrer)
{
    if (url.scheme == "file") {
        // A local page may reference local files; a remote one may not.
        if (first_party.scheme != "file")
            return { std::nullopt, "a web page cannot read local files" };
        return load_file(url);
    }
    net::FetchOptions options;
    options.cookie_jar = &m_cookies;
    options.first_party = &first_party;
    options.referrer = referrer;
    options.cache = &m_cache;
    options.pool = &m_pool;
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
