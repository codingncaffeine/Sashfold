// CBOR (RFC 8949) in the CTAP2 canonical form and in general, and COSE keys
// (RFC 9052, RFC 9053): the RFCs' own examples, the canonical rules of
// CTAP 2.1 §8 one by one, and the SubjectPublicKeyInfo getPublicKey()
// returns.
#include "AuthTestSupport.h"
#include "Test.h"

#include "auth/Cbor.h"
#include "auth/Cose.h"
#include "auth/DerWriter.h"
#include "crypto/Ec.h"
#include "net/tls/X509.h"

#include <cmath>
#include <string>

using namespace sashfold;
using namespace sashfold::auth;
using sashfold::test::from_hex;
using sashfold::test::to_hex;

namespace {

struct Example {
    char const* hex;
    bool strict; // admitted by the CTAP2 canonical profile
    bool reencodes; // general decoding then encoding gives the same bytes back
};

// RFC 8949 Appendix A, every row in table order. The profile refuses tags,
// indefinite lengths and the simple values other than false, true and null;
// floats are admitted with their width (CTAP 2.1 §8). The bignums
// (tags 2 and 3) are tags.
Example const appendix_a[] = {
    { "00", true, true },
    { "01", true, true },
    { "0a", true, true },
    { "17", true, true },
    { "1818", true, true },
    { "1819", true, true },
    { "1864", true, true },
    { "1903e8", true, true },
    { "1a000f4240", true, true },
    { "1b000000e8d4a51000", true, true },
    { "1bffffffffffffffff", true, true },
    { "c249010000000000000000", false, true },
    { "3bffffffffffffffff", true, true },
    { "c349010000000000000000", false, true },
    { "20", true, true },
    { "29", true, true },
    { "3863", true, true },
    { "3903e7", true, true },
    { "f90000", true, true },
    { "f98000", true, true },
    { "f93c00", true, true },
    { "fb3ff199999999999a", true, true },
    { "f93e00", true, true },
    { "f97bff", true, true },
    { "fa47c35000", true, true },
    { "fa7f7fffff", true, true },
    { "fb7e37e43c8800759c", true, true },
    { "f90001", true, true },
    { "f90400", true, true },
    { "f9c400", true, true },
    { "fbc010666666666666", true, true },
    { "f97c00", true, true },
    { "f97e00", true, true },
    { "f9fc00", true, true },
    { "fa7f800000", true, true },
    { "fa7fc00000", true, true },
    { "faff800000", true, true },
    { "fb7ff0000000000000", true, true },
    { "fb7ff8000000000000", true, true },
    { "fbfff0000000000000", true, true },
    { "f4", true, true },
    { "f5", true, true },
    { "f6", true, true },
    { "f7", false, true },
    { "f0", false, true },
    { "f8ff", false, true },
    { "c074323031332d30332d32315432303a30343a30305a", false, true },
    { "c11a514b67b0", false, true },
    { "c1fb41d452d9ec200000", false, true },
    { "d74401020304", false, true },
    { "d818456449455446", false, true },
    { "d82076687474703a2f2f7777772e6578616d706c652e636f6d", false, true },
    { "40", true, true },
    { "4401020304", true, true },
    { "60", true, true },
    { "6161", true, true },
    { "6449455446", true, true },
    { "62225c", true, true },
    { "62c3bc", true, true },
    { "63e6b0b4", true, true },
    { "64f0908591", true, true },
    { "80", true, true },
    { "83010203", true, true },
    { "8301820203820405", true, true },
    { "98190102030405060708090a0b0c0d0e0f101112131415161718181819", true, true },
    { "a0", true, true },
    { "a201020304", true, true },
    { "a26161016162820203", true, true },
    { "826161a161626163", true, true },
    { "a56161614161626142616361436164614461656145", true, true },
    { "5f42010243030405ff", false, false },
    { "7f657374726561646d696e67ff", false, false },
    { "9fff", false, false },
    { "9f018202039f0405ffff", false, false },
    { "9f01820203820405ff", false, false },
    { "83018202039f0405ff", false, false },
    { "83019f0203ff820405", false, false },
    { "9f0102030405060708090a0b0c0d0e0f101112131415161718181819ff", false, false },
    { "bf61610161629f0203ffff", false, false },
    { "826161bf61626163ff", false, false },
    { "bf6346756ef563416d7421ff", false, false },
};

void appendix_a_examples()
{
    for (Example const& example : appendix_a) {
        auto const bytes = from_hex(example.hex);
        auto const strict = cbor::decode(bytes, cbor::Mode::Ctap2);
        if (!CHECK_EQ(strict.has_value(), example.strict))
            std::cerr << "  example " << example.hex << "\n";
        if (strict)
            CHECK_EQ(to_hex(cbor::encode(*strict)), std::string(example.hex));
        auto const general = cbor::decode(bytes, cbor::Mode::General);
        if (!CHECK(general.has_value()))
            std::cerr << "  example " << example.hex << "\n";
        if (general && example.reencodes)
            CHECK_EQ(to_hex(cbor::encode(*general)), std::string(example.hex));
    }

    // The values the examples stand for.
    CHECK_EQ(cbor::decode(from_hex("3903e7"))->as_int64().value_or(0), -1000);
    CHECK_EQ(cbor::decode(from_hex("1bffffffffffffffff"))->raw_number(), 18446744073709551615ull);
    CHECK(!cbor::decode(from_hex("1bffffffffffffffff"))->as_int64()); // beyond int64
    auto const negative = cbor::decode(from_hex("3bffffffffffffffff"));
    CHECK(negative->type() == cbor::Type::Negative && negative->raw_number() == 18446744073709551615ull);
    CHECK_EQ(cbor::decode(from_hex("f97bff"))->as_double(), 65504.0);
    CHECK_EQ(cbor::decode(from_hex("f90001"))->as_double(), 5.960464477539063e-8);
    CHECK_EQ(cbor::decode(from_hex("fa47c35000"))->as_double(), 100000.0);
    CHECK_EQ(cbor::decode(from_hex("f9c400"))->as_double(), -4.0);
    CHECK(std::isnan(cbor::decode(from_hex("f97e00"))->as_double()));
    CHECK(std::signbit(cbor::decode(from_hex("f98000"))->as_double()));
    CHECK_EQ(std::string(cbor::decode(from_hex("63e6b0b4"))->as_text()), std::string("\xe6\xb0\xb4"));
    auto const streamed = cbor::decode(from_hex("7f657374726561646d696e67ff"), cbor::Mode::General);
    CHECK_EQ(std::string(streamed->as_text()), std::string("streaming"));
    auto const fun = cbor::decode(from_hex("bf6346756ef563416d7421ff"), cbor::Mode::General);
    CHECK(fun->find("Fun") && fun->find("Fun")->as_bool() == true);
    CHECK_EQ(fun->find("Amt")->as_int64().value_or(0), -2);
    // An indefinite item re-encodes definite: the canonical form of it.
    CHECK_EQ(to_hex(cbor::encode(*cbor::decode(from_hex("9f018202039f0405ffff"), cbor::Mode::General))), std::string("8301820203820405"));
    auto const tag = cbor::decode(from_hex("c11a514b67b0"), cbor::Mode::General);
    CHECK(tag->type() == cbor::Type::Tag && tag->raw_number() == 1 && tag->tagged().raw_number() == 1363896240);
}

void canonical_rules()
{
    // Integers and lengths in their shortest form only (CTAP 2.1 §8).
    for (char const* hex : { "1800", "1817", "190017", "1900ff", "1a0000ffff", "1b00000000ffffffff", "3817", "5800", "580100", "7800",
             "9800", "b800", "f90000ff" }) {
        auto const bytes = from_hex(hex);
        if (!CHECK(!cbor::decode(bytes, cbor::Mode::Ctap2)))
            std::cerr << "  accepted " << hex << "\n";
    }
    // The same in general decoding.
    CHECK(cbor::decode(from_hex("1817"), cbor::Mode::General).has_value());
    // Map keys: major type, then length, then bytes.
    CHECK(cbor::decode(from_hex("a2010203 04")).has_value());
    CHECK(!cbor::decode(from_hex("a2030401 02")).has_value()); // 3 before 1
    CHECK(!cbor::decode(from_hex("a2200101 02")).has_value()); // -1 before 1: major type 1 after 0
    CHECK(cbor::decode(from_hex("a2010220 03")).has_value()); // 1 then -1
    CHECK(cbor::decode(from_hex("a2181801 2002")).has_value()); // 24 (two bytes) before -1: major type first
    CHECK(!cbor::decode(from_hex("a2616201 61610 2")).has_value()); // "b" before "a"
    CHECK(cbor::decode(from_hex("a2616201 62616102")).has_value()); // "b" before "aa": shorter first
    CHECK(!cbor::decode(from_hex("a2626161 01616202")).has_value()); // "aa" before "b"
    CHECK(cbor::decode(from_hex("a2626161 01616202"), cbor::Mode::General).has_value());
    // Duplicate keys, in both modes.
    CHECK(!cbor::decode(from_hex("a2010201 03")).has_value());
    CHECK(!cbor::decode(from_hex("a2010201 03"), cbor::Mode::General).has_value());
    CHECK(!cbor::decode(from_hex("a2616101 616102"), cbor::Mode::General).has_value());
    // Trailing bytes, truncation.
    CHECK(!cbor::decode(from_hex("0100")).has_value());
    CHECK(!cbor::decode(from_hex("19 01")).has_value());
    CHECK(!cbor::decode(from_hex("43 0102")).has_value());
    CHECK(!cbor::decode(from_hex("82 01")).has_value());
    CHECK(!cbor::decode(from_hex("")).has_value());
    auto const prefix = cbor::decode_prefix(from_hex("a10102 a0"));
    CHECK(prefix && prefix->second == 3);
    // Nesting: four levels pass, five do not.
    CHECK(cbor::decode(from_hex("81818181 01")).has_value());
    CHECK(!cbor::decode(from_hex("8181818181 01")).has_value());
    CHECK(cbor::decode(from_hex("a101 a101 a101 a10101")).has_value());
    CHECK(!cbor::decode(from_hex("a101 a101 a101 a101 a10101")).has_value());
    CHECK(cbor::decode(from_hex("8181818181 01"), cbor::Mode::General).has_value());
    std::vector<std::uint8_t> deep(10000, 0x81);
    deep.push_back(0x01);
    CHECK(!cbor::decode(deep, cbor::Mode::General).has_value()); // bounded, not a stack overflow
    // Invalid UTF-8, surrogates, overlong forms.
    CHECK(!cbor::decode(from_hex("62c328")).has_value());
    CHECK(!cbor::decode(from_hex("63eda080")).has_value());
    CHECK(!cbor::decode(from_hex("62c0af"), cbor::Mode::General).has_value());
    // A text chunk of an indefinite string must be text, and whole.
    CHECK(!cbor::decode(from_hex("7f4161ff"), cbor::Mode::General).has_value());
    CHECK(!cbor::decode(from_hex("7f61c361bcff"), cbor::Mode::General).has_value());
    // Reserved additional information, a lone break, a two-byte simple
    // value below 32.
    CHECK(!cbor::decode(from_hex("1c"), cbor::Mode::General).has_value());
    CHECK(!cbor::decode(from_hex("ff"), cbor::Mode::General).has_value());
    CHECK(!cbor::decode(from_hex("f814"), cbor::Mode::General).has_value());
    // A count larger than the bytes left is refused before anything is
    // reserved for it.
    CHECK(!cbor::decode(from_hex("9b7fffffffffffffff 00")).has_value());
    CHECK(!cbor::decode(from_hex("bb7fffffffffffffff 00"), cbor::Mode::General).has_value());
    CHECK(!cbor::decode(from_hex("5b7fffffffffffffff 00")).has_value());

    // The encoder sorts whatever order the map was built in.
    cbor::Value map = cbor::Value::map();
    map.set("bb", cbor::Value::integer(1));
    map.set(-3, cbor::Value::integer(2));
    map.set("a", cbor::Value::integer(3));
    map.set(24, cbor::Value::integer(4));
    map.set(1, cbor::Value::integer(5));
    map.set(-1, cbor::Value::integer(6));
    CHECK_EQ(to_hex(cbor::encode(map)), std::string("a6" "0105" "181804" "2006" "2202" "616103" "62626201"));
    CHECK(cbor::decode(cbor::encode(map)).has_value());
    // Integers at each boundary of their forms, round trip.
    for (std::int64_t const v : std::initializer_list<std::int64_t> { 0, 23, 24, 255, 256, 65535, 65536, 4294967295, 4294967296, -1, -24, -25,
             -256, -257, -65536, -65537, -4294967296, -4294967297, INT64_MIN, INT64_MAX }) {
        auto const decoded = cbor::decode(cbor::encode(cbor::Value::integer(v)));
        CHECK(decoded && decoded->as_int64() == v);
    }
    CHECK_EQ(to_hex(cbor::encode(cbor::Value::integer(-257))), std::string("390100"));
    CHECK_EQ(to_hex(cbor::encode(cbor::Value::floating(1.5, 2))), std::string("f93e00"));
    CHECK_EQ(to_hex(cbor::encode(cbor::Value::floating(100000.0, 4))), std::string("fa47c35000"));
}

void cose_keys_of_rfc9052()
{
    // RFC 9052 Appendix C.7.2: the example private keys, with their public
    // points. The point each d gives must be the one the key carries, read
    // through the COSE map.
    struct Key {
        std::int64_t crv;
        char const* x;
        char const* y;
        char const* d;
    };
    Key const keys[] = {
        { 1, "65eda5a12577c2bae829437fe338701a10aaa375e1bb5b5de108de439c08551d",
            "1e52ed75701163f7f9e40ddf9f341b3dc9ba860af7e0ca7ca7e9eecd0084d19c",
            "aff907c99f9ad3aae6c4cdf21122bce2bd68b5283e6907154ad911840fa208cf" },
        { 1, "bac5b11cad8f99f9c72b05cf4b9e26d244dc189f745228255a219a86d6a09eff",
            "20138bf82dc1b6d562be0fa54ab7804a3a64b6d72ccfed6b6fb6ed28bbfc117e",
            "57c92077664146e876760c9520d054aa93c3afb04e306705db6090308507b4d3" },
        { 3,
            "0072992cb3ac08ecf3e5c63dedec0d51a8c1f79ef2f82f94f3c737bf5de7986671eac625fe8257bbd0394644caaa3aaf8f27a4585fbbcad0f2457620085e5c8f42ad",
            "01dca6947bce88bc5790485ac97427342bc35f887d86d65a089377e247e60baa55e4e8501e2ada5724ac51d6909008033ebc10ac999b9d7f5cc2519f3fe1ea1d9475",
            "00085138ddabf5ca975f5860f91a08e91d6d5f9a76ad4018766a476680b55cd339e8ab6c72b5facdb2a2a50ac25bd086647dd3e2e6e99e84ca2c3609fdf177feb26d" },
        { 1, "98f50a4ff6c05861c8860d13a638ea56c3f5ad7590bbfbf054e1c7b4d91d6280",
            "f01400b089867804b8e9fc96c3932161f1934f4223069170d924b7e03bf822bb",
            "02d1f7e6f26c43d4868d87ceb2353161740aacf1f7163647984b522a848df1c3" },
    };
    for (Key const& k : keys) {
        // The map as C.7.1 writes it: -1, -2, -3, 1, 2 — not the canonical
        // order, so it is read in general mode, as WebAuthn reads keys.
        std::vector<std::uint8_t> written;
        written.push_back(0xa5);
        cbor::encode_into(cbor::Value::integer(-1), written);
        cbor::encode_into(cbor::Value::integer(k.crv), written);
        cbor::encode_into(cbor::Value::integer(-2), written);
        cbor::encode_into(cbor::Value::bytes(from_hex(k.x)), written);
        cbor::encode_into(cbor::Value::integer(-3), written);
        cbor::encode_into(cbor::Value::bytes(from_hex(k.y)), written);
        cbor::encode_into(cbor::Value::integer(1), written);
        cbor::encode_into(cbor::Value::integer(2), written);
        cbor::encode_into(cbor::Value::integer(2), written);
        cbor::encode_into(cbor::Value::bytes(from_hex("3131")), written);
        CHECK(!cbor::decode(written, cbor::Mode::Ctap2));
        auto const map = cbor::decode(written, cbor::Mode::General);
        CHECK(map.has_value());
        if (!map)
            continue;
        auto const key = cose_key_from_cbor(*map);
        CHECK(key.has_value());
        if (!key)
            continue;
        auto const point = cose_key_ec_point(*key);
        CHECK(point.has_value());
        if (!point)
            continue;
        auto const d = from_hex(k.d);
        auto const from_d = crypto::ec_public_point(point->first, d);
        CHECK(from_d && from_d->x == point->second.x && from_d->y == point->second.y);
        // Written back canonically, read strictly, the same key.
        auto const canonical = cbor::encode(cose_key_to_cbor(*key));
        auto const again = cbor::decode(canonical);
        CHECK(again && cose_key_from_cbor(*again) == key);
        // The SPKI reads back through the certificate parser to the same point.
        auto const spki = cose_key_to_spki(*key);
        CHECK(spki.has_value());
        if (spki && k.crv == 3) {
            // The certificate parser has no P-521; RFC 5480's layout, by hand:
            // SEQUENCE { SEQUENCE { id-ecPublicKey, secp521r1 }, BIT STRING 04 x y }.
            CHECK_EQ(to_hex(*spki), std::string("30819b301006072a8648ce3d020106052b810400230381860004") + k.x + k.y);
        } else if (spki) {
            auto const parsed = tls::parse_public_key(*spki);
            CHECK(parsed && parsed->kind == tls::PublicKey::Kind::Ec && parsed->ec.x == point->second.x && parsed->ec.y == point->second.y);
        }
    }
    // A point off its curve is refused when it is used.
    CoseKey bad;
    bad.kty = cose::kty_ec2;
    bad.crv = cose::crv_p256;
    bad.x = from_hex(keys[0].x);
    bad.y = from_hex(keys[0].x);
    CHECK(!cose_key_ec_point(bad));
    CHECK(!cose_key_to_spki(bad));
    // Missing members, wrong sizes, unknown kinds.
    CHECK(!cose_key_from_cbor(*cbor::decode(from_hex("a10102"))));
    CHECK(!cose_key_from_cbor(*cbor::decode(from_hex("a201022001"))));
    CHECK(!cose_key_from_cbor(*cbor::decode(from_hex("a3010220012141aa"))));
    CHECK(!cose_key_from_cbor(*cbor::decode(from_hex("a10109"))));
}

void spki_forms()
{
    // RFC 8410 §10.1's example Ed25519 public key and its SPKI.
    auto const ed = from_hex("19bf44096984cdfe8541bac167dc3b96c85086aa30b6b6cb0c5c38ad703166e1");
    CoseKey const ed_key = cose_key_from_ed25519(std::span<std::uint8_t const, 32>(ed.data(), 32));
    auto const ed_spki = cose_key_to_spki(ed_key);
    CHECK(ed_spki.has_value());
    if (ed_spki)
        CHECK_EQ(to_hex(*ed_spki), std::string("302a300506032b657003210019bf44096984cdfe8541bac167dc3b96c85086aa30b6b6cb0c5c38ad703166e1"));
    auto const ed_cbor = cbor::encode(cose_key_to_cbor(ed_key));
    // {1: 1, 3: -8, -1: 6, -2: x}
    CHECK_EQ(to_hex(ed_cbor), std::string("a4010103272006215820") + to_hex(ed));

    // RSA: the SPKI reads back through the certificate parser.
    crypto::RsaPublicKey rsa;
    rsa.n = *crypto::BigInt::from_hex(
        "c4e0b3e2a0e4d0b1a4c99bdb73f8fd3e9d5a2c3d1f1a6c4c0a5ab6f2c7b9e7a1d3f5c7e9b1a3c5e7f9b2d4f6a8c0e2f4a6b8c0d2e4f6a8b0c2d4e6f8a0b2c4d6"
        "e8f0a2b4c6d8e0f2a4b6c8d0e2f4a6b8c0d2e4f6a8b0c2d4e6f8a0b2c4d6e8f0a2b4c6d8e0f2a4b6c8d0e2f4a6b8c0d2e4f6a8b0c2d4e6f8a0b2c4d6e8f0a2b5");
    rsa.e = crypto::BigInt::from_u64(65537);
    CoseKey const rsa_key = cose_key_from_rsa(rsa);
    auto const rsa_spki = cose_key_to_spki(rsa_key);
    CHECK(rsa_spki.has_value());
    if (rsa_spki) {
        auto const parsed = tls::parse_public_key(*rsa_spki);
        CHECK(parsed && parsed->kind == tls::PublicKey::Kind::Rsa && parsed->rsa.n == rsa.n && parsed->rsa.e == rsa.e);
    }
    // An even modulus is not an RSA key.
    CoseKey even = rsa_key;
    even.n.back() &= 0xfe;
    CHECK(!cose_key_to_spki(even));

    // DER signatures: r and s round trip, and a high bit gets its zero.
    auto const r = *crypto::BigInt::from_hex("80");
    auto const s = *crypto::BigInt::from_hex("7f");
    auto const sig = der::ecdsa_signature(r, s);
    CHECK_EQ(to_hex(sig), std::string("3007020200800201" "7f"));
    auto const back = der::parse_ecdsa_signature(sig);
    CHECK(back && back->first == r && back->second == s);
    CHECK(!der::parse_ecdsa_signature(from_hex("3007020200800201")));
    CHECK(!der::parse_ecdsa_signature(from_hex("3007020200800201" "7f00")));
    CHECK_EQ(to_hex(der::oid("1.2.840.10045.2.1")), std::string("06072a8648ce3d0201"));
}

}

int main()
{
    appendix_a_examples();
    canonical_rules();
    cose_keys_of_rfc9052();
    spki_forms();
    return sashfold::test::report("test_auth_cbor");
}
