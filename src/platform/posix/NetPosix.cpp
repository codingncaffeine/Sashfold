#include "platform/Net.h"

#include <algorithm>
#include <cerrno>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
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

} // namespace

std::optional<TcpSocket> TcpSocket::connect(std::string const& host, std::uint16_t port)
{
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    std::string const port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results) != 0)
        return std::nullopt;
    int handle = -1;
    for (addrinfo* entry = results; entry; entry = entry->ai_next) {
        handle = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (handle < 0)
            continue;
        if (connect_with_deadline(handle, entry->ai_addr, entry->ai_addrlen))
            break;
        ::close(handle);
        handle = -1;
    }
    freeaddrinfo(results);
    if (handle < 0)
        return std::nullopt;
    return TcpSocket(static_cast<std::uintptr_t>(handle));
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
        || ::listen(handle, 4) != 0) {
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
