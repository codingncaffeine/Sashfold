#pragma once

// The TLS seam: on Windows and macOS the OS interface carries TLS (SChannel
// / Network.framework — shipped with, patched by, and trusted like the OS);
// on Linux the TLS 1.3 client will be ours, and is not written yet.
// Certificate validation happens against the OS trust store inside the
// platform implementation; a failed handshake is indistinguishable from a
// refused connection on purpose — the caller shows the cert-error page
// policy when the shell lands.

#include "platform/Net.h"

#include <memory>
#include <optional>
#include <string>

namespace sashfold::platform {

class TlsSocket {
public:
    // True when this build carries a TLS backend.
    static bool available();

    // Takes ownership of the connected socket and runs the handshake with
    // SNI for `host`. nullopt on any handshake or validation failure.
    static std::optional<TlsSocket> connect(TcpSocket socket, std::string const& host);

    TlsSocket(TlsSocket&&) noexcept;
    TlsSocket& operator=(TlsSocket&&) noexcept;
    TlsSocket(TlsSocket const&) = delete;
    TlsSocket& operator=(TlsSocket const&) = delete;
    ~TlsSocket();

    bool send_all(std::uint8_t const* data, std::size_t size);
    // >0 plaintext bytes, 0 orderly close, <0 error.
    std::ptrdiff_t receive(std::uint8_t* buffer, std::size_t size);
    void close();

private:
    struct Impl;
    explicit TlsSocket(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

}
