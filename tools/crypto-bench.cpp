// crypto_bench times the public-key operations the TLS client performs per
// connection, on this machine: an ECDSA verification on each curve, the
// two halves of an ECDH key agreement, the field multiplication and
// inversion they are made of (next to BigInt's general-purpose ones, which
// RSA still uses), and an RSA PKCS#1 v1.5 verification at 2048 and 4096
// bits. The signatures are not valid — verification does its whole scalar
// multiplication or modular exponentiation before it can say so — and the
// generator stands in for a public key.
//
//   cmake --build build --target crypto_bench && build/crypto_bench

#include "crypto/BigInt.h"
#include "crypto/Ec.h"
#include "crypto/Rsa.h"
#include "crypto/Sha2.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace sashfold::crypto;

namespace {

template<typename F>
double us_per(int iterations, F&& f)
{
    auto const start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i)
        f();
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / iterations;
}

} // namespace

int main()
{
    std::uint8_t digest[48];
    for (std::size_t i = 0; i < sizeof digest; ++i)
        digest[i] = static_cast<std::uint8_t>(i * 37 + 11);

    for (CurveId const id : { CurveId::P256, CurveId::P384 }) {
        Curve const& c = curve(id);
        char const* name = id == CurveId::P256 ? "P-256" : "P-384";
        EcPoint const key { c.gx, c.gy };
        BigInt const r = c.n.sub(BigInt::from_u64(12345));
        BigInt const s = c.n.sub(BigInt::from_u64(67890));
        std::span<std::uint8_t const> const d(digest, c.field_bytes);
        std::vector<std::uint8_t> scalar(c.field_bytes);
        for (std::size_t i = 0; i < scalar.size(); ++i)
            scalar[i] = static_cast<std::uint8_t>(i * 7 + 3);
        // The first call builds the generator's table; time it apart.
        double const first = us_per(1, [&] { (void)ecdsa_verify(id, key, d, r, s); });
        double const verify = us_per(200, [&] { (void)ecdsa_verify(id, key, d, r, s); });
        double const public_point = us_per(200, [&] { (void)ec_public_point(id, scalar); });
        double const shared = us_per(200, [&] { (void)ecdh_shared_x(id, scalar, key); });
        int const chain = 1000000;
        BigInt sink;
        double const field_mul = us_per(1, [&] { sink = ec_test::field_multiply_chain(id, c.gx, c.gy, chain); }) / chain;
        double const field_inverse = us_per(2000, [&] { sink = ec_test::field_inverse(id, c.gx); });
        double const bigint_mul = us_per(20000, [&] { sink = c.gx.mod_mul(c.gy, c.p); });
        double const bigint_inverse = us_per(50, [&] { sink = c.gx.mod_inverse_prime(c.p); });
        std::printf("%s ECDSA verify %.1f us (first call %.0f us)   ECDH public point %.1f us, shared x %.1f us\n", name, verify,
            first, public_point, shared);
        std::printf("%s field mul %.1f ns, inverse %.2f us   BigInt mod_mul %.1f ns, mod_inverse_prime %.1f us   (%zu)\n", name,
            field_mul * 1000.0, field_inverse, bigint_mul * 1000.0, bigint_inverse, sink.bit_length());
    }

    for (std::size_t const bits : { std::size_t(2048), std::size_t(4096) }) {
        std::vector<std::uint8_t> modulus(bits / 8, 0xC3);
        modulus.front() |= 0x80;
        modulus.back() |= 1;
        RsaPublicKey const key { *BigInt::from_bytes(modulus), BigInt::from_u64(65537) };
        std::vector<std::uint8_t> signature(bits / 8, 0x5A);
        signature.front() = 0x01;
        double const verify = us_per(20, [&] {
            (void)rsa_verify_pkcs1_v15(key, HashId::Sha256, std::span<std::uint8_t const>(digest, 32), signature);
        });
        std::printf("RSA-%zu PKCS#1 v1.5 verify: %.1f us\n", bits, verify);
    }
    return 0;
}
