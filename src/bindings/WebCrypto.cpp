#include "bindings/Internal.h"

// Web Crypto (W3C Web Cryptography API, the 2024 Level 2 draft as the
// browsers ship it): CryptoKey, and SubtleCrypto's twelve operations over
// the algorithms every engine registers — RSASSA-PKCS1-v1_5, RSA-PSS,
// RSA-OAEP, ECDSA and ECDH on P-256, P-384 and P-521, Ed25519 and X25519, AES-CTR,
// AES-CBC, AES-GCM and AES-KW, HMAC, SHA-1 and SHA-2, HKDF and PBKDF2 — with
// the key formats raw, SPKI, PKCS #8 and JWK. The arithmetic is src/crypto's,
// the same code TLS runs.
//
// Each operation does its work at once and settles its promise before it
// returns, where the specification runs the work "in parallel" and queues
// the settling: a page cannot tell the two apart except by timing, and the
// one slow step, an RSA key's generation, takes tens of milliseconds at the
// sizes pages ask for.
//
// The steps and the errors are the specification's, in its order: argument
// conversion (a TypeError), algorithm normalization (§18.4: NotSupportedError
// for an unknown name, a TypeError for a dictionary member that is missing
// or out of range), then the key's algorithm and usages (InvalidAccessError),
// then the algorithm's own checks (SyntaxError for usages it cannot have,
// DataError for key data it cannot read, OperationError for anything the
// operation itself refuses).

#include "core/Ascii.h"
#include "crypto/Aes.h"
#include "crypto/AesGcm.h"
#include "crypto/AesModes.h"
#include "crypto/BigInt.h"
#include "crypto/Ec.h"
#include "crypto/Ed25519.h"
#include "crypto/Hkdf.h"
#include "crypto/Hmac.h"
#include "crypto/Rsa.h"
#include "crypto/Sha1.h"
#include "crypto/Sha2.h"
#include "crypto/X25519.h"
#include "net/tls/Der.h"
#include "platform/Random.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sashfold::bindings {

namespace {

using Bytes = std::vector<std::uint8_t>;
using View = std::span<std::uint8_t const>;

// ---- what a key is

enum class Alg : std::uint8_t {
    RsassaPkcs1,
    RsaPss,
    RsaOaep,
    Ecdsa,
    Ecdh,
    Ed25519,
    X25519,
    AesCtr,
    AesCbc,
    AesGcm,
    AesKw,
    Hmac,
    Hkdf,
    Pbkdf2,
    Sha1,
    Sha256,
    Sha384,
    Sha512,
};

std::string_view alg_name(Alg alg)
{
    switch (alg) {
    case Alg::RsassaPkcs1:
        return "RSASSA-PKCS1-v1_5";
    case Alg::RsaPss:
        return "RSA-PSS";
    case Alg::RsaOaep:
        return "RSA-OAEP";
    case Alg::Ecdsa:
        return "ECDSA";
    case Alg::Ecdh:
        return "ECDH";
    case Alg::Ed25519:
        return "Ed25519";
    case Alg::X25519:
        return "X25519";
    case Alg::AesCtr:
        return "AES-CTR";
    case Alg::AesCbc:
        return "AES-CBC";
    case Alg::AesGcm:
        return "AES-GCM";
    case Alg::AesKw:
        return "AES-KW";
    case Alg::Hmac:
        return "HMAC";
    case Alg::Hkdf:
        return "HKDF";
    case Alg::Pbkdf2:
        return "PBKDF2";
    case Alg::Sha1:
        return "SHA-1";
    case Alg::Sha256:
        return "SHA-256";
    case Alg::Sha384:
        return "SHA-384";
    case Alg::Sha512:
        return "SHA-512";
    }
    return "";
}

bool is_rsa(Alg alg)
{
    return alg == Alg::RsassaPkcs1 || alg == Alg::RsaPss || alg == Alg::RsaOaep;
}

bool is_aes(Alg alg)
{
    return alg == Alg::AesCtr || alg == Alg::AesCbc || alg == Alg::AesGcm || alg == Alg::AesKw;
}

bool is_ec(Alg alg)
{
    return alg == Alg::Ecdsa || alg == Alg::Ecdh;
}

bool is_okp(Alg alg)
{
    return alg == Alg::Ed25519 || alg == Alg::X25519;
}

crypto::HashId hash_of(Alg alg)
{
    switch (alg) {
    case Alg::Sha1:
        return crypto::HashId::Sha1;
    case Alg::Sha384:
        return crypto::HashId::Sha384;
    case Alg::Sha512:
        return crypto::HashId::Sha512;
    default:
        return crypto::HashId::Sha256;
    }
}

std::string_view hash_name(crypto::HashId hash)
{
    switch (hash) {
    case crypto::HashId::Sha1:
        return "SHA-1";
    case crypto::HashId::Sha256:
        return "SHA-256";
    case crypto::HashId::Sha384:
        return "SHA-384";
    case crypto::HashId::Sha512:
        return "SHA-512";
    }
    return "";
}

// The recognized key usages, in the order the specification lists them —
// which is the order a key's usages are kept and reported in.
// The last four are ML-KEM's, which no algorithm here allows: a key asked
// for with one is refused as any other usage it cannot have.
constexpr std::array<std::string_view, 12> usage_names = { "encrypt", "decrypt", "sign", "verify", "deriveKey", "deriveBits", "wrapKey", "unwrapKey",
    "encapsulateKey", "encapsulateBits", "decapsulateKey", "decapsulateBits" };

namespace usage {
constexpr unsigned encrypt = 1u << 0;
constexpr unsigned decrypt = 1u << 1;
constexpr unsigned sign = 1u << 2;
constexpr unsigned verify = 1u << 3;
constexpr unsigned derive_key = 1u << 4;
constexpr unsigned derive_bits = 1u << 5;
constexpr unsigned wrap_key = 1u << 6;
constexpr unsigned unwrap_key = 1u << 7;
}

std::optional<unsigned> usage_bit(std::string_view name)
{
    for (std::size_t i = 0; i < usage_names.size(); ++i) {
        if (usage_names[i] == name)
            return 1u << i;
    }
    return std::nullopt;
}

enum class KeyType : std::uint8_t { Secret, Public, Private };

std::string_view key_type_name(KeyType type)
{
    switch (type) {
    case KeyType::Secret:
        return "secret";
    case KeyType::Public:
        return "public";
    case KeyType::Private:
        return "private";
    }
    return "";
}

// A key's [[algorithm]] and its material. Immutable once made: a clone of a
// key in another realm shares it.
struct KeyData {
    Alg alg = Alg::AesGcm;
    KeyType type = KeyType::Secret;
    // [[algorithm]]'s members beside the name.
    std::size_t length = 0; // AES and HMAC, in bits
    crypto::HashId hash = crypto::HashId::Sha256; // HMAC and RSA
    std::string named_curve; // EC
    // The material.
    Bytes secret; // AES, HMAC, HKDF, PBKDF2; the EC scalar; the Ed25519 seed or X25519 scalar
    crypto::RsaPublicKey rsa_public;
    std::optional<crypto::RsaPrivateKey> rsa_private;
    crypto::EcPoint ec_public;
    std::array<std::uint8_t, 32> okp_public {};
};

crypto::CurveId curve_id(std::string_view name)
{
    return name == "P-384" ? crypto::CurveId::P384 : name == "P-521" ? crypto::CurveId::P521 : crypto::CurveId::P256;
}

std::size_t curve_bytes(std::string_view name)
{
    return name == "P-384" ? 48 : name == "P-521" ? 66 : 32;
}

bool curve_supported(std::string_view name)
{
    return name == "P-256" || name == "P-384" || name == "P-521";
}

bool curve_recognized(std::string_view name)
{
    return curve_supported(name) || name == "P-521";
}

class CryptoKeyObject final : public js::Object {
public:
    CryptoKeyObject(js::Object* prototype, js::RealmRecord* realm, std::shared_ptr<KeyData const> the_data, bool is_extractable, unsigned key_usages)
        : Object(prototype, Class::Host)
        , data(std::move(the_data))
        , extractable(is_extractable)
        , usages(key_usages)
        , m_realm(realm)
    {
    }
    js::RealmRecord* home_realm() const override { return m_realm; }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(algorithm_cached);
        tracer.visit(usages_cached);
    }
    std::size_t size_in_bytes() const override { return sizeof(*this) + data->secret.size(); }

    std::shared_ptr<KeyData const> data;
    bool extractable;
    unsigned usages;
    js::Value algorithm_cached;
    js::Value usages_cached;

private:
    js::RealmRecord* m_realm;
};

CryptoKeyObject* as_key(js::Value const& value)
{
    return value.is_object() ? dynamic_cast<CryptoKeyObject*>(value.as_object()) : nullptr;
}

// ---- base64url (RFC 4648 §5), without padding as JWK writes it

std::string base64url_encode(View bytes)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        std::uint32_t const v = (std::uint32_t { bytes[i] } << 16) | (std::uint32_t { bytes[i + 1] } << 8) | bytes[i + 2];
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (bytes.size() - i == 1) {
        std::uint32_t const v = std::uint32_t { bytes[i] } << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
    } else if (bytes.size() - i == 2) {
        std::uint32_t const v = (std::uint32_t { bytes[i] } << 16) | (std::uint32_t { bytes[i + 1] } << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
    }
    return out;
}

// Padding is tolerated at the end only; any other character refuses.
std::optional<Bytes> base64url_decode(std::string_view text)
{
    while (!text.empty() && text.back() == '=')
        text.remove_suffix(1);
    if (text.size() % 4 == 1)
        return std::nullopt;
    Bytes out;
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (char const c : text) {
        int value;
        if (c >= 'A' && c <= 'Z')
            value = c - 'A';
        else if (c >= 'a' && c <= 'z')
            value = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            value = c - '0' + 52;
        else if (c == '-')
            value = 62;
        else if (c == '_')
            value = 63;
        else
            return std::nullopt;
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(accumulator >> bits));
        }
    }
    return out;
}

// ---- DER, written: just what SPKI and PKCS #8 need

void der_length(Bytes& out, std::size_t length)
{
    if (length < 0x80) {
        out.push_back(static_cast<std::uint8_t>(length));
        return;
    }
    Bytes digits;
    while (length > 0) {
        digits.insert(digits.begin(), static_cast<std::uint8_t>(length));
        length >>= 8;
    }
    out.push_back(static_cast<std::uint8_t>(0x80 | digits.size()));
    out.insert(out.end(), digits.begin(), digits.end());
}

