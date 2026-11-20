// authenticatorCredentialManagement (CTAP 2.1 §6.8) and the key settings'
// session (auth/KeyManagement.h): the requests byte for byte as the
// specification builds them (it gives no test vectors for this command, so
// the bytes are written out here from its rules, and the MAC computed with
// HMAC directly rather than through the PIN protocol code), the answers
// parsed and refused by shape, every subcommand against the software key
// (its side written from the same section, separately), the permission and
// RP ID rules of the token, the pre-release's prototype, and the session
// from "insert your key" to a deleted passkey, with each error a key can
// give: a wrong PIN with tries left, a blocked PIN, three wrong in a row,
// a key pulled out, a key without the command, a token the key stopped
// taking. Then hostile answers: lies about totals, missing members, noise.
#include "AuthTestSupport.h"
#include "SoftAuthenticator.h"
#include "Test.h"

#include "auth/ClientPin.h"
#include "auth/CredentialManagement.h"
#include "auth/KeyManagement.h"
#include "crypto/Ec.h"
#include "crypto/Hmac.h"
#include "crypto/Sha2.h"
#include "platform/Random.h"

#include <thread>

using namespace sashfold;
using namespace sashfold::auth;
using sashfold::test::from_hex;
using sashfold::test::to_hex;
using sashfold::test::TestRandom;

namespace {

using Clock = std::chrono::steady_clock;
using Phase = KeyManagementState::Phase;
using Outcome = KeyManagementState::Outcome;
using Failure = KeyManagementState::Failure;

std::array<std::uint8_t, 32> sha256_of(std::string_view text)
{
    return crypto::Sha256::hash(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(text.data()), text.size()));
}

std::uint8_t status(ctap2::Status s) { return static_cast<std::uint8_t>(s); }

// A discoverable credential placed on a software key.
void add_passkey(test::SoftAuthenticator& key, std::string const& rp_id, std::string const& name, std::string const& display, std::uint8_t id)
{
    test::SoftAuthenticator::Credential credential;
    credential.id = { 0xc0, id, 0x11, 0x22 };
    credential.rp_id = rp_id;
    credential.rp_id_hash = sha256_of(rp_id);
    credential.user.id = { id, 0x55 };
    credential.user.name = name;
    credential.user.has_name = true;
    credential.user.display_name = display;
    credential.user.has_display_name = !display.empty();
    credential.discoverable = true;
    do {
        platform::fill_random(credential.private_key);
    } while (!crypto::ec_private_scalar_valid(crypto::CurveId::P256, credential.private_key));
    key.add_credential(credential);
}

// One key on a CTAPHID channel, spoken to directly.
struct Direct {
    explicit Direct(test::SoftAuthenticator& key)
        : hid(key.connect(), DeviceInfo {})
    {
        CHECK(hid.init(Clock::now() + std::chrono::seconds(5)));
        Ctap2Device device(hid);
        info = device.get_info(Clock::now() + std::chrono::seconds(5), nullptr);
        CHECK(info.has_value());
    }
    client_pin::Call call()
    {
        return [this](std::uint8_t command, cbor::Value const* params) {
            Ctap2Device device(hid);
            return device.call(command, params, Clock::now() + std::chrono::seconds(10), nullptr);
        };
    }
    std::optional<client_pin::Token> token(std::string const& pin, std::uint8_t permissions, std::optional<std::string> rp_id = std::nullopt)
    {
        auto const protocol = client_pin::preferred_protocol(*info);
        if (!protocol)
            return std::nullopt;
        return client_pin::token_using_pin(call(), *info, *protocol, pin, permissions, rp_id).token;
    }
    CtapHidDevice hid;
    std::optional<AuthenticatorInfo> info;
};

// ----- The requests, byte for byte -----

