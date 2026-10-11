#pragma once
// Certificate path validation (RFC 5280 profile, _plans/tls-DESIGN.md §6):
// build a path from the server's leaf to a trust anchor and check every
// link's signature, the validity window, the CA constraints, the key
// strength, the leaf's hostname and, when a fetcher is given, revocation.
#include "net/tls/TrustStore.h"
#include "net/tls/X509.h"

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sashfold::tls {

struct Verdict {
    bool trusted = false;
    std::string reason; // empty when trusted; the failing check otherwise
};

// Fetches the bytes at an http:// URL (a CRL), or returns empty. Optional;
// nullptr skips revocation with a soft pass, as the OS validators do when
// a revocation source is unreachable.
using HttpFetch = std::function<std::vector<std::uint8_t>(std::string const& url)>;

// `chain` is the server's Certificate message, leaf first. `host` is the
// name being connected to (empty for an IP-literal connection, which then
// matches an iPAddress SAN). `now` is seconds since the epoch.
Verdict validate_chain(std::vector<Certificate> const& chain, std::string const& host, std::int64_t now,
    TrustStore const& store, HttpFetch const& fetch_crl);

// Whether a presented dNSName matches the wanted host, with a single
// leftmost wildcard label (exposed for testing).
bool host_matches(std::string const& presented, std::string const& host);

// Revocation lists by URL, each kept until its nextUpdate (a day at the
// most, an hour when the list names none), so a distribution point is
// asked once per list and not once per connection; a fetch that came back
// empty is remembered for ten minutes, so a point that is down does not
// slow every handshake to its timeout. The fetch given does the HTTP.
//
// With a folder set, each list fetched is also written there and read back
// by the next run until the same expiry, as the operating systems' own
// validators keep theirs (Windows' CryptnetUrlCache), so a browser start
// does not download every CA's list again; failures stay in memory only.
// Safe from several threads: handshakes wanting the same list share its
// one download, and a download blocks only the handshakes waiting on it.
class CrlCache {
public:
    explicit CrlCache(HttpFetch fetch)
        : m_fetch(std::move(fetch))
    {
    }

    // Where the lists are kept between runs; empty keeps them in memory.
    void set_directory(std::string directory);

    // The bytes of the list at `url` as of `now` (seconds since the epoch),
    // from the cache, the folder or the network; empty when none has it.
    std::vector<std::uint8_t> get(std::string const& url, std::int64_t now);
    // A fetcher over this cache, for validate_chain.
    HttpFetch fetcher(std::int64_t now);

    std::size_t fetches() const;

    static constexpr std::int64_t longest_hold_seconds = 24 * 60 * 60;
    static constexpr std::int64_t unnamed_hold_seconds = 60 * 60;
    static constexpr std::int64_t failure_hold_seconds = 10 * 60;

private:
    struct Entry {
        std::vector<std::uint8_t> bytes;
        std::int64_t expires = 0;
    };
    Entry const* find(std::string const& url, std::int64_t now) const; // under m_mutex
    std::optional<Entry> read_kept(std::string const& url, std::int64_t now) const;
    void keep(std::string const& url, Entry const& entry) const;
    std::string path_for(std::string const& url) const;

    HttpFetch m_fetch;
    mutable std::mutex m_mutex;
    std::condition_variable m_arrived;
    std::vector<std::pair<std::string, Entry>> m_entries;
    std::vector<std::string> m_in_flight; // URLs being fetched now
    std::string m_directory;
    std::size_t m_fetches = 0;
};

}
