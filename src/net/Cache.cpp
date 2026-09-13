#include "net/Cache.h"

#include "core/Ascii.h"
#include "crypto/Sha2.h"
#include "net/Cookies.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>

namespace sashfold::net {

namespace {

// RFC 9111 §1.2.2: a delta-seconds beyond what we can represent counts as
// 2^31 — and any larger value is meaningless for a session cache anyway.
constexpr std::int64_t max_delta_seconds = std::int64_t { 1 } << 31;
// §4.2.2: a heuristic lifetime is a tenth of the time since the last
// modification, and never more than a week — Firefox's cap.
constexpr std::int64_t max_heuristic_seconds = 7 * 24 * 3600;

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

bool has_validator(std::vector<Header> const& headers)
{
    return find_header(headers, "etag") != nullptr || find_header(headers, "last-modified") != nullptr;
}

// What an entry costs in memory: its headers, and its body while loaded.
std::size_t cost_of(std::string const& key, FetchResponse const& response)
{
    std::size_t cost = key.size() + response.body.size();
    for (Header const& header : response.headers)
        cost += header.name.size() + header.value.size();
    return cost;
}

std::string header_or_empty(std::vector<Header> const& headers, std::string_view name)
{
    std::string const* const value = find_header(headers, name);
    return value ? *value : std::string();
}

bool clean_line(std::string_view text)
{
    return text.find('\n') == std::string_view::npos && text.find('\r') == std::string_view::npos;
}

} // namespace

std::optional<std::int64_t> fresh_until(std::vector<Header> const& headers, std::int64_t now, int status)
{
    CacheControl const control = parse_cache_control(headers);
    if (control.no_store || !vary_is_honored(headers))
        return std::nullopt;

    std::int64_t age = 0;
    if (std::string const* const age_header = find_header(headers, "age"))
        age = parse_delta_seconds(*age_header).value_or(0); // invalid Age: ignored (§5.1)

    // no-cache: kept, never served without asking the origin (§5.2.2.4).
    if (control.no_cache)
        return now;
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

    // §4.2.2: with nothing explicit, a 200 with a Last-Modified is fresh
    // for a tenth of the time since, as the origin dated it.
    if (status == 200) {
        if (std::string const* const modified = find_header(headers, "last-modified")) {
            if (std::optional<std::int64_t> const modified_at = parse_cookie_date(*modified)) {
                std::int64_t dated = now;
                if (std::string const* const date = find_header(headers, "date")) {
                    if (std::optional<std::int64_t> const origin = parse_cookie_date(*date))
                        dated = *origin;
                }
                std::int64_t const since = dated - *modified_at;
                if (since > 0)
                    return now + std::min(since / 10, max_heuristic_seconds) - age;
            }
        }
    }
    // A validator alone: stored to be revalidated, never served as it is.
    if (has_validator(headers))
        return now;
    return std::nullopt;
}

// --- Lookup, refresh, store ------------------------------------------------------------

HttpCache::Lookup HttpCache::lookup(Url const& url, std::int64_t now)
{
    Lookup result;
    std::string const key = url.serialize(true);
    auto const it = m_entries.find(key);
    if (it == m_entries.end())
        return result;
    Entry& entry = it->second;
    if (!entry.body_loaded && !load_body(key, entry, now)) {
        drop(it, true);
        return result;
    }
    result.response = &entry.response;
    result.fresh = entry.fresh_until > now;
    result.etag = entry.etag;
    result.last_modified = entry.last_modified;
    return result;
}

FetchResponse const* HttpCache::refresh(Url const& url, std::vector<Header> const& headers, std::int64_t now)
{
    std::string const key = url.serialize(true);
    auto const it = m_entries.find(key);
    if (it == m_entries.end())
        return nullptr;
    Entry& entry = it->second;
    if (!entry.body_loaded && !load_body(key, entry, now)) {
        drop(it, true);
        return nullptr;
    }
    // §3.2: the 304's fields replace the stored ones of the same name;
    // Content-Length describes the body that did not come, and a
    // Set-Cookie is the jar's, never the cache's.
    std::vector<Header> merged = entry.response.headers;
    for (Header const& fresh_header : headers) {
        if (ascii_ci_equals(fresh_header.name, "content-length") || ascii_ci_equals(fresh_header.name, "set-cookie"))
            continue;
        std::erase_if(merged, [&](Header const& old) { return ascii_ci_equals(old.name, fresh_header.name); });
    }
    for (Header const& fresh_header : headers) {
        if (ascii_ci_equals(fresh_header.name, "content-length") || ascii_ci_equals(fresh_header.name, "set-cookie"))
            continue;
        merged.push_back(fresh_header);
    }
    std::optional<std::int64_t> const until = fresh_until(merged, now, entry.response.status);
    if (!until) {
        drop(it, true); // the origin says: do not keep this
        return nullptr;
    }
    m_bytes -= entry.cost;
    entry.response.headers = std::move(merged);
    entry.cost = cost_of(key, entry.response);
    m_bytes += entry.cost;
    entry.fresh_until = *until;
    entry.stored_at = now;
    entry.etag = header_or_empty(entry.response.headers, "etag");
    entry.last_modified = header_or_empty(entry.response.headers, "last-modified");
    if (!m_directory.empty())
        write_entry(key, entry, false);
    return &entry.response;
}

bool HttpCache::store(Url const& url, FetchResponse const& response, std::int64_t now)
{
    if (response.status != 200)
        return false;
    std::optional<std::int64_t> const until = fresh_until(response.headers, now, response.status);
    if (!until)
        return false;
    if (*until <= now && !has_validator(response.headers))
        return false; // stale on arrival and no way to ask about it

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
    entry.body_bytes = response.body.size();
    entry.fresh_until = *until;
    entry.stored_at = now;
    entry.etag = header_or_empty(entry.response.headers, "etag");
    entry.last_modified = header_or_empty(entry.response.headers, "last-modified");
    if (entry.cost > m_max_entry_bytes)
        return false;

    if (auto const existing = m_entries.find(key); existing != m_entries.end())
        drop(existing, true);
    evict_to_fit(entry.cost, now);
    m_bytes += entry.cost;
    if (!m_directory.empty()) {
        evict_disk_to_fit(entry.body_bytes);
        write_entry(key, entry, true);
    }
    m_entries.emplace(std::move(key), std::move(entry));
    return true;
}

void HttpCache::clear()
{
    for (auto it = m_entries.begin(); it != m_entries.end();)
        drop(it++, true);
    m_entries.clear();
    m_bytes = 0;
    m_disk_bytes = 0;
}

// Removes an entry — from memory, and from the directory when asked.
void HttpCache::drop(std::unordered_map<std::string, Entry>::iterator it, bool files_too)
{
    m_bytes -= it->second.cost;
    if (it->second.on_disk) {
        m_disk_bytes -= std::min(m_disk_bytes, it->second.body_bytes);
        if (files_too)
            remove_files(it->first);
    }
    m_entries.erase(it);
}

void HttpCache::evict_to_fit(std::size_t incoming, std::int64_t now, std::string const* keep)
{
    // A stale entry with nothing to revalidate it by is dead weight: it
    // goes first, unconditionally. Then the oldest until the newcomer fits
    // — a body that is on disk is let go of, its entry kept; one that is
    // not takes its entry with it.
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->second.fresh_until <= now && it->second.etag.empty() && it->second.last_modified.empty()
            && (!keep || it->first != *keep))
            drop(it++, true);
        else
            ++it;
    }
    while (m_bytes + incoming > m_max_total_bytes) {
        auto oldest = m_entries.end();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->second.cost == 0 || !it->second.body_loaded || (keep && it->first == *keep))
                continue;
            if (oldest == m_entries.end() || it->second.stored_at < oldest->second.stored_at)
                oldest = it;
        }
        if (oldest == m_entries.end())
            break;
        Entry& entry = oldest->second;
        if (entry.on_disk) {
            m_bytes -= entry.cost;
            entry.response.body.clear();
            entry.response.body.shrink_to_fit();
            entry.body_loaded = false;
            entry.cost = cost_of(oldest->first, entry.response);
            m_bytes += entry.cost;
        } else {
            drop(oldest, true);
        }
    }
}

