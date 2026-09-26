#include "platform/Tls.h"

#include "crypto/X25519.h"
#include "net/tls/TrustStore.h"
#include "net/tls/Tls13.h"
#include "net/tls/Validate.h"
#include "platform/Random.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The Linux TLS backend: our own TLS 1.3 and 1.2 client (src/net/tls)
// driven over a TcpSocket. Certificate validation runs against the system
// trust store (net/tls/Validate), with the leaf's revocation list fetched
// over plain HTTP through the fetch the net layer installs and held in a
// per-process cache; SASHFOLD_TLS_INSECURE=1 accepts any chain and says so
// loudly, SASHFOLD_TLS_SUITE narrows the hello to one suite and
// SASHFOLD_TLS_VERSION to one version, to drive a path end to end against a
// real server. A second connection to a server resumes the first: the 1.3
// tickets and the 1.2 session each server gave are kept per host and port
// for the process's lifetime, and SASHFOLD_TLS_RESUME=0 stops them being
// offered, so a run with and one without can be compared. macOS gets
// Network.framework with its shell.

namespace sashfold::platform {

namespace {

bool insecure_requested()
{
    char const* const value = std::getenv("SASHFOLD_TLS_INSECURE");
    return value != nullptr && value[0] == '1';
}

bool resumption_requested()
{
    char const* const value = std::getenv("SASHFOLD_TLS_RESUME");
    return value == nullptr || value[0] != '0';
}

// What the process may resume by, per server, behind one lock. A 1.3
// ticket is offered once and then gone (RFC 8446 §C.4), the newest first,
// never past its lifetime, at most a few kept per server; a 1.2 session
// serves every connection until a full handshake replaces it or a day has
// passed. The lock is taken to read and to keep, never across a
// handshake, so the engine's callbacks may take it again.
class SessionStore {
public:
    static SessionStore& instance()
    {
        static SessionStore store;
        return store;
    }

    std::optional<tls::Ticket> take_ticket(std::string const& key, std::uint32_t& age_ms)
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        auto const found = m_tickets.find(key);
        if (found == m_tickets.end())
            return std::nullopt;
        std::vector<KeptTicket>& kept = found->second;
        auto const now = std::chrono::steady_clock::now();
        while (!kept.empty()) {
            KeptTicket newest = std::move(kept.back());
            kept.pop_back();
            auto const age = std::chrono::duration_cast<std::chrono::milliseconds>(now - newest.received).count();
            if (age < 0 || age >= static_cast<long long>(newest.ticket.lifetime_seconds) * 1000)
                continue;
            age_ms = static_cast<std::uint32_t>(age);
            return std::move(newest.ticket);
        }
        m_tickets.erase(found);
        return std::nullopt;
    }

    void keep_ticket(std::string const& key, tls::Ticket ticket)
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        std::vector<KeptTicket>& kept = m_tickets[key];
        kept.push_back(KeptTicket { std::move(ticket), std::chrono::steady_clock::now() });
        if (kept.size() > max_tickets_per_server)
            kept.erase(kept.begin());
    }

    std::optional<tls::Session12> session_for(std::string const& key)
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        auto const found = m_sessions.find(key);
        if (found == m_sessions.end())
            return std::nullopt;
        auto const now = std::chrono::steady_clock::now();
        auto const age = std::chrono::duration_cast<std::chrono::seconds>(now - found->second.received).count();
        long long lifetime = max_session_seconds;
        if (found->second.session.lifetime_hint_seconds != 0)
            lifetime = std::min<long long>(lifetime, found->second.session.lifetime_hint_seconds);
        if (age < 0 || age >= lifetime) {
            m_sessions.erase(found);
            return std::nullopt;
        }
        return found->second.session;
    }

    void keep_session(std::string const& key, tls::Session12 session)
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        m_sessions[key] = KeptSession { std::move(session), std::chrono::steady_clock::now() };
    }

private:
    static constexpr std::size_t max_tickets_per_server = 4;
    static constexpr long long max_session_seconds = 24 * 60 * 60;

    struct KeptTicket {
        tls::Ticket ticket;
        std::chrono::steady_clock::time_point received;
    };
    struct KeptSession {
        tls::Session12 session;
        std::chrono::steady_clock::time_point received;
    };
    std::mutex m_mutex;
    std::unordered_map<std::string, std::vector<KeptTicket>> m_tickets;
    std::unordered_map<std::string, KeptSession> m_sessions;
};

