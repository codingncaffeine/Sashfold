#pragma once

// Persistent connections. A framed response on a connection the server did
// not close leaves that connection good for the next request, so the session
// keeps it — keyed by scheme, host and port, for a bounded idle time — and
// the next fetch to that origin skips the TCP connect and, over https, the
// TLS handshake: the cost that dominated a page with a dozen stylesheets and
// images, each on a fresh connection. The pool is a session object like the
// cookie jar and the cache; fetch() takes it through FetchOptions and keeps
// nothing without one.
//
// A pooled connection can be dead by the time it is reused (the server's
// idle timeout won the race, or the network went away), which shows up as a
// failure on the very next request. fetch() retries such a request once on
// a fresh connection; GET is the only method sent, so that is safe.
// Single-threaded, like the rest of the session.

#include "platform/Net.h"
#include "platform/Tls.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::net {

// One established transport, plain or TLS. Move-only.
class Connection {
public:
    // Connects and, for https, handshakes with SNI for `host`. On failure
    // `error` names the step that failed, in the words the shell keys on.
    static std::optional<Connection> open(std::string const& host, std::uint16_t port,
        bool secure, std::string& error);

    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;
    Connection(Connection const&) = delete;
    Connection& operator=(Connection const&) = delete;

    bool secure() const { return m_tls.has_value(); }
    bool send_all(std::uint8_t const* data, std::size_t size);
    // >0 bytes received, 0 orderly close, <0 error.
    std::ptrdiff_t receive(std::uint8_t* buffer, std::size_t size);

private:
    Connection() = default;
    std::optional<platform::TcpSocket> m_tcp; // plain
    std::optional<platform::TlsSocket> m_tls; // secure: owns its socket
};

// The key a connection is pooled under: "https://host:443".
std::string origin_key(bool secure, std::string const& host, std::uint16_t port);

class ConnectionPool {
public:
    explicit ConnectionPool(std::size_t max_idle = 32, std::size_t max_idle_per_origin = 6,
        std::int64_t max_idle_seconds = 60)
        : m_max_idle(max_idle)
        , m_max_idle_per_origin(max_idle_per_origin)
        , m_max_idle_seconds(max_idle_seconds)
    {
    }

    // The most recently kept connection to the origin, if it is still
    // within its idle allowance (the youngest is the likeliest to be alive).
    // A connection handed out counts as reused.
    std::optional<Connection> take(std::string const& key, std::int64_t now);

    // Keeps a connection for a later request to the same origin. Past the
    // per-origin or the total limit, the longest-idle connection goes.
    void give(std::string const& key, Connection connection, std::int64_t now);

    std::size_t size() const { return m_idle.size(); }
    void clear() { m_idle.clear(); }

    // What the session's connections cost, for --bench and tests:
    // connections opened, requests served on a pooled connection, and
    // requests re-sent on a fresh connection after a pooled one failed.
    struct Stats {
        std::size_t opened = 0;
        std::size_t reused = 0;
        std::size_t retried = 0;
    };
    Stats const& stats() const { return m_stats; }
    void note_opened() { ++m_stats.opened; }
    void note_retried() { ++m_stats.retried; }

private:
    struct Idle {
        std::string key;
        Connection connection;
        std::int64_t since; // unix seconds
    };
    void expire(std::int64_t now);

    std::size_t m_max_idle;
    std::size_t m_max_idle_per_origin;
    std::int64_t m_max_idle_seconds;
    std::vector<Idle> m_idle; // longest idle first
    Stats m_stats;
};

}