void HttpCache::evict_disk_to_fit(std::size_t incoming)
{
    while (!m_entries.empty() && m_disk_bytes + incoming > m_max_disk_bytes) {
        auto oldest = m_entries.end();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (!it->second.on_disk)
                continue;
            if (oldest == m_entries.end() || it->second.stored_at < oldest->second.stored_at)
                oldest = it;
        }
        if (oldest == m_entries.end())
            break;
        drop(oldest, true);
    }
}

// --- The directory ------------------------------------------------------------------------

std::string HttpCache::file_stem(std::string const& key) const
{
    crypto::Sha256::Digest const digest = crypto::Sha256::hash(
        std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(key.data()), key.size()));
    static constexpr char hex[] = "0123456789abcdef";
    std::string stem;
    for (std::uint8_t const byte : digest) {
        stem += hex[byte >> 4];
        stem += hex[byte & 15];
    }
    return (std::filesystem::path(m_directory) / stem).string();
}

void HttpCache::set_directory(std::string directory)
{
    m_directory = std::move(directory);
    if (m_directory.empty())
        return;
    std::error_code error;
    std::filesystem::create_directories(m_directory, error);
    if (!std::filesystem::is_directory(m_directory, error)) {
        m_directory.clear();
        return;
    }
    read_directory();
}

