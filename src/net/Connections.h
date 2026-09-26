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
// The pool serves every fetch of the session, and those run on several
// threads at once: every call takes the pool's lock. A connection handed out
// is the caller's alone until it is given back.
//
// An origin that speaks HTTP/2 gets one connection, shared: the pool keeps
// its session, which every fetch to the origin rides at once. While the
// first connection to an origin that may speak it is being opened, other
// fetches to that origin wait to learn what it spoke, rather than each
// opening a connection of its own that the session would make redundant.

#include "platform/Net.h"
#include "platform/Tls.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sashfold::net {

// What opening a connection cost, in milliseconds: the name lookup, the
// TCP connect and, over https, the TLS handshake.
struct ConnectionTiming {
    double resolve_ms = 0;
    double connect_ms = 0;
    double tls_ms = 0;
};

// One established transport, plain or TLS. Move-only.
class Connection {
public:
    // Connects and, for https, handshakes with SNI for `host`, offering
    // HTTP/2 by ALPN when `offer_h2` is set. On failure `error` names the
    // step that failed, in the words the shell keys on. `timing`, when
    // given, receives what each step took, on failure too.
    static std::optional<Connection> open(std::string const& host, std::uint16_t port,
        bool secure, std::string& error, ConnectionTiming* timing = nullptr, bool offer_h2 = false);

    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;
    Connection(Connection const&) = delete;
    Connection& operator=(Connection const&) = delete;

    bool secure() const { return m_tls.has_value(); }
    // What the TLS handshake settled on ("h2", "http/1.1"), or empty.
    std::string alpn() const { return m_tls ? m_tls->alpn() : std::string(); }
    // One thread may receive while another sends (see TlsSocket).
    bool send_all(std::uint8_t const* data, std::size_t size);
    // >0 bytes received, 0 orderly close, <0 error.
    std::ptrdiff_t receive(std::uint8_t* buffer, std::size_t size);
    // How long one receive may wait for the peer; zero waits indefinitely.
    bool set_receive_timeout(int milliseconds);
    // Wakes a receive blocked on another thread; the connection is done.
    void shutdown();

private:
    Connection() = default;
    std::optional<platform::TcpSocket> m_tcp; // plain
    std::optional<platform::TlsSocket> m_tls; // secure: owns its socket
};

// The key a connection is pooled under: "https://host:443".
std::string origin_key(bool secure, std::string const& host, std::uint16_t port);

class Http2Session;

class ConnectionPool {
public:
    explicit ConnectionPool(std::size_t max_idle = 32, std::size_t max_idle_per_origin = 6,
        std::int64_t max_idle_seconds = 60)
        : m_max_idle(max_idle)
        , m_max_idle_per_origin(max_idle_per_origin)
        , m_max_idle_seconds(max_idle_seconds)
    {
    }
    // Closes every HTTP/2 session and waits for their readers.
    ~ConnectionPool();
    ConnectionPool(ConnectionPool const&) = delete;
    ConnectionPool& operator=(ConnectionPool const&) = delete;

    // The origin's HTTP/2 session when one is up and taking requests. With
    // none, and another fetch opening the first connection to the origin,
    // this waits for that one to settle and looks again. When it returns
    // nothing and sets `claimed`, the caller is the one opening a
    // connection and must settle() it, whatever becomes of it. An origin
    // known to speak only HTTP/1.1 returns nothing at once, unclaimed.
    std::shared_ptr<Http2Session> find_session(std::string const& key, std::int64_t now, bool& claimed);
    // Ends a claim: the session the new connection became, or nothing when
    // it did not become one (it spoke HTTP/1.1, or never connected).
    void settle(std::string const& key, std::shared_ptr<Http2Session> session);
    // The origin speaks HTTP/1.1 only: its server chose it by ALPN, or its
    // HTTP/2 failed. No fetch to it waits on another's connect after this.
    void note_http1_only(std::string const& key);
    bool http1_only(std::string const& key) const;
    std::size_t sessions() const;

    // The most recently kept connection to the origin, if it is still
    // within its idle allowance (the youngest is the likeliest to be alive).
    // A connection handed out counts as reused.
    std::optional<Connection> take(std::string const& key, std::int64_t now);

    // Keeps a connection for a later request to the same origin. Past the
    // per-origin or the total limit, the longest-idle connection goes.
    void give(std::string const& key, Connection connection, std::int64_t now);

    std::size_t size() const
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        return m_idle.size();
    }
    void clear();

    // What the session's connections cost, for --bench and tests:
    // connections opened, requests served on a pooled connection or an
    // existing HTTP/2 session, requests re-sent on a fresh connection after
    // a pooled one failed (or an HTTP/2 server refused them, or spoke the
    // protocol badly), and the connections that became HTTP/2 sessions.
    struct Stats {
        std::size_t opened = 0;
        std::size_t reused = 0;
        std::size_t retried = 0;
        std::size_t sessions = 0;
    };
    Stats stats() const
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        return m_stats;
    }
    void note_opened()
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        ++m_stats.opened;
    }
    void note_retried()
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        ++m_stats.retried;
    }

private:
    struct Idle {
        std::string key;
        Connection connection;
        std::int64_t since; // unix seconds
    };
    void expire(std::int64_t now);
    // Takes out the sessions that went away or sat idle too long, to be
    // released by the caller outside the lock (releasing one joins its
    // reader).
    void expire_sessions(std::int64_t now, std::vector<std::shared_ptr<Http2Session>>& gone);

    std::size_t m_max_idle;
    std::size_t m_max_idle_per_origin;
    std::int64_t m_max_idle_seconds;
    mutable std::mutex m_mutex;
    std::condition_variable m_settled;
    std::vector<Idle> m_idle; // longest idle first
    std::map<std::string, std::shared_ptr<Http2Session>> m_sessions;
    std::set<std::string> m_connecting; // origins whose first connection is being opened
    std::set<std::string> m_http1_only;
    Stats m_stats;
};

}