// The process's revocation lists, behind one lock: the fetch pipeline is
// synchronous, but a second thread validating a chain must not race the
// first through the cache.
tls::HttpFetch revocation_fetcher(std::int64_t now)
{
    RevocationFetch const& fetch = revocation_fetch();
    if (!fetch)
        return nullptr;
    return [now](std::string const& url) {
        static std::mutex mutex;
        static tls::CrlCache cache([](std::string const& at) { return revocation_fetch()(at); });
        std::lock_guard<std::mutex> const lock(mutex);
        return cache.get(url, now);
    };
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

// Once connected, the engine is shared by the thread that receives and the
// one that sends: `engine_mutex` covers every use of it, and the socket
// writes that go with it, so records leave in the order they were sealed.
// The receive itself waits outside the lock. The plaintext buffer and
// `failed` are the receiving thread's alone.
struct TlsSocket::Impl {
    TcpSocket socket;
    std::mutex engine_mutex;
    tls::TlsEngine engine;
    std::vector<std::uint8_t> plaintext; // decrypted, not yet handed out
    std::size_t plaintext_at = 0;
    std::atomic<bool> closed = false;
    bool failed = false;

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
        std::lock_guard<std::mutex> const lock(engine_mutex);
        tls::TlsOutput out;
        bool const ok = engine.feed(std::span<std::uint8_t const>(buffer, static_cast<std::size_t>(received)), out);
        if (!out.to_send.empty())
            socket.send_all(out.to_send.data(), out.to_send.size());
        if (!out.plaintext.empty())
            plaintext.insert(plaintext.end(), out.plaintext.begin(), out.plaintext.end());
        if (!ok || engine.state() == tls::TlsState::Closed)
            closed = true;
        failed = engine.state() == tls::TlsState::Failed;
        return !out.plaintext.empty();
    }
};

bool TlsSocket::available()
{
    return true;
}

std::optional<TlsSocket> TlsSocket::connect(TcpSocket socket, std::string const& host, std::uint16_t port, bool offer_h2)
{
    tls::TlsConfig config;
    if (offer_h2)
        config.alpn = { "h2", "http/1.1" };
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

    // The server's tickets and session, kept for the next connection and
    // offered on this one: a ticket that a suite request cannot use (its
    // key schedule is the suite's hash, which every suite here shares) is
    // not a concern, and a server that declines gets a full handshake.
    std::string const key = host + ":" + std::to_string(port);
    SessionStore& sessions = SessionStore::instance();
    if (resumption_requested()) {
        std::uint32_t age_ms = 0;
        if (config.offer_tls13)
            if (std::optional<tls::Ticket> ticket = sessions.take_ticket(key, age_ms)) {
                config.resume_ticket = std::move(ticket);
                config.ticket_age_ms = age_ms;
            }
        if (config.offer_tls12)
            config.resume_session = sessions.session_for(key);
    }
    config.on_ticket = [key](tls::Ticket ticket) { SessionStore::instance().keep_ticket(key, std::move(ticket)); };
    config.on_session = [key](tls::Session12 session) { SessionStore::instance().keep_session(key, std::move(session)); };

    bool const insecure = insecure_requested();
    std::string const host_copy = host;
    config.verify_chain = [insecure, host_copy](std::vector<tls::Certificate> const& chain, std::string& reason) {
        if (insecure) {
            std::fprintf(stderr, "sashfold: SASHFOLD_TLS_INSECURE is set — the server's certificate for %s is NOT being verified\n", host_copy.c_str());
            return true;
        }
        static tls::TrustStore const store = tls::TrustStore::load();
        std::int64_t const now = static_cast<std::int64_t>(std::time(nullptr));
        tls::Verdict const verdict = tls::validate_chain(chain, host_copy, now, store, revocation_fetcher(now));
        if (!verdict.trusted) {
            reason = "certificate validation failed: " + verdict.reason;
            return false;
        }
        return true;
    };

    auto impl = std::make_unique<Impl>(std::move(socket), std::move(config));
    if (!impl->handshake())
        return std::nullopt;
    // A resumed connection rests on a chain the flag waved through: say so
    // as loudly as the verification would have.
    if (insecure && impl->engine.resumed())
        std::fprintf(stderr, "sashfold: SASHFOLD_TLS_INSECURE is set — the resumed connection to %s rests on a chain that was NOT verified\n", host_copy.c_str());
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
        std::lock_guard<std::mutex> const lock(m_impl->engine_mutex);
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
    std::lock_guard<std::mutex> const lock(m_impl->engine_mutex);
    std::vector<std::uint8_t> const records = m_impl->engine.seal(std::span<std::uint8_t const>(data, size));
    return m_impl->send_records(records);
}

bool TlsSocket::set_receive_timeout(int milliseconds)
{
    return m_impl && m_impl->socket.set_receive_timeout(milliseconds);
}

std::string TlsSocket::alpn() const
{
    if (!m_impl)
        return {};
    std::lock_guard<std::mutex> const lock(m_impl->engine_mutex);
    return m_impl->engine.alpn();
}

void TlsSocket::shutdown()
{
    if (m_impl)
        m_impl->socket.shutdown();
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
        if (m_impl->plaintext.empty() && m_impl->failed)
            return -1;
    }
    std::size_t const available = m_impl->plaintext.size() - m_impl->plaintext_at;
    std::size_t const take = size < available ? size : available;
    std::memcpy(buffer, m_impl->plaintext.data() + m_impl->plaintext_at, take);
    m_impl->plaintext_at += take;
    return static_cast<std::ptrdiff_t>(take);
}

}