void requests_by_the_rules()
{
    client_pin::Token token;
    token.protocol = pin::Protocol::Two;
    token.token.assign(32, 0x5a);
    // getCredsMetadata: pinUvAuthParam = authenticate(token, 0x01) (§6.8.2);
    // protocol two's authenticate is the whole HMAC-SHA-256.
    {
        std::uint8_t const message[] = { 0x01 };
        auto const mac = crypto::Hmac<crypto::Sha256>::mac(token.token, message);
        auto const encoded = cbor::encode(cred_mgmt::encode_request(cred_mgmt::get_creds_metadata, std::nullopt, &token));
        CHECK_EQ(to_hex(encoded), "a301010302045820" + to_hex(mac));
    }
    // Protocol one: the first 16 bytes.
    {
        client_pin::Token one = token;
        one.protocol = pin::Protocol::One;
        std::uint8_t const message[] = { 0x02 };
        auto const mac = crypto::Hmac<crypto::Sha256>::mac(one.token, message);
        auto const encoded = cbor::encode(cred_mgmt::encode_request(cred_mgmt::enumerate_rps_begin, std::nullopt, &one));
        CHECK_EQ(to_hex(encoded), "a301020301" "0450" + to_hex(std::span<std::uint8_t const>(mac.data(), 16)));
    }
    // The "next" subcommands carry neither protocol nor parameter (§6.8.3, §6.8.4).
    CHECK_EQ(to_hex(cbor::encode(cred_mgmt::encode_request(cred_mgmt::enumerate_rps_get_next_rp, std::nullopt, &token))), "a10103");
    CHECK_EQ(to_hex(cbor::encode(cred_mgmt::encode_request(cred_mgmt::enumerate_credentials_get_next_credential, std::nullopt, &token))), "a10105");
    CHECK(!cred_mgmt::needs_token(cred_mgmt::enumerate_rps_get_next_rp));
    CHECK(cred_mgmt::needs_token(cred_mgmt::delete_credential));
    // enumerateCredentialsBegin: the message is 0x04 || {1: rpIDHash}.
    {
        cred_mgmt::SubCommandParams params;
        params.rp_id_hash = sha256_of("example.org");
        auto const sub = cred_mgmt::encode_sub_command_params(params);
        std::string const sub_hex = "a1015820" + to_hex(sha256_of("example.org"));
        CHECK_EQ(to_hex(cbor::encode(sub)), sub_hex);
        auto const message = from_hex("04" + sub_hex);
        CHECK_EQ(to_hex(cred_mgmt::auth_message(cred_mgmt::enumerate_credentials_begin, sub)), to_hex(message));
        auto const mac = crypto::Hmac<crypto::Sha256>::mac(token.token, message);
        auto const encoded = cbor::encode(cred_mgmt::encode_request(cred_mgmt::enumerate_credentials_begin, sub, &token));
        CHECK_EQ(to_hex(encoded), "a4010402" + sub_hex + "0302045820" + to_hex(mac));
    }
    // deleteCredential: {2: {"id": ..., "type": "public-key"}}, the
    // descriptor's keys in canonical order ("id" is shorter than "type").
    {
        cred_mgmt::SubCommandParams params;
        params.credential_id = CredentialDescriptor { { 0x01, 0x02 }, {} };
        std::string const descriptor = "a2" "626964" "420102" "6474797065" "6a7075626c69632d6b6579";
        auto const sub = cred_mgmt::encode_sub_command_params(params);
        CHECK_EQ(to_hex(cbor::encode(sub)), "a102" + descriptor);
        auto const mac = crypto::Hmac<crypto::Sha256>::mac(token.token, from_hex("06a102" + descriptor));
        CHECK_EQ(to_hex(cbor::encode(cred_mgmt::encode_request(cred_mgmt::delete_credential, sub, &token))),
            "a4010602a102" + descriptor + "0302045820" + to_hex(mac));
    }
    // updateUserInformation: the descriptor under 2, the user under 3.
    {
        cred_mgmt::SubCommandParams params;
        params.credential_id = CredentialDescriptor { { 0x01 }, {} };
        UserEntity user;
        user.id = { 0x07 };
        user.name = "a";
        user.has_name = true;
        params.user = user;
        CHECK_EQ(to_hex(cbor::encode(cred_mgmt::encode_sub_command_params(params))),
            "a202a2626964410164747970656a7075626c69632d6b657903a2626964410764" "6e616d656161");
    }
    // Which command a key offers.
    AuthenticatorInfo info;
    CHECK(cred_mgmt::support_of(info) == cred_mgmt::Support::None);
    info.options["credentialMgmtPreview"] = true;
    CHECK(cred_mgmt::support_of(info) == cred_mgmt::Support::Preview);
    CHECK_EQ(int(cred_mgmt::command_for(cred_mgmt::Support::Preview)), 0x41);
    info.options["credMgmt"] = true;
    CHECK(cred_mgmt::support_of(info) == cred_mgmt::Support::Full);
    CHECK_EQ(int(cred_mgmt::command_for(cred_mgmt::Support::Full)), 0x0a);
    info.options["credMgmt"] = false;
    CHECK(cred_mgmt::support_of(info) == cred_mgmt::Support::Preview);
}

