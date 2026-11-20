#pragma once
// Authenticator data and attestation objects (WebAuthn Level 3 §6.1, §6.5)
// on the client's side: reading them, the attestation conveyance
// preference applied to them (§5.1.3 step 20), and the values the script
// API exposes from a new credential (§5.2.1.1: getAuthenticatorData(),
// getPublicKey(), getPublicKeyAlgorithm(), getTransports()).
//
// Verification of the attestation statement formats the keys this client
// speaks to produce — "packed" (§8.2), "fido-u2f" (§8.6) and "none" (§8.7)
// — and of assertion signatures is here too: a relying party's job, which
// the client does not do on a ceremony, but which the tests and the
// comparison tool hold the client's output to.
#include "auth/Cbor.h"
#include "auth/Cose.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sashfold::auth {

namespace flags {
inline constexpr std::uint8_t up = 0x01;
inline constexpr std::uint8_t uv = 0x04;
inline constexpr std::uint8_t be = 0x08;
inline constexpr std::uint8_t bs = 0x10;
inline constexpr std::uint8_t at = 0x40;
inline constexpr std::uint8_t ed = 0x80;
}

struct AttestedCredentialData {
    std::array<std::uint8_t, 16> aaguid {};
    std::vector<std::uint8_t> credential_id;
    CoseKey public_key;
    std::vector<std::uint8_t> public_key_cbor; // as the authenticator wrote it
};

struct AuthenticatorData {
    std::array<std::uint8_t, 32> rp_id_hash {};
    std::uint8_t flags = 0;
    std::uint32_t sign_count = 0;
    std::optional<AttestedCredentialData> attested;
    std::optional<cbor::Value> extensions; // the authenticator extension outputs map

    bool user_present() const { return (flags & flags::up) != 0; }
    bool user_verified() const { return (flags & flags::uv) != 0; }
};

// §6.1: the fixed 37 bytes, the attested credential data when AT is set,
// the extensions map when ED is set, and nothing after. Nullopt for any
// length that does not add up, a credential ID over 1023 bytes, or a
// public key that is not a COSE key.
std::optional<AuthenticatorData> parse_authenticator_data(std::span<std::uint8_t const> bytes);

struct AttestationObject {
    std::string fmt;
    cbor::Value att_stmt = cbor::Value::map();
    std::vector<std::uint8_t> auth_data;
    AuthenticatorData parsed;
};

// The {"fmt", "attStmt", "authData"} map of §6.5.4, read as general CBOR.
std::optional<AttestationObject> parse_attestation_object(std::span<std::uint8_t const> bytes);
// The map written canonically (which is the order §6.5.4's example has).
std::vector<std::uint8_t> encode_attestation_object(std::string const& fmt, cbor::Value const& att_stmt, std::span<std::uint8_t const> auth_data);

enum class AttestationConveyance : std::uint8_t { None, Indirect, Direct, Enterprise };

// §5.1.3 step 20. For "none": self attestation (a zero AAGUID, "packed",
// no x5c) is left as it is; anything else becomes fmt "none" with an empty
// statement. The authenticator data, AAGUID included, is unchanged: Level
// 3 no longer replaces the AAGUID. "indirect" is passed through unchanged
// (the client may anonymize it and this one does not); "direct" and
// "enterprise" are passed through by definition.
AttestationObject apply_conveyance(AttestationObject const& object, AttestationConveyance preference);

// What the script API reads off a new credential (§5.2.1.1).
struct CreatedCredentialFields {
    std::vector<std::uint8_t> credential_id;
    std::vector<std::uint8_t> authenticator_data; // getAuthenticatorData()
    std::optional<std::vector<std::uint8_t>> public_key_spki; // getPublicKey(): null for an algorithm we cannot express
    std::int64_t public_key_algorithm = 0; // getPublicKeyAlgorithm()
    std::vector<std::string> transports; // getTransports(), sorted, no duplicates
};
std::optional<CreatedCredentialFields> created_credential_fields(AttestationObject const& object, std::vector<std::string> transports);

enum class StatementCheck : std::uint8_t { Valid, Invalid, Unsupported };

// The statement's signature over authData || clientDataHash under the
// format's rules: "packed" with x5c (the leaf certificate's key, its alg
// ES256/384/512) or self (the credential's own key and alg); "fido-u2f"
// (§8.6 verification step 5's data, the one certificate's P-256 key);
// "none" (an empty statement). Certificate chains are not judged.
StatementCheck verify_attestation_statement(AttestationObject const& object, std::span<std::uint8_t const> client_data_hash);

// An assertion's signature over authenticatorData || clientDataHash
// (§7.2 step 20).
bool verify_assertion_signature(CoseKey const& key, std::int64_t alg, std::span<std::uint8_t const> authenticator_data,
    std::span<std::uint8_t const> client_data_hash, std::span<std::uint8_t const> signature);

}
