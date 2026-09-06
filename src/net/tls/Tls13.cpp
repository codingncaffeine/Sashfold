#include "net/tls/Tls13.h"

#include "crypto/ChaCha20Poly1305.h"
#include "crypto/Hkdf.h"
#include "crypto/Hmac.h"
#include "crypto/Sha2.h"

#include <algorithm>
#include <cstring>

namespace sashfold::tls {

namespace {

using Hash = crypto::Sha256;
using Bytes = std::vector<std::uint8_t>;
using View = std::span<std::uint8_t const>;

// RFC 8446 §5.1 ContentType, §4 HandshakeType, §4.2 ExtensionType, §6 AlertDescription.
enum ContentType : std::uint8_t { change_cipher_spec = 20, alert = 21, handshake = 22, application_data = 23 };
enum HandshakeType : std::uint8_t {
    client_hello = 1,
    server_hello = 2,
    new_session_ticket = 4,
    encrypted_extensions = 8,
    certificate = 11,
    certificate_request = 13,
    certificate_verify = 15,
    finished = 20,
    key_update = 24,
};
enum Extension : std::uint16_t {
    ext_server_name = 0,
    ext_supported_groups = 10,
    ext_signature_algorithms = 13,
    ext_alpn = 16,
    ext_pre_shared_key = 41,
    ext_early_data = 42,
    ext_supported_versions = 43,
    ext_psk_key_exchange_modes = 45,
    ext_key_share = 51,
};
enum Alert : std::uint8_t {
    alert_close_notify = 0,
    unexpected_message = 10,
    bad_record_mac = 20,
    record_overflow = 22,
    handshake_failure = 40,
    bad_certificate = 42,
    unsupported_certificate = 43,
    illegal_parameter = 47,
    decode_error = 50,
    decrypt_error = 51,
    protocol_version = 70,
    internal_error = 80,
    missing_extension = 109,
    unsupported_extension = 110,
    no_application_protocol = 120,
};

constexpr std::uint16_t suite_chacha20_poly1305_sha256 = 0x1303;
constexpr std::uint16_t group_x25519 = 0x001d;
constexpr std::uint16_t version_tls13 = 0x0304;
constexpr std::size_t max_plaintext = 16384;
constexpr std::size_t max_ciphertext = max_plaintext + 256;
constexpr std::size_t max_handshake_message = 1 << 20;

// §4.1.3: the random of a HelloRetryRequest, and the downgrade sentinels.
constexpr std::uint8_t hello_retry_request_random[32] = {
    0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c,
};
constexpr std::uint8_t downgrade_tls12[8] = { 0x44, 0x4f, 0x57, 0x4e, 0x47, 0x52, 0x44, 0x01 };
constexpr std::uint8_t downgrade_tls11[8] = { 0x44, 0x4f, 0x57, 0x4e, 0x47, 0x52, 0x44, 0x00 };

// ---- byte writing and reading

void put8(Bytes& out, std::uint8_t v) { out.push_back(v); }
void put16(Bytes& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}
void put24(Bytes& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}
void put_bytes(Bytes& out, View bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }

// A bounds-checked cursor over a message; every read fails cleanly.
struct Reader {
    View bytes;
    std::size_t offset = 0;

