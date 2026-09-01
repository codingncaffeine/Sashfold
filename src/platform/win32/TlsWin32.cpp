#include "platform/Tls.h"

#include <algorithm>
#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <windows.h>

#include <schannel.h>
#include <security.h>

namespace sashfold::platform {

struct TlsSocket::Impl {
    TcpSocket socket;
    CredHandle credentials {};
    CtxtHandle context {};
    bool credentials_valid = false;
    bool context_valid = false;
    SecPkgContext_StreamSizes sizes {};
    std::vector<std::uint8_t> incoming; // encrypted bytes from the wire
    std::vector<std::uint8_t> plaintext; // decrypted, not yet handed out
    std::size_t plaintext_at = 0;
    bool peer_closed = false;

    explicit Impl(TcpSocket s)
        : socket(std::move(s))
    {
    }

    ~Impl()
    {
        if (context_valid)
            DeleteSecurityContext(&context);
        if (credentials_valid)
            FreeCredentialsHandle(&credentials);
    }

    bool receive_more()
    {
        std::uint8_t buffer[16 * 1024];
        std::ptrdiff_t const received = socket.receive(buffer, sizeof buffer);
        if (received <= 0)
            return false;
        incoming.insert(incoming.end(), buffer, buffer + received);
        return true;
    }

    bool handshake(std::string const& host)
    {
        SCHANNEL_CRED credential_config {};
        credential_config.dwVersion = SCHANNEL_CRED_VERSION;
        credential_config.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS
            | SCH_USE_STRONG_CRYPTO;
        if (AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
                SECPKG_CRED_OUTBOUND, nullptr, &credential_config, nullptr, nullptr, &credentials,
                nullptr)
            != SEC_E_OK)
            return false;
        credentials_valid = true;

        std::wstring wide_host(host.begin(), host.end()); // host is ASCII (punycoded)
        DWORD const request_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT
            | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

        bool first = true;
        while (true) {
            SecBuffer in_buffers[2] {};
            in_buffers[0].BufferType = SECBUFFER_TOKEN;
            in_buffers[0].pvBuffer = incoming.data();
            in_buffers[0].cbBuffer = static_cast<unsigned long>(incoming.size());
            in_buffers[1].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc in_desc { SECBUFFER_VERSION, 2, in_buffers };

            SecBuffer out_buffers[1] {};
            out_buffers[0].BufferType = SECBUFFER_TOKEN;
            SecBufferDesc out_desc { SECBUFFER_VERSION, 1, out_buffers };

            DWORD attributes = 0;
            SECURITY_STATUS const status = InitializeSecurityContextW(&credentials,
                first ? nullptr : &context, wide_host.data(), request_flags, 0, 0,
                first ? nullptr : &in_desc, 0, &context, &out_desc, &attributes, nullptr);
            if (first) {
                context_valid = true;
                first = false;
            }

            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                if (!receive_more())
                    return false;
                continue;
            }

            // A produced token travels regardless of continue/complete.
            if (out_buffers[0].pvBuffer && out_buffers[0].cbBuffer > 0) {
                bool const sent = socket.send_all(
                    static_cast<std::uint8_t const*>(out_buffers[0].pvBuffer),
                    out_buffers[0].cbBuffer);
                FreeContextBuffer(out_buffers[0].pvBuffer);
                if (!sent)
                    return false;
            }

            // Keep any unprocessed input (the start of application data or the
            // next handshake record).
            if (in_buffers[1].BufferType == SECBUFFER_EXTRA && in_buffers[1].cbBuffer > 0) {
                std::size_t const extra = in_buffers[1].cbBuffer;
                incoming.erase(incoming.begin(),
                    incoming.begin() + static_cast<std::ptrdiff_t>(incoming.size() - extra));
            } else if (status != SEC_E_INCOMPLETE_MESSAGE) {
                incoming.clear();
            }

            if (status == SEC_E_OK)
                break;
            if (status == SEC_I_CONTINUE_NEEDED) {
                if (incoming.empty() && !receive_more())
                    return false;
                continue;
            }
            return false; // any validation or negotiation failure
        }

