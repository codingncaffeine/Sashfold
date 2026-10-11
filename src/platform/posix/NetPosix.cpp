#include "platform/Net.h"

#include "platform/HostCache.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sashfold::platform {

namespace {

constexpr std::uintptr_t invalid_handle = static_cast<std::uintptr_t>(-1);

int fd_of(std::uintptr_t handle)
{
    return static_cast<int>(handle);
}

// Each address gets this long to answer the SYN. A route that swallows it
// — an IPv6 address on a machine with an IPv6 address of its own but no
// way out is the everyday case — would otherwise hold the connect for the
// kernel's minutes before the next address, usually the IPv4 one, got its
// turn. One address at a time, in the resolver's order: the spirit of
// RFC 8305 without the race.
constexpr int connect_timeout_ms = 5000;

bool connect_with_deadline(int handle, sockaddr const* address, socklen_t length)
{
    int const flags = ::fcntl(handle, F_GETFL, 0);
    if (flags < 0 || ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) < 0)
        return ::connect(handle, address, length) == 0;
    if (::connect(handle, address, length) != 0) {
        if (errno != EINPROGRESS)
            return false;
        pollfd waiter { handle, POLLOUT, 0 };
        if (::poll(&waiter, 1, connect_timeout_ms) <= 0)
            return false;
        int error = 0;
        socklen_t error_size = sizeof error;
        if (::getsockopt(handle, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0 || error != 0)
            return false;
    }
    return ::fcntl(handle, F_SETFL, flags) >= 0;
}

// The host's addresses in the resolver's order, from the process's cache
// or the system resolver; nothing when the name has none.
std::optional<std::vector<ResolvedAddress>> resolve(std::string const& host)
{
    auto const now = HostCache::Clock::now();
    if (std::optional<std::vector<ResolvedAddress>> kept = host_cache().find(host, now))
        return kept;
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    host_cache().count_lookup();
    if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0)
        return std::nullopt;
    std::vector<ResolvedAddress> addresses;
    for (addrinfo* entry = results; entry; entry = entry->ai_next) {
        if ((entry->ai_family != AF_INET && entry->ai_family != AF_INET6) || entry->ai_addrlen > sizeof(sockaddr_storage))
            continue;
        auto const* const bytes = reinterpret_cast<std::uint8_t const*>(entry->ai_addr);
        addresses.push_back({ entry->ai_family, entry->ai_socktype, entry->ai_protocol,
            std::vector<std::uint8_t>(bytes, bytes + entry->ai_addrlen) });
    }
    freeaddrinfo(results);
    if (addresses.empty())
        return std::nullopt;
    host_cache().keep(host, addresses, now);
    return addresses;
}

// The address with the port written into it.
sockaddr_storage with_port(ResolvedAddress const& address, std::uint16_t port, socklen_t& length)
{
    sockaddr_storage target {};
    std::memcpy(&target, address.address.data(), address.address.size());
    length = static_cast<socklen_t>(address.address.size());
    if (address.family == AF_INET)
        reinterpret_cast<sockaddr_in*>(&target)->sin_port = htons(port);
    else
        reinterpret_cast<sockaddr_in6*>(&target)->sin6_port = htons(port);
    return target;
}

} // namespace

std::optional<TcpSocket> TcpSocket::connect(std::string const& host, std::uint16_t port, ConnectTiming* timing)
{
    using clock = std::chrono::steady_clock;
    using ms = std::chrono::duration<double, std::milli>;
    auto const started = clock::now();
    std::optional<std::vector<ResolvedAddress>> const addresses = resolve(host);
    auto const resolved = clock::now();
    if (timing)
        timing->resolve_ms = ms(resolved - started).count();
    if (!addresses)
        return std::nullopt;
    int handle = -1;
    for (ResolvedAddress const& address : *addresses) {
        socklen_t length = 0;
        sockaddr_storage const target = with_port(address, port, length);
        handle = ::socket(address.family, address.socktype, address.protocol);
        if (handle < 0)
            continue;
        if (connect_with_deadline(handle, reinterpret_cast<sockaddr const*>(&target), length))
            break;
        ::close(handle);
        handle = -1;
    }
    if (timing)
        timing->connect_ms = ms(clock::now() - resolved).count();
    if (handle < 0) {
        // None of them took a connection: the next connect asks again.
        host_cache().forget(host);
        return std::nullopt;
    }
    // Every write goes out at once, as curl's and every browser's do: with
    // Nagle's algorithm a small write waits for the peer to acknowledge the
    // last one, so the HTTP/2 preface, the request after it and each TLS
    // record that follows a handshake flight cost a round trip apiece.
    int const no_delay = 1;
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof no_delay);
    return TcpSocket(static_cast<std::uintptr_t>(handle));
}