    std::size_t left() const { return bytes.size() - offset; }
    bool done() const { return offset >= bytes.size(); }
    bool u8(std::uint8_t& v)
    {
        if (left() < 1)
            return false;
        v = bytes[offset++];
        return true;
    }
    bool u16(std::uint16_t& v)
    {
        if (left() < 2)
            return false;
        v = static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
        offset += 2;
        return true;
    }
    bool u24(std::uint32_t& v)
    {
        if (left() < 3)
            return false;
        v = (std::uint32_t(bytes[offset]) << 16) | (std::uint32_t(bytes[offset + 1]) << 8) | bytes[offset + 2];
        offset += 3;
        return true;
    }
    bool take(std::size_t n, View& v)
    {
        if (left() < n)
            return false;
        v = bytes.subspan(offset, n);
        offset += n;
        return true;
    }
    // A vector with a one-, two- or three-byte length prefix.
    bool vector8(View& v)
    {
        std::uint8_t n;
        return u8(n) && take(n, v);
    }
    bool vector16(View& v)
    {
        std::uint16_t n;
        return u16(n) && take(n, v);
    }
    bool vector24(View& v)
    {
        std::uint32_t n;
        return u24(n) && take(n, v);
    }
};

// ---- the key schedule (§7.1) over HKDF-SHA256

Bytes hkdf_expand_label(View secret, std::string_view label, View context, std::size_t length)
{
    // HkdfLabel: uint16 length; opaque label<7..255> = "tls13 " + Label; opaque context<0..255>.
    Bytes info;
    put16(info, static_cast<std::uint16_t>(length));
    std::string const full = "tls13 " + std::string(label);
    put8(info, static_cast<std::uint8_t>(full.size()));
    info.insert(info.end(), full.begin(), full.end());
    put8(info, static_cast<std::uint8_t>(context.size()));
    put_bytes(info, context);
    return crypto::hkdf_expand<Hash>(secret, info, length);
}

Bytes derive_secret(View secret, std::string_view label, View transcript_hash)
{
    return hkdf_expand_label(secret, label, transcript_hash, Hash::digest_size);
}

Bytes hkdf_extract(View salt, View ikm)
{
    Hash::Digest const prk = crypto::hkdf_extract<Hash>(salt, ikm);
    return Bytes(prk.begin(), prk.end());
}

// One direction's record protection: the key and IV of §7.3 and the
// sequence number that makes each nonce (§5.3).
struct TrafficKeys {
    crypto::ChaChaKey key {};
    crypto::ChaChaNonce iv {};
    std::uint64_t sequence = 0;
    bool active = false;

    static TrafficKeys from_secret(View secret)
    {
        TrafficKeys keys;
        Bytes const key = hkdf_expand_label(secret, "key", {}, 32);
        Bytes const iv = hkdf_expand_label(secret, "iv", {}, 12);
        std::copy(key.begin(), key.end(), keys.key.begin());
        std::copy(iv.begin(), iv.end(), keys.iv.begin());
        keys.active = true;
        return keys;
    }

    crypto::ChaChaNonce nonce() const
    {
        crypto::ChaChaNonce n = iv;
        for (int i = 0; i < 8; ++i)
            n[4 + i] = static_cast<std::uint8_t>(n[4 + i] ^ static_cast<std::uint8_t>(sequence >> (8 * (7 - i))));
        return n;
    }
};

// §5.2: a protected record is the content with its true type appended,
// sealed under the header as additional data.
Bytes seal_record(TrafficKeys& keys, std::uint8_t type, View content)
{
    Bytes inner(content.begin(), content.end());
    inner.push_back(type);
    Bytes record;
    put8(record, application_data);
    put16(record, 0x0303);
    put16(record, static_cast<std::uint16_t>(inner.size() + 16));
    View const header(record);
    Bytes ciphertext(inner.size());
    crypto::Poly1305Tag const tag = crypto::chacha20_poly1305_seal(keys.key, keys.nonce(), header, inner, ciphertext);
    keys.sequence += 1;
    put_bytes(record, ciphertext);
    put_bytes(record, tag);
    return record;
}

// The reverse: the plaintext and its true type, or false on a bad tag or
// a record that is all padding.
bool open_record(TrafficKeys& keys, View header, View body, Bytes& plaintext, std::uint8_t& type)
{
    if (body.size() < 16)
        return false;
    View const ciphertext = body.subspan(0, body.size() - 16);
    crypto::Poly1305Tag tag;
    std::copy(body.end() - 16, body.end(), tag.begin());
    plaintext.assign(ciphertext.size(), 0);
    if (!crypto::chacha20_poly1305_open(keys.key, keys.nonce(), header, ciphertext, tag, plaintext))
        return false;
    keys.sequence += 1;
    while (!plaintext.empty() && plaintext.back() == 0)
        plaintext.pop_back();
    if (plaintext.empty())
        return false;
    type = plaintext.back();
    plaintext.pop_back();
    return true;
}

Bytes plain_record(std::uint8_t type, View content)
{
    Bytes record;
    put8(record, type);
    put16(record, 0x0303);
    put16(record, static_cast<std::uint16_t>(content.size()));
    put_bytes(record, content);
    return record;
}

Bytes handshake_message(std::uint8_t type, View body)
{
    Bytes message;
    put8(message, type);
    put24(message, static_cast<std::uint32_t>(body.size()));
    put_bytes(message, body);
    return message;
}

}

// ---- the engine

struct TlsEngine::Impl {
    TlsConfig config;
    TlsState state = TlsState::Start;
    std::string error;
    std::string alpn;
    std::vector<Certificate> chain;
    TlsSecrets secrets;

