#pragma once

// The platform networking seam: TCP + name resolution through the
// OS interface — Winsock on Windows, BSD sockets elsewhere. Synchronous for
// the fetch pipeline; the event-loop integration comes later.
// The loopback listener exists for hermetic tests.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace sashfold::platform {

class TcpSocket {
public:
    // Connects to a DNS name or address literal (getaddrinfo — an OS
    // interface — does the resolving).
    static std::optional<TcpSocket> connect(std::string const& host, std::uint16_t port);

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(TcpSocket const&) = delete;
    TcpSocket& operator=(TcpSocket const&) = delete;
    ~TcpSocket();

    bool send_all(std::uint8_t const* data, std::size_t size);
    // >0 bytes received, 0 orderly close, <0 error.
    std::ptrdiff_t receive(std::uint8_t* buffer, std::size_t size);
    void close();

private:
    friend class TcpListener;
    explicit TcpSocket(std::uintptr_t handle)
        : m_handle(handle)
    {
    }
    std::uintptr_t m_handle;
};

class TcpListener {
public:
    // Binds 127.0.0.1 on the given port (0 = ephemeral).
    static std::optional<TcpListener> listen_loopback(std::uint16_t port = 0);

    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    TcpListener(TcpListener const&) = delete;
    TcpListener& operator=(TcpListener const&) = delete;
    ~TcpListener();

    std::uint16_t port() const { return m_port; }
    std::optional<TcpSocket> accept();
    void close();

private:
    TcpListener(std::uintptr_t handle, std::uint16_t port)
        : m_handle(handle)
        , m_port(port)
    {
    }
    std::uintptr_t m_handle;
    std::uint16_t m_port;
};

}
