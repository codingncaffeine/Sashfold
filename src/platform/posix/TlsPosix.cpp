#include "platform/Tls.h"

// No OS-shipped TLS exists on Linux; our own TLS 1.3 client is not written
// yet. Until it lands this backend reports unavailable and fetch says so
// plainly. (macOS gets Network.framework with its shell.)

namespace sashfold::platform {

struct TlsSocket::Impl { };

bool TlsSocket::available()
{
    return false;
}

std::optional<TlsSocket> TlsSocket::connect(TcpSocket, std::string const&)
{
    return std::nullopt;
}

TlsSocket::TlsSocket(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

TlsSocket::TlsSocket(TlsSocket&&) noexcept = default;
TlsSocket& TlsSocket::operator=(TlsSocket&&) noexcept = default;
TlsSocket::~TlsSocket() = default;

void TlsSocket::close()
{
}

bool TlsSocket::send_all(std::uint8_t const*, std::size_t)
{
    return false;
}

std::ptrdiff_t TlsSocket::receive(std::uint8_t*, std::size_t)
{
    return -1;
}

}