    Bytes in; // socket bytes not yet a whole record
    Bytes handshake_buffer; // handshake bytes not yet a whole message
    Hash transcript;
    Bytes session_id_sent;
    TrafficKeys read_keys;
    TrafficKeys write_keys;
    Bytes shared_secret;
    Bytes hash_before_certificate_verify;
    bool certificate_requested = false;
    Bytes certificate_request_context;
    PublicKey leaf_key;

    explicit Impl(TlsConfig c)
        : config(std::move(c))
    {
    }

    Hash::Digest transcript_hash() const
    {
        Hash copy = transcript;
        return copy.finish();
    }

    // A fatal alert under whatever keys are current, and the failed state.
    void fail(TlsOutput& out, std::uint8_t description, std::string reason)
    {
        if (state == TlsState::Failed)
            return;
        std::uint8_t const body[2] = { 2, description };
        if (write_keys.active)
            put_bytes(out.to_send, seal_record(write_keys, alert, body));
        else
            put_bytes(out.to_send, plain_record(alert, body));
        state = TlsState::Failed;
        error = std::move(reason);
    }

    // ---- ClientHello (§4.1.2)

    Bytes build_client_hello()
    {
        Bytes body;
        put16(body, 0x0303);
        put_bytes(body, config.client_random);
        session_id_sent.clear();
        if (config.compatibility_mode)
            session_id_sent.assign(config.session_id.begin(), config.session_id.end());
        put8(body, static_cast<std::uint8_t>(session_id_sent.size()));
        put_bytes(body, session_id_sent);
        put16(body, 2);
        put16(body, suite_chacha20_poly1305_sha256);
        put8(body, 1);
        put8(body, 0);

        Bytes extensions;
        auto extension = [&](std::uint16_t type, View data) {
            put16(extensions, type);
            put16(extensions, static_cast<std::uint16_t>(data.size()));
            put_bytes(extensions, data);
        };
        if (!config.server_name.empty()) {
            Bytes sni;
            put16(sni, static_cast<std::uint16_t>(config.server_name.size() + 3));
            put8(sni, 0);
            put16(sni, static_cast<std::uint16_t>(config.server_name.size()));
            sni.insert(sni.end(), config.server_name.begin(), config.server_name.end());
            extension(ext_server_name, sni);
        }
        {
            Bytes groups;
            put16(groups, 2);
            put16(groups, group_x25519);
            extension(ext_supported_groups, groups);
        }
        {
            Bytes algorithms;
            std::uint16_t const schemes[] = { 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601 };
            put16(algorithms, static_cast<std::uint16_t>(sizeof schemes));
            for (std::uint16_t const scheme : schemes)
                put16(algorithms, scheme);
            extension(ext_signature_algorithms, algorithms);
        }
        {
            Bytes versions;
            put8(versions, 2);
            put16(versions, version_tls13);
            extension(ext_supported_versions, versions);
        }
        {
            Bytes modes;
            put8(modes, 1);
            put8(modes, 1); // psk_dhe_ke
            extension(ext_psk_key_exchange_modes, modes);
        }
        {
            crypto::X25519Key const public_key = crypto::x25519_public(config.private_key);
            Bytes shares;
            put16(shares, 32 + 4);
            put16(shares, group_x25519);
            put16(shares, 32);
            put_bytes(shares, public_key);
            extension(ext_key_share, shares);
        }
        {
            Bytes alpn_list;
            std::string const http11 = "http/1.1";
            put16(alpn_list, static_cast<std::uint16_t>(http11.size() + 1));
            put8(alpn_list, static_cast<std::uint8_t>(http11.size()));
            alpn_list.insert(alpn_list.end(), http11.begin(), http11.end());
            extension(ext_alpn, alpn_list);
        }
        put16(body, static_cast<std::uint16_t>(extensions.size()));
        put_bytes(body, extensions);
        return handshake_message(client_hello, body);
    }

