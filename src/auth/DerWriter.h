#pragma once
// The few DER (ITU-T X.690) constructions the security-key code writes: a
// SubjectPublicKeyInfo for getPublicKey(), an ECDSA signature's
// SEQUENCE { r, s }, and (for the test authenticator) a certificate. Each
// returns the whole element: tag, shortest definite length, content.
// Reading DER is net/tls/Der.h's.
#include "crypto/BigInt.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::auth::der {

std::vector<std::uint8_t> element(std::uint8_t tag, std::span<std::uint8_t const> content);
std::vector<std::uint8_t> sequence(std::initializer_list<std::span<std::uint8_t const>> parts);
std::vector<std::uint8_t> set(std::initializer_list<std::span<std::uint8_t const>> parts);
// An INTEGER from a big-endian magnitude: leading zeros dropped, one zero
// put back where the high bit would read as a sign.
std::vector<std::uint8_t> integer(std::span<std::uint8_t const> magnitude);
std::vector<std::uint8_t> small_integer(std::uint64_t value);
// An OBJECT IDENTIFIER from its dotted form.
std::vector<std::uint8_t> oid(std::string_view dotted);
std::vector<std::uint8_t> null();
// A BIT STRING with no unused bits.
std::vector<std::uint8_t> bit_string(std::span<std::uint8_t const> bytes);
std::vector<std::uint8_t> octet_string(std::span<std::uint8_t const> bytes);
std::vector<std::uint8_t> utf8_string(std::string_view text);
// GeneralizedTime from "YYYYMMDDHHMMSSZ".
std::vector<std::uint8_t> generalized_time(std::string_view text);
// A context-specific constructed [n].
std::vector<std::uint8_t> explicit_tag(std::uint8_t number, std::span<std::uint8_t const> content);

// Ecdsa-Sig-Value (RFC 3279 §2.2.3, SEC 1 §C.8): the form WebAuthn's
// ES256, ES384 and ES512 signatures and U2F's take.
std::vector<std::uint8_t> ecdsa_signature(crypto::BigInt const& r, crypto::BigInt const& s);
std::optional<std::pair<crypto::BigInt, crypto::BigInt>> parse_ecdsa_signature(std::span<std::uint8_t const> der);

}