void answers_by_shape()
{
    using cbor::Value;
    // getCredsMetadata's two counts.
    Value metadata = Value::map();
    metadata.set(0x01, Value::unsigned_integer(3));
    metadata.set(0x02, Value::unsigned_integer(22));
    auto const parsed = cred_mgmt::parse_response(metadata);
    CHECK(parsed && parsed->existing_resident_credentials_count == 3u && parsed->max_possible_remaining_resident_credentials_count == 22u);
    // An RP: its entity, its hash of 32 bytes, the total.
    Value rp = Value::map();
    Value entity = Value::map();
    entity.set("id", Value::text("example.org"));
    rp.set(0x03, entity);
    rp.set(0x04, Value::bytes(sha256_of("example.org")));
    rp.set(0x05, Value::unsigned_integer(2));
    auto const party = cred_mgmt::parse_response(rp);
    CHECK(party && party->rp && party->rp->id == "example.org" && party->total_rps == 2u);
    // Refused by shape: a count that is not a number, a hash that is not 32
    // bytes, an RP without its id, a user without its id, a descriptor
    // without its type.
    Value bad = metadata;
    bad.set(0x01, Value::text("3"));
    CHECK(!cred_mgmt::parse_response(bad));
    bad = rp;
    bad.set(0x04, Value::bytes(std::vector<std::uint8_t>(31)));
    CHECK(!cred_mgmt::parse_response(bad));
    bad = rp;
    bad.set(0x03, Value::map());
    CHECK(!cred_mgmt::parse_response(bad));
    Value user = Value::map();
    user.set("name", Value::text("alice"));
    bad = Value::map();
    bad.set(0x06, user);
    CHECK(!cred_mgmt::parse_response(bad));
    Value descriptor = Value::map();
    descriptor.set("id", Value::bytes(std::vector<std::uint8_t> { 1 }));
    bad = Value::map();
    bad.set(0x07, descriptor);
    CHECK(!cred_mgmt::parse_response(bad));
    bad = Value::map();
    bad.set(0x08, Value::bytes(std::vector<std::uint8_t> { 1 }));
    CHECK(!cred_mgmt::parse_response(bad));
    CHECK(!cred_mgmt::parse_response(Value::array()));
    // A public key of an algorithm the engine does not read is no fault.
    Value odd = Value::map();
    Value key = Value::map();
    key.set(1, Value::unsigned_integer(3));
    key.set(3, Value::integer(-257));
    odd.set(0x08, key);
    auto const unknown_key = cred_mgmt::parse_response(odd);
    CHECK(unknown_key && !unknown_key->public_key);
}

// ----- Every subcommand against the software key -----

void subcommands()
{
    test::SoftOptions options;
    options.pin = "1234";
    options.discoverable_capacity = 10;
    test::SoftAuthenticator key(options);
    add_passkey(key, "example.org", "bob", "Bob", 2);
    add_passkey(key, "example.com", "carol", "", 3);
    add_passkey(key, "example.org", "alice", "Alice", 1);
    Direct direct(key);
    CHECK(cred_mgmt::support_of(*direct.info) == cred_mgmt::Support::Full);

    auto const token = direct.token("1234", ctap2::permission_credential_management);
    CHECK(token.has_value());
    if (!token)
        return;
    CredentialManager manager(direct.call(), cred_mgmt::Support::Full, *token);
    CredentialsMetadata metadata;
    CHECK(manager.metadata(metadata).ok());
    CHECK_EQ(metadata.existing, 3u);
    CHECK_EQ(metadata.remaining, 7u);
    std::vector<Passkey> passkeys;
    CHECK(manager.passkeys(passkeys).ok());
    CHECK_EQ(passkeys.size(), 3u);
    if (passkeys.size() == 3) {
        // By site, then by user name.
        CHECK_EQ(passkeys[0].rp_id, std::string("example.com"));
        CHECK_EQ(passkeys[0].user.name, std::string("carol"));
        CHECK(!passkeys[0].user.has_display_name);
        CHECK_EQ(passkeys[1].user.name, std::string("alice"));
        CHECK_EQ(passkeys[1].user.display_name, std::string("Alice"));
        CHECK_EQ(passkeys[2].user.name, std::string("bob"));
        CHECK(passkeys[1].rp_id_hash == sha256_of("example.org"));
        CHECK(passkeys[1].credential.id == (std::vector<std::uint8_t> { 0xc0, 1, 0x11, 0x22 }));
        CHECK(passkeys[1].cred_protect == 1u);
    }

    // updateUserInformation: the names replaced; another user's id refused.
    UserEntity renamed;
    renamed.id = { 2, 0x55 };
    renamed.name = "robert";
    renamed.has_name = true;
    CHECK(manager.update_user(CredentialDescriptor { { 0xc0, 2, 0x11, 0x22 }, {} }, renamed).ok());
    UserEntity stranger = renamed;
    stranger.id = { 9 };
    auto const refused = manager.update_user(CredentialDescriptor { { 0xc0, 2, 0x11, 0x22 }, {} }, stranger);
    CHECK_EQ(int(refused.status), int(status(ctap2::Status::InvalidParameter)));
    for (auto const& c : key.credentials()) {
        if (c.id == std::vector<std::uint8_t> { 0xc0, 2, 0x11, 0x22 }) {
            CHECK_EQ(c.user.name, std::string("robert"));
            CHECK(!c.user.has_display_name); // absent in the update: removed
        }
    }

    // deleteCredential, then the same again: NO_CREDENTIALS.
    CHECK(manager.remove(CredentialDescriptor { { 0xc0, 3, 0x11, 0x22 }, {} }).ok());
    CHECK_EQ(key.credentials().size(), 2u);
    auto const again = manager.remove(CredentialDescriptor { { 0xc0, 3, 0x11, 0x22 }, {} });
    CHECK_EQ(int(again.status), int(status(ctap2::Status::NoCredentials)));
    CHECK(manager.passkeys(passkeys).ok());
    CHECK_EQ(passkeys.size(), 2u);
    CHECK(manager.metadata(metadata).ok());
    CHECK_EQ(metadata.existing, 2u);
    CHECK_EQ(metadata.remaining, 8u);
    // Emptied: enumerateRPsBegin answers NO_CREDENTIALS, an empty list here.
    for (auto const& p : std::vector<Passkey>(passkeys))
        CHECK(manager.remove(p.credential).ok());
    CHECK(manager.passkeys(passkeys).ok());
    CHECK(passkeys.empty());
    CHECK(manager.metadata(metadata).ok());
    CHECK_EQ(metadata.existing, 0u);
}