// An entry's two files: the body as it is, and the rest as text — a
// header line per stored header, each field of the entry a line of its
// own before them. The body is written first and the text last, so a
// write cut short leaves a body no text names, which the next read drops.
void HttpCache::write_entry(std::string const& key, Entry& entry, bool body_too)
{
    if (!clean_line(key))
        return;
    std::string const stem = file_stem(key);
    std::error_code error;
    if (body_too) {
        std::ofstream body(stem + ".body", std::ios::binary | std::ios::trunc);
        body.write(reinterpret_cast<char const*>(entry.response.body.data()),
            static_cast<std::streamsize>(entry.response.body.size()));
        if (!body)
            return;
    }
    std::ostringstream text;
    text << "sashfold-cache 1\n"
         << "url " << key << '\n'
         << "final " << entry.response.final_url.serialize() << '\n'
         << "status " << entry.response.status << ' ' << entry.response.status_text << '\n'
         << "stored " << entry.stored_at << '\n'
         << "fresh " << entry.fresh_until << '\n'
         << "body " << entry.body_bytes << '\n'
         << "--\n";
    for (Header const& header : entry.response.headers) {
        if (clean_line(header.name) && clean_line(header.value))
            text << header.name << ": " << header.value << '\n';
    }
    {
        std::ofstream meta(stem + ".meta.tmp", std::ios::binary | std::ios::trunc);
        meta << text.str();
        if (!meta)
            return;
    }
    std::filesystem::rename(stem + ".meta.tmp", stem + ".meta", error);
    if (error)
        return;
    if (!entry.on_disk) {
        entry.on_disk = true;
        m_disk_bytes += entry.body_bytes;
    }
}

void HttpCache::remove_files(std::string const& key)
{
    if (m_directory.empty())
        return;
    std::string const stem = file_stem(key);
    std::error_code error;
    std::filesystem::remove(stem + ".meta", error);
    std::filesystem::remove(stem + ".body", error);
}

