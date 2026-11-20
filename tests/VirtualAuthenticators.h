#pragma once
// WebDriver's virtual authenticators (WebAuthn Level 3 §11, "WebAuthn
// WebDriver Extension Capability") for the WPT runner: the commands
// test_driver sends — Add Virtual Authenticator, Remove Virtual
// Authenticator, Add Credential, Get Credentials, Remove Credential, Remove
// All Credentials, Set User Verified — carried out on software keys
// (SoftAuthenticator.h) plugged into a transport of the runner's own, and
// the ceremonies a page's create() and get() start, run through the same
// auth::Ceremony the window runs, with a prompt that is the virtual
// authenticator's user: it touches when the authenticator says the user
// consents, and verifies as isUserVerified says.
//
// Commands go in and come out as JSON text (the WebDriver command's
// parameters; {"value": ...} or {"error": "..."}).
#include "SoftAuthenticator.h"

#include "auth/Client.h"
#include "auth/ClientData.h"
#include "auth/DerWriter.h"
#include "core/Base64.h"
#include "core/Json.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::test {

class VirtualAuthenticators {
public:
    ~VirtualAuthenticators()
    {
        std::map<std::uint64_t, std::unique_ptr<auth::Ceremony>> ceremonies;
        {
            std::lock_guard lock(m_state->mutex);
            ceremonies.swap(m_ceremonies);
        }
        for (auto& [id, ceremony] : ceremonies)
            ceremony->cancel(false);
    }

    // A WebDriver command by its testdriver name, its parameters as JSON.
    std::string command(std::string_view name, std::string_view parameters)
    {
        std::optional<JsonValue> const parsed = JsonValue::parse(parameters.empty() ? std::string_view("{}") : parameters);
        if (!parsed || !parsed->is_object())
            return error("the parameters are not an object");
        JsonValue const& p = *parsed;
        if (name == "add")
            return add(p);
        std::shared_ptr<SoftAuthenticator> const key = find(string_of(p, "authenticatorId"));
        if (!key)
            return error("no such authenticator");
        if (name == "remove") {
            m_transport->unplug(key);
            std::erase_if(m_keys, [&](auto const& entry) { return entry.second == key; });
            return value("null");
        }
        if (name == "add_credential")
            return add_credential(*key, p.get("credential"));
        if (name == "get_credentials")
            return get_credentials(*key);
        if (name == "remove_credential") {
            std::optional<std::vector<std::uint8_t>> const id = base64url_decode(string_of(p, "credentialId"));
            if (!id || !key->remove_credentials(*id))
                return error("no such credential");
            return value("null");
        }
        if (name == "remove_all_credentials") {
            key->remove_credentials();
            return value("null");
        }
        if (name == "set_user_verified") {
            JsonValue const* const verified = p.get("isUserVerified");
            if (!verified || !verified->is_bool())
                return error("isUserVerified is not a boolean");
            key->set_uv_succeeds(verified->as_bool());
            return value("null");
        }
        return error("unknown command");
    }

    // A page's ceremony (HostHooks::security_key_ceremony).
    std::uint64_t start(auth::Request const& request, std::function<void(auth::Outcome)> done)
    {
        std::lock_guard lock(m_state->mutex);
        std::uint64_t const id = m_next++;
        m_state->last_report = std::chrono::steady_clock::now();
        ++m_state->running;
        std::shared_ptr<State> const state = m_state;
        auto ceremony = std::make_unique<auth::Ceremony>(request, m_transport, std::make_shared<User>(state),
            [state, done = std::move(done)](auth::Outcome outcome) {
                done(std::move(outcome));
                std::lock_guard inner(state->mutex);
                --state->running;
                state->last_report = std::chrono::steady_clock::now();
            });
        ceremony->start();
        m_ceremonies[id] = std::move(ceremony);
        return id;
    }
    void cancel(std::uint64_t id, bool by_page)
    {
        std::lock_guard lock(m_state->mutex);
        if (auto const found = m_ceremonies.find(id); found != m_ceremonies.end())
            found->second->cancel(by_page);
    }