void token_rules()
{
    test::SoftOptions options;
    options.pin = "1234";
    test::SoftAuthenticator key(options);
    add_passkey(key, "example.org", "alice", "Alice", 1);
    add_passkey(key, "example.com", "carol", "Carol", 3);
    Direct direct(key);
    auto const raw = [&](cbor::Value const& request) {
        return direct.call()(ctap2::cmd_credential_management, &request).status;
    };
    // No pinUvAuthParam: PUAT_REQUIRED (§6.8.2 step 1).
    {
        cbor::Value request = cbor::Value::map();
        request.set(0x01, cbor::Value::unsigned_integer(cred_mgmt::get_creds_metadata));
        CHECK_EQ(int(raw(request)), int(status(ctap2::Status::PuatRequired)));
    }
    // A "next" with no enumeration begun: NOT_ALLOWED.
    {
        cbor::Value request = cbor::Value::map();
        request.set(0x01, cbor::Value::unsigned_integer(cred_mgmt::enumerate_rps_get_next_rp));
        CHECK_EQ(int(raw(request)), int(status(ctap2::Status::NotAllowed)));
    }
    // A token without the cm permission (and without a permissions RP ID,
    // which would be refused for its own reason) is refused.
    {
        auto const token = direct.token("1234", ctap2::permission_large_blob_write);
        CHECK(token.has_value());
        CredentialManager manager(direct.call(), cred_mgmt::Support::Full, *token);
        CredentialsMetadata metadata;
        CHECK_EQ(int(manager.metadata(metadata).status), int(status(ctap2::Status::PinAuthInvalid)));
    }
    // A wrong MAC is refused.
    {
        auto token = direct.token("1234", ctap2::permission_credential_management);
        CHECK(token.has_value());
        token->token[0] ^= 1;
        CredentialManager manager(direct.call(), cred_mgmt::Support::Full, *token);
        CredentialsMetadata metadata;
        CHECK_EQ(int(manager.metadata(metadata).status), int(status(ctap2::Status::PinAuthInvalid)));
    }
    // A token with a permissions RP ID: no metadata, no list of every RP,
    // but that RP's credentials, and only its own deleted.
    {
        auto const token = direct.token("1234", ctap2::permission_credential_management, std::string("example.org"));
        CHECK(token.has_value());
        CredentialManager manager(direct.call(), cred_mgmt::Support::Full, *token);
        CredentialsMetadata metadata;
        CHECK_EQ(int(manager.metadata(metadata).status), int(status(ctap2::Status::PinAuthInvalid)));
        std::vector<Passkey> passkeys;
        CHECK_EQ(int(manager.passkeys(passkeys).status), int(status(ctap2::Status::PinAuthInvalid)));
        cred_mgmt::SubCommandParams params;
        params.rp_id_hash = sha256_of("example.org");
        auto const sub = cred_mgmt::encode_sub_command_params(params);
        auto const request = cred_mgmt::encode_request(cred_mgmt::enumerate_credentials_begin, sub, &*token);
        auto const reply = direct.call()(ctap2::cmd_credential_management, &request);
        CHECK(reply.ok());
        auto const parsed = cred_mgmt::parse_response(reply.body);
        CHECK(parsed && parsed->user && parsed->user->name == "alice" && parsed->total_credentials == 1u);
        params.rp_id_hash = sha256_of("example.com");
        auto const other = cred_mgmt::encode_request(cred_mgmt::enumerate_credentials_begin, cred_mgmt::encode_sub_command_params(params), &*token);
        CHECK_EQ(int(direct.call()(ctap2::cmd_credential_management, &other).status), int(status(ctap2::Status::PinAuthInvalid)));
        CHECK_EQ(int(manager.remove(CredentialDescriptor { { 0xc0, 3, 0x11, 0x22 }, {} }).status), int(status(ctap2::Status::PinAuthInvalid)));
        CHECK(manager.remove(CredentialDescriptor { { 0xc0, 1, 0x11, 0x22 }, {} }).ok());
        CHECK_EQ(key.credentials().size(), 1u);
    }
    // The prototype is not this key's.
    {
        cbor::Value request = cbor::Value::map();
        request.set(0x01, cbor::Value::unsigned_integer(cred_mgmt::get_creds_metadata));
        CHECK_EQ(int(direct.call()(ctap2::cmd_credential_management_preview, &request).status), int(status(ctap2::Status::InvalidCommand)));
    }
}

