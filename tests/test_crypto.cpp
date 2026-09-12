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
    return sashfold::test::report("crypto");
}
