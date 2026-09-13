#pragma once

// The HTTP cache (RFC 9111). A stored response is served while it is
// fresh — by its max-age or Expires, else, for a 200 that carries a
// Last-Modified, by a tenth of its age so far and a week at most — and a
// stale one that carries a validator is revalidated: the request goes
// out with If-None-Match or If-Modified-Since, and a 304 renews the
// stored copy in place, its body never sent again. no-store is honoured
// absolutely; no-cache stores but never serves without revalidating; a
// private cache, so `private` stores. Storable: 200 responses whose Vary
// names only fields fetch() sends identically every time (Accept,
// Accept-Encoding, User-Agent). Set-Cookie headers are dropped from the
// stored copy so a hit never replays a cookie.
//
// The entries live in memory and, when a directory is given, in it as
// well — a file of headers and a file of body per entry, read back at
// the next start with the bodies loaded as they are asked for — under a
// cap of their own. History navigation does not use this cache: the
// shell keeps its own document copy per history entry, so Back never
// re-fetches.

#include "net/Http.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sashfold::net {

class HttpCache {
public:
    explicit HttpCache(std::size_t max_total_bytes = 64u * 1024u * 1024u,
        std::size_t max_entry_bytes = 8u * 1024u * 1024u,
        std::size_t max_disk_bytes = 256u * 1024u * 1024u)
        : m_max_total_bytes(max_total_bytes)
        , m_max_entry_bytes(max_entry_bytes)
        , m_max_disk_bytes(max_disk_bytes)
    {
    }

    // The directory the entries are kept in between runs, made when
    // missing; what it holds is read now, bodies left on disk until they
    // are asked for. Empty for none.
    void set_directory(std::string directory);
    std::string const& directory() const { return m_directory; }

    struct Lookup {
        FetchResponse const* response = nullptr; // the stored one, fresh or not; null for none
        bool fresh = false; // it may be served as it is
        std::string etag; // its validators, for a conditional request; empty for none
        std::string last_modified;
    };
    // The stored response for the URL (fragment ignored) and whether it
    // may be served as it is. The pointer stays valid until the next
    // store(), refresh() or clear().
    Lookup lookup(Url const& url, std::int64_t now);

    // A 304's headers renew the stored response (RFC 9111 §3.2, §4.3.4):
    // its headers are replaced by the 304's and its freshness recomputed
    // from them. The response to serve, or null when nothing is stored
    // for the URL any more.
    FetchResponse const* refresh(Url const& url, std::vector<Header> const& headers, std::int64_t now);

    // Stores a cacheable 200 response under the URL (fragment ignored) —
    // fresh, or stale with a validator to revalidate it by — and returns
    // true; anything not storable is ignored.
    bool store(Url const& url, FetchResponse const& response, std::int64_t now);

    std::size_t size() const { return m_entries.size(); }
    std::size_t bytes() const { return m_bytes; } // in memory
    std::size_t disk_bytes() const { return m_disk_bytes; }
    // Forgets everything, the directory's files included.
    void clear();

private:
    struct Entry {
        FetchResponse response; // its body empty while it waits on disk
        std::size_t cost = 0; // what it holds in memory
        std::int64_t fresh_until = 0; // unix seconds, exclusive
        std::int64_t stored_at = 0;
        std::string etag;
        std::string last_modified;
        bool body_loaded = true;
        std::size_t body_bytes = 0; // the body's size, loaded or not
        bool on_disk = false;
    };
    bool load_body(std::string const& key, Entry& entry, std::int64_t now);
    // Makes room in memory for `incoming` bytes; the entry under `keep`,
    // the one being served, is never touched.
    void evict_to_fit(std::size_t incoming, std::int64_t now, std::string const* keep = nullptr);
    void evict_disk_to_fit(std::size_t incoming);
    void write_entry(std::string const& key, Entry& entry, bool body_too);
    void remove_files(std::string const& key);
    void read_directory();
    std::string file_stem(std::string const& key) const;
    void drop(std::unordered_map<std::string, Entry>::iterator it, bool files_too);

    std::size_t m_max_total_bytes;
    std::size_t m_max_entry_bytes;
    std::size_t m_max_disk_bytes;
    std::unordered_map<std::string, Entry> m_entries; // key: URL sans fragment
    std::size_t m_bytes = 0;
    std::size_t m_disk_bytes = 0;
    std::string m_directory;
};

// Exposed for tests: the instant (unix seconds) until which a response
// with these headers is fresh, as of `now` — max-age (less Age) first,
// else Expires (relative to Date when both parse), else, for a 200 with a
// Last-Modified, the heuristic tenth of its age so far, a week at most.
// A value <= now means stale now: storable only to revalidate, which
// no-cache also means, and which a response with a validator but no
// freshness at all means. nullopt means not storable at all: no-store, a
// Vary we cannot honour, or nothing to go by.
std::optional<std::int64_t> fresh_until(std::vector<Header> const& headers, std::int64_t now, int status = 200);

}
