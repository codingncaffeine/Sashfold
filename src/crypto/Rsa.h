#pragma once
// RSA signature verification (RFC 8017): RSASSA-PKCS1-v1_5 (§8.2.2), the
// scheme of certificate signatures, and RSASSA-PSS (§8.1.2, §9.1.2), the
// scheme TLS 1.3 requires in CertificateVerify. Public keys of 2048 to
// 8192 bits. Verification only, on public values.
#include "crypto/BigInt.h"
#include "crypto/Sha2.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace sashfold::crypto {

struct RsaPublicKey {
    BigInt n;
    BigInt e;
};

// True when the key is within the accepted sizes and shapes: n of 2048
// to 8192 bits, e odd and between 3 and 2^64.
bool rsa_key_acceptable(RsaPublicKey const& key);

// `digest` is the message's hash under `hash`; the signature must be
// exactly the modulus's length in bytes.
bool rsa_verify_pkcs1_v15(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature);

// PSS with MGF1 over the same hash and a salt of `salt_length` bytes
// (TLS 1.3 fixes it to the digest length).
bool rsa_verify_pss(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature, std::size_t salt_length);

}
