#pragma once
// The TLS client as a state machine over bytes: TLS 1.3 (RFC 8446), and
// TLS 1.2 (RFC 5246) for a server that has nothing newer, negotiated in the
// same hello. The caller moves bytes between it and a socket and hands it
// what it needs from the world — the ephemeral keys, the client random, the
// verdict on the server's chain — so a transcript with a known key
// reproduces byte for byte. In 1.3: two cipher suites,
// TLS_CHACHA20_POLY1305_SHA256 and TLS_AES_128_GCM_SHA256; the groups
// x25519 and secp256r1; resumption by the tickets a server issues (§4.6.1,
// §4.2.11: a pre-shared key with its binder, always with a fresh key
// exchange beside it); no early data, no client certificates (an empty one
// answers a request). A server that will not take the key share offered
// asks for another hello (§4.1.4). In 1.2: the ECDHE suites with the same
// two AEADs under RSA or ECDSA authentication, over the same two groups,
// the extended master secret when the server takes it, resumption by
// session ticket (RFC 5077) or by session id (RFC 5246 §7.4.1.2), and never
// renegotiation. The engine checks the server's signature over the
// handshake and the Finished MAC itself; whether the chain is trusted is
// the verdict of the callback the caller installs (the validator's, or the
// insecure flag's). A resumed connection rests on the chain the caller
// accepted when the ticket was issued, which the ticket carries.
#include "crypto/X25519.h"
#include "net/tls/X509.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sashfold::tls {

enum class TlsState : std::uint8_t {
    Start,
    WaitServerHello,
    WaitEncryptedExtensions,
    WaitCertificate, // or a CertificateRequest first
    WaitCertificateVerify,
    WaitFinished,
    Connected,
    Closed, // close_notify received or sent
    Failed,
    // TLS 1.2 only: after the Certificate, the server's ephemeral key with
    // its signature over it, then the end of the server's flight.
    WaitServerKeyExchange,
    WaitServerHelloDone,
};

// The suites a record can be protected with: the TLS 1.3 ones (§B.4) and
// the TLS 1.2 ECDHE ones over the same two AEADs (RFC 5289, RFC 7905).
// Every one keys its schedule with SHA-256, so the transcript hash does
// not turn on the choice.
enum class CipherSuite : std::uint16_t {
    Aes128GcmSha256 = 0x1301,
    ChaCha20Poly1305Sha256 = 0x1303,
    EcdheEcdsaAes128GcmSha256 = 0xc02b,
    EcdheRsaAes128GcmSha256 = 0xc02f,
    EcdheRsaChaCha20Poly1305Sha256 = 0xcca8,
    EcdheEcdsaChaCha20Poly1305Sha256 = 0xcca9,
};

char const* cipher_suite_name(CipherSuite suite);

// A TLS 1.3 session ticket (§4.6.1) as the client keeps it: the ticket
// itself, the pre-shared key derived from it, what makes its obfuscated
// age, how long it is good for, and the connection it came from — the
// chain the caller accepted then, which is what a resumed connection
// rests on. A ticket is used once (§C.4).
struct Ticket {
    std::vector<std::uint8_t> ticket;
    std::vector<std::uint8_t> psk;
    std::uint32_t age_add = 0;
    std::uint32_t lifetime_seconds = 0; // never past seven days
    CipherSuite suite = CipherSuite::ChaCha20Poly1305Sha256;
    std::string server_name;
    std::vector<Certificate> chain;
};

// A TLS 1.2 session as the client keeps it: resumed by the ticket (RFC
// 5077) when the server issued one, else by the session id the server
// chose; the master secret, the suite and whether the extended master
// secret bound it (RFC 7627 §5.3), which a resumption must keep.
struct Session12 {
    std::vector<std::uint8_t> session_id;
    std::vector<std::uint8_t> ticket;
    std::vector<std::uint8_t> master_secret;
    std::uint32_t lifetime_hint_seconds = 0;
    CipherSuite suite = CipherSuite::EcdheRsaAes128GcmSha256;
    bool extended_master_secret = false;
    std::string server_name;
    std::vector<Certificate> chain;
};

// The fields of a NewSessionTicket message (§4.6.1), as read off the wire.
struct NewSessionTicket {
    std::uint32_t lifetime_seconds = 0;
    std::uint32_t age_add = 0;
    std::vector<std::uint8_t> nonce;
    std::vector<std::uint8_t> ticket;
    std::uint32_t max_early_data = 0; // the early_data extension, or 0
};
std::optional<NewSessionTicket> parse_new_session_ticket(std::span<std::uint8_t const> body);

// The pieces of the key schedule a ticket turns on, on their own so the
// RFC 8448 traces can check them: the pre-shared key of a ticket (§4.6.1,
// HKDF-Expand-Label of the resumption master secret with the ticket's
// nonce), and the binder over a partial ClientHello (§4.2.11.2: the
// Finished construction under the binder key, over the hash of the
// transcript through the hello truncated before its binders).
std::vector<std::uint8_t> resumption_psk(std::span<std::uint8_t const> resumption_master_secret,
    std::span<std::uint8_t const> ticket_nonce);
