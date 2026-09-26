// The primitives under src/crypto against the vectors their standards
// publish: FIPS 180-4 (SHA-2), RFC 4231 (HMAC), RFC 5869 (HKDF), RFC 8439
// (ChaCha20-Poly1305), FIPS 197 and SP 800-38A/38D (AES-128 and AES-128-GCM),
// RFC 7748 (X25519), RFC 6979 (ECDSA), RFC 8017 (RSA, with a key and
// signatures made by OpenSSL). A vector that fails is a wrong
// implementation, never a wrong vector.
#include "Test.h"

#include "crypto/Aes.h"
#include "crypto/AesGcm.h"
#include "crypto/BigInt.h"
#include "crypto/ChaCha20Poly1305.h"
#include "crypto/Ec.h"
#include "crypto/Hkdf.h"
#include "crypto/Hmac.h"
#include "crypto/Rsa.h"
#include "crypto/Sha2.h"
#include "crypto/X25519.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace sashfold;

namespace {

std::string hex(std::span<std::uint8_t const> bytes)
{
    std::string out;
    char buffer[3];
    for (std::uint8_t const b : bytes) {
        std::snprintf(buffer, sizeof buffer, "%02x", b);
        out += buffer;
    }
    return out;
}

std::vector<std::uint8_t> from_hex(std::string_view text)
{
    std::vector<std::uint8_t> out;
    auto nibble = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9')
            return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f')
            return static_cast<std::uint8_t>(c - 'a' + 10);
        return static_cast<std::uint8_t>(c - 'A' + 10);
    };
    for (std::size_t i = 0; i + 1 < text.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>((nibble(text[i]) << 4) | nibble(text[i + 1])));
    return out;
}

std::span<std::uint8_t const> ascii(std::string_view s)
{
    return { reinterpret_cast<std::uint8_t const*>(s.data()), s.size() };
}

