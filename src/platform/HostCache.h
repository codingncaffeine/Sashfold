#pragma once

// The process's answers from the system resolver, kept for a minute as
// Chromium's host cache keeps them (getaddrinfo says nothing of a record's
// lifetime, so a fixed hold stands in for it). A machine without a caching
// resolver of its own — Debian's default, among others — otherwise asks the
// network again for every connection. The connects and a page's
// dns-prefetch lookups share it; a host none of whose addresses would take
// a connection is forgotten, so the next connect asks afresh.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sashfold::platform {

// One address as the resolver gave it, the socket's arguments with it; the
// address bytes are a sockaddr of `family` with its port left at zero.
struct ResolvedAddress {
    int family = 0;
    int socktype = 0;
    int protocol = 0;
    std::vector<std::uint8_t> address;
};

class HostCache {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds hold { 60 };
    static constexpr std::size_t most_hosts = 512;

    // The host's addresses if they were kept within the hold, else nothing.
    std::optional<std::vector<ResolvedAddress>> find(std::string const& host, Clock::time_point now);
    // Keeps what the resolver answered (an empty answer is not kept).
    void keep(std::string const& host, std::vector<ResolvedAddress> addresses, Clock::time_point now);
    void forget(std::string const& host);
    void clear();

    // How many times a name was handed to the system resolver: what the
    // tests read to see that a kept answer was used.
    std::size_t lookups() const;
    void count_lookup();

private:
    struct Entry {
        std::vector<ResolvedAddress> addresses;
        Clock::time_point kept;
    };
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
    std::size_t m_lookups = 0;
};

// The one the process's sockets use.
HostCache& host_cache();

}
