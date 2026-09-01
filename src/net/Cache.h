#pragma once

// HTTP cache v0 (M3): in-memory, per-session, explicit freshness only — a
// response is served from cache while Cache-Control: max-age (or an Expires
// date) says it is fresh, and no-store is honored absolutely. No heuristic
// freshness and no revalidation yet (conditional requests arrive with cache
// maturity, plan M7), so no-cache is treated as "do not store". History
// navigation does not use this cache — the shell keeps its own document
// copy per history entry, so Back never re-fetches.
//
// Storable: 200 responses whose Vary names only fields fetch() sends
// identically every time (Accept, Accept-Encoding, User-Agent). Set-Cookie
// headers are dropped from the stored copy so a hit never replays a cookie.

#include "net/Http.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sashfold::net {

class MemoryCache {
public:
    explicit MemoryCache(std::size_t max_total_bytes = 64u * 1024u * 1024u,
        std::size_t max_entry_bytes = 8u * 1024u * 1024u)
        : m_max_total_bytes(max_total_bytes)
        , m_max_entry_bytes(max_entry_bytes)
    {
    }

    // A fresh stored response for the URL (fragment ignored), or nullptr.
    // The pointer stays valid until the next store() or clear().
    FetchResponse const* lookup(Url const& url, std::int64_t now) const;

    // Stores a cacheable 200 response under the URL (fragment ignored) and
    // returns true; anything not storable is ignored.
    bool store(Url const& url, FetchResponse const& response, std::int64_t now);

    std::size_t size() const { return m_entries.size(); }
    std::size_t bytes() const { return m_bytes; }
    void clear()
    {
        m_entries.clear();
        m_bytes = 0;
    }

private:
    struct Entry {
        FetchResponse response;
        std::size_t cost = 0;
        std::int64_t fresh_until = 0; // unix seconds, exclusive
        std::int64_t stored_at = 0;
    };
    void evict_to_fit(std::size_t incoming, std::int64_t now);

    std::size_t m_max_total_bytes;
    std::size_t m_max_entry_bytes;
    std::unordered_map<std::string, Entry> m_entries; // key: URL sans fragment
    std::size_t m_bytes = 0;
};

// Exposed for tests: the instant (unix seconds) until which a response with
// these headers is fresh, as of `now` — max-age (less Age) first, else
// Expires (relative to Date when both parse); a value <= now means already
// stale. nullopt means not storable at all: no-store, no-cache, a Vary we
// cannot honor, or no explicit freshness (heuristic freshness is a later
// decision, made deliberately).
std::optional<std::int64_t> fresh_until(std::vector<Header> const& headers, std::int64_t now);

}