Bytes der(std::uint8_t tag, View content)
{
    Bytes out { tag };
    der_length(out, content.size());
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

Bytes der_sequence(std::initializer_list<Bytes> parts)
{
    Bytes content;
    for (Bytes const& part : parts)
        content.insert(content.end(), part.begin(), part.end());
    return der(0x30, content);
}

// An INTEGER of a non-negative magnitude: a zero byte in front when the top
// bit is set, none of the leading zeros otherwise.
Bytes der_integer(View magnitude)
{
    std::size_t start = 0;
    while (start + 1 < magnitude.size() && magnitude[start] == 0)
        ++start;
    Bytes content;
    if (magnitude.empty()) {
        content.push_back(0);
    } else {
        if ((magnitude[start] & 0x80) != 0)
            content.push_back(0);
        content.insert(content.end(), magnitude.begin() + static_cast<std::ptrdiff_t>(start), magnitude.end());
    }
    return der(0x02, content);
}

Bytes der_integer(crypto::BigInt const& value)
{
    return der_integer(value.to_bytes());
}

Bytes der_small_integer(unsigned value)
{
    std::uint8_t const byte = static_cast<std::uint8_t>(value);
    return der_integer(View(&byte, 1));
}

Bytes der_oid(std::string_view dotted)
{
    std::vector<std::uint64_t> arcs;
    std::uint64_t current = 0;
    for (char const c : dotted) {
        if (c == '.') {
            arcs.push_back(current);
            current = 0;
        } else {
            current = current * 10 + static_cast<std::uint64_t>(c - '0');
        }
    }
    arcs.push_back(current);
    Bytes content;
    auto const base128 = [&](std::uint64_t value) {
        Bytes digits { static_cast<std::uint8_t>(value & 0x7f) };
        value >>= 7;
        while (value > 0) {
            digits.insert(digits.begin(), static_cast<std::uint8_t>(0x80 | (value & 0x7f)));
            value >>= 7;
        }
        content.insert(content.end(), digits.begin(), digits.end());
    };
    base128(arcs[0] * 40 + arcs[1]);
    for (std::size_t i = 2; i < arcs.size(); ++i)
        base128(arcs[i]);
    return der(0x06, content);
}

Bytes der_null()
{
    return Bytes { 0x05, 0x00 };
}

Bytes der_bit_string(View bytes)
{
    Bytes content { 0 };
    content.insert(content.end(), bytes.begin(), bytes.end());
    return der(0x03, content);
}

Bytes der_octet_string(View bytes)
{
    return der(0x04, bytes);
}

constexpr std::string_view oid_rsa = "1.2.840.113549.1.1.1";
constexpr std::string_view oid_ec_public_key = "1.2.840.10045.2.1";
constexpr std::string_view oid_ec_dh = "1.3.132.1.12";
constexpr std::string_view oid_p256 = "1.2.840.10045.3.1.7";
constexpr std::string_view oid_p384 = "1.3.132.0.34";
constexpr std::string_view oid_p521 = "1.3.132.0.35";
constexpr std::string_view oid_x25519 = "1.3.101.110";
constexpr std::string_view oid_ed25519 = "1.3.101.112";

std::string_view curve_oid(std::string_view curve)
{
    if (curve == "P-384")
        return oid_p384;
    if (curve == "P-521")
        return oid_p521;
    return oid_p256;
}

std::optional<std::string> curve_from_oid(std::string_view oid)
{
    if (oid == oid_p256)
        return std::string("P-256");
    if (oid == oid_p384)
        return std::string("P-384");
    if (oid == oid_p521)
        return std::string("P-521");
    return std::nullopt;
}

// SubjectPublicKeyInfo (RFC 5280 §4.1.2.7) and PrivateKeyInfo (RFC 5208 §5):
// the algorithm's OID, its parameters as their DER, and the key.
struct KeyInfo {
    std::string oid;
    std::optional<tls::DerElement> parameters;
    View key; // the BIT STRING's bytes, or the OCTET STRING's
};

std::optional<KeyInfo> read_algorithm(tls::DerReader& reader, KeyInfo& info)
{
    std::optional<tls::DerElement> const algorithm = reader.next(tls::DerType::Sequence);
    if (!algorithm)
        return std::nullopt;
    tls::DerReader inner(algorithm->content);
    std::optional<tls::DerElement> const oid = inner.next(tls::DerType::Oid);
    if (!oid)
        return std::nullopt;
    std::optional<std::string> const dotted = tls::der_oid(*oid);
    if (!dotted)
        return std::nullopt;
    info.oid = *dotted;
    if (!inner.at_end()) {
        info.parameters = inner.next();
        if (!info.parameters || !inner.at_end())
            return std::nullopt;
    }
    return info;
}

std::optional<KeyInfo> read_spki(View bytes)
{
    tls::DerReader outer(bytes);
    std::optional<tls::DerElement> const sequence = outer.next(tls::DerType::Sequence);
    if (!sequence || !outer.at_end())
        return std::nullopt;
    tls::DerReader reader(sequence->content);
    KeyInfo info;
    if (!read_algorithm(reader, info))
        return std::nullopt;
    std::optional<tls::DerElement> const key = reader.next(tls::DerType::BitString);
    if (!key || !reader.at_end())
        return std::nullopt;
    std::optional<View> const key_bytes = tls::der_bit_string(*key);
    if (!key_bytes)
        return std::nullopt;
    info.key = *key_bytes;
    return info;
}

std::optional<KeyInfo> read_pkcs8(View bytes)
{
    tls::DerReader outer(bytes);
    std::optional<tls::DerElement> const sequence = outer.next(tls::DerType::Sequence);
    if (!sequence || !outer.at_end())
        return std::nullopt;
    tls::DerReader reader(sequence->content);
    std::optional<tls::DerElement> const version = reader.next(tls::DerType::Integer);
    if (!version)
        return std::nullopt;
    std::optional<std::uint64_t> const number = tls::der_small_integer(*version);
    if (!number || *number > 1)
        return std::nullopt;
    KeyInfo info;
    if (!read_algorithm(reader, info))
        return std::nullopt;
    std::optional<tls::DerElement> const key = reader.next(tls::DerType::OctetString);
    if (!key)
        return std::nullopt;
    info.key = key->content;
    // Attributes [0] and, in a version 2 key, the public key [1] may follow.
    return info;
}

Bytes write_spki(Bytes algorithm, View key)
{
    return der_sequence({ std::move(algorithm), der_bit_string(key) });
}

Bytes write_pkcs8(Bytes algorithm, View key)
{
    return der_sequence({ der_small_integer(0), std::move(algorithm), der_octet_string(key) });
}

std::optional<crypto::BigInt> der_bigint(tls::DerReader& reader)
{
    std::optional<tls::DerElement> const element = reader.next(tls::DerType::Integer);
    if (!element)
        return std::nullopt;
    std::optional<View> const magnitude = tls::der_integer(*element);
    if (!magnitude)
        return std::nullopt;
    return crypto::BigInt::from_bytes(*magnitude);
}

void random_fill(std::span<std::uint8_t> out)
{
    platform::fill_random(out);
}

Bytes random_bytes(std::size_t size)
{
    Bytes out(size);
    if (size > 0)
        platform::fill_random(out);
    return out;
}

// PBKDF2 (RFC 8018 §5.2) over HMAC with the named hash.
template<typename Hash>
Bytes pbkdf2_with(View password, View salt, std::uint32_t iterations, std::size_t length)
{
    Bytes out;
    out.reserve(length);
    // The HMAC keyed once, its padded key blocks already absorbed: each
    // iteration starts from a copy of it rather than keying it again, which
    // halves the hashing (and spares a long password being hashed each time).
    crypto::Hmac<Hash> const keyed(password);
    for (std::uint32_t block = 1; out.size() < length; ++block) {
        crypto::Hmac<Hash> first = keyed;
        first.update(salt);
        std::uint8_t const index[4] = { static_cast<std::uint8_t>(block >> 24), static_cast<std::uint8_t>(block >> 16),
            static_cast<std::uint8_t>(block >> 8), static_cast<std::uint8_t>(block) };
        first.update(View(index, 4));
        typename Hash::Digest u = first.finish();
        typename Hash::Digest t = u;
        for (std::uint32_t i = 1; i < iterations; ++i) {
            crypto::Hmac<Hash> next = keyed;
            next.update(u);
            u = next.finish();
            for (std::size_t j = 0; j < t.size(); ++j)
                t[j] = static_cast<std::uint8_t>(t[j] ^ u[j]);
        }
        std::size_t const take = std::min(length - out.size(), t.size());
        out.insert(out.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return out;
}

Bytes pbkdf2(crypto::HashId hash, View password, View salt, std::uint32_t iterations, std::size_t length)
{
    switch (hash) {
    case crypto::HashId::Sha1:
        return pbkdf2_with<crypto::Sha1>(password, salt, iterations, length);
    case crypto::HashId::Sha256:
        return pbkdf2_with<crypto::Sha256>(password, salt, iterations, length);
    case crypto::HashId::Sha384:
        return pbkdf2_with<crypto::Sha384>(password, salt, iterations, length);
    case crypto::HashId::Sha512:
        return pbkdf2_with<crypto::Sha512>(password, salt, iterations, length);
    }
    return {};
}

std::optional<Bytes> hkdf(crypto::HashId hash, View ikm, View salt, View info, std::size_t length)
{
    auto const run = [&]<typename Hash>() -> std::optional<Bytes> {
        if (length > 255 * Hash::digest_size)
            return std::nullopt;
        typename Hash::Digest const prk = crypto::hkdf_extract<Hash>(salt, ikm);
        return crypto::hkdf_expand<Hash>(prk, info, length);
    };
    switch (hash) {
    case crypto::HashId::Sha1:
        return run.template operator()<crypto::Sha1>();
    case crypto::HashId::Sha256:
        return run.template operator()<crypto::Sha256>();
    case crypto::HashId::Sha384:
        return run.template operator()<crypto::Sha384>();
    case crypto::HashId::Sha512:
        return run.template operator()<crypto::Sha512>();
    }
    return std::nullopt;
}

Bytes hmac(crypto::HashId hash, View key, View data)
{
    auto const run = [&]<typename Hash>() {
        typename Hash::Digest const mac = crypto::Hmac<Hash>::mac(key, data);
        return Bytes(mac.begin(), mac.end());
    };
    switch (hash) {
    case crypto::HashId::Sha1:
        return run.template operator()<crypto::Sha1>();
    case crypto::HashId::Sha256:
        return run.template operator()<crypto::Sha256>();
    case crypto::HashId::Sha384:
        return run.template operator()<crypto::Sha384>();
    case crypto::HashId::Sha512:
        return run.template operator()<crypto::Sha512>();
    }
    return {};
}

// The HMAC block size, which is a generated or imported key's default length.
std::size_t hash_block_bits(crypto::HashId hash)
{
    return hash == crypto::HashId::Sha384 || hash == crypto::HashId::Sha512 ? 1024 : 512;
}

// ---- WebIDL conversions the dictionaries need

// A member read from a dictionary: nullopt when the getter threw.
std::optional<js::Value> member(js::Interpreter& interp, js::Value const& dictionary, std::string_view name)
{
    if (!dictionary.is_object())
        return js::Value::undefined();
    return interp.get(dictionary, name);
}

std::nullopt_t required_missing(js::Interpreter& interp, std::string_view dictionary, std::string_view name)
{
    return interp.throw_type_error("Failed to read the '" + std::string(name) + "' property from '" + std::string(dictionary)
        + "': Required member is undefined.");
}

// [EnforceRange] on an integer type whose largest value is `max`.
std::optional<double> enforce_range(js::Interpreter& interp, js::Value const& value, double max, std::string_view name)
{
    std::optional<double> const number = interp.to_number(value);
    if (!number)
        return std::nullopt;
    if (!std::isfinite(*number))
        return interp.throw_type_error("Value of '" + std::string(name) + "' is not a finite number.");
    double const integer = std::trunc(*number) + 0.0;
    if (integer < 0 || integer > max)
        return interp.throw_type_error("Value of '" + std::string(name) + "' is outside the accepted range.");
    return integer;
}

// A BufferSource member: its bytes copied now, as normalization says.
std::optional<Bytes> buffer_member(js::Interpreter& interp, js::Value const& value, std::string_view name)
{
    std::optional<View> const bytes = buffer_source_bytes(value);
    if (!bytes) {
        interp.throw_type_error("Value of '" + std::string(name) + "' is not of type '(ArrayBuffer or ArrayBufferView)'.");
        return std::nullopt;
    }
    return Bytes(bytes->begin(), bytes->end());
}

std::optional<std::string> string_of(js::Interpreter& interp, js::Value const& value)
{
    return internals_of(interp).to_utf8(value);
}

// ---- algorithm normalization (§18.4)

enum class Op : std::uint8_t { Encrypt, Decrypt, Sign, Verify, Digest, GenerateKey, DeriveBits, ImportKey, GetKeyLength, WrapKey, UnwrapKey };

enum class Dict : std::uint8_t {
    None,
    RsaHashedKeyGen,
    RsaHashedImport,
    RsaPss,
    RsaOaep,
    Ecdsa,
    EcKeyGen,
    EcKeyImport,
    EcdhDerive,
    AesCtr,
    AesKeyGen,
    AesDerived,
    AesCbc,
    AesGcm,
    HmacImport,
    HmacKeyGen,
    Hkdf,
    Pbkdf2,
};

struct Registered {
    Alg alg;
    Dict dict;
};

std::span<Registered const> registered(Op op)
{
    static constexpr Registered cipher[] = { { Alg::RsaOaep, Dict::RsaOaep }, { Alg::AesCtr, Dict::AesCtr }, { Alg::AesCbc, Dict::AesCbc },
        { Alg::AesGcm, Dict::AesGcm } };
    static constexpr Registered signature[] = { { Alg::RsassaPkcs1, Dict::None }, { Alg::RsaPss, Dict::RsaPss }, { Alg::Ecdsa, Dict::Ecdsa },
        { Alg::Ed25519, Dict::None }, { Alg::Hmac, Dict::None } };
    static constexpr Registered digest[] = { { Alg::Sha1, Dict::None }, { Alg::Sha256, Dict::None }, { Alg::Sha384, Dict::None },
        { Alg::Sha512, Dict::None } };
    static constexpr Registered generate[] = { { Alg::RsassaPkcs1, Dict::RsaHashedKeyGen }, { Alg::RsaPss, Dict::RsaHashedKeyGen },
        { Alg::RsaOaep, Dict::RsaHashedKeyGen }, { Alg::Ecdsa, Dict::EcKeyGen }, { Alg::Ecdh, Dict::EcKeyGen }, { Alg::Ed25519, Dict::None },
        { Alg::X25519, Dict::None }, { Alg::AesCtr, Dict::AesKeyGen }, { Alg::AesCbc, Dict::AesKeyGen }, { Alg::AesGcm, Dict::AesKeyGen },
        { Alg::AesKw, Dict::AesKeyGen }, { Alg::Hmac, Dict::HmacKeyGen } };
    static constexpr Registered derive[] = { { Alg::Ecdh, Dict::EcdhDerive }, { Alg::X25519, Dict::EcdhDerive }, { Alg::Hkdf, Dict::Hkdf },
        { Alg::Pbkdf2, Dict::Pbkdf2 } };
    static constexpr Registered import[] = { { Alg::RsassaPkcs1, Dict::RsaHashedImport }, { Alg::RsaPss, Dict::RsaHashedImport },
        { Alg::RsaOaep, Dict::RsaHashedImport }, { Alg::Ecdsa, Dict::EcKeyImport }, { Alg::Ecdh, Dict::EcKeyImport }, { Alg::Ed25519, Dict::None },
        { Alg::X25519, Dict::None }, { Alg::AesCtr, Dict::None }, { Alg::AesCbc, Dict::None }, { Alg::AesGcm, Dict::None },
        { Alg::AesKw, Dict::None }, { Alg::Hmac, Dict::HmacImport }, { Alg::Hkdf, Dict::None }, { Alg::Pbkdf2, Dict::None } };
    static constexpr Registered key_length[] = { { Alg::AesCtr, Dict::AesDerived }, { Alg::AesCbc, Dict::AesDerived },
        { Alg::AesGcm, Dict::AesDerived }, { Alg::AesKw, Dict::AesDerived }, { Alg::Hmac, Dict::HmacImport }, { Alg::Hkdf, Dict::None },
        { Alg::Pbkdf2, Dict::None } };
    static constexpr Registered wrap[] = { { Alg::AesKw, Dict::None } };
    switch (op) {
    case Op::Encrypt:
    case Op::Decrypt:
        return cipher;
    case Op::Sign:
    case Op::Verify:
        return signature;
    case Op::Digest:
        return digest;
    case Op::GenerateKey:
        return generate;
    case Op::DeriveBits:
        return derive;
    case Op::ImportKey:
        return import;
    case Op::GetKeyLength:
        return key_length;
    case Op::WrapKey:
    case Op::UnwrapKey:
        return wrap;
    }
    return {};
}

// A normalized algorithm: the registered name and every member any of the
// dictionaries above can carry.
struct Params {
    Alg alg = Alg::AesGcm;
    crypto::HashId hash = crypto::HashId::Sha256;
    bool has_hash = false;
    double modulus_length = 0;
    Bytes public_exponent;
    std::string named_curve;
    std::optional<double> length; // AES (unsigned short), HMAC (unsigned long)
    Bytes counter;
    double counter_length = 0;
    Bytes iv;
    std::optional<Bytes> additional_data;
    std::optional<double> tag_length;
    std::optional<Bytes> label;
    double salt_length = 0;
    Bytes salt;
    Bytes info;
    double iterations = 0;
    CryptoKeyObject* public_key = nullptr;
};

std::optional<Params> normalize(js::Interpreter& interp, Op op, js::Value const& algorithm, bool* unsupported = nullptr);

// A HashAlgorithmIdentifier member, normalized for digest.
std::optional<crypto::HashId> hash_member(js::Interpreter& interp, js::Value const& dictionary, std::string_view dictionary_name)
{
    std::optional<js::Value> const value = member(interp, dictionary, "hash");
    if (!value)
        return std::nullopt;
    if (value->is_undefined())
        return required_missing(interp, dictionary_name, "hash");
    js::Value identifier = *value;
    if (!identifier.is_object()) {
        std::optional<js::JsString*> const text = interp.to_string(identifier);
        if (!text)
            return std::nullopt;
        identifier = js::Value::string(*text);
    }
    std::optional<Params> const hash = normalize(interp, Op::Digest, identifier);
    if (!hash)
        return std::nullopt;
    return hash_of(hash->alg);
}

template<typename Read>
bool required(js::Interpreter& interp, js::Value const& dictionary, std::string_view dictionary_name, std::string_view name, Read&& read)
{
    std::optional<js::Value> const value = member(interp, dictionary, name);
    if (!value)
        return false;
    if (value->is_undefined()) {
        required_missing(interp, dictionary_name, name);
        return false;
    }
    return read(*value);
}

template<typename Read>
bool optional_member(js::Interpreter& interp, js::Value const& dictionary, std::string_view name, Read&& read)
{
    std::optional<js::Value> const value = member(interp, dictionary, name);
    if (!value)
        return false;
    if (value->is_undefined())
        return true;
    return read(*value);
}

// The dictionary's members beside `name`, base dictionaries first and each
// one's members in lexicographic order, as WebIDL converts them.
bool convert_members(js::Interpreter& interp, Dict dict, js::Value const& value, Params& params)
{
    auto const bytes_into = [&](Bytes& into, std::string_view name) {
        return [&interp, &into, name](js::Value const& v) {
            std::optional<Bytes> bytes = buffer_member(interp, v, name);
            if (!bytes)
                return false;
            into = std::move(*bytes);
            return true;
        };
    };
    auto const number_into = [&](double& into, double max, std::string_view name) {
        return [&interp, &into, max, name](js::Value const& v) {
            std::optional<double> const number = enforce_range(interp, v, max, name);
            if (!number)
                return false;
            into = *number;
            return true;
        };
    };
    auto const hash_into = [&](std::string_view dictionary_name) {
        std::optional<crypto::HashId> const hash = hash_member(interp, value, dictionary_name);
        if (!hash)
            return false;
        params.hash = *hash;
        params.has_hash = true;
        return true;
    };
    constexpr double max_octet = 255;
    constexpr double max_short = 65535;
    constexpr double max_long = 4294967295.0;
    switch (dict) {
    case Dict::None:
        return true;
    case Dict::RsaHashedKeyGen:
        return required(interp, value, "RsaHashedKeyGenParams", "modulusLength", number_into(params.modulus_length, max_long, "modulusLength"))
            && required(interp, value, "RsaHashedKeyGenParams", "publicExponent",
                [&](js::Value const& v) {
                    if (!v.is_object() || v.as_object()->class_id() != js::Object::Class::TypedArray
                        || static_cast<js::TypedArrayObject*>(v.as_object())->element_type() != js::ElementType::Uint8) {
                        interp.throw_type_error("Value of 'publicExponent' is not of type 'Uint8Array'.");
                        return false;
                    }
                    std::optional<Bytes> bytes = buffer_member(interp, v, "publicExponent");
                    if (!bytes)
                        return false;
                    params.public_exponent = std::move(*bytes);
                    return true;
                })
            && hash_into("RsaHashedKeyGenParams");
    case Dict::RsaHashedImport:
        return hash_into("RsaHashedImportParams");
    case Dict::RsaPss:
        return required(interp, value, "RsaPssParams", "saltLength", number_into(params.salt_length, max_long, "saltLength"));
    case Dict::RsaOaep:
        return optional_member(interp, value, "label", [&](js::Value const& v) {
            std::optional<Bytes> bytes = buffer_member(interp, v, "label");
            if (!bytes)
                return false;
            params.label = std::move(*bytes);
            return true;
        });
    case Dict::Ecdsa:
        return hash_into("EcdsaParams");
    case Dict::EcKeyGen:
    case Dict::EcKeyImport:
        return required(interp, value, dict == Dict::EcKeyGen ? "EcKeyGenParams" : "EcKeyImportParams", "namedCurve", [&](js::Value const& v) {
            std::optional<std::string> const text = string_of(interp, v);
            if (!text)
                return false;
            params.named_curve = *text;
            return true;
        });
    case Dict::EcdhDerive:
        return required(interp, value, "EcdhKeyDeriveParams", "public", [&](js::Value const& v) {
            params.public_key = as_key(v);
            if (params.public_key == nullptr) {
                interp.throw_type_error("Value of 'public' is not of type 'CryptoKey'.");
                return false;
            }
            return true;
        });
    case Dict::AesCtr:
        return required(interp, value, "AesCtrParams", "counter", bytes_into(params.counter, "counter"))
            && required(interp, value, "AesCtrParams", "length", number_into(params.counter_length, max_octet, "length"));
    case Dict::AesKeyGen:
    case Dict::AesDerived:
        return required(interp, value, dict == Dict::AesKeyGen ? "AesKeyGenParams" : "AesDerivedKeyParams", "length", [&](js::Value const& v) {
            std::optional<double> const number = enforce_range(interp, v, max_short, "length");
            if (!number)
                return false;
            params.length = *number;
            return true;
        });
    case Dict::AesCbc:
        return required(interp, value, "AesCbcParams", "iv", bytes_into(params.iv, "iv"));
    case Dict::AesGcm:
        return optional_member(interp, value, "additionalData",
                   [&](js::Value const& v) {
                       std::optional<Bytes> bytes = buffer_member(interp, v, "additionalData");
                       if (!bytes)
                           return false;
                       params.additional_data = std::move(*bytes);
                       return true;
                   })
            && required(interp, value, "AesGcmParams", "iv", bytes_into(params.iv, "iv"))
            && optional_member(interp, value, "tagLength", [&](js::Value const& v) {
                   std::optional<double> const number = enforce_range(interp, v, max_octet, "tagLength");
                   if (!number)
                       return false;
                   params.tag_length = *number;
                   return true;
               });
    case Dict::HmacImport:
    case Dict::HmacKeyGen:
        return hash_into(dict == Dict::HmacImport ? "HmacImportParams" : "HmacKeyGenParams")
            && optional_member(interp, value, "length", [&](js::Value const& v) {
                   std::optional<double> const number = enforce_range(interp, v, max_long, "length");
                   if (!number)
                       return false;
                   params.length = *number;
                   return true;
               });
    case Dict::Hkdf:
        return hash_into("HkdfParams") && required(interp, value, "HkdfParams", "info", bytes_into(params.info, "info"))
            && required(interp, value, "HkdfParams", "salt", bytes_into(params.salt, "salt"));
    case Dict::Pbkdf2:
        return hash_into("Pbkdf2Params")
            && required(interp, value, "Pbkdf2Params", "iterations", number_into(params.iterations, max_long, "iterations"))
            && required(interp, value, "Pbkdf2Params", "salt", bytes_into(params.salt, "salt"));
    }
    return true;
}

// §18.4.4: a string stands for { name: string }; the name is matched
// without regard to ASCII case against the operation's registered names;
// then the registered dictionary's members are converted.
std::optional<Params> normalize(js::Interpreter& interp, Op op, js::Value const& algorithm, bool* unsupported)
{
    js::Value dictionary = algorithm;
    std::string name;
    if (algorithm.is_object()) {
        std::optional<js::Value> const value = member(interp, algorithm, "name");
        if (!value)
            return std::nullopt;
        if (value->is_undefined())
            return required_missing(interp, "Algorithm", "name");
        std::optional<std::string> const text = string_of(interp, *value);
        if (!text)
            return std::nullopt;
        name = *text;
    } else {
        std::optional<std::string> const text = string_of(interp, algorithm);
        if (!text)
            return std::nullopt;
        name = *text;
        dictionary = js::Value::undefined();
    }
    for (Registered const& entry : registered(op)) {
        if (!ascii_ci_equals(name, alg_name(entry.alg)))
            continue;
        Params params;
        params.alg = entry.alg;
        if (entry.dict != Dict::None) {
            if (!dictionary.is_object()) {
                // A bare name for an algorithm whose dictionary has
                // required members fails on the first of them.
                if (!convert_members(interp, entry.dict, js::Value::undefined(), params))
                    return std::nullopt;
            } else if (!convert_members(interp, entry.dict, dictionary, params)) {
                return std::nullopt;
            }
        }
        return params;
    }
    if (unsupported != nullptr) {
        *unsupported = true;
        return std::nullopt;
    }
    internals_of(interp).throw_dom_exception("NotSupportedError", "Algorithm: Unrecognized name");
    return std::nullopt;
}

// ---- JsonWebKey (RFC 7517), as the dictionary of the specification §25

struct Jwk {
    std::optional<std::string> alg, crv, d, dp, dq, e, k, kty, n, p, q, qi, use, x, y;
    std::optional<bool> ext;
    std::optional<std::vector<std::string>> key_ops;
    bool has_oth = false;
};

std::optional<Jwk> read_jwk(js::Interpreter& interp, js::Value const& value)
{
    if (!value.is_object() && !value.is_undefined() && !value.is_null()) {
        interp.throw_type_error("Key data is not of type 'JsonWebKey'.");
        return std::nullopt;
    }
    Jwk jwk;
    auto const text = [&](std::optional<std::string>& into, std::string_view name) {
        return optional_member(interp, value, name, [&](js::Value const& v) {
            std::optional<std::string> s = string_of(interp, v);
            if (!s)
                return false;
            into = std::move(*s);
            return true;
        });
    };
    bool const ok = text(jwk.alg, "alg") && text(jwk.crv, "crv") && text(jwk.d, "d") && text(jwk.dp, "dp") && text(jwk.dq, "dq")
        && text(jwk.e, "e")
        && optional_member(interp, value, "ext",
            [&](js::Value const& v) {
                jwk.ext = js::Interpreter::to_boolean(v);
                return true;
            })
        && text(jwk.k, "k")
        && optional_member(interp, value, "key_ops",
            [&](js::Value const& v) {
                if (!v.is_object()) {
                    interp.throw_type_error("Value of 'key_ops' is not a sequence.");
                    return false;
                }
                std::optional<std::vector<js::Value>> const list = interp.iterable_to_list(v);
                if (!list)
                    return false;
                std::vector<std::string> ops;
                for (js::Value const& item : *list) {
                    std::optional<std::string> s = string_of(interp, item);
                    if (!s)
                        return false;
                    ops.push_back(std::move(*s));
                }
                jwk.key_ops = std::move(ops);
                return true;
            })
        && text(jwk.kty, "kty") && text(jwk.n, "n")
        && optional_member(interp, value, "oth",
            [&](js::Value const&) {
                jwk.has_oth = true;
                return true;
            })
        && text(jwk.p, "p") && text(jwk.q, "q") && text(jwk.qi, "qi") && text(jwk.use, "use") && text(jwk.x, "x") && text(jwk.y, "y");
    if (!ok)
        return std::nullopt;
    return jwk;
}

// ---- the operations, algorithm by algorithm

std::nullopt_t dom_error(js::Interpreter& interp, std::string_view name, std::string_view message)
{
    internals_of(interp).throw_dom_exception(name, message);
    return std::nullopt;
}

bool usages_within(js::Interpreter& interp, unsigned usages, unsigned allowed)
{
    if ((usages & ~allowed) != 0) {
        dom_error(interp, "SyntaxError", "Cannot create a key using the specified key usages.");
        return false;
    }
    return true;
}

std::optional<crypto::Aes> aes_of(KeyData const& key)
{
    return crypto::Aes::with_key(key.secret);
}

crypto::AesBlock block_of(View bytes)
{
    crypto::AesBlock block {};
    std::memcpy(block.data(), bytes.data(), 16);
    return block;
}

std::optional<Bytes> encrypt(js::Interpreter& interp, Params const& params, KeyData const& key, View data)
{
    switch (params.alg) {
    case Alg::RsaOaep: {
        if (key.type != KeyType::Public)
            return dom_error(interp, "InvalidAccessError", "The key is not a public key.");
        Bytes const label = params.label.value_or(Bytes {});
        std::optional<Bytes> out = crypto::rsa_oaep_encrypt(key.rsa_public, key.hash, label, data, random_fill);
        if (!out)
            return dom_error(interp, "OperationError", "The data is too long for the key.");
        return out;
    }
    case Alg::AesCtr: {
        if (params.counter.size() != 16 || params.counter_length == 0 || params.counter_length > 128)
            return dom_error(interp, "OperationError", "The counter must be 16 bytes and its length 1 to 128 bits.");
        std::optional<Bytes> out = crypto::aes_ctr(*aes_of(key), block_of(params.counter), static_cast<std::size_t>(params.counter_length), data);
        if (!out)
            return dom_error(interp, "OperationError", "The data would wrap the counter.");
        return out;
    }
    case Alg::AesCbc:
        if (params.iv.size() != 16)
            return dom_error(interp, "OperationError", "The iv must be 16 bytes.");
        return crypto::aes_cbc_encrypt(*aes_of(key), block_of(params.iv), data);
    case Alg::AesGcm: {
        double const tag_length = params.tag_length.value_or(128);
        if (tag_length != 32 && tag_length != 64 && tag_length != 96 && tag_length != 104 && tag_length != 112 && tag_length != 120 && tag_length != 128)
            return dom_error(interp, "OperationError", "The tag length is not one AES-GCM allows.");
        if (params.iv.empty())
            return dom_error(interp, "OperationError", "The iv must not be empty.");
        Bytes const aad = params.additional_data.value_or(Bytes {});
        return crypto::aes_gcm_seal(*aes_of(key), params.iv, aad, data, static_cast<std::size_t>(tag_length) / 8);
    }
    default:
        break;
    }
    return dom_error(interp, "NotSupportedError", "The algorithm does not encrypt.");
}

std::optional<Bytes> decrypt(js::Interpreter& interp, Params const& params, KeyData const& key, View data)
{
    switch (params.alg) {
    case Alg::RsaOaep: {
        if (key.type != KeyType::Private)
            return dom_error(interp, "InvalidAccessError", "The key is not a private key.");
        Bytes const label = params.label.value_or(Bytes {});
        std::optional<Bytes> out = crypto::rsa_oaep_decrypt(*key.rsa_private, key.hash, label, data);
        if (!out)
            return dom_error(interp, "OperationError", "The data could not be decrypted.");
        return out;
    }
    case Alg::AesCtr:
        return encrypt(interp, params, key, data);
    case Alg::AesCbc: {
        if (params.iv.size() != 16)
            return dom_error(interp, "OperationError", "The iv must be 16 bytes.");
        std::optional<Bytes> out = crypto::aes_cbc_decrypt(*aes_of(key), block_of(params.iv), data);
        if (!out)
            return dom_error(interp, "OperationError", "The data could not be decrypted.");
        return out;
    }
    case Alg::AesGcm: {
        double const tag_length = params.tag_length.value_or(128);
        if (tag_length != 32 && tag_length != 64 && tag_length != 96 && tag_length != 104 && tag_length != 112 && tag_length != 120 && tag_length != 128)
            return dom_error(interp, "OperationError", "The tag length is not one AES-GCM allows.");
        if (params.iv.empty())
            return dom_error(interp, "OperationError", "The iv must not be empty.");
        if (data.size() < static_cast<std::size_t>(tag_length) / 8)
            return dom_error(interp, "OperationError", "The data is shorter than the tag.");
        Bytes const aad = params.additional_data.value_or(Bytes {});
        std::optional<Bytes> out = crypto::aes_gcm_open(*aes_of(key), params.iv, aad, data, static_cast<std::size_t>(tag_length) / 8);
        if (!out)
            return dom_error(interp, "OperationError", "The data could not be decrypted.");
        return out;
    }
    default:
        break;
    }
    return dom_error(interp, "NotSupportedError", "The algorithm does not decrypt.");
}

Bytes fixed_width(crypto::BigInt const& value, std::size_t size)
{
    return *value.to_bytes(size);
}

std::optional<Bytes> sign(js::Interpreter& interp, Params const& params, KeyData const& key, View data)
{
    if (params.alg != Alg::Hmac && key.type != KeyType::Private)
        return dom_error(interp, "InvalidAccessError", "The key is not a private key.");
    switch (params.alg) {
    case Alg::RsassaPkcs1:
    case Alg::RsaPss: {
        Bytes const digest = crypto::hash_with(key.hash, data);
        std::optional<Bytes> out = params.alg == Alg::RsassaPkcs1
            ? crypto::rsa_sign_pkcs1_v15(*key.rsa_private, key.hash, digest)
            : crypto::rsa_sign_pss(*key.rsa_private, key.hash, digest, static_cast<std::size_t>(params.salt_length), random_fill);
        if (!out)
            return dom_error(interp, "OperationError", "The key is too small for the signature.");
        return out;
    }
    case Alg::Ecdsa: {
        Bytes const digest = crypto::hash_with(params.hash, data);
        std::optional<std::pair<crypto::BigInt, crypto::BigInt>> const rs = crypto::ecdsa_sign(curve_id(key.named_curve), key.secret, digest, random_fill);
        if (!rs)
            return dom_error(interp, "OperationError", "The signature could not be made.");
        std::size_t const size = curve_bytes(key.named_curve);
        Bytes out = fixed_width(rs->first, size);
        Bytes const s = fixed_width(rs->second, size);
        out.insert(out.end(), s.begin(), s.end());
        return out;
    }
    case Alg::Ed25519: {
        crypto::Ed25519Seed seed {};
        std::memcpy(seed.data(), key.secret.data(), 32);
        crypto::Ed25519Signature const signature = crypto::ed25519_sign(seed, data);
        return Bytes(signature.begin(), signature.end());
    }
    case Alg::Hmac:
        return hmac(key.hash, key.secret, data);
    default:
        break;
    }
    return dom_error(interp, "NotSupportedError", "The algorithm does not sign.");
}

std::optional<bool> verify(js::Interpreter& interp, Params const& params, KeyData const& key, View signature, View data)
{
    if (params.alg != Alg::Hmac && key.type != KeyType::Public)
        return dom_error(interp, "InvalidAccessError", "The key is not a public key.");
    switch (params.alg) {
    case Alg::RsassaPkcs1:
    case Alg::RsaPss: {
        Bytes const digest = crypto::hash_with(key.hash, data);
        return params.alg == Alg::RsassaPkcs1
            ? crypto::rsa_verify_pkcs1_v15_any(key.rsa_public, key.hash, digest, signature)
            : crypto::rsa_verify_pss_any(key.rsa_public, key.hash, digest, signature, static_cast<std::size_t>(params.salt_length));
    }
    case Alg::Ecdsa: {
        std::size_t const size = curve_bytes(key.named_curve);
        if (signature.size() != 2 * size)
            return false;
        Bytes const digest = crypto::hash_with(params.hash, data);
        crypto::BigInt const r = *crypto::BigInt::from_bytes(signature.subspan(0, size));
        crypto::BigInt const s = *crypto::BigInt::from_bytes(signature.subspan(size));
        return crypto::ecdsa_verify(curve_id(key.named_curve), key.ec_public, digest, r, s);
    }
    case Alg::Ed25519:
        return crypto::ed25519_verify(key.okp_public, data, signature);
    case Alg::Hmac: {
        Bytes const expected = hmac(key.hash, key.secret, data);
        return crypto::constant_time_equal(expected, signature);
    }
    default:
        break;
    }
    return dom_error(interp, "NotSupportedError", "The algorithm does not verify.");
}

// The bit length a derived key of this algorithm asks for; nullopt with
// nothing thrown for HKDF and PBKDF2, whose length is null.
struct KeyLength {
    std::optional<std::size_t> bits;
};

std::optional<KeyLength> key_length(js::Interpreter& interp, Params const& params)
{
    if (is_aes(params.alg)) {
        double const length = params.length.value_or(0);
        if (length != 128 && length != 192 && length != 256)
            return dom_error(interp, "OperationError", "AES keys are 128, 192 or 256 bits.");
        return KeyLength { static_cast<std::size_t>(length) };
    }
    if (params.alg == Alg::Hmac) {
        if (!params.length)
            return KeyLength { hash_block_bits(params.hash) };
        if (*params.length == 0)
            return interp.throw_type_error("An HMAC key cannot be empty.");
        return KeyLength { static_cast<std::size_t>(*params.length) };
    }
    return KeyLength {};
}

// The first `bits` bits of the secret, the rest of the last byte cleared;
// the whole of it when bits is null.
std::optional<Bytes> truncate_bits(js::Interpreter& interp, Bytes secret, std::optional<double> bits)
{
    if (!bits)
        return secret;
    if (*bits > 8.0 * static_cast<double>(secret.size()))
        return dom_error(interp, "OperationError", "The length is longer than the derived secret.");
    std::size_t const length = static_cast<std::size_t>(*bits);
    secret.resize((length + 7) / 8);
    if (length % 8 != 0)
        secret.back() = static_cast<std::uint8_t>(secret.back() & (0xff << (8 - length % 8)));
    return secret;
}

std::optional<Bytes> derive_bits(js::Interpreter& interp, Params const& params, KeyData const& key, std::optional<double> const& length)
{
    switch (params.alg) {
    case Alg::Ecdh:
    case Alg::X25519: {
        CryptoKeyObject const* peer = params.public_key;
        if (key.type != KeyType::Private)
            return dom_error(interp, "InvalidAccessError", "The base key is not a private key.");
        KeyData const& other = *peer->data;
        if (other.type != KeyType::Public)
            return dom_error(interp, "InvalidAccessError", "The public key is not a public key.");
        if (other.alg != key.alg)
            return dom_error(interp, "InvalidAccessError", "The public key is of another algorithm.");
        if (params.alg == Alg::Ecdh) {
            if (other.named_curve != key.named_curve)
                return dom_error(interp, "InvalidAccessError", "The public key is on another curve.");
            std::optional<Bytes> secret = crypto::ecdh_shared_x(curve_id(key.named_curve), key.secret, other.ec_public);
            if (!secret)
                return dom_error(interp, "OperationError", "The shared secret could not be made.");
            return truncate_bits(interp, std::move(*secret), length);
        }
        crypto::X25519Key scalar {};
        std::memcpy(scalar.data(), key.secret.data(), 32);
        crypto::X25519Key shared {};
        if (!crypto::x25519(shared, scalar, other.okp_public))
            return dom_error(interp, "OperationError", "The shared secret is all zeros.");
        return truncate_bits(interp, Bytes(shared.begin(), shared.end()), length);
    }
    case Alg::Hkdf:
    case Alg::Pbkdf2: {
        if (!length)
            return dom_error(interp, "OperationError", "A length is required.");
        if (std::fmod(*length, 8) != 0)
            return dom_error(interp, "OperationError", "The length must be a multiple of 8.");
        std::size_t const bytes = static_cast<std::size_t>(*length) / 8;
        if (bytes == 0)
            return Bytes {};
        if (params.alg == Alg::Pbkdf2) {
            if (params.iterations == 0)
                return dom_error(interp, "OperationError", "The iteration count must not be zero.");
            return pbkdf2(params.hash, key.secret, params.salt, static_cast<std::uint32_t>(params.iterations), bytes);
        }
        std::optional<Bytes> out = hkdf(params.hash, key.secret, params.salt, params.info, bytes);
        if (!out)
            return dom_error(interp, "OperationError", "The length is too long for HKDF.");
        return out;
    }
    default:
        break;
    }
    return dom_error(interp, "NotSupportedError", "The algorithm does not derive bits.");
}

// ---- generating keys

struct Generated {
    std::shared_ptr<KeyData> first; // the secret key, or the public key of a pair
    std::shared_ptr<KeyData> private_key; // null for a secret key
    unsigned public_usages = 0;
    unsigned private_usages = 0;
};

std::optional<Generated> generate_key(js::Interpreter& interp, Params const& params, unsigned usages)
{
    Generated out;
    auto key = std::make_shared<KeyData>();
    key->alg = params.alg;
    if (is_rsa(params.alg)) {
        unsigned const public_allowed = params.alg == Alg::RsaOaep ? usage::encrypt | usage::wrap_key : usage::verify;
        unsigned const private_allowed = params.alg == Alg::RsaOaep ? usage::decrypt | usage::unwrap_key : usage::sign;
        if (!usages_within(interp, usages, public_allowed | private_allowed))
            return std::nullopt;
        std::optional<crypto::BigInt> const e = crypto::BigInt::from_bytes(params.public_exponent);
        if (!e || params.modulus_length > 4096)
            return dom_error(interp, "OperationError", "The key could not be generated.");
        std::optional<crypto::RsaPrivateKey> const generated = crypto::rsa_generate(static_cast<std::size_t>(params.modulus_length), *e, random_fill);
        if (!generated)
            return dom_error(interp, "OperationError", "The key could not be generated.");
        key->hash = params.hash;
        key->type = KeyType::Public;
        key->rsa_public = crypto::RsaPublicKey { generated->n, generated->e };
        auto private_key = std::make_shared<KeyData>(*key);
        private_key->type = KeyType::Private;
        private_key->rsa_private = *generated;
        out.first = key;
        out.private_key = private_key;
        out.public_usages = usages & public_allowed;
        out.private_usages = usages & private_allowed;
        return out;
    }
    if (is_ec(params.alg) || is_okp(params.alg)) {
        bool const signs = params.alg == Alg::Ecdsa || params.alg == Alg::Ed25519;
        unsigned const public_allowed = signs ? usage::verify : 0;
        unsigned const private_allowed = signs ? usage::sign : usage::derive_key | usage::derive_bits;
        if (!usages_within(interp, usages, public_allowed | private_allowed))
            return std::nullopt;
        key->type = KeyType::Public;
        auto private_key = std::make_shared<KeyData>(*key);
        private_key->type = KeyType::Private;
        if (is_ec(params.alg)) {
            if (!curve_supported(params.named_curve))
                return dom_error(interp, "NotSupportedError", "The named curve is not supported.");
            crypto::CurveId const curve = curve_id(params.named_curve);
            for (;;) {
                Bytes scalar = random_bytes(curve_bytes(params.named_curve));
                if (!crypto::ec_private_scalar_valid(curve, scalar))
                    continue;
                std::optional<crypto::EcPoint> const point = crypto::ec_public_point(curve, scalar);
                if (!point)
                    continue;
                key->named_curve = private_key->named_curve = params.named_curve;
                key->ec_public = private_key->ec_public = *point;
                private_key->secret = std::move(scalar);
                break;
            }
        } else {
            Bytes const secret = random_bytes(32);
            std::array<std::uint8_t, 32> scalar {};
            std::memcpy(scalar.data(), secret.data(), 32);
            key->okp_public = params.alg == Alg::Ed25519 ? crypto::ed25519_public_key(scalar) : crypto::x25519_public(scalar);
            private_key->okp_public = key->okp_public;
            private_key->secret = secret;
        }
        out.first = key;
        out.private_key = private_key;
        out.public_usages = usages & public_allowed;
        out.private_usages = usages & private_allowed;
        return out;
    }
    if (is_aes(params.alg)) {
        unsigned const allowed = params.alg == Alg::AesKw ? usage::wrap_key | usage::unwrap_key
                                                          : usage::encrypt | usage::decrypt | usage::wrap_key | usage::unwrap_key;
        if (!usages_within(interp, usages, allowed))
            return std::nullopt;
        double const length = params.length.value_or(0);
        if (length != 128 && length != 192 && length != 256)
            return dom_error(interp, "OperationError", "AES keys are 128, 192 or 256 bits.");
        key->length = static_cast<std::size_t>(length);
        key->secret = random_bytes(key->length / 8);
        out.first = key;
        out.public_usages = usages;
        return out;
    }
    if (params.alg == Alg::Hmac) {
        if (!usages_within(interp, usages, usage::sign | usage::verify))
            return std::nullopt;
        std::size_t bits = hash_block_bits(params.hash);
        if (params.length) {
            if (*params.length == 0)
                return dom_error(interp, "OperationError", "An HMAC key cannot be empty.");
            bits = static_cast<std::size_t>(*params.length);
        }
        key->hash = params.hash;
        key->length = bits;
        key->secret = random_bytes((bits + 7) / 8);
        if (bits % 8 != 0)
            key->secret.back() = static_cast<std::uint8_t>(key->secret.back() & (0xff << (8 - bits % 8)));
        out.first = key;
        out.public_usages = usages;
        return out;
    }
    return dom_error(interp, "NotSupportedError", "The algorithm does not generate keys.");
}

// ---- importing keys

enum class Format : std::uint8_t { Raw, Spki, Pkcs8, Jwk };

std::optional<Format> format_of(std::string_view text)
{
    if (text == "raw")
        return Format::Raw;
    if (text == "spki")
        return Format::Spki;
    if (text == "pkcs8")
        return Format::Pkcs8;
    if (text == "jwk")
        return Format::Jwk;
    return std::nullopt;
}

struct KeyInput {
    Format format = Format::Raw;
    Bytes bytes;
    Jwk jwk;
};

std::optional<Bytes> jwk_bytes(js::Interpreter& interp, std::optional<std::string> const& field, std::string_view name)
{
    if (!field)
        return dom_error(interp, "DataError", "The JWK member '" + std::string(name) + "' is missing.");
    std::optional<Bytes> bytes = base64url_decode(*field);
    if (!bytes)
        return dom_error(interp, "DataError", "The JWK member '" + std::string(name) + "' is not base64url.");
    return bytes;
}

// The checks every JWK import makes after its kty (§ "import key" steps of
// each algorithm): use, key_ops and ext against what the key will be.
bool jwk_checks(js::Interpreter& interp, Jwk const& jwk, unsigned usages, bool extractable, std::string_view use)
{
    if (usages != 0 && jwk.use && *jwk.use != use) {
        dom_error(interp, "DataError", "The JWK 'use' member does not allow these usages.");
        return false;
    }
    if (jwk.key_ops) {
        std::vector<std::string> sorted = *jwk.key_ops;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            dom_error(interp, "DataError", "The JWK 'key_ops' member repeats a value.");
            return false;
        }
        for (std::size_t i = 0; i < usage_names.size(); ++i) {
            if ((usages & (1u << i)) != 0 && std::find(sorted.begin(), sorted.end(), usage_names[i]) == sorted.end()) {
                dom_error(interp, "DataError", "The JWK 'key_ops' member does not allow these usages.");
                return false;
            }
        }
    }
    if (jwk.ext && !*jwk.ext && extractable) {
        dom_error(interp, "DataError", "The JWK is not extractable.");
        return false;
    }
    return true;
}

std::string_view hash_suffix(crypto::HashId hash)
{
    switch (hash) {
    case crypto::HashId::Sha1:
        return "1";
    case crypto::HashId::Sha256:
        return "256";
    case crypto::HashId::Sha384:
        return "384";
    case crypto::HashId::Sha512:
        return "512";
    }
    return "";
}

// The JWK "alg" of a key (RFC 7518 §3.1, §4.1, §6), empty where JWK has none.
std::string jwk_alg(KeyData const& key)
{
    switch (key.alg) {
    case Alg::RsassaPkcs1:
        return "RS" + std::string(hash_suffix(key.hash));
    case Alg::RsaPss:
        return "PS" + std::string(hash_suffix(key.hash));
    case Alg::RsaOaep:
        return key.hash == crypto::HashId::Sha1 ? "RSA-OAEP" : "RSA-OAEP-" + std::string(hash_suffix(key.hash));
    case Alg::Hmac:
        return "HS" + std::string(hash_suffix(key.hash));
    case Alg::AesCtr:
        return "A" + std::to_string(key.length) + "CTR";
    case Alg::AesCbc:
        return "A" + std::to_string(key.length) + "CBC";
    case Alg::AesGcm:
        return "A" + std::to_string(key.length) + "GCM";
    case Alg::AesKw:
        return "A" + std::to_string(key.length) + "KW";
    case Alg::Ed25519:
        return "Ed25519";
    default:
        return "";
    }
}

std::optional<std::shared_ptr<KeyData>> import_secret(js::Interpreter& interp, Params const& params, KeyInput const& input, bool extractable, unsigned usages)
{
    auto key = std::make_shared<KeyData>();
    key->alg = params.alg;
    key->type = KeyType::Secret;
    if (params.alg == Alg::Hkdf || params.alg == Alg::Pbkdf2) {
        if (input.format != Format::Raw)
            return dom_error(interp, "NotSupportedError", "This key imports from raw bytes only.");
        if (!usages_within(interp, usages, usage::derive_key | usage::derive_bits))
            return std::nullopt;
        if (extractable)
            return dom_error(interp, "SyntaxError", "This key cannot be extractable.");
        key->secret = input.bytes;
        return key;
    }
    bool const is_hmac = params.alg == Alg::Hmac;
    unsigned const allowed = is_hmac ? usage::sign | usage::verify
        : params.alg == Alg::AesKw   ? usage::wrap_key | usage::unwrap_key
                                     : usage::encrypt | usage::decrypt | usage::wrap_key | usage::unwrap_key;
    if (!usages_within(interp, usages, allowed))
        return std::nullopt;
    if (is_hmac)
        key->hash = params.hash;
    Bytes data;
    if (input.format == Format::Raw) {
        data = input.bytes;
    } else if (input.format == Format::Jwk) {
        Jwk const& jwk = input.jwk;
        if (!jwk.kty || *jwk.kty != "oct")
            return dom_error(interp, "DataError", "The JWK 'kty' member is not 'oct'.");
        std::optional<Bytes> k = jwk_bytes(interp, jwk.k, "k");
        if (!k)
            return std::nullopt;
        data = std::move(*k);
        if (is_hmac) {
            if (jwk.alg && *jwk.alg != "HS" + std::string(hash_suffix(params.hash)))
                return dom_error(interp, "DataError", "The JWK 'alg' member does not match the hash.");
        } else {
            if (data.size() != 16 && data.size() != 24 && data.size() != 32)
                return dom_error(interp, "DataError", "AES keys are 128, 192 or 256 bits.");
            KeyData sized;
            sized.alg = params.alg;
            sized.length = data.size() * 8;
            if (jwk.alg && *jwk.alg != jwk_alg(sized))
                return dom_error(interp, "DataError", "The JWK 'alg' member does not match the key.");
        }
        if (!jwk_checks(interp, jwk, usages, extractable, is_hmac ? "sig" : "enc"))
            return std::nullopt;
    } else {
        return dom_error(interp, "NotSupportedError", "This key imports from raw bytes or a JWK only.");
    }
    if (is_hmac) {
        std::size_t const bits = data.size() * 8;
        if (bits == 0)
            return dom_error(interp, "DataError", "An HMAC key cannot be empty.");
        std::size_t length = bits;
        if (params.length) {
            if (*params.length > static_cast<double>(bits) || *params.length <= static_cast<double>(bits) - 8)
                return dom_error(interp, "DataError", "The length does not match the key data.");
            length = static_cast<std::size_t>(*params.length);
        }
        key->length = length;
    } else {
        if (data.size() != 16 && data.size() != 24 && data.size() != 32)
            return dom_error(interp, "DataError", "AES keys are 128, 192 or 256 bits.");
        key->length = data.size() * 8;
    }
    key->secret = std::move(data);
    return key;
}

std::optional<crypto::BigInt> jwk_bigint(js::Interpreter& interp, std::optional<std::string> const& field, std::string_view name)
{
    std::optional<Bytes> const bytes = jwk_bytes(interp, field, name);
    if (!bytes)
        return std::nullopt;
    std::optional<crypto::BigInt> value = crypto::BigInt::from_bytes(*bytes);
    if (!value)
        return dom_error(interp, "DataError", "The JWK member '" + std::string(name) + "' is too large.");
    return value;
}

bool rsa_public_acceptable(crypto::RsaPublicKey const& key)
{
    std::size_t const bits = key.n.bit_length();
    return key.n.is_odd() && bits >= 512 && bits <= crypto::BigInt::max_bits / 2 && key.e.is_odd() && key.e.compare(crypto::BigInt::from_u64(3)) >= 0
        && key.e.compare(key.n) < 0;
}

std::optional<std::shared_ptr<KeyData>> import_rsa(js::Interpreter& interp, Params const& params, KeyInput const& input, bool extractable, unsigned usages)
{
    bool const oaep = params.alg == Alg::RsaOaep;
    unsigned const public_allowed = oaep ? usage::encrypt | usage::wrap_key : usage::verify;
    unsigned const private_allowed = oaep ? usage::decrypt | usage::unwrap_key : usage::sign;
    auto key = std::make_shared<KeyData>();
    key->alg = params.alg;
    key->hash = params.hash;
    switch (input.format) {
    case Format::Spki: {
        if (!usages_within(interp, usages, public_allowed))
            return std::nullopt;
        std::optional<KeyInfo> const info = read_spki(input.bytes);
        if (!info || info->oid != oid_rsa)
            return dom_error(interp, "DataError", "The data is not an RSA SubjectPublicKeyInfo.");
        tls::DerReader outer(info->key);
        std::optional<tls::DerElement> const sequence = outer.next(tls::DerType::Sequence);
        if (!sequence || !outer.at_end())
            return dom_error(interp, "DataError", "The RSA public key is malformed.");
        tls::DerReader reader(sequence->content);
        std::optional<crypto::BigInt> const n = der_bigint(reader);
        std::optional<crypto::BigInt> const e = n ? der_bigint(reader) : std::nullopt;
        if (!n || !e || !reader.at_end())
            return dom_error(interp, "DataError", "The RSA public key is malformed.");
        key->type = KeyType::Public;
        key->rsa_public = crypto::RsaPublicKey { *n, *e };
        break;
    }
    case Format::Pkcs8: {
        if (!usages_within(interp, usages, private_allowed))
            return std::nullopt;
        std::optional<KeyInfo> const info = read_pkcs8(input.bytes);
        if (!info || info->oid != oid_rsa)
            return dom_error(interp, "DataError", "The data is not an RSA PrivateKeyInfo.");
        tls::DerReader outer(info->key);
        std::optional<tls::DerElement> const sequence = outer.next(tls::DerType::Sequence);
        if (!sequence)
            return dom_error(interp, "DataError", "The RSA private key is malformed.");
        tls::DerReader reader(sequence->content);
        std::optional<crypto::BigInt> const version = der_bigint(reader);
        if (!version || !version->is_zero())
            return dom_error(interp, "DataError", "The RSA private key is malformed.");
        std::array<crypto::BigInt, 8> parts;
        for (crypto::BigInt& part : parts) {
            std::optional<crypto::BigInt> const value = der_bigint(reader);
            if (!value)
                return dom_error(interp, "DataError", "The RSA private key is malformed.");
            part = *value;
        }
        if (!reader.at_end())
            return dom_error(interp, "DataError", "The RSA private key is malformed.");
        std::optional<crypto::RsaPrivateKey> const private_key = crypto::rsa_private_key(parts[0], parts[1], parts[2], parts[3], parts[4], parts[5], parts[6], parts[7]);
        if (!private_key)
            return dom_error(interp, "DataError", "The RSA private key is not consistent.");
        key->type = KeyType::Private;
        key->rsa_private = *private_key;
        key->rsa_public = crypto::RsaPublicKey { private_key->n, private_key->e };
        break;
    }
    case Format::Jwk: {
        Jwk const& jwk = input.jwk;
        bool const is_private = jwk.d.has_value();
        if (!usages_within(interp, usages, is_private ? private_allowed : public_allowed))
            return std::nullopt;
        if (!jwk.kty || *jwk.kty != "RSA")
            return dom_error(interp, "DataError", "The JWK 'kty' member is not 'RSA'.");
        if (!jwk_checks(interp, jwk, usages, extractable, oaep ? "enc" : "sig"))
            return std::nullopt;
        if (jwk.alg) {
            KeyData expected;
            expected.alg = params.alg;
            expected.hash = params.hash;
            if (*jwk.alg != jwk_alg(expected))
                return dom_error(interp, "DataError", "The JWK 'alg' member does not match the algorithm.");
        }
        std::optional<crypto::BigInt> const n = jwk_bigint(interp, jwk.n, "n");
        if (!n)
            return std::nullopt;
        std::optional<crypto::BigInt> const e = jwk_bigint(interp, jwk.e, "e");
        if (!e)
            return std::nullopt;
        key->rsa_public = crypto::RsaPublicKey { *n, *e };
        if (is_private) {
            if (jwk.has_oth)
                return dom_error(interp, "DataError", "Keys of more than two primes are not supported.");
            std::array<crypto::BigInt, 6> parts;
            std::array<std::pair<std::optional<std::string> const*, std::string_view>, 6> const fields = { { { &jwk.d, "d" }, { &jwk.p, "p" },
                { &jwk.q, "q" }, { &jwk.dp, "dp" }, { &jwk.dq, "dq" }, { &jwk.qi, "qi" } } };
            for (std::size_t i = 0; i < fields.size(); ++i) {
                std::optional<crypto::BigInt> const value = jwk_bigint(interp, *fields[i].first, fields[i].second);
                if (!value)
                    return std::nullopt;
                parts[i] = *value;
            }
            std::optional<crypto::RsaPrivateKey> const private_key = crypto::rsa_private_key(*n, *e, parts[0], parts[1], parts[2], parts[3], parts[4], parts[5]);
            if (!private_key)
                return dom_error(interp, "DataError", "The RSA private key is not consistent.");
            key->type = KeyType::Private;
            key->rsa_private = *private_key;
        } else {
            key->type = KeyType::Public;
        }
        break;
    }
    case Format::Raw:
        return dom_error(interp, "NotSupportedError", "RSA keys do not import from raw bytes.");
    }
    if (!rsa_public_acceptable(key->rsa_public))
        return dom_error(interp, "DataError", "The RSA key's modulus or exponent is not supported.");
    return key;
}

// An EC private key's scalar, left-padded to the field's size.
std::optional<Bytes> scalar_of(View bytes, std::size_t size)
{
    if (bytes.size() > size)
        return std::nullopt;
    Bytes out(size - bytes.size(), 0);
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

std::optional<std::shared_ptr<KeyData>> import_ec(js::Interpreter& interp, Params const& params, KeyInput const& input, bool extractable, unsigned usages)
{
    bool const ecdsa = params.alg == Alg::Ecdsa;
    unsigned const public_allowed = ecdsa ? usage::verify : 0;
    unsigned const private_allowed = ecdsa ? usage::sign : usage::derive_key | usage::derive_bits;
    auto key = std::make_shared<KeyData>();
    key->alg = params.alg;
    // The curve named by an algorithm identifier's parameters; DataError
    // when they are not a named curve or name another curve than asked.
    auto const curve_from_parameters = [&](std::optional<tls::DerElement> const& parameters) -> std::optional<std::string> {
        if (!parameters || !parameters->tag.is(tls::DerType::Oid))
            return dom_error(interp, "DataError", "The key's parameters are not a named curve.");
        std::optional<std::string> const oid = tls::der_oid(*parameters);
        std::optional<std::string> const curve = oid ? curve_from_oid(*oid) : std::nullopt;
        if (!curve)
            return dom_error(interp, "DataError", "The key's curve is not a recognized curve.");
        if (*curve != params.named_curve)
            return dom_error(interp, "DataError", "The key's curve is not the curve asked for.");
        return curve;
    };
    if (!curve_recognized(params.named_curve))
        return dom_error(interp, "NotSupportedError", "The named curve is not supported.");
    switch (input.format) {
    case Format::Spki: {
        if (!usages_within(interp, usages, public_allowed))
            return std::nullopt;
        std::optional<KeyInfo> const info = read_spki(input.bytes);
        if (!info || (info->oid != oid_ec_public_key && !(params.alg == Alg::Ecdh && info->oid == oid_ec_dh)))
            return dom_error(interp, "DataError", "The data is not an EC SubjectPublicKeyInfo.");
        if (!curve_from_parameters(info->parameters))
            return std::nullopt;
        if (!curve_supported(params.named_curve))
            return dom_error(interp, "NotSupportedError", "The named curve is not supported.");
        std::optional<crypto::EcPoint> const point = crypto::ec_decode_point_any(curve_id(params.named_curve), info->key);
        if (!point)
            return dom_error(interp, "DataError", "The public key is not a point of the curve.");
        key->type = KeyType::Public;
        key->ec_public = *point;
        break;
    }
    case Format::Pkcs8: {
        if (!usages_within(interp, usages, private_allowed))
            return std::nullopt;
        std::optional<KeyInfo> const info = read_pkcs8(input.bytes);
        if (!info || (info->oid != oid_ec_public_key && !(params.alg == Alg::Ecdh && info->oid == oid_ec_dh)))
            return dom_error(interp, "DataError", "The data is not an EC PrivateKeyInfo.");
        if (!curve_from_parameters(info->parameters))
            return std::nullopt;
        if (!curve_supported(params.named_curve))
            return dom_error(interp, "NotSupportedError", "The named curve is not supported.");
        // ECPrivateKey (RFC 5915 §3).
        tls::DerReader outer(info->key);
        std::optional<tls::DerElement> const sequence = outer.next(tls::DerType::Sequence);
        if (!sequence || !outer.at_end())
            return dom_error(interp, "DataError", "The EC private key is malformed.");
        tls::DerReader reader(sequence->content);
        std::optional<tls::DerElement> const version = reader.next(tls::DerType::Integer);
        std::optional<std::uint64_t> const version_number = version ? tls::der_small_integer(*version) : std::nullopt;
        std::optional<tls::DerElement> const scalar = reader.next(tls::DerType::OctetString);
        if (!version_number || *version_number != 1 || !scalar)
            return dom_error(interp, "DataError", "The EC private key is malformed.");
        if (reader.peek_context(0)) {
            std::optional<tls::DerElement> const parameters = reader.next_context(0);
            tls::DerReader inner(parameters->content);
            std::optional<tls::DerElement> const curve = inner.next();
            if (!curve_from_parameters(curve))
                return std::nullopt;
        }
        std::optional<crypto::EcPoint> embedded;
        if (reader.peek_context(1)) {
            std::optional<tls::DerElement> const public_key = reader.next_context(1);
            tls::DerReader inner(public_key->content);
            std::optional<tls::DerElement> const bits = inner.next(tls::DerType::BitString);
            std::optional<View> const point_bytes = bits ? tls::der_bit_string(*bits) : std::nullopt;
            if (point_bytes)
                embedded = crypto::ec_decode_point(curve_id(params.named_curve), *point_bytes);
            if (!embedded)
                return dom_error(interp, "DataError", "The EC private key's public key is malformed.");
        }
        std::optional<Bytes> d = scalar_of(scalar->content, curve_bytes(params.named_curve));
        if (!d || !crypto::ec_private_scalar_valid(curve_id(params.named_curve), *d))
            return dom_error(interp, "DataError", "The EC private key is out of range.");
        std::optional<crypto::EcPoint> const point = crypto::ec_public_point(curve_id(params.named_curve), *d);
        if (!point || (embedded && !(embedded->x == point->x && embedded->y == point->y)))
            return dom_error(interp, "DataError", "The EC private key does not match its public key.");
        key->type = KeyType::Private;
        key->secret = std::move(*d);
        key->ec_public = *point;
        break;
    }
    case Format::Jwk: {
        Jwk const& jwk = input.jwk;
        bool const is_private = jwk.d.has_value();
        if (!usages_within(interp, usages, is_private ? private_allowed : public_allowed))
            return std::nullopt;
        if (!jwk.kty || *jwk.kty != "EC")
            return dom_error(interp, "DataError", "The JWK 'kty' member is not 'EC'.");
        if (!jwk_checks(interp, jwk, usages, extractable, ecdsa ? "sig" : "enc"))
            return std::nullopt;
        if (!jwk.crv || *jwk.crv != params.named_curve)
            return dom_error(interp, "DataError", "The JWK 'crv' member does not match the curve.");
        if (ecdsa && jwk.alg) {
            std::string_view const expected = params.named_curve == "P-256" ? "ES256" : params.named_curve == "P-384" ? "ES384" : "ES512";
            if (*jwk.alg != expected)
                return dom_error(interp, "DataError", "The JWK 'alg' member does not match the curve.");
        }
        if (!curve_supported(params.named_curve))
            return dom_error(interp, "NotSupportedError", "The named curve is not supported.");
        std::size_t const size = curve_bytes(params.named_curve);
        std::optional<Bytes> const x = jwk_bytes(interp, jwk.x, "x");
        if (!x)
            return std::nullopt;
        std::optional<Bytes> const y = jwk_bytes(interp, jwk.y, "y");
        if (!y)
            return std::nullopt;
        if (x->size() != size || y->size() != size)
            return dom_error(interp, "DataError", "The JWK coordinates are not the curve's size.");
        Bytes encoded { 0x04 };
        encoded.insert(encoded.end(), x->begin(), x->end());
        encoded.insert(encoded.end(), y->begin(), y->end());
        std::optional<crypto::EcPoint> const point = crypto::ec_decode_point(curve_id(params.named_curve), encoded);
        if (!point)
            return dom_error(interp, "DataError", "The JWK is not a point of the curve.");
        key->ec_public = *point;
        key->type = KeyType::Public;
        if (is_private) {
            std::optional<Bytes> d = jwk_bytes(interp, jwk.d, "d");
            if (!d)
                return std::nullopt;
            if (d->size() != size || !crypto::ec_private_scalar_valid(curve_id(params.named_curve), *d))
                return dom_error(interp, "DataError", "The JWK private key is out of range.");
            std::optional<crypto::EcPoint> const derived = crypto::ec_public_point(curve_id(params.named_curve), *d);
            if (!derived || !(derived->x == point->x && derived->y == point->y))
                return dom_error(interp, "DataError", "The JWK private key does not match its public key.");
            key->type = KeyType::Private;
            key->secret = std::move(*d);
        }
        break;
    }
    case Format::Raw: {
        if (!usages_within(interp, usages, public_allowed))
            return std::nullopt;
        if (!curve_supported(params.named_curve))
            return dom_error(interp, "NotSupportedError", "The named curve is not supported.");
        std::optional<crypto::EcPoint> const point = crypto::ec_decode_point_any(curve_id(params.named_curve), input.bytes);
        if (!point)
            return dom_error(interp, "DataError", "The data is not a point of the curve.");
        key->type = KeyType::Public;
        key->ec_public = *point;
        break;
    }
    }
    key->named_curve = params.named_curve;
    return key;
}

std::optional<std::shared_ptr<KeyData>> import_okp(js::Interpreter& interp, Params const& params, KeyInput const& input, bool extractable, unsigned usages)
{
    bool const ed = params.alg == Alg::Ed25519;
    unsigned const public_allowed = ed ? usage::verify : 0;
    unsigned const private_allowed = ed ? usage::sign : usage::derive_key | usage::derive_bits;
    std::string_view const oid = ed ? oid_ed25519 : oid_x25519;
    auto key = std::make_shared<KeyData>();
    key->alg = params.alg;
    auto const public_of = [&](View secret) {
        std::array<std::uint8_t, 32> scalar {};
        std::memcpy(scalar.data(), secret.data(), 32);
        return ed ? crypto::ed25519_public_key(scalar) : crypto::x25519_public(scalar);
    };
    switch (input.format) {
    case Format::Raw:
    case Format::Spki: {
        if (!usages_within(interp, usages, public_allowed))
            return std::nullopt;
        View bytes = input.bytes;
        std::optional<KeyInfo> info;
        if (input.format == Format::Spki) {
            info = read_spki(input.bytes);
            if (!info || info->oid != oid || info->parameters)
                return dom_error(interp, "DataError", "The data is not this algorithm's SubjectPublicKeyInfo.");
            bytes = info->key;
        }
        if (bytes.size() != 32)
            return dom_error(interp, "DataError", "The public key is not 32 bytes.");
        key->type = KeyType::Public;
        std::memcpy(key->okp_public.data(), bytes.data(), 32);
        return key;
    }
    case Format::Pkcs8: {
        if (!usages_within(interp, usages, private_allowed))
            return std::nullopt;
        std::optional<KeyInfo> const info = read_pkcs8(input.bytes);
        if (!info || info->oid != oid || info->parameters)
            return dom_error(interp, "DataError", "The data is not this algorithm's PrivateKeyInfo.");
        tls::DerReader reader(info->key);
        std::optional<tls::DerElement> const inner = reader.next(tls::DerType::OctetString);
        if (!inner || !reader.at_end() || inner->content.size() != 32)
            return dom_error(interp, "DataError", "The private key is not 32 bytes.");
        key->type = KeyType::Private;
        key->secret.assign(inner->content.begin(), inner->content.end());
        key->okp_public = public_of(key->secret);
        return key;
    }
    case Format::Jwk: {
        Jwk const& jwk = input.jwk;
        bool const is_private = jwk.d.has_value();
        if (!usages_within(interp, usages, is_private ? private_allowed : public_allowed))
            return std::nullopt;
        if (!jwk.kty || *jwk.kty != "OKP")
            return dom_error(interp, "DataError", "The JWK 'kty' member is not 'OKP'.");
        if (!jwk.crv || *jwk.crv != alg_name(params.alg))
            return dom_error(interp, "DataError", "The JWK 'crv' member does not match the algorithm.");
        if (ed && jwk.alg && *jwk.alg != "Ed25519" && *jwk.alg != "EdDSA")
            return dom_error(interp, "DataError", "The JWK 'alg' member does not match the algorithm.");
        if (!jwk_checks(interp, jwk, usages, extractable, ed ? "sig" : "enc"))
            return std::nullopt;
        std::optional<Bytes> const x = jwk_bytes(interp, jwk.x, "x");
        if (!x)
            return std::nullopt;
        if (x->size() != 32)
            return dom_error(interp, "DataError", "The public key is not 32 bytes.");
        std::memcpy(key->okp_public.data(), x->data(), 32);
        key->type = KeyType::Public;
        if (is_private) {
            std::optional<Bytes> d = jwk_bytes(interp, jwk.d, "d");
            if (!d)
                return std::nullopt;
            if (d->size() != 32)
                return dom_error(interp, "DataError", "The private key is not 32 bytes.");
            if (public_of(*d) != key->okp_public)
                return dom_error(interp, "DataError", "The JWK private key does not match its public key.");
            key->type = KeyType::Private;
            key->secret = std::move(*d);
        }
        return key;
    }
    }
    return std::nullopt;
}

std::optional<std::shared_ptr<KeyData>> import_key(js::Interpreter& interp, Params const& params, KeyInput const& input, bool extractable, unsigned usages)
{
    if (is_rsa(params.alg))
        return import_rsa(interp, params, input, extractable, usages);
    if (is_ec(params.alg))
        return import_ec(interp, params, input, extractable, usages);
    if (is_okp(params.alg))
        return import_okp(interp, params, input, extractable, usages);
    return import_secret(interp, params, input, extractable, usages);
}

// ---- exporting keys

js::Value usages_array(js::Interpreter& interp, unsigned usages)
{
    js::Interpreter::Roots const roots(interp);
    std::vector<js::Value> values;
    for (std::size_t i = 0; i < usage_names.size(); ++i) {
        if ((usages & (1u << i)) != 0)
            values.push_back(interp.root(internals_of(interp).string(usage_names[i])));
    }
    return js::Value::object(interp.new_array(values));
}

void put(js::Interpreter& interp, js::Object& object, std::string_view name, js::Value const& value)
{
    interp.create_data_property(object, interp.key(name), value);
}

void put(js::Interpreter& interp, js::Object& object, std::string_view name, std::string_view text)
{
    put(interp, object, name, internals_of(interp).string(text));
}

Bytes ec_algorithm_identifier(std::string_view curve)
{
    return der_sequence({ der_oid(oid_ec_public_key), der_oid(curve_oid(curve)) });
}

Bytes rsa_algorithm_identifier()
{
    return der_sequence({ der_oid(oid_rsa), der_null() });
}

Bytes okp_algorithm_identifier(Alg alg)
{
    return der_sequence({ der_oid(alg == Alg::Ed25519 ? oid_ed25519 : oid_x25519) });
}

Bytes ec_point_bytes(KeyData const& key)
{
    return crypto::ec_encode_point(curve_id(key.named_curve), key.ec_public);
}

// The exported key: bytes for raw, spki and pkcs8, a JWK object for jwk.
Native export_key(js::Interpreter& interp, Format format, CryptoKeyObject const& object)
{
    KeyData const& key = *object.data;
    if (key.alg == Alg::Hkdf || key.alg == Alg::Pbkdf2)
        return dom_error(interp, "NotSupportedError", "This key cannot be exported.");
    if (!object.extractable)
        return dom_error(interp, "InvalidAccessError", "The key is not extractable.");
    auto const bytes = [&](View data) -> Native {
        std::optional<js::ArrayBufferObject*> const buffer = js::allocate_array_buffer(interp, nullptr, static_cast<double>(data.size()), std::nullopt);
        if (!buffer)
            return std::nullopt;
        if (!data.empty())
            std::memcpy((*buffer)->data(), data.data(), data.size());
        return js::Value::object(*buffer);
    };
    auto const require_type = [&](KeyType type) {
        if (key.type != type) {
            dom_error(interp, "InvalidAccessError", "The key is not of the type this format holds.");
            return false;
        }
        return true;
    };
    switch (format) {
    case Format::Raw:
        if (key.type == KeyType::Secret)
            return bytes(key.secret);
        if (is_ec(key.alg) && require_type(KeyType::Public))
            return bytes(ec_point_bytes(key));
        if (is_okp(key.alg) && require_type(KeyType::Public))
            return bytes(key.okp_public);
        if (interp.has_exception())
            return std::nullopt;
        return dom_error(interp, "NotSupportedError", "This key does not export as raw bytes.");
    case Format::Spki:
        if (key.type == KeyType::Secret)
            return dom_error(interp, "NotSupportedError", "A secret key does not export as SPKI.");
        if (!require_type(KeyType::Public))
            return std::nullopt;
        if (is_rsa(key.alg))
            return bytes(write_spki(rsa_algorithm_identifier(), der_sequence({ der_integer(key.rsa_public.n), der_integer(key.rsa_public.e) })));
        if (is_ec(key.alg))
            return bytes(write_spki(ec_algorithm_identifier(key.named_curve), ec_point_bytes(key)));
        return bytes(write_spki(okp_algorithm_identifier(key.alg), key.okp_public));
    case Format::Pkcs8:
        if (key.type == KeyType::Secret)
            return dom_error(interp, "NotSupportedError", "A secret key does not export as PKCS #8.");
        if (!require_type(KeyType::Private))
            return std::nullopt;
        if (is_rsa(key.alg)) {
            crypto::RsaPrivateKey const& k = *key.rsa_private;
            return bytes(write_pkcs8(rsa_algorithm_identifier(),
                der_sequence({ der_small_integer(0), der_integer(k.n), der_integer(k.e), der_integer(k.d), der_integer(k.p), der_integer(k.q),
                    der_integer(k.dp), der_integer(k.dq), der_integer(k.qi) })));
        }
        if (is_ec(key.alg)) {
            Bytes const public_key = der(0xa1, der_bit_string(ec_point_bytes(key)));
            return bytes(write_pkcs8(ec_algorithm_identifier(key.named_curve),
                der_sequence({ der_small_integer(1), der_octet_string(key.secret), public_key })));
        }
        return bytes(write_pkcs8(okp_algorithm_identifier(key.alg), der_octet_string(key.secret)));
    case Format::Jwk: {
        js::Interpreter::Roots const roots(interp);
        js::Object* jwk = interp.new_object();
        interp.root(js::Value::object(jwk));
        std::string const alg = jwk_alg(key);
        if (key.type == KeyType::Secret) {
            put(interp, *jwk, "kty", "oct");
            put(interp, *jwk, "k", base64url_encode(key.secret));
        } else if (is_rsa(key.alg)) {
            put(interp, *jwk, "kty", "RSA");
            put(interp, *jwk, "n", base64url_encode(key.rsa_public.n.to_bytes()));
            put(interp, *jwk, "e", base64url_encode(key.rsa_public.e.to_bytes()));
            if (key.type == KeyType::Private) {
                crypto::RsaPrivateKey const& k = *key.rsa_private;
                put(interp, *jwk, "d", base64url_encode(k.d.to_bytes()));
                put(interp, *jwk, "p", base64url_encode(k.p.to_bytes()));
                put(interp, *jwk, "q", base64url_encode(k.q.to_bytes()));
                put(interp, *jwk, "dp", base64url_encode(k.dp.to_bytes()));
                put(interp, *jwk, "dq", base64url_encode(k.dq.to_bytes()));
                put(interp, *jwk, "qi", base64url_encode(k.qi.to_bytes()));
            }
        } else if (is_ec(key.alg)) {
            std::size_t const size = curve_bytes(key.named_curve);
            put(interp, *jwk, "kty", "EC");
            put(interp, *jwk, "crv", key.named_curve);
            put(interp, *jwk, "x", base64url_encode(fixed_width(key.ec_public.x, size)));
            put(interp, *jwk, "y", base64url_encode(fixed_width(key.ec_public.y, size)));
            if (key.type == KeyType::Private)
                put(interp, *jwk, "d", base64url_encode(key.secret));
        } else {
            put(interp, *jwk, "kty", "OKP");
            put(interp, *jwk, "crv", alg_name(key.alg));
            put(interp, *jwk, "x", base64url_encode(key.okp_public));
            if (key.type == KeyType::Private)
                put(interp, *jwk, "d", base64url_encode(key.secret));
        }
        if (!alg.empty())
            put(interp, *jwk, "alg", alg);
        put(interp, *jwk, "key_ops", usages_array(interp, object.usages));
        put(interp, *jwk, "ext", js::Value::boolean(object.extractable));
        return js::Value::object(jwk);
    }
    }
    return std::nullopt;
}

// ---- CryptoKey (§13)

js::Value make_key(Realm::Internals& internals, std::shared_ptr<KeyData const> data, bool extractable, unsigned usages)
{
    return js::Value::object(internals.interpreter.heap().allocate<CryptoKeyObject>(internals.prototype("CryptoKey"), internals.realm_record,
        std::move(data), extractable, usages));
}

js::Value make_key(js::Interpreter& interp, std::shared_ptr<KeyData const> data, bool extractable, unsigned usages)
{
    return make_key(internals_of(interp), std::move(data), extractable, usages);
}

// [[algorithm]] as the KeyAlgorithm dictionary of its kind, made once and
// kept, so that key.algorithm is the same object every time.
js::Value algorithm_object(js::Interpreter& interp, KeyData const& key)
{
    js::Interpreter::Roots const roots(interp);
    js::Object* object = interp.new_object();
    interp.root(js::Value::object(object));
    put(interp, *object, "name", alg_name(key.alg));
    auto const hash = [&] {
        js::Object* hash_object = interp.new_object();
        interp.root(js::Value::object(hash_object));
        put(interp, *hash_object, "name", hash_name(key.hash));
        put(interp, *object, "hash", js::Value::object(hash_object));
    };
    if (is_aes(key.alg)) {
        put(interp, *object, "length", js::Value::number(static_cast<double>(key.length)));
    } else if (key.alg == Alg::Hmac) {
        hash();
        put(interp, *object, "length", js::Value::number(static_cast<double>(key.length)));
    } else if (is_rsa(key.alg)) {
        put(interp, *object, "modulusLength", js::Value::number(static_cast<double>(key.rsa_public.n.bit_length())));
        Bytes const exponent = key.rsa_public.e.to_bytes();
        Native const array = uint8_array_of(interp, exponent);
        if (array)
            put(interp, *object, "publicExponent", *array);
        hash();
    } else if (is_ec(key.alg)) {
        put(interp, *object, "namedCurve", key.named_curve);
    }
    return js::Value::object(object);
}

std::optional<CryptoKeyObject*> this_key(js::Interpreter& interp, js::Value const& this_value)
{
    if (CryptoKeyObject* key = as_key(this_value))
        return key;
    interp.throw_type_error("Illegal invocation");
    return std::nullopt;
}

// ---- the arguments every operation converts first

std::optional<CryptoKeyObject*> key_argument(js::Interpreter& interp, js::Value const& value, std::string_view operation, int position)
{
    if (CryptoKeyObject* key = as_key(value))
        return key;
    interp.throw_type_error("Failed to execute '" + std::string(operation) + "' on 'SubtleCrypto': parameter " + std::to_string(position)
        + " is not of type 'CryptoKey'.");
    return std::nullopt;
}

// A BufferSource argument: its type is checked as it is converted, and its
// bytes are copied only after the algorithm is normalized (§14.3: a getter
// the normalization runs may change or detach the buffer first).
std::optional<js::Value> bytes_argument(js::Interpreter& interp, js::Value const& value, std::string_view operation, int position)
{
    if (!buffer_source_bytes(value)) {
        interp.throw_type_error("Failed to execute '" + std::string(operation) + "' on 'SubtleCrypto': parameter " + std::to_string(position)
            + " is not of type '(ArrayBuffer or ArrayBufferView)'.");
        return std::nullopt;
    }
    return value;
}

Bytes copy_of(js::Value const& value)
{
    std::optional<View> const bytes = buffer_source_bytes(value);
    return bytes ? Bytes(bytes->begin(), bytes->end()) : Bytes {};
}

// AlgorithmIdentifier is (object or DOMString): a primitive becomes its
// string at conversion, before any step runs.
std::optional<js::Value> algorithm_argument(js::Interpreter& interp, js::Value const& value)
{
    if (value.is_object())
        return value;
    std::optional<js::JsString*> const text = interp.to_string(value);
    if (!text)
        return std::nullopt;
    return js::Value::string(*text);
}

std::optional<Format> format_argument(js::Interpreter& interp, js::Value const& value)
{
    std::optional<std::string> const text = string_of(interp, value);
    if (!text)
        return std::nullopt;
    std::optional<Format> const format = format_of(*text);
    if (!format)
        interp.throw_type_error("The provided value '" + *text + "' is not a valid enum value of type KeyFormat.");
    return format;
}

std::optional<unsigned> usages_argument(js::Interpreter& interp, js::Value const& value)
{
    if (!value.is_object()) {
        interp.throw_type_error("The key usages are not a sequence.");
        return std::nullopt;
    }
    std::optional<std::vector<js::Value>> const list = interp.iterable_to_list(value);
    if (!list)
        return std::nullopt;
    unsigned usages = 0;
    for (js::Value const& item : *list) {
        std::optional<std::string> const text = string_of(interp, item);
        if (!text)
            return std::nullopt;
        std::optional<unsigned> const bit = usage_bit(*text);
        if (!bit) {
            interp.throw_type_error("The provided value '" + *text + "' is not a valid enum value of type KeyUsage.");
            return std::nullopt;
        }
        usages |= *bit;
    }
    return usages;
}

Native buffer_of(js::Interpreter& interp, View data)
{
    std::optional<js::ArrayBufferObject*> const buffer = js::allocate_array_buffer(interp, nullptr, static_cast<double>(data.size()), std::nullopt);
    if (!buffer)
        return std::nullopt;
    if (!data.empty())
        std::memcpy((*buffer)->data(), data.data(), data.size());
    return js::Value::object(*buffer);
}

// The key's algorithm must be the one asked for and allow the usage.
bool key_allows(js::Interpreter& interp, Params const& params, CryptoKeyObject const& key, unsigned needed)
{
    if (key.data->alg != params.alg) {
        dom_error(interp, "InvalidAccessError", "The key is not a key of the requested algorithm.");
        return false;
    }
    if ((key.usages & needed) == 0) {
        dom_error(interp, "InvalidAccessError", "The key does not allow the requested operation.");
        return false;
    }
    return true;
}

// A key that is secret or private must be usable for something.
bool usable(js::Interpreter& interp, KeyData const& key, unsigned usages)
{
    if (key.type != KeyType::Public && usages == 0) {
        dom_error(interp, "SyntaxError", "Usages cannot be empty when creating a key.");
        return false;
    }
    return true;
}

Native settle(js::Interpreter& interp, Native const& result)
{
    if (!result)
        return std::nullopt;
    return resolved_promise(interp, *result);
}

// An operation of Promise type rejects where its steps throw (WebIDL
// §3.7.7, "rejecting on exception"); a terminated script stays terminated.
Native rejecting(js::Interpreter& interp, Native const& result)
{
    if (result || !interp.has_exception() || interp.terminated())
        return result;
    return rejected_promise(interp, interp.take_exception());
}

// ---- SubtleCrypto (§14)

Native cipher_operation(js::Interpreter& interp, Args args, bool encrypting)
{
    char const* const name = encrypting ? "encrypt" : "decrypt";
    std::optional<js::Value> const algorithm = algorithm_argument(interp, js::argument(args, 0));
    if (!algorithm)
        return std::nullopt;
    std::optional<CryptoKeyObject*> const key = key_argument(interp, js::argument(args, 1), name, 2);
    if (!key)
        return std::nullopt;
    std::optional<js::Value> const data_value = bytes_argument(interp, js::argument(args, 2), name, 3);
    if (!data_value)
        return std::nullopt;
    std::optional<Params> const params = normalize(interp, encrypting ? Op::Encrypt : Op::Decrypt, *algorithm);
    if (!params)
        return std::nullopt;
    Bytes const data = copy_of(*data_value);
    if (!key_allows(interp, *params, **key, encrypting ? usage::encrypt : usage::decrypt))
        return std::nullopt;
    std::optional<Bytes> const out = encrypting ? encrypt(interp, *params, *(*key)->data, data) : decrypt(interp, *params, *(*key)->data, data);
    if (!out)
        return std::nullopt;
    return settle(interp, buffer_of(interp, *out));
}

Native subtle_encrypt_steps(js::Interpreter& interp, Args args)
{
    return cipher_operation(interp, args, true);
}

Native subtle_decrypt_steps(js::Interpreter& interp, Args args)
{
    return cipher_operation(interp, args, false);
}

Native subtle_sign_steps(js::Interpreter& interp, Args args)
{
    std::optional<js::Value> const algorithm = algorithm_argument(interp, js::argument(args, 0));
    if (!algorithm)
        return std::nullopt;
    std::optional<CryptoKeyObject*> const key = key_argument(interp, js::argument(args, 1), "sign", 2);
    if (!key)
        return std::nullopt;
    std::optional<js::Value> const data_value = bytes_argument(interp, js::argument(args, 2), "sign", 3);
    if (!data_value)
        return std::nullopt;
    std::optional<Params> const params = normalize(interp, Op::Sign, *algorithm);
    if (!params)
        return std::nullopt;
    Bytes const data = copy_of(*data_value);
    if (!key_allows(interp, *params, **key, usage::sign))
        return std::nullopt;
    std::optional<Bytes> const out = sign(interp, *params, *(*key)->data, data);
    if (!out)
        return std::nullopt;
    return settle(interp, buffer_of(interp, *out));
}

Native subtle_verify_steps(js::Interpreter& interp, Args args)
{
    std::optional<js::Value> const algorithm = algorithm_argument(interp, js::argument(args, 0));
    if (!algorithm)
        return std::nullopt;
    std::optional<CryptoKeyObject*> const key = key_argument(interp, js::argument(args, 1), "verify", 2);
    if (!key)
        return std::nullopt;
    std::optional<js::Value> const signature_value = bytes_argument(interp, js::argument(args, 2), "verify", 3);
    if (!signature_value)
        return std::nullopt;
    std::optional<js::Value> const data_value = bytes_argument(interp, js::argument(args, 3), "verify", 4);
    if (!data_value)
        return std::nullopt;
    std::optional<Params> const params = normalize(interp, Op::Verify, *algorithm);
    if (!params)
        return std::nullopt;
    Bytes const signature = copy_of(*signature_value);
    Bytes const data = copy_of(*data_value);
    if (!key_allows(interp, *params, **key, usage::verify))
        return std::nullopt;
    std::optional<bool> const result = verify(interp, *params, *(*key)->data, signature, data);
    if (!result)
        return std::nullopt;
    return resolved_promise(interp, js::Value::boolean(*result));
}

Native subtle_digest_steps(js::Interpreter& interp, Args args)
{
    std::optional<js::Value> const algorithm = algorithm_argument(interp, js::argument(args, 0));
    if (!algorithm)
        return std::nullopt;
    std::optional<js::Value> const data_value = bytes_argument(interp, js::argument(args, 1), "digest", 2);
    if (!data_value)
        return std::nullopt;
    std::optional<Params> const params = normalize(interp, Op::Digest, *algorithm);
    if (!params)
        return std::nullopt;
    return settle(interp, buffer_of(interp, crypto::hash_with(hash_of(params->alg), copy_of(*data_value))));
}

Native subtle_generate_key_steps(js::Interpreter& interp, Args args)
{
    std::optional<js::Value> const algorithm = algorithm_argument(interp, js::argument(args, 0));
    if (!algorithm)
        return std::nullopt;
    bool const extractable = js::Interpreter::to_boolean(js::argument(args, 1));
    std::optional<unsigned> const usages = usages_argument(interp, js::argument(args, 2));
    if (!usages)
        return std::nullopt;
    std::optional<Params> const params = normalize(interp, Op::GenerateKey, *algorithm);
    if (!params)
        return std::nullopt;
    std::optional<Generated> const generated = generate_key(interp, *params, *usages);
    if (!generated)
        return std::nullopt;
    if (!generated->private_key) {
        if (!usable(interp, *generated->first, generated->public_usages))
            return std::nullopt;
        return resolved_promise(interp, make_key(interp, generated->first, extractable, generated->public_usages));
    }
    if (generated->private_usages == 0)
        return dom_error(interp, "SyntaxError", "Usages cannot be empty when creating a key.");
    js::Interpreter::Roots const roots(interp);
    js::Value const public_key = interp.root(make_key(interp, generated->first, true, generated->public_usages));
    js::Value const private_key = interp.root(make_key(interp, generated->private_key, extractable, generated->private_usages));
    js::Object* pair = interp.new_object();
    interp.root(js::Value::object(pair));
    put(interp, *pair, "privateKey", private_key);
    put(interp, *pair, "publicKey", public_key);
    return resolved_promise(interp, js::Value::object(pair));
}

// deriveBits's length is `optional unsigned long? length = null`, converted
// with [EnforceRange] as the browsers do.
std::optional<std::optional<double>> length_argument(js::Interpreter& interp, js::Value const& value)
{
    if (value.is_undefined() || value.is_null())
        return std::optional<double> {};
    std::optional<double> const number = enforce_range(interp, value, 4294967295.0, "length");
    if (!number)
        return std::nullopt;
    return std::optional<double> { *number };
}

Native subtle_derive_bits_steps(js::Interpreter& interp, Args args)
{
    std::optional<js::Value> const algorithm = algorithm_argument(interp, js::argument(args, 0));
    if (!algorithm)
        return std::nullopt;
    std::optional<CryptoKeyObject*> const key = key_argument(interp, js::argument(args, 1), "deriveBits", 2);
    if (!key)
        return std::nullopt;
    std::optional<std::optional<double>> const length = length_argument(interp, js::argument(args, 2));
    if (!length)
        return std::nullopt;
    std::optional<Params> const params = normalize(interp, Op::DeriveBits, *algorithm);
    if (!params || !key_allows(interp, *params, **key, usage::derive_bits))
        return std::nullopt;
    std::optional<Bytes> const out = derive_bits(interp, *params, *(*key)->data, *length);
    if (!out)
        return std::nullopt;
    return settle(interp, buffer_of(interp, *out));
}

Native subtle_derive_key_steps(js::Interpreter& interp, Args args)
{
    std::optional<js::Value> const algorithm = algorithm_argument(interp, js::argument(args, 0));
    if (!algorithm)
        return std::nullopt;
    std::optional<CryptoKeyObject*> const key = key_argument(interp, js::argument(args, 1), "deriveKey", 2);
    if (!key)
        return std::nullopt;
    std::optional<js::Value> const derived_type = algorithm_argument(interp, js::argument(args, 2));
    if (!derived_type)
        return std::nullopt;
    bool const extractable = js::Interpreter::to_boolean(js::argument(args, 3));
    std::optional<unsigned> const usages = usages_argument(interp, js::argument(args, 4));
    if (!usages)
        return std::nullopt;
    std::optional<Params> const params = normalize(interp, Op::DeriveBits, *algorithm);
    if (!params)
        return std::nullopt;
    std::optional<Params> const import_params = normalize(interp, Op::ImportKey, *derived_type);
    if (!import_params)
        return std::nullopt;
    std::optional<Params> const length_params = normalize(interp, Op::GetKeyLength, *derived_type);
    if (!length_params || !key_allows(interp, *params, **key, usage::derive_key))
        return std::nullopt;
    std::optional<KeyLength> const length = key_length(interp, *length_params);
    if (!length)
        return std::nullopt;
    std::optional<double> bits;
    if (length->bits)
        bits = static_cast<double>(*length->bits);
    std::optional<Bytes> secret = derive_bits(interp, *params, *(*key)->data, bits);
    if (!secret)
        return std::nullopt;
    KeyInput input;
    input.format = Format::Raw;
    input.bytes = std::move(*secret);
    std::optional<std::shared_ptr<KeyData>> const derived = import_key(interp, *import_params, input, extractable, *usages);
    if (!derived || !usable(interp, **derived, *usages))
        return std::nullopt;
    return resolved_promise(interp, make_key(interp, *derived, extractable, *usages));
}

// The key data argument of importKey, (BufferSource or JsonWebKey), checked
// against the format.
std::optional<KeyInput> key_input(js::Interpreter& interp, Format format, js::Value const& value)
{
    KeyInput input;
    input.format = format;
    std::optional<View> const bytes = buffer_source_bytes(value);
    if (format == Format::Jwk) {
        if (bytes) {
            interp.throw_type_error("Key data must be a JsonWebKey for the 'jwk' format.");
            return std::nullopt;
        }
        std::optional<Jwk> jwk = read_jwk(interp, value);
        if (!jwk)
            return std::nullopt;
        input.jwk = std::move(*jwk);
        return input;
    }
    if (!bytes) {
        interp.throw_type_error("Key data must be a BufferSource for non-JWK formats.");
        return std::nullopt;
    }
    input.bytes.assign(bytes->begin(), bytes->end());
    return input;
}

Native subtle_import_key_steps(js::Interpreter& interp, Args args)
{
    std::optional<Format> const format = format_argument(interp, js::argument(args, 0));
    if (!format)
        return std::nullopt;
    js::Value const key_data = js::argument(args, 1);
    if (!key_data.is_object() && !key_data.is_undefined() && !key_data.is_null())
        return interp.throw_type_error("Key data is neither a BufferSource nor a JsonWebKey.");
    std::optional<js::Value> const algorithm = algorithm_argument(interp, js::argument(args, 2));
    if (!algorithm)
        return std::nullopt;
    bool const extractable = js::Interpreter::to_boolean(js::argument(args, 3));
    std::optional<unsigned> const usages = usages_argument(interp, js::argument(args, 4));
    if (!usages)
        return std::nullopt;
    std::optional<Params> const params = normalize(interp, Op::ImportKey, *algorithm);
    if (!params)
        return std::nullopt;
    std::optional<KeyInput> const input = key_input(interp, *format, key_data);
    if (!input)
        return std::nullopt;
    std::optional<std::shared_ptr<KeyData>> const key = import_key(interp, *params, *input, extractable, *usages);
    if (!key || !usable(interp, **key, *usages))
        return std::nullopt;
    return resolved_promise(interp, make_key(interp, *key, extractable, *usages));
}

Native subtle_export_key_steps(js::Interpreter& interp, Args args)
{
    std::optional<Format> const format = format_argument(interp, js::argument(args, 0));
    if (!format)
        return std::nullopt;
    std::optional<CryptoKeyObject*> const key = key_argument(interp, js::argument(args, 1), "exportKey", 2);
    if (!key)
        return std::nullopt;
    return settle(interp, export_key(interp, *format, **key));
}

// JSON through the realm's own JSON object, for a wrapped JWK.
Native json_call(js::Interpreter& interp, std::string_view method, js::Value const& argument)
{
    js::Value const json = js::Value::object(interp.intrinsics().json);
    std::optional<js::Value> const function = interp.get(json, method);
    if (!function)
        return std::nullopt;
    js::Interpreter::Roots const roots(interp);
    interp.root(*function);
    interp.root(argument);
    js::Value const arguments[1] = { argument };
    return interp.call(*function, json, arguments);
}

// The wrap and unwrap algorithms: AES-KW's own, or any cipher's encrypt.
std::optional<Params> wrap_params(js::Interpreter& interp, Op op, js::Value const& algorithm)
{
    bool unsupported = false;
    std::optional<Params> params = normalize(interp, op, algorithm, &unsupported);
    if (params || !unsupported)
        return params;
    return normalize(interp, op == Op::WrapKey ? Op::Encrypt : Op::Decrypt, algorithm);
}

Native subtle_wrap_key_steps(js::Interpreter& interp, Args args)
{
    std::optional<Format> const format = format_argument(interp, js::argument(args, 0));
    if (!format)
        return std::nullopt;
    std::optional<CryptoKeyObject*> const key = key_argument(interp, js::argument(args, 1), "wrapKey", 2);
    if (!key)
        return std::nullopt;
    std::optional<CryptoKeyObject*> const wrapping_key = key_argument(interp, js::argument(args, 2), "wrapKey", 3);
    if (!wrapping_key)
        return std::nullopt;
    std::optional<js::Value> const algorithm = algorithm_argument(interp, js::argument(args, 3));
    if (!algorithm)
        return std::nullopt;
    std::optional<Params> const params = wrap_params(interp, Op::WrapKey, *algorithm);
    if (!params || !key_allows(interp, *params, **wrapping_key, usage::wrap_key))
        return std::nullopt;
    Native const exported = export_key(interp, *format, **key);
    if (!exported)
        return std::nullopt;
    Bytes bytes;
    if (*format == Format::Jwk) {
        Native const text = json_call(interp, "stringify", *exported);
        if (!text)
            return std::nullopt;
        std::optional<std::string> const utf8 = string_of(interp, *text);
        if (!utf8)
            return std::nullopt;
        bytes.assign(utf8->begin(), utf8->end());
    } else {
        std::optional<View> const view = buffer_source_bytes(*exported);
        bytes.assign(view->begin(), view->end());
    }
    std::optional<Bytes> wrapped;
    if (params->alg == Alg::AesKw) {
        wrapped = crypto::aes_kw_wrap(*aes_of(*(*wrapping_key)->data), bytes);
        if (!wrapped)
            return dom_error(interp, "OperationError", "AES-KW wraps data of a multiple of 8 bytes, at least 16.");
    } else {
        wrapped = encrypt(interp, *params, *(*wrapping_key)->data, bytes);
        if (!wrapped)
            return std::nullopt;
    }
    return settle(interp, buffer_of(interp, *wrapped));
}

Native subtle_unwrap_key_steps(js::Interpreter& interp, Args args)
{
    std::optional<Format> const format = format_argument(interp, js::argument(args, 0));
    if (!format)
        return std::nullopt;
    std::optional<js::Value> const wrapped_value = bytes_argument(interp, js::argument(args, 1), "unwrapKey", 2);
    if (!wrapped_value)
        return std::nullopt;
    std::optional<CryptoKeyObject*> const unwrapping_key = key_argument(interp, js::argument(args, 2), "unwrapKey", 3);
    if (!unwrapping_key)
        return std::nullopt;
    std::optional<js::Value> const algorithm = algorithm_argument(interp, js::argument(args, 3));
    if (!algorithm)
        return std::nullopt;
    std::optional<js::Value> const key_algorithm = algorithm_argument(interp, js::argument(args, 4));
    if (!key_algorithm)
        return std::nullopt;
    bool const extractable = js::Interpreter::to_boolean(js::argument(args, 5));
    std::optional<unsigned> const usages = usages_argument(interp, js::argument(args, 6));
    if (!usages)
        return std::nullopt;
    std::optional<Params> const params = wrap_params(interp, Op::UnwrapKey, *algorithm);
    if (!params)
        return std::nullopt;
    std::optional<Params> const import_params = normalize(interp, Op::ImportKey, *key_algorithm);
    if (!import_params)
        return std::nullopt;
    Bytes const wrapped = copy_of(*wrapped_value);
    if (!key_allows(interp, *params, **unwrapping_key, usage::unwrap_key))
        return std::nullopt;
    std::optional<Bytes> bytes;
    if (params->alg == Alg::AesKw) {
        bytes = crypto::aes_kw_unwrap(*aes_of(*(*unwrapping_key)->data), wrapped);
        if (!bytes)
            return dom_error(interp, "OperationError", "The wrapped key could not be unwrapped.");
    } else {
        bytes = decrypt(interp, *params, *(*unwrapping_key)->data, wrapped);
        if (!bytes)
            return std::nullopt;
    }
    KeyInput input;
    input.format = *format;
    if (*format == Format::Jwk) {
        Native const parsed = json_call(interp, "parse", internals_of(interp).string(std::string_view(reinterpret_cast<char const*>(bytes->data()), bytes->size())));
        if (!parsed) {
            interp.clear_exception();
            return dom_error(interp, "DataError", "The unwrapped key is not JSON.");
        }
        js::Interpreter::Roots const roots(interp);
        interp.root(*parsed);
        std::optional<Jwk> jwk = read_jwk(interp, *parsed);
        if (!jwk)
            return std::nullopt;
        if (!jwk->kty)
            return dom_error(interp, "DataError", "The unwrapped JWK has no 'kty' member.");
        input.jwk = std::move(*jwk);
    } else {
        input.bytes = std::move(*bytes);
    }
    std::optional<std::shared_ptr<KeyData>> const key = import_key(interp, *import_params, input, extractable, *usages);
    if (!key || !usable(interp, **key, *usages))
        return std::nullopt;
    return resolved_promise(interp, make_key(interp, *key, extractable, *usages));
}

// The operations as the prototype holds them.
Native subtle_encrypt(js::Interpreter& interp, js::Value const&, Args args)
{
    return rejecting(interp, subtle_encrypt_steps(interp, args));
}

Native subtle_decrypt(js::Interpreter& interp, js::Value const&, Args args)
{
    return rejecting(interp, subtle_decrypt_steps(interp, args));
}

Native subtle_sign(js::Interpreter& interp, js::Value const&, Args args)
{
    return rejecting(interp, subtle_sign_steps(interp, args));
}

Native subtle_verify(js::Interpreter& interp, js::Value const&, Args args)
{
    return rejecting(interp, subtle_verify_steps(interp, args));
}

Native subtle_digest(js::Interpreter& interp, js::Value const&, Args args)
{
    return rejecting(interp, subtle_digest_steps(interp, args));
}

Native subtle_generate_key(js::Interpreter& interp, js::Value const&, Args args)
{
    return rejecting(interp, subtle_generate_key_steps(interp, args));
}

Native subtle_derive_key(js::Interpreter& interp, js::Value const&, Args args)
{
    return rejecting(interp, subtle_derive_key_steps(interp, args));
}

Native subtle_derive_bits(js::Interpreter& interp, js::Value const&, Args args)
{
    return rejecting(interp, subtle_derive_bits_steps(interp, args));
}

Native subtle_import_key(js::Interpreter& interp, js::Value const&, Args args)
{
    return rejecting(interp, subtle_import_key_steps(interp, args));
}

Native subtle_export_key(js::Interpreter& interp, js::Value const&, Args args)
{
    return rejecting(interp, subtle_export_key_steps(interp, args));
}

Native subtle_wrap_key(js::Interpreter& interp, js::Value const&, Args args)
{
    return rejecting(interp, subtle_wrap_key_steps(interp, args));
}

Native subtle_unwrap_key(js::Interpreter& interp, js::Value const&, Args args)
{
    return rejecting(interp, subtle_unwrap_key_steps(interp, args));
}

}

void install_web_crypto(Realm::Internals& in, js::Object& subtle)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* key = define_interface(in, "CryptoKey", nullptr);
    define_getter(in, *key, "type", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<CryptoKeyObject*> const self = this_key(interp, this_value);
        if (!self)
            return std::nullopt;
        return internals_of(interp).string(key_type_name((*self)->data->type));
    });
    define_getter(in, *key, "extractable", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<CryptoKeyObject*> const self = this_key(interp, this_value);
        if (!self)
            return std::nullopt;
        return js::Value::boolean((*self)->extractable);
    });
    define_getter(in, *key, "algorithm", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<CryptoKeyObject*> const self = this_key(interp, this_value);
        if (!self)
            return std::nullopt;
        if ((*self)->algorithm_cached.is_undefined())
            (*self)->algorithm_cached = algorithm_object(interp, *(*self)->data);
        return (*self)->algorithm_cached;
    });
    define_getter(in, *key, "usages", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<CryptoKeyObject*> const self = this_key(interp, this_value);
        if (!self)
            return std::nullopt;
        if ((*self)->usages_cached.is_undefined())
            (*self)->usages_cached = usages_array(interp, (*self)->usages);
        return (*self)->usages_cached;
    });
    define_promise_operation(interpreter, subtle, "encrypt", 3, subtle_encrypt);
    define_promise_operation(interpreter, subtle, "decrypt", 3, subtle_decrypt);
    define_promise_operation(interpreter, subtle, "sign", 3, subtle_sign);
    define_promise_operation(interpreter, subtle, "verify", 4, subtle_verify);
    define_promise_operation(interpreter, subtle, "digest", 2, subtle_digest);
    define_promise_operation(interpreter, subtle, "generateKey", 3, subtle_generate_key);
    define_promise_operation(interpreter, subtle, "deriveKey", 5, subtle_derive_key);
    define_promise_operation(interpreter, subtle, "deriveBits", 2, subtle_derive_bits);
    define_promise_operation(interpreter, subtle, "importKey", 5, subtle_import_key);
    define_promise_operation(interpreter, subtle, "exportKey", 2, subtle_export_key);
    define_promise_operation(interpreter, subtle, "wrapKey", 4, subtle_wrap_key);
    define_promise_operation(interpreter, subtle, "unwrapKey", 7, subtle_unwrap_key);
}

