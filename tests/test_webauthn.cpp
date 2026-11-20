// Web Authentication's script interface (bindings/WebAuthn.cpp) and the
// client's own steps (auth/ClientData.h): the client data serialized as
// WebAuthn Level 3 §5.8.1.1 gives it, against the specification's own test
// vectors (§16); the RP ID's check against the caller's origin; and whole
// ceremonies from a page's navigator.credentials through a host hook to the
// software authenticator, whose attestation and assertion are then checked
// as a relying party checks them (§7.1, §7.2).
#include "AuthCeremony.h"
#include "AuthTestSupport.h"
#include "JsTest.h"
#include "Test.h"
#include "WebAuthnVectors.h"

#include "auth/Attestation.h"
#include "auth/ClientData.h"
#include "bindings/Realm.h"
#include "crypto/Sha2.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "media/ClearKey.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace sashfold;
using sashfold::test::from_hex;
using sashfold::test::to_hex;

namespace {

std::string string_of(std::vector<std::uint8_t> const& bytes) { return std::string(bytes.begin(), bytes.end()); }

// The value of a string member of a flat JSON object the vectors use.
std::string json_member(std::string const& json, std::string const& name)
{
    std::string const key = "\"" + name + "\":\"";
    std::size_t const at = json.find(key);
    if (at == std::string::npos)
        return {};
    std::size_t const from = at + key.size();
    return json.substr(from, json.find('"', from) - from);
}

void client_data_matches_the_vectors()
{
    int compared = 0;
    for (WebAuthnVector const& vector : webauthn_vectors) {
        for (auto const& [hex, type] : { std::pair { vector.reg_clientDataJSON, "webauthn.create" }, std::pair { vector.auth_clientDataJSON, "webauthn.get" } }) {
            std::string const given = string_of(from_hex(hex));
            auth::CollectedClientData data;
            data.type = type;
            data.challenge = json_member(given, "challenge");
            data.origin = json_member(given, "origin");
            data.cross_origin = given.find("\"crossOrigin\":true") != std::string::npos;
            // The challenge survives base64url both ways.
            std::optional<std::vector<std::uint8_t>> const challenge = media::base64url_decode(data.challenge);
            CHECK(challenge.has_value());
            if (challenge)
                CHECK_EQ(auth::base64url(*challenge), data.challenge);
            // Ours is the vector's to the byte, up to the members the
            // vector adds after crossOrigin (its "extraData", which §5.8.1.1
            // step 7 places there and this client does not add).
            std::string const ours = auth::serialize_client_data(data);
            std::string const prefix = ours.substr(0, ours.size() - 1);
            CHECK(given.starts_with(prefix));
            CHECK(given.size() > prefix.size() && (given[prefix.size()] == '}' || given[prefix.size()] == ','));
            if (given.size() == ours.size())
                CHECK_EQ(ours, given);
            ++compared;
        }
    }
    CHECK_EQ(compared, static_cast<int>(2 * std::size(webauthn_vectors)));
    CHECK(compared >= 16);
    std::cout << "client data: " << compared << " vectors' clientDataJSON reproduced\n";
}

void client_data_escapes()
{
    // CCDToString: the quotation mark, the reverse solidus and the controls
    // escaped, lower-case hex; everything else as it is, UTF-8 included.
    CHECK_EQ(auth::ccd_to_string("a\"b\\c\x01\x1f\x7f\xc3\xa9"), std::string("\"a\\\"b\\\\c\\u0001\\u001f\x7f\xc3\xa9\""));
    auth::CollectedClientData data { "webauthn.get", "AAEC", "https://sub.example.org:8443", true, std::string("https://example.org") };
    CHECK_EQ(auth::serialize_client_data(data),
        std::string("{\"type\":\"webauthn.get\",\"challenge\":\"AAEC\",\"origin\":\"https://sub.example.org:8443\",\"crossOrigin\":true,"
                    "\"topOrigin\":\"https://example.org\"}"));
}

void rp_ids()
{
    // Equal, or a registrable domain suffix of the effective domain.
    CHECK(auth::rp_id_allowed("login.example.com", "https", "login.example.com"));
    CHECK(auth::rp_id_allowed("example.com", "https", "login.example.com"));
    CHECK(auth::rp_id_allowed("example.co.uk", "https", "a.example.co.uk"));
    CHECK(auth::rp_id_allowed("web-platform.test", "https", "web-platform.test"));
    CHECK(auth::rp_id_allowed("localhost", "http", "localhost"));
    // A public suffix, another site, a sibling, a longer name, a port, a
    // space, an empty string, the word null: none.
    CHECK(!auth::rp_id_allowed("com", "https", "login.example.com"));
    CHECK(!auth::rp_id_allowed("co.uk", "https", "a.example.co.uk"));
    CHECK(!auth::rp_id_allowed("example.org", "https", "login.example.com"));
    CHECK(!auth::rp_id_allowed("other.example.com", "https", "login.example.com"));
    CHECK(!auth::rp_id_allowed("a.login.example.com", "https", "login.example.com"));
    CHECK(!auth::rp_id_allowed("web-platform.test:8443", "https", "web-platform.test"));
    CHECK(!auth::rp_id_allowed("invalid domain.com", "https", "web-platform.test"));
    CHECK(!auth::rp_id_allowed("", "https", "web-platform.test"));
    CHECK(!auth::rp_id_allowed("null", "https", "web-platform.test"));
    CHECK(!auth::rp_id_allowed("ample.com", "https", "example.com"));
    // Not a secure origin, or an IP address: nothing is allowed.
    CHECK(!auth::rp_id_allowed("example.com", "http", "example.com"));
    CHECK(!auth::rp_id_allowed("127.0.0.1", "https", "127.0.0.1"));
    // The timeout's range (§15.1).
    CHECK_EQ(auth::ceremony_timeout(std::nullopt).count(), 300000);
    CHECK_EQ(auth::ceremony_timeout(1).count(), 300000);
    CHECK_EQ(auth::ceremony_timeout(450000).count(), 450000);
    CHECK_EQ(auth::ceremony_timeout(4000000000u).count(), 600000);
}

// A page whose host runs ceremonies on software keys, the way the shell
// runs them on the system's: on threads of their own, the outcome carried
// back to the page's thread by the realm.
struct KeyPage {
    std::unique_ptr<dom::Document> document = std::make_unique<dom::Document>();
    std::unique_ptr<bindings::Realm> realm;
    double clock = 1000;
    std::string console;
    std::shared_ptr<test::FakeTransport> transport = std::make_shared<test::FakeTransport>();
    std::shared_ptr<test::ScriptedPrompt> prompt = std::make_shared<test::ScriptedPrompt>();
    std::mutex mutex;
    std::map<std::uint64_t, std::unique_ptr<auth::Ceremony>> ceremonies;
    std::vector<std::pair<std::uint64_t, bool>> cancels;
    std::vector<bindings::HostHooks::SecurityKeyAsk> asks;
    std::vector<auth::Request> requests;
    std::uint64_t next = 1;
    bool focused = true;

