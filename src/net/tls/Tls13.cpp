#include "net/tls/Tls13.h"

#include "crypto/AesGcm.h"
#include "crypto/ChaCha20Poly1305.h"
#include "crypto/Ec.h"
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

// RFC 8446 §5.1 ContentType, §4 HandshakeType, §4.2 ExtensionType, §6 AlertDescription;
// the TLS 1.2 messages and extensions from RFC 5246 §7.4, RFC 8422 §5.1,
// RFC 7627 and RFC 5746.
enum ContentType : std::uint8_t { change_cipher_spec = 20, alert = 21, handshake = 22, application_data = 23 };
enum HandshakeType : std::uint8_t {
    hello_request = 0,
    client_hello = 1,
    server_hello = 2,
    new_session_ticket = 4,
    encrypted_extensions = 8,
    certificate = 11,
    server_key_exchange = 12,
    certificate_request = 13,
    server_hello_done = 14,
    certificate_verify = 15,
    client_key_exchange = 16,
    finished = 20,
    key_update = 24,
    message_hash = 254, // §4.4.1: the synthetic message a retry leaves behind
};
enum Extension : std::uint16_t {
    ext_server_name = 0,
    ext_supported_groups = 10,
    ext_ec_point_formats = 11,
    ext_signature_algorithms = 13,
    ext_alpn = 16,
    ext_extended_master_secret = 23,
    ext_pre_shared_key = 41,
    ext_early_data = 42,
    ext_supported_versions = 43,
    ext_cookie = 44,
    ext_psk_key_exchange_modes = 45,
    ext_key_share = 51,
    ext_renegotiation_info = 0xff01,
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

// ChaCha20-Poly1305 first: the AES beside it is software, and a client that
// cannot reach for the instructions should say which it would rather have.
constexpr CipherSuite default_suites13[] = { CipherSuite::ChaCha20Poly1305Sha256, CipherSuite::Aes128GcmSha256 };
constexpr CipherSuite default_suites12[] = { CipherSuite::EcdheEcdsaChaCha20Poly1305Sha256, CipherSuite::EcdheRsaChaCha20Poly1305Sha256,
    CipherSuite::EcdheEcdsaAes128GcmSha256, CipherSuite::EcdheRsaAes128GcmSha256 };
constexpr std::uint16_t group_x25519 = 0x001d;
constexpr std::uint16_t group_secp256r1 = 0x0017;
// The groups a key exchange can happen over, in the order of preference.
// A 1.3 hello carries a share for the first alone, so a server that wants
// the second asks for another hello (§4.2.8) — and a hello with no share
// leaves the retry request the choice of either.
constexpr std::uint16_t offered_groups[] = { group_x25519, group_secp256r1 };
constexpr std::uint16_t version_tls13 = 0x0304;
constexpr std::uint16_t version_tls12 = 0x0303;
constexpr std::size_t max_plaintext = 16384;
constexpr std::size_t max_ciphertext = max_plaintext + 256;
constexpr std::size_t max_ciphertext12 = max_plaintext + 2048; // RFC 5246 §6.2.3
constexpr std::size_t max_handshake_message = 1 << 20;

bool is_tls12_suite(CipherSuite suite)
{
    return suite != CipherSuite::Aes128GcmSha256 && suite != CipherSuite::ChaCha20Poly1305Sha256;
}

// A 1.2 suite names how the server proves itself; the leaf's key must fit.
bool suite_wants_ecdsa(CipherSuite suite)
{
    return suite == CipherSuite::EcdheEcdsaAes128GcmSha256 || suite == CipherSuite::EcdheEcdsaChaCha20Poly1305Sha256;
}

enum class Aead : std::uint8_t { Aes128Gcm, ChaCha20Poly1305 };

Aead aead_of(CipherSuite suite)
{
    switch (suite) {
    case CipherSuite::Aes128GcmSha256:
    case CipherSuite::EcdheEcdsaAes128GcmSha256:
    case CipherSuite::EcdheRsaAes128GcmSha256:
        return Aead::Aes128Gcm;
    default:
        return Aead::ChaCha20Poly1305;
    }
}

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
void put64(Bytes& out, std::uint64_t v)
{
    for (int i = 7; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
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

// RFC 5246 §5: the TLS 1.2 pseudorandom function, P_SHA256 over
// label || seed, as many blocks as the length asks.
Bytes prf12(View secret, std::string_view label, View seed, std::size_t length)
{
    Bytes label_seed(label.begin(), label.end());
    put_bytes(label_seed, seed);
    Bytes out;
    Hash::Digest a = crypto::Hmac<Hash>::mac(secret, label_seed); // A(1)
    while (out.size() < length) {
        Bytes input(a.begin(), a.end());
        put_bytes(input, label_seed);
        Hash::Digest const block = crypto::Hmac<Hash>::mac(secret, input);
        out.insert(out.end(), block.begin(), block.end());
        a = crypto::Hmac<Hash>::mac(secret, a);
    }
    out.resize(length);
    return out;
}

// One direction's record protection: which AEAD holds it, the key and IV,
// and the sequence number that makes each nonce. In 1.3 (§7.3, §5.3) both
// AEADs take a 12-byte IV XOR the sequence number. In 1.2 the AES-GCM
// nonce is a 4-byte salt from the key block and 8 explicit bytes that
// travel with the record (RFC 5288), while ChaCha20-Poly1305 keeps the
// 1.3 shape (RFC 7905). Both leave a 16-byte tag.
struct TrafficKeys {
    Aead aead = Aead::ChaCha20Poly1305;
    bool tls12 = false;
    std::array<std::uint8_t, 32> key {};
    std::size_t key_size = 0;
    std::array<std::uint8_t, 12> iv {};
    std::uint64_t sequence = 0;
    bool active = false;

    static TrafficKeys from_secret(CipherSuite suite, View secret)
    {
        TrafficKeys keys;
        keys.aead = aead_of(suite);
        keys.key_size = keys.aead == Aead::Aes128Gcm ? 16 : 32;
        Bytes const key = hkdf_expand_label(secret, "key", {}, keys.key_size);
        Bytes const iv = hkdf_expand_label(secret, "iv", {}, keys.iv.size());
        std::copy(key.begin(), key.end(), keys.key.begin());
        std::copy(iv.begin(), iv.end(), keys.iv.begin());
        keys.active = true;
        return keys;
    }

    // From the 1.2 key block: the key, and the salt (AES-GCM) or the whole
    // IV (ChaCha20). Not active until the ChangeCipherSpec that turns
    // them on.
    static TrafficKeys from_block(Aead aead, View key, View iv)
    {
        TrafficKeys keys;
        keys.aead = aead;
        keys.tls12 = true;
        keys.key_size = key.size();
        std::copy(key.begin(), key.end(), keys.key.begin());
        std::copy(iv.begin(), iv.end(), keys.iv.begin());
        return keys;
    }

    static std::size_t iv_size12(Aead aead) { return aead == Aead::Aes128Gcm ? 4 : 12; }

    std::array<std::uint8_t, 12> nonce() const
    {
        std::array<std::uint8_t, 12> n = iv;
        for (int i = 0; i < 8; ++i)
            n[4 + i] = static_cast<std::uint8_t>(n[4 + i] ^ static_cast<std::uint8_t>(sequence >> (8 * (7 - i))));
        return n;
    }

    // The 1.2 AES-GCM nonce: the salt and the explicit part given.
    std::array<std::uint8_t, 12> nonce12(View explicit_part) const
    {
        std::array<std::uint8_t, 12> n {};
        std::copy_n(iv.begin(), 4, n.begin());
        std::copy_n(explicit_part.begin(), 8, n.begin() + 4);
        return n;
    }
};

using Tag = std::array<std::uint8_t, 16>;
using Nonce = std::array<std::uint8_t, 12>;

Tag aead_seal(TrafficKeys const& keys, Nonce const& nonce, View aad, View plaintext, std::span<std::uint8_t> ciphertext)
{
    if (keys.aead == Aead::Aes128Gcm) {
        crypto::AesKey key {};
        std::copy_n(keys.key.begin(), key.size(), key.begin());
        return crypto::aes128_gcm_seal(key, nonce, aad, plaintext, ciphertext);
    }
    crypto::ChaChaKey key {};
    std::copy_n(keys.key.begin(), key.size(), key.begin());
    return crypto::chacha20_poly1305_seal(key, nonce, aad, plaintext, ciphertext);
}

bool aead_open(TrafficKeys const& keys, Nonce const& nonce, View aad, View ciphertext, Tag const& tag, std::span<std::uint8_t> plaintext)
{
    if (keys.aead == Aead::Aes128Gcm) {
        crypto::AesKey key {};
        std::copy_n(keys.key.begin(), key.size(), key.begin());
        return crypto::aes128_gcm_open(key, nonce, aad, ciphertext, tag, plaintext);
    }
    crypto::ChaChaKey key {};
    std::copy_n(keys.key.begin(), key.size(), key.begin());
    return crypto::chacha20_poly1305_open(key, nonce, aad, ciphertext, tag, plaintext);
}

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
    Tag const tag = aead_seal(keys, keys.nonce(), header, inner, ciphertext);
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
    Tag tag;
    std::copy(body.end() - 16, body.end(), tag.begin());
    plaintext.assign(ciphertext.size(), 0);
    if (!aead_open(keys, keys.nonce(), header, ciphertext, tag, plaintext))
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

// RFC 5246 §6.2.3.3: a 1.2 record keeps its true type in the header, and
// the additional data is the sequence number, the type, the version and
// the plaintext length. AES-GCM writes its explicit nonce — the sequence
// number, by the convention of RFC 5288 — before the ciphertext.
Bytes aad12(std::uint64_t sequence, std::uint8_t type, std::size_t length)
{
    Bytes aad;
    put64(aad, sequence);
    put8(aad, type);
    put16(aad, 0x0303);
    put16(aad, static_cast<std::uint16_t>(length));
    return aad;
}

Bytes seal_record12(TrafficKeys& keys, std::uint8_t type, View content)
{
    Bytes const aad = aad12(keys.sequence, type, content.size());
    Nonce nonce {};
    Bytes explicit_part;
    if (keys.aead == Aead::Aes128Gcm) {
        put64(explicit_part, keys.sequence);
        nonce = keys.nonce12(explicit_part);
    } else {
        nonce = keys.nonce();
    }
    Bytes record;
    put8(record, type);
    put16(record, 0x0303);
    put16(record, static_cast<std::uint16_t>(explicit_part.size() + content.size() + 16));
    put_bytes(record, explicit_part);
    Bytes ciphertext(content.size());
    Tag const tag = aead_seal(keys, nonce, aad, content, ciphertext);
    keys.sequence += 1;
    put_bytes(record, ciphertext);
    put_bytes(record, tag);
    return record;
}

bool open_record12(TrafficKeys& keys, std::uint8_t type, View body, Bytes& plaintext)
{
    std::size_t const explicit_size = keys.aead == Aead::Aes128Gcm ? 8 : 0;
    if (body.size() < explicit_size + 16)
        return false;
    Nonce const nonce = explicit_size != 0 ? keys.nonce12(body.subspan(0, 8)) : keys.nonce();
    View const ciphertext = body.subspan(explicit_size, body.size() - explicit_size - 16);
    Tag tag;
    std::copy(body.end() - 16, body.end(), tag.begin());
    Bytes const aad = aad12(keys.sequence, type, ciphertext.size());
    plaintext.assign(ciphertext.size(), 0);
    if (!aead_open(keys, nonce, aad, ciphertext, tag, plaintext))
        return false;
    keys.sequence += 1;
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
    std::vector<CipherSuite> offered_suites;
    CipherSuite negotiated_suite = CipherSuite::ChaCha20Poly1305Sha256;
    int retries = 0; // the retry requests seen; §4.1.4 allows one
    Bytes cookie; // the server's cookie, echoed in the second hello
    bool share_sent = true; // whether the hello carried a key share at all
    std::uint16_t share_group = group_x25519;
    TrafficKeys read_keys;
    TrafficKeys write_keys;
    Bytes shared_secret; // the ECDH output: 1.3's input to the schedule, 1.2's pre-master secret
    Bytes hash_before_certificate_verify;
    bool certificate_requested = false;
    Bytes certificate_request_context;
    PublicKey leaf_key;
    // TLS 1.2: chosen once a ServerHello carries no supported_versions.
    bool tls12 = false;
    std::array<std::uint8_t, 32> server_random {};
    bool extended_master_secret = false;
    std::uint16_t kx_group = 0; // the group of the server's ephemeral key
    TrafficKeys pending_read_keys; // the server's, from its ChangeCipherSpec on

    explicit Impl(TlsConfig c)
        : config(std::move(c))
    {
        offered_suites = config.cipher_suites;
        if (offered_suites.empty()) {
            if (config.offer_tls13)
                offered_suites.insert(offered_suites.end(), std::begin(default_suites13), std::end(default_suites13));
            if (config.offer_tls12)
                offered_suites.insert(offered_suites.end(), std::begin(default_suites12), std::end(default_suites12));
        }
        share_sent = !config.empty_key_share;
    }

    // The ECDH of the group in hand: x25519 as RFC 7748 §6.1, with a
    // low-order point refused; secp256r1 as RFC 8446 §7.4.2 and RFC 8422
    // §5.7, the x-coordinate of the product. The output is 1.3's shared
    // secret and 1.2's pre-master secret alike.
    bool derive_shared_secret(std::uint16_t group, View peer)
    {
        if (group == group_x25519) {
            if (peer.size() != 32)
                return false;
            crypto::X25519Key server_key {};
            std::copy(peer.begin(), peer.end(), server_key.begin());
            crypto::X25519Key shared {};
            if (!crypto::x25519(shared, config.private_key, server_key))
                return false;
            shared_secret.assign(shared.begin(), shared.end());
            return true;
        }
        if (group == group_secp256r1) {
            std::optional<crypto::EcPoint> const point = crypto::ec_decode_point(crypto::CurveId::P256, peer);
            if (!point)
                return false;
            std::optional<Bytes> const x = crypto::ecdh_shared_x(crypto::CurveId::P256, config.p256_private_key, *point);
            if (!x)
                return false;
            shared_secret = *x;
            return true;
        }
        return false;
    }

    // Our public key for the group, in the form the wire takes.
    Bytes own_public_key(std::uint16_t group) const
    {
        Bytes out;
        if (group == group_secp256r1) {
            std::optional<crypto::EcPoint> const point = crypto::ec_public_point(crypto::CurveId::P256, config.p256_private_key);
            if (point)
                out = crypto::ec_encode_point(crypto::CurveId::P256, *point);
        } else {
            crypto::X25519Key const raw = crypto::x25519_public(config.private_key);
            out.assign(raw.begin(), raw.end());
        }
        return out;
    }

    Hash::Digest transcript_hash() const
    {
        Hash copy = transcript;
        return copy.finish();
    }

    bool was_offered(std::uint16_t suite) const
    {
        for (CipherSuite const candidate : offered_suites)
            if (static_cast<std::uint16_t>(candidate) == suite)
                return true;
        return false;
    }

    // §4.2: no extension type appears twice in one block. One instance per
    // block being read; a repeat is illegal_parameter.
    struct ExtensionBlock {
        std::vector<std::uint16_t> seen;
        bool repeats(std::uint16_t type)
        {
            if (std::find(seen.begin(), seen.end(), type) != seen.end())
                return true;
            seen.push_back(type);
            return false;
        }
    };

    // The extension types this client knows, whether or not it offered them.
    static bool recognized_extension(std::uint16_t type)
    {
        switch (type) {
        case ext_server_name:
        case ext_supported_groups:
        case ext_ec_point_formats:
        case ext_signature_algorithms:
        case ext_alpn:
        case ext_extended_master_secret:
        case ext_pre_shared_key:
        case ext_early_data:
        case ext_supported_versions:
        case ext_cookie:
        case ext_psk_key_exchange_modes:
        case ext_key_share:
        case ext_renegotiation_info:
            return true;
        default:
            return false;
        }
    }

    // §4.2 names two faults for an extension that does not belong: one the
    // client recognizes but that is not specified for the message it came in
    // is illegal_parameter; one the client never asked for is
    // unsupported_extension.
    void refuse_extension(TlsOutput& out, std::uint16_t type, char const* message)
    {
        if (recognized_extension(type))
            fail(out, illegal_parameter, std::string(message) + " carried an extension that has no place in it");
        else
            fail(out, unsupported_extension, std::string(message) + " carried an extension that was not offered");
    }

    // A fatal alert under whatever keys are current, and the failed state.
    void fail(TlsOutput& out, std::uint8_t description, std::string reason)
    {
        if (state == TlsState::Failed)
            return;
        std::uint8_t const body[2] = { 2, description };
        if (write_keys.active)
            put_bytes(out.to_send, tls12 ? seal_record12(write_keys, alert, body) : seal_record(write_keys, alert, body));
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
        put16(body, static_cast<std::uint16_t>(2 * offered_suites.size()));
        for (CipherSuite const suite : offered_suites)
            put16(body, static_cast<std::uint16_t>(suite));
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
            put16(groups, static_cast<std::uint16_t>(2 * std::size(offered_groups)));
            for (std::uint16_t const group : offered_groups)
                put16(groups, group);
            extension(ext_supported_groups, groups);
        }
        if (config.offer_tls12) {
            // RFC 8422 §5.1.2: uncompressed points, the only form written.
            Bytes formats;
            put8(formats, 1);
            put8(formats, 0);
            extension(ext_ec_point_formats, formats);
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
            put8(versions, static_cast<std::uint8_t>(2 * ((config.offer_tls13 ? 1 : 0) + (config.offer_tls12 ? 1 : 0))));
            if (config.offer_tls13)
                put16(versions, version_tls13);
            if (config.offer_tls12)
                put16(versions, version_tls12);
            extension(ext_supported_versions, versions);
        }
        if (config.offer_tls13) {
            Bytes modes;
            put8(modes, 1);
            put8(modes, 1); // psk_dhe_ke
            extension(ext_psk_key_exchange_modes, modes);
        }
        if (config.offer_tls13) {
            // §4.2.8: a share for the group in hand, or an empty list, which
            // asks the server to name the group it wants instead.
            Bytes shares;
            if (share_sent) {
                Bytes const public_key = own_public_key(share_group);
                put16(shares, static_cast<std::uint16_t>(public_key.size() + 4));
                put16(shares, share_group);
                put16(shares, static_cast<std::uint16_t>(public_key.size()));
                put_bytes(shares, public_key);
            } else {
                put16(shares, 0);
            }
            extension(ext_key_share, shares);
        }
        if (config.offer_tls12) {
            // RFC 7627: the master secret bound to the whole handshake; and
            // RFC 5746 §3.2: the empty renegotiation_info of a first handshake.
            extension(ext_extended_master_secret, {});
            std::uint8_t const none = 0;
            extension(ext_renegotiation_info, View(&none, 1));
        }
        {
            Bytes alpn_list;
            std::string const http11 = "http/1.1";
            put16(alpn_list, static_cast<std::uint16_t>(http11.size() + 1));
            put8(alpn_list, static_cast<std::uint8_t>(http11.size()));
            alpn_list.insert(alpn_list.end(), http11.begin(), http11.end());
            extension(ext_alpn, alpn_list);
        }
        if (!cookie.empty()) {
            // §4.2.2: a cookie comes back exactly as the retry request gave it.
            Bytes echoed;
            put16(echoed, static_cast<std::uint16_t>(cookie.size()));
            put_bytes(echoed, cookie);
            extension(ext_cookie, echoed);
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

    // §4.1.4: a ServerHello whose random is the special value is a request
    // for another hello rather than a hello. The first one stands in the
    // transcript from here on as a synthetic message_hash message (§4.4.1),
    // so both sides go on hashing the same bytes; the second hello carries
    // the group the server named and the cookie it sent, and nothing else
    // changes.
    bool on_hello_retry_request(View raw_message, View extensions, std::uint16_t suite, TlsOutput& out)
    {
        if (retries > 0) {
            fail(out, unexpected_message, "the server asked for a second hello retry");
            return false;
        }
        bool saw_version = false;
        bool saw_group = false;
        std::uint16_t selected_group = 0;
        Bytes new_cookie;
        ExtensionBlock block;
        Reader e { extensions };
        while (!e.done()) {
            std::uint16_t type = 0;
            View data;
            if (!e.u16(type) || !e.vector16(data)) {
                fail(out, decode_error, "a hello retry request did not decode");
                return false;
            }
            if (block.repeats(type)) {
                fail(out, illegal_parameter, "a hello retry request carried an extension twice");
                return false;
            }
            Reader d { data };
            if (type == ext_supported_versions) {
                std::uint16_t chosen = 0;
                if (!d.u16(chosen) || !d.done() || chosen != version_tls13) {
                    fail(out, illegal_parameter, "a hello retry request named a version other than TLS 1.3");
                    return false;
                }
                saw_version = true;
            } else if (type == ext_key_share) {
                if (!d.u16(selected_group) || !d.done()) {
                    fail(out, decode_error, "a hello retry request's key share did not decode");
                    return false;
                }
                saw_group = true;
            } else if (type == ext_cookie) {
                View value;
                if (!d.vector16(value) || !d.done() || value.empty()) {
                    fail(out, decode_error, "a hello retry request's cookie did not decode");
                    return false;
                }
                new_cookie.assign(value.begin(), value.end());
            } else {
                refuse_extension(out, type, "a hello retry request");
                return false;
            }
        }
        if (!saw_version) {
            fail(out, missing_extension, "a hello retry request did not negotiate TLS 1.3");
            return false;
        }
        // A retry that would leave the hello exactly as it was is forbidden.
        if (!saw_group && new_cookie.empty()) {
            fail(out, illegal_parameter, "a hello retry request asked for no change");
            return false;
        }
        if (saw_group) {
            // §4.2.8: the group has to be one this client offered and did not
            // already send a share for.
            bool const known = std::find(std::begin(offered_groups), std::end(offered_groups), selected_group) != std::end(offered_groups);
            if (!known) {
                fail(out, illegal_parameter, "the server asked for a key-share group that was not offered");
                return false;
            }
            if (share_sent && selected_group == share_group) {
                fail(out, illegal_parameter, "the server asked again for the group it already had a share for");
                return false;
            }
            share_group = selected_group;
            share_sent = true;
        }
        if (!share_sent) {
            fail(out, illegal_parameter, "the server asked for another hello without naming a group");
            return false;
        }
        Hash::Digest const first_hello = transcript_hash();
        Bytes synthetic;
        put8(synthetic, message_hash);
        put24(synthetic, static_cast<std::uint32_t>(first_hello.size()));
        put_bytes(synthetic, first_hello);
        transcript = Hash();
        transcript.update(synthetic);
        transcript.update(raw_message);
        retries += 1;
        negotiated_suite = static_cast<CipherSuite>(suite);
        cookie = std::move(new_cookie);
        // §D.4: the dummy record goes out before the second flight.
        if (config.compatibility_mode) {
            std::uint8_t const one = 1;
            put_bytes(out.to_send, plain_record(change_cipher_spec, View(&one, 1)));
        }
        put_bytes(out.to_send, send_client_hello(build_client_hello()));
        return true;
    }

    bool on_server_hello(View body, View raw_message, TlsOutput& out)
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
        if (version != 0x0303 || compression != 0) {
            fail(out, illegal_parameter, "ServerHello carried a legacy version or a compression method");
            return false;
        }
        if (!was_offered(suite)) {
            fail(out, illegal_parameter, "the server chose a cipher suite that was not offered");
            return false;
        }
        // §4.1.3: the special random makes this a HelloRetryRequest, which is
        // 1.3's alone and must say so — before any question of 1.2 arises.
        if (std::equal(random.begin(), random.end(), hello_retry_request_random)) {
            if (!std::equal(session_id.begin(), session_id.end(), session_id_sent.begin(), session_id_sent.end())) {
                fail(out, illegal_parameter, "ServerHello did not echo the session id");
                return false;
            }
            return on_hello_retry_request(raw_message, extensions, suite, out);
        }
        // A server speaking 1.3 says so in supported_versions; one that sends
        // none has answered the 1.2 half of the hello.
        bool has_supported_versions = false;
        {
            Reader scan { extensions };
            while (!scan.done()) {
                std::uint16_t type = 0;
                View data;
                if (!scan.u16(type) || !scan.vector16(data)) {
                    fail(out, decode_error, "ServerHello extensions did not decode");
                    return false;
                }
                if (type == ext_supported_versions)
                    has_supported_versions = true;
            }
        }
        if (!has_supported_versions) {
            if (!config.offer_tls12) {
                fail(out, protocol_version, "the server did not negotiate TLS 1.3");
                return false;
            }
            return on_server_hello_12(random, suite, extensions, raw_message, out);
        }
        if (!std::equal(session_id.begin(), session_id.end(), session_id_sent.begin(), session_id_sent.end())) {
            fail(out, illegal_parameter, "ServerHello did not echo the session id");
            return false;
        }
        if (is_tls12_suite(static_cast<CipherSuite>(suite))) {
            fail(out, illegal_parameter, "the server chose a TLS 1.2 suite for TLS 1.3");
            return false;
        }
        // §4.1.4: the hello that follows a retry must keep the suite it named.
        if (retries > 0 && static_cast<std::uint16_t>(negotiated_suite) != suite) {
            fail(out, illegal_parameter, "the server changed cipher suite after asking for another hello");
            return false;
        }
        negotiated_suite = static_cast<CipherSuite>(suite);
        transcript.update(raw_message);
        bool saw_version = false;
        bool saw_key_share = false;
        ExtensionBlock block;
        Reader e { extensions };
        while (!e.done()) {
            std::uint16_t type = 0;
            View data;
            if (!e.u16(type) || !e.vector16(data)) {
                fail(out, decode_error, "ServerHello extensions did not decode");
                return false;
            }
            if (block.repeats(type)) {
                fail(out, illegal_parameter, "ServerHello carried an extension twice");
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
                // §4.2.8: a hello that carried no share asked the server to
                // name its group, and a retry request is the only answer
                // that can; a plain hello with a share is out of order.
                if (!share_sent) {
                    fail(out, illegal_parameter, "the server answered a hello without a key share with a key share instead of a retry request");
                    return false;
                }
                std::uint16_t group = 0;
                View key;
                if (!d.u16(group) || !d.vector16(key) || !d.done() || group != share_group) {
                    fail(out, illegal_parameter, "the server's key share is not for the group offered");
                    return false;
                }
                if (!derive_shared_secret(group, key)) {
                    fail(out, illegal_parameter, "the server's key share is not a valid point");
                    return false;
                }
                saw_key_share = true;
            } else {
                refuse_extension(out, type, "ServerHello");
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
        Bytes const zeros(Hash::digest_size, 0);
        Bytes const early = hkdf_extract(zeros, zeros);
        Hash::Digest const empty_hash = Hash::hash({});
        Bytes const derived = derive_secret(early, "derived", empty_hash);
        secrets.handshake_secret = hkdf_extract(derived, shared_secret);
        Hash::Digest const hello_hash = transcript_hash();
        secrets.client_handshake_traffic = derive_secret(secrets.handshake_secret, "c hs traffic", hello_hash);
        secrets.server_handshake_traffic = derive_secret(secrets.handshake_secret, "s hs traffic", hello_hash);
        read_keys = TrafficKeys::from_secret(negotiated_suite, secrets.server_handshake_traffic);
        write_keys = TrafficKeys::from_secret(negotiated_suite, secrets.client_handshake_traffic);
        state = TlsState::WaitEncryptedExtensions;
        return true;
    }

    // ProtocolNameList with exactly one name, one of ours.
    bool read_alpn(View data, TlsOutput& out)
    {
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
        return true;
    }

    // ---- TLS 1.2 (RFC 5246 §7.4, RFC 8422 §5, RFC 7627, RFC 5746)

    // A ServerHello with no supported_versions: the server has taken the
    // 1.2 half of the hello. Its extensions are the ones offered for 1.2,
    // its session id is its own business (no resumption is offered), and
    // a server that could have spoken 1.3 marks a 1.2 random with the
    // downgrade sentinel of RFC 8446 §4.1.3, which is refused.
    bool on_server_hello_12(View random, std::uint16_t suite, View extensions, View raw_message, TlsOutput& out)
    {
        if (!is_tls12_suite(static_cast<CipherSuite>(suite))) {
            fail(out, illegal_parameter, "the server chose a TLS 1.3 suite for TLS 1.2");
            return false;
        }
        if (retries > 0) {
            fail(out, illegal_parameter, "the server negotiated TLS 1.2 after asking for another hello");
            return false;
        }
        View const tail = random.subspan(24, 8);
        if ((config.offer_tls13 && std::equal(tail.begin(), tail.end(), downgrade_tls12)) || std::equal(tail.begin(), tail.end(), downgrade_tls11)) {
            fail(out, illegal_parameter, "the server's random carries a downgrade sentinel");
            return false;
        }
        tls12 = true;
        negotiated_suite = static_cast<CipherSuite>(suite);
        std::copy(random.begin(), random.end(), server_random.begin());
        transcript.update(raw_message);
        ExtensionBlock block;
        Reader e { extensions };
        while (!e.done()) {
            std::uint16_t type = 0;
            View data;
            if (!e.u16(type) || !e.vector16(data)) {
                fail(out, decode_error, "ServerHello extensions did not decode");
                return false;
            }
            if (block.repeats(type)) {
                fail(out, illegal_parameter, "ServerHello carried an extension twice");
                return false;
            }
            Reader d { data };
            if (type == ext_renegotiation_info) {
                // RFC 5746 §3.4: a first handshake's renegotiated_connection is empty.
                std::uint8_t length = 0;
                if (!d.u8(length) || length != 0 || !d.done()) {
                    fail(out, handshake_failure, "renegotiation_info was not the empty one of a first handshake");
                    return false;
                }
            } else if (type == ext_extended_master_secret) {
                if (!d.done()) {
                    fail(out, decode_error, "extended_master_secret did not decode");
                    return false;
                }
                extended_master_secret = true;
            } else if (type == ext_ec_point_formats) {
                View formats;
                if (!d.vector8(formats) || !d.done() || std::find(formats.begin(), formats.end(), 0) == formats.end()) {
                    fail(out, illegal_parameter, "the server's point formats leave out the uncompressed one");
                    return false;
                }
            } else if (type == ext_alpn) {
                if (!read_alpn(data, out))
                    return false;
            } else if (type == ext_server_name && !config.server_name.empty()) {
                // The empty acknowledgement.
            } else {
                refuse_extension(out, type, "ServerHello");
                return false;
            }
        }
        state = TlsState::WaitCertificate;
        return true;
    }

    // RFC 5246 §7.4.2: the chain, leaf first, with nothing per certificate.
    bool on_certificate_12(View body, TlsOutput& out)
    {
        Reader r { body };
        View list;
        if (!r.vector24(list) || !r.done()) {
            fail(out, decode_error, "Certificate did not decode");
            return false;
        }
        Reader entries { list };
        while (!entries.done()) {
            View cert_data;
            if (!entries.vector24(cert_data)) {
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
        // The suite names how the server proves itself; the key has to fit.
        bool const wants_ecdsa = suite_wants_ecdsa(negotiated_suite);
        if ((wants_ecdsa && leaf_key.kind != PublicKey::Kind::Ec) || (!wants_ecdsa && leaf_key.kind != PublicKey::Kind::Rsa)) {
            fail(out, illegal_parameter, "the server's certificate key does not fit the cipher suite");
            return false;
        }
        state = TlsState::WaitServerKeyExchange;
        return true;
    }

    // RFC 5246 §7.4.1.4.1 read with RFC 8446 §4.2.3's code points: the hash
    // and the kind of a signature, which must be the leaf key's kind; in
    // 1.2 an ECDSA scheme carries no curve, the key's own is used.
    static bool algorithm_for(std::uint16_t scheme, PublicKey const& key, SignatureAlgorithm& algorithm)
    {
        auto hash_of = [](std::uint8_t code, crypto::HashId& id) {
            switch (code) {
            case 4:
                id = crypto::HashId::Sha256;
                return true;
            case 5:
                id = crypto::HashId::Sha384;
                return true;
            case 6:
                id = crypto::HashId::Sha512;
                return true;
            default:
                return false;
            }
        };
        if (scheme == 0x0804 || scheme == 0x0805 || scheme == 0x0806) {
            if (key.kind != PublicKey::Kind::Rsa)
                return false;
            algorithm.kind = SignatureKind::RsaPss;
            algorithm.hash = scheme == 0x0804 ? crypto::HashId::Sha256 : scheme == 0x0805 ? crypto::HashId::Sha384 : crypto::HashId::Sha512;
            algorithm.pss_salt_length = crypto::digest_size(algorithm.hash);
            return true;
        }
        std::uint8_t const hash = static_cast<std::uint8_t>(scheme >> 8);
        std::uint8_t const kind = static_cast<std::uint8_t>(scheme & 0xff);
        if (kind == 1 && key.kind == PublicKey::Kind::Rsa) {
            algorithm.kind = SignatureKind::RsaPkcs1;
            return hash_of(hash, algorithm.hash);
        }
        if (kind == 3 && key.kind == PublicKey::Kind::Ec) {
            algorithm.kind = SignatureKind::Ecdsa;
            return hash_of(hash, algorithm.hash);
        }
        return false;
    }

    // RFC 8422 §5.4: the server's ephemeral key for a named group and its
    // signature, under the leaf's key, over both randoms and the
    // parameters. The shared secret is made here.
    bool on_server_key_exchange(View body, TlsOutput& out)
    {
        Reader r { body };
        std::uint8_t curve_type = 0;
        std::uint16_t group = 0;
        View point;
        if (!r.u8(curve_type) || curve_type != 3 || !r.u16(group) || !r.vector8(point)) {
            fail(out, decode_error, "ServerKeyExchange did not decode");
            return false;
        }
        std::size_t const params_end = r.offset;
        std::uint16_t scheme = 0;
        View signature;
        if (!r.u16(scheme) || !r.vector16(signature) || !r.done()) {
            fail(out, decode_error, "ServerKeyExchange did not decode");
            return false;
        }
        if (std::find(std::begin(offered_groups), std::end(offered_groups), group) == std::end(offered_groups)) {
            fail(out, illegal_parameter, "the server's key exchange uses a group that was not offered");
            return false;
        }
        SignatureAlgorithm algorithm;
        if (!algorithm_for(scheme, leaf_key, algorithm)) {
            fail(out, illegal_parameter, "the server's key exchange is signed with a scheme that does not fit its key");
            return false;
        }
        Bytes signed_data;
        put_bytes(signed_data, config.client_random);
        put_bytes(signed_data, server_random);
        put_bytes(signed_data, body.subspan(0, params_end));
        if (!verify_signature(leaf_key, algorithm, signed_data, signature)) {
            fail(out, decrypt_error, "the server's signature over its key exchange did not verify");
            return false;
        }
        if (!derive_shared_secret(group, point)) {
            fail(out, illegal_parameter, "the server's ephemeral key is not a valid point");
            return false;
        }
        kx_group = group;
        state = TlsState::WaitServerHelloDone;
        return true;
    }

    // RFC 5246 §7.4.4: read for its shape only; an empty certificate answers it.
    bool on_certificate_request_12(View body, TlsOutput& out)
    {
        Reader r { body };
        View types;
        View algorithms;
        View authorities;
        if (!r.vector8(types) || !r.vector16(algorithms) || !r.vector16(authorities) || !r.done()) {
            fail(out, decode_error, "CertificateRequest did not decode");
            return false;
        }
        certificate_requested = true;
        return true;
    }

    // RFC 5246 §7.4.5: the server's flight is over. The chain is judged,
    // then our own flight goes out — the empty certificate if one was
    // asked for, the key exchange, the ChangeCipherSpec that turns our
    // keys on, and the Finished under them — with the master secret and
    // the key block derived between (§8.1, RFC 7627 §4, §6.3).
    bool on_server_hello_done(View body, TlsOutput& out)
    {
        if (!body.empty()) {
            fail(out, decode_error, "ServerHelloDone did not decode");
            return false;
        }
        std::string reason;
        if (config.verify_chain && !config.verify_chain(chain, reason)) {
            fail(out, bad_certificate, reason.empty() ? "certificate validation failed" : reason);
            return false;
        }
        Bytes flight;
        if (certificate_requested) {
            Bytes empty_list;
            put24(empty_list, 0);
            Bytes const message = handshake_message(certificate, empty_list);
            transcript.update(message);
            put_bytes(flight, message);
        }
        Bytes const public_key = own_public_key(kx_group);
        if (public_key.empty()) {
            fail(out, internal_error, "no ephemeral key could be made for the group");
            return false;
        }
        Bytes exchange;
        put8(exchange, static_cast<std::uint8_t>(public_key.size()));
        put_bytes(exchange, public_key);
        Bytes const exchange_message = handshake_message(client_key_exchange, exchange);
        transcript.update(exchange_message);
        put_bytes(flight, exchange_message);

        if (extended_master_secret) {
            Hash::Digest const session_hash = transcript_hash();
            secrets.master_secret = prf12(shared_secret, "extended master secret", session_hash, 48);
        } else {
            Bytes randoms;
            put_bytes(randoms, config.client_random);
            put_bytes(randoms, server_random);
            secrets.master_secret = prf12(shared_secret, "master secret", randoms, 48);
        }
        std::fill(shared_secret.begin(), shared_secret.end(), 0);
        Aead const aead = aead_of(negotiated_suite);
        std::size_t const key_size = aead == Aead::Aes128Gcm ? 16 : 32;
        std::size_t const iv_size = TrafficKeys::iv_size12(aead);
        Bytes randoms;
        put_bytes(randoms, server_random);
        put_bytes(randoms, config.client_random);
        Bytes const block = prf12(secrets.master_secret, "key expansion", randoms, 2 * key_size + 2 * iv_size);
        View const whole(block);
        write_keys = TrafficKeys::from_block(aead, whole.subspan(0, key_size), whole.subspan(2 * key_size, iv_size));
        pending_read_keys = TrafficKeys::from_block(aead, whole.subspan(key_size, key_size), whole.subspan(2 * key_size + iv_size, iv_size));

        put_bytes(out.to_send, plain_record(handshake, flight));
        std::uint8_t const one = 1;
        put_bytes(out.to_send, plain_record(change_cipher_spec, View(&one, 1)));
        write_keys.active = true;
        Bytes const verify_data = prf12(secrets.master_secret, "client finished", transcript_hash(), 12);
        Bytes const finished_message = handshake_message(finished, verify_data);
        transcript.update(finished_message);
        put_bytes(out.to_send, seal_record12(write_keys, handshake, finished_message));
        state = TlsState::WaitFinished;
        return true;
    }

    // RFC 5246 §7.4.9: the server's Finished over the whole handshake, ours
    // included, under the keys its ChangeCipherSpec turned on.
    bool on_server_finished_12(View body, View raw_message, TlsOutput& out)
    {
        if (!read_keys.active) {
            fail(out, unexpected_message, "the server's Finished arrived before its ChangeCipherSpec");
            return false;
        }
        Bytes const expected = prf12(secrets.master_secret, "server finished", transcript_hash(), 12);
        if (body.size() != expected.size() || !crypto::constant_time_equal(body, expected)) {
            fail(out, decrypt_error, "the server's Finished did not verify");
            return false;
        }
        transcript.update(raw_message);
        state = TlsState::Connected;
        return true;
    }

    bool on_handshake_message_12(std::uint8_t type, View body, View raw_message, TlsOutput& out)
    {
        switch (state) {
        case TlsState::WaitCertificate:
            if (type != certificate)
                break;
            transcript.update(raw_message);
            return on_certificate_12(body, out);
        case TlsState::WaitServerKeyExchange:
            if (type != server_key_exchange)
                break;
            transcript.update(raw_message);
            return on_server_key_exchange(body, out);
        case TlsState::WaitServerHelloDone:
            if (type == certificate_request) {
                transcript.update(raw_message);
                return on_certificate_request_12(body, out);
            }
            if (type != server_hello_done)
                break;
            transcript.update(raw_message);
            return on_server_hello_done(body, out);
        case TlsState::WaitFinished:
            if (type != finished)
                break;
            return on_server_finished_12(body, raw_message, out);
        case TlsState::Connected:
            // §7.4.1.1: a request to renegotiate may be ignored, and is;
            // a ticket (RFC 5077) was never asked for and is ignored too.
            if (type == hello_request || type == new_session_ticket)
                return true;
            break;
        default:
            break;
        }
        fail(out, unexpected_message, "a handshake message arrived out of order");
        return false;
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
        ExtensionBlock block;
        Reader e { extensions };
        while (!e.done()) {
            std::uint16_t type = 0;
            View data;
            if (!e.u16(type) || !e.vector16(data)) {
                fail(out, decode_error, "EncryptedExtensions did not decode");
                return false;
            }
            if (block.repeats(type)) {
                fail(out, illegal_parameter, "EncryptedExtensions carried an extension twice");
                return false;
            }
            if (type == ext_alpn) {
                if (!read_alpn(data, out))
                    return false;
            } else if ((type == ext_server_name && !config.server_name.empty()) || type == ext_supported_groups) {
                // The empty acknowledgement, and the server's groups: nothing to do.
            } else {
                refuse_extension(out, type, "EncryptedExtensions");
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

        read_keys = TrafficKeys::from_secret(negotiated_suite, secrets.server_application_traffic);
        write_keys = TrafficKeys::from_secret(negotiated_suite, secrets.client_application_traffic);
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
        read_keys = TrafficKeys::from_secret(negotiated_suite, secrets.server_application_traffic);
        if (body[0] == 1) {
            std::uint8_t const update_not_requested = 0;
            Bytes const message = handshake_message(key_update, View(&update_not_requested, 1));
            put_bytes(out.to_send, seal_record(write_keys, handshake, message));
            secrets.client_application_traffic = hkdf_expand_label(secrets.client_application_traffic, "traffic upd", {}, Hash::digest_size);
            write_keys = TrafficKeys::from_secret(negotiated_suite, secrets.client_application_traffic);
        }
        return true;
    }

    bool on_handshake_message(std::uint8_t type, View body, View raw_message, TlsOutput& out)
    {
        if (tls12)
            return on_handshake_message_12(type, body, raw_message, out);
        switch (state) {
        case TlsState::WaitServerHello:
            if (type != server_hello)
                break;
            // The transcript is written inside: a retry request has to replace
            // the first hello with its hash before hashing itself (§4.4.1).
            return on_server_hello(body, raw_message, out);
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
            if (encrypted || fragment.size() != 1 || fragment[0] != 1 || state == TlsState::Connected) {
                fail(out, unexpected_message, "an unexpected ChangeCipherSpec");
                return false;
            }
            if (tls12) {
                // RFC 5246 §7.1: the server's keys turn on here, after its
                // flight and ours; no handshake message may straddle it.
                if (state != TlsState::WaitFinished || read_keys.active || !handshake_buffer.empty()) {
                    fail(out, unexpected_message, "an unexpected ChangeCipherSpec");
                    return false;
                }
                read_keys = pending_read_keys;
                read_keys.active = true;
                return true;
            }
            // §5: a compatibility-mode CCS between the hellos and the
            // server's Finished; anything else, or a bad body, is an error.
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
            std::size_t const limit = read_keys.active && tls12 ? max_ciphertext12
                : read_keys.active && type == application_data ? max_ciphertext
                                                                : max_plaintext;
            if (length > limit) {
                fail(out, record_overflow, "a record is larger than the protocol allows");
                break;
            }
            if (in.size() - offset - 5 < length)
                break;
            View const header(in.data() + offset, 5);
            View const record_body(in.data() + offset + 5, length);
            offset += 5 + length;
            if (read_keys.active && tls12) {
                // From the server's ChangeCipherSpec on every record is
                // protected and keeps its type in the header.
                Bytes plaintext;
                if (!open_record12(read_keys, type, record_body, plaintext)) {
                    fail(out, bad_record_mac, "a record failed to decrypt");
                    break;
                }
                if (!handle_record(type, plaintext, true, out))
                    break;
            } else if (read_keys.active && type == application_data) {
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
            View const piece = plaintext.subspan(offset, take);
            put_bytes(out, tls12 ? seal_record12(write_keys, application_data, piece) : seal_record(write_keys, application_data, piece));
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
    Bytes const record = m_impl->tls12 ? seal_record12(m_impl->write_keys, alert, body) : seal_record(m_impl->write_keys, alert, body);
    m_impl->state = TlsState::Closed;
    return record;
}

TlsState TlsEngine::state() const { return m_impl->state; }
std::uint16_t TlsEngine::version() const
{
    if (m_impl->tls12)
        return version_tls12;
    if (m_impl->state == TlsState::Start || m_impl->state == TlsState::WaitServerHello)
        return 0;
    return version_tls13;
}
std::string const& TlsEngine::error() const { return m_impl->error; }
std::vector<Certificate> const& TlsEngine::peer_chain() const { return m_impl->chain; }
std::string const& TlsEngine::alpn() const { return m_impl->alpn; }
CipherSuite TlsEngine::cipher_suite() const { return m_impl->negotiated_suite; }
int TlsEngine::hello_retry_requests() const { return m_impl->retries; }
TlsSecrets const& TlsEngine::secrets() const { return m_impl->secrets; }

char const* cipher_suite_name(CipherSuite suite)
{
    switch (suite) {
    case CipherSuite::Aes128GcmSha256:
        return "TLS_AES_128_GCM_SHA256";
    case CipherSuite::ChaCha20Poly1305Sha256:
        return "TLS_CHACHA20_POLY1305_SHA256";
    case CipherSuite::EcdheEcdsaAes128GcmSha256:
        return "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256";
    case CipherSuite::EcdheRsaAes128GcmSha256:
        return "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256";
    case CipherSuite::EcdheRsaChaCha20Poly1305Sha256:
        return "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256";
    case CipherSuite::EcdheEcdsaChaCha20Poly1305Sha256:
        return "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256";
    }
    return "TLS_CHACHA20_POLY1305_SHA256";
}

}
