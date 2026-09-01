#include "platform/Net.h"

#include <algorithm>

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

} // namespace

std::optional<TcpSocket> TcpSocket::connect(std::string const& host, std::uint16_t port)
{
    if (!ensure_winsock())
        return std::nullopt;
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    std::string const port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results) != 0)
        return std::nullopt;
    SOCKET handle = INVALID_SOCKET;
    for (addrinfo* entry = results; entry; entry = entry->ai_next) {
        handle = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (handle == INVALID_SOCKET)
            continue;
        if (::connect(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0)
            break;
        closesocket(handle);
        handle = INVALID_SOCKET;
    }
    freeaddrinfo(results);
    if (handle == INVALID_SOCKET)
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
        closesocket(static_cast<SOCKET>(m_handle));
        m_handle = invalid_handle;
    }
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
        || ::listen(handle, 4) != 0) {
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