    // Whether a ceremony is making progress the page will hear of soon: one
    // running whose keys have spoken within the last half second. A ceremony
    // that waits for a key that will not come or will not be touched is not
    // busy, and the page's clock may run on to its timeout.
    bool busy() const
    {
        std::lock_guard lock(m_state->mutex);
        return m_state->running > 0 && std::chrono::steady_clock::now() - m_state->last_report < std::chrono::milliseconds(500);
    }

private:
    struct State {
        std::mutex mutex;
        int running = 0;
        std::chrono::steady_clock::time_point last_report {};
    };

    // The virtual authenticator's user: nothing to type and nothing to
    // choose but the first account, as the WebDriver extension specifies no
    // prompt for either.
    class User final : public auth::Prompt {
    public:
        explicit User(std::shared_ptr<State> state)
            : m_state(std::move(state))
        {
        }
        void show(auth::PromptInfo const& info) override
        {
            std::lock_guard lock(m_state->mutex);
            // Waiting for a key, or for a touch that may never come, is not
            // progress; anything else the key says is.
            if (info.state != auth::PromptState::InsertKey && info.state != auth::PromptState::TouchKey)
                m_state->last_report = std::chrono::steady_clock::now();
        }
        void ask_pin(auth::PinQuestion const&, std::function<void(std::optional<auth::PinAnswer>)> answer) override { answer(std::nullopt); }
        void ask_account(std::vector<auth::Account> const&, std::function<void(std::optional<std::size_t>)> answer) override { answer(0); }
        void close() override { }

    private:
        std::shared_ptr<State> m_state;
    };

    static std::string escaped(std::string_view text)
    {
        std::string out = "\"";
        for (char const c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                char hex[8];
                std::snprintf(hex, sizeof hex, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += hex;
            } else {
                out += c;
            }
        }
        return out + "\"";
    }
    static std::string error(std::string_view why) { return "{\"error\": " + escaped(why) + "}"; }
    static std::string value(std::string_view json) { return "{\"value\": " + std::string(json) + "}"; }
    static std::string string_of(JsonValue const& object, std::string_view key)
    {
        JsonValue const* const found = object.get(key);
        return found && found->is_string() ? found->as_string() : std::string();
    }
    static std::optional<bool> bool_of(JsonValue const& object, std::string_view key)
    {
        JsonValue const* const found = object.get(key);
        if (!found || !found->is_bool())
            return std::nullopt;
        return found->as_bool();
    }

    // base64url as WebDriver writes it, and the plain base64 some tests give.
    static std::optional<std::vector<std::uint8_t>> base64url_decode(std::string text)
    {
        for (char& c : text) {
            if (c == '-')
                c = '+';
            else if (c == '_')
                c = '/';
        }
        return base64_decode(text);
    }

    std::shared_ptr<SoftAuthenticator> find(std::string const& id) const
    {
        for (auto const& [name, key] : m_keys) {
            if (name == id)
                return key;
        }
        return nullptr;
    }

    // Add Virtual Authenticator (§11.3): the authenticator's configuration.
    std::string add(JsonValue const& p)
    {
        SoftOptions options;
        std::string const protocol = string_of(p, "protocol");
        if (protocol == "ctap1/u2f")
            options.protocol = SoftOptions::Protocol::U2fOnly;
        else if (protocol == "ctap2")
            options.protocol = SoftOptions::Protocol::Ctap20;
        else if (protocol == "ctap2_1")
            options.protocol = SoftOptions::Protocol::Ctap21;
        else
            return error("unknown protocol");
        std::string const transport = string_of(p, "transport");
        if (transport != "usb" && transport != "nfc" && transport != "ble" && transport != "internal" && transport != "hybrid" && transport != "smart-card")
            return error("unknown transport");
        options.resident_keys = bool_of(p, "hasResidentKey").value_or(false);
        options.built_in_uv = bool_of(p, "hasUserVerification").value_or(false);
        // A virtual authenticator has no PIN: the user is verified by the
        // authenticator itself, or not at all.
        options.client_pin = false;
        options.cred_blob = false;
        options.hmac_secret = false;
        options.large_blob_key = false;
        options.min_pin_length_extension = false;
        if (JsonValue const* const extensions = p.get("extensions"); extensions && extensions->is_array()) {
            for (JsonValue const& extension : extensions->as_array()) {
                std::string const name = extension.is_string() ? extension.as_string() : std::string();
                if (name == "credBlob")
                    options.cred_blob = true;
                else if (name == "prf" || name == "hmac-secret")
                    options.hmac_secret = true;
                else if (name == "largeBlob")
                    options.large_blob_key = true;
                else if (name == "minPinLength")
                    options.min_pin_length_extension = true;
                else if (name != "credProps")
                    return error("unsupported extension");
            }
        }
        auto key = m_transport->plug(options);
        key->set_uv_succeeds(bool_of(p, "isUserVerified").value_or(false));
        key->set_touch(bool_of(p, "isUserConsenting").value_or(true) ? TouchPlan::immediately() : TouchPlan::never());
        std::string const id = "authenticator-" + std::to_string(++m_counter);
        m_keys.emplace_back(id, std::move(key));
        return value(escaped(id));
    }

