#include "net/Hsts.h"

#include "net/Csp.h"

#include <algorithm>
#include <charconv>
#include <limits>

namespace sashfold::net {

namespace {

bool is_token_char(char c)
{
    // RFC 7230 §3.2.6 tchar.
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

std::string lowered(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// The host a list is keyed by: lowercase, one trailing dot dropped.
std::string host_key(std::string_view host)
{
    if (!host.empty() && host.back() == '.')
        host.remove_suffix(1);
    return lowered(host);
}

// A directive's value: a token, or a quoted string with its quotes and
// escapes taken off; nothing when it is neither.
std::optional<std::string> directive_value(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    if (text.front() != '"') {
        if (!std::all_of(text.begin(), text.end(), is_token_char))
            return std::nullopt;
        return std::string(text);
    }
    if (text.size() < 2 || text.back() != '"')
        return std::nullopt;
    std::string out;
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
        if (text[i] == '\\') {
            if (i + 2 >= text.size())
                return std::nullopt;
            ++i;
        } else if (text[i] == '"') {
            return std::nullopt;
        }
        out += text[i];
    }
    return out;
}

}

std::optional<StrictTransportSecurity> parse_strict_transport_security(std::string_view value)
{
    StrictTransportSecurity out;
    bool has_max_age = false;
    bool has_subdomains = false;
    std::vector<std::string> seen;
    // Directives are split at the semicolons outside quoted strings.
    std::size_t start = 0;
    bool quoted = false;
    for (std::size_t i = 0; i <= value.size(); ++i) {
        if (i < value.size()) {
            if (quoted) {
                if (value[i] == '\\' && i + 1 < value.size())
                    ++i;
                else if (value[i] == '"')
                    quoted = false;
                continue;
            }
            if (value[i] == '"') {
                quoted = true;
                continue;
            }
            if (value[i] != ';')
                continue;
        }
        std::string_view const directive = trimmed(value.substr(start, i - start));
        start = i + 1;
        if (directive.empty())
            continue; // `a; ; b` and a trailing semicolon are allowed
        std::size_t const equals = directive.find('=');
        std::string_view const raw_name = trimmed(directive.substr(0, equals));
        if (raw_name.empty() || !std::all_of(raw_name.begin(), raw_name.end(), is_token_char))
            return std::nullopt;
        std::string const name = lowered(raw_name);
        if (std::find(seen.begin(), seen.end(), name) != seen.end())
            return std::nullopt; // a directive appears once (§6.1 rule 2)
        seen.push_back(name);
        std::optional<std::string> argument;
        if (equals != std::string_view::npos) {
            argument = directive_value(trimmed(directive.substr(equals + 1)));
            if (!argument)
                return std::nullopt;
        }
        if (name == "max-age") {
            if (!argument || argument->empty() || !std::all_of(argument->begin(), argument->end(), [](char c) { return c >= '0' && c <= '9'; }))
                return std::nullopt;
            std::int64_t seconds = 0;
            auto const [end, error] = std::from_chars(argument->data(), argument->data() + argument->size(), seconds);
            // More seconds than fit is as long as can be said.
            out.max_age = error == std::errc() && end == argument->data() + argument->size()
                ? seconds
                : std::numeric_limits<std::int64_t>::max();
            has_max_age = true;
        } else if (name == "includesubdomains") {
            if (argument)
                return std::nullopt; // it takes no value
            has_subdomains = true;
        }
    }
    if (quoted || !has_max_age)
        return std::nullopt;
    out.include_subdomains = has_subdomains;
    return out;
}

void TransportSecurity::put(std::string host, Entry entry, std::int64_t now)
{
    auto const found = m_hosts.find(host);
    if (found != m_hosts.end()) {
        if (found->second.expires == entry.expires && found->second.include_subdomains == entry.include_subdomains)
            return;
        found->second = entry;
        ++m_changes;
        return;
    }
    if (m_hosts.size() >= max_hosts) {
        std::erase_if(m_hosts, [now](auto const& known) { return known.second.expires <= now; });
        if (m_hosts.size() >= max_hosts) {
            auto const soonest = std::min_element(m_hosts.begin(), m_hosts.end(),
                [](auto const& a, auto const& b) { return a.second.expires < b.second.expires; });
            m_hosts.erase(soonest);
        }
    }
    m_hosts.emplace(std::move(host), entry);
    ++m_changes;
}

void TransportSecurity::note(Url const& url, std::vector<Header> const& response_headers, std::int64_t now)
{
    if (url.scheme != "https" || url.host_kind != Url::HostKind::Domain || url.host.empty())
        return;
    std::string const* const header = find_header(response_headers, "strict-transport-security");
    if (header == nullptr)
        return;
    std::optional<StrictTransportSecurity> const said = parse_strict_transport_security(*header);
    if (!said)
        return;
    std::string host = host_key(url.host);
    std::lock_guard<std::mutex> const lock(m_mutex);
    if (said->max_age == 0) {
        if (m_hosts.erase(host) != 0)
            ++m_changes;
        return;
    }
    std::int64_t const forever = std::numeric_limits<std::int64_t>::max();
    std::int64_t const expires = said->max_age > forever - now ? forever : now + said->max_age;
    put(std::move(host), Entry { expires, said->include_subdomains }, now);
}

bool TransportSecurity::known(std::string_view host, std::int64_t now) const
{
    std::string const key = host_key(host);
    std::lock_guard<std::mutex> const lock(m_mutex);
    if (m_hosts.empty())
        return false;
    std::string_view rest = key;
    bool itself = true;
    while (!rest.empty()) {
        auto const found = m_hosts.find(rest);
        if (found != m_hosts.end() && found->second.expires > now && (itself || found->second.include_subdomains))
            return true;
        std::size_t const dot = rest.find('.');
        if (dot == std::string_view::npos)
            break;
        rest.remove_prefix(dot + 1);
        itself = false;
    }
    return false;
}

Url TransportSecurity::upgraded(Url url, std::int64_t now) const
{
    if ((url.scheme != "http" && url.scheme != "ws") || url.host_kind != Url::HostKind::Domain)
        return url;
    if (!known(url.host, now))
        return url;
    return upgraded_insecure(std::move(url));
}

std::string TransportSecurity::serialize(std::int64_t now) const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    std::string out;
    for (auto const& [host, entry] : m_hosts) {
        if (entry.expires <= now)
            continue;
        out += host;
        out += '\t';
        out += std::to_string(entry.expires);
        out += '\t';
        out += entry.include_subdomains ? '1' : '0';
        out += '\n';
    }
    return out;
}

void TransportSecurity::load(std::string_view text, std::int64_t now)
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    while (!text.empty()) {
        std::size_t const end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#')
            continue;
        std::size_t const first = line.find('\t');
        std::size_t const second = first == std::string_view::npos ? first : line.find('\t', first + 1);
        if (second == std::string_view::npos)
            continue;
        std::string_view const host = line.substr(0, first);
        std::string_view const expiry = line.substr(first + 1, second - first - 1);
        std::string_view const subdomains = line.substr(second + 1);
        std::int64_t expires = 0;
        auto const [stop, error] = std::from_chars(expiry.data(), expiry.data() + expiry.size(), expires);
        if (error != std::errc() || stop != expiry.data() + expiry.size() || host.empty() || (subdomains != "0" && subdomains != "1"))
            continue;
        if (expires <= now)
            continue;
        put(host_key(host), Entry { expires, subdomains == "1" }, now);
    }
}

}