    Bytes send_client_hello(Bytes const& message)
    {
        transcript.update(message);
        state = TlsState::WaitServerHello;
        return plain_record(handshake, message);
    }

    // ---- ServerHello (§4.1.3) and the handshake keys (§7.1)

    bool on_server_hello(View body, TlsOutput& out)
    {
        Reader r { body };
        std::uint16_t version = 0;
        View random;
        View session_id;
        std::uint16_t suite = 0;
        std::uint8_t compression = 0;
        View extensions;
        if (!r.u16(version) || !r.take(32, random) || !r.vector8(session_id) || !r.u16(suite) || !r.u8(compression) || !r.vector16(extensions) || !r.done()) {
            fail(out, decode_error, "ServerHello did not decode");
            return false;
        }
        if (std::equal(random.begin(), random.end(), hello_retry_request_random)) {
            fail(out, handshake_failure, "the server asked for a HelloRetryRequest, which is not supported yet");
            return false;
        }
        if (version != 0x0303 || compression != 0) {
            fail(out, illegal_parameter, "ServerHello carried a legacy version or a compression method");
            return false;
        }
        if (!std::equal(session_id.begin(), session_id.end(), session_id_sent.begin(), session_id_sent.end())) {
            fail(out, illegal_parameter, "ServerHello did not echo the session id");
            return false;
        }
        if (suite != suite_chacha20_poly1305_sha256) {
            fail(out, illegal_parameter, "the server chose a cipher suite that was not offered");
            return false;
        }
        bool saw_version = false;
        bool saw_key_share = false;
        crypto::X25519Key server_key {};
        Reader e { extensions };
        while (!e.done()) {
            std::uint16_t type = 0;
            View data;
            if (!e.u16(type) || !e.vector16(data)) {
                fail(out, decode_error, "ServerHello extensions did not decode");
                return false;
            }
            Reader d { data };
            if (type == ext_supported_versions) {
                std::uint16_t chosen = 0;
                if (!d.u16(chosen) || !d.done() || chosen != version_tls13) {
                    fail(out, illegal_parameter, "the server chose a version other than TLS 1.3");
                    return false;
                }
                saw_version = true;
            } else if (type == ext_key_share) {
                std::uint16_t group = 0;
                View key;
                if (!d.u16(group) || !d.vector16(key) || !d.done() || group != group_x25519 || key.size() != 32) {
                    fail(out, illegal_parameter, "the server's key share is not an x25519 key");
                    return false;
                }
                std::copy(key.begin(), key.end(), server_key.begin());
                saw_key_share = true;
            } else {
                fail(out, unsupported_extension, "ServerHello carried an extension that was not offered");
                return false;
            }
        }
        if (!saw_version) {
            fail(out, protocol_version, "the server did not negotiate TLS 1.3");
            return false;
        }
        if (!saw_key_share) {
            fail(out, missing_extension, "ServerHello carried no key share");
            return false;
        }
        View const tail = random.subspan(24, 8);
        if (std::equal(tail.begin(), tail.end(), downgrade_tls12) || std::equal(tail.begin(), tail.end(), downgrade_tls11)) {
            fail(out, illegal_parameter, "the server's random carries a downgrade sentinel");
            return false;
        }
        crypto::X25519Key shared {};
        if (!crypto::x25519(shared, config.private_key, server_key)) {
            fail(out, illegal_parameter, "the server's key share is a low-order point");
            return false;
        }
        shared_secret.assign(shared.begin(), shared.end());

        Bytes const zeros(Hash::digest_size, 0);
        Bytes const early = hkdf_extract(zeros, zeros);
        Hash::Digest const empty_hash = Hash::hash({});
        Bytes const derived = derive_secret(early, "derived", empty_hash);
        secrets.handshake_secret = hkdf_extract(derived, shared_secret);
        Hash::Digest const hello_hash = transcript_hash();
        secrets.client_handshake_traffic = derive_secret(secrets.handshake_secret, "c hs traffic", hello_hash);
        secrets.server_handshake_traffic = derive_secret(secrets.handshake_secret, "s hs traffic", hello_hash);
        read_keys = TrafficKeys::from_secret(secrets.server_handshake_traffic);
        write_keys = TrafficKeys::from_secret(secrets.client_handshake_traffic);
        state = TlsState::WaitEncryptedExtensions;
        return true;
    }

