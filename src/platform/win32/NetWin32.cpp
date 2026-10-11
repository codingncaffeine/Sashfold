#include "platform/Net.h"

#include "platform/HostCache.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

namespace sashfold::platform {

namespace {

constexpr std::uintptr_t invalid_handle = static_cast<std::uintptr_t>(INVALID_SOCKET);

bool ensure_winsock()
{
    static bool const initialized = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}

// The host's addresses in the resolver's order, from the process's cache
// or the system resolver (see NetPosix.cpp); nothing when it has none.
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
        if ((entry->ai_family != AF_INET && entry->ai_family != AF_INET6) || entry->ai_addrlen > sizeof(SOCKADDR_STORAGE))
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
SOCKADDR_STORAGE with_port(ResolvedAddress const& address, std::uint16_t port, int& length)
{
    SOCKADDR_STORAGE target {};
    std::memcpy(&target, address.address.data(), address.address.size());
    length = static_cast<int>(address.address.size());
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
    if (!ensure_winsock())
        return std::nullopt;
    auto const started = clock::now();
    std::optional<std::vector<ResolvedAddress>> const addresses = resolve(host);
    auto const resolved = clock::now();
    if (timing)
        timing->resolve_ms = ms(resolved - started).count();
    if (!addresses)
        return std::nullopt;
    SOCKET handle = INVALID_SOCKET;
    for (ResolvedAddress const& address : *addresses) {
        int length = 0;
        SOCKADDR_STORAGE const target = with_port(address, port, length);
        handle = ::socket(address.family, address.socktype, address.protocol);
        if (handle == INVALID_SOCKET)
            continue;
        if (::connect(handle, reinterpret_cast<sockaddr const*>(&target), length) == 0)
            break;
        closesocket(handle);
        handle = INVALID_SOCKET;
    }
    if (timing)
        timing->connect_ms = ms(clock::now() - resolved).count();
    if (handle == INVALID_SOCKET) {
        // None of them took a connection: the next connect asks again.
        host_cache().forget(host);
        return std::nullopt;
    }
    // Every write goes out at once (see NetPosix.cpp): Nagle's algorithm
    // would hold each small write for the last one's acknowledgement.
    BOOL const no_delay = TRUE;
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&no_delay), sizeof no_delay);
    return TcpSocket(static_cast<std::uintptr_t>(handle));
}

bool TcpSocket::look_up(std::string const& host)
{
    if (!ensure_winsock())
        return false;
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
        ::shutdown(static_cast<SOCKET>(m_handle), SD_BOTH);
}

void TcpSocket::close()
{
    if (m_handle != invalid_handle) {
        closesocket(static_cast<SOCKET>(m_handle));
        m_handle = invalid_handle;
    }
}

bool TcpSocket::set_receive_timeout(int milliseconds)
{
    if (m_handle == invalid_handle || milliseconds < 0)
        return false;
    DWORD const window = static_cast<DWORD>(milliseconds);
    return ::setsockopt(static_cast<SOCKET>(m_handle), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char const*>(&window), sizeof window)
        == 0;
}

bool TcpSocket::send_all(std::uint8_t const* data, std::size_t size)
{
    if (m_handle == invalid_handle)
        return false;
    std::size_t sent = 0;
    while (sent < size) {
        int const chunk = static_cast<int>(
            std::min<std::size_t>(size - sent, static_cast<std::size_t>(1) << 20));
        int const result = ::send(static_cast<SOCKET>(m_handle),
            reinterpret_cast<char const*>(data + sent), chunk, 0);
        if (result <= 0)
            return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

std::ptrdiff_t TcpSocket::receive(std::uint8_t* buffer, std::size_t size)
{
    if (m_handle == invalid_handle)
        return -1;
    int const result = ::recv(static_cast<SOCKET>(m_handle), reinterpret_cast<char*>(buffer),
        static_cast<int>(std::min<std::size_t>(size, static_cast<std::size_t>(1) << 20)), 0);
    if (result > 0)
        return result;
    if (result == 0)
        return 0;
    return -1;
}

std::optional<TcpListener> TcpListener::listen_loopback(std::uint16_t port)
{
    if (!ensure_winsock())
        return std::nullopt;
    SOCKET const handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET)
        return std::nullopt;
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0
        || ::listen(handle, 64) != 0) {
        closesocket(handle);
        return std::nullopt;
    }
    int length = sizeof address;
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        closesocket(handle);
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
        closesocket(static_cast<SOCKET>(m_handle));
        m_handle = invalid_handle;
    }
}

std::optional<TcpSocket> TcpListener::accept()
{
    if (m_handle == invalid_handle)
        return std::nullopt;
    SOCKET const client = ::accept(static_cast<SOCKET>(m_handle), nullptr, nullptr);
    if (client == INVALID_SOCKET)
        return std::nullopt;
    return TcpSocket(static_cast<std::uintptr_t>(client));
}

}
