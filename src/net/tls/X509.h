#pragma once
// X.509 certificates and CRLs (RFC 5280) as the validator and the TLS
// client read them: the fields and the extensions the policy of
// _plans/tls-DESIGN.md §6 consults, no more. The DER is kept whole, so a
// signature is checked over the exact bytes the issuer signed and names
// are compared as the bytes they were written in.
#include "crypto/Ec.h"
#include "crypto/Rsa.h"
#include "crypto/Sha2.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::tls {

// A distinguished name: its DER for equality, its attributes for display.
struct Name {
    std::vector<std::uint8_t> der;
    std::string common_name;
    std::string display; // "CN=…, O=…, C=…"

    bool operator==(Name const& other) const { return der == other.der; }
};

enum class SignatureKind : std::uint8_t { RsaPkcs1, RsaPss, Ecdsa, Unsupported };

struct SignatureAlgorithm {
    SignatureKind kind = SignatureKind::Unsupported;
    crypto::HashId hash = crypto::HashId::Sha256;
    std::size_t pss_salt_length = 0;
    std::string oid;
};

struct PublicKey {
    enum class Kind : std::uint8_t { Rsa, Ec, Unsupported };
    Kind kind = Kind::Unsupported;
    crypto::RsaPublicKey rsa;
    crypto::CurveId curve = crypto::CurveId::P256;
    crypto::EcPoint ec;
    std::vector<std::uint8_t> spki; // the SubjectPublicKeyInfo DER, for key equality
};

struct GeneralNames {
    std::vector<std::string> dns_names; // as written, ASCII
    std::vector<std::vector<std::uint8_t>> ip_addresses; // 4 or 16 bytes each
    std::vector<std::string> uris;
    bool has_other = false; // a name of a kind we do not read
};

struct NameConstraints {
    std::vector<std::string> permitted_dns; // ".example.com" or "example.com"
    std::vector<std::string> excluded_dns;
    bool permitted_has_other_kinds = false;
};

struct Certificate {
    std::vector<std::uint8_t> der;
    std::size_t tbs_offset = 0;
    std::size_t tbs_length = 0;
    std::span<std::uint8_t const> tbs() const { return std::span<std::uint8_t const>(der).subspan(tbs_offset, tbs_length); }

    int version = 1; // 1, 2 or 3
    std::vector<std::uint8_t> serial;
    SignatureAlgorithm signature_algorithm; // the outer one; the inner must match
    Name issuer;
    Name subject;
    std::int64_t not_before = 0;
    std::int64_t not_after = 0;
    PublicKey public_key;
    std::vector<std::uint8_t> signature;

    // Extensions.
    bool has_basic_constraints = false;
    bool is_ca = false;
    std::optional<std::uint64_t> path_length;
    bool has_key_usage = false;
    bool key_usage_digital_signature = false;
    bool key_usage_key_cert_sign = false;
    bool key_usage_crl_sign = false;
    bool has_extended_key_usage = false;
    bool eku_server_auth = false;
    bool eku_any = false;
    bool has_subject_alt_name = false;
    GeneralNames subject_alt_name;
    std::optional<NameConstraints> name_constraints;
    std::vector<std::uint8_t> subject_key_identifier;
    std::vector<std::uint8_t> authority_key_identifier;
    std::vector<std::string> crl_distribution_points; // http URLs only
    std::vector<std::string> ocsp_responders;
    bool unknown_critical_extension = false;

    bool is_self_issued() const { return issuer == subject; }
};

struct Crl {
    std::vector<std::uint8_t> der;
    std::size_t tbs_offset = 0;
    std::size_t tbs_length = 0;
    std::span<std::uint8_t const> tbs() const { return std::span<std::uint8_t const>(der).subspan(tbs_offset, tbs_length); }

    SignatureAlgorithm signature_algorithm;
    Name issuer;
    std::int64_t this_update = 0;
    std::optional<std::int64_t> next_update;
    std::vector<std::vector<std::uint8_t>> revoked_serials;
    std::vector<std::uint8_t> authority_key_identifier;
    std::vector<std::uint8_t> signature;

    bool revokes(std::span<std::uint8_t const> serial) const;
};

std::optional<Certificate> parse_certificate(std::span<std::uint8_t const> der);
std::optional<Crl> parse_crl(std::span<std::uint8_t const> der);

// Every "-----BEGIN <label>-----" block's bytes, in order; blocks that do
// not decode are skipped.
std::vector<std::vector<std::uint8_t>> pem_decode(std::string_view text, std::string_view label = "CERTIFICATE");

// A signature over `signed_bytes` under the signer's key: RSA PKCS#1
// v1.5 and PSS, ECDSA with the DER-wrapped (r, s).
bool verify_signature(PublicKey const& signer, SignatureAlgorithm const& algorithm, std::span<std::uint8_t const> signed_bytes, std::span<std::uint8_t const> signature);

// A public key from the SubjectPublicKeyInfo DER alone (the TLS client
// reads the leaf's this way too).
std::optional<PublicKey> parse_public_key(std::span<std::uint8_t const> spki);

// The dotted OIDs the parser knows, for callers that name them.
namespace oid {
inline constexpr std::string_view rsa_encryption = "1.2.840.113549.1.1.1";
inline constexpr std::string_view sha256_with_rsa = "1.2.840.113549.1.1.11";
inline constexpr std::string_view sha384_with_rsa = "1.2.840.113549.1.1.12";
inline constexpr std::string_view sha512_with_rsa = "1.2.840.113549.1.1.13";
inline constexpr std::string_view rsassa_pss = "1.2.840.113549.1.1.10";
inline constexpr std::string_view mgf1 = "1.2.840.113549.1.1.8";
inline constexpr std::string_view ec_public_key = "1.2.840.10045.2.1";
inline constexpr std::string_view prime256v1 = "1.2.840.10045.3.1.7";
inline constexpr std::string_view secp384r1 = "1.3.132.0.34";
inline constexpr std::string_view ecdsa_with_sha256 = "1.2.840.10045.4.3.2";
inline constexpr std::string_view ecdsa_with_sha384 = "1.2.840.10045.4.3.3";
inline constexpr std::string_view ecdsa_with_sha512 = "1.2.840.10045.4.3.4";
inline constexpr std::string_view sha256 = "2.16.840.1.101.3.4.2.1";
inline constexpr std::string_view sha384 = "2.16.840.1.101.3.4.2.2";
inline constexpr std::string_view sha512 = "2.16.840.1.101.3.4.2.3";
inline constexpr std::string_view common_name = "2.5.4.3";
inline constexpr std::string_view basic_constraints = "2.5.29.19";
inline constexpr std::string_view key_usage = "2.5.29.15";
inline constexpr std::string_view extended_key_usage = "2.5.29.37";
inline constexpr std::string_view subject_alt_name = "2.5.29.17";
inline constexpr std::string_view name_constraints = "2.5.29.30";
inline constexpr std::string_view subject_key_identifier = "2.5.29.14";
inline constexpr std::string_view authority_key_identifier = "2.5.29.35";
inline constexpr std::string_view crl_distribution_points = "2.5.29.31";
inline constexpr std::string_view certificate_policies = "2.5.29.32";
inline constexpr std::string_view authority_info_access = "1.3.6.1.5.5.7.1.1";
inline constexpr std::string_view ocsp = "1.3.6.1.5.5.7.48.1";
inline constexpr std::string_view server_auth = "1.3.6.1.5.5.7.3.1";
inline constexpr std::string_view any_extended_key_usage = "2.5.29.37.0";
}

}