    // ---- the encrypted flight (§4.3, §4.4)

    bool on_encrypted_extensions(View body, TlsOutput& out)
    {
        Reader r { body };
        View extensions;
        if (!r.vector16(extensions) || !r.done()) {
            fail(out, decode_error, "EncryptedExtensions did not decode");
            return false;
        }
        Reader e { extensions };
        while (!e.done()) {
            std::uint16_t type = 0;
            View data;
            if (!e.u16(type) || !e.vector16(data)) {
                fail(out, decode_error, "EncryptedExtensions did not decode");
                return false;
            }
            if (type == ext_alpn) {
                // ProtocolNameList with exactly one name, one of ours.
                Reader d { data };
                View list;
                View name;
                if (!d.vector16(list) || !d.done()) {
                    fail(out, decode_error, "the ALPN answer did not decode");
                    return false;
                }
                Reader l { list };
                if (!l.vector8(name) || !l.done() || std::string(name.begin(), name.end()) != "http/1.1") {
                    fail(out, no_application_protocol, "the server chose a protocol that was not offered");
                    return false;
                }
                alpn = "http/1.1";
            } else if (type == ext_server_name || type == ext_supported_groups) {
                // The empty acknowledgement, and the server's groups: nothing to do.
            } else {
                fail(out, unsupported_extension, "EncryptedExtensions carried an extension that was not offered");
                return false;
            }
        }
        state = TlsState::WaitCertificate;
        return true;
    }

    bool on_certificate_request(View body, TlsOutput& out)
    {
        Reader r { body };
        View context;
        View extensions;
        if (!r.vector8(context) || !r.vector16(extensions) || !r.done()) {
            fail(out, decode_error, "CertificateRequest did not decode");
            return false;
        }
        certificate_requested = true;
        certificate_request_context.assign(context.begin(), context.end());
        return true;
    }

    bool on_certificate(View body, TlsOutput& out)
    {
        Reader r { body };
        View context;
        View list;
        if (!r.vector8(context) || !r.vector24(list) || !r.done()) {
            fail(out, decode_error, "Certificate did not decode");
            return false;
        }
        if (!context.empty()) {
            fail(out, illegal_parameter, "Certificate carried a request context");
            return false;
        }
        Reader entries { list };
        while (!entries.done()) {
            View cert_data;
            View extensions;
            if (!entries.vector24(cert_data) || !entries.vector16(extensions)) {
                fail(out, decode_error, "the certificate list did not decode");
                return false;
            }
            std::optional<Certificate> parsed = parse_certificate(cert_data);
            if (!parsed) {
                fail(out, bad_certificate, "a certificate the server sent did not parse");
                return false;
            }
            chain.push_back(std::move(*parsed));
        }
        if (chain.empty()) {
            fail(out, decode_error, "the server sent no certificate");
            return false;
        }
        leaf_key = chain[0].public_key;
        if (leaf_key.kind == PublicKey::Kind::Unsupported) {
            fail(out, unsupported_certificate, "the server's certificate carries a key of a kind not supported");
            return false;
        }
        Hash::Digest const cert_hash = transcript_hash();
        hash_before_certificate_verify.assign(cert_hash.begin(), cert_hash.end());
        state = TlsState::WaitCertificateVerify;
        return true;
    }

