#include "platform/Net.h"

#include <algorithm>
#include <cerrno>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sashfold::platform {

namespace {

constexpr std::uintptr_t invalid_handle = static_cast<std::uintptr_t>(-1);

int fd_of(std::uintptr_t handle)
{
    return static_cast<int>(handle);
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
        if (::connect(handle, entry->ai_addr, entry->ai_addrlen) == 0)
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