std::vector<std::uint8_t> psk_binder(std::span<std::uint8_t const> psk, std::span<std::uint8_t const> truncated_transcript_hash);

struct TlsConfig {
    std::string server_name; // the SNI host; empty for an IP literal, which sends none
    crypto::X25519Key private_key {}; // the ephemeral scalar: platform::fill_random, or a test's
    // The ephemeral scalar for secp256r1, used when the server names that
    // group: in a 1.3 retry request, or in a 1.2 key exchange.
    std::array<std::uint8_t, 32> p256_private_key {};
    std::array<std::uint8_t, 32> client_random {};
    // Which versions the hello offers. With both, a server takes 1.3 when
    // it can; a hello for 1.2 alone is how a test or a probe reaches the
    // 1.2 path of a server that would rather speak 1.3.
    bool offer_tls13 = true;
    bool offer_tls12 = true;
    // RFC 8446 §D.4 compatibility mode: a 32-byte legacy_session_id and a
    // ChangeCipherSpec before the second flight, so middleboxes see a
    // TLS 1.2 shape. Off for the RFC 8448 transcript, on for the world.
    bool compatibility_mode = true;
    std::array<std::uint8_t, 32> session_id {};
    // The chain (leaf first) once it has arrived and the server has proven
    // it holds the leaf's key; false fails the handshake with an alert and
    // the reason lands in error().
    std::function<bool(std::vector<Certificate> const& chain, std::string& reason)> verify_chain;
    // The suites to offer, in the order the server reads them; empty means
    // both, ChaCha20-Poly1305 first — the AES beside it is software, so on a
    // machine with no instructions for AES it is the slower of the two.
    std::vector<CipherSuite> cipher_suites;
    // §4.2.8: a ClientHello may carry no key share at all, which asks the
    // server to name the group it wants in a HelloRetryRequest. It costs a
    // round trip; with one group implemented it is also the only hello a
    // server can legally ask us to retry.
    bool empty_key_share = false;
    // Resumption. A 1.3 ticket to offer, with its age in milliseconds at
    // the moment of the hello (§4.2.11.1), and a 1.2 session to offer;
    // either is offered only when the hello offers its version, and a
    // server that declines gets a full handshake. What this connection
    // is given in turn comes out through the callbacks: every 1.3 ticket
    // as it arrives, the 1.2 session once the handshake is done, when the
    // server gave anything to resume by.
    std::optional<Ticket> resume_ticket;
    std::uint32_t ticket_age_ms = 0;
    std::optional<Session12> resume_session;
    std::function<void(Ticket)> on_ticket;
    std::function<void(Session12)> on_session;
};

// What one call produced: bytes for the socket and plaintext for the caller.
struct TlsOutput {
    std::vector<std::uint8_t> to_send;
    std::vector<std::uint8_t> plaintext;
};

// The secrets of the handshake, exposed for the RFC 8448 test.
struct TlsSecrets {
    std::vector<std::uint8_t> handshake_secret;
    std::vector<std::uint8_t> client_handshake_traffic;
    std::vector<std::uint8_t> server_handshake_traffic;
    std::vector<std::uint8_t> master_secret;
    std::vector<std::uint8_t> client_application_traffic;
    std::vector<std::uint8_t> server_application_traffic;
    std::vector<std::uint8_t> resumption_master; // §7.1, once the client Finished is sent
};

class TlsEngine {
public:
    explicit TlsEngine(TlsConfig config);
    ~TlsEngine();
    TlsEngine(TlsEngine const&) = delete;
    TlsEngine& operator=(TlsEngine const&) = delete;

    // The first flight: a record carrying the ClientHello.
    std::vector<std::uint8_t> start();
    // The test's way in: the given ClientHello handshake message (type,
    // length and body) stands as ours, with the config's private key.
    std::vector<std::uint8_t> start_with_client_hello(std::span<std::uint8_t const> message);

    // Bytes from the socket, any amount. False when the connection has
    // failed (error() says why); out carries what to send and what was read.
    bool feed(std::span<std::uint8_t const> bytes, TlsOutput& out);
    // Application data for the peer, as protected records; empty when not connected.
    std::vector<std::uint8_t> seal(std::span<std::uint8_t const> plaintext);
    // The close_notify alert, protected; the engine is Closed afterwards.
    std::vector<std::uint8_t> close_notify();

    TlsState state() const;
    bool connected() const { return state() == TlsState::Connected; }
    // 0x0304 or 0x0303 once the server has chosen; 0 before that.
    std::uint16_t version() const;
    std::string const& error() const;
    std::vector<Certificate> const& peer_chain() const;
    std::string const& alpn() const; // the protocol the server chose, or empty
    CipherSuite cipher_suite() const; // the suite the server chose
    int hello_retry_requests() const; // the retries the server asked for; a second is fatal
    // Whether the server took the ticket or session offered, so that no
    // certificate was sent and the handshake was the short one.
    bool resumed() const;
    TlsSecrets const& secrets() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
