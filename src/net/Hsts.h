#pragma once

// HTTP Strict Transport Security (RFC 6797): a host that says, over HTTPS,
// that it is to be reached over HTTPS only is remembered for as long as it
// asked, and every later request to it — and to its subdomains when it said
// so — is rewritten to https before a connection is opened, so nothing goes
// out in the clear to a host that asked for that.
//
// What is kept is what hosts said themselves: there is no built-in list, so
// the first visit to a host is as its address was written. An address that
// is an IP literal is never remembered (§8.1.1), and a header that came over
// plain HTTP means nothing (§8.1): anyone on the path could have written it.
//
// The store keeps no file of its own: it serializes to lines of
// `host <tab> expiry in Unix seconds <tab> 1 when subdomains count, else 0`
// and the shell's profile decides where that file lives and when it is
// written.

#include "net/Http.h"
#include "net/Url.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::net {

// One Strict-Transport-Security header's meaning (§6.1), or nothing for a
// header that is not valid: no max-age, a max-age that is not a number of
// seconds, a directive given twice, or a directive that is not a token with
// an optional value. Directives nobody knows are passed over.
struct StrictTransportSecurity {
    std::int64_t max_age = 0; // seconds; 0 takes the host off the list
    bool include_subdomains = false;
};
std::optional<StrictTransportSecurity> parse_strict_transport_security(std::string_view value);

// The hosts known to want HTTPS. A store serves every fetch of its
// profile, and those run on several threads at once: every call takes the
// store's lock.
class TransportSecurity {
public:
    // What a response said, when it came over HTTPS from a host that has a
    // name: the first Strict-Transport-Security header alone counts (§8.1).
    void note(Url const& url, std::vector<Header> const& response_headers, std::int64_t now);
    // Is this host to be reached over HTTPS only — itself on the list, or
    // under a host that is and said its subdomains count (§8.2)?
    bool known(std::string_view host, std::int64_t now) const;
    // The address as it must go out (§8.3): an http or ws address of a
    // known host becomes https or wss, a port written as the scheme's
    // default goes with it and any other port stays. Every other address
    // comes back as it was.
    Url upgraded(Url url, std::int64_t now) const;

    std::size_t size() const
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        return m_hosts.size();
    }
    // The list as text, a host already expired left out; and a file read
    // back, added to what the store holds.
    std::string serialize(std::int64_t now) const;
    void load(std::string_view text, std::int64_t now);
    // Moves whenever a host is added, changed or taken off: a shell writes
    // the list out when it has moved.
    std::uint64_t changes() const
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        return m_changes;
    }

    // No more hosts than this are kept: past it the ones that have expired
    // go, then the one that expires soonest.
    static constexpr std::size_t max_hosts = 20000;

private:
    struct Entry {
        std::int64_t expires = 0; // unix seconds
        bool include_subdomains = false;
    };
    void put(std::string host, Entry entry, std::int64_t now); // under the lock

    mutable std::mutex m_mutex;
    std::map<std::string, Entry, std::less<>> m_hosts; // by host, lowercase, no trailing dot
    std::uint64_t m_changes = 0;
};

}