bool TcpSocket::look_up(std::string const& host)
{
    return resolve(host).has_value();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : m_handle(other.m_handle)
{
    other.m_handle = invalid_handle;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept
{
    if (this != &other) {
        close();
        m_handle = other.m_handle;
        other.m_handle = invalid_handle;
    }
    return *this;
}

TcpSocket::~TcpSocket()
{
    close();
}

void TcpSocket::shutdown()
{
    if (m_handle != invalid_handle)
        ::shutdown(fd_of(m_handle), SHUT_RDWR);
}

void TcpSocket::close()
{
    if (m_handle != invalid_handle) {
        ::close(fd_of(m_handle));
        m_handle = invalid_handle;
    }
}

bool TcpSocket::send_all(std::uint8_t const* data, std::size_t size)
{
    if (m_handle == invalid_handle)
        return false;
    std::size_t sent = 0;
    while (sent < size) {
        ssize_t const result = ::send(fd_of(m_handle), data + sent, size - sent, 0);
        if (result <= 0) {
            if (result < 0 && errno == EINTR)
                continue;
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

std::ptrdiff_t TcpSocket::receive(std::uint8_t* buffer, std::size_t size)
{
    if (m_handle == invalid_handle)
        return -1;
    while (true) {
        ssize_t const result = ::recv(fd_of(m_handle), buffer, size, 0);
        if (result >= 0)
            return result;
        if (errno != EINTR)
            return -1;
    }
}

bool TcpSocket::set_receive_timeout(int milliseconds)
{
    if (m_handle == invalid_handle || milliseconds < 0)
        return false;
    timeval window {};
    window.tv_sec = milliseconds / 1000;
    window.tv_usec = static_cast<suseconds_t>((milliseconds % 1000) * 1000);
    return ::setsockopt(fd_of(m_handle), SOL_SOCKET, SO_RCVTIMEO, &window, sizeof window) == 0;
}

std::optional<TcpListener> TcpListener::listen_loopback(std::uint16_t port)
{
    int const handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle < 0)
        return std::nullopt;
    int enable = 1;
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0
        || ::listen(handle, 64) != 0) {
        ::close(handle);
        return std::nullopt;
    }
    socklen_t length = sizeof address;
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(handle);
        return std::nullopt;
    }
    return TcpListener(static_cast<std::uintptr_t>(handle), ntohs(address.sin_port));
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : m_handle(other.m_handle)
    , m_port(other.m_port)
{
    other.m_handle = invalid_handle;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept
{
    if (this != &other) {
        close();
        m_handle = other.m_handle;
        m_port = other.m_port;
        other.m_handle = invalid_handle;
    }
    return *this;
}

TcpListener::~TcpListener()
{
    close();
}

void TcpListener::close()
{
    if (m_handle != invalid_handle) {
        ::close(fd_of(m_handle));
        m_handle = invalid_handle;
    }
}

std::optional<TcpSocket> TcpListener::accept()
{
    if (m_handle == invalid_handle)
        return std::nullopt;
    while (true) {
        int const client = ::accept(fd_of(m_handle), nullptr, nullptr);
        if (client >= 0)
            return TcpSocket(static_cast<std::uintptr_t>(client));
        if (errno != EINTR)
            return std::nullopt;
    }
}

}