    bool on_certificate_verify(View body, TlsOutput& out)
    {
        Reader r { body };
        std::uint16_t scheme = 0;
        View signature;
        if (!r.u16(scheme) || !r.vector16(signature) || !r.done()) {
            fail(out, decode_error, "CertificateVerify did not decode");
            return false;
        }
        // §4.2.3: the scheme names the key kind, the curve and the hash.
        SignatureAlgorithm algorithm;
        PublicKey::Kind required = PublicKey::Kind::Unsupported;
        crypto::CurveId curve = crypto::CurveId::P256;
        switch (scheme) {
        case 0x0403:
            algorithm.kind = SignatureKind::Ecdsa;
            algorithm.hash = crypto::HashId::Sha256;
            required = PublicKey::Kind::Ec;
            curve = crypto::CurveId::P256;
            break;
        case 0x0503:
            algorithm.kind = SignatureKind::Ecdsa;
            algorithm.hash = crypto::HashId::Sha384;
            required = PublicKey::Kind::Ec;
            curve = crypto::CurveId::P384;
            break;
        case 0x0804:
        case 0x0805:
        case 0x0806:
            algorithm.kind = SignatureKind::RsaPss;
            algorithm.hash = scheme == 0x0804 ? crypto::HashId::Sha256 : scheme == 0x0805 ? crypto::HashId::Sha384 : crypto::HashId::Sha512;
            algorithm.pss_salt_length = crypto::digest_size(algorithm.hash);
            required = PublicKey::Kind::Rsa;
            break;
        default:
            fail(out, illegal_parameter, "CertificateVerify used a signature scheme that was not offered");
            return false;
        }
        if (leaf_key.kind != required || (required == PublicKey::Kind::Ec && leaf_key.curve != curve)) {
            fail(out, illegal_parameter, "CertificateVerify's scheme does not fit the server's key");
            return false;
        }
        // §4.4.3: 64 spaces, the context string, a zero byte, the transcript hash.
        Bytes content(64, 0x20);
        std::string const context = "TLS 1.3, server CertificateVerify";
        content.insert(content.end(), context.begin(), context.end());
        content.push_back(0);
        put_bytes(content, hash_before_certificate_verify);
        if (!verify_signature(leaf_key, algorithm, content, signature)) {
            fail(out, decrypt_error, "the server's signature over the handshake did not verify");
            return false;
        }
        state = TlsState::WaitFinished;
        return true;
    }

    Bytes finished_mac(View traffic_secret, Hash::Digest const& hash) const
    {
        Bytes const key = hkdf_expand_label(traffic_secret, "finished", {}, Hash::digest_size);
        Hash::Digest const mac = crypto::Hmac<Hash>::mac(key, hash);
        return Bytes(mac.begin(), mac.end());
    }

    // The server's Finished (§4.4.4), then the application keys (§7.1),
    // the verdict on the chain, and our own second flight.
    bool on_server_finished(View body, View raw_message, TlsOutput& out)
    {
        Bytes const expected = finished_mac(secrets.server_handshake_traffic, transcript_hash());
        if (!crypto::constant_time_equal(body, expected)) {
            fail(out, decrypt_error, "the server's Finished did not verify");
            return false;
        }
        transcript.update(raw_message);
        Hash::Digest const empty_hash = Hash::hash({});
        Bytes const derived = derive_secret(secrets.handshake_secret, "derived", empty_hash);
        Bytes const zeros(Hash::digest_size, 0);
        secrets.master_secret = hkdf_extract(derived, zeros);
        Hash::Digest const finished_hash = transcript_hash();
        secrets.client_application_traffic = derive_secret(secrets.master_secret, "c ap traffic", finished_hash);
        secrets.server_application_traffic = derive_secret(secrets.master_secret, "s ap traffic", finished_hash);

        std::string reason;
        if (config.verify_chain && !config.verify_chain(chain, reason)) {
            fail(out, bad_certificate, reason.empty() ? "certificate validation failed" : reason);
            return false;
        }

        // §D.4: the ChangeCipherSpec of compatibility mode, then, still under
        // the handshake keys, an empty Certificate if one was asked for, and
        // the Finished over the transcript through that.
        if (config.compatibility_mode) {
            std::uint8_t const one = 1;
            put_bytes(out.to_send, plain_record(change_cipher_spec, View(&one, 1)));
        }
        Bytes flight;
        if (certificate_requested) {
            Bytes body_bytes;
            put8(body_bytes, static_cast<std::uint8_t>(certificate_request_context.size()));
            put_bytes(body_bytes, certificate_request_context);
            put24(body_bytes, 0);
            Bytes const message = handshake_message(certificate, body_bytes);
            transcript.update(message);
            put_bytes(flight, message);
        }
        Bytes const verify_data = finished_mac(secrets.client_handshake_traffic, transcript_hash());
        Bytes const message = handshake_message(finished, verify_data);
        transcript.update(message);
        put_bytes(flight, message);
        put_bytes(out.to_send, seal_record(write_keys, handshake, flight));

        read_keys = TrafficKeys::from_secret(secrets.server_application_traffic);
        write_keys = TrafficKeys::from_secret(secrets.client_application_traffic);
        state = TlsState::Connected;
        return true;
    }

