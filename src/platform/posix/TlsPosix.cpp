#include "platform/Tls.h"

#include "crypto/X25519.h"
#include "net/tls/TrustStore.h"
#include "net/tls/Tls13.h"
#include "net/tls/Validate.h"
#include "platform/Random.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// The Linux TLS backend: our own TLS 1.3 and 1.2 client (src/net/tls)
// driven over a TcpSocket. Certificate validation runs against the system
// trust store (net/tls/Validate); SASHFOLD_TLS_INSECURE=1 accepts any chain
// and says so loudly, SASHFOLD_TLS_SUITE narrows the hello to one suite and
// SASHFOLD_TLS_VERSION to one version, to drive a path end to end against a
// real server. macOS gets Network.framework with its shell.

namespace sashfold::platform {

namespace {

bool insecure_requested()
{
    char const* const value = std::getenv("SASHFOLD_TLS_INSECURE");
    return value != nullptr && value[0] == '1';
}

// A suite named in the environment narrows what the hello offers to that one
// alone, so either record protection can be driven end to end against a real
// server rather than only in a test.
void apply_suite_request(tls::TlsConfig& config)
{
    char const* const value = std::getenv("SASHFOLD_TLS_SUITE");
    if (value == nullptr || value[0] == '\0')
        return;
    std::string const name(value);
    if (name == "aes128gcm" || name == "TLS_AES_128_GCM_SHA256")
        config.cipher_suites = { tls::CipherSuite::Aes128GcmSha256 };
    else if (name == "chacha20" || name == "TLS_CHACHA20_POLY1305_SHA256")
        config.cipher_suites = { tls::CipherSuite::ChaCha20Poly1305Sha256 };
    else if (name == "ecdhe-rsa-aes128gcm" || name == "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256")
        config.cipher_suites = { tls::CipherSuite::EcdheRsaAes128GcmSha256 };
    else if (name == "ecdhe-ecdsa-aes128gcm" || name == "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256")
        config.cipher_suites = { tls::CipherSuite::EcdheEcdsaAes128GcmSha256 };
    else if (name == "ecdhe-rsa-chacha20" || name == "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256")
        config.cipher_suites = { tls::CipherSuite::EcdheRsaChaCha20Poly1305Sha256 };
    else if (name == "ecdhe-ecdsa-chacha20" || name == "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256")
        config.cipher_suites = { tls::CipherSuite::EcdheEcdsaChaCha20Poly1305Sha256 };
    else {
        std::fprintf(stderr, "sashfold: SASHFOLD_TLS_SUITE names no suite this client has (%s)\n", value);
        return;
    }
    std::fprintf(stderr, "sashfold: offering only %s\n", tls::cipher_suite_name(config.cipher_suites.front()));
}

// A version named in the environment narrows the hello to it, so the 1.2
// path can be driven against a server that would rather speak 1.3.
void apply_version_request(tls::TlsConfig& config)
{
    char const* const value = std::getenv("SASHFOLD_TLS_VERSION");
    if (value == nullptr || value[0] == '\0')
        return;
    std::string const name(value);
    if (name == "1.2")
        config.offer_tls13 = false;
    else if (name == "1.3")
        config.offer_tls12 = false;
    else {
        std::fprintf(stderr, "sashfold: SASHFOLD_TLS_VERSION names no version this client has (%s)\n", value);
        return;
    }
    std::fprintf(stderr, "sashfold: offering only TLS %s\n", name.c_str());
}

}

struct TlsSocket::Impl {
    TcpSocket socket;
    tls::TlsEngine engine;
    std::vector<std::uint8_t> plaintext; // decrypted, not yet handed out
    std::size_t plaintext_at = 0;
    bool closed = false;

    Impl(TcpSocket s, tls::TlsConfig config)
        : socket(std::move(s))
        , engine(std::move(config))
    {
    }

    bool send_records(std::vector<std::uint8_t> const& records)
    {
        return records.empty() || socket.send_all(records.data(), records.size());
    }

