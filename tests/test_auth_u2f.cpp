// CTAP1/U2F: the message formats, CTAP 2.1 §10.2 and §10.3's own sample
// registration and authentication mapped to WebAuthn byte for byte, and
// whole ceremonies with a key that speaks only U2F — "fido-u2f"
// attestation, the touch polled for, the exclude list by check-only, a
// key not registered, what U2F cannot do, and the appid / appidExclude
// extensions on U2F and CTAP2 keys alike.
#include "AuthCeremony.h"
#include "AuthTestSupport.h"
#include "SoftAuthenticator.h"
#include "Test.h"

#include "auth/Attestation.h"
#include "auth/Client.h"
#include "auth/U2f.h"
#include "crypto/Sha2.h"

using namespace sashfold;
using namespace sashfold::auth;
using sashfold::test::create_request;
using sashfold::test::FakeTransport;
using sashfold::test::from_hex;
using sashfold::test::get_request;
using sashfold::test::run_ceremony;
using sashfold::test::ScriptedPrompt;
using sashfold::test::SoftOptions;
using sashfold::test::to_hex;
using sashfold::test::TouchPlan;
using Clock = std::chrono::steady_clock;

namespace {

// CTAP 2.1 §10.2, "Sample CTAP1/U2F Response from the device" and the
// authenticator data it maps to.
char const* const sample_registration
    = "05"
      "04E87625896EE4E46DC032766E8087962F36DF9DFE8B567F3763015B1990A60E1427DE612D66418BDA1950581EBC5C8C1DAD710CB14C22F8C97045F4612FB20C91"
      "40"
      "3EBD89BF77EC509755EE9C2635EFAAAC7B2B9C5CEF1736C3717DA48534C8C6B654D7FF945F50B5CC4E78055BDD396B64F78DA2C5F96200CCD415CD08FE420038"
      "3082024A30820132A0030201020204046C8822300D06092A864886F70D01010B0500302E312C302A0603550403132359756269636F2055324620526F6F742043"
      "412053657269616C203435373230303633313020170D3134303830313030303030305A180F32303530303930343030303030305A302C312A302806035504030C"
      "2159756269636F205532462045452053657269616C203234393138323332343737303059301306072A8648CE3D020106082A8648CE3D030107034200043CCAB9"
      "2CCB97287EE8E639437E21FCD6B6F165B2D5A3F3DB131D31C16B742BB476D8D1E99080EB546C9BBDF556E6210FD42785899E78CC589EBE310F6CDB9FF4A33B30"
      "39302206092B0601040182C40A020415312E332E362E312E342E312E34313438322E312E323013060B2B0601040182E51C020101040403020430300D06092A86"
      "4886F70D01010B050003820101009F9B052248BC4CF42CC5991FCAABAC9B651BBE5BDCDC8EF0AD2C1C1FFB36D18715D42E78B249224F92C7E6E7A05C49F0E7E4"
      "C881BF2E94F45E4A21833D7456851D0F6C145A29540C874F3092C934B43D222B8962C0F410CEF1DB75892AF116B44A96F5D35ADEA3822FC7146F6004385BCB69"
      "B65C99E7EB6919786703C0D8CD41E8F75CCA44AA8AB725AD8E799FF3A8696A6F1B2656E631B1E40183C08FDA53FA4A8F85A05693944AE179A1339D002D15CABD"
      "810090EC722EF5DEF9965A371D415D624B68A2707CAD97BCDD1785AF97E258F33DF56A031AA0356D8E8D5EBCADC74E071636C6B110ACE5CC9B90DFEACAE640FF"
      "1BB0F1FE5DB4EFF7A95F060733F5"
      "30450220324779C68F3380288A1197B6095F7A6EB9B1B1C127F66AE12A99FE8532EC23B9022100E39516AC4D61EE64044D50B415A6A4D4D84BA6D895CB5AB7A1"
      "AA7D081DE341FA";
char const* const sample_registration_auth_data
    = "A379A6F6EEAFB9A55E378C118034E2751E682FAB9F2D30AB13D2125586CE1947"
      "41"
      "00000000"
      "00000000000000000000000000000000"
      "0040"
      "3EBD89BF77EC509755EE9C2635EFAAAC7B2B9C5CEF1736C3717DA48534C8C6B654D7FF945F50B5CC4E78055BDD396B64F78DA2C5F96200CCD415CD08FE420038"
      "A5010203262001215820E87625896EE4E46DC032766E8087962F36DF9DFE8B567F3763015B1990A60E1422582027DE612D66418BDA1950581EBC5C8C1DAD710C"
      "B14C22F8C97045F4612FB20C91";
// §10.3's sample authentication answer and its authenticator data.
char const* const sample_authentication
    = "01"
      "0000003B"
      "304402207BDE0A52AC1F4C8B27E003A370CD66A4C7118DD22D5447835F45B99C68423FF702203C517B47877F85782DE10086A783D1E7DF4E3639E771F5F6AFA3"
      "5AAD5373858E";
char const* const sample_authentication_auth_data = "A379A6F6EEAFB9A55E378C118034E2751E682FAB9F2D30AB13D2125586CE1947010000003B";
char const* const sample_rp_id_hash = "A379A6F6EEAFB9A55E378C118034E2751E682FAB9F2D30AB13D2125586CE1947";
char const* const sample_client_data_hash = "687134968222EC17202E42505F8ED2B16AE22F16BB05B88C25DB9E602645F141";

std::string upper(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(c >= 'a' && c <= 'f' ? c - 32 : c);
    return text;
}

void specification_samples()
{
    // The rpIdHash of the sample is SHA-256("example.com").
    std::string_view const rp = "example.com";
    auto const rp_hash = crypto::Sha256::hash(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(rp.data()), rp.size()));
    CHECK_EQ(upper(to_hex(rp_hash)), std::string(sample_rp_id_hash));

