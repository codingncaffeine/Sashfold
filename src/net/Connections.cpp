#include "net/Connections.h"

#include <algorithm>
#include <utility>

namespace sashfold::net {

std::optional<Connection> Connection::open(std::string const& host, std::uint16_t port,
    bool secure, std::string& error)
{
    // (IPv6 hosts are stored bracket-free, which is what getaddrinfo wants;
    // the messages put the brackets back for the reader.)
    std::string const shown = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    auto tcp = platform::TcpSocket::connect(host, port);
    if (!tcp) {
        error = "could not connect to " + shown;
        return std::nullopt;
    }
    Connection connection;
    if (secure) {
        auto tls = platform::TlsSocket::connect(std::move(*tcp), host);
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

}
