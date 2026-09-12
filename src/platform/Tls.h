#pragma once

// The TLS seam: on Windows and macOS the OS interface carries TLS (SChannel
// / Network.framework — shipped with, patched by, and trusted like the OS);
// on Linux the client is ours (net/tls), over the OS's own trust store.
// Certificate validation happens inside the platform implementation; a
// failed handshake is indistinguishable from a refused connection on
// purpose — the caller shows the cert-error page policy when the shell
// lands.

#include "platform/Net.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    // As TcpSocket::set_receive_timeout, on the socket underneath.
    bool set_receive_timeout(int milliseconds);
    void close();

private:
    struct Impl;
    explicit TlsSocket(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

// Revocation lists travel over plain HTTP, and the platform layer has no
// HTTP client of its own: the net layer installs one here, and the TLS
// backend that validates chains itself (Linux) asks it for a CRL by URL.
// The fetch returns the list's bytes, or nothing when the point could not
// be reached — which the validator treats as a soft pass, as the OS
// validators do. With nothing installed, revocation is not consulted.
using RevocationFetch = std::function<std::vector<std::uint8_t>(std::string const& url)>;
void set_revocation_fetch(RevocationFetch fetch);
RevocationFetch const& revocation_fetch();

}