    // §4.6.3: the peer's keys turn over; when asked, ours do too, after
    // our own KeyUpdate goes out under the old ones.
    bool on_key_update(View body, TlsOutput& out)
    {
        if (body.size() != 1 || body[0] > 1) {
            fail(out, illegal_parameter, "KeyUpdate did not decode");
            return false;
        }
        secrets.server_application_traffic = hkdf_expand_label(secrets.server_application_traffic, "traffic upd", {}, Hash::digest_size);
        read_keys = TrafficKeys::from_secret(secrets.server_application_traffic);
        if (body[0] == 1) {
            std::uint8_t const update_not_requested = 0;
            Bytes const message = handshake_message(key_update, View(&update_not_requested, 1));
            put_bytes(out.to_send, seal_record(write_keys, handshake, message));
            secrets.client_application_traffic = hkdf_expand_label(secrets.client_application_traffic, "traffic upd", {}, Hash::digest_size);
            write_keys = TrafficKeys::from_secret(secrets.client_application_traffic);
        }
        return true;
    }

    bool on_handshake_message(std::uint8_t type, View body, View raw_message, TlsOutput& out)
    {
        switch (state) {
        case TlsState::WaitServerHello:
            if (type != server_hello)
                break;
            transcript.update(raw_message);
            return on_server_hello(body, out);
        case TlsState::WaitEncryptedExtensions:
            if (type != encrypted_extensions)
                break;
            transcript.update(raw_message);
            return on_encrypted_extensions(body, out);
        case TlsState::WaitCertificate:
            if (type == certificate_request) {
                transcript.update(raw_message);
                return on_certificate_request(body, out);
            }
            if (type != certificate)
                break;
            transcript.update(raw_message);
            return on_certificate(body, out);
        case TlsState::WaitCertificateVerify:
            if (type != certificate_verify)
                break;
            transcript.update(raw_message);
            return on_certificate_verify(body, out);
        case TlsState::WaitFinished:
            if (type != finished)
                break;
            return on_server_finished(body, raw_message, out);
        case TlsState::Connected:
            if (type == new_session_ticket)
                return true; // §4.6.1: tickets are for resumption, which is not written
            if (type == key_update)
                return on_key_update(body, out);
            break;
        default:
            break;
        }
        fail(out, unexpected_message, "a handshake message arrived out of order");
        return false;
    }

    // ---- records in, plaintext and the next flight out

    // §5.1: a handshake fragment may be split across records and several
    // messages may share one; reassemble on the transcript's own buffer.
    bool drain_handshake(TlsOutput& out)
    {
        for (;;) {
            if (handshake_buffer.size() < 4)
                return true;
            std::uint32_t const length = (std::uint32_t(handshake_buffer[1]) << 16) | (std::uint32_t(handshake_buffer[2]) << 8) | handshake_buffer[3];
            if (length > max_handshake_message) {
                fail(out, decode_error, "a handshake message is too large");
                return false;
            }
            if (handshake_buffer.size() < 4 + length)
                return true;
            std::uint8_t const type = handshake_buffer[0];
            View const raw(handshake_buffer.data(), 4 + length);
            View const body = raw.subspan(4);
            if (!on_handshake_message(type, body, raw, out))
                return false;
            handshake_buffer.erase(handshake_buffer.begin(), handshake_buffer.begin() + 4 + static_cast<std::ptrdiff_t>(length));
            if (state == TlsState::Failed || state == TlsState::Closed)
                return state != TlsState::Failed;
        }
    }

    // Feed handshake bytes to the reassembler; refuse plaintext handshake
    // records once the keys are up (§5.1: no mixing).
    bool accept_handshake(View fragment, bool encrypted, TlsOutput& out)
    {
        if (!encrypted && read_keys.active) {
            fail(out, unexpected_message, "a plaintext handshake record arrived after the keys were established");
            return false;
        }
        handshake_buffer.insert(handshake_buffer.end(), fragment.begin(), fragment.end());
        return drain_handshake(out);
    }

