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

}
