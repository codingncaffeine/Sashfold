#pragma once
// COSE keys (RFC 9052 §7, RFC 9053 §2 and §7) as WebAuthn and CTAP carry
// them: a credential's public key in authenticator data, and the ECDH keys
// of the PIN/UV auth protocols. The kinds read and written:
//
//   EC2 (kty 2) on P-256, P-384, P-521 (crv 1, 2, 3): x (-2), y (-3)
//   OKP (kty 1) on Ed25519 (crv 6): x (-2)
//   RSA (kty 3, RFC 8230 §4): n (-1), e (-2)
//
// with the algorithms ES256 (-7), ES384 (-35), ES512 (-36), EdDSA (-8),
// RS256 (-257) and, for key agreement, ECDH-ES+HKDF-256 (-25). A key is
// checked when it is turned into a crypto type (the point on its curve, the
// Ed25519 encoding, RSA's modulus odd and in range), and its DER
// SubjectPublicKeyInfo — the form WebAuthn's getPublicKey() returns
// (WebAuthn Level 3 §5.2.1.1) — is written from it.
#include "auth/Cbor.h"
#include "crypto/Ec.h"
#include "crypto/Rsa.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::auth {

namespace cose {
inline constexpr std::int64_t kty_okp = 1;
inline constexpr std::int64_t kty_ec2 = 2;
inline constexpr std::int64_t kty_rsa = 3;

inline constexpr std::int64_t crv_p256 = 1;
inline constexpr std::int64_t crv_p384 = 2;
inline constexpr std::int64_t crv_p521 = 3;
inline constexpr std::int64_t crv_ed25519 = 6;

inline constexpr std::int64_t alg_es256 = -7;
inline constexpr std::int64_t alg_eddsa = -8;
inline constexpr std::int64_t alg_ecdh_es_hkdf_256 = -25;
inline constexpr std::int64_t alg_es384 = -35;
inline constexpr std::int64_t alg_es512 = -36;
inline constexpr std::int64_t alg_rs256 = -257;
}

struct CoseKey {
    std::int64_t kty = 0;
    std::optional<std::int64_t> alg;
    std::int64_t crv = 0; // EC2 and OKP
    std::vector<std::uint8_t> x; // EC2, OKP
    std::vector<std::uint8_t> y; // EC2
    std::vector<std::uint8_t> n; // RSA modulus, big-endian
    std::vector<std::uint8_t> e; // RSA exponent

    bool operator==(CoseKey const&) const = default;
};

// The key a COSE_Key map describes; nullopt for a map that is not one of
// the kinds above or misses a member it needs. Unknown labels are ignored
// (RFC 9052 §7.1 lets a key carry more).
std::optional<CoseKey> cose_key_from_cbor(cbor::Value const& value);
// The canonical map: kty, alg (when set), then the kind's parameters.
cbor::Value cose_key_to_cbor(CoseKey const& key);

CoseKey cose_key_from_ec(crypto::CurveId curve, crypto::EcPoint const& point, std::int64_t alg);
CoseKey cose_key_from_ed25519(std::span<std::uint8_t const, 32> public_key);
CoseKey cose_key_from_rsa(crypto::RsaPublicKey const& key);

// The point of an EC2 key, decoded and checked to lie on its curve.
std::optional<std::pair<crypto::CurveId, crypto::EcPoint>> cose_key_ec_point(CoseKey const& key);
std::optional<crypto::RsaPublicKey> cose_key_rsa(CoseKey const& key);

// The algorithm a key is used with: its alg member, or nullopt.
std::optional<std::int64_t> cose_key_algorithm(CoseKey const& key);

// DER SubjectPublicKeyInfo (RFC 5280 §4.1.2.7): id-ecPublicKey with the
// curve's named OID and the uncompressed point (RFC 5480), id-Ed25519
// (RFC 8410), rsaEncryption with NULL parameters (RFC 8017 A.1). Nullopt
// for a key that does not check out.
std::optional<std::vector<std::uint8_t>> cose_key_to_spki(CoseKey const& key);

// Verifies a WebAuthn signature over `data` with the key under `alg`:
// ES256/384/512 take a DER Ecdsa-Sig-Value over SHA-256/384/512 of data,
// EdDSA the 64-byte Ed25519 signature over data itself, RS256 PKCS #1 v1.5
// over SHA-256. False for any mismatch between key and algorithm.
bool cose_verify(CoseKey const& key, std::int64_t alg, std::span<std::uint8_t const> data, std::span<std::uint8_t const> signature);

}
