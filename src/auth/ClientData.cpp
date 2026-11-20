#include "auth/ClientData.h"

#include "net/Filters.h"
#include "net/Url.h"

#include <algorithm>

namespace sashfold::auth {

std::string ccd_to_string(std::string_view utf8)
{
    // The input is UTF-8 already (the page's strings were converted with
    // lone surrogates replaced), so every code point at or above U+0020 is
    // copied byte for byte; only the ASCII ones below it, the quotation
    // mark and the reverse solidus are written otherwise.
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (char const c : utf8) {
        auto const byte = static_cast<unsigned char>(c);
        if (byte == 0x22) {
            out += "\\\"";
        } else if (byte == 0x5c) {
            out += "\\\\";
        } else if (byte < 0x20) {
            out += "\\u00";
            out += hex[byte >> 4];
            out += hex[byte & 15];
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

std::string serialize_client_data(CollectedClientData const& data)
{
    // §5.8.1.1 steps 1 to 5, then the closing brace of step 8: this client
    // adds no members of its own (step 7 is for future ones).
    std::string out = "{\"type\":" + ccd_to_string(data.type);
    out += ",\"challenge\":" + ccd_to_string(data.challenge);
    out += ",\"origin\":" + ccd_to_string(data.origin);
    out += data.cross_origin ? ",\"crossOrigin\":true" : ",\"crossOrigin\":false";
    if (data.top_origin)
        out += ",\"topOrigin\":" + ccd_to_string(*data.top_origin);
    out += '}';
    return out;
}

std::string base64url(std::span<std::uint8_t const> bytes)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((bytes.size() * 4 + 2) / 3);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        std::uint32_t const n = std::uint32_t { bytes[i] } << 16 | std::uint32_t { bytes[i + 1] } << 8 | bytes[i + 2];
        out += alphabet[n >> 18];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += alphabet[n & 63];
    }
    if (bytes.size() - i == 1) {
        std::uint32_t const n = std::uint32_t { bytes[i] } << 16;
        out += alphabet[n >> 18];
        out += alphabet[(n >> 12) & 63];
    } else if (bytes.size() - i == 2) {
        std::uint32_t const n = std::uint32_t { bytes[i] } << 16 | std::uint32_t { bytes[i + 1] } << 8;
        out += alphabet[n >> 18];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
    }
    return out;
}

namespace {

// The host parser's answer for a string that is meant to be a host and
// nothing else: nothing when it is not one (it holds a port, a path, a
// space, credentials).
std::optional<net::Url> host_alone(std::string_view text)
{
    if (text.empty() || text.find_first_of("/?#@\\") != std::string_view::npos)
        return std::nullopt;
    std::optional<net::Url> url = net::parse_url("https://" + std::string(text) + "/");
    if (!url || url->port || !url->has_host() || url->host_kind == net::Url::HostKind::Empty)
        return std::nullopt;
    // A colon that the parser read as a port separator with the default
    // port (":443") leaves no port behind: refuse it too.
    if (url->host_kind != net::Url::HostKind::Ipv6 && text.find(':') != std::string_view::npos)
        return std::nullopt;
    return url;
}

bool is_domain(net::Url const& url) { return url.host_kind == net::Url::HostKind::Domain; }

}

bool is_registrable_suffix_or_equal(std::string_view host_suffix, std::string_view original_host)
{
    std::optional<net::Url> const parsed = host_alone(host_suffix);
    if (!parsed)
        return false;
    std::string const& host = parsed->host;
    if (host == original_host)
        return true;
    std::optional<net::Url> const original = host_alone(original_host);
    if (!original || !is_domain(*parsed) || !is_domain(*original))
        return false;
    // `host`, with a dot before it, ends the original host...
    std::string const dotted = "." + host;
    if (!original->host.ends_with(dotted))
        return false;
    // ...and is not shorter than its registrable domain: a public suffix
    // ("com", "co.uk") is never one, nor is anything between it and the
    // registrable domain. The engine's registrable domain is the last two
    // labels, three under a country code's known second levels.
    std::string const registrable = net::registrable_domain(original->host);
    return host == registrable || host.ends_with("." + registrable);
}

bool rp_id_allowed(std::string_view rp_id, std::string_view scheme, std::string_view host)
{
    // A secure context's origin (§5.1.3 step 8 requires the effective
    // domain to be a valid domain; localhost is a secure origin by HTML).
    bool const local = host == "localhost" || host.ends_with(".localhost");
    if (scheme != "https" && !(scheme == "http" && local))
        return false;
    if (host.empty())
        return false;
    // An IP address is no valid domain: no RP ID is allowed for it.
    std::optional<net::Url> const original = host_alone(host);
    if (!original || !is_domain(*original))
        return false;
    return is_registrable_suffix_or_equal(rp_id, host);
}

std::chrono::milliseconds ceremony_timeout(std::optional<std::uint32_t> page_value)
{
    constexpr std::uint32_t lowest = 300000;
    constexpr std::uint32_t highest = 600000;
    if (!page_value)
        return std::chrono::milliseconds(lowest);
    return std::chrono::milliseconds(std::clamp(*page_value, lowest, highest));
}

}