    // Runs the handshake to completion: send our flights, read the peer's,
    // until the engine is connected or has failed.
    bool handshake()
    {
        if (!send_records(engine.start()))
            return false;
        std::uint8_t buffer[16 * 1024];
        while (!engine.connected()) {
            std::ptrdiff_t const received = socket.receive(buffer, sizeof buffer);
            if (received <= 0)
                return false;
            tls::TlsOutput out;
            bool const ok = engine.feed(std::span<std::uint8_t const>(buffer, static_cast<std::size_t>(received)), out);
            if (!send_records(out.to_send))
                return false;
            if (!out.plaintext.empty())
                plaintext.insert(plaintext.end(), out.plaintext.begin(), out.plaintext.end());
            if (!ok)
                return false;
        }
        return true;
    }

    bool pull()
    {
        std::uint8_t buffer[16 * 1024];
        std::ptrdiff_t const received = socket.receive(buffer, sizeof buffer);
        if (received <= 0) {
            closed = true;
            return false;
        }
        tls::TlsOutput out;
        bool const ok = engine.feed(std::span<std::uint8_t const>(buffer, static_cast<std::size_t>(received)), out);
        if (!out.to_send.empty())
            socket.send_all(out.to_send.data(), out.to_send.size());
        if (!out.plaintext.empty())
            plaintext.insert(plaintext.end(), out.plaintext.begin(), out.plaintext.end());
        if (!ok || engine.state() == tls::TlsState::Closed)
            closed = true;
        return !out.plaintext.empty();
    }
};

bool TlsSocket::available()
{
    return true;
}

std::optional<TlsSocket> TlsSocket::connect(TcpSocket socket, std::string const& host)
{
    tls::TlsConfig config;
    // An IP literal sends no SNI (RFC 6066); a name does.
    bool const is_ip = !host.empty() && (host.find_first_not_of("0123456789.") == std::string::npos || host.find(':') != std::string::npos);
    if (!is_ip)
        config.server_name = host;
    fill_random(config.private_key);
    fill_random(config.p256_private_key);
    fill_random(config.client_random);
    fill_random(config.session_id);
    apply_suite_request(config);
    apply_version_request(config);

    bool const insecure = insecure_requested();
    std::string const host_copy = host;
    config.verify_chain = [insecure, host_copy](std::vector<tls::Certificate> const& chain, std::string& reason) {
        if (insecure) {
            std::fprintf(stderr, "sashfold: SASHFOLD_TLS_INSECURE is set — the server's certificate for %s is NOT being verified\n", host_copy.c_str());
            return true;
        }
        static tls::TrustStore const store = tls::TrustStore::load();
        std::int64_t const now = static_cast<std::int64_t>(std::time(nullptr));
        tls::Verdict const verdict = tls::validate_chain(chain, host_copy, now, store, nullptr);
        if (!verdict.trusted) {
            reason = "certificate validation failed: " + verdict.reason;
            return false;
        }
        return true;
    };

    auto impl = std::make_unique<Impl>(std::move(socket), std::move(config));
    if (!impl->handshake())
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
    if (m_impl && !m_impl->closed) {
        std::vector<std::uint8_t> const alert = m_impl->engine.close_notify();
        if (!alert.empty())
            m_impl->socket.send_all(alert.data(), alert.size());
        m_impl->socket.close();
        m_impl->closed = true;
    }
}

bool TlsSocket::send_all(std::uint8_t const* data, std::size_t size)
{
    if (!m_impl)
        return false;
    std::vector<std::uint8_t> const records = m_impl->engine.seal(std::span<std::uint8_t const>(data, size));
    return m_impl->send_records(records);
}

std::ptrdiff_t TlsSocket::receive(std::uint8_t* buffer, std::size_t size)
{
    if (!m_impl)
        return -1;
    while (m_impl->plaintext_at >= m_impl->plaintext.size()) {
        m_impl->plaintext.clear();
        m_impl->plaintext_at = 0;
        if (m_impl->closed)
            return 0;
        m_impl->pull();
        if (m_impl->plaintext.empty() && m_impl->closed)
            return 0;
        if (m_impl->plaintext.empty() && m_impl->engine.state() == tls::TlsState::Failed)
            return -1;
    }
    std::size_t const available = m_impl->plaintext.size() - m_impl->plaintext_at;
    std::size_t const take = size < available ? size : available;
    std::memcpy(buffer, m_impl->plaintext.data() + m_impl->plaintext_at, take);
    m_impl->plaintext_at += take;
    return static_cast<std::ptrdiff_t>(take);
}

}