    // §10.2: the request APDU's data is clientDataHash || rpIdHash.
    auto const challenge = from_hex(sample_client_data_hash);
    auto const request = u2f::register_request(std::span<std::uint8_t const, 32>(challenge.data(), 32), rp_hash);
    CHECK_EQ(to_hex(std::span<std::uint8_t const>(request).first(7)), std::string("00010000000040"));
    CHECK_EQ(upper(to_hex(std::span<std::uint8_t const>(request).subspan(7, 64))), std::string(sample_client_data_hash) + sample_rp_id_hash);
    CHECK_EQ(to_hex(std::span<std::uint8_t const>(request).last(2)), std::string("0000"));

    auto const registration = u2f::parse_registration(from_hex(sample_registration));
    CHECK(registration.has_value());
    if (!registration)
        return;
    CHECK_EQ(registration->key_handle.size(), std::size_t { 64 });
    CHECK_EQ(registration->certificate.size(), std::size_t { 4 + 0x24a });
    CHECK_EQ(registration->signature.size(), std::size_t { 71 });
    auto const auth_data = u2f::registration_authenticator_data(rp_hash, *registration);
    CHECK_EQ(upper(to_hex(auth_data)), std::string(sample_registration_auth_data));
    // The attestation object reads back as fido-u2f with the certificate.
    auto const object = parse_attestation_object(u2f::registration_attestation_object(rp_hash, *registration));
    CHECK(object && object->fmt == "fido-u2f" && object->parsed.attested && object->parsed.attested->credential_id == registration->key_handle);

    auto const authentication = u2f::parse_authentication(from_hex(sample_authentication));
    CHECK(authentication && authentication->counter == 0x3b && authentication->user_presence == 1);
    if (authentication)
        CHECK_EQ(upper(to_hex(u2f::authentication_authenticator_data(rp_hash, *authentication))), std::string(sample_authentication_auth_data));
    // §10.3: the authenticate request.
    auto const handle = registration->key_handle;
    auto const auth_request = u2f::authenticate_request(u2f::check_only, std::span<std::uint8_t const, 32>(challenge.data(), 32), rp_hash, handle);
    CHECK(auth_request && auth_request->size() == 7 + 65 + 64 + 2);
    CHECK(auth_request && (*auth_request)[2] == 0x07 && (*auth_request)[7 + 64] == 0x40);
    CHECK(!u2f::authenticate_request(u2f::check_only, rp_hash, rp_hash, std::vector<std::uint8_t>(256)));