void preview_key()
{
    // A pre-release key: the prototype under 0x41, a getPinToken token.
    test::SoftOptions options;
    options.protocol = test::SoftOptions::Protocol::Ctap20;
    options.credential_management = test::SoftOptions::CredentialManagement::Preview;
    options.pin = "123456";
    test::SoftAuthenticator key(options);
    add_passkey(key, "example.org", "alice", "Alice", 1);
    Direct direct(key);
    CHECK(direct.info->has_version("FIDO_2_1_PRE"));
    CHECK(cred_mgmt::support_of(*direct.info) == cred_mgmt::Support::Preview);
    auto const token = direct.token("123456", ctap2::permission_credential_management);
    CHECK(token.has_value());
    if (!token)
        return;
    CredentialManager manager(direct.call(), cred_mgmt::Support::Preview, *token);
    std::vector<Passkey> passkeys;
    CHECK(manager.passkeys(passkeys).ok());
    CHECK_EQ(passkeys.size(), 1u);
    CHECK(manager.remove(CredentialDescriptor { { 0xc0, 1, 0x11, 0x22 }, {} }).ok());
    CHECK(key.credentials().empty());
    // The 2.1 command is not this key's.
    CredentialManager full(direct.call(), cred_mgmt::Support::Full, *token);
    CredentialsMetadata metadata;
    CHECK_EQ(int(full.metadata(metadata).status), int(status(ctap2::Status::InvalidCommand)));
}

// ----- The session -----

