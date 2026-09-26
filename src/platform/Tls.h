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
    // SNI for `host`. nullopt on any handshake or validation failure. The
    // port names, with the host, the server whose session the connection
    // may resume and whose tickets it keeps for the next one. With
    // `offer_h2`, the hello offers ALPN "h2" ahead of "http/1.1"; alpn()
    // then says which the server chose. A backend without ALPN offers
    // neither and reports nothing chosen, which reads as HTTP/1.1.
    static std::optional<TlsSocket> connect(TcpSocket socket, std::string const& host, std::uint16_t port = 443,
        bool offer_h2 = false);

    TlsSocket(TlsSocket&&) noexcept;
    TlsSocket& operator=(TlsSocket&&) noexcept;
    TlsSocket(TlsSocket const&) = delete;
    TlsSocket& operator=(TlsSocket const&) = delete;
    ~TlsSocket();

    // One thread may receive while another sends: an HTTP/2 connection is
    // read by its own thread and written by the fetches it carries. Two
    // senders, or two receivers, must take turns.
    bool send_all(std::uint8_t const* data, std::size_t size);
    // >0 plaintext bytes, 0 orderly close, <0 error.
    std::ptrdiff_t receive(std::uint8_t* buffer, std::size_t size);
    // As TcpSocket::set_receive_timeout, on the socket underneath.
    bool set_receive_timeout(int milliseconds);
    // The application protocol the server chose, or empty.
    std::string alpn() const;
    // As TcpSocket::shutdown: wakes a receive blocked on another thread.
    void shutdown();
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