    // A PKCS #8 private key (RFC 5208) of P-256 (RFC 5915's ECPrivateKey
    // inside) or of Ed25519 (RFC 8410): the scalar or the seed, and the
    // algorithm.
    static std::optional<std::pair<std::array<std::uint8_t, 32>, std::int64_t>> private_key_of(std::span<std::uint8_t const> der)
    {
        struct Tlv {
            std::uint8_t tag = 0;
            std::span<std::uint8_t const> content;
        };
        auto const read = [](std::span<std::uint8_t const>& in) -> std::optional<Tlv> {
            if (in.size() < 2)
                return std::nullopt;
            Tlv tlv;
            tlv.tag = in[0];
            std::size_t length = in[1];
            std::size_t at = 2;
            if (length & 0x80) {
                std::size_t const count = length & 0x7f;
                if (count == 0 || count > 3 || in.size() < 2 + count)
                    return std::nullopt;
                length = 0;
                for (std::size_t i = 0; i < count; ++i)
                    length = length << 8 | in[2 + i];
                at = 2 + count;
            }
            if (in.size() < at + length)
                return std::nullopt;
            tlv.content = in.subspan(at, length);
            in = in.subspan(at + length);
            return tlv;
        };
        std::span<std::uint8_t const> rest = der;
        std::optional<Tlv> const outer = read(rest);
        if (!outer || outer->tag != 0x30)
            return std::nullopt;
        std::span<std::uint8_t const> inside = outer->content;
        std::optional<Tlv> const version = read(inside);
        std::optional<Tlv> const algorithm = read(inside);
        std::optional<Tlv> const key = read(inside);
        if (!version || !algorithm || !key || algorithm->tag != 0x30 || key->tag != 0x04)
            return std::nullopt;
        std::span<std::uint8_t const> algorithm_parts = algorithm->content;
        std::optional<Tlv> const oid = read(algorithm_parts);
        if (!oid || oid->tag != 0x06)
            return std::nullopt;
        std::vector<std::uint8_t> const ec = auth::der::oid("1.2.840.10045.2.1");
        std::vector<std::uint8_t> const ed25519 = auth::der::oid("1.3.101.112");
        std::span<std::uint8_t const> key_bytes = key->content;
        std::optional<Tlv> const inner = read(key_bytes);
        if (!inner)
            return std::nullopt;
        std::array<std::uint8_t, 32> secret {};
        auto const oid_is = [&](std::vector<std::uint8_t> const& whole) {
            return whole.size() >= 2 && std::ranges::equal(std::span(whole).subspan(2), oid->content);
        };
        if (oid_is(ec) && inner->tag == 0x30) {
            std::span<std::uint8_t const> parts = inner->content;
            std::optional<Tlv> const one = read(parts);
            std::optional<Tlv> const scalar = read(parts);
            if (!one || !scalar || scalar->tag != 0x04 || scalar->content.size() != 32)
                return std::nullopt;
            std::ranges::copy(scalar->content, secret.begin());
            return std::pair { secret, auth::cose::alg_es256 };
        }
        if (oid_is(ed25519) && inner->tag == 0x04 && inner->content.size() == 32) {
            std::ranges::copy(inner->content, secret.begin());
            return std::pair { secret, auth::cose::alg_eddsa };
        }
        return std::nullopt;
    }

