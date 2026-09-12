#pragma once
// Certificate path validation (RFC 5280 profile, _plans/tls-DESIGN.md §6):
// build a path from the server's leaf to a trust anchor and check every
// link's signature, the validity window, the CA constraints, the key
// strength, the leaf's hostname and, when a fetcher is given, revocation.
#include "net/tls/TrustStore.h"
#include "net/tls/X509.h"

#include <cstdint>
#include <functional>
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
class CrlCache {
public:
    explicit CrlCache(HttpFetch fetch)
        : m_fetch(std::move(fetch))
    {
    }

    // The bytes of the list at `url` as of `now` (seconds since the epoch),
    // from the cache or the network; empty when neither has it.
    std::vector<std::uint8_t> get(std::string const& url, std::int64_t now);
    // A fetcher over this cache, for validate_chain.
    HttpFetch fetcher(std::int64_t now);

    std::size_t fetches() const { return m_fetches; }

    static constexpr std::int64_t longest_hold_seconds = 24 * 60 * 60;
    static constexpr std::int64_t unnamed_hold_seconds = 60 * 60;
    static constexpr std::int64_t failure_hold_seconds = 10 * 60;

private:
    struct Entry {
        std::vector<std::uint8_t> bytes;
        std::int64_t expires = 0;
    };
    HttpFetch m_fetch;
    std::vector<std::pair<std::string, Entry>> m_entries;
    std::size_t m_fetches = 0;
};

}