    explicit KeyPage(std::string const& url = "https://login.example.com/page.html")
    {
        bindings::HostHooks hooks;
        hooks.now = [this] { return clock; };
        hooks.console = [this](std::string_view level, std::string_view message) { console += std::string(level) + ":" + std::string(message) + "|"; };
        hooks.document_focused = [this] { return focused; };
        hooks.security_key_ceremony = [this](bindings::HostHooks::SecurityKeyAsk const& ask, auth::Request const& request,
                                          std::function<void(auth::Outcome)> done) -> std::uint64_t {
            std::lock_guard lock(mutex);
            std::uint64_t const id = next++;
            asks.push_back(ask);
            requests.push_back(request);
            auto ceremony = std::make_unique<auth::Ceremony>(request, transport, prompt, std::move(done));
            ceremony->start();
            ceremonies[id] = std::move(ceremony);
            return id;
        };
        hooks.cancel_security_key_ceremony = [this](std::uint64_t id, bool by_page) {
            std::lock_guard lock(mutex);
            cancels.emplace_back(id, by_page);
            if (auto const found = ceremonies.find(id); found != ceremonies.end())
                found->second->cancel(by_page);
        };
        realm = std::make_unique<bindings::Realm>(*document, *net::parse_url(url), std::move(hooks));
        html::parse_document_bytes_into(*document, std::string("<!doctype html><title>keys</title>"), realm.get());
        realm->document_parsed();
        while (realm->run_pending()) {
        }
    }

