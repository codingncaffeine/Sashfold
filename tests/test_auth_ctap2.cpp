// CTAP 2.1 commands through the whole client: authenticatorGetInfo parsed
// member by member, make-credential and get-assertion ceremonies whose
// attestation objects and signatures verify, several accounts, the PIN/UV
// auth protocols one and two with every PIN state (set, enter, wrong,
// blocked, three in a row, forced change), built-in UV, CTAP 2.0 keys, and
// the controls: a wrong pinUvAuthParam is refused by the key, a flipped
// signature bit fails verification.
#include "AuthCeremony.h"
#include "AuthTestSupport.h"
#include "SoftAuthenticator.h"
#include "Test.h"

#include "auth/Attestation.h"
#include "auth/Client.h"
#include "auth/Ctap2.h"
#include "auth/CtapHid.h"
#include "auth/PinProtocol.h"

using namespace sashfold;
using namespace sashfold::auth;
using sashfold::test::create_request;
using sashfold::test::FakeTransport;
using sashfold::test::get_request;
using sashfold::test::pin_answer;
using sashfold::test::run_ceremony;
using sashfold::test::ScriptedPrompt;
using sashfold::test::SoftOptions;
using sashfold::test::TouchPlan;
using Clock = std::chrono::steady_clock;
using Status = sashfold::auth::ctap2::Status;