// A key's clone: a version byte, the slots, the algorithm's members and the
// material, each variable part with its length in front. What IndexedDB
// keeps on disk, so the layout is read back by version.
std::optional<std::vector<std::uint8_t>> crypto_key_copy(js::Object const& object)
{
    auto const* key = dynamic_cast<CryptoKeyObject const*>(&object);
    if (key == nullptr)
        return std::nullopt;
    KeyData const& data = *key->data;
    Bytes out { 1, static_cast<std::uint8_t>(data.alg), static_cast<std::uint8_t>(data.type), static_cast<std::uint8_t>(key->extractable ? 1 : 0),
        static_cast<std::uint8_t>(data.hash) };
    auto const u32 = [&](std::size_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    auto const part = [&](View bytes) {
        u32(bytes.size());
        out.insert(out.end(), bytes.begin(), bytes.end());
    };
    u32(key->usages);
    u32(data.length);
    part(View(reinterpret_cast<std::uint8_t const*>(data.named_curve.data()), data.named_curve.size()));
    part(data.secret);
    if (is_rsa(data.alg)) {
        part(data.rsa_public.n.to_bytes());
        part(data.rsa_public.e.to_bytes());
        if (data.rsa_private) {
            crypto::RsaPrivateKey const& k = *data.rsa_private;
            for (crypto::BigInt const* value : { &k.d, &k.p, &k.q, &k.dp, &k.dq, &k.qi })
                part(value->to_bytes());
        }
    } else if (is_ec(data.alg)) {
        part(data.ec_public.x.to_bytes());
        part(data.ec_public.y.to_bytes());
    } else if (is_okp(data.alg)) {
        part(data.okp_public);
    }
    return out;
}

std::optional<js::Value> crypto_key_from_copy(Realm::Internals& in, std::span<std::uint8_t const> bytes)
{
    std::size_t at = 0;
    bool good = true;
    auto const u8 = [&]() -> std::uint8_t {
        if (at >= bytes.size()) {
            good = false;
            return 0;
        }
        return bytes[at++];
    };
    auto const u32 = [&]() -> std::size_t {
        std::size_t value = 0;
        for (int i = 0; i < 4; ++i)
            value = (value << 8) | u8();
        return value;
    };
    auto const part = [&]() -> Bytes {
        std::size_t const size = u32();
        if (!good || size > bytes.size() - at) {
            good = false;
            return {};
        }
        Bytes out(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
        return out;
    };
    auto const big = [&]() { return crypto::BigInt::from_bytes(part()).value_or(crypto::BigInt {}); };
    if (u8() != 1)
        return std::nullopt;
    auto data = std::make_shared<KeyData>();
    std::uint8_t const alg = u8();
    std::uint8_t const type = u8();
    bool const extractable = u8() != 0;
    std::uint8_t const hash = u8();
    if (alg > static_cast<std::uint8_t>(Alg::Pbkdf2) || type > static_cast<std::uint8_t>(KeyType::Private) || hash > static_cast<std::uint8_t>(crypto::HashId::Sha1))
        return std::nullopt;
    data->alg = static_cast<Alg>(alg);
    data->type = static_cast<KeyType>(type);
    data->hash = static_cast<crypto::HashId>(hash);
    unsigned const usages = static_cast<unsigned>(u32());
    data->length = u32();
    Bytes const curve = part();
    data->named_curve.assign(curve.begin(), curve.end());
    data->secret = part();
    if (is_rsa(data->alg)) {
        data->rsa_public.n = big();
        data->rsa_public.e = big();
        if (data->type == KeyType::Private) {
            crypto::RsaPrivateKey k { data->rsa_public.n, data->rsa_public.e, {}, {}, {}, {}, {}, {} };
            for (crypto::BigInt* value : { &k.d, &k.p, &k.q, &k.dp, &k.dq, &k.qi })
                *value = big();
            data->rsa_private = k;
        }
    } else if (is_ec(data->alg)) {
        data->ec_public.x = big();
        data->ec_public.y = big();
        if (!curve_supported(data->named_curve))
            good = false;
    } else if (is_okp(data->alg)) {
        Bytes const public_key = part();
        if (public_key.size() != 32)
            good = false;
        else
            std::memcpy(data->okp_public.data(), public_key.data(), 32);
    }
    if (!good || at != bytes.size())
        return std::nullopt;
    return make_key(in, std::move(data), extractable, usages);
}

}