    // Malformed answers.
    auto broken = from_hex(sample_registration);
    broken[0] = 0x04;
    CHECK(!u2f::parse_registration(broken));
    auto off_curve = from_hex(sample_registration);
    off_curve[30] ^= 1;
    CHECK(!u2f::parse_registration(off_curve));
    auto const full = from_hex(sample_registration);
    for (std::size_t n = 0; n < full.size() - 8; n += 7)
        CHECK(!u2f::parse_registration(std::span<std::uint8_t const>(full).first(n)));
    CHECK(!u2f::parse_authentication(from_hex("010000003b")));
    CHECK(!u2f::split_answer(from_hex("90")));
    CHECK(u2f::split_answer(from_hex("6985"))->status == u2f::sw_conditions_not_satisfied);
}

SoftOptions u2f_only()
{
    SoftOptions options;
    options.protocol = SoftOptions::Protocol::U2fOnly;
    return options;
}

void u2f_ceremonies()
{
    auto transport = std::make_shared<FakeTransport>();
    auto key = transport->plug(u2f_only());
    key->set_touch(TouchPlan::after(300));
    auto prompt = std::make_shared<ScriptedPrompt>();
    auto request = create_request();
    auto const created = run_ceremony(transport, request, prompt);
    CHECK(created.result.has_value());
    if (!created.result)
        return;
    auto const object = parse_attestation_object(created.result->attestation_object);
    CHECK(object && object->fmt == "fido-u2f");
    if (object) {
        CHECK(verify_attestation_statement(*object, request.client_data_hash) == StatementCheck::Valid);
        CHECK(object->att_stmt.find("x5c")->items()[0].byte_vector() == key->attestation_certificate());
        auto tampered = request.client_data_hash;
        tampered[0] ^= 1;
        CHECK(verify_attestation_statement(*object, tampered) == StatementCheck::Invalid);
    }
    // It asked more than once before the touch came.
    CHECK(key->stats().u2f_requests.load() >= 3);
    CHECK(created.result->created && created.result->created->public_key_algorithm == cose::alg_es256);

    auto const get = get_request({ from_hex("0011"), created.result->credential_id });
    auto const asserted = run_ceremony(transport, get, std::make_shared<ScriptedPrompt>());
    CHECK(asserted.result.has_value());
    if (asserted.result && object) {
        CHECK(verify_assertion_signature(object->parsed.attested->public_key, cose::alg_es256, asserted.result->authenticator_data,
            get.client_data_hash, asserted.result->signature));
        auto const data = parse_authenticator_data(asserted.result->authenticator_data);
        CHECK(data && data->user_present() && data->sign_count == 1);
        CHECK(asserted.result->credential_id == created.result->credential_id);
    }

    // Excluded: check-only finds it, a bogus registration collects the touch.
    auto excluded_request = create_request();
    excluded_request.credentials = { { created.result->credential_id, {} } };
    auto excluded_prompt = std::make_shared<ScriptedPrompt>();
    auto const excluded = run_ceremony(transport, excluded_request, excluded_prompt);
    CHECK(excluded.error && excluded.error->kind == ErrorKind::InvalidState);
    CHECK(excluded_prompt->saw(PromptState::KeyAlreadyRegistered));
    CHECK_EQ(key->credentials().size(), std::size_t { 2 }); // the real one and the bogus one

    // Not registered.
    auto missing_prompt = std::make_shared<ScriptedPrompt>();
    auto const missing = run_ceremony(transport, get_request({ from_hex("0102030405") }), missing_prompt);
    CHECK(missing.error && missing.error->reason == Reason::NotRegistered);
    CHECK(missing_prompt->saw(PromptState::KeyNotRegistered));

    // What U2F cannot do: discoverable credentials, UV, other algorithms.
    for (int which = 0; which < 4; ++which) {
        Request unsuitable = which < 3 ? create_request() : get_request({});
        if (which == 0)
            unsuitable.resident_key = Requirement::Required;
        if (which == 1)
            unsuitable.user_verification = Requirement::Required;
        if (which == 2)
            unsuitable.algorithms = { cose::alg_eddsa };
        auto p = std::make_shared<ScriptedPrompt>();
        auto const outcome = run_ceremony(transport, unsuitable, p);
        CHECK(outcome.error && outcome.error->reason == Reason::Unsuitable);
        CHECK(p->saw(PromptState::KeyUnsuitable));
    }
    // "preferred" UV and resident key are fine on U2F (and credProps says rk false).
    auto preferred = create_request();
    preferred.user_verification = Requirement::Preferred;
    preferred.resident_key = Requirement::Preferred;
    preferred.extensions.cred_props = true;
    auto const fine = run_ceremony(transport, preferred, std::make_shared<ScriptedPrompt>());
    CHECK(fine.result && fine.result->extensions.cred_props_rk == false);

    // The touch never comes: the deadline ends it, NotAllowedError.
    key->set_touch(TouchPlan::never());
    auto slow = create_request();
    slow.timeout = std::chrono::milliseconds(600);
    auto const timed_out = run_ceremony(transport, slow, std::make_shared<ScriptedPrompt>());
    CHECK(timed_out.error && timed_out.error->reason == Reason::Timeout && timed_out.error->kind == ErrorKind::NotAllowed);
}

