#pragma once
// RSA (RFC 8017). Verification for TLS: RSASSA-PKCS1-v1_5 (§8.2.2), the
// scheme of certificate signatures, and RSASSA-PSS (§8.1.2, §9.1.2), the
// scheme TLS 1.3 requires in CertificateVerify, on public keys of 2048 to
// 4096 bits. And the whole of what Web Crypto names: key generation
// (FIPS 186-5 §A.1.3's random primes, Miller–Rabin), the private operation
// by the Chinese remainder theorem, both signature schemes' signing, and
// RSAES-OAEP (§7.1) both ways, on keys of 512 to 4096 bits.
//
// The private operation exponentiates in Montgomery form with fixed 4-bit
// windows whose table is read in full at every step, so neither the
// sequence of operations nor the memory touched depends on the private
// exponent; the result is checked against the public exponent before it
// leaves (a fault would otherwise leak a factor, RFC 8017 §5.1.2 note).
#include "crypto/BigInt.h"
#include "crypto/Random.h"
#include "crypto/Sha2.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::crypto {

struct RsaPublicKey {
    BigInt n;
    BigInt e;
};

struct RsaPrivateKey {
    BigInt n;
    BigInt e;
    BigInt d;
    BigInt p;
    BigInt q;
    BigInt dp; // d mod (p − 1)
    BigInt dq; // d mod (q − 1)
    BigInt qi; // q⁻¹ mod p
};

// True when the key is within the sizes and shapes TLS accepts: n of 2048
// to 4096 bits, e odd and between 3 and 2^64.
bool rsa_key_acceptable(RsaPublicKey const& key);

// `digest` is the message's hash under `hash`; the signature must be
// exactly the modulus's length in bytes. These two refuse a key TLS would
// not accept; the _any forms take any odd modulus of 512 to 4096 bits.
bool rsa_verify_pkcs1_v15(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature);
bool rsa_verify_pkcs1_v15_any(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature);

// PSS with MGF1 over the same hash and a salt of `salt_length` bytes
// (TLS 1.3 fixes it to the digest length).
bool rsa_verify_pss(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature, std::size_t salt_length);
bool rsa_verify_pss_any(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature, std::size_t salt_length);

// A new key of `bits` bits (a multiple of 8 from 512 to 4096) with public
// exponent e (odd, at least 3); nullopt for sizes or exponents outside that.
std::optional<RsaPrivateKey> rsa_generate(std::size_t bits, BigInt const& e, RandomFill random);

// A private key from its parts as a key file or JWK carries them, checked
// for consistency (n = p·q, the CRT values match d); nullopt otherwise.
std::optional<RsaPrivateKey> rsa_private_key(BigInt const& n, BigInt const& e, BigInt const& d, BigInt const& p, BigInt const& q,
    BigInt const& dp, BigInt const& dq, BigInt const& qi);

// RSASSA-PKCS1-v1_5 and RSASSA-PSS signatures over a digest; nullopt when
// the modulus is too small for the encoding.
std::optional<std::vector<std::uint8_t>> rsa_sign_pkcs1_v15(RsaPrivateKey const& key, HashId hash, std::span<std::uint8_t const> digest);
std::optional<std::vector<std::uint8_t>> rsa_sign_pss(RsaPrivateKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::size_t salt_length, RandomFill random);

// RSAES-OAEP with MGF1 over the same hash; nullopt for a message too long
// for the key, or a ciphertext that does not decrypt.
std::optional<std::vector<std::uint8_t>> rsa_oaep_encrypt(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> label, std::span<std::uint8_t const> message, RandomFill random);
std::optional<std::vector<std::uint8_t>> rsa_oaep_decrypt(RsaPrivateKey const& key, HashId hash, std::span<std::uint8_t const> label, std::span<std::uint8_t const> ciphertext);

// The modular inverse of a modulo m by the extended Euclidean algorithm,
// for any m; nullopt when gcd(a, m) ≠ 1. Variable time: public or random
// operands only.
std::optional<BigInt> mod_inverse(BigInt const& a, BigInt const& m);

}