    bool handle_record(std::uint8_t type, View fragment, bool encrypted, TlsOutput& out)
    {
        switch (type) {
        case handshake:
            return accept_handshake(fragment, encrypted, out);
        case application_data:
            if (state != TlsState::Connected) {
                fail(out, unexpected_message, "application data arrived before the handshake finished");
                return false;
            }
            out.plaintext.insert(out.plaintext.end(), fragment.begin(), fragment.end());
            return true;
        case alert: {
            if (fragment.size() != 2) {
                fail(out, decode_error, "an alert did not decode");
                return false;
            }
            if (fragment[1] == alert_close_notify) {
                state = TlsState::Closed;
                return true;
            }
            state = TlsState::Failed;
            error = "the server sent a fatal alert (" + std::to_string(fragment[1]) + ")";
            return false;
        }
        case change_cipher_spec:
            // §5: a compatibility-mode CCS between the hellos and the
            // server's Finished; anything else, or a bad body, is an error.
            if (encrypted || fragment.size() != 1 || fragment[0] != 1 || state == TlsState::Connected) {
                fail(out, unexpected_message, "an unexpected ChangeCipherSpec");
                return false;
            }
            return true;
        default:
            fail(out, unexpected_message, "a record of an unknown type");
            return false;
        }
    }

    bool feed(View bytes, TlsOutput& out)
    {
        if (state == TlsState::Failed)
            return false;
        in.insert(in.end(), bytes.begin(), bytes.end());
        std::size_t offset = 0;
        while (in.size() - offset >= 5) {
            std::uint8_t const type = in[offset];
            std::size_t const length = (std::size_t(in[offset + 3]) << 8) | in[offset + 4];
            std::size_t const limit = read_keys.active && type == application_data ? max_ciphertext : max_plaintext;
            if (length > limit) {
                fail(out, record_overflow, "a record is larger than the protocol allows");
                break;
            }
            if (in.size() - offset - 5 < length)
                break;
            View const header(in.data() + offset, 5);
            View const record_body(in.data() + offset + 5, length);
            offset += 5 + length;
            if (read_keys.active && type == application_data) {
                Bytes plaintext;
                std::uint8_t inner_type = 0;
                if (!open_record(read_keys, header, record_body, plaintext, inner_type)) {
                    fail(out, bad_record_mac, "a record failed to decrypt");
                    break;
                }
                if (!handle_record(inner_type, plaintext, true, out))
                    break;
            } else {
                if (!handle_record(type, record_body, false, out))
                    break;
            }
            if (state == TlsState::Failed || state == TlsState::Closed)
                break;
        }
        in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(offset));
        return state != TlsState::Failed;
    }

    // Application data as one or more records, each at most a full plaintext.
    Bytes seal(View plaintext)
    {
        Bytes out;
        if (state != TlsState::Connected)
            return out;
        std::size_t offset = 0;
        while (offset < plaintext.size()) {
            std::size_t const take = std::min(max_plaintext, plaintext.size() - offset);
            put_bytes(out, seal_record(write_keys, application_data, plaintext.subspan(offset, take)));
            offset += take;
        }
        return out;
    }
};

// ---- the public surface

TlsEngine::TlsEngine(TlsConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config)))
{
}

TlsEngine::~TlsEngine() = default;

std::vector<std::uint8_t> TlsEngine::start()
{
    return m_impl->send_client_hello(m_impl->build_client_hello());
}

std::vector<std::uint8_t> TlsEngine::start_with_client_hello(std::span<std::uint8_t const> message)
{
    return m_impl->send_client_hello(Bytes(message.begin(), message.end()));
}

bool TlsEngine::feed(std::span<std::uint8_t const> bytes, TlsOutput& out)
{
    return m_impl->feed(bytes, out);
}

std::vector<std::uint8_t> TlsEngine::seal(std::span<std::uint8_t const> plaintext)
{
    return m_impl->seal(plaintext);
}

std::vector<std::uint8_t> TlsEngine::close_notify()
{
    if (m_impl->state != TlsState::Connected)
        return {};
    std::uint8_t const body[2] = { 1, alert_close_notify };
    Bytes const record = seal_record(m_impl->write_keys, alert, body);
    m_impl->state = TlsState::Closed;
    return record;
}

TlsState TlsEngine::state() const { return m_impl->state; }
std::string const& TlsEngine::error() const { return m_impl->error; }
std::vector<Certificate> const& TlsEngine::peer_chain() const { return m_impl->chain; }
std::string const& TlsEngine::alpn() const { return m_impl->alpn; }
TlsSecrets const& TlsEngine::secrets() const { return m_impl->secrets; }

}