        return QueryContextAttributesW(&context, SECPKG_ATTR_STREAM_SIZES, &sizes) == SEC_E_OK;
    }

    bool send_encrypted(std::uint8_t const* data, std::size_t size)
    {
        while (size > 0) {
            std::size_t const chunk = std::min<std::size_t>(size, sizes.cbMaximumMessage);
            std::vector<std::uint8_t> message(
                static_cast<std::size_t>(sizes.cbHeader) + chunk + sizes.cbTrailer);
            std::memcpy(message.data() + sizes.cbHeader, data, chunk);

            SecBuffer buffers[4] {};
            buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
            buffers[0].pvBuffer = message.data();
            buffers[0].cbBuffer = sizes.cbHeader;
            buffers[1].BufferType = SECBUFFER_DATA;
            buffers[1].pvBuffer = message.data() + sizes.cbHeader;
            buffers[1].cbBuffer = static_cast<unsigned long>(chunk);
            buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
            buffers[2].pvBuffer = message.data() + sizes.cbHeader + chunk;
            buffers[2].cbBuffer = sizes.cbTrailer;
            buffers[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc descriptor { SECBUFFER_VERSION, 4, buffers };

            if (EncryptMessage(&context, 0, &descriptor, 0) != SEC_E_OK)
                return false;
            std::size_t const total = buffers[0].cbBuffer + buffers[1].cbBuffer
                + buffers[2].cbBuffer;
            if (!socket.send_all(message.data(), total))
                return false;
            data += chunk;
            size -= chunk;
        }
        return true;
    }

    // Fills `plaintext` with at least one decrypted byte, or reports close.
    bool decrypt_more()
    {
        while (true) {
            if (incoming.empty() && !receive_more()) {
                peer_closed = true; // transport EOF: treat as close
                return false;
            }
            SecBuffer buffers[4] {};
            buffers[0].BufferType = SECBUFFER_DATA;
            buffers[0].pvBuffer = incoming.data();
            buffers[0].cbBuffer = static_cast<unsigned long>(incoming.size());
            buffers[1].BufferType = SECBUFFER_EMPTY;
            buffers[2].BufferType = SECBUFFER_EMPTY;
            buffers[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc descriptor { SECBUFFER_VERSION, 4, buffers };

            SECURITY_STATUS const status = DecryptMessage(&context, &descriptor, 0, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                if (!receive_more()) {
                    peer_closed = true;
                    return false;
                }
                continue;
            }
            if (status == SEC_I_CONTEXT_EXPIRED) { // close_notify
                peer_closed = true;
                return false;
            }
            if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE)
                return false;

            std::size_t extra = 0;
            for (SecBuffer const& buffer : buffers) {
                if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer > 0) {
                    auto const* begin = static_cast<std::uint8_t const*>(buffer.pvBuffer);
                    plaintext.insert(plaintext.end(), begin, begin + buffer.cbBuffer);
                }
                if (buffer.BufferType == SECBUFFER_EXTRA)
                    extra = buffer.cbBuffer;
            }
            if (extra > 0)
                incoming.erase(incoming.begin(),
                    incoming.begin() + static_cast<std::ptrdiff_t>(incoming.size() - extra));
            else
                incoming.clear();

            if (status == SEC_I_RENEGOTIATE)
                return false; // mid-stream renegotiation: refused for now

            if (!plaintext.empty())
                return true;
            // A record with no app data (e.g. a session ticket): keep going.
        }
    }
};

bool TlsSocket::available()
{
    return true;
}

std::optional<TlsSocket> TlsSocket::connect(TcpSocket socket, std::string const& host)
{
    auto impl = std::make_unique<Impl>(std::move(socket));
    if (!impl->handshake(host))
        return std::nullopt;
    return TlsSocket(std::move(impl));
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
    if (m_impl)
        m_impl->socket.close();
}

bool TlsSocket::send_all(std::uint8_t const* data, std::size_t size)
{
    return m_impl && m_impl->send_encrypted(data, size);
}

std::ptrdiff_t TlsSocket::receive(std::uint8_t* buffer, std::size_t size)
{
    if (!m_impl)
        return -1;
    Impl& impl = *m_impl;
    if (impl.plaintext_at >= impl.plaintext.size()) {
        impl.plaintext.clear();
        impl.plaintext_at = 0;
        if (!impl.decrypt_more())
            return impl.peer_closed ? 0 : -1;
    }
    std::size_t const available_bytes = impl.plaintext.size() - impl.plaintext_at;
    std::size_t const take = std::min(size, available_bytes);
    std::memcpy(buffer, impl.plaintext.data() + impl.plaintext_at, take);
    impl.plaintext_at += take;
    return static_cast<std::ptrdiff_t>(take);
}

}
