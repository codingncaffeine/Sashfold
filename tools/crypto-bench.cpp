// crypto_bench times the public-key verifications the TLS client performs
// per certificate chain, on this machine: an ECDSA verification on each
// curve, an RSA PKCS#1 v1.5 verification at 2048 and 4096 bits, and the
// modular multiplication and inversion they are made of. The signatures are
// not valid — verification does its whole scalar multiplication or modular
// exponentiation before it can say so — and the generator stands in for a
// public key.
//
//   g++ -std=c++23 -O2 -I src -I . tools/crypto-bench.cpp build-gcc/libsashfold_core.a -o crypto_bench
//
// Dev-only, not a CMake target: it links libsashfold_core.a, so RELINK it
// after every core build or it times old code.

#include "crypto/BigInt.h"
#include "crypto/Ec.h"
#include "crypto/Rsa.h"
#include "crypto/Sha2.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace sashfold::crypto;

namespace {

template<typename F>
double ms_per(int iterations, F&& f)
{
    auto const start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i)
        f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / iterations;
}

} // namespace

int main()
{
    std::uint8_t digest[48];
    for (std::size_t i = 0; i < sizeof digest; ++i)
        digest[i] = static_cast<std::uint8_t>(i * 37 + 11);

    for (CurveId const id : { CurveId::P256, CurveId::P384 }) {
        Curve const& c = curve(id);
        EcPoint const key { c.gx, c.gy };
        BigInt const r = c.n.sub(BigInt::from_u64(12345));
        BigInt const s = c.n.sub(BigInt::from_u64(67890));
        std::span<std::uint8_t const> const d(digest, c.field_bytes);
        double const verify = ms_per(3, [&] { (void)ecdsa_verify(id, key, d, r, s); });
        double const mul = ms_per(2000, [&] { (void)c.gx.mod_mul(c.gy, c.p); });
        double const inverse = ms_per(10, [&] { (void)c.gx.mod_inverse_prime(c.p); });
        std::printf("ECDSA %s verify: %.0f ms   (mod_mul %.1f us, mod_inverse_prime %.1f ms)\n",
            id == CurveId::P256 ? "P-256" : "P-384", verify, mul * 1000.0, inverse);
    }

    for (std::size_t const bits : { std::size_t(2048), std::size_t(4096) }) {
        std::vector<std::uint8_t> modulus(bits / 8, 0xC3);
        modulus.front() |= 0x80;
        modulus.back() |= 1;
        RsaPublicKey const key { *BigInt::from_bytes(modulus), BigInt::from_u64(65537) };
        std::vector<std::uint8_t> signature(bits / 8, 0x5A);
        signature.front() = 0x01;
        double const verify = ms_per(5, [&] {
            (void)rsa_verify_pkcs1_v15(key, HashId::Sha256, std::span<std::uint8_t const>(digest, 32), signature);
        });
        std::printf("RSA-%zu PKCS#1 v1.5 verify: %.0f ms\n", bits, verify);
    }
    return 0;
}
