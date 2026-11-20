// The extensions the web platform passes to keys (CTAP 2.1 §12, WebAuthn
// Level 3 §10): credProtect and what it hides, prf through hmac-secret on
// both PIN/UV auth protocols (the outputs checked against the key's own
// secret, computed independently), credBlob, largeBlobKey, minPinLength,
// and credProps, computed by the client.
#include "AuthCeremony.h"
#include "SoftAuthenticator.h"
#include "Test.h"

#include "auth/Attestation.h"
#include "auth/Client.h"
#include "crypto/Hmac.h"
#include "crypto/Sha2.h"

using namespace sashfold;
using namespace sashfold::auth;
using sashfold::test::create_request;
using sashfold::test::FakeTransport;
using sashfold::test::get_request;
using sashfold::test::pin_answer;
using sashfold::test::run_ceremony;
using sashfold::test::ScriptedPrompt;
using sashfold::test::SoftOptions;

namespace {

std::shared_ptr<ScriptedPrompt> prompt_with_pin(int times = 1)
{
    auto prompt = std::make_shared<ScriptedPrompt>();
    for (int i = 0; i < times; ++i)
        prompt->pins.push_back(pin_answer("1234"));
    return prompt;
}

std::optional<cbor::Value> authenticator_outputs(Result const& result)
{
    auto const object = parse_attestation_object(result.attestation_object);
    if (!object || !object->parsed.extensions)
        return std::nullopt;
    return object->parsed.extensions;
}

void cred_protect()
{
    SoftOptions options;
    options.pin = "1234";
    auto transport = std::make_shared<FakeTransport>();
    auto key = transport->plug(options);
    for (std::uint8_t level = 1; level <= 3; ++level) {
        auto request = create_request();
        request.resident_key = Requirement::Required;
        request.user.id = { level };
        request.extensions.cred_protect = level;
        request.extensions.cred_props = true;
        auto const outcome = run_ceremony(transport, request, prompt_with_pin());
        CHECK(outcome.result.has_value());
        if (!outcome.result)
            continue;
        CHECK(outcome.result->extensions.cred_protect == level);
        CHECK(outcome.result->extensions.cred_props_rk == true);
        auto const outputs = authenticator_outputs(*outcome.result);
        CHECK(outputs && outputs->find("credProtect") && outputs->find("credProtect")->raw_number() == level);
    }
    // Without UV, an empty allow list finds only level 1 (§12.1).
    auto discoverable = get_request({});
    discoverable.user_verification = Requirement::Discouraged;
    // The key has a PIN, so "discouraged" still skips it; the user picks
    // from what the key returns.
    auto choose = std::make_shared<ScriptedPrompt>();
    auto const without_uv = run_ceremony(transport, discoverable, choose);
    CHECK(without_uv.result && without_uv.result->user_handle == std::vector<std::uint8_t>({ 1 }));
    CHECK(choose->accounts.empty()); // one account only: nothing to choose
    // With UV all three are offered.
    discoverable.user_verification = Requirement::Required;
    auto all = prompt_with_pin();
    auto const with_uv = run_ceremony(transport, discoverable, all);
    CHECK(with_uv.result.has_value());
    CHECK_EQ(all->accounts.size(), std::size_t { 3 });
    // Level 2 with its credential ID listed: found without UV; level 3: not.
    auto const credentials = key->credentials();
    for (auto const& credential : credentials) {
        auto listed = get_request({ credential.id });
        auto const outcome = run_ceremony(transport, listed, std::make_shared<ScriptedPrompt>());
        if (credential.cred_protect < 3)
            CHECK(outcome.result.has_value());
        else
            CHECK(outcome.error && outcome.error->reason == Reason::NotRegistered);
    }
    // Level 3 excluded: the probe without UV cannot see it, but with UV
    // (here required) the key is still recognized.
    auto exclude = create_request();
    exclude.user_verification = Requirement::Required;
    for (auto const& credential : credentials)
        if (credential.cred_protect == 3)
            exclude.credentials.push_back({ credential.id, {} });
    auto const excluded = run_ceremony(transport, exclude, prompt_with_pin());
    CHECK(excluded.error && excluded.error->kind == ErrorKind::InvalidState);

    // enforceCredentialProtectionPolicy on a key without credProtect: unsuitable.
    SoftOptions plain;
    plain.cred_protect = false;
    auto transport2 = std::make_shared<FakeTransport>();
    transport2->plug(plain);
    auto enforced = create_request();
    enforced.extensions.cred_protect = 2;
    enforced.extensions.enforce_cred_protect = true;
    auto const refused = run_ceremony(transport2, enforced, std::make_shared<ScriptedPrompt>());
    CHECK(refused.error && refused.error->reason == Reason::Unsuitable);
    enforced.extensions.enforce_cred_protect = false;
    auto const allowed = run_ceremony(transport2, enforced, std::make_shared<ScriptedPrompt>());
    CHECK(allowed.result && !allowed.result->extensions.cred_protect);
}

// What the key's hmac-secret gives for a prf input, computed here from the
// key's stored secret (§12.5 and WebAuthn §10.1.4), not by the client.
std::vector<std::uint8_t> expected_prf(test::SoftAuthenticator::Credential const& credential, bool uv, std::vector<std::uint8_t> const& input)
{
    std::string const label = "WebAuthn PRF";
    std::vector<std::uint8_t> salted(label.begin(), label.end());
    salted.push_back(0);
    salted.insert(salted.end(), input.begin(), input.end());
    auto const salt = crypto::Sha256::hash(salted);
    auto const& random = uv ? credential.cred_random_with_uv : credential.cred_random_without_uv;
    auto const mac = crypto::Hmac<crypto::Sha256>::mac(random, salt);
    return std::vector<std::uint8_t>(mac.begin(), mac.end());
}

void prf(std::uint8_t protocol)
{
    SoftOptions options;
    options.pin = "1234";
    options.pin_protocols = { protocol };
    auto transport = std::make_shared<FakeTransport>();
    auto key = transport->plug(options);
    auto request = create_request();
    request.extensions.prf = true;
    auto const created = run_ceremony(transport, request, std::make_shared<ScriptedPrompt>());
    CHECK(created.result && created.result->extensions.prf_enabled == true);
    auto const other = run_ceremony(transport, create_request(), std::make_shared<ScriptedPrompt>());
    if (!created.result || !other.result)
        return;
    auto const credentials = key->credentials();
    CHECK_EQ(credentials.size(), std::size_t { 2 });

    std::vector<std::uint8_t> const first { 1, 2, 3, 4 };
    std::vector<std::uint8_t> const second { 5, 6, 7, 8 };
    std::vector<std::uint8_t> const special { 9, 10, 11, 12 };
    // eval with two inputs, without UV.
    auto get = get_request({ created.result->credential_id });
    get.extensions.prf = true;
    get.extensions.prf_eval = PrfInputs { first, second };
    auto const asserted = run_ceremony(transport, get, std::make_shared<ScriptedPrompt>());
    CHECK(asserted.result.has_value());
    if (asserted.result) {
        CHECK(asserted.result->extensions.prf_first == expected_prf(credentials[0], false, first));
        CHECK(asserted.result->extensions.prf_second == expected_prf(credentials[0], false, second));
    }
    // With UV the other secret, so other outputs.
    get.user_verification = Requirement::Required;
    get.extensions.prf_eval = PrfInputs { first, std::nullopt };
    auto const verified = run_ceremony(transport, get, prompt_with_pin());
    CHECK(verified.result.has_value());
    if (verified.result) {
        CHECK(verified.result->extensions.prf_first == expected_prf(credentials[0], true, first));
        CHECK(!verified.result->extensions.prf_second);
        CHECK(verified.result->extensions.prf_first != expected_prf(credentials[0], false, first));
    }
    // evalByCredential picks the inputs of the credential that answered.
    auto by_credential = get_request({ other.result->credential_id });
    by_credential.extensions.prf = true;
    by_credential.extensions.prf_eval = PrfInputs { first, std::nullopt };
    by_credential.extensions.prf_eval_by_credential = { { created.result->credential_id, PrfInputs { special, std::nullopt } },
        { other.result->credential_id, PrfInputs { special, special } } };
    auto const picked = run_ceremony(transport, by_credential, std::make_shared<ScriptedPrompt>());
    CHECK(picked.result.has_value());
    if (picked.result) {
        CHECK(picked.result->extensions.prf_first == expected_prf(credentials[1], false, special));
        CHECK(picked.result->extensions.prf_second == picked.result->extensions.prf_first);
    }
    // No eval: no results.
    auto bare = get_request({ created.result->credential_id });
    bare.extensions.prf = true;
    auto const nothing = run_ceremony(transport, bare, std::make_shared<ScriptedPrompt>());
    CHECK(nothing.result && !nothing.result->extensions.prf_first);
    // hmacCreateSecret, the CTAP name for the same thing.
    auto hmac = create_request();
    hmac.extensions.hmac_create_secret = true;
    auto const hmac_created = run_ceremony(transport, hmac, std::make_shared<ScriptedPrompt>());
    CHECK(hmac_created.result && hmac_created.result->extensions.hmac_create_secret == true);

    // A key without hmac-secret: prf.enabled false.
    SoftOptions without;
    without.hmac_secret = false;
    auto transport2 = std::make_shared<FakeTransport>();
    transport2->plug(without);
    auto const disabled = run_ceremony(transport2, request, std::make_shared<ScriptedPrompt>());
    CHECK(disabled.result && disabled.result->extensions.prf_enabled == false);
}

void cred_blob_large_blob_key_min_pin_length()
{
    SoftOptions options;
    options.pin = "1234";
    options.min_pin_length_rp_ids = { "example.org" };
    auto transport = std::make_shared<FakeTransport>();
    auto key = transport->plug(options);

    auto request = create_request();
    request.resident_key = Requirement::Required;
    request.extensions.cred_blob = std::vector<std::uint8_t>(32, 0x5a);
    request.extensions.large_blob_key = true;
    request.extensions.min_pin_length = true;
    auto const created = run_ceremony(transport, request, prompt_with_pin());
    CHECK(created.result.has_value());
    if (!created.result)
        return;
    CHECK(created.result->extensions.cred_blob_stored == true);
    CHECK(created.result->extensions.large_blob_key && created.result->extensions.large_blob_key->size() == 32);
    CHECK(created.result->extensions.min_pin_length == std::uint64_t { 4 });

    auto get = get_request({ created.result->credential_id });
    get.extensions.get_cred_blob = true;
    get.extensions.large_blob_key = true;
    auto const asserted = run_ceremony(transport, get, std::make_shared<ScriptedPrompt>());
    CHECK(asserted.result.has_value());
    if (asserted.result) {
        CHECK(asserted.result->extensions.cred_blob == std::vector<std::uint8_t>(32, 0x5a));
        CHECK(asserted.result->extensions.large_blob_key == created.result->extensions.large_blob_key);
    }

    // A blob longer than the key keeps is not sent; minPinLength is for listed RPs only.
    auto other = create_request("other.example");
    other.resident_key = Requirement::Required;
    other.extensions.cred_blob = std::vector<std::uint8_t>(33, 1);
    other.extensions.min_pin_length = true;
    auto const second = run_ceremony(transport, other, prompt_with_pin());
    CHECK(second.result.has_value());
    if (second.result) {
        CHECK(!second.result->extensions.cred_blob_stored);
        CHECK(!second.result->extensions.min_pin_length);
        auto const outputs = authenticator_outputs(*second.result);
        CHECK(!outputs || outputs->find("credBlob") == nullptr);
    }
    // getCredBlob of a credential without one: empty.
    auto get_other = get_request({ second.result ? second.result->credential_id : std::vector<std::uint8_t> {} }, "other.example");
    get_other.extensions.get_cred_blob = true;
    auto const empty_blob = run_ceremony(transport, get_other, std::make_shared<ScriptedPrompt>());
    CHECK(empty_blob.result && empty_blob.result->extensions.cred_blob == std::vector<std::uint8_t> {});
    // largeBlobKey asks for a discoverable credential: without one the key gets none.
    auto non_resident = create_request();
    non_resident.extensions.large_blob_key = true;
    auto const no_key = run_ceremony(transport, non_resident, std::make_shared<ScriptedPrompt>());
    CHECK(no_key.result && !no_key.result->extensions.large_blob_key);
    CHECK(no_key.result && no_key.result->extensions.cred_props_rk == std::nullopt); // not asked for
}

}

int main()
{
    cred_protect();
    prf(1);
    prf(2);
    cred_blob_large_blob_key_min_pin_length();
    return sashfold::test::report("test_auth_extensions");
}
