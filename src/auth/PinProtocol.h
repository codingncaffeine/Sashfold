#pragma once
// The PIN/UV auth protocols one and two of CTAP 2.1 §6.5.6 and §6.5.7: the
// key agreement that gives the platform and the authenticator a shared
// secret, and the encryption and MAC built on it. They protect the PIN on
// its way to the key, the pinUvAuthToken on its way back, the
// pinUvAuthParam that proves a command came from the platform that holds
// the token, and hmac-secret's salts and outputs.
//
//   one: shared secret SHA-256(Z); AES-256-CBC with a zero IV; MAC = the
//        first 16 bytes of HMAC-SHA-256.
//   two: shared secret HKDF(Z, "CTAP2 HMAC key") || HKDF(Z, "CTAP2 AES
//        key"); AES-256-CBC under the second half with a random IV sent in
//        front; MAC = HMAC-SHA-256 under the first 32 bytes, all 32 bytes.
//
// Z is the x-coordinate of the shared P-256 point. Both sides are here:
// the platform's (the browser) and, for the test authenticator, the
// authenticator's, since they are the same functions.
#include "auth/Cose.h"
#include "crypto/Random.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::auth::pin {

enum class Protocol : std::uint8_t { One = 1, Two = 2 };

std::optional<Protocol> protocol_from_number(std::int64_t number);

// kdf(Z) for the protocol: 32 bytes for one, 64 for two.
std::vector<std::uint8_t> kdf(Protocol protocol, std::span<std::uint8_t const> z);

// A P-256 key-agreement key pair.
struct KeyAgreementKey {
    std::array<std::uint8_t, 32> scalar {};
    CoseKey public_key; // kty 2, alg -25 (ECDH-ES+HKDF-256), crv 1
};
// A fresh pair from `random`; nullopt only if the source keeps giving
// scalars outside [1, n − 1].
std::optional<KeyAgreementKey> generate_key_agreement_key(crypto::RandomFill random);
// The pair for a given scalar (tests, and the specification's vectors).
std::optional<KeyAgreementKey> key_agreement_key_from_scalar(std::span<std::uint8_t const, 32> scalar);

// ecdh(peerCoseKey): kdf of the shared point's x-coordinate; nullopt for a
// peer key that is not a P-256 point on the curve.
std::optional<std::vector<std::uint8_t>> ecdh(Protocol protocol, KeyAgreementKey const& own, CoseKey const& peer);

struct Encapsulation {
    CoseKey platform_key; // sent to the authenticator as keyAgreement
    std::vector<std::uint8_t> shared_secret;
};
// encapsulate(peerCoseKey) with a fresh platform key.
std::optional<Encapsulation> encapsulate(Protocol protocol, CoseKey const& authenticator_key, crypto::RandomFill random);

// encrypt(key, demPlaintext): the plaintext must be a whole number of AES
// blocks. Protocol two draws its IV from `random`.
std::optional<std::vector<std::uint8_t>> encrypt(Protocol protocol, std::span<std::uint8_t const> key, std::span<std::uint8_t const> plaintext,
    crypto::RandomFill random);
// Protocol two with a given IV (the specification's test vectors fix it).
std::optional<std::vector<std::uint8_t>> encrypt_with_iv(std::span<std::uint8_t const> key, std::span<std::uint8_t const, 16> iv,
    std::span<std::uint8_t const> plaintext);
std::optional<std::vector<std::uint8_t>> decrypt(Protocol protocol, std::span<std::uint8_t const> key, std::span<std::uint8_t const> ciphertext);

// authenticate(key, message): 16 bytes for one, 32 for two.
std::vector<std::uint8_t> authenticate(Protocol protocol, std::span<std::uint8_t const> key, std::span<std::uint8_t const> message);
// verify(key, message, signature), in constant time over the signature.
bool verify(Protocol protocol, std::span<std::uint8_t const> key, std::span<std::uint8_t const> message, std::span<std::uint8_t const> signature);

// The PIN as setPIN and changePIN send it (§6.5.5.5): its UTF-8 padded
// with zeros to 64 bytes. Nullopt for a PIN under 4 code points or over 63
// bytes, which the authenticator would refuse anyway.
std::optional<std::vector<std::uint8_t>> padded_pin(std::string_view pin);
// LEFT(SHA-256(pin), 16): what getPinToken and changePIN send.
std::array<std::uint8_t, 16> pin_hash(std::string_view pin);

}