void appid()
{
    std::string const appid = "https://example.org/u2f-app-id.json";
    // A U2F key registered long ago under the AppID (U2F's rp was the AppID string).
    for (bool dual : { false, true }) {
        auto transport = std::make_shared<FakeTransport>();
        SoftOptions options = dual ? SoftOptions {} : u2f_only();
        auto key = transport->plug(options);
        auto legacy = create_request(appid);
        legacy.attestation = AttestationConveyance::Direct;
        Outcome registered;
        if (dual) {
            // A dual key made it through U2F: drive its U2F side by hand.
            DeviceInfo device;
            CtapHidDevice hid(key->connect(), device);
            CHECK(hid.init(Clock::now() + std::chrono::seconds(1)));
            auto const app = crypto::Sha256::hash(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(appid.data()), appid.size()));
            auto const answer = u2f::u2f_poll(hid, u2f::register_request(legacy.client_data_hash, app), Clock::now() + std::chrono::seconds(2), nullptr);
            CHECK(answer && answer->status == u2f::sw_no_error);
            auto const registration = answer ? u2f::parse_registration(answer->data) : std::nullopt;
            CHECK(registration.has_value());
            if (!registration)
                continue;
            Result result;
            result.credential_id = registration->key_handle;
            result.attestation_object = u2f::registration_attestation_object(app, *registration);
            registered.result = result;
        } else {
            registered = run_ceremony(transport, legacy, std::make_shared<ScriptedPrompt>());
        }
        CHECK(registered.result.has_value());
        if (!registered.result)
            continue;
        auto const object = parse_attestation_object(registered.result->attestation_object);

        // Without the extension the RP ID finds nothing.
        auto get = get_request({ registered.result->credential_id });
        auto const without = run_ceremony(transport, get, std::make_shared<ScriptedPrompt>());
        CHECK(without.error && without.error->reason == Reason::NotRegistered);
        // With it, the AppID: authenticator data under SHA-256(AppID), appid: true.
        get.extensions.appid = appid;
        auto const with = run_ceremony(transport, get, std::make_shared<ScriptedPrompt>());
        CHECK(with.result.has_value());
        if (with.result && object) {
            CHECK(with.result->extensions.appid == true);
            auto const data = parse_authenticator_data(with.result->authenticator_data);
            CHECK(data && data->rp_id_hash == object->parsed.rp_id_hash);
            CHECK(verify_assertion_signature(object->parsed.attested->public_key, cose::alg_es256, with.result->authenticator_data,
                get.client_data_hash, with.result->signature));
        }
        // A credential under the RP ID itself: appid false.
        auto const fresh = run_ceremony(transport, create_request(), std::make_shared<ScriptedPrompt>());
        if (fresh.result) {
            auto both = get_request({ fresh.result->credential_id });
            both.extensions.appid = appid;
            auto const outcome = run_ceremony(transport, both, std::make_shared<ScriptedPrompt>());
            CHECK(outcome.result && outcome.result->extensions.appid == false);
        }
        // appidExclude: the AppID's credential excludes the key.
        auto exclude = create_request();
        exclude.credentials = { { registered.result->credential_id, {} } };
        auto const not_excluded = run_ceremony(transport, exclude, std::make_shared<ScriptedPrompt>());
        CHECK(not_excluded.result && not_excluded.result->extensions.appid_exclude == std::nullopt);
        exclude.extensions.appid_exclude = appid;
        auto const excluded = run_ceremony(transport, exclude, std::make_shared<ScriptedPrompt>());
        CHECK(excluded.error && excluded.error->kind == ErrorKind::InvalidState);
    }
}

}

int main()
{
    specification_samples();
    u2f_ceremonies();
    appid();
    return sashfold::test::report("test_auth_u2f");
}
