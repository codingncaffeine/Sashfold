// The Web Authentication Level 3 test vectors (§16): complete registration
// and authentication ceremonies whose attestation objects, authenticator
// data and signatures must read, re-encode and verify; and the prf /
// hmac-secret mappings of §16.17.1, which fix the PIN/UV auth protocols'
// key agreement, KDFs and encryption to the byte.
#include "AuthTestSupport.h"
#include "Test.h"
#include "WebAuthnVectors.h"

#include "auth/Attestation.h"
#include "auth/Cbor.h"
#include "auth/Cose.h"
#include "auth/PinProtocol.h"
#include "crypto/Ed25519.h"
#include "crypto/Hmac.h"
#include "crypto/Sha2.h"
#include "net/tls/X509.h"
#include "platform/Random.h"

#include <string>

using namespace sashfold;
using namespace sashfold::auth;
using sashfold::test::from_hex;
using sashfold::test::to_hex;

namespace {

std::vector<std::uint8_t> sha256_of(std::span<std::uint8_t const> data)
{
    auto const d = crypto::Sha256::hash(data);
    return std::vector<std::uint8_t>(d.begin(), d.end());
}

void ceremonies()
{
    int verified_statements = 0;
    int verified_assertions = 0;
    for (auto const& v : webauthn_vectors) {
        std::string const section = v.section;
        auto const attestation_bytes = from_hex(v.reg_attestationObject);
        auto const object = parse_attestation_object(attestation_bytes);
        if (!CHECK(object.has_value())) {
            std::cerr << "  section " << section << "\n";
            continue;
        }
        // The encoder writes the same bytes back: these objects are in the
        // canonical form.
        auto const reencoded = encode_attestation_object(object->fmt, object->att_stmt, object->auth_data);
        if (!CHECK(reencoded == attestation_bytes))
            std::cerr << "  section " << section << " re-encoded differently\n";
        CHECK(cbor::decode(attestation_bytes, cbor::Mode::Ctap2).has_value());
        auto const& attested = object->parsed.attested;
        if (!CHECK(attested.has_value()))
            continue;
        CHECK_EQ(to_hex(attested->credential_id), std::string(v.reg_credential_id));
        CHECK_EQ(to_hex(attested->aaguid), std::string(v.reg_aaguid));
        CHECK(object->parsed.user_present());

        // The credential's public key is the one its private key gives.
        auto const& key = attested->public_key;
        if (*v.reg_credential_private_key != '\0') {
            auto scalar = from_hex(v.reg_credential_private_key);
            auto const point = cose_key_ec_point(key);
            CHECK(point.has_value());
            if (point) {
                // §16.9 derives 65 bytes for a P-521 scalar of 66.
                std::size_t const size = crypto::curve(point->first).field_bytes;
                if (scalar.size() < size)
                    scalar.insert(scalar.begin(), size - scalar.size(), 0);
                auto const derived = crypto::ec_public_point(point->first, scalar);
                CHECK(derived && derived->x == point->second.x && derived->y == point->second.y);
            }
        } else if (*v.reg_private_key_p != '\0') {
            auto const p = crypto::BigInt::from_bytes(from_hex(v.reg_private_key_p));
            auto const q = crypto::BigInt::from_bytes(from_hex(v.reg_private_key_q));
            auto const rsa = cose_key_rsa(key);
            CHECK(rsa && p && q && rsa->n == p->mul(*q));
            CHECK(key.alg == cose::alg_rs256);
        } else if (*v.reg_private_key != '\0' && key.kty == cose::kty_okp) {
            crypto::Ed25519Seed seed {};
            auto const bytes = from_hex(v.reg_private_key);
            std::copy(bytes.begin(), bytes.end(), seed.begin());
            auto const pk = crypto::ed25519_public_key(seed);
            CHECK(std::ranges::equal(pk, key.x));
        }

        // The attestation statement verifies over SHA-256(clientDataJSON).
        auto const client_data = from_hex(v.reg_clientDataJSON);
        auto const hash = sha256_of(client_data);
        auto const check = verify_attestation_statement(*object, hash);
        bool const p521_attestation = section == "16.9";
        if (p521_attestation) {
            // The certificate's key is on P-521, which the certificate
            // parser does not read.
            CHECK(check != StatementCheck::Invalid);
        } else if (CHECK(check == StatementCheck::Valid)) {
            ++verified_statements;
        } else {
            std::cerr << "  section " << section << " statement\n";
        }
        // Controls: one bit of the client data, of the signature.
        auto tampered = client_data;
        tampered[10] ^= 0x01;
        if (!p521_attestation && object->fmt != "none")
            CHECK(verify_attestation_statement(*object, sha256_of(tampered)) == StatementCheck::Invalid);

        // getPublicKey(): an SPKI for every algorithm here, which the
        // certificate parser reads back for the curves it knows.
        auto const fields = created_credential_fields(*object, { "usb", "nfc", "usb" });
        CHECK(fields.has_value());
        if (fields) {
            CHECK(fields->public_key_spki.has_value());
            CHECK_EQ(fields->public_key_algorithm, key.alg.value_or(0));
            CHECK(fields->transports == std::vector<std::string>({ "nfc", "usb" }));
            CHECK(fields->authenticator_data == object->auth_data);
            if (fields->public_key_spki && (key.alg == cose::alg_es256 || key.alg == cose::alg_es384 || key.alg == cose::alg_rs256)) {
                auto const parsed = tls::parse_public_key(*fields->public_key_spki);
                CHECK(parsed.has_value());
            }
        }

        // The authentication ceremony's signature verifies under the key.
        auto const auth_data = from_hex(v.auth_authenticatorData);
        auto const auth_client_data = from_hex(v.auth_clientDataJSON);
        auto const signature = from_hex(v.auth_signature);
        auto const parsed_auth = parse_authenticator_data(auth_data);
        CHECK(parsed_auth && !parsed_auth->attested && parsed_auth->rp_id_hash == object->parsed.rp_id_hash);
        std::int64_t const alg = key.alg.value_or(0);
        if (CHECK(verify_assertion_signature(key, alg, auth_data, sha256_of(auth_client_data), signature)))
            ++verified_assertions;
        else
            std::cerr << "  section " << section << " assertion\n";
        auto bad_signature = signature;
        bad_signature[bad_signature.size() / 2] ^= 0x10;
        CHECK(!verify_assertion_signature(key, alg, auth_data, sha256_of(auth_client_data), bad_signature));
        auto bad_data = auth_data;
        bad_data[33] ^= 0x01;
        CHECK(!verify_assertion_signature(key, alg, bad_data, sha256_of(auth_client_data), signature));

        // Conveyance "none": the statement goes, the authenticator data stays.
        auto const none = apply_conveyance(*object, AttestationConveyance::None);
        CHECK_EQ(none.fmt, std::string("none"));
        CHECK(none.att_stmt.entries().empty());
        CHECK(none.auth_data == object->auth_data);
        CHECK(verify_attestation_statement(none, hash) == StatementCheck::Valid);
        auto const direct = apply_conveyance(*object, AttestationConveyance::Direct);
        CHECK_EQ(direct.fmt, object->fmt);
    }
    std::cout << "WebAuthn L3 §16 vectors: " << verified_statements << " attestation statements and " << verified_assertions
              << " assertion signatures verified\n";
    CHECK_EQ(verified_statements, 7);
    CHECK_EQ(verified_assertions, 8);

    // The attestation CA certificate of §16.1 parses, and the "packed" leaf
    // of §16.7 is signed by it.
    auto const ca = tls::parse_certificate(from_hex(attestation_ca_cert));
    CHECK(ca.has_value());
    auto const packed = parse_attestation_object(from_hex(webauthn_vectors[2].reg_attestationObject));
    if (ca && packed) {
        auto const leaf_der = packed->att_stmt.find("x5c")->items()[0].byte_vector();
        auto const leaf = tls::parse_certificate(leaf_der);
        CHECK(leaf && tls::verify_signature(ca->public_key, leaf->signature_algorithm, leaf->tbs(), leaf->signature));
    }

    // Self attestation with a zero AAGUID is left alone by "none".
    auto self = parse_attestation_object(from_hex(webauthn_vectors[1].reg_attestationObject));
    if (self) {
        self->parsed.attested->aaguid.fill(0);
        CHECK_EQ(apply_conveyance(*self, AttestationConveyance::None).fmt, std::string("packed"));
    }
}

void malformed_authenticator_data()
{
    auto const good = from_hex(webauthn_vectors[0].reg_attestationObject);
    auto const object = parse_attestation_object(good);
    auto const& data = object->auth_data;
    // Every truncation fails, and so does one byte more.
    for (std::size_t n = 0; n < data.size(); ++n)
        CHECK(!parse_authenticator_data(std::span<std::uint8_t const>(data).first(n)));
    auto longer = data;
    longer.push_back(0);
    CHECK(!parse_authenticator_data(longer));
    // ED set with nothing after; AT clear with a credential after.
    auto ed = data;
    ed[32] |= flags::ed;
    CHECK(!parse_authenticator_data(ed));
    auto no_at = data;
    no_at[32] &= static_cast<std::uint8_t>(~flags::at);
    CHECK(!parse_authenticator_data(no_at));
    // A credential ID length past the end.
    auto long_id = data;
    long_id[53] = 0x03;
    long_id[54] = 0xff;
    CHECK(!parse_authenticator_data(long_id));
    // An attestation object missing a member, or with authData of the wrong type.
    CHECK(!parse_attestation_object(from_hex("a263666d74646e6f6e656761747453746d74a0")));
    CHECK(!parse_attestation_object(from_hex("a363666d74646e6f6e656761747453746d74a06861757468446174616161")));
}

void prf_and_hmac_secret()
{
    // §16.17.1.1: the evalByCredential key and the PRF salt.
    std::string const seed = "WebAuthn PRF test vectors";
    auto seed_with = [&](std::uint8_t last) {
        std::vector<std::uint8_t> out(seed.begin(), seed.end());
        out.push_back(last);
        return out;
    };
    CHECK_EQ(to_hex(sha256_of(seed_with(0x01))), std::string("c4172e982e9097c39a6c0cb720cb375b92e3fcad154a63e43a93f1096b1e1973"));

    // §16.17.1.2: the client's side of hmac-secret for prf.
    auto const eval_first = seed_with(0x02);
    auto const eval_second = seed_with(0x03);
    CHECK_EQ(to_hex(eval_first), std::string("576562417574686e20505246207465737420766563746f727302"));
    // WebAuthn §10.1.4: salt = SHA-256("WebAuthn PRF" || 0x00 || input).
    auto salt_for = [](std::span<std::uint8_t const> input) {
        std::string const label = "WebAuthn PRF";
        std::vector<std::uint8_t> data(label.begin(), label.end());
        data.push_back(0x00);
        data.insert(data.end(), input.begin(), input.end());
        return sha256_of(data);
    };
    auto const salt1 = salt_for(eval_first);
    auto const salt2 = salt_for(eval_second);
    CHECK_EQ(to_hex(salt1), std::string("527413ebb48293772df30f031c5ac4650c7de14bf9498671ae163447b6a772b3"));
    CHECK_EQ(to_hex(salt2), std::string("d68ac03329a10ee5e0ec834492bb9a96a0e547baf563bf78ccbe8789b22e776b"));

    auto const platform_scalar = from_hex("0971bc7fb1be48270adcd3d9a5fc15d5fb0f335b3071ff36a54c007fa6c76514");
    CHECK_EQ(to_hex(sha256_of(seed_with(0x04))), to_hex(platform_scalar));
    auto const platform = pin::key_agreement_key_from_scalar(std::span<std::uint8_t const, 32>(platform_scalar.data(), 32));
    CHECK(platform.has_value());
    CoseKey authenticator;
    authenticator.kty = cose::kty_ec2;
    authenticator.alg = cose::alg_ecdh_es_hkdf_256;
    authenticator.crv = cose::crv_p256;
    authenticator.x = from_hex("a30522c2de402b561965c3cf949a1cab020c6f6ea36fcf7e911ac1a0f1515300");
    authenticator.y = from_hex("9961a929abdb2f42e6566771887d41484d889e735e3248518a53112d2b915f00");
    // The authenticator's key is the one sk = SHA-256(seed || 0x05) gives.
    auto const authenticator_scalar = sha256_of(seed_with(0x05));
    auto const authenticator_pair = pin::key_agreement_key_from_scalar(std::span<std::uint8_t const, 32>(authenticator_scalar.data(), 32));
    CHECK(authenticator_pair && authenticator_pair->public_key.x == authenticator.x && authenticator_pair->public_key.y == authenticator.y);
    if (!platform || !authenticator_pair)
        return;

    auto const cred_random = from_hex("437e065e723a98b2f08f39d8baf7c53ecb3c363c5e5104bdaaf5d5ca2e028154");
    auto const output1 = crypto::Hmac<crypto::Sha256>::mac(cred_random, salt1);
    auto const output2 = crypto::Hmac<crypto::Sha256>::mac(cred_random, salt2);
    CHECK_EQ(to_hex(output1), std::string("3c33e07d202c3b029cc21f1722767021bf27d595933b3d2b6a1b9d5dddc77fae"));
    CHECK_EQ(to_hex(output2), std::string("a62a8773b19cda90d7ed4ef72a80a804320dbd3997e2f663805ad1fd3293d50b"));

    auto truncated_iv = [&](std::uint8_t last) {
        auto const d = sha256_of(seed_with(last));
        std::array<std::uint8_t, 16> iv {};
        std::copy_n(d.begin(), 16, iv.begin());
        return iv;
    };

    // Protocol two, one salt and two.
    auto const shared2 = pin::ecdh(pin::Protocol::Two, *platform, authenticator);
    CHECK(shared2.has_value());
    if (shared2) {
        CHECK_EQ(to_hex(*shared2), std::string("0c63083de8170101d38bcf8bd72309568ddb4550867e23404b35d85712f7c20d"
                                               "8bc911ee23c06034cbc14290b9669bec07739053c5a416e313ef905c79955876"));
        // Both sides reach the same secret.
        CHECK(pin::ecdh(pin::Protocol::Two, *authenticator_pair, platform->public_key) == shared2);
        auto const enc1 = pin::encrypt_with_iv(*shared2, truncated_iv(0x07), salt1);
        CHECK(enc1 && to_hex(*enc1) == "23dde5e3462daf36559b85c4ac5f9656aa9bfd81c1dc2bf8533c8b9f3882854786b4f500e25b4e3d81f7fc7c74236229");
        std::vector<std::uint8_t> salts = salt1;
        salts.insert(salts.end(), salt2.begin(), salt2.end());
        auto const enc2 = pin::encrypt_with_iv(*shared2, truncated_iv(0x08), salts);
        CHECK(enc2
            && to_hex(*enc2)
                == "d9f4236403e0fe843a8e4e5be764d120904c198ad6e77b089876a3391961f183b0008b4ca66b91cd72aa35b6151ff981f6e5649f3c040e6615ad7dd8ae96ef2"
                   "3b229a5c97c3f0dcd8605eee166ce163a");
        auto const out1 = pin::decrypt(pin::Protocol::Two, *shared2,
            from_hex("3bfaa48f7952330d63e35ff8cd5bca48d2a12823828915749287256ab146272f9fb437bf65691243c3f504bd7ea6d5e6"));
        CHECK(out1 && to_hex(*out1) == to_hex(output1));
        auto const out2 = pin::decrypt(pin::Protocol::Two, *shared2,
            from_hex("90ee52f739043bc17b3488a74306d7801debb5b61f18662c648a25b5b5678ede482cdaff99a537a44f064fcb10ce6e04dfd27619dc96a0daff8507e499296b1"
                     "eecf0981f7c8518b277a7a3018f5ec6fb"));
        CHECK(out2 && to_hex(*out2) == to_hex(output1) + to_hex(output2));
        // The authenticator's encryption of the outputs, with the vector's IV.
        auto const output_enc = pin::encrypt_with_iv(*shared2, truncated_iv(0x09), output1);
        CHECK(output_enc && to_hex(*output_enc) == "3bfaa48f7952330d63e35ff8cd5bca48d2a12823828915749287256ab146272f9fb437bf65691243c3f504bd7ea6d5e6");
        // A wrong key, a short ciphertext.
        auto wrong = *shared2;
        wrong[40] ^= 1;
        auto const garbled = pin::decrypt(pin::Protocol::Two, wrong, *output_enc);
        CHECK(!garbled || to_hex(*garbled) != to_hex(output1));
        CHECK(!pin::decrypt(pin::Protocol::Two, *shared2, std::vector<std::uint8_t>(15)));
        CHECK(!pin::decrypt(pin::Protocol::Two, *shared2, std::vector<std::uint8_t>(17)));
        // saltAuth is the full HMAC under the first half.
        CHECK_EQ(pin::authenticate(pin::Protocol::Two, *shared2, *enc1).size(), std::size_t { 32 });
        CHECK(pin::verify(pin::Protocol::Two, *shared2, *enc1, pin::authenticate(pin::Protocol::Two, *shared2, *enc1)));
        auto mac = pin::authenticate(pin::Protocol::Two, *shared2, *enc1);
        mac[0] ^= 1;
        CHECK(!pin::verify(pin::Protocol::Two, *shared2, *enc1, mac));
    }

    // Protocol one.
    auto const shared1 = pin::ecdh(pin::Protocol::One, *platform, authenticator);
    CHECK(shared1 && to_hex(*shared1) == "23e5ed7157c25892b77732fb9c8a107e3518800db2af4142f9f4adfacb771d39");
    if (shared1) {
        auto const enc = pin::encrypt(pin::Protocol::One, *shared1, salt1, sashfold::platform::fill_random);
        CHECK(enc && to_hex(*enc) == "ab8c878bb05d04700f077ed91845ec9c503c925cb12b327ddbeb4243c397f913");
        auto const out = pin::decrypt(pin::Protocol::One, *shared1, from_hex("15d4e4f3f04109b492b575c1b38c28585b6719cf8d61304215108d939f37ccfb"));
        CHECK(out && to_hex(*out) == to_hex(output1));
        CHECK_EQ(pin::authenticate(pin::Protocol::One, *shared1, salt1).size(), std::size_t { 16 });
        CHECK(!pin::encrypt(pin::Protocol::One, *shared1, std::vector<std::uint8_t>(31), sashfold::platform::fill_random));
    }

    // A peer point off the curve is refused.
    CoseKey off = authenticator;
    off.y[31] ^= 1;
    CHECK(!pin::ecdh(pin::Protocol::Two, *platform, off));

    // The PIN encodings of §6.5.5.5.
    CHECK(!pin::padded_pin("123"));
    CHECK(pin::padded_pin("1234").has_value());
    CHECK(!pin::padded_pin("\xc3\xa9\xc3\xa9\xc3\xa9")); // three code points in six bytes
    CHECK(pin::padded_pin("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9").has_value());
    CHECK(!pin::padded_pin(std::string(64, '1')));
    CHECK_EQ(pin::padded_pin("1234")->size(), std::size_t { 64 });
    CHECK_EQ(to_hex(pin::pin_hash("1234")), std::string("03ac674216f3e15c761ee1a5e255f067"));
}

}

int main()
{
    ceremonies();
    malformed_authenticator_data();
    prf_and_hmac_secret();
    return sashfold::test::report("test_auth_vectors");
}