    ~KeyPage()
    {
        realm.reset();
        std::lock_guard lock(mutex);
        ceremonies.clear(); // each cancelled and waited for
    }

    test::JsRun eval(std::string_view source)
    {
        js::Outcome const outcome = realm->run(source, "<test>");
        test::JsRun run;
        run.ok = outcome.ok;
        run.value = outcome.value;
        if (!outcome.ok)
            run.thrown = realm->interpreter().describe(outcome.value);
        return run;
    }
    std::string string(std::string_view source)
    {
        test::JsRun const run = eval(source);
        if (!run.ok || !run.value.is_string())
            return run.ok ? "<not a string>" : "<threw " + run.thrown + ">";
        return run.value.as_string()->to_utf8();
    }

    // Runs the page's loop until `globalThis.outcome` is set (real time:
    // the keys answer from their own threads), at most five seconds.
    std::string await_outcome()
    {
        auto const until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < until) {
            while (realm->run_pending()) {
            }
            std::string const now = string("typeof outcome === 'string' ? outcome : ''");
            if (!now.empty()) {
                static_cast<void>(eval("delete globalThis.outcome"));
                return now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return "<no outcome>";
    }

    void advance(double ms)
    {
        clock += ms;
        while (realm->run_pending()) {
        }
    }
};

// The page's side: the options a test passes, the outcome as a string.
constexpr std::string_view create_script = R"JS(
    var made;
    function bytes(text) { return new TextEncoder().encode(text); }
    function hex(buffer) { return Array.from(new Uint8Array(buffer), b => b.toString(16).padStart(2, "0")).join(""); }
    function createOptions(extra) {
        let options = { publicKey: {
            rp: { name: "Example" },
            user: { id: bytes("user-1"), name: "alice@example.com", displayName: "Alice" },
            challenge: bytes("a challenge of sixteen+"),
            pubKeyCredParams: [{ type: "public-key", alg: -7 }],
            attestation: "direct",
        } };
        Object.assign(options.publicKey, extra || {});
        return options;
    }
    function report(promise) {
        promise.then(value => { made = value; outcome = "ok"; },
                     error => { outcome = typeof error === "string" ? "reason: " + error : error.name + ": " + error.message; });
    }
)JS";

void ceremonies_end_to_end()
{
    KeyPage page;
    auto key = page.transport->plug(test::SoftOptions {});
    CHECK(page.eval(create_script).ok);
    CHECK_EQ(page.string("typeof navigator.credentials.create"), std::string("function"));
    CHECK_EQ(page.string("String(window.isSecureContext && typeof PublicKeyCredential)"), std::string("function"));

    // create(): the key touched at once.
    CHECK(page.eval("report(navigator.credentials.create(createOptions()))").ok);
    CHECK_EQ(page.await_outcome(), std::string("ok"));
    CHECK_EQ(page.string("made.type + ' ' + made.authenticatorAttachment + ' ' + (made instanceof PublicKeyCredential)"),
        std::string("public-key cross-platform true"));
    CHECK_EQ(page.string("String(made.rawId === made.rawId && made.response.clientDataJSON === made.response.clientDataJSON)"), std::string("true"));
    std::string const client_data_create = string_of(from_hex(page.string("hex(made.response.clientDataJSON)")));
    CHECK_EQ(client_data_create,
        std::string("{\"type\":\"webauthn.create\",\"challenge\":\"YSBjaGFsbGVuZ2Ugb2Ygc2l4dGVlbis\",\"origin\":\"https://login.example.com\",\"crossOrigin\":false}"));
    CHECK_EQ(page.asks.size(), std::size_t { 1 });
    if (!page.asks.empty()) {
        CHECK_EQ(page.asks[0].site, std::string("login.example.com"));
        CHECK_EQ(page.asks[0].origin, std::string("https://login.example.com"));
    }
    if (!page.requests.empty()) {
        CHECK_EQ(page.requests[0].rp_id, std::string("login.example.com"));
        CHECK_EQ(page.requests[0].timeout.count(), 300000);
    }
    // As a relying party checks it (§7.1): the statement over authData and
    // the client data's hash, the RP ID's hash, the flags.
    std::vector<std::uint8_t> const attestation = from_hex(page.string("hex(made.response.attestationObject)"));
    std::optional<auth::AttestationObject> const object = auth::parse_attestation_object(attestation);
    CHECK(object.has_value());
    std::array<std::uint8_t, 32> const create_hash = test::client_data_hash_of(client_data_create);
    std::vector<std::uint8_t> credential_id;
    std::optional<auth::CoseKey> credential_key;
    std::int64_t credential_alg = 0;
    if (object) {
        CHECK_EQ(object->fmt, std::string("packed"));
        CHECK(auth::verify_attestation_statement(*object, create_hash) == auth::StatementCheck::Valid);
        std::string const rp = "login.example.com";
        CHECK(object->parsed.rp_id_hash == crypto::Sha256::hash(std::span(reinterpret_cast<std::uint8_t const*>(rp.data()), rp.size())));
        CHECK(object->parsed.user_present());
        if (object->parsed.attested) {
            credential_id = object->parsed.attested->credential_id;
            credential_key = object->parsed.attested->public_key;
            credential_alg = object->parsed.attested->public_key.alg.value_or(0);
        }
    }
    CHECK_EQ(page.string("made.id"), auth::base64url(credential_id));
    CHECK_EQ(page.string("hex(made.response.getAuthenticatorData())"), to_hex(object ? object->auth_data : std::vector<std::uint8_t> {}));
    CHECK_EQ(page.string("String(made.response.getPublicKeyAlgorithm())"), std::string("-7"));
    CHECK_EQ(page.string("made.response.getTransports().join()"), std::string("usb"));
    CHECK_EQ(page.string("String(made.response.getPublicKey().byteLength)"), std::string("91"));
    CHECK_EQ(page.string("JSON.stringify(made.getClientExtensionResults())"), std::string("{}"));
    CHECK_EQ(page.string("Object.keys(made.toJSON().response).join()"),
        std::string("clientDataJSON,authenticatorData,transports,publicKey,publicKeyAlgorithm,attestationObject"));

    // The same credential excluded: InvalidStateError once the key is touched.
    CHECK(page.eval("report(navigator.credentials.create(createOptions({ excludeCredentials: [{ type: 'public-key', id: made.rawId }] })))").ok);
    CHECK_EQ(page.string("String(made !== undefined)"), std::string("true"));
    std::string const excluded = page.await_outcome();
    CHECK(excluded.starts_with("InvalidStateError"));

    // get(): an assertion the credential's key verifies (§7.2 step 20).
    CHECK(page.eval("var id = made.rawId; report(navigator.credentials.get({ publicKey: { challenge: bytes('another challenge'), "
                    "allowCredentials: [{ type: 'public-key', id }], userVerification: 'discouraged' } }))")
              .ok);
    CHECK_EQ(page.await_outcome(), std::string("ok"));
    std::string const client_data_get = string_of(from_hex(page.string("hex(made.response.clientDataJSON)")));
    CHECK_EQ(json_member(client_data_get, "type"), std::string("webauthn.get"));
    std::vector<std::uint8_t> const auth_data = from_hex(page.string("hex(made.response.authenticatorData)"));
    std::vector<std::uint8_t> const signature = from_hex(page.string("hex(made.response.signature)"));
    if (credential_key)
        CHECK(auth::verify_assertion_signature(*credential_key, credential_alg, auth_data, test::client_data_hash_of(client_data_get), signature));
    CHECK_EQ(page.string("String(made.response.userHandle)"), std::string("null"));
    CHECK_EQ(page.string("String(made instanceof PublicKeyCredential && made.response instanceof AuthenticatorAssertionResponse)"), std::string("true"));
    std::cout << "ceremonies: create, exclude and get checked as a relying party checks them\n";
}

void refusals()
{
    KeyPage page;
    page.transport->plug(test::SoftOptions {});
    CHECK(page.eval(create_script).ok);
    // An RP ID that is a public suffix, or another site's: SecurityError,
    // before any key is asked.
    CHECK(page.eval("report(navigator.credentials.create(createOptions({ rp: { id: 'com', name: 'x' } })))").ok);
    CHECK(page.await_outcome().starts_with("SecurityError"));
    CHECK(page.eval("report(navigator.credentials.get({ publicKey: { challenge: bytes('c'), rpId: 'example.org' } }))").ok);
    CHECK(page.await_outcome().starts_with("SecurityError"));
    CHECK(page.eval("report(navigator.credentials.create(createOptions({ rp: { id: 'example.com', name: 'x' }, challenge: undefined })))").ok);
    CHECK(page.await_outcome().starts_with("TypeError"));
    CHECK(page.eval("report(navigator.credentials.create(createOptions({ user: { id: new Uint8Array(65), name: 'a', displayName: 'b' } })))").ok);
    CHECK(page.await_outcome().starts_with("TypeError"));
    CHECK(page.eval("report(navigator.credentials.create(createOptions({ pubKeyCredParams: [{ type: 'other', alg: -7 }] })))").ok);
    CHECK(page.await_outcome().starts_with("NotSupportedError"));
    CHECK(page.eval("report(navigator.credentials.create({}))").ok);
    CHECK(page.await_outcome().starts_with("NotSupportedError"));
    CHECK(page.eval("report(navigator.credentials.create(createOptions({ extensions: { prf: { evalByCredential: { AAAA: { first: bytes('x') } } } } })))").ok);
    CHECK(page.await_outcome().starts_with("NotSupportedError"));
    CHECK(page.eval("report(navigator.credentials.get({ mediation: 'conditional', publicKey: { challenge: bytes('c') } }))").ok);
    CHECK(page.await_outcome().starts_with("TypeError"));
    CHECK(page.asks.empty());
    // Unfocused: refused.
    page.focused = false;
    CHECK(page.eval("report(navigator.credentials.get({ publicKey: { challenge: bytes('c') } }))").ok);
    CHECK(page.await_outcome().starts_with("NotAllowedError"));
    CHECK(page.asks.empty());
    // The rp.id given as the effective domain's registrable suffix is fine.
    page.focused = true;
    CHECK(page.eval("report(navigator.credentials.create(createOptions({ rp: { id: 'example.com', name: 'x' } })))").ok);
    CHECK_EQ(page.await_outcome(), std::string("ok"));
    if (!page.requests.empty())
        CHECK_EQ(page.requests.back().rp_id, std::string("example.com"));
}

void abort_and_timeout()
{
    KeyPage page;
    auto key = page.transport->plug(test::SoftOptions {});
    key->set_touch(test::TouchPlan::never());
    CHECK(page.eval(create_script).ok);
    // Aborted before: the signal's reason, no ceremony.
    CHECK(page.eval("report(navigator.credentials.create(Object.assign(createOptions(), { signal: AbortSignal.abort('gone') })))").ok);
    CHECK_EQ(page.await_outcome(), std::string("reason: gone"));
    CHECK(page.asks.empty());
    // Aborted while the key waits for its touch: the ceremony is cancelled
    // as the page's, the promise rejected with the reason.
    CHECK(page.eval("var controller = new AbortController(); "
                    "report(navigator.credentials.create(Object.assign(createOptions(), { signal: controller.signal })))")
              .ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(page.eval("controller.abort()").ok);
    CHECK(page.await_outcome().starts_with("AbortError"));
    CHECK_EQ(page.cancels.size(), std::size_t { 1 });
    if (!page.cancels.empty())
        CHECK(page.cancels[0].second);
    // One at a time per document.
    CHECK(page.eval("report(navigator.credentials.create(createOptions({ timeout: 1 })))").ok);
    CHECK(page.eval("var second; navigator.credentials.get({ publicKey: { challenge: bytes('c') } }).catch(e => second = e.name)").ok);
    while (page.realm->run_pending()) {
    }
    CHECK_EQ(page.string("String(second)"), std::string("NotAllowedError"));
    // The lifetime timer, on the page's clock: 1 ms is brought up to the
    // 300 s floor; nothing before it, NotAllowedError at it, and the
    // ceremony cancelled as not the page's.
    page.advance(299000);
    CHECK_EQ(page.string("typeof outcome"), std::string("undefined"));
    page.advance(1000);
    CHECK(page.await_outcome().starts_with("NotAllowedError"));
    CHECK_EQ(page.cancels.size(), std::size_t { 2 });
    if (page.cancels.size() == 2)
        CHECK(!page.cancels[1].second);
    // A platform authenticator asked for where there is none: no key is
    // asked, and the page hears only when the time is up (§14.5.1).
    std::size_t const asked = page.asks.size();
    CHECK(page.eval("report(navigator.credentials.create(createOptions({ authenticatorSelection: { authenticatorAttachment: 'platform' } })))").ok);
    page.advance(10);
    CHECK_EQ(page.string("typeof outcome"), std::string("undefined"));
    CHECK_EQ(page.asks.size(), asked);
    page.advance(300000);
    CHECK(page.await_outcome().starts_with("NotAllowedError"));
}

void the_document_going_cancels()
{
    auto page = std::make_unique<KeyPage>();
    auto key = page->transport->plug(test::SoftOptions {});
    key->set_touch(test::TouchPlan::never());
    CHECK(page->eval(create_script).ok);
    CHECK(page->eval("report(navigator.credentials.create(createOptions()))").ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    page->realm.reset();
    CHECK_EQ(page->cancels.size(), std::size_t { 1 });
    if (!page->cancels.empty())
        CHECK(!page->cancels[0].second);
}

void insecure_context_has_none()
{
    KeyPage page("http://example.test/page.html");
    CHECK_EQ(page.string("typeof navigator.credentials + ' ' + typeof PublicKeyCredential + ' ' + typeof CredentialsContainer"),
        std::string("undefined undefined undefined"));
}

void statics_and_json()
{
    KeyPage page;
    CHECK(page.eval(create_script).ok);
    CHECK(page.eval("PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable().then(v => outcome = String(v))").ok);
    CHECK_EQ(page.await_outcome(), std::string("false"));
    CHECK(page.eval("PublicKeyCredential.getClientCapabilities().then(v => outcome = Object.keys(v).join())").ok);
    std::string const capabilities = page.await_outcome();
    CHECK(capabilities.starts_with("conditionalCreate,conditionalGet,extension:appid"));
    CHECK(page.eval("var parsed = PublicKeyCredential.parseCreationOptionsFromJSON({ rp: { name: 'x' }, user: { id: 'AAEC', name: 'a', displayName: 'b' }, "
                    "challenge: 'AQID', pubKeyCredParams: [{ type: 'public-key', alg: -7 }], excludeCredentials: [{ type: 'public-key', id: 'BAU' }], "
                    "extensions: { prf: { eval: { first: 'AA' } } } })")
              .ok);
    CHECK_EQ(page.string("hex(parsed.user.id) + ' ' + hex(parsed.challenge) + ' ' + hex(parsed.excludeCredentials[0].id) + ' ' + hex(parsed.extensions.prf.eval.first)"),
        std::string("000102 010203 0405 00"));
    CHECK_EQ(page.string("try { PublicKeyCredential.parseRequestOptionsFromJSON({ challenge: '***' }); 'no' } catch (e) { e.name }"), std::string("EncodingError"));
    CHECK_EQ(page.string("try { PublicKeyCredential.parseRequestOptionsFromJSON({}); 'no' } catch (e) { e.name }"), std::string("TypeError"));
    CHECK(page.eval("PublicKeyCredential.signalUnknownCredential({ rpId: 'example.org', credentialId: 'AA' }).then(() => outcome = 'resolved', e => outcome = e.name)").ok);
    CHECK_EQ(page.await_outcome(), std::string("SecurityError"));
    CHECK(page.eval("PublicKeyCredential.signalUnknownCredential({ rpId: 'example.com', credentialId: 'AA' }).then(() => outcome = 'resolved', e => outcome = e.name)").ok);
    CHECK_EQ(page.await_outcome(), std::string("resolved"));
    CHECK(page.eval("navigator.credentials.store({}).then(() => outcome = 'stored', e => outcome = e.name)").ok);
    CHECK_EQ(page.await_outcome(), std::string("TypeError"));
}

}

int main()
{
    client_data_matches_the_vectors();
    client_data_escapes();
    rp_ids();
    ceremonies_end_to_end();
    refusals();
    abort_and_timeout();
    the_document_going_cancels();
    insecure_context_has_none();
    statics_and_json();
    return sashfold::test::report("test_webauthn");
}