void test_sha2()
{
    // FIPS 180-4 examples, and the one-million-a message of the SHA test.
    CHECK_EQ(hex(crypto::Sha256::hash(ascii("abc"))), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(hex(crypto::Sha256::hash(ascii(""))), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(hex(crypto::Sha256::hash(ascii("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK_EQ(hex(crypto::Sha384::hash(ascii("abc"))),
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
    CHECK_EQ(hex(crypto::Sha384::hash(ascii(""))),
        "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b");
    CHECK_EQ(hex(crypto::Sha512::hash(ascii("abc"))),
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK_EQ(hex(crypto::Sha512::hash(ascii(""))),
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    // Streaming in odd pieces reaches the same digest as one call: the
    // buffer logic across block boundaries.
    std::vector<std::uint8_t> const million(1000000, static_cast<std::uint8_t>('a'));
    crypto::Sha256 streamed;
    std::size_t offset = 0;
    for (std::size_t piece = 1; offset < million.size(); piece = piece * 3 % 200 + 1) {
        std::size_t const take = std::min(piece, million.size() - offset);
        streamed.update(std::span<std::uint8_t const>(million.data() + offset, take));
        offset += take;
    }
    CHECK_EQ(hex(streamed.finish()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK_EQ(hex(crypto::Sha256::hash(million)), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    crypto::Sha512 streamed512;
    for (std::size_t i = 0; i < million.size(); i += 129)
        streamed512.update(std::span<std::uint8_t const>(million.data() + i, std::min<std::size_t>(129, million.size() - i)));
    CHECK_EQ(hex(streamed512.finish()),
        "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
    // finish() starts the object over.
    CHECK_EQ(hex(streamed.finish()), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(hex(crypto::hash_with(crypto::HashId::Sha384, ascii("abc"))),
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
}

void test_hmac()
{
    // RFC 4231 test cases 1, 2 and 6 (a key longer than the block).
    std::vector<std::uint8_t> const key1(20, 0x0b);
    CHECK_EQ(hex(crypto::Hmac<crypto::Sha256>::mac(key1, ascii("Hi There"))),
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK_EQ(hex(crypto::Hmac<crypto::Sha384>::mac(key1, ascii("Hi There"))),
        "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59cfaea9ea9076ede7f4af152e8b2fa9cb6");
    CHECK_EQ(hex(crypto::Hmac<crypto::Sha512>::mac(key1, ascii("Hi There"))),
        "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854");
    CHECK_EQ(hex(crypto::Hmac<crypto::Sha256>::mac(ascii("Jefe"), ascii("what do ya want for nothing?"))),
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    std::vector<std::uint8_t> const key6(131, 0xaa);
    CHECK_EQ(hex(crypto::Hmac<crypto::Sha256>::mac(key6, ascii("Test Using Larger Than Block-Size Key - Hash Key First"))),
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    CHECK_EQ(hex(crypto::Hmac<crypto::Sha512>::mac(key6, ascii("Test Using Larger Than Block-Size Key - Hash Key First"))),
        "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f3526b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598");
    // The streaming form agrees with the one-shot form.
    crypto::Hmac<crypto::Sha256> streamed(ascii("Jefe"));
    streamed.update(ascii("what do ya "));
    streamed.update(ascii("want for nothing?"));
    CHECK_EQ(hex(streamed.finish()), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(crypto::constant_time_equal(ascii("abc"), ascii("abc")));
    CHECK(!crypto::constant_time_equal(ascii("abc"), ascii("abd")));
    CHECK(!crypto::constant_time_equal(ascii("abc"), ascii("ab")));
}

void test_hkdf()
{
    // RFC 5869 test cases 1 and 3 (SHA-256; the third with empty salt and info).
    std::vector<std::uint8_t> const ikm(22, 0x0b);
    std::vector<std::uint8_t> const salt = from_hex("000102030405060708090a0b0c");
    std::vector<std::uint8_t> const info = from_hex("f0f1f2f3f4f5f6f7f8f9");
    crypto::Sha256::Digest const prk = crypto::hkdf_extract<crypto::Sha256>(salt, ikm);
    CHECK_EQ(hex(prk), "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    CHECK_EQ(hex(crypto::hkdf_expand<crypto::Sha256>(prk, info, 42)),
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    crypto::Sha256::Digest const prk3 = crypto::hkdf_extract<crypto::Sha256>({}, ikm);
    CHECK_EQ(hex(prk3), "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04");
    CHECK_EQ(hex(crypto::hkdf_expand<crypto::Sha256>(prk3, {}, 42)),
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");
    CHECK(crypto::hkdf_expand<crypto::Sha256>(prk, info, 255 * 32 + 1).empty());
    CHECK_EQ(crypto::hkdf_expand<crypto::Sha256>(prk, info, 255 * 32).size(), std::size_t(255 * 32));
}

}
void test_chacha20_poly1305()
{
    // RFC 8439 §2.3.2 (the block function), §2.4.2 (encryption), §2.5.2
    // (Poly1305) and §2.8.2 (the AEAD), every byte the RFC's.
    crypto::ChaChaKey key;
    for (std::size_t i = 0; i < 32; ++i)
        key[i] = static_cast<std::uint8_t>(i);
    crypto::ChaChaNonce const block_nonce = { 0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    CHECK_EQ(hex(crypto::chacha20_block(key, 1, block_nonce)),
        "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e");
    std::string_view const sunscreen = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    crypto::ChaChaNonce const nonce = { 0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    std::vector<std::uint8_t> text(sunscreen.begin(), sunscreen.end());
    crypto::chacha20_xor(key, 1, nonce, text);
    CHECK_EQ(hex(text),
        "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d");
    crypto::chacha20_xor(key, 1, nonce, text);
    CHECK_EQ(std::string(text.begin(), text.end()), std::string(sunscreen));
    crypto::Poly1305Key poly_key;
    std::vector<std::uint8_t> const poly_key_bytes = from_hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    std::copy(poly_key_bytes.begin(), poly_key_bytes.end(), poly_key.begin());
    CHECK_EQ(hex(crypto::poly1305_mac(poly_key, ascii("Cryptographic Forum Research Group"))), "a8061dc1305136c6c22b8baf0c0127a9");
    crypto::ChaChaKey aead_key;
    for (std::size_t i = 0; i < 32; ++i)
        aead_key[i] = static_cast<std::uint8_t>(0x80 + i);
    crypto::ChaChaNonce const aead_nonce = { 0x07, 0, 0, 0, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47 };
    std::vector<std::uint8_t> const aad = from_hex("50515253c0c1c2c3c4c5c6c7");
    std::vector<std::uint8_t> sealed(sunscreen.size());
    crypto::Poly1305Tag const tag = crypto::chacha20_poly1305_seal(aead_key, aead_nonce, aad, ascii(sunscreen), sealed);
    CHECK_EQ(hex(sealed),
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116");
    CHECK_EQ(hex(tag), "1ae10b594f09e26a7e902ecbd0600691");
    // Opening in place, a wrong tag, a changed AAD, a changed byte.
    std::vector<std::uint8_t> opened = sealed;
    CHECK(crypto::chacha20_poly1305_open(aead_key, aead_nonce, aad, opened, tag, opened));
    CHECK_EQ(std::string(opened.begin(), opened.end()), std::string(sunscreen));
    crypto::Poly1305Tag wrong = tag;
    wrong[0] ^= 1;
    std::vector<std::uint8_t> sink(sealed.size());
    CHECK(!crypto::chacha20_poly1305_open(aead_key, aead_nonce, aad, sealed, wrong, sink));
    CHECK(!crypto::chacha20_poly1305_open(aead_key, aead_nonce, ascii("x"), sealed, tag, sink));
    std::vector<std::uint8_t> tampered = sealed;
    tampered[10] ^= 1;
    CHECK(!crypto::chacha20_poly1305_open(aead_key, aead_nonce, aad, tampered, tag, sink));
    // Empty plaintext and AAD still authenticate.
    std::vector<std::uint8_t> nothing;
    crypto::Poly1305Tag const empty_tag = crypto::chacha20_poly1305_seal(aead_key, aead_nonce, {}, {}, nothing);
    CHECK(crypto::chacha20_poly1305_open(aead_key, aead_nonce, {}, {}, empty_tag, nothing));
}

void test_x25519()
{
    // RFC 7748 §5.2 (two scalar-point pairs, then the iterated test) and
    // §6.1 (the Alice/Bob exchange).
    auto key = [](std::string_view h) {
        crypto::X25519Key k;
        std::vector<std::uint8_t> const bytes = from_hex(h);
        std::copy(bytes.begin(), bytes.end(), k.begin());
        return k;
    };
    crypto::X25519Key out;
    CHECK(crypto::x25519(out, key("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"), key("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")));
    CHECK_EQ(hex(out), "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    CHECK(crypto::x25519(out, key("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"), key("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493")));
    CHECK_EQ(hex(out), "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");
    crypto::X25519Key k = key("0900000000000000000000000000000000000000000000000000000000000000");
    crypto::X25519Key u = k;
    for (int i = 0; i < 1000; ++i) {
        crypto::X25519Key next;
        crypto::x25519(next, k, u);
        u = k;
        k = next;
        if (i == 0)
            CHECK_EQ(hex(k), "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079");
    }
    CHECK_EQ(hex(k), "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51");
    crypto::X25519Key const alice = key("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    crypto::X25519Key const bob = key("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    CHECK_EQ(hex(crypto::x25519_public(alice)), "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    CHECK_EQ(hex(crypto::x25519_public(bob)), "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    crypto::X25519Key shared_a, shared_b;
    CHECK(crypto::x25519(shared_a, alice, crypto::x25519_public(bob)));
    CHECK(crypto::x25519(shared_b, bob, crypto::x25519_public(alice)));
    CHECK_EQ(hex(shared_a), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    CHECK_EQ(hex(shared_b), hex(shared_a));
    // A low-order point gives the all-zero result the client must refuse.
    crypto::X25519Key zero {};
    CHECK(!crypto::x25519(out, alice, zero));
}

void test_bigint()
{
    using crypto::BigInt;
    CHECK_EQ(BigInt::from_hex("00ff")->to_hex(), "ff");
    CHECK_EQ(BigInt::from_hex("0")->to_hex(), "0");
    CHECK_EQ(BigInt::from_u64(0x123456789abcdef0).to_hex(), "123456789abcdef0");
    CHECK_EQ(BigInt::from_hex("ffffffffffffffff")->add(BigInt::from_u64(1)).to_hex(), "010000000000000000");
    CHECK_EQ(BigInt::from_hex("10000000000000000")->sub(BigInt::from_u64(1)).to_hex(), "ffffffffffffffff");
    CHECK_EQ(BigInt::from_hex("ffffffffffffffff")->mul(*BigInt::from_hex("ffffffffffffffff")).to_hex(), "fffffffffffffffe0000000000000001");
    CHECK_EQ(BigInt::from_hex("123456789abcdef0123456789abcdef0")->mul(*BigInt::from_hex("fedcba9876543210")).to_hex(),
        "121fa00ad77d7422358d29092d964322236d88fe5618cf00");
    BigInt quotient, remainder;
    BigInt::from_hex("121fa00ad77d7422358d29092d964322236d88fe5618cf01")->divmod(*BigInt::from_hex("fedcba9876543210"), quotient, remainder);
    CHECK_EQ(quotient.to_hex(), "123456789abcdef0123456789abcdef0");
    CHECK_EQ(remainder.to_hex(), "01");
    CHECK_EQ(BigInt::from_u64(4).mod_pow(BigInt::from_u64(13), BigInt::from_u64(497)).to_hex(), "01bd"); // 445
    CHECK_EQ(BigInt::from_u64(3).mod_inverse_prime(BigInt::from_u64(11)).to_hex(), "04"); // 3·4 = 12 ≡ 1
    CHECK_EQ(BigInt::from_u64(7).mod_sub(BigInt::from_u64(9), BigInt::from_u64(11)).to_hex(), "09");
    CHECK_EQ(BigInt::from_u64(7).mod_add(BigInt::from_u64(9), BigInt::from_u64(11)).to_hex(), "05");
    CHECK_EQ(BigInt::from_u64(1).shift_left(200).shift_right(199).to_hex(), "02");
    CHECK_EQ(BigInt::from_hex("abcdef")->bit_length(), std::size_t(24));
    CHECK(!BigInt::from_bytes(std::vector<std::uint8_t>(1025, 1)));
    CHECK(BigInt::from_bytes(std::vector<std::uint8_t>(1024, 1)).has_value());
    CHECK_EQ(hex(*BigInt::from_u64(258).to_bytes(4)), "00000102");
    CHECK(!BigInt::from_u64(258).to_bytes(1));

    // The division held to its own identity — quotient × divisor + remainder
    // is the dividend and the remainder is below the divisor — over operands
    // shaped to reach every branch of the long division: limbs of all ones,
    // a lone top bit, a top limb of one or two (the full normalisation), and
    // random limbs, at every size the curves and RSA use and at the capacity.
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;
    auto next = [&seed] {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    auto shaped = [&next](std::size_t limbs) {
        std::vector<std::uint8_t> bytes(limbs * 8);
        std::uint64_t const shape = next() % 5;
        for (std::size_t i = 0; i < limbs; ++i) {
            std::uint64_t limb = next();
            if (shape == 1)
                limb = ~std::uint64_t(0);
            else if (shape == 2)
                limb = std::uint64_t(1) << 63;
            else if (shape == 3 && i + 1 == limbs)
                limb = 1 + next() % 2;
            else if (shape == 4 && i % 2 == 0)
                limb = 0;
            for (std::size_t b = 0; b < 8; ++b)
                bytes[(limbs - 1 - i) * 8 + (7 - b)] = static_cast<std::uint8_t>(limb >> (8 * b));
        }
        return *BigInt::from_bytes(bytes);
    };
    auto holds = [](BigInt const& u, BigInt const& v) {
        BigInt q, r;
        u.divmod(v, q, r);
        return r < v && q.mul(v).add(r) == u;
    };
    int identity_failures = 0;
    for (int round = 0; round < 4000; ++round) {
        std::size_t const divisor_limbs = 1 + next() % 8;
        std::size_t const dividend_limbs = divisor_limbs + next() % 9;
        BigInt const u = shaped(dividend_limbs);
        BigInt v = shaped(divisor_limbs);
        if (v.is_zero())
            v = BigInt::from_u64(1 + next());
        if (!holds(u, v))
            ++identity_failures;
    }
    CHECK_EQ(identity_failures, 0);
    BigInt const full = *BigInt::from_bytes(std::vector<std::uint8_t>(1024, 0xFF));
    CHECK(holds(full, BigInt::from_u64(3)));
    CHECK(holds(full, *BigInt::from_bytes(std::vector<std::uint8_t>(512, 0x80))));
    CHECK(holds(full, full.shift_right(1).add(BigInt::from_u64(7))));
    CHECK(holds(full, full));
    CHECK(holds(BigInt::from_u64(5), BigInt::from_u64(9)));
    CHECK(holds(BigInt(), BigInt::from_u64(9)));
    // A divisor with a lone top bit above a dividend limb of all ones is the
    // case whose trial quotient starts one too large.
    CHECK(holds(*BigInt::from_hex("7fffffffffffffffffffffffffffffffffffffffffffffff"), *BigInt::from_hex("800000000000000000000000000000000000000000000001")));
    CHECK(holds(*BigInt::from_hex("ffffffffffffffff0000000000000000ffffffffffffffff"), *BigInt::from_hex("ffffffffffffffff0000000000000001")));
    // A prime field's multiplication, the way the curves use it.
    BigInt const p384 = *BigInt::from_hex("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff");
    BigInt const a = p384.sub(BigInt::from_u64(1));
    CHECK_EQ(a.mod_mul(a, p384).to_hex(), "01"); // (p − 1)² ≡ 1
    CHECK_EQ(a.mod_inverse_prime(p384).to_hex(), a.to_hex()); // p − 1 is its own inverse
}

void test_rsa()
{
    // A 2048-bit key and two signatures of "sample" under SHA-256 made
    // with OpenSSL 3.6 (genpkey, dgst -sign; PSS with a 32-byte salt) —
    // fixed inputs, not a dependency.
    crypto::RsaPublicKey const key { *crypto::BigInt::from_hex("00bb40bd9eec426db5cc1965f2c3d5a68b71dd470c808c5e7c17146c8a663a2fc69fa989d423e4cf28b862e459ec426cee2f0f5b9d579f2b44b4481919320f11934c7f3cbf2c3a7154100649d55194832da994b1d11f59c58ab34c92eeec9d613e1227375cdebadc95e1e1f5428f722c541465d353e8c53b4189e221f36b4a703dd3c9f5cfd03bc2c0f574040c8dc450d011ae9cfe235fd2db4cabca41e5cdf56385818eb04a78235aaef96682852560ff5a5280e5be6e7178209002c998425bf229033deeeb69d099b627bac4d6b0ab064af768cf694362cb6a831edb9feeebf0f8d3fb07411cf4f4d13c324a789e432959e8ed006e87acd48a8811310cd19c83"), crypto::BigInt::from_u64(65537) };
    CHECK(crypto::rsa_key_acceptable(key));
    crypto::Sha256::Digest const digest = crypto::Sha256::hash(ascii("sample"));
    std::vector<std::uint8_t> const sig15 = from_hex("19e83a2332c9419eba56e94e09ea222f1c71184f084e155d59c53607ef8ea4d2e2caf8929c13a4470878db6817dd13899c12784fa33692c06128cc19b5c974a772308c8d45f0ac0e11e32a6112feda19ee414cdd6985d9b92f5da5f123cced2e096743afda9c32197a971d2e8ae0174c0d22fc532de46a53cd8bd936ed9e816c3782343b688ea79fe81a4285b0133563efaa42e40ce075cdebed748aaa3a863c6d37150624206913158986ab1e2920b9c963960c922045f4cabf1de7182bccea44434d41f7788d8dcb77bb8f48e01478cbe59a7b97dcb4e056e3e577f2adeb70056b7295e850ee349866075ecfe4b9ed9dd8cd8e9def2c518ae3d7fee195f10b");
    std::vector<std::uint8_t> const sigpss = from_hex("5ce5dda2d9074de420fa898a664000541f7334050a97c7654ccefa57ad35c132bc86601112af080f76045bf2ae0aea575d81e37112f8a29e602ec906f7d151a0335eef98bea93797412898ea623b2c033a3e4bb2cbb674f435e97b608959485a127b7dd0654df4ac043978417a421bfeabb1ce55294bf8ac3c97ab86ee02aa7967cf144235aa281324e8acdbeba9d7982556e824c853633ff92fa6ee809fc5aad58572c399f440567d1dab39753e9608af428a9a6e39dc23e73479360bc749b655004bc21a1ce5c29874d91bb3313787f801e55444185062b28aaa810297de9f5233d8fab5fc2c55375bfecde47201c05ab80274ef62c7a4a5c9ab06563bfd73");
    CHECK(crypto::rsa_verify_pkcs1_v15(key, crypto::HashId::Sha256, digest, sig15));
    CHECK(crypto::rsa_verify_pss(key, crypto::HashId::Sha256, digest, sigpss, 32));
    // The wrong scheme, hash, salt length, digest or a flipped byte all fail.
    CHECK(!crypto::rsa_verify_pkcs1_v15(key, crypto::HashId::Sha256, digest, sigpss));
    CHECK(!crypto::rsa_verify_pss(key, crypto::HashId::Sha256, digest, sig15, 32));
    CHECK(!crypto::rsa_verify_pss(key, crypto::HashId::Sha256, digest, sigpss, 20));
    CHECK(!crypto::rsa_verify_pkcs1_v15(key, crypto::HashId::Sha256, crypto::Sha256::hash(ascii("sampl")), sig15));
    std::vector<std::uint8_t> flipped = sig15;
    flipped[100] ^= 1;
    CHECK(!crypto::rsa_verify_pkcs1_v15(key, crypto::HashId::Sha256, digest, flipped));
    CHECK(!crypto::rsa_verify_pkcs1_v15(key, crypto::HashId::Sha256, digest, std::span<std::uint8_t const>(sig15).subspan(1)));
    crypto::RsaPublicKey const small { *crypto::BigInt::from_hex("c5"), crypto::BigInt::from_u64(3) };
    CHECK(!crypto::rsa_key_acceptable(small));
}

void test_ecdsa()
{
    // RFC 6979 A.2.5 (P-256: "sample" and "test" under SHA-256, "sample"
    // under SHA-384, which exercises the digest truncation) and A.2.6
    // (P-384 under SHA-384); each verified independently with OpenSSL.
    using crypto::BigInt;
    auto point = [](crypto::CurveId id, std::string_view x, std::string_view y) {
        std::string encoded = "04";
        encoded += x;
        encoded += y;
        return crypto::ec_decode_point(id, from_hex(encoded));
    };
    std::optional<crypto::EcPoint> const p256 = point(crypto::CurveId::P256,
        "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6", "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299");
    CHECK(p256.has_value());
    CHECK(crypto::ecdsa_verify(crypto::CurveId::P256, *p256, crypto::Sha256::hash(ascii("sample")),
        *BigInt::from_hex("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716"), *BigInt::from_hex("F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8")));
    CHECK(crypto::ecdsa_verify(crypto::CurveId::P256, *p256, crypto::Sha256::hash(ascii("test")),
        *BigInt::from_hex("F1ABB023518351CD71D881567B1EA663ED3EFCF6C5132B354F28D3B0B7D38367"), *BigInt::from_hex("019F4113742A2B14BD25926B49C649155F267E60D3814B4C0CC84250E46F0083")));
    CHECK(crypto::ecdsa_verify(crypto::CurveId::P256, *p256, crypto::Sha384::hash(ascii("sample")),
        *BigInt::from_hex("0EAFEA039B20E9B42309FB1D89E213057CBF973DC0CFC8F129EDDDC800EF7719"), *BigInt::from_hex("4861F0491E6998B9455193E34E7B0D284DDD7149A74B95B9261F13ABDE940954")));
    // The wrong message, a changed r, s out of range, and a point off the curve.
    CHECK(!crypto::ecdsa_verify(crypto::CurveId::P256, *p256, crypto::Sha256::hash(ascii("test")),
        *BigInt::from_hex("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716"), *BigInt::from_hex("F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8")));
    CHECK(!crypto::ecdsa_verify(crypto::CurveId::P256, *p256, crypto::Sha256::hash(ascii("sample")),
        *BigInt::from_hex("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3717"), *BigInt::from_hex("F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8")));
    CHECK(!crypto::ecdsa_verify(crypto::CurveId::P256, *p256, crypto::Sha256::hash(ascii("sample")),
        *BigInt::from_hex("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716"), BigInt()));
    CHECK(!point(crypto::CurveId::P256, "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB7", "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299").has_value());
    CHECK(!crypto::ec_decode_point(crypto::CurveId::P256, from_hex("0260FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6")).has_value());
    std::optional<crypto::EcPoint> const p384 = point(crypto::CurveId::P384,
        "EC3A4E415B4E19A4568618029F427FA5DA9A8BC4AE92E02E06AAE5286B300C64DEF8F0EA9055866064A254515480BC13",
        "8015D9B72D7D57244EA8EF9AC0C621896708A59367F9DFB9F54CA84B3F1C9DB1288B231C3AE0D4FE7344FD2533264720");
    CHECK(p384.has_value());
    BigInt const r384 = *BigInt::from_hex("94EDBB92A5ECB8AAD4736E56C691916B3F88140666CE9FA73D64C4EA95AD133C81A648152E44ACF96E36DD1E80FABE46");
    BigInt const s384 = *BigInt::from_hex("99EF4AEB15F178CEA1FE40DB2603138F130E740A19624526203B6351D0A3A94FA329C145786E679E7B82C71A38628AC8");
    CHECK(crypto::ecdsa_verify(crypto::CurveId::P384, *p384, crypto::Sha384::hash(ascii("sample")), r384, s384));
    CHECK(!crypto::ecdsa_verify(crypto::CurveId::P384, *p384, crypto::Sha384::hash(ascii("test")), r384, s384));
    // The cost is held as well as the answer: a P-384 verification stays
    // under a second even under the sanitizers. With the reduction written
    // as bit-by-bit long division it took 570 ms without them, and a
    // certificate chain costs two of these before a page can start.
    auto const start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i)
        CHECK(crypto::ecdsa_verify(crypto::CurveId::P384, *p384, crypto::Sha384::hash(ascii("sample")), r384, s384));
    double const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 3;
    CHECK(ms < 1000.0);
}

void test_ecdh()
{
    // ECDH over P-256 as a TLS 1.2 key exchange uses it: the generator for
    // a scalar of one, the shared secret the same from both ends, a point
    // off the curve refused, and the two scalars the reduction leaves at
    // zero refused. The live handshake test is the check against another
    // implementation; this is the check against itself.
    using crypto::BigInt;
    crypto::Curve const& c = crypto::curve(crypto::CurveId::P256);
    std::vector<std::uint8_t> one(32, 0);
    one.back() = 1;
    std::optional<crypto::EcPoint> const g = crypto::ec_public_point(crypto::CurveId::P256, one);
    CHECK(g.has_value());
    CHECK(g->x == c.gx && g->y == c.gy);
    CHECK_EQ(hex(crypto::ec_encode_point(crypto::CurveId::P256, *g)).substr(0, 10), "046b17d1f2");
    std::vector<std::uint8_t> a(32);
    std::vector<std::uint8_t> b(32);
    for (std::size_t i = 0; i < 32; ++i) {
        a[i] = static_cast<std::uint8_t>(i * 7 + 3);
        b[i] = static_cast<std::uint8_t>(255 - i * 5);
    }
    std::optional<crypto::EcPoint> const pa = crypto::ec_public_point(crypto::CurveId::P256, a);
    std::optional<crypto::EcPoint> const pb = crypto::ec_public_point(crypto::CurveId::P256, b);
    CHECK(pa.has_value() && pb.has_value());
    // Round trip through the wire form.
    std::optional<crypto::EcPoint> const decoded = crypto::ec_decode_point(crypto::CurveId::P256, crypto::ec_encode_point(crypto::CurveId::P256, *pa));
    CHECK(decoded.has_value() && decoded->x == pa->x && decoded->y == pa->y);
    std::optional<std::vector<std::uint8_t>> const ab = crypto::ecdh_shared_x(crypto::CurveId::P256, a, *pb);
    std::optional<std::vector<std::uint8_t>> const ba = crypto::ecdh_shared_x(crypto::CurveId::P256, b, *pa);
    CHECK(ab.has_value() && ba.has_value());
    CHECK(ab.has_value() && ba.has_value() && *ab == *ba && ab->size() == 32);
    // The ladder agrees with the public double-and-add through the signature
    // path: k·G for the same k reached both ways is the same point.
    std::optional<std::vector<std::uint8_t>> const via_generator = crypto::ecdh_shared_x(crypto::CurveId::P256, a, *g);
    CHECK(via_generator.has_value() && *via_generator == *pa->x.to_bytes(32));
    crypto::EcPoint off { pb->x, pb->y.add(BigInt::from_u64(1)) };
    CHECK(!crypto::ecdh_shared_x(crypto::CurveId::P256, a, off).has_value());
    std::vector<std::uint8_t> const zero(32, 0);
    CHECK(!crypto::ec_public_point(crypto::CurveId::P256, zero).has_value());
    CHECK(!crypto::ec_public_point(crypto::CurveId::P256, *c.n.to_bytes(32)).has_value());
    CHECK(!crypto::ec_public_point(crypto::CurveId::P256, std::vector<std::uint8_t>(31, 1)).has_value());
}

// The public keys of RFC 6979 A.2.5 and A.2.6.
constexpr std::string_view rfc6979_p256_key = "0460FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB67903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299";
constexpr std::string_view rfc6979_p384_key = "04EC3A4E415B4E19A4568618029F427FA5DA9A8BC4AE92E02E06AAE5286B300C64DEF8F0EA9055866064A254515480BC13"
                                              "8015D9B72D7D57244EA8EF9AC0C621896708A59367F9DFB9F54CA84B3F1C9DB1288B231C3AE0D4FE7344FD2533264720";

std::vector<std::uint8_t> rfc6979_digest(std::string_view hash, std::string_view message)
{
    // SHA-224 is not implemented here; its digests of the two messages are given.
    if (hash == "SHA-224")
        return from_hex(message == "sample" ? "9003e374bc726550c2c289447fd0533160f875709386dfa377bfd41c" : "90a3ed9e32b2aaf4c61c410eb925426119e1a9dc53d4286ade99a809");
    if (hash == "SHA-256") {
        auto const d = crypto::Sha256::hash(ascii(message));
        return { d.begin(), d.end() };
    }
    if (hash == "SHA-384") {
        auto const d = crypto::Sha384::hash(ascii(message));
        return { d.begin(), d.end() };
    }
    auto const d = crypto::Sha512::hash(ascii(message));
    return { d.begin(), d.end() };
}

// Every signature RFC 6979 A.2.5 (P-256) and A.2.6 (P-384) gives, "sample"
// and "test" under SHA-224, SHA-256, SHA-384 and SHA-512: digests shorter
// than the order, as long, and longer (truncated to the order's bits).
// OpenSSL's deterministic signing reproduces each and node's verify accepts
// each. Each signature must also fail over the other message.
void test_ecdsa_rfc6979()
{
    using crypto::BigInt;
    using crypto::CurveId;
    struct Vector {
        CurveId id;
        std::string_view hash;
        std::string_view message;
        std::string_view r;
        std::string_view s;
    };
    static constexpr Vector vectors[] = {
        { CurveId::P256, "SHA-224", "sample", "53B2FFF5D1752B2C689DF257C04C40A587FABABB3F6FC2702F1343AF7CA9AA3F", "B9AFB64FDC03DC1A131C7D2386D11E349F070AA432A4ACC918BEA988BF75C74C" },
        { CurveId::P256, "SHA-224", "test", "C37EDB6F0AE79D47C3C27E962FA269BB4F441770357E114EE511F662EC34A692", "C820053A05791E521FCAAD6042D40AEA1D6B1A540138558F47D0719800E18F2D" },
        { CurveId::P256, "SHA-256", "sample", "EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716", "F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8" },
        { CurveId::P256, "SHA-256", "test", "F1ABB023518351CD71D881567B1EA663ED3EFCF6C5132B354F28D3B0B7D38367", "019F4113742A2B14BD25926B49C649155F267E60D3814B4C0CC84250E46F0083" },
        { CurveId::P256, "SHA-384", "sample", "0EAFEA039B20E9B42309FB1D89E213057CBF973DC0CFC8F129EDDDC800EF7719", "4861F0491E6998B9455193E34E7B0D284DDD7149A74B95B9261F13ABDE940954" },
        { CurveId::P256, "SHA-384", "test", "83910E8B48BB0C74244EBDF7F07A1C5413D61472BD941EF3920E623FBCCEBEB6", "8DDBEC54CF8CD5874883841D712142A56A8D0F218F5003CB0296B6B509619F2C" },
        { CurveId::P256, "SHA-512", "sample", "8496A60B5E9B47C825488827E0495B0E3FA109EC4568FD3F8D1097678EB97F00", "2362AB1ADBE2B8ADF9CB9EDAB740EA6049C028114F2460F96554F61FAE3302FE" },
        { CurveId::P256, "SHA-512", "test", "461D93F31B6540894788FD206C07CFA0CC35F46FA3C91816FFF1040AD1581A04", "39AF9F15DE0DB8D97E72719C74820D304CE5226E32DEDAE67519E840D1194E55" },
        { CurveId::P384, "SHA-224", "sample", "42356E76B55A6D9B4631C865445DBE54E056D3B3431766D0509244793C3F9366450F76EE3DE43F5A125333A6BE060122", "9DA0C81787064021E78DF658F2FBB0B042BF304665DB721F077A4298B095E4834C082C03D83028EFBF93A3C23940CA8D" },
        { CurveId::P384, "SHA-224", "test", "E8C9D0B6EA72A0E7837FEA1D14A1A9557F29FAA45D3E7EE888FC5BF954B5E62464A9A817C47FF78B8C11066B24080E72", "07041D4A7A0379AC7232FF72E6F77B6DDB8F09B16CCE0EC3286B2BD43FA8C6141C53EA5ABEF0D8231077A04540A96B66" },
        { CurveId::P384, "SHA-256", "sample", "21B13D1E013C7FA1392D03C5F99AF8B30C570C6F98D4EA8E354B63A21D3DAA33BDE1E888E63355D92FA2B3C36D8FB2CD", "F3AA443FB107745BF4BD77CB3891674632068A10CA67E3D45DB2266FA7D1FEEBEFDC63ECCD1AC42EC0CB8668A4FA0AB0" },
        { CurveId::P384, "SHA-256", "test", "6D6DEFAC9AB64DABAFE36C6BF510352A4CC27001263638E5B16D9BB51D451559F918EEDAF2293BE5B475CC8F0188636B", "2D46F3BECBCC523D5F1A1256BF0C9B024D879BA9E838144C8BA6BAEB4B53B47D51AB373F9845C0514EEFB14024787265" },
        { CurveId::P384, "SHA-384", "sample", "94EDBB92A5ECB8AAD4736E56C691916B3F88140666CE9FA73D64C4EA95AD133C81A648152E44ACF96E36DD1E80FABE46", "99EF4AEB15F178CEA1FE40DB2603138F130E740A19624526203B6351D0A3A94FA329C145786E679E7B82C71A38628AC8" },
        { CurveId::P384, "SHA-384", "test", "8203B63D3C853E8D77227FB377BCF7B7B772E97892A80F36AB775D509D7A5FEB0542A7F0812998DA8F1DD3CA3CF023DB", "DDD0760448D42D8A43AF45AF836FCE4DE8BE06B485E9B61B827C2F13173923E06A739F040649A667BF3B828246BAA5A5" },
        { CurveId::P384, "SHA-512", "sample", "ED0959D5880AB2D869AE7F6C2915C6D60F96507F9CB3E047C0046861DA4A799CFE30F35CC900056D7C99CD7882433709", "512C8CCEEE3890A84058CE1E22DBC2198F42323CE8ACA9135329F03C068E5112DC7CC3EF3446DEFCEB01A45C2667FDD5" },
        { CurveId::P384, "SHA-512", "test", "A0D5D090C9980FAF3C2CE57B7AE951D31977DD11C775D314AF55F76C676447D06FB6495CD21B4B6E340FC236584FB277", "976984E59B4C77B0E8E4460DCA3D9F20E07B9BB1F63BEEFAF576F6B2E8B224634A2092CD3792E0159AD9CEE37659C736" },
    };
    std::optional<crypto::EcPoint> const p256 = crypto::ec_decode_point(CurveId::P256, from_hex(rfc6979_p256_key));
    std::optional<crypto::EcPoint> const p384 = crypto::ec_decode_point(CurveId::P384, from_hex(rfc6979_p384_key));
    CHECK(p256.has_value() && p384.has_value());
    if (!p256 || !p384)
        return;
    for (Vector const& v : vectors) {
        crypto::EcPoint const& key = v.id == CurveId::P256 ? *p256 : *p384;
        std::string const name = std::string(v.id == CurveId::P256 ? "P-256 " : "P-384 ") + std::string(v.hash) + " " + std::string(v.message);
        BigInt const r = *BigInt::from_hex(v.r);
        BigInt const s = *BigInt::from_hex(v.s);
        bool const valid = crypto::ecdsa_verify(v.id, key, rfc6979_digest(v.hash, v.message), r, s);
        CHECK_EQ(name + (valid ? " verifies" : " fails"), name + " verifies");
        bool const other = crypto::ecdsa_verify(v.id, key, rfc6979_digest(v.hash, v.message == "sample" ? "test" : "sample"), r, s);
        CHECK_EQ(name + (other ? " verifies the other message" : " refuses the other message"), name + " refuses the other message");
    }
}

// A deterministic stream for the randomised checks below.
struct SplitMix64 {
    std::uint64_t state;
    std::uint64_t next()
    {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
        return z ^ (z >> 31);
    }
};

crypto::BigInt random_scalar(SplitMix64& rng, crypto::Curve const& c)
{
    std::vector<std::uint8_t> bytes(c.field_bytes);
    for (std::uint8_t& b : bytes)
        b = static_cast<std::uint8_t>(rng.next() >> 56);
    return crypto::BigInt::from_bytes(bytes)->mod(c.n);
}

bool same_point(std::optional<crypto::EcPoint> const& a, std::optional<crypto::EcPoint> const& b)
{
    if (!a || !b)
        return !a && !b;
    return a->x == b->x && a->y == b->y;
}

crypto::EcPoint negate(crypto::Curve const& c, crypto::EcPoint const& p)
{
    return crypto::EcPoint { p.x, c.p.sub(p.y) };
}

// A signature made here over BigInt, the textbook way (SEC 1 §4.1.3), from
// a private key d and a nonce k: r = x(k·G) mod n, s = k⁻¹(e + r·d) mod n.
struct Signature {
    crypto::BigInt r;
    crypto::BigInt s;
};

Signature sign(crypto::CurveId id, crypto::BigInt const& d, crypto::BigInt const& k, crypto::BigInt const& e)
{
    crypto::Curve const& c = crypto::curve(id);
    std::optional<crypto::EcPoint> const r_point = crypto::ec_test::multiply(id, crypto::ec_test::Method::Reference, { c.gx, c.gy }, k);
    crypto::BigInt const r = r_point->x.mod(c.n);
    crypto::BigInt const s = k.mod_inverse_prime(c.n).mod_mul(e.mod(c.n).mod_add(r.mod_mul(d, c.n), c.n), c.n);
    return { r, s };
}

// The ranges and cases SEC 1 §4.1.4 and FIPS 186-4 §6.4.2 name, each
// checked on the side where a wrong verifier would say yes: r and s at 0
// and at n, s + n (which a verifier that reduced instead of refusing would
// accept), coordinates past p that are congruent to a point on the curve,
// the encodings the decoder must refuse, a digest longer than the order
// whose truncated-away bits must not matter, a signature whose x is r + n
// (x mod n = r with x ≥ n, reachable only on a verifier that knows it), a
// sum that is the point at infinity, and a sum whose two halves are the
// same point.
void test_ecdsa_edges()
{
    using crypto::BigInt;
    using crypto::CurveId;
    using crypto::ec_test::Method;
    std::optional<crypto::EcPoint> const key = crypto::ec_decode_point(CurveId::P256, from_hex(rfc6979_p256_key));
    CHECK(key.has_value());
    if (!key)
        return;
    crypto::Curve const& c = crypto::curve(CurveId::P256);
    auto const digest = crypto::Sha256::hash(ascii("sample"));
    BigInt const r = *BigInt::from_hex("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716");
    BigInt const s = *BigInt::from_hex("F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8");
    BigInt const one = BigInt::from_u64(1);
    CHECK(crypto::ecdsa_verify(CurveId::P256, *key, digest, r, s));
    CHECK(!crypto::ecdsa_verify(CurveId::P256, *key, digest, BigInt(), s));
    CHECK(!crypto::ecdsa_verify(CurveId::P256, *key, digest, r, BigInt()));
    CHECK(!crypto::ecdsa_verify(CurveId::P256, *key, digest, c.n, s));
    CHECK(!crypto::ecdsa_verify(CurveId::P256, *key, digest, r, c.n));
    CHECK(!crypto::ecdsa_verify(CurveId::P256, *key, digest, c.n.sub(one), s));
    CHECK(!crypto::ecdsa_verify(CurveId::P256, *key, digest, r, s.add(c.n)));
    CHECK(!crypto::ecdsa_verify(CurveId::P256, *key, digest, r.add(c.n), s));
    // The key with x or y moved up by p: the same point modulo p, refused.
    CHECK(!crypto::ecdsa_verify(CurveId::P256, { key->x.add(c.p), key->y }, digest, r, s));
    CHECK(!crypto::ecdsa_verify(CurveId::P256, { key->x, key->y.add(c.p) }, digest, r, s));
    CHECK(!crypto::ecdsa_verify(CurveId::P256, { key->x, key->y.add(one) }, digest, r, s));
    // Encodings: a wrong prefix, x = p, no prefix, the one-byte point at
    // infinity, and a compressed point, which the decoder does not take.
    std::vector<std::uint8_t> encoded = from_hex(rfc6979_p256_key);
    encoded[0] = 0x05;
    CHECK(!crypto::ec_decode_point(CurveId::P256, encoded).has_value());
    encoded[0] = 0x04;
    CHECK(crypto::ec_decode_point(CurveId::P256, encoded).has_value());
    std::vector<std::uint8_t> x_is_p = encoded;
    std::vector<std::uint8_t> const p_bytes = *c.p.to_bytes(32);
    std::copy(p_bytes.begin(), p_bytes.end(), x_is_p.begin() + 1);
    CHECK(!crypto::ec_decode_point(CurveId::P256, x_is_p).has_value());
    CHECK(!crypto::ec_decode_point(CurveId::P256, std::span<std::uint8_t const>(encoded).subspan(1)).has_value());
    CHECK(!crypto::ec_decode_point(CurveId::P256, std::vector<std::uint8_t> { 0x00 }).has_value());
    std::vector<std::uint8_t> compressed(encoded.begin(), encoded.begin() + 33);
    compressed[0] = 0x02 | (encoded.back() & 1);
    CHECK(!crypto::ec_decode_point(CurveId::P256, compressed).has_value());
    // SHA-512 under P-256 keeps only the digest's first 32 bytes: changing
    // any later byte leaves the signature good, changing byte 31 does not.
    BigInt const r512 = *BigInt::from_hex("8496A60B5E9B47C825488827E0495B0E3FA109EC4568FD3F8D1097678EB97F00");
    BigInt const s512 = *BigInt::from_hex("2362AB1ADBE2B8ADF9CB9EDAB740EA6049C028114F2460F96554F61FAE3302FE");
    auto long_digest = crypto::Sha512::hash(ascii("sample"));
    for (std::size_t i = 32; i < long_digest.size(); ++i)
        long_digest[i] ^= 0xA5;
    CHECK(crypto::ecdsa_verify(CurveId::P256, *key, long_digest, r512, s512));
    long_digest[31] ^= 1;
    CHECK(!crypto::ecdsa_verify(CurveId::P256, *key, long_digest, r512, s512));

    for (CurveId const id : { CurveId::P256, CurveId::P384 }) {
        crypto::Curve const& cv = crypto::curve(id);
        crypto::EcPoint const g { cv.gx, cv.gy };
        std::string const curve_name = id == CurveId::P256 ? "P-256" : "P-384";
        // A key and a signature made here, with a digest as long as the
        // order: it verifies, and so does the digest plus n, which is the
        // same e once reduced. e is kept below 2^128 so that e + n still
        // fits the digest's bytes.
        SplitMix64 rng { 0x5eed0001 };
        BigInt const d = random_scalar(rng, cv);
        BigInt const k = random_scalar(rng, cv);
        BigInt const e = random_scalar(rng, cv).shift_right(cv.n.bit_length() - 128);
        std::optional<crypto::EcPoint> const q = crypto::ec_test::multiply(id, Method::Reference, g, d);
        Signature const sig = sign(id, d, k, e);
        CHECK_EQ(curve_name + (crypto::ecdsa_verify(id, *q, *e.to_bytes(cv.field_bytes), sig.r, sig.s) ? " own signature verifies" : " own signature fails"),
            curve_name + " own signature verifies");
        std::optional<std::vector<std::uint8_t>> const e_plus_n = e.add(cv.n).to_bytes(cv.field_bytes);
        CHECK(e_plus_n.has_value() && crypto::ecdsa_verify(id, *q, *e_plus_n, sig.r, sig.s));
        CHECK(!crypto::ecdsa_verify(id, *q, *e.add(BigInt::from_u64(1)).to_bytes(cv.field_bytes), sig.r, sig.s));

        // x(R) ≥ n: R = (n + t, y) for the smallest t ≥ 1 that is on the
        // curve (p ≡ 3 mod 4, so a square root is a power), then a key Q
        // made so that u1·G + u2·Q = R for a chosen e and s. Then r = t.
        BigInt const exponent = cv.p.add(BigInt::from_u64(1)).shift_right(2);
        std::optional<crypto::EcPoint> big_r;
        for (std::uint64_t t = 1; t < 1000 && !big_r; ++t) {
            BigInt const x = cv.n.add(BigInt::from_u64(t));
            BigInt const rhs = x.mod_mul(x, cv.p).mod_mul(x, cv.p).mod_sub(BigInt::from_u64(3).mod_mul(x, cv.p), cv.p).mod_add(cv.b, cv.p);
            BigInt const y = rhs.mod_pow(exponent, cv.p);
            if (y.mod_mul(y, cv.p) == rhs)
                big_r = crypto::EcPoint { x, y };
        }
        CHECK(big_r.has_value());
        if (big_r) {
            BigInt const r_small = big_r->x.sub(cv.n);
            BigInt const s_chosen = BigInt::from_u64(0x1234567);
            BigInt const w = s_chosen.mod_inverse_prime(cv.n);
            BigInt const u1 = e.mod_mul(w, cv.n);
            BigInt const u2 = r_small.mod_mul(w, cv.n);
            std::optional<crypto::EcPoint> const u1g = crypto::ec_test::multiply(id, Method::Reference, g, u1);
            std::optional<crypto::EcPoint> const difference = crypto::ec_test::add(id, *big_r, negate(cv, *u1g), false);
            std::optional<crypto::EcPoint> const q_made = crypto::ec_test::multiply(id, Method::Reference, *difference, u2.mod_inverse_prime(cv.n));
            bool const valid = crypto::ecdsa_verify(id, *q_made, *e.to_bytes(cv.field_bytes), r_small, s_chosen);
            CHECK_EQ(curve_name + (valid ? " x = r + n verifies" : " x = r + n fails"), curve_name + " x = r + n verifies");
            CHECK(!crypto::ecdsa_verify(id, *q_made, *e.to_bytes(cv.field_bytes), r_small.add(BigInt::from_u64(1)), s_chosen));
        }

        // The key G with e = n − 1, r = s = 1: u1·G + u2·G = n·G, the point
        // at infinity, which must be a refusal.
        CHECK(!crypto::ecdsa_verify(id, g, *cv.n.sub(BigInt::from_u64(1)).to_bytes(cv.field_bytes), BigInt::from_u64(1), BigInt::from_u64(1)));
        // The key G with e = r and s = 2r/k: u1 = u2 = k/2, so the final sum
        // adds a point to itself and must double it.
        std::optional<crypto::EcPoint> const kg = crypto::ec_test::multiply(id, Method::Reference, g, k);
        BigInt const r_k = kg->x.mod(cv.n);
        BigInt const s_k = r_k.mod_add(r_k, cv.n).mod_mul(k.mod_inverse_prime(cv.n), cv.n);
        bool const doubled = crypto::ecdsa_verify(id, g, *r_k.to_bytes(cv.field_bytes), r_k, s_k);
        CHECK_EQ(curve_name + (doubled ? " equal halves verify" : " equal halves fail"), curve_name + " equal halves verify");
    }
}

// RFC 5903 §8.1 (P-256) and §8.2 (P-384): the initiator's and responder's
// private scalars, their public points, and the shared x, from both ends.
// node's crypto computes the same points and secret from the same scalars.
void test_ecdh_rfc5903()
{
    using crypto::CurveId;
    struct Vector {
        CurveId id;
        std::string_view i;
        std::string_view gi;
        std::string_view r;
        std::string_view gr;
        std::string_view shared;
    };
    static constexpr Vector vectors[] = {
        { CurveId::P256, "C88F01F510D9AC3F70A292DAA2316DE544E9AAB8AFE84049C62A9C57862D1433",
            "04dad0b65394221cf9b051e1feca5787d098dfe637fc90b9ef945d0c37725811805271a0461cdb8252d61f1c456fa3e59ab1f45b33accf5f58389e0577b8990bb3",
            "C6EF9C5D78AE012A011164ACB397CE2088685D8F06BF9BE0B283AB46476BEE53",
            "04d12dfb5289c8d4f81208b70270398c342296970a0bccb74c736fc7554494bf6356fbf3ca366cc23e8157854c13c58d6aac23f046ada30f8353e74f33039872ab",
            "d6840f6b42f6edafd13116e0e12565202fef8e9ece7dce03812464d04b9442de" },
        { CurveId::P384, "099F3C7034D4A2C699884D73A375A67F7624EF7C6B3C0F160647B67414DCE655E35B538041E649EE3FAEF896783AB194",
            "04667842d7d180ac2cde6f74f37551f55755c7645c20ef73e31634fe72b4c55ee6de3ac808acb4bdb4c88732aee95f41aa9482ed1fc0eeb9cafc4984625ccfc23f65032149e0e144ada024181535a0f38eeb9fcff3c2c947dae69b4c634573a81c",
            "41CB0779B4BDB85D47846725FBEC3C9430FAB46CC8DC5060855CC9BDA0AA2942E0308312916B8ED2960E4BD55A7448FC",
            "04e558dbef53eecde3d3fccfc1aea08a89a987475d12fd950d83cfa41732bc509d0d1ac43a0336def96fda41d0774a3571dcfbec7aacf3196472169e838430367f66eebe3c6e70c416dd5f0c68759dd1fff83fa40142209dff5eaad96db9e6386c",
            "11187331c279962d93d604243fd592cb9d0a926f422e47187521287e7156c5c4d603135569b9e9d09cf5d4a270f59746" },
    };
    for (Vector const& v : vectors) {
        std::string const name = v.id == CurveId::P256 ? "P-256" : "P-384";
        std::optional<crypto::EcPoint> const gi = crypto::ec_public_point(v.id, from_hex(v.i));
        std::optional<crypto::EcPoint> const gr = crypto::ec_public_point(v.id, from_hex(v.r));
        CHECK(gi.has_value() && gr.has_value());
        if (!gi || !gr)
            continue;
        CHECK_EQ(name + " gi " + hex(crypto::ec_encode_point(v.id, *gi)), name + " gi " + std::string(v.gi));
        CHECK_EQ(name + " gr " + hex(crypto::ec_encode_point(v.id, *gr)), name + " gr " + std::string(v.gr));
        std::optional<crypto::EcPoint> const peer_r = crypto::ec_decode_point(v.id, from_hex(v.gr));
        std::optional<crypto::EcPoint> const peer_i = crypto::ec_decode_point(v.id, from_hex(v.gi));
        CHECK(peer_r.has_value() && peer_i.has_value());
        if (!peer_r || !peer_i)
            continue;
        std::optional<std::vector<std::uint8_t>> const from_i = crypto::ecdh_shared_x(v.id, from_hex(v.i), *peer_r);
        std::optional<std::vector<std::uint8_t>> const from_r = crypto::ecdh_shared_x(v.id, from_hex(v.r), *peer_i);
        CHECK_EQ(name + " shared " + (from_i ? hex(*from_i) : "none"), name + " shared " + std::string(v.shared));
        CHECK_EQ(name + " shared " + (from_r ? hex(*from_r) : "none"), name + " shared " + std::string(v.shared));
        // A peer point moved up by p is the same point modulo p, refused.
        CHECK(!crypto::ecdh_shared_x(v.id, from_hex(v.i), { peer_r->x.add(crypto::curve(v.id).p), peer_r->y }).has_value());
    }
    // A scalar at or past n is reduced into [1, n − 1] before use: n + 1 is
    // 1, and all ones is 2^256 − 1 − n.
    for (CurveId const id : { CurveId::P256, CurveId::P384 }) {
        crypto::Curve const& c = crypto::curve(id);
        std::optional<crypto::EcPoint> const one = crypto::ec_public_point(id, *c.n.add(crypto::BigInt::from_u64(1)).to_bytes(c.field_bytes));
        CHECK(one.has_value() && one->x == c.gx && one->y == c.gy);
        std::vector<std::uint8_t> const ones(c.field_bytes, 0xFF);
        std::optional<crypto::EcPoint> const from_ones = crypto::ec_public_point(id, ones);
        CHECK(same_point(from_ones, crypto::ec_test::multiply(id, crypto::ec_test::Method::Reference, { c.gx, c.gy }, crypto::BigInt::from_bytes(ones)->mod(c.n))));
    }
}

// The fixed-width arithmetic against the textbook formulas over BigInt it
// replaced, bit for bit: 200 scalars per curve (the edges a window method
// can get wrong first, the rest from a fixed seed), each times one of 20
// points, through every multiplication path; and the generator's table
// against the same reference. A point that differs in one bit of x or y
// counts as a mismatch.
void test_ec_differential()
{
    using crypto::BigInt;
    using crypto::CurveId;
    using crypto::ec_test::Method;
    for (CurveId const id : { CurveId::P256, CurveId::P384 }) {
        crypto::Curve const& c = crypto::curve(id);
        crypto::EcPoint const g { c.gx, c.gy };
        std::string const name = id == CurveId::P256 ? "P-256" : "P-384";
        SplitMix64 rng { id == CurveId::P256 ? 0x0256u : 0x0384u };
        std::vector<crypto::EcPoint> points;
        for (int j = 0; j < 20; ++j)
            points.push_back(*crypto::ec_test::multiply(id, Method::Reference, g, random_scalar(rng, c)));
        std::vector<BigInt> scalars;
        for (std::uint64_t const small : { 1u, 2u, 3u, 15u, 16u, 17u, 255u, 256u, 0xFFFFu, 0x10000u })
            scalars.push_back(BigInt::from_u64(small));
        for (std::uint64_t const below : { 1u, 2u, 15u, 16u, 17u })
            scalars.push_back(c.n.sub(BigInt::from_u64(below)));
        scalars.push_back(BigInt::from_u64(~std::uint64_t(0)));
        scalars.push_back(c.n.shift_right(1));
        scalars.push_back(BigInt::from_u64(1).shift_left(c.n.bit_length() - 1));
        scalars.push_back(BigInt::from_u64(15).shift_left(c.n.bit_length() - 4).mod(c.n));
        for (std::uint8_t const pattern : { 0x0F, 0xF0, 0x11, 0xFF, 0x80 })
            scalars.push_back(BigInt::from_bytes(std::vector<std::uint8_t>(c.field_bytes, pattern))->mod(c.n));
        while (scalars.size() < 200)
            scalars.push_back(random_scalar(rng, c));
        int double_add = 0;
        int window = 0;
        int constant_time = 0;
        int base = 0;
        int base_constant_time = 0;
        for (std::size_t i = 0; i < scalars.size(); ++i) {
            crypto::EcPoint const& p = points[i % points.size()];
            std::optional<crypto::EcPoint> const expected = crypto::ec_test::multiply(id, Method::Reference, p, scalars[i]);
            double_add += !same_point(crypto::ec_test::multiply(id, Method::DoubleAndAdd, p, scalars[i]), expected);
            window += !same_point(crypto::ec_test::multiply(id, Method::Window, p, scalars[i]), expected);
            constant_time += !same_point(crypto::ec_test::multiply(id, Method::ConstantTime, p, scalars[i]), expected);
            if (i < 40) {
                std::optional<crypto::EcPoint> const expected_g = crypto::ec_test::multiply(id, Method::Reference, g, scalars[i]);
                base += !same_point(crypto::ec_test::multiply(id, Method::BaseTable, g, scalars[i]), expected_g);
                base_constant_time += !same_point(crypto::ec_test::multiply(id, Method::BaseTableConstantTime, g, scalars[i]), expected_g);
            }
        }
        CHECK_EQ(name + " double-and-add mismatches " + std::to_string(double_add), name + " double-and-add mismatches 0");
        CHECK_EQ(name + " window mismatches " + std::to_string(window), name + " window mismatches 0");
        CHECK_EQ(name + " constant-time mismatches " + std::to_string(constant_time), name + " constant-time mismatches 0");
        CHECK_EQ(name + " base table mismatches " + std::to_string(base), name + " base table mismatches 0");
        CHECK_EQ(name + " constant-time base table mismatches " + std::to_string(base_constant_time), name + " constant-time base table mismatches 0");
    }
}

// The fixed-width paths against each other: k·G by the windows and the
// tables equals k·G by plain double-and-add; a·G + b·G equals (a + b)·G
// through both additions; P + P is 2·P and P + (−P) the point at infinity
// through both, which the complete constant-time addition must get right
// by mask; (n − 1)·G is −G; and 0·P is the point at infinity everywhere.
void test_ec_consistency()
{
    using crypto::BigInt;
    using crypto::CurveId;
    using crypto::ec_test::Method;
    for (CurveId const id : { CurveId::P256, CurveId::P384 }) {
        crypto::Curve const& c = crypto::curve(id);
        crypto::EcPoint const g { c.gx, c.gy };
        std::string const name = id == CurveId::P256 ? "P-256" : "P-384";
        SplitMix64 rng { 0xC0DE0000u + static_cast<std::uint64_t>(id) };
        int window_mismatches = 0;
        int sum_mismatches = 0;
        for (int i = 0; i < 50; ++i) {
            BigInt const k = random_scalar(rng, c);
            std::optional<crypto::EcPoint> const plain = crypto::ec_test::multiply(id, Method::DoubleAndAdd, g, k);
            window_mismatches += !same_point(crypto::ec_test::multiply(id, Method::Window, g, k), plain);
            window_mismatches += !same_point(crypto::ec_test::multiply(id, Method::ConstantTime, g, k), plain);
            window_mismatches += !same_point(crypto::ec_test::multiply(id, Method::BaseTable, g, k), plain);
            window_mismatches += !same_point(crypto::ec_test::multiply(id, Method::BaseTableConstantTime, g, k), plain);
            BigInt const a = random_scalar(rng, c);
            BigInt const b = random_scalar(rng, c);
            std::optional<crypto::EcPoint> const ag = crypto::ec_test::multiply(id, Method::BaseTable, g, a);
            std::optional<crypto::EcPoint> const bg = crypto::ec_test::multiply(id, Method::BaseTable, g, b);
            std::optional<crypto::EcPoint> const sum = crypto::ec_test::multiply(id, Method::BaseTable, g, a.mod_add(b, c.n));
            sum_mismatches += !same_point(crypto::ec_test::add(id, *ag, *bg, false), sum);
            sum_mismatches += !same_point(crypto::ec_test::add(id, *ag, *bg, true), sum);
        }
        CHECK_EQ(name + " k·G mismatches " + std::to_string(window_mismatches), name + " k·G mismatches 0");
        CHECK_EQ(name + " a·G + b·G mismatches " + std::to_string(sum_mismatches), name + " a·G + b·G mismatches 0");
        BigInt const k = random_scalar(rng, c);
        std::optional<crypto::EcPoint> const p = crypto::ec_test::multiply(id, Method::Window, g, k);
        std::optional<crypto::EcPoint> const twice = crypto::ec_test::multiply(id, Method::Window, *p, BigInt::from_u64(2));
        CHECK(same_point(crypto::ec_test::add(id, *p, *p, false), twice));
        CHECK(same_point(crypto::ec_test::add(id, *p, *p, true), twice));
        CHECK(!crypto::ec_test::add(id, *p, negate(c, *p), false).has_value());
        CHECK(!crypto::ec_test::add(id, *p, negate(c, *p), true).has_value());
        std::optional<crypto::EcPoint> const minus_g = crypto::ec_test::multiply(id, Method::BaseTableConstantTime, g, c.n.sub(BigInt::from_u64(1)));
        CHECK(same_point(minus_g, negate(c, g)));
        for (Method const m : { Method::Reference, Method::DoubleAndAdd, Method::Window, Method::ConstantTime, Method::BaseTable, Method::BaseTableConstantTime })
            CHECK(!crypto::ec_test::multiply(id, m, g, BigInt()).has_value());
    }
}


// AES-128 (FIPS 197) and AES-128-GCM (SP 800-38D), the record protection of
// TLS_AES_128_GCM_SHA256: the block cipher's known answers first, then the
// mode's own test cases — an empty message, whole blocks, a partial final
// block with additional data — and the shape the TLS record layer uses.
void test_aes_gcm()
{
    auto encrypt_block = [](std::string_view key_hex, std::string_view plaintext_hex) {
        std::vector<std::uint8_t> const key_bytes = from_hex(key_hex);
        std::vector<std::uint8_t> const plaintext_bytes = from_hex(plaintext_hex);
        crypto::AesKey key {};
        crypto::AesBlock block {};
        std::copy(key_bytes.begin(), key_bytes.end(), key.begin());
        std::copy(plaintext_bytes.begin(), plaintext_bytes.end(), block.begin());
        crypto::Aes128 const aes(key);
        return hex(aes.encrypt(block));
    };
    // FIPS 197 §C.1, then the four AES-128 blocks of SP 800-38A §F.1.1.
    CHECK_EQ(encrypt_block("000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff"),
        "69c4e0d86a7b0430d8cdb78070b4c55a");
    CHECK_EQ(encrypt_block("2b7e151628aed2a6abf7158809cf4f3c", "6bc1bee22e409f96e93d7e117393172a"),
        "3ad77bb40d7a3660a89ecaf32466ef97");
    CHECK_EQ(encrypt_block("2b7e151628aed2a6abf7158809cf4f3c", "ae2d8a571e03ac9c9eb76fac45af8e51"),
        "f5d3d58503b9699de785895a96fdbaaf");
    CHECK_EQ(encrypt_block("2b7e151628aed2a6abf7158809cf4f3c", "30c81c46a35ce411e5fbc1191a0a52ef"),
        "43b1cd7f598ece23881b00e3ed030688");
    CHECK_EQ(encrypt_block("2b7e151628aed2a6abf7158809cf4f3c", "f69f2445df4f9b17ad2b417be66c3710"),
        "7b0c785e27e8ad3f8223207104725dd4");

    struct GcmCase {
        char const* key;
        char const* nonce;
        char const* aad;
        char const* plaintext;
        char const* ciphertext;
        char const* tag;
    };
    GcmCase const cases[] = {
        { "00000000000000000000000000000000", "000000000000000000000000", "", "", "",
            "58e2fccefa7e3061367f1d57a4e7455a" },
        { "00000000000000000000000000000000", "000000000000000000000000", "",
            "00000000000000000000000000000000", "0388dace60b6a392f328c2b971b2fe78",
            "ab6e47d42cec13bdf53a67b21257bddf" },
        { "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888", "",
            "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
            "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
            "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
            "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985",
            "4d5c2af327cd64a62cf35abd2ba6fab4" },
        { "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
            "feedfacedeadbeeffeedfacedeadbeefabaddad2",
            "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
            "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
            "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
            "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091",
            "5bc94fbc3221a5db94fae95ae7121a47" },
        // A record the TLS layer's shape: the 5-byte header as additional
        // data over a short body.
        { "000102030405060708090a0b0c0d0e0f", "0c0b0a090807060504030201", "1703030021",
            "48656c6c6f2c20544c5321", "2e5e020b1e1cd60768432b",
            "50584895ee2bc99255a11b9957c2c076" },
    };
    for (GcmCase const& test_case : cases) {
        std::vector<std::uint8_t> const key_bytes = from_hex(test_case.key);
        std::vector<std::uint8_t> const nonce_bytes = from_hex(test_case.nonce);
        crypto::AesKey key {};
        crypto::GcmNonce nonce {};
        std::copy(key_bytes.begin(), key_bytes.end(), key.begin());
        std::copy(nonce_bytes.begin(), nonce_bytes.end(), nonce.begin());
        std::vector<std::uint8_t> const aad = from_hex(test_case.aad);
        std::vector<std::uint8_t> const plaintext = from_hex(test_case.plaintext);
        std::vector<std::uint8_t> ciphertext(plaintext.size(), 0);
        crypto::GcmTag const tag = crypto::aes128_gcm_seal(key, nonce, aad, plaintext, ciphertext);
        CHECK_EQ(hex(ciphertext), std::string(test_case.ciphertext));
        CHECK_EQ(hex(tag), std::string(test_case.tag));

        std::vector<std::uint8_t> opened(ciphertext.size(), 0xaa);
        CHECK(crypto::aes128_gcm_open(key, nonce, aad, ciphertext, tag, opened));
        CHECK_EQ(hex(opened), std::string(test_case.plaintext));

        // A flipped bit anywhere the tag covers is refused, and a refused
        // record leaves the caller's buffer exactly as it was.
        std::vector<std::uint8_t> const untouched(ciphertext.size(), 0xaa);
        std::vector<std::uint8_t> scratch = untouched;
        crypto::GcmTag wrong_tag = tag;
        wrong_tag[0] = static_cast<std::uint8_t>(wrong_tag[0] ^ 1);
        CHECK(!crypto::aes128_gcm_open(key, nonce, aad, ciphertext, wrong_tag, scratch));
        crypto::AesKey wrong_key = key;
        wrong_key[0] = static_cast<std::uint8_t>(wrong_key[0] ^ 1);
        CHECK(!crypto::aes128_gcm_open(wrong_key, nonce, aad, ciphertext, tag, scratch));
        crypto::GcmNonce wrong_nonce = nonce;
        wrong_nonce[11] = static_cast<std::uint8_t>(wrong_nonce[11] ^ 1);
        CHECK(!crypto::aes128_gcm_open(key, wrong_nonce, aad, ciphertext, tag, scratch));
        if (!aad.empty()) {
            std::vector<std::uint8_t> wrong_aad = aad;
            wrong_aad[0] = static_cast<std::uint8_t>(wrong_aad[0] ^ 1);
            CHECK(!crypto::aes128_gcm_open(key, nonce, wrong_aad, ciphertext, tag, scratch));
        }
        if (!ciphertext.empty()) {
            std::vector<std::uint8_t> wrong_ciphertext = ciphertext;
            wrong_ciphertext[0] = static_cast<std::uint8_t>(wrong_ciphertext[0] ^ 1);
            CHECK(!crypto::aes128_gcm_open(key, nonce, aad, wrong_ciphertext, tag, scratch));
        }
        CHECK_EQ(hex(scratch), hex(untouched));
    }

    // Every length a final block can have, sealed and opened in one buffer:
    // the record layer hands the same memory to both directions.
    crypto::AesKey key {};
    crypto::GcmNonce nonce {};
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<std::uint8_t>(0x40 + i);
    for (std::size_t i = 0; i < nonce.size(); ++i)
        nonce[i] = static_cast<std::uint8_t>(0x90 + i);
    std::vector<std::uint8_t> const aad = from_hex("1703030013");
    for (std::size_t length = 0; length <= 33; ++length) {
        std::vector<std::uint8_t> original(length, 0);
        for (std::size_t i = 0; i < length; ++i)
            original[i] = static_cast<std::uint8_t>(7 * length + i);
        std::vector<std::uint8_t> buffer = original;
        crypto::GcmTag const tag = crypto::aes128_gcm_seal(key, nonce, aad, buffer, buffer);
        CHECK(length == 0 || hex(buffer) != hex(original)); // it really did encrypt
        CHECK(crypto::aes128_gcm_open(key, nonce, aad, buffer, tag, buffer));
        CHECK_EQ(hex(buffer), hex(original));
    }
}

int main()
{
    test_sha2();
    test_hmac();
    test_hkdf();
    test_chacha20_poly1305();
    test_aes_gcm();
    test_x25519();
    test_bigint();
    test_rsa();
    test_ecdsa();
    test_ecdh();
    test_ecdsa_rfc6979();
    test_ecdsa_edges();
    test_ecdh_rfc5903();
    test_ec_differential();
    test_ec_consistency();
    return sashfold::test::report("crypto");
}