namespace {

auto in(int ms) { return Clock::now() + std::chrono::milliseconds(ms); }

// Registers a credential and returns the result; the attestation must verify.
std::optional<Result> registered(std::shared_ptr<FakeTransport> transport, Request request, std::shared_ptr<ScriptedPrompt> prompt = nullptr)
{
    if (!prompt)
        prompt = std::make_shared<ScriptedPrompt>();
    auto const outcome = run_ceremony(transport, request, prompt);
    if (!CHECK(outcome.result.has_value())) {
        if (outcome.error)
            std::cerr << "  error reason " << int(outcome.error->reason) << " status " << int(outcome.error->ctap_status) << "\n";
        return std::nullopt;
    }
    auto const object = parse_attestation_object(outcome.result->attestation_object);
    CHECK(object.has_value());
    if (object)
        CHECK(verify_attestation_statement(*object, request.client_data_hash) == StatementCheck::Valid);
    return outcome.result;
}

CoseKey key_of(Result const& created)
{
    return parse_attestation_object(created.attestation_object)->parsed.attested->public_key;
}

void get_info_members()
{
    SoftOptions options;
    options.built_in_uv = true;
    options.pin = "1234";
    options.max_credential_count_in_list = 5;
    options.min_pin_length = 6;
    options.force_pin_change = true;
    test::SoftAuthenticator key(options);
    DeviceInfo device;
    CtapHidDevice hid(key.connect(), device);
    CHECK(hid.init(in(1000)));
    Ctap2Device ctap(hid);
    auto const info = ctap.get_info(in(1000), nullptr);
    CHECK(info.has_value());
    if (!info)
        return;
    CHECK(info->has_version("FIDO_2_1"));
    CHECK(info->has_version("FIDO_2_0"));
    CHECK(info->has_version("U2F_V2"));
    CHECK(info->is_ctap21());
    CHECK(info->has_extension("credProtect") && info->has_extension("hmac-secret") && info->has_extension("credBlob")
        && info->has_extension("largeBlobKey") && info->has_extension("minPinLength"));
    CHECK(info->aaguid == options.aaguid);
    CHECK(info->option_true("rk") && info->option_true("up") && info->option_true("uv") && info->option_true("clientPin")
        && info->option_true("pinUvAuthToken") && info->option_true("makeCredUvNotRqd"));
    CHECK(!info->option("alwaysUv").has_value());
    CHECK_EQ(info->max_msg_size, std::size_t { 1200 });
    CHECK(info->pin_uv_auth_protocols == std::vector<std::uint8_t>({ 2, 1 }));
    CHECK(info->max_credential_count_in_list == std::size_t { 5 });
    CHECK(info->max_credential_id_length == std::size_t { 128 });
    CHECK(info->transports == std::vector<std::string>({ "usb" }));
    CHECK(info->algorithms == std::vector<std::int64_t>({ -7, -8 }));
    CHECK(info->force_pin_change);
    CHECK(info->min_pin_length == std::size_t { 6 });
    CHECK(info->firmware_version == std::uint64_t { 0x010203 });
    CHECK(info->max_cred_blob_length == std::size_t { 32 });
    CHECK(info->remaining_discoverable_credentials == std::size_t { 25 });
    CHECK(info->uv_protected());
    // Every member is kept, the unknown ones too.
    CHECK(info->raw.find(0x0e) != nullptr && info->raw.find(0x14) != nullptr);

    // A FIDO_2_0 key's answer written out by hand from §6.4's member
    // table: a maxMsgSize below the 1024 every key must take, a PIN that is
    // supported but not set.
    cbor::Value example = cbor::Value::map();
    example.set(0x01, cbor::Value::array({ cbor::Value::text("U2F_V2"), cbor::Value::text("FIDO_2_0") }));
    example.set(0x02, cbor::Value::array({ cbor::Value::text("credProtect"), cbor::Value::text("hmac-secret") }));
    example.set(0x03, cbor::Value::bytes(test::from_hex("2c6b1b6b7a8b4c2ab8fe9d5c4e1a8a0f")));
    cbor::Value example_options = cbor::Value::map();
    example_options.set("rk", cbor::Value::boolean(true));
    example_options.set("up", cbor::Value::boolean(true));
    example_options.set("clientPin", cbor::Value::boolean(false));
    example.set(0x04, example_options);
    example.set(0x05, cbor::Value::unsigned_integer(512)); // below 1024: 1024 is what may be sent
    example.set(0x06, cbor::Value::array({ cbor::Value::unsigned_integer(1) }));
    auto const parsed = parse_authenticator_info(*cbor::decode(cbor::encode(example)));
    CHECK(parsed && parsed->max_msg_size == 1024 && !parsed->is_ctap21() && parsed->option("clientPin") == false && !parsed->uv_protected());
    // Without versions or a 16-byte AAGUID it is not an answer.
    cbor::Value broken = example;
    broken.set(0x03, cbor::Value::bytes(std::vector<std::uint8_t>(15)));
    CHECK(!parse_authenticator_info(broken));
}

void make_and_get()
{
    auto transport = std::make_shared<FakeTransport>();
    auto key = transport->plug();
    auto const created = registered(transport, create_request());
    if (!created)
        return;
    CHECK(created->created.has_value());
    CHECK(created->created && created->created->public_key_spki.has_value());
    CHECK_EQ(created->created->public_key_algorithm, cose::alg_es256);
    CHECK(created->created->transports == std::vector<std::string>({ "usb" }));
    auto const object = parse_attestation_object(created->attestation_object);
    CHECK_EQ(object->fmt, std::string("packed"));
    CHECK(object->att_stmt.find("x5c") != nullptr);
    CHECK(object->parsed.user_present() && !object->parsed.user_verified());
    CHECK(object->parsed.attested->aaguid == key->options().aaguid);
    CHECK(created->credential_id == object->parsed.attested->credential_id);
    CHECK_EQ(key->credentials().size(), std::size_t { 1 });
    CHECK(!key->credentials().empty() && !key->credentials()[0].discoverable);

    // Assert with it: the signature verifies under the new key.
    auto const request = get_request({ created->credential_id });
    auto const outcome = run_ceremony(transport, request, std::make_shared<ScriptedPrompt>());
    CHECK(outcome.result.has_value());
    if (outcome.result) {
        CHECK(outcome.result->credential_id == created->credential_id);
        CHECK(verify_assertion_signature(key_of(*created), cose::alg_es256, outcome.result->authenticator_data, request.client_data_hash,
            outcome.result->signature));
        // Control: one bit of the signature.
        auto flipped = outcome.result->signature;
        flipped[10] ^= 0x04;
        CHECK(!verify_assertion_signature(key_of(*created), cose::alg_es256, outcome.result->authenticator_data, request.client_data_hash, flipped));
        auto const data = parse_authenticator_data(outcome.result->authenticator_data);
        // The silent probe of the allow list was an assertion too.
        CHECK(data && data->sign_count == 2 && data->user_present());
        CHECK(!outcome.result->user_handle);
    }

    // "none": the statement goes, self attestation stays.
    auto none_request = create_request();
    none_request.attestation = AttestationConveyance::None;
    auto const none = registered(transport, none_request);
    if (none)
        CHECK_EQ(parse_attestation_object(none->attestation_object)->fmt, std::string("none"));

    // EdDSA when the page puts it first.
    auto ed_request = create_request();
    ed_request.algorithms = { cose::alg_eddsa, cose::alg_es256 };
    auto const ed = registered(transport, ed_request);
    if (ed) {
        CHECK_EQ(ed->created->public_key_algorithm, cose::alg_eddsa);
        auto const ed_get = get_request({ ed->credential_id });
        auto const ed_outcome = run_ceremony(transport, ed_get, std::make_shared<ScriptedPrompt>());
        CHECK(ed_outcome.result
            && verify_assertion_signature(key_of(*ed), cose::alg_eddsa, ed_outcome.result->authenticator_data, ed_get.client_data_hash,
                ed_outcome.result->signature));
    }

    // Self attestation (a key configured without a certificate).
    SoftOptions self_options;
    self_options.self_attestation = true;
    auto self_transport = std::make_shared<FakeTransport>();
    self_transport->plug(self_options);
    auto const self = registered(self_transport, create_request());
    if (self)
        CHECK(parse_attestation_object(self->attestation_object)->att_stmt.find("x5c") == nullptr);

    // An algorithm no key has: the key is unsuitable, and touching it says so.
    auto rs_request = create_request();
    rs_request.algorithms = { cose::alg_rs256 };
    auto prompt = std::make_shared<ScriptedPrompt>();
    auto const unsuitable = run_ceremony(transport, rs_request, prompt);
    CHECK(unsuitable.error && unsuitable.error->reason == Reason::Unsuitable);
    CHECK(unsuitable.error && std::string(unsuitable.error->dom_exception_name()) == "NotAllowedError");
    CHECK(prompt->saw(PromptState::KeyUnsuitable));
    CHECK(prompt->closed);
}

void exclude_and_allow()
{
    auto transport = std::make_shared<FakeTransport>();
    transport->plug();
    auto const created = registered(transport, create_request());
    if (!created)
        return;
    // Excluded: InvalidStateError once the key is touched.
    auto again = create_request();
    again.credentials = { { test::from_hex("00112233"), {} }, { created->credential_id, {} } };
    auto prompt = std::make_shared<ScriptedPrompt>();
    auto const excluded = run_ceremony(transport, again, prompt);
    CHECK(excluded.error && excluded.error->kind == ErrorKind::InvalidState && excluded.error->reason == Reason::CredentialExcluded);
    CHECK(excluded.error && std::string(excluded.error->dom_exception_name()) == "InvalidStateError");
    CHECK(prompt->saw(PromptState::KeyAlreadyRegistered));
    // An exclude list of strangers' credentials does not stop it.
    auto strangers = create_request();
    strangers.credentials = { { test::from_hex("00112233"), {} } };
    CHECK(registered(transport, strangers).has_value());
    // Not registered: NotAllowedError after the touch.
    auto prompt2 = std::make_shared<ScriptedPrompt>();
    auto const unknown = run_ceremony(transport, get_request({ test::from_hex("deadbeef") }), prompt2);
    CHECK(unknown.error && unknown.error->reason == Reason::NotRegistered && unknown.error->kind == ErrorKind::NotAllowed);
    CHECK(prompt2->saw(PromptState::KeyNotRegistered));
    // The same credential under another RP is not found either.
    auto const other_rp = run_ceremony(transport, get_request({ created->credential_id }, "other.example"), std::make_shared<ScriptedPrompt>());
    CHECK(other_rp.error && other_rp.error->reason == Reason::NotRegistered);

    // Split lists: a key that takes two credentials per request and IDs of
    // at most 64 bytes, the real one last behind strangers and an ID too long.
    SoftOptions small;
    small.max_credential_count_in_list = 2;
    small.max_credential_id_length = 64;
    auto small_transport = std::make_shared<FakeTransport>();
    auto small_key = small_transport->plug(small);
    auto const small_created = registered(small_transport, create_request());
    if (!small_created)
        return;
    std::vector<std::vector<std::uint8_t>> allow;
    for (int i = 0; i < 5; ++i)
        allow.push_back(std::vector<std::uint8_t>(40, static_cast<std::uint8_t>(i)));
    allow.push_back(std::vector<std::uint8_t>(100, 9));
    allow.push_back(small_created->credential_id);
    int const before = small_key->stats().cbor_requests.load();
    auto const split = run_ceremony(small_transport, get_request(allow), std::make_shared<ScriptedPrompt>());
    CHECK(split.result && split.result->credential_id == small_created->credential_id);
    // getInfo, three probes of two or fewer (the 100-byte ID dropped), the assertion.
    CHECK_EQ(small_key->stats().cbor_requests.load() - before, 5);
}

void discoverable_accounts()
{
    SoftOptions options;
    options.pin = "123456";
    auto transport = std::make_shared<FakeTransport>();
    auto key = transport->plug(options);
    for (std::uint8_t user = 1; user <= 3; ++user) {
        auto request = create_request();
        request.resident_key = Requirement::Required;
        request.user.id = { user, user };
        request.user.name = "user" + std::to_string(user);
        auto prompt = std::make_shared<ScriptedPrompt>();
        prompt->pins.push_back(pin_answer("123456"));
        auto const created = registered(transport, request, prompt);
        if (created) {
            auto const object = parse_attestation_object(created->attestation_object);
            CHECK(object->parsed.user_verified());
        }
        CHECK_EQ(prompt->questions.size(), std::size_t { 1 });
    }
    CHECK_EQ(key->credentials().size(), std::size_t { 3 });
    CHECK(!key->credentials().empty() && key->credentials()[0].discoverable);
    // Three accounts; the user picks the second offered (newest first: user 2).
    auto request = get_request({});
    request.user_verification = Requirement::Required;
    auto prompt = std::make_shared<ScriptedPrompt>();
    prompt->pins.push_back(pin_answer("123456"));
    prompt->account_choice = 1;
    auto const outcome = run_ceremony(transport, request, prompt);
    CHECK(outcome.result.has_value());
    CHECK_EQ(prompt->accounts.size(), std::size_t { 3 });
    if (outcome.result && prompt->accounts.size() == 3) {
        CHECK(outcome.result->user_handle == std::vector<std::uint8_t>({ 2, 2 }));
        CHECK_EQ(prompt->accounts[0].name, std::string("user3"));
        auto const data = parse_authenticator_data(outcome.result->authenticator_data);
        CHECK(data && data->user_verified());
    }
    // The user turns the account list down.
    auto declined_prompt = std::make_shared<ScriptedPrompt>();
    declined_prompt->pins.push_back(pin_answer("123456"));
    declined_prompt->account_choice.reset();
    auto const declined = run_ceremony(transport, request, declined_prompt);
    CHECK(declined.error && declined.error->reason == Reason::Cancelled);
    // No discoverable credential for another RP.
    auto other = get_request({}, "nobody.example");
    auto other_prompt = std::make_shared<ScriptedPrompt>();
    auto const none = run_ceremony(transport, other, other_prompt);
    CHECK(none.error && none.error->reason == Reason::NotRegistered);
}

void pin_states(std::uint8_t protocol)
{
    SoftOptions options;
    options.pin = "1234";
    options.pin_protocols = { protocol };
    auto transport = std::make_shared<FakeTransport>();
    auto key = transport->plug(options);

    // Wrong, then right: "Retry" with seven tries left.
    auto request = create_request();
    request.user_verification = Requirement::Required;
    auto prompt = std::make_shared<ScriptedPrompt>();
    prompt->pins = { pin_answer("0000"), pin_answer("1234") };
    auto const created = registered(transport, request, prompt);
    CHECK_EQ(prompt->questions.size(), std::size_t { 2 });
    if (prompt->questions.size() == 2) {
        CHECK(prompt->questions[0].kind == PinQuestion::Kind::Enter);
        CHECK(prompt->questions[1].kind == PinQuestion::Kind::Retry);
        CHECK_EQ(prompt->questions[1].retries, 7);
    }
    CHECK_EQ(key->pin_retries(), 8);
    if (created)
        CHECK(parse_attestation_object(created->attestation_object)->parsed.user_verified());

    // Three wrong in a row: the key wants to be reinserted.
    auto blocked_prompt = std::make_shared<ScriptedPrompt>();
    blocked_prompt->pins = { pin_answer("0000"), pin_answer("0001"), pin_answer("0002"), pin_answer("1234") };
    auto const blocked = run_ceremony(transport, request, blocked_prompt);
    CHECK(blocked.error && blocked.error->reason == Reason::PinAuthBlocked);
    CHECK(blocked_prompt->saw(PromptState::PinAuthBlocked));
    CHECK_EQ(key->pin_retries(), 5);

    // The user dismisses the PIN dialog.
    SoftOptions fresh = options;
    auto transport2 = std::make_shared<FakeTransport>();
    transport2->plug(fresh);
    auto dismissed_prompt = std::make_shared<ScriptedPrompt>();
    auto const dismissed = run_ceremony(transport2, request, dismissed_prompt);
    CHECK(dismissed.error && dismissed.error->reason == Reason::Cancelled && dismissed.error->kind == ErrorKind::NotAllowed);

    // A PIN blocked for good.
    SoftOptions nearly = options;
    auto transport3 = std::make_shared<FakeTransport>();
    auto key3 = transport3->plug(nearly);
    for (int round = 0; round < 3; ++round) {
        // Two wrong answers per insertion, then the dialog closed.
        auto p = std::make_shared<ScriptedPrompt>();
        p->pins = { pin_answer("9999"), pin_answer("9998") };
        run_ceremony(transport3, request, p);
        key3->power_cycle(); // reinserted
    }
    CHECK_EQ(key3->pin_retries(), 2);
    auto last = std::make_shared<ScriptedPrompt>();
    last->pins = { pin_answer("9999"), pin_answer("9998") };
    auto const gone = run_ceremony(transport3, request, last);
    CHECK(gone.error && gone.error->reason == Reason::PinBlocked);
    CHECK(last->saw(PromptState::PinBlocked));
    CHECK_EQ(key3->pin_retries(), 0);
    auto after = std::make_shared<ScriptedPrompt>();
    auto const still = run_ceremony(transport3, request, after);
    CHECK(still.error && still.error->reason == Reason::PinBlocked);
    CHECK(after->questions.empty()); // not even asked

    // No PIN yet and the page requires UV: one is set, then used.
    SoftOptions bare;
    bare.pin_protocols = { protocol };
    auto transport4 = std::make_shared<FakeTransport>();
    auto key4 = transport4->plug(bare);
    auto set_prompt = std::make_shared<ScriptedPrompt>();
    set_prompt->pins = { pin_answer("12"), pin_answer("424242") };
    auto const with_new_pin = registered(transport4, request, set_prompt);
    // The PIN just chosen is the one used: it is not asked for again.
    CHECK_EQ(set_prompt->questions.size(), std::size_t { 2 });
    CHECK(set_prompt->questions.size() == 2 && set_prompt->questions[0].kind == PinQuestion::Kind::Set
        && set_prompt->questions[1].kind == PinQuestion::Kind::Set);
    CHECK(key4->pin_hash() == pin::pin_hash("424242"));
    if (with_new_pin)
        CHECK(parse_attestation_object(with_new_pin->attestation_object)->parsed.user_verified());

    // forcePINChange: the current PIN and a new one, which is then used.
    SoftOptions forced = options;
    forced.force_pin_change = true;
    auto transport5 = std::make_shared<FakeTransport>();
    auto key5 = transport5->plug(forced);
    auto change_prompt = std::make_shared<ScriptedPrompt>();
    change_prompt->pins = { pin_answer("1234", "56785678") };
    CHECK(registered(transport5, request, change_prompt).has_value());
    CHECK_EQ(change_prompt->questions.size(), std::size_t { 1 });
    CHECK(!change_prompt->questions.empty() && change_prompt->questions[0].kind == PinQuestion::Kind::Change);
    CHECK(key5->pin_hash() == pin::pin_hash("56785678"));
}

void controls_on_the_key(std::uint8_t protocol_number)
{
    // Driven by hand: the key itself must refuse a wrong pinUvAuthParam,
    // a token used for another RP, and a token without the permission.
    SoftOptions options;
    options.pin = "1234";
    options.pin_protocols = { protocol_number };
    test::SoftAuthenticator key(options);
    DeviceInfo device;
    CtapHidDevice hid(key.connect(), device);
    CHECK(hid.init(in(1000)));
    Ctap2Device ctap(hid);
    auto const protocol = *pin::protocol_from_number(protocol_number);

    ClientPinParams agreement;
    agreement.protocol = protocol_number;
    agreement.subcommand = ctap2::pin_get_key_agreement;
    auto const encoded_agreement = encode_client_pin(agreement);
    auto const reply = ctap.call(ctap2::cmd_client_pin, &encoded_agreement, in(1000), nullptr);
    CHECK(reply.ok());
    auto const response = parse_client_pin_response(reply.body);
    auto const shared = pin::encapsulate(protocol, *response->key_agreement, platform::fill_random);
    CHECK(shared.has_value());
    if (!shared)
        return;
    ClientPinParams get_token;
    get_token.protocol = protocol_number;
    get_token.subcommand = ctap2::pin_get_token_using_pin;
    get_token.key_agreement = shared->platform_key;
    get_token.pin_hash_enc = pin::encrypt(protocol, shared->shared_secret, pin::pin_hash("1234"), platform::fill_random);
    get_token.permissions = ctap2::permission_get_assertion;
    get_token.rp_id = "example.org";
    auto const encoded_token = encode_client_pin(get_token);
    auto const token_reply = ctap.call(ctap2::cmd_client_pin, &encoded_token, in(1000), nullptr);
    CHECK(token_reply.ok());
    auto const token = pin::decrypt(protocol, shared->shared_secret, *parse_client_pin_response(token_reply.body)->pin_uv_auth_token);
    CHECK(token && token->size() == 32);
    if (!token)
        return;
    std::array<std::uint8_t, 32> hash {};
    hash.fill(7);
    auto const assertion_with = [&](std::vector<std::uint8_t> param, std::string rp_id) {
        GetAssertionParams params;
        params.rp_id = std::move(rp_id);
        params.client_data_hash = hash;
        params.allow_list = { { std::vector<std::uint8_t>(16, 1), {} } };
        params.pin_uv_auth_param = std::move(param);
        params.pin_uv_auth_protocol = protocol_number;
        params.up = false;
        auto const encoded = encode_get_assertion(params);
        return ctap.call(ctap2::cmd_get_assertion, &encoded, in(1000), nullptr).status;
    };
    auto good = pin::authenticate(protocol, *token, hash);
    // A good param reaches the credential lookup (none there).
    CHECK_EQ(int(assertion_with(good, "example.org")), int(Status::NoCredentials));
    auto bad = good;
    bad[0] ^= 1;
    CHECK_EQ(int(assertion_with(bad, "example.org")), int(Status::PinAuthInvalid));
    CHECK_EQ(int(assertion_with(good, "other.example")), int(Status::PinAuthInvalid));
    // makeCredential with a token that has only the ga permission.
    MakeCredentialParams make;
    make.client_data_hash = hash;
    make.rp.id = "example.org";
    make.user.id = { 1 };
    make.algorithms = { cose::alg_es256 };
    make.pin_uv_auth_param = good;
    make.pin_uv_auth_protocol = protocol_number;
    auto const encoded_make = encode_make_credential(make);
    CHECK_EQ(int(ctap.call(ctap2::cmd_make_credential, &encoded_make, in(1000), nullptr).status), int(Status::PinAuthInvalid));
    // A protocol the key does not speak.
    make.pin_uv_auth_protocol = protocol_number == 1 ? 2 : 1;
    auto const encoded_other = encode_make_credential(make);
    CHECK_EQ(int(ctap.call(ctap2::cmd_make_credential, &encoded_other, in(1000), nullptr).status), int(Status::InvalidParameter));
    // Non-canonical CBOR is the key's INVALID_CBOR.
    std::vector<std::uint8_t> sloppy { ctap2::cmd_get_assertion, 0xa2, 0x02, 0x40, 0x01, 0x60 };
    auto const sloppy_reply = hid.transact(ctaphid::cmd_cbor, sloppy, in(1000));
    CHECK(sloppy_reply.ok() && !sloppy_reply.payload.empty() && sloppy_reply.payload[0] == std::uint8_t(Status::InvalidCbor));
}

void built_in_uv()
{
    SoftOptions options;
    options.built_in_uv = true;
    options.pin = "1234";
    auto transport = std::make_shared<FakeTransport>();
    auto key = transport->plug(options);
    auto request = create_request();
    request.user_verification = Requirement::Required;
    request.resident_key = Requirement::Required;
    auto prompt = std::make_shared<ScriptedPrompt>();
    auto const created = registered(transport, request, prompt);
    CHECK(prompt->questions.empty()); // no PIN asked
    CHECK(prompt->saw(PromptState::VerifyFingerprint));
    if (created)
        CHECK(parse_attestation_object(created->attestation_object)->parsed.user_verified());

    // The finger is not recognized: three tries, then the PIN.
    key->set_uv_succeeds(false);
    auto fallback_prompt = std::make_shared<ScriptedPrompt>();
    fallback_prompt->pins = { pin_answer("1234") };
    auto const by_pin = registered(transport, request, fallback_prompt);
    CHECK(fallback_prompt->saw(PromptState::UvRetry));
    CHECK_EQ(fallback_prompt->questions.size(), std::size_t { 1 });
    CHECK_EQ(key->uv_retries(), 5);
    if (by_pin)
        CHECK(parse_attestation_object(by_pin->attestation_object)->parsed.user_verified());
}

void ctap20_key()
{
    // A CTAP 2.0 key: no authenticatorSelection, getPinToken, the "uv" option.
    SoftOptions options;
    options.protocol = SoftOptions::Protocol::Ctap20;
    options.pin = "1234";
    options.pin_protocols = { 1 };
    auto transport = std::make_shared<FakeTransport>();
    transport->plug(options);
    auto request = create_request();
    request.user_verification = Requirement::Required;
    auto prompt = std::make_shared<ScriptedPrompt>();
    prompt->pins = { pin_answer("1234") };
    auto const created = registered(transport, request, prompt);
    if (created) {
        CHECK(parse_attestation_object(created->attestation_object)->parsed.user_verified());
        auto get = get_request({ created->credential_id });
        get.user_verification = Requirement::Required;
        auto get_prompt = std::make_shared<ScriptedPrompt>();
        get_prompt->pins = { pin_answer("1234") };
        auto const outcome = run_ceremony(transport, get, get_prompt);
        CHECK(outcome.result
            && verify_assertion_signature(key_of(*created), cose::alg_es256, outcome.result->authenticator_data, get.client_data_hash,
                outcome.result->signature));
    }

    SoftOptions uv_options;
    uv_options.protocol = SoftOptions::Protocol::Ctap20;
    uv_options.built_in_uv = true;
    uv_options.client_pin = false;
    auto uv_transport = std::make_shared<FakeTransport>();
    uv_transport->plug(uv_options);
    auto const by_uv = registered(uv_transport, request);
    if (by_uv)
        CHECK(parse_attestation_object(by_uv->attestation_object)->parsed.user_verified());
}

}

int main()
{
    get_info_members();
    make_and_get();
    exclude_and_allow();
    discoverable_accounts();
    pin_states(1);
    pin_states(2);
    controls_on_the_key(1);
    controls_on_the_key(2);
    built_in_uv();
    ctap20_key();
    return sashfold::test::report("test_auth_ctap2");
}