    static std::vector<std::uint8_t> pkcs8_of(SoftAuthenticator::Credential const& credential)
    {
        namespace der = auth::der;
        if (credential.alg == auth::cose::alg_eddsa) {
            std::vector<std::uint8_t> const seed = der::octet_string(credential.private_key);
            return der::sequence({ der::small_integer(0), der::sequence({ der::oid("1.3.101.112") }), der::octet_string(seed) });
        }
        std::vector<std::uint8_t> const ec_key = der::sequence({ der::small_integer(1), der::octet_string(credential.private_key) });
        return der::sequence({ der::small_integer(0), der::sequence({ der::oid("1.2.840.10045.2.1"), der::oid("1.2.840.10045.3.1.7") }),
            der::octet_string(ec_key) });
    }

    // Add Credential (§11.5).
    std::string add_credential(SoftAuthenticator& key, JsonValue const* credential)
    {
        if (!credential || !credential->is_object())
            return error("no credential");
        std::optional<std::vector<std::uint8_t>> const id = base64url_decode(string_of(*credential, "credentialId"));
        std::optional<std::vector<std::uint8_t>> const pkcs8 = base64url_decode(string_of(*credential, "privateKey"));
        std::string const rp_id = string_of(*credential, "rpId");
        std::optional<bool> const resident = bool_of(*credential, "isResidentCredential");
        if (!id || id->empty() || !pkcs8 || rp_id.empty() || !resident)
            return error("invalid credential parameters");
        auto const secret = private_key_of(*pkcs8);
        if (!secret)
            return error("the private key is not a PKCS #8 P-256 or Ed25519 key");
        if (*resident && !key.options().resident_keys)
            return error("the authenticator does not store resident credentials");
        SoftAuthenticator::Credential made;
        made.id = *id;
        made.rp_id = rp_id;
        made.rp_id_hash = crypto::Sha256::hash(std::span(reinterpret_cast<std::uint8_t const*>(rp_id.data()), rp_id.size()));
        made.private_key = secret->first;
        made.alg = secret->second;
        made.discoverable = *resident;
        made.from_u2f = key.options().protocol == SoftOptions::Protocol::U2fOnly;
        if (std::string const handle = string_of(*credential, "userHandle"); !handle.empty()) {
            std::optional<std::vector<std::uint8_t>> const user = base64url_decode(handle);
            if (!user)
                return error("invalid user handle");
            made.user.id = *user;
        }
        if (JsonValue const* const count = credential->get("signCount"); count && count->is_number())
            made.sign_count = static_cast<std::uint32_t>(count->as_number());
        platform::fill_random(made.cred_random_with_uv);
        platform::fill_random(made.cred_random_without_uv);
        key.add_credential(std::move(made));
        return value("null");
    }

    // Get Credentials (§11.6).
    std::string get_credentials(SoftAuthenticator& key)
    {
        std::string list = "[";
        bool first = true;
        for (SoftAuthenticator::Credential const& credential : key.credentials()) {
            list += first ? "" : ", ";
            first = false;
            list += "{\"credentialId\": " + escaped(auth::base64url(credential.id));
            list += ", \"isResidentCredential\": " + std::string(credential.discoverable ? "true" : "false");
            list += ", \"rpId\": " + escaped(credential.rp_id);
            list += ", \"privateKey\": " + escaped(auth::base64url(pkcs8_of(credential)));
            if (!credential.user.id.empty())
                list += ", \"userHandle\": " + escaped(auth::base64url(credential.user.id));
            list += ", \"signCount\": " + std::to_string(credential.sign_count) + "}";
        }
        return value(list + "]");
    }

    std::shared_ptr<FakeTransport> m_transport = std::make_shared<FakeTransport>();
    std::vector<std::pair<std::string, std::shared_ptr<SoftAuthenticator>>> m_keys;
    int m_counter = 0;
    std::shared_ptr<State> m_state = std::make_shared<State>();
    std::map<std::uint64_t, std::unique_ptr<auth::Ceremony>> m_ceremonies;
    std::uint64_t m_next = 1;
};

}
