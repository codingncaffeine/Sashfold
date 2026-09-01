#include "net/Cache.h"

#include "core/Ascii.h"
#include "net/Cookies.h"

#include <algorithm>
#include <utility>

namespace sashfold::net {

namespace {

// RFC 9111 §1.2.2: a delta-seconds beyond what we can represent counts as
// 2^31 — and any larger value is meaningless for a session cache anyway.
constexpr std::int64_t max_delta_seconds = std::int64_t { 1 } << 31;

std::string_view trim_ows(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

// A non-negative delta-seconds; nullopt unless the whole token is digits.
std::optional<std::int64_t> parse_delta_seconds(std::string_view text)
{
    text = trim_ows(text);
    if (text.empty())
        return std::nullopt;
    std::int64_t value = 0;
    for (char const c : text) {
        if (!is_ascii_digit(static_cast<unsigned char>(c)))
            return std::nullopt;
        if (value < max_delta_seconds)
            value = value * 10 + (c - '0');
    }
    return std::min(value, max_delta_seconds);
}

// Calls fn(member) for each comma-separated member of a header list value,
// with commas inside quoted strings left alone (RFC 9110 §5.6.1 list syntax
// over §5.6.4 quoted-string).
template<typename Fn>
void for_each_list_member(std::string_view value, Fn&& fn)
{
    std::size_t start = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        char const c = value[i];
        if (quoted) {
            if (c == '\\' && i + 1 < value.size())
                ++i; // quoted-pair
            else if (c == '"')
                quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            fn(trim_ows(value.substr(start, i - start)));
            start = i + 1;
        }
    }
    fn(trim_ows(value.substr(start)));
}

// Strips one layer of quoted-string syntax, unescaping quoted-pairs.
std::string unquote(std::string_view text)
{
    if (text.size() < 2 || text.front() != '"' || text.back() != '"')
        return std::string(text);
    std::string out;
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
        if (text[i] == '\\' && i + 2 < text.size())
            ++i;
        out += text[i];
    }
    return out;
}

struct CacheControl {
    bool no_store = false;
    bool no_cache = false;
    std::optional<std::int64_t> max_age; // seconds; 0 when the value was invalid
};

CacheControl parse_cache_control(std::vector<Header> const& headers)
{
    CacheControl control;
    for (Header const& header : headers) {
        if (!ascii_ci_equals(header.name, "cache-control"))
            continue;
        for_each_list_member(header.value, [&](std::string_view member) {
            if (member.empty())
                return;
            std::size_t const equals = member.find('=');
            std::string_view const name = trim_ows(member.substr(0, equals));
            if (ascii_ci_equals(name, "no-store")) {
                control.no_store = true;
            } else if (ascii_ci_equals(name, "no-cache")) {
                control.no_cache = true;
            } else if (ascii_ci_equals(name, "max-age")) {
                // Duplicate directives: the first wins (RFC 9111 §4.2.1);
                // an unparseable argument means stale (§5.2.2.1).
                if (control.max_age)
                    return;
                std::optional<std::int64_t> seconds;
                if (equals != std::string_view::npos)
                    seconds = parse_delta_seconds(unquote(trim_ows(member.substr(equals + 1))));
                control.max_age = seconds.value_or(0);
            }
        });
    }
    return control;
}

// fetch() sends these with the same value on every request, so a response
// that varies only on them is the same response for us.
bool vary_is_honored(std::vector<Header> const& headers)
{
    bool honored = true;
    for (Header const& header : headers) {
        if (!ascii_ci_equals(header.name, "vary"))
            continue;
        for_each_list_member(header.value, [&](std::string_view member) {
            if (member.empty())
                return;
            if (!ascii_ci_equals(member, "accept") && !ascii_ci_equals(member, "accept-encoding")
                && !ascii_ci_equals(member, "user-agent"))
                honored = false; // "*" lands here too
        });
    }
    return honored;
}

// What an entry costs against the caps: its payload bytes. Fixed per-entry
// overhead is deliberately left out so the caps read as "bytes of content".
std::size_t cost_of(std::string const& key, FetchResponse const& response)
{
    std::size_t cost = key.size() + response.body.size();
    for (Header const& header : response.headers)
        cost += header.name.size() + header.value.size();
    return cost;
}

} // namespace

std::optional<std::int64_t> fresh_until(std::vector<Header> const& headers, std::int64_t now)
{
    CacheControl const control = parse_cache_control(headers);
    if (control.no_store || control.no_cache || !vary_is_honored(headers))
        return std::nullopt;

    std::int64_t age = 0;
    if (std::string const* const age_header = find_header(headers, "age"))
        age = parse_delta_seconds(*age_header).value_or(0); // invalid Age: ignored (§5.1)

    if (control.max_age)
        return now + *control.max_age - age;

    if (std::string const* const expires = find_header(headers, "expires")) {
        std::optional<std::int64_t> const expiry = parse_cookie_date(*expires);
        if (!expiry)
            return now; // invalid dates, "0" included, mean already expired (§5.3)
        if (std::string const* const date = find_header(headers, "date")) {
            if (std::optional<std::int64_t> const origin = parse_cookie_date(*date))
                return now + (*expiry - *origin) - age; // lifetime as the origin measured it
        }
        return *expiry;
    }
    return std::nullopt;
}

FetchResponse const* MemoryCache::lookup(Url const& url, std::int64_t now) const
{
    auto const it = m_entries.find(url.serialize(true));
    if (it == m_entries.end() || it->second.fresh_until <= now)
        return nullptr;
    return &it->second.response;
}

bool MemoryCache::store(Url const& url, FetchResponse const& response, std::int64_t now)
{
    if (response.status != 200)
        return false;
    std::optional<std::int64_t> const until = fresh_until(response.headers, now);
    if (!until || *until <= now)
        return false;

    std::string key = url.serialize(true);
    Entry entry;
    entry.response.status = response.status;
    entry.response.status_text = response.status_text;
    entry.response.final_url = response.final_url;
    entry.response.body = response.body;
    entry.response.from_cache = true;
    for (Header const& header : response.headers) {
        if (!ascii_ci_equals(header.name, "set-cookie"))
            entry.response.headers.push_back(header);
    }
    entry.cost = cost_of(key, entry.response);
    entry.fresh_until = *until;
    entry.stored_at = now;
    if (entry.cost > m_max_entry_bytes)
        return false;

    if (auto const existing = m_entries.find(key); existing != m_entries.end()) {
        m_bytes -= existing->second.cost;
        m_entries.erase(existing);
    }
    evict_to_fit(entry.cost, now);
    m_bytes += entry.cost;
    m_entries.emplace(std::move(key), std::move(entry));
    return true;
}

void MemoryCache::evict_to_fit(std::size_t incoming, std::int64_t now)
{
    // Stale entries are dead weight: they go first, unconditionally.
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->second.fresh_until <= now) {
            m_bytes -= it->second.cost;
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    // Then the oldest fresh entries until the newcomer fits.
    while (!m_entries.empty() && m_bytes + incoming > m_max_total_bytes) {
        auto oldest = m_entries.begin();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->second.stored_at < oldest->second.stored_at)
                oldest = it;
        }
        m_bytes -= oldest->second.cost;
        m_entries.erase(oldest);
    }
}

}