struct Session {
    explicit Session(std::shared_ptr<Transport> transport)
        : management(std::move(transport), [] { })
    {
        management.start();
    }
    // Waits for the state the test expects (a generous bound: the threads
    // may be slow, never wrong).
    KeyManagementState until(std::function<bool(KeyManagementState const&)> const& wanted)
    {
        auto const limit = Clock::now() + std::chrono::seconds(60);
        while (Clock::now() < limit) {
            KeyManagementState const now = management.state();
            if (wanted(now))
                return now;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        KeyManagementState const last = management.state();
        std::cerr << "the session never reached the state waited for (phase " << int(last.phase) << ", outcome " << int(last.outcome) << ")\n";
        CHECK(false);
        return last;
    }
    // Waits for the command count to move past `done`.
    KeyManagementState after(std::uint64_t done)
    {
        return until([&](KeyManagementState const& s) { return s.commands_done > done || s.phase == Phase::Ended; });
    }
    KeyManagement management;
};

bool ready(KeyManagementState const& s) { return s.phase == Phase::Ready; }

void session_pins()
{
    auto transport = std::make_shared<test::FakeTransport>();
    Session session(transport);
    auto state = session.until([](auto const& s) { return s.phase == Phase::InsertKey; });
    CHECK(state.keys_present == 0);
    // A key without a PIN comes, and waits for its touch.
    test::SoftOptions options;
    options.min_pin_length = 6;
    auto key = transport->plug(options);
    key->set_touch(test::TouchPlan::never());
    state = session.until([](auto const& s) { return s.phase == Phase::TouchKey; });
    CHECK_EQ(state.keys_present, 1u);
    // A command before a key is chosen is not taken.
    session.management.set_pin("123456");
    key->set_touch(test::TouchPlan::immediately());
    state = session.until(ready);
    CHECK(state.key && state.key->ctap2 && state.key->pin_supported && !state.key->pin_set);
    CHECK_EQ(state.key->min_pin_length, 6u);
    CHECK(state.key->credential_management == cred_mgmt::Support::Full);
    CHECK_EQ(state.commands_done, 0u);
    CHECK(!key->pin_hash());

    // Too short for this key: refused by it (PIN_POLICY_VIOLATION).
    session.management.set_pin("12345");
    state = session.after(0);
    CHECK(state.outcome == Outcome::PinPolicy);
    CHECK(!key->pin_hash());
    session.management.set_pin("123456");
    state = session.after(1);
    CHECK(state.outcome == Outcome::PinSet);
    CHECK(state.key->pin_set);
    CHECK(key->pin_hash() == pin::pin_hash("123456"));
    // Change: wrong current PIN, the tries left; then right.
    session.management.change_pin("000000", "654321");
    state = session.after(2);
    CHECK(state.outcome == Outcome::PinWrong);
    CHECK_EQ(state.retries, 7);
    session.management.change_pin("123456", "654321");
    state = session.after(3);
    CHECK(state.outcome == Outcome::PinChanged);
    CHECK(key->pin_hash() == pin::pin_hash("654321"));
    CHECK_EQ(key->pin_retries(), 8);
    // Set again on a key that has one: not offered.
    session.management.set_pin("777777");
    state = session.after(4);
    CHECK(state.outcome == Outcome::NotSupported);
    CHECK(key->pin_hash() == pin::pin_hash("654321"));
    // Three wrong in a row: the key wants to be reinserted, and the session ends.
    for (std::uint64_t i = 0; i < 2; ++i) {
        session.management.unlock(std::string("999999"));
        state = session.after(5 + i);
        CHECK(state.outcome == Outcome::PinWrong);
    }
    session.management.unlock(std::string("999999"));
    state = session.until([](auto const& s) { return s.phase == Phase::Ended; });
    CHECK(state.failure == Failure::PinAuthBlocked);
}

void session_passkeys()
{
    auto transport = std::make_shared<test::FakeTransport>();
    test::SoftOptions options;
    options.pin = "1234";
    auto key = transport->plug(options);
    add_passkey(*key, "example.org", "alice", "Alice", 1);
    add_passkey(*key, "example.org", "bob", "Bob", 2);
    add_passkey(*key, "example.net", "dave", "", 4);
    Session session(transport);
    auto state = session.until(ready);
    CHECK(!state.unlocked);
    session.management.unlock(std::string("1234"));
    state = session.after(0);
    CHECK(state.outcome == Outcome::Unlocked);
    CHECK(state.unlocked);
    CHECK(state.metadata && state.metadata->existing == 3u && state.metadata->remaining == 22u);
    CHECK_EQ(state.passkeys.size(), 3u);
    if (state.passkeys.size() == 3)
        CHECK_EQ(state.passkeys[0].user.name, std::string("dave"));
    // Delete one: the list read again.
    session.management.remove(state.passkeys[1].credential);
    state = session.after(1);
    CHECK(state.outcome == Outcome::Deleted);
    CHECK_EQ(state.passkeys.size(), 2u);
    CHECK_EQ(key->credentials().size(), 2u);
    CHECK(state.metadata && state.metadata->existing == 2u);
    // Gone already (another program took it): said, and the list read again.
    key->remove_credentials(state.passkeys[0].credential.id);
    session.management.remove(state.passkeys[0].credential);
    state = session.after(2);
    CHECK(state.outcome == Outcome::NotFound);
    CHECK_EQ(state.passkeys.size(), 1u);
    // The key stops taking the token (here: it lost power, §6.5.2.1):
    // the list stays, locked, and the PIN is asked again.
    key->power_cycle();
    session.management.remove(state.passkeys[0].credential);
    state = session.after(3);
    CHECK(state.outcome == Outcome::TokenExpired);
    CHECK(!state.unlocked);
    CHECK_EQ(state.passkeys.size(), 1u);
    CHECK_EQ(key->credentials().size(), 1u);
    session.management.unlock(std::string("1234"));
    state = session.after(4);
    CHECK(state.outcome == Outcome::Unlocked);
    // Pulled out while it waits: the end.
    transport->unplug(key);
    state = session.until([](auto const& s) { return s.phase == Phase::Ended; });
    CHECK(state.failure == Failure::KeyGone);
    session.management.refresh(); // after the end: ignored
    CHECK(session.until([&](auto const&) { return session.management.finished(); }).phase == Phase::Ended);
}

void session_errors()
{
    // A PIN with one try left: wrong once, blocked for good.
    {
        auto transport = std::make_shared<test::FakeTransport>();
        test::SoftOptions options;
        options.pin = "1234";
        options.pin_retries = 1;
        transport->plug(options);
        Session session(transport);
        session.until(ready);
        session.management.unlock(std::string("4321"));
        auto const state = session.until([](auto const& s) { return s.phase == Phase::Ended; });
        CHECK(state.failure == Failure::PinBlocked);
    }
    // A key without the command, and a U2F-only key: nothing to list.
    {
        auto transport = std::make_shared<test::FakeTransport>();
        test::SoftOptions options;
        options.pin = "1234";
        options.credential_management = test::SoftOptions::CredentialManagement::None;
        transport->plug(options);
        Session session(transport);
        auto state = session.until(ready);
        CHECK(state.key && state.key->credential_management == cred_mgmt::Support::None);
        session.management.unlock(std::string("1234"));
        state = session.after(0);
        CHECK(state.outcome == Outcome::NotSupported);
    }
    {
        auto transport = std::make_shared<test::FakeTransport>();
        test::SoftOptions options;
        options.protocol = test::SoftOptions::Protocol::U2fOnly;
        transport->plug(options);
        Session session(transport);
        auto state = session.until(ready);
        CHECK(state.key && !state.key->ctap2 && !state.key->pin_supported);
        session.management.set_pin("1234");
        state = session.after(0);
        CHECK(state.outcome == Outcome::NotSupported);
    }
    // A key that insists on a new PIN gives no token for the old one.
    {
        auto transport = std::make_shared<test::FakeTransport>();
        test::SoftOptions options;
        options.pin = "1234";
        options.force_pin_change = true;
        transport->plug(options);
        Session session(transport);
        auto state = session.until(ready);
        CHECK(state.key && state.key->force_pin_change);
        session.management.unlock(std::string("1234"));
        state = session.after(0);
        CHECK(state.outcome == Outcome::PinNeedsChange);
        session.management.change_pin("1234", "5678");
        state = session.after(1);
        CHECK(state.outcome == Outcome::PinChanged);
        CHECK(state.key && !state.key->force_pin_change);
        session.management.unlock(std::string("5678"));
        state = session.after(2);
        CHECK(state.outcome == Outcome::Unlocked);
    }
    // Built-in user verification: a token without a PIN.
    {
        auto transport = std::make_shared<test::FakeTransport>();
        test::SoftOptions options;
        options.built_in_uv = true;
        auto key = transport->plug(options);
        add_passkey(*key, "example.org", "alice", "Alice", 1);
        Session session(transport);
        auto state = session.until(ready);
        CHECK(state.key && state.key->built_in_uv);
        key->set_uv_succeeds(false);
        session.management.unlock(std::nullopt);
        state = session.after(0);
        CHECK(state.outcome == Outcome::UvWrong);
        CHECK_EQ(state.retries, 7);
        key->set_uv_succeeds(true);
        session.management.unlock(std::nullopt);
        state = session.after(1);
        CHECK(state.outcome == Outcome::Unlocked);
        CHECK_EQ(state.passkeys.size(), 1u);
    }
    // Two keys: the one touched is chosen, the other let go.
    {
        auto transport = std::make_shared<test::FakeTransport>();
        auto first = transport->plug({});
        first->set_touch(test::TouchPlan::never());
        test::SoftOptions second_options;
        second_options.pin = "2222";
        auto second = transport->plug(second_options);
        second->set_touch(test::TouchPlan::never());
        Session session(transport);
        session.until([](auto const& s) { return s.phase == Phase::TouchKey && s.keys_present == 2; });
        second->set_touch(test::TouchPlan::immediately());
        auto const state = session.until(ready);
        CHECK(state.key && state.key->pin_set);
        // The other one hears CTAPHID_CANCEL.
        auto const limit = Clock::now() + std::chrono::seconds(30);
        while (first->stats().cancels_honored.load() == 0 && Clock::now() < limit)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(first->stats().cancels_honored.load() > 0);
    }
    // Pulled out while it waits for its touch: back to "insert".
    {
        auto transport = std::make_shared<test::FakeTransport>();
        auto key = transport->plug({});
        key->set_touch(test::TouchPlan::never());
        Session session(transport);
        session.until([](auto const& s) { return s.phase == Phase::TouchKey; });
        transport->unplug(key);
        auto const state = session.until([](auto const& s) { return s.phase == Phase::InsertKey; });
        CHECK_EQ(state.keys_present, 0u);
    }
    // Cancel while a key waits for its touch: the threads end promptly, and
    // destroying it waits for nothing more.
    {
        auto transport = std::make_shared<test::FakeTransport>();
        auto key = transport->plug({});
        key->set_touch(test::TouchPlan::never());
        auto session = std::make_unique<Session>(transport);
        session->until([](auto const& s) { return s.phase == Phase::TouchKey; });
        session->management.cancel();
        auto const state = session->until([&](auto const&) { return session->management.finished(); });
        CHECK(state.phase == Phase::Ended && state.failure == Failure::Cancelled);
        session.reset();
    }
}

// ----- Hostile answers -----

// A device behind a Call that answers from a script of replies.
struct Liar {
    std::function<Ctap2Reply(std::uint8_t sub)> answer;
    int calls = 0;
    client_pin::Call call()
    {
        return [this](std::uint8_t command, cbor::Value const* params) {
            ++calls;
            std::uint8_t sub = 0;
            if (params != nullptr) {
                if (cbor::Value const* const s = params->find(0x01); s != nullptr && s->is_unsigned())
                    sub = static_cast<std::uint8_t>(s->raw_number());
            }
            (void)command;
            return answer(sub);
        };
    }
};

Ctap2Reply ok_reply(cbor::Value body)
{
    Ctap2Reply reply;
    reply.body = std::move(body);
    return reply;
}

void hostile()
{
    using cbor::Value;
    client_pin::Token token { pin::Protocol::Two, std::vector<std::uint8_t>(32, 1) };
    Value entity = Value::map();
    entity.set("id", Value::text("example.org"));
    auto const party = [&](std::uint64_t total) {
        Value v = Value::map();
        v.set(0x03, entity);
        v.set(0x04, Value::bytes(sha256_of("example.org")));
        if (total > 0)
            v.set(0x05, Value::unsigned_integer(total));
        return v;
    };
    Value user = Value::map();
    user.set("id", Value::bytes(std::vector<std::uint8_t> { 1 }));
    user.set("name", Value::text("alice"));
    auto const credential = [&](std::uint64_t total) {
        Value v = Value::map();
        v.set(0x06, user);
        v.set(0x07, descriptor_to_cbor({ { 1, 2 }, {} }));
        if (total > 0)
            v.set(0x09, Value::unsigned_integer(total));
        return v;
    };

    // A key that claims 2^63 parties: the list ends at the bound, malformed.
    {
        Liar liar;
        liar.answer = [&](std::uint8_t sub) { return ok_reply(sub == cred_mgmt::enumerate_rps_begin ? party(1ull << 63) : party(0)); };
        CredentialManager manager(liar.call(), cred_mgmt::Support::Full, token);
        std::vector<Passkey> passkeys;
        CHECK(manager.passkeys(passkeys).failure == HidFailure::Malformed);
        CHECK(liar.calls <= 2);
    }
    // One party with more credentials than any key holds: stops at the bound.
    {
        Liar liar;
        liar.answer = [&](std::uint8_t sub) {
            if (sub == cred_mgmt::enumerate_rps_begin)
                return ok_reply(party(1));
            return ok_reply(credential(sub == cred_mgmt::enumerate_credentials_begin ? 1ull << 40 : 0));
        };
        CredentialManager manager(liar.call(), cred_mgmt::Support::Full, token);
        std::vector<Passkey> passkeys;
        CHECK(manager.passkeys(passkeys).failure == HidFailure::Malformed);
        CHECK(liar.calls <= static_cast<int>(CredentialManager::max_credentials) + 3);
    }
    // A total of zero, a next without its members, a metadata without its counts.
    {
        Liar liar;
        liar.answer = [&](std::uint8_t sub) { return ok_reply(sub == cred_mgmt::enumerate_rps_begin ? party(2) : Value::map()); };
        CredentialManager manager(liar.call(), cred_mgmt::Support::Full, token);
        std::vector<Passkey> passkeys;
        CHECK(manager.passkeys(passkeys).failure == HidFailure::Malformed);
        CredentialsMetadata metadata;
        CHECK(manager.metadata(metadata).failure == HidFailure::Malformed);
    }
    // Noise: mutated real answers, every subcommand's turn, many rounds.
    // Nothing may crash or loop; every outcome is an answer.
    TestRandom random;
    std::vector<std::vector<std::uint8_t>> seeds { cbor::encode(party(3)), cbor::encode(party(0)), cbor::encode(credential(2)),
        cbor::encode(credential(0)) };
    {
        Value metadata = Value::map();
        metadata.set(0x01, Value::unsigned_integer(2));
        metadata.set(0x02, Value::unsigned_integer(5));
        seeds.push_back(cbor::encode(metadata));
    }
    int malformed = 0;
    for (int round = 0; round < 3000; ++round) {
        Liar liar;
        liar.answer = [&](std::uint8_t) {
            std::vector<std::uint8_t> bytes = seeds[random.below(seeds.size())];
            for (std::size_t i = 0, n = random.below(4); i < n && !bytes.empty(); ++i)
                bytes[random.below(bytes.size())] = static_cast<std::uint8_t>(random.next());
            if (random.below(5) == 0)
                bytes.resize(random.below(bytes.size() + 1));
            Ctap2Reply reply;
            if (random.below(10) == 0) {
                reply.status = static_cast<std::uint8_t>(random.next());
                return reply;
            }
            auto decoded = cbor::decode(bytes, cbor::Mode::Ctap2);
            if (!decoded || !decoded->is_map()) {
                reply.malformed = true;
                return reply;
            }
            reply.body = std::move(*decoded);
            return reply;
        };
        CredentialManager manager(liar.call(), cred_mgmt::Support::Full, token);
        std::vector<Passkey> passkeys;
        auto const answer = manager.passkeys(passkeys);
        malformed += answer.failure == HidFailure::Malformed ? 1 : 0;
        CHECK(liar.calls <= static_cast<int>(CredentialManager::max_relying_parties + CredentialManager::max_credentials) + 4);
        CredentialsMetadata metadata;
        manager.metadata(metadata);
        for (auto const& seed : seeds) {
            if (auto const value = cbor::decode(seed))
                cred_mgmt::parse_response(*value);
        }
    }
    CHECK(malformed > 0);
}

}

int main()
{
    requests_by_the_rules();
    answers_by_shape();
    subcommands();
    token_rules();
    preview_key();
    session_pins();
    session_passkeys();
    session_errors();
    hostile();
    return sashfold::test::report("test_auth_credmgmt");
}