bool HttpCache::load_body(std::string const& key, Entry& entry, std::int64_t now)
{
    if (m_directory.empty() || !entry.on_disk)
        return false;
    std::ifstream body(file_stem(key) + ".body", std::ios::binary);
    if (!body)
        return false;
    std::vector<std::uint8_t> bytes(entry.body_bytes);
    body.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(body.gcount()) != entry.body_bytes)
        return false;
    m_bytes -= entry.cost;
    entry.response.body = std::move(bytes);
    entry.body_loaded = true;
    entry.cost = cost_of(key, entry.response);
    m_bytes += entry.cost;
    evict_to_fit(0, now, &key); // room in memory, at the expense of older bodies, never this one
    return true;
}

void HttpCache::read_directory()
{
    std::error_code error;
    std::vector<std::filesystem::path> bodies;
    for (std::filesystem::directory_entry const& file : std::filesystem::directory_iterator(m_directory, error)) {
        if (!file.is_regular_file(error))
            continue;
        std::filesystem::path const& path = file.path();
        if (path.extension() == ".body") {
            bodies.push_back(path);
            continue;
        }
        if (path.extension() != ".meta")
            continue;
        // The text is read whole and closed before anything is removed:
        // Windows refuses to delete a file that is still open.
        std::ifstream meta(path, std::ios::binary);
        std::string line;
        if (!std::getline(meta, line) || line != "sashfold-cache 1") {
            meta.close();
            std::filesystem::remove(path, error);
            continue;
        }
        Entry entry;
        std::string key;
        bool headers_now = false;
        bool complete = false;
        while (std::getline(meta, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (headers_now) {
                std::size_t const colon = line.find(": ");
                if (colon != std::string::npos)
                    entry.response.headers.push_back({ line.substr(0, colon), line.substr(colon + 2) });
                continue;
            }
            if (line == "--") {
                headers_now = true;
                complete = true;
                continue;
            }
            std::size_t const space = line.find(' ');
            std::string const field = line.substr(0, space);
            std::string const value = space == std::string::npos ? std::string() : line.substr(space + 1);
            if (field == "url") {
                key = value;
            } else if (field == "final") {
                if (std::optional<Url> const url = parse_url(value))
                    entry.response.final_url = *url;
            } else if (field == "status") {
                std::size_t const gap = value.find(' ');
                entry.response.status = std::atoi(value.substr(0, gap).c_str());
                entry.response.status_text = gap == std::string::npos ? std::string() : value.substr(gap + 1);
            } else if (field == "stored") {
                entry.stored_at = std::atoll(value.c_str());
            } else if (field == "fresh") {
                entry.fresh_until = std::atoll(value.c_str());
            } else if (field == "body") {
                entry.body_bytes = static_cast<std::size_t>(std::atoll(value.c_str()));
            }
        }
        meta.close();
        std::string const stem = path.string().substr(0, path.string().size() - 5);
        if (!complete || key.empty() || entry.response.status != 200 || m_entries.contains(key)
            || file_stem(key) != stem || !std::filesystem::is_regular_file(stem + ".body", error)) {
            std::filesystem::remove(path, error);
            std::filesystem::remove(stem + ".body", error);
            continue;
        }
        entry.response.from_cache = true;
        entry.body_loaded = false;
        entry.on_disk = true;
        entry.etag = header_or_empty(entry.response.headers, "etag");
        entry.last_modified = header_or_empty(entry.response.headers, "last-modified");
        entry.cost = cost_of(key, entry.response);
        m_bytes += entry.cost;
        m_disk_bytes += entry.body_bytes;
        m_entries.emplace(std::move(key), std::move(entry));
    }
    // A body no text names is a write that was cut short.
    for (std::filesystem::path const& body : bodies) {
        std::string const stem = body.string().substr(0, body.string().size() - 5);
        if (!std::filesystem::is_regular_file(stem + ".meta", error))
            std::filesystem::remove(body, error);
    }
}

}
