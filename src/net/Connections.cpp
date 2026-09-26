#include "net/Connections.h"

#include "net/Http2Session.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace sashfold::net {

std::optional<Connection> Connection::open(std::string const& host, std::uint16_t port,
    bool secure, std::string& error, ConnectionTiming* timing, bool offer_h2)
{
    // (IPv6 hosts are stored bracket-free, which is what getaddrinfo wants;
    // the messages put the brackets back for the reader.)
    std::string const shown = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    platform::ConnectTiming connected;
    auto tcp = platform::TcpSocket::connect(host, port, &connected);
    if (timing) {
        timing->resolve_ms = connected.resolve_ms;
        timing->connect_ms = connected.connect_ms;
    }
    if (!tcp) {
        error = "could not connect to " + shown;
        return std::nullopt;
    }
    Connection connection;
    if (secure) {
        auto const handshake_started = std::chrono::steady_clock::now();
        auto tls = platform::TlsSocket::connect(std::move(*tcp), host, port, offer_h2);
        if (timing)
            timing->tls_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - handshake_started).count();
        if (!tls) {
            error = "TLS handshake or certificate validation failed for " + shown;
            return std::nullopt;
        }
        connection.m_tls = std::move(*tls);
    } else {
        connection.m_tcp = std::move(*tcp);
    }
    return connection;
}

bool Connection::send_all(std::uint8_t const* data, std::size_t size)
{
    if (m_tls)
        return m_tls->send_all(data, size);
    if (m_tcp)
        return m_tcp->send_all(data, size);
    return false;
}

std::ptrdiff_t Connection::receive(std::uint8_t* buffer, std::size_t size)
{
    if (m_tls)
        return m_tls->receive(buffer, size);
    if (m_tcp)
        return m_tcp->receive(buffer, size);
    return -1;
}

bool Connection::set_receive_timeout(int milliseconds)
{
    if (m_tls)
        return m_tls->set_receive_timeout(milliseconds);
    if (m_tcp)
        return m_tcp->set_receive_timeout(milliseconds);
    return false;
}

void Connection::shutdown()
{
    if (m_tls)
        m_tls->shutdown();
    if (m_tcp)
        m_tcp->shutdown();
}

std::string origin_key(bool secure, std::string const& host, std::uint16_t port)
{
    return (secure ? "https://" : "http://") + host + ":" + std::to_string(port);
}

void ConnectionPool::expire(std::int64_t now)
{
    std::erase_if(m_idle, [&](Idle const& idle) { return now - idle.since > m_max_idle_seconds; });
}

std::optional<Connection> ConnectionPool::take(std::string const& key, std::int64_t now)
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    expire(now);
    auto const found = std::find_if(m_idle.rbegin(), m_idle.rend(),
        [&](Idle const& idle) { return idle.key == key; });
    if (found == m_idle.rend())
        return std::nullopt;
    Connection connection = std::move(found->connection);
    m_idle.erase(std::next(found).base());
    ++m_stats.reused;
    return connection;
}

void ConnectionPool::give(std::string const& key, Connection connection, std::int64_t now)
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    expire(now);
    if (m_max_idle == 0 || m_max_idle_per_origin == 0)
        return;
    auto const same_origin = [&](Idle const& idle) { return idle.key == key; };
    if (static_cast<std::size_t>(std::count_if(m_idle.begin(), m_idle.end(), same_origin))
        >= m_max_idle_per_origin) {
        m_idle.erase(std::find_if(m_idle.begin(), m_idle.end(), same_origin));
    }
    if (m_idle.size() >= m_max_idle)
        m_idle.erase(m_idle.begin());
    m_idle.push_back(Idle { key, std::move(connection), now });
}

ConnectionPool::~ConnectionPool()
{
    std::map<std::string, std::shared_ptr<Http2Session>> sessions;
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        sessions.swap(m_sessions);
    }
    for (auto& [key, session] : sessions)
        session->close();
}

void ConnectionPool::clear()
{
    std::map<std::string, std::shared_ptr<Http2Session>> sessions;
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        m_idle.clear();
        sessions.swap(m_sessions);
    }
    for (auto& [key, session] : sessions)
        session->close();
}

void ConnectionPool::expire_sessions(std::int64_t now, std::vector<std::shared_ptr<Http2Session>>& gone)
{
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (!it->second->accepting() || it->second->idle_seconds(now) > m_max_idle_seconds) {
            gone.push_back(std::move(it->second));
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

std::shared_ptr<Http2Session> ConnectionPool::find_session(std::string const& key, std::int64_t now, bool& claimed)
{
    claimed = false;
    std::vector<std::shared_ptr<Http2Session>> gone;
    std::shared_ptr<Http2Session> found;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (true) {
            expire_sessions(now, gone);
            if (m_http1_only.contains(key))
                break;
            auto const session = m_sessions.find(key);
            if (session != m_sessions.end()) {
                found = session->second;
                ++m_stats.reused;
                break;
            }
            if (!m_connecting.contains(key)) {
                m_connecting.insert(key);
                claimed = true;
                break;
            }
            m_settled.wait(lock);
        }
    }
    // An idle session goes now, with a GOAWAY. One the server sent away may
    // still be finishing streams for the fetches that hold it; it ends on
    // its own when they are done, and the last of them releases it.
    for (std::shared_ptr<Http2Session> const& session : gone) {
        if (session->accepting())
            session->close();
    }
    return found;
}

void ConnectionPool::settle(std::string const& key, std::shared_ptr<Http2Session> session)
{
    std::shared_ptr<Http2Session> replaced;
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        m_connecting.erase(key);
        if (session) {
            ++m_stats.sessions;
            std::shared_ptr<Http2Session>& slot = m_sessions[key];
            replaced = std::move(slot);
            slot = std::move(session);
        }
    }
    m_settled.notify_all();
}

void ConnectionPool::note_http1_only(std::string const& key)
{
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        // Bounded, as the origins a session meets are not: past the bound
        // the memory starts over, and an origin forgotten costs one more
        // connection that asks for h2 and is told otherwise.
        if (m_http1_only.size() >= 4096)
            m_http1_only.clear();
        m_http1_only.insert(key);
    }
    m_settled.notify_all();
}

bool ConnectionPool::http1_only(std::string const& key) const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    return m_http1_only.contains(key);
}

std::size_t ConnectionPool::sessions() const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    return m_sessions.size();
}

}
