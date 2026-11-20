#include "auth/Client.h"

#include "auth/CtapHid.h"
#include "auth/PinProtocol.h"
#include "auth/U2f.h"
#include "crypto/Sha2.h"
#include "platform/Random.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sashfold::auth {

char const* Error::dom_exception_name() const
{
    switch (kind) {
    case ErrorKind::NotAllowed:
        return "NotAllowedError";
    case ErrorKind::InvalidState:
        return "InvalidStateError";
    case ErrorKind::Abort:
        return "AbortError";
    case ErrorKind::Unknown:
        return "UnknownError";
    }
    return "UnknownError";
}

namespace {

using Clock = std::chrono::steady_clock;
using Status = ctap2::Status;

Error not_allowed(Reason reason, std::uint8_t status = 0) { return Error { ErrorKind::NotAllowed, reason, status }; }

std::array<std::uint8_t, 32> sha256_text(std::string_view text)
{
    return crypto::Sha256::hash(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(text.data()), text.size()));
}

// WebAuthn §10.1.4: the prf extension's inputs reach hmac-secret as
// SHA-256("WebAuthn PRF" || 0x00 || input).
std::array<std::uint8_t, 32> prf_salt(std::span<std::uint8_t const> input)
{
    crypto::Sha256 hash;
    std::string_view const label = "WebAuthn PRF";
    hash.update(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(label.data()), label.size()));
    std::uint8_t const zero = 0;
    hash.update(std::span<std::uint8_t const>(&zero, 1));
    hash.update(input);
    return hash.finish();
}

}

struct Ceremony::State : std::enable_shared_from_this<Ceremony::State> {
    Request request;
    std::shared_ptr<Transport> transport;
    std::shared_ptr<Prompt> prompt;
    Done done;
    Clock::time_point deadline;

    std::atomic<bool> cancelled { false };
    std::atomic<bool> by_page { false };

    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Outcome> outcome;
    bool delivered = false;
    int winner = -1;

    struct Slot {
        int id = 0;
        DeviceInfo device;
        CancelFlag cancel { false };
        std::atomic<bool> finished { false };
        std::thread thread;
    };
    std::vector<std::unique_ptr<Slot>> slots;
    std::thread main;
    bool started = false;

    std::mutex prompt_mutex;
    std::optional<PromptState> shown;

    void run();
    void run_key(Slot& slot);

    void finish(Outcome result)
    {
        std::lock_guard lock(mutex);
        if (!outcome)
            outcome = std::move(result);
        for (auto& slot : slots)
            slot->cancel = true;
        changed.notify_all();
    }
    void fail(Error error) { finish(Outcome { std::nullopt, error }); }

    bool has_outcome()
    {
        std::lock_guard lock(mutex);
        return outcome.has_value();
    }

    // The first key the user touches wins; the others are cancelled.
    bool claim(int id)
    {
        std::lock_guard lock(mutex);
        if (outcome)
            return false;
        if (winner == -1) {
            winner = id;
            for (auto& slot : slots) {
                if (slot->id != id)
                    slot->cancel = true;
            }
            return true;
        }
        return winner == id;
    }

    bool is_winner(int id)
    {
        std::lock_guard lock(mutex);
        return winner == id;
    }

    bool someone_won()
    {
        std::lock_guard lock(mutex);
        return winner != -1;
    }

    void show(PromptInfo const& info)
    {
        std::lock_guard lock(prompt_mutex);
        if (prompt)
            prompt->show(info);
        shown = info.state;
    }

    std::size_t keys_present()
    {
        std::lock_guard lock(mutex);
        return static_cast<std::size_t>(std::ranges::count_if(slots, [](auto const& s) { return !s->finished.load(); }));
    }
};

namespace {

// Waits for one answer from the prompt, or the end of the ceremony.
template <class T>
struct AnswerBox {
    std::mutex mutex;
    std::condition_variable answered;
    bool done = false;
    std::optional<T> value;
};

class KeyWorker {
public:
    KeyWorker(Ceremony::State& state, Ceremony::State::Slot& slot)
        : s(state)
        , m_slot(slot)
        , m_request(state.request)
    {
    }

    void run();

private:
    // ----- Plumbing -----

    bool stopped() const { return m_slot.cancel.load() || s.cancelled.load() || Clock::now() >= s.deadline; }

    // A key that refuses at once, again and again, is not asked in a
    // tight loop: a quarter second between tries.
    void pause_if_quick(Clock::time_point asked)
    {
        auto const until = asked + std::chrono::milliseconds(250);
        while (Clock::now() < until && !stopped())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    template <class T, class Ask>
    std::optional<T> ask(Ask const& ask_prompt)
    {
        auto box = std::make_shared<AnswerBox<T>>();
        {
            std::lock_guard lock(s.prompt_mutex);
            if (!s.prompt)
                return std::nullopt;
            ask_prompt(*s.prompt, [box](std::optional<T> value) {
                std::lock_guard inner(box->mutex);
                if (box->done)
                    return;
                box->done = true;
                box->value = std::move(value);
                box->answered.notify_all();
            });
        }
        std::unique_lock lock(box->mutex);
        while (!box->done) {
            if (stopped())
                return std::nullopt;
            box->answered.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (!box->value) {
            // The user dismissed the prompt.
            s.cancelled = true;
            s.fail(not_allowed(Reason::Cancelled));
        }
        return box->value;
    }

    Ctap2Reply call(std::uint8_t command, cbor::Value const* params)
    {
        Ctap2Device device(*m_hid);
        return device.call(command, params, s.deadline, &m_slot.cancel, [this](std::uint8_t status) {
            if (status == ctaphid::status_processing && s.is_winner(m_slot.id))
                show(PromptState::Processing);
            else if (status == ctaphid::status_up_needed && s.is_winner(m_slot.id) && m_touch_again)
                show(PromptState::TouchKeyAgain);
        });
    }

    void show(PromptState state, int retries = -1)
    {
        PromptInfo info;
        info.state = state;
        info.retries = retries;
        info.keys_present = s.keys_present();
        info.device = m_slot.device.product;
        s.show(info);
    }

    // Ends the ceremony with this key's failure, once the key has been
    // chosen (or is chosen now).
    void fail_chosen(Error error, std::optional<PromptState> state = std::nullopt)
    {
        if (!s.claim(m_slot.id))
            return;
        // A command that failed because the key left says so.
        if (error.reason == Reason::DeviceError && m_hid && m_hid->gone())
            error.reason = Reason::DeviceGone;
        if (state)
            show(*state);
        s.fail(error);
    }

    // ----- Identification -----

    bool is_ctap2() const { return m_info.has_value(); }
    bool ctap21() const { return m_info && m_info->is_ctap21(); }

    // ----- Touch to choose a key -----

    // Collects a touch on this key without making anything, and claims the
    // ceremony for it. False when another key won, the user did not touch
    // it before the deadline, or the ceremony ended.
    bool select()
    {
        if (s.is_winner(m_slot.id))
            return true;
        while (!stopped()) {
            auto const asked = Clock::now();
            if (!is_ctap2()) {
                auto const request = u2f::register_request(u2f::bogus_challenge(), u2f::bogus_application());
                auto const answer = u2f::u2f_poll(*m_hid, request, s.deadline, &m_slot.cancel);
                if (!answer)
                    return false;
                if (answer->status == u2f::sw_no_error)
                    return s.claim(m_slot.id);
                return false;
            }
            Ctap2Reply reply;
            if (ctap21()) {
                reply = call(ctap2::cmd_selection, nullptr);
                if (reply.ok())
                    return s.claim(m_slot.id);
            } else if (m_info->options.contains("clientPin")) {
                // CTAP 2.0: a zero-length pinUvAuthParam waits for a touch
                // and answers PIN_NOT_SET or PIN_INVALID (§6.1.2 step 1).
                MakeCredentialParams params = bogus_make_credential();
                params.pin_uv_auth_param = std::vector<std::uint8_t> {};
                params.pin_uv_auth_protocol = 1;
                auto const encoded = encode_make_credential(params);
                reply = call(ctap2::cmd_make_credential, &encoded);
                if (reply.failure == HidFailure::None
                    && (reply.status == std::uint8_t(Status::PinNotSet) || reply.status == std::uint8_t(Status::PinInvalid)))
                    return s.claim(m_slot.id);
            } else {
                // A key with no PIN at all: a throwaway credential for a
                // name no site has is the touch.
                auto const encoded = encode_make_credential(bogus_make_credential());
                reply = call(ctap2::cmd_make_credential, &encoded);
                if (reply.ok())
                    return s.claim(m_slot.id);
            }
            if (reply.failure != HidFailure::None || reply.status == std::uint8_t(Status::KeepaliveCancel))
                return false;
            // OPERATION_DENIED, USER_ACTION_TIMEOUT: the key gave up waiting; ask again.
            if (reply.status != std::uint8_t(Status::OperationDenied) && reply.status != std::uint8_t(Status::UserActionTimeout)
                && reply.status != std::uint8_t(Status::ActionTimeout))
                return false;
            pause_if_quick(asked);
        }
        return false;
    }

    MakeCredentialParams bogus_make_credential() const
    {
        MakeCredentialParams params;
        params.client_data_hash.fill(0);
        params.rp.id = ".dummy";
        params.user.id = { 0 };
        params.user.name = "dummy";
        params.user.has_name = true;
        params.algorithms = { cose::alg_es256 };
        return params;
    }

    // ----- PIN and UV -----

    struct Token {
        pin::Protocol protocol = pin::Protocol::One;
        std::vector<std::uint8_t> token;
        bool uv_option = false; // CTAP 2.0 built-in UV: the command carries "uv": true instead
    };

    std::optional<pin::Protocol> protocol() const
    {
        for (std::uint8_t const p : m_info->pin_uv_auth_protocols) {
            if (auto const protocol = pin::protocol_from_number(p))
                return protocol;
        }
        // A CTAP 2.0 key that names no protocol speaks protocol one.
        if (m_info->pin_uv_auth_protocols.empty() && !ctap21())
            return pin::Protocol::One;
        return std::nullopt;
    }

    std::optional<pin::Encapsulation> key_agreement(pin::Protocol protocol)
    {
        ClientPinParams params;
        params.protocol = static_cast<std::uint8_t>(protocol);
        params.subcommand = ctap2::pin_get_key_agreement;
        auto const encoded = encode_client_pin(params);
        auto const reply = call(ctap2::cmd_client_pin, &encoded);
        if (!reply.ok())
            return std::nullopt;
        auto const response = parse_client_pin_response(reply.body);
        if (!response || !response->key_agreement)
            return std::nullopt;
        return pin::encapsulate(protocol, *response->key_agreement, platform::fill_random);
    }

    std::optional<std::uint64_t> retries(std::uint8_t subcommand)
    {
        ClientPinParams params;
        params.protocol = static_cast<std::uint8_t>(protocol().value_or(pin::Protocol::One));
        params.subcommand = subcommand;
        auto const encoded = encode_client_pin(params);
        auto const reply = call(ctap2::cmd_client_pin, &encoded);
        if (!reply.ok())
            return std::nullopt;
        auto const response = parse_client_pin_response(reply.body);
        if (!response)
            return std::nullopt;
        return subcommand == ctap2::pin_get_uv_retries ? response->uv_retries : response->pin_retries;
    }

    // The token from a decrypted getPinToken answer.
    std::optional<Token> token_from(pin::Protocol protocol, pin::Encapsulation const& shared, Ctap2Reply const& reply)
    {
        auto const response = parse_client_pin_response(reply.body);
        if (!response || !response->pin_uv_auth_token)
            return std::nullopt;
        auto token = pin::decrypt(protocol, shared.shared_secret, *response->pin_uv_auth_token);
        if (!token || (token->size() != 32 && !(protocol == pin::Protocol::One && token->size() == 16)))
            return std::nullopt;
        return Token { protocol, std::move(*token), false };
    }

    // Built-in UV through getPinUvAuthTokenUsingUvWithPermissions (§6.5.5.7.3).
    // Nullopt with `fall_back_to_pin` set when UV is blocked and a PIN exists.
    std::optional<Token> token_by_uv(pin::Protocol protocol, std::uint8_t permissions, bool& fall_back_to_pin)
    {
        std::size_t const attempts = std::max<std::size_t>(1, m_info->preferred_platform_uv_attempts.value_or(3));
        for (std::size_t attempt = 0; attempt < attempts && !stopped(); ++attempt) {
            show(PromptState::VerifyFingerprint);
            auto const shared = key_agreement(protocol);
            if (!shared)
                break;
            ClientPinParams params;
            params.protocol = static_cast<std::uint8_t>(protocol);
            params.subcommand = ctap2::pin_get_token_using_uv;
            params.key_agreement = shared->platform_key;
            params.permissions = permissions;
            params.rp_id = m_request.rp_id;
            auto const encoded = encode_client_pin(params);
            auto const reply = call(ctap2::cmd_client_pin, &encoded);
            if (reply.ok())
                return token_from(protocol, *shared, reply);
            if (reply.failure != HidFailure::None || reply.status == std::uint8_t(Status::KeepaliveCancel))
                return std::nullopt;
            if (reply.status == std::uint8_t(Status::UvInvalid)) {
                auto const left = retries(ctap2::pin_get_uv_retries);
                show(PromptState::UvRetry, left ? static_cast<int>(*left) : -1);
                continue;
            }
            if (reply.status == std::uint8_t(Status::UvBlocked)) {
                show(PromptState::UvBlocked);
                fall_back_to_pin = m_info->option_true("clientPin");
                if (!fall_back_to_pin)
                    fail_chosen(not_allowed(Reason::UvBlocked));
                return std::nullopt;
            }
            if (reply.status == std::uint8_t(Status::UserActionTimeout))
                continue;
            fail_chosen(not_allowed(Reason::DeviceError, reply.status));
            return std::nullopt;
        }
        fall_back_to_pin = m_info->option_true("clientPin");
        return std::nullopt;
    }

    bool set_pin(pin::Protocol protocol, std::string const& new_pin)
    {
        auto const padded = pin::padded_pin(new_pin);
        auto const shared = key_agreement(protocol);
        if (!padded || !shared)
            return false;
        auto const encrypted = pin::encrypt(protocol, shared->shared_secret, *padded, platform::fill_random);
        if (!encrypted)
            return false;
        ClientPinParams params;
        params.protocol = static_cast<std::uint8_t>(protocol);
        params.subcommand = ctap2::pin_set;
        params.key_agreement = shared->platform_key;
        params.new_pin_enc = *encrypted;
        params.pin_uv_auth_param = pin::authenticate(protocol, shared->shared_secret, *encrypted);
        auto const encoded = encode_client_pin(params);
        return call(ctap2::cmd_client_pin, &encoded).ok();
    }

    Status change_pin(pin::Protocol protocol, std::string const& current, std::string const& new_pin)
    {
        auto const padded = pin::padded_pin(new_pin);
        if (!padded)
            return Status::PinPolicyViolation;
        auto const shared = key_agreement(protocol);
        if (!shared)
            return Status::Other;
        auto const new_pin_enc = pin::encrypt(protocol, shared->shared_secret, *padded, platform::fill_random);
        auto const hash = pin::pin_hash(current);
        auto const pin_hash_enc = pin::encrypt(protocol, shared->shared_secret, hash, platform::fill_random);
        if (!new_pin_enc || !pin_hash_enc)
            return Status::Other;
        ClientPinParams params;
        params.protocol = static_cast<std::uint8_t>(protocol);
        params.subcommand = ctap2::pin_change;
        params.key_agreement = shared->platform_key;
        params.new_pin_enc = *new_pin_enc;
        params.pin_hash_enc = *pin_hash_enc;
        std::vector<std::uint8_t> message = *new_pin_enc;
        message.insert(message.end(), pin_hash_enc->begin(), pin_hash_enc->end());
        params.pin_uv_auth_param = pin::authenticate(protocol, shared->shared_secret, message);
        auto const encoded = encode_client_pin(params);
        auto const reply = call(ctap2::cmd_client_pin, &encoded);
        if (reply.failure != HidFailure::None)
            return Status::Other;
        return static_cast<Status>(reply.status);
    }

    // The PIN flow of §6.5.5.7: ask, send the hash encrypted, read the
    // answer; a wrong PIN asks again with the tries left; a blocked PIN or
    // three in a row end the ceremony.
    std::optional<Token> token_by_pin(pin::Protocol protocol, std::uint8_t permissions)
    {
        auto const question = [&](PinQuestion::Kind kind, int left) {
            PinQuestion q;
            q.kind = kind;
            q.retries = left;
            q.min_length = m_info->min_pin_length.value_or(4);
            q.device = m_slot.device.product;
            return q;
        };
        // A PIN the user has just chosen is the one the token is asked for
        // with: they are not asked to type it a second time.
        std::optional<std::string> chosen;
        if (!m_info->option_true("clientPin")) {
            // No PIN yet: one is set first, when the key can have one.
            if (!m_info->options.contains("clientPin") || m_request.operation != Operation::Create) {
                fail_chosen(not_allowed(Reason::Unsuitable), PromptState::KeyUnsuitable);
                return std::nullopt;
            }
            while (true) {
                auto const answer = ask<PinAnswer>([&](Prompt& p, auto callback) { p.ask_pin(question(PinQuestion::Kind::Set, -1), callback); });
                if (!answer)
                    return std::nullopt;
                if (set_pin(protocol, answer->pin)) {
                    m_info->options["clientPin"] = true;
                    chosen = answer->pin;
                    break;
                }
                if (stopped())
                    return std::nullopt;
            }
        }
        if (m_info->force_pin_change) {
            auto const answer = ask<PinAnswer>([&](Prompt& p, auto callback) { p.ask_pin(question(PinQuestion::Kind::Change, -1), callback); });
            if (!answer)
                return std::nullopt;
            Status const changed = change_pin(protocol, answer->pin, answer->new_pin);
            if (changed != Status::Ok) {
                fail_chosen(not_allowed(changed == Status::PinPolicyViolation ? Reason::PinPolicy : Reason::DeviceError, std::uint8_t(changed)));
                return std::nullopt;
            }
            m_info->force_pin_change = false;
            chosen = answer->new_pin;
        }
        auto left = retries(ctap2::pin_get_retries);
        if (left && *left == 0) {
            fail_chosen(not_allowed(Reason::PinBlocked), PromptState::PinBlocked);
            return std::nullopt;
        }
        bool wrong = false;
        while (!stopped()) {
            std::optional<PinAnswer> answer;
            if (chosen) {
                answer = PinAnswer { *chosen, {} };
                chosen.reset();
            } else {
                answer = ask<PinAnswer>([&](Prompt& p, auto callback) {
                    p.ask_pin(question(wrong ? PinQuestion::Kind::Retry : PinQuestion::Kind::Enter, left ? static_cast<int>(*left) : -1), callback);
                });
            }
            if (!answer)
                return std::nullopt;
            auto const shared = key_agreement(protocol);
            if (!shared)
                break;
            auto const hash = pin::pin_hash(answer->pin);
            auto const pin_hash_enc = pin::encrypt(protocol, shared->shared_secret, hash, platform::fill_random);
            if (!pin_hash_enc)
                break;
            ClientPinParams params;
            params.protocol = static_cast<std::uint8_t>(protocol);
            params.key_agreement = shared->platform_key;
            params.pin_hash_enc = *pin_hash_enc;
            if (ctap21() && m_info->option_true("pinUvAuthToken")) {
                params.subcommand = ctap2::pin_get_token_using_pin;
                params.permissions = permissions;
                params.rp_id = m_request.rp_id;
            } else {
                params.subcommand = ctap2::pin_get_token;
            }
            auto const encoded = encode_client_pin(params);
            auto const reply = call(ctap2::cmd_client_pin, &encoded);
            if (reply.ok())
                return token_from(protocol, *shared, reply);
            if (reply.failure != HidFailure::None)
                break;
            switch (static_cast<Status>(reply.status)) {
            case Status::PinInvalid:
                wrong = true;
                left = retries(ctap2::pin_get_retries);
                if (left && *left == 0) {
                    fail_chosen(not_allowed(Reason::PinBlocked), PromptState::PinBlocked);
                    return std::nullopt;
                }
                continue;
            case Status::PinBlocked:
                fail_chosen(not_allowed(Reason::PinBlocked), PromptState::PinBlocked);
                return std::nullopt;
            case Status::PinAuthBlocked:
                fail_chosen(not_allowed(Reason::PinAuthBlocked), PromptState::PinAuthBlocked);
                return std::nullopt;
            default:
                fail_chosen(not_allowed(Reason::DeviceError, reply.status));
                return std::nullopt;
            }
        }
        if (!stopped())
            fail_chosen(not_allowed(Reason::DeviceError));
        return std::nullopt;
    }

    // Built-in UV where the key has it, else the PIN.
    std::optional<Token> obtain_token(std::uint8_t permissions)
    {
        auto const chosen = protocol();
        if (m_info->option_true("uv")) {
            if (ctap21() && m_info->option_true("pinUvAuthToken") && chosen) {
                bool fall_back = false;
                auto token = token_by_uv(*chosen, permissions, fall_back);
                if (token || !fall_back)
                    return token;
            } else {
                return Token { pin::Protocol::One, {}, true };
            }
        }
        if (!chosen) {
            fail_chosen(not_allowed(Reason::Unsuitable), PromptState::KeyUnsuitable);
            return std::nullopt;
        }
        return token_by_pin(*chosen, permissions);
    }

    // ----- Probing the credential lists -----

    // The first credential of `list` this key holds under `rp_id`, asked
    // silently (up=false) in chunks the key accepts; nullopt when it holds
    // none. With a token, credentials protected by credProtect level 3 are
    // found too.
    std::optional<CredentialDescriptor> probe(std::vector<CredentialDescriptor> const& list, std::string const& rp_id, Token const* token)
    {
        std::vector<CredentialDescriptor> usable;
        for (auto const& credential : list) {
            if (m_info->max_credential_id_length && credential.id.size() > *m_info->max_credential_id_length)
                continue;
            usable.push_back(credential);
        }
        std::size_t const chunk = std::max<std::size_t>(1, m_info->max_credential_count_in_list.value_or(1));
        for (std::size_t start = 0; start < usable.size() && !stopped(); start += chunk) {
            GetAssertionParams params;
            params.rp_id = rp_id;
            params.client_data_hash.fill(0);
            params.allow_list.assign(usable.begin() + static_cast<std::ptrdiff_t>(start),
                usable.begin() + static_cast<std::ptrdiff_t>(std::min(usable.size(), start + chunk)));
            params.up = false;
            if (token != nullptr && !token->uv_option && !token->token.empty()) {
                params.pin_uv_auth_param = pin::authenticate(token->protocol, token->token, params.client_data_hash);
                params.pin_uv_auth_protocol = static_cast<std::uint8_t>(token->protocol);
            }
            auto const encoded = encode_get_assertion(params);
            auto const reply = call(ctap2::cmd_get_assertion, &encoded);
            if (reply.ok()) {
                auto const response = parse_get_assertion_response(reply.body);
                if (response && response->credential)
                    return response->credential;
                if (params.allow_list.size() == 1)
                    return params.allow_list.front();
                continue;
            }
            if (reply.failure != HidFailure::None)
                return std::nullopt;
        }
        return std::nullopt;
    }

    // U2F check-only: the first handle of `list` this key knows under `application`.
    std::optional<CredentialDescriptor> u2f_probe(std::vector<CredentialDescriptor> const& list, std::array<std::uint8_t, 32> const& application)
    {
        auto const challenge = u2f::bogus_challenge();
        for (auto const& credential : list) {
            auto const request = u2f::authenticate_request(u2f::check_only, challenge, application, credential.id);
            if (!request)
                continue;
            auto const answer = u2f::u2f_send(*m_hid, *request, s.deadline, &m_slot.cancel);
            if (!answer)
                return std::nullopt;
            // "Test of user presence required" means the handle is the key's own.
            if (answer->status == u2f::sw_conditions_not_satisfied || answer->status == u2f::sw_no_error)
                return credential;
        }
        return std::nullopt;
    }

    // ----- The two ceremonies -----

    void ctap2_create();
    void ctap2_get();
    void u2f_create();
    void u2f_get();

    // The pinUvAuthParam pair for a command.
    void authorize(std::optional<std::vector<std::uint8_t>>& param, std::optional<std::uint8_t>& protocol_number, Token const& token)
    {
        if (token.uv_option)
            return;
        param = pin::authenticate(token.protocol, token.token, m_request.client_data_hash);
        protocol_number = static_cast<std::uint8_t>(token.protocol);
    }

    // hmac-secret for prf (§12.5, WebAuthn §10.1.4): the salts encrypted
    // under a fresh shared secret.
    struct HmacSecret {
        pin::Protocol protocol;
        std::vector<std::uint8_t> shared_secret;
        bool two = false;
    };
    std::optional<HmacSecret> hmac_secret_input(cbor::Value& extensions, PrfInputs const& inputs)
    {
        auto const chosen = protocol();
        if (!chosen)
            return std::nullopt;
        auto const shared = key_agreement(*chosen);
        if (!shared)
            return std::nullopt;
        std::vector<std::uint8_t> salts;
        auto const first = prf_salt(inputs.first);
        salts.insert(salts.end(), first.begin(), first.end());
        if (inputs.second) {
            auto const second = prf_salt(*inputs.second);
            salts.insert(salts.end(), second.begin(), second.end());
        }
        auto const salt_enc = pin::encrypt(*chosen, shared->shared_secret, salts, platform::fill_random);
        if (!salt_enc)
            return std::nullopt;
        cbor::Value input = cbor::Value::map();
        input.set(0x01, cose_key_to_cbor(shared->platform_key));
        input.set(0x02, cbor::Value::bytes(*salt_enc));
        input.set(0x03, cbor::Value::bytes(pin::authenticate(*chosen, shared->shared_secret, *salt_enc)));
        if (*chosen != pin::Protocol::One)
            input.set(0x04, cbor::Value::unsigned_integer(static_cast<std::uint64_t>(*chosen)));
        extensions.set("hmac-secret", std::move(input));
        return HmacSecret { *chosen, shared->shared_secret, inputs.second.has_value() };
    }

    PrfInputs const* prf_inputs_for(std::span<std::uint8_t const> credential_id) const
    {
        for (auto const& [id, inputs] : m_request.extensions.prf_eval_by_credential) {
            if (std::ranges::equal(id, credential_id))
                return &inputs;
        }
        return m_request.extensions.prf_eval ? &*m_request.extensions.prf_eval : nullptr;
    }

    void finish_get(GetAssertionResponse const& response, CredentialDescriptor const& fallback, std::optional<HmacSecret> const& hmac,
        bool used_appid);

    Ceremony::State& s;
    Ceremony::State::Slot& m_slot;
    Request const& m_request;
    std::unique_ptr<CtapHidDevice> m_hid;
    std::optional<AuthenticatorInfo> m_info;
    bool m_touch_again = false;
};

void KeyWorker::run()
{
    auto connection = s.transport->open(m_slot.device);
    if (!connection)
        return;
    m_hid = std::make_unique<CtapHidDevice>(std::move(connection), m_slot.device);
    if (!m_hid->init(s.deadline, &m_slot.cancel))
        return;
    if (m_hid->has_cbor()) {
        Ctap2Device device(*m_hid);
        m_info = device.get_info(s.deadline, &m_slot.cancel);
        if (stopped())
            return;
        if (!m_info && !m_hid->has_msg())
            return;
    } else if (!m_hid->has_msg()) {
        return;
    }
    if (!s.someone_won())
        show(PromptState::TouchKey);
    if (m_request.operation == Operation::Create) {
        if (is_ctap2())
            ctap2_create();
        else
            u2f_create();
    } else {
        if (is_ctap2())
            ctap2_get();
        else
            u2f_get();
    }
    // The chosen key went away before it answered.
    if (m_hid->gone() && s.is_winner(m_slot.id))
        s.fail(not_allowed(Reason::DeviceGone));
}

void KeyWorker::ctap2_create()
{
    AuthenticatorInfo& info = *m_info;
    ExtensionInputs const& ext = m_request.extensions;

    // Can it serve the request at all?
    bool suitable = true;
    if (!info.algorithms.empty()
        && std::ranges::none_of(m_request.algorithms, [&](std::int64_t alg) { return std::ranges::find(info.algorithms, alg) != info.algorithms.end(); }))
        suitable = false;
    bool const supports_rk = info.option_true("rk");
    if (m_request.resident_key == Requirement::Required && !supports_rk)
        suitable = false;
    bool const can_verify = info.uv_protected() || info.options.contains("clientPin");
    if (m_request.user_verification == Requirement::Required && !can_verify)
        suitable = false;
    if (ext.cred_protect && *ext.cred_protect > 1 && ext.enforce_cred_protect && !info.has_extension("credProtect"))
        suitable = false;
    if (!suitable) {
        if (select())
            fail_chosen(not_allowed(Reason::Unsuitable), PromptState::KeyUnsuitable);
        return;
    }

    bool const rk = m_request.resident_key == Requirement::Required
        || (m_request.resident_key == Requirement::Preferred && supports_rk && info.uv_protected());
    // §6.1.2 step 6 and the request: when is user verification done?
    bool const protected_key = info.uv_protected();
    bool const need_uv = m_request.user_verification == Requirement::Required
        || (protected_key
            && (m_request.user_verification == Requirement::Preferred || rk || info.option_true("alwaysUv")
                || !info.option_true("makeCredUvNotRqd")));

    std::optional<Token> token;
    std::uint8_t const permissions = ctap2::permission_make_credential
        | (m_request.credentials.empty() ? 0 : ctap2::permission_get_assertion);
    if (need_uv) {
        if (!select())
            return;
        token = obtain_token(permissions);
        if (!token)
            return;
    }

    // The exclude list, under the RP ID and the appidExclude AppID.
    if (!m_request.credentials.empty()) {
        auto found = probe(m_request.credentials, m_request.rp_id, token ? &*token : nullptr);
        if (!found && ext.appid_exclude)
            found = probe(m_request.credentials, *ext.appid_exclude, nullptr);
        if (stopped())
            return;
        if (found) {
            if (select())
                fail_chosen(Error { ErrorKind::InvalidState, Reason::CredentialExcluded, 0 }, PromptState::KeyAlreadyRegistered);
            return;
        }
    }

    MakeCredentialParams params;
    params.client_data_hash = m_request.client_data_hash;
    params.rp.id = m_request.rp_id;
    params.rp.name = m_request.rp_name;
    params.user = m_request.user;
    params.algorithms = m_request.algorithms;
    if (rk)
        params.rk = true;
    // Extensions (CTAP 2.1 §12), each only where the key names it.
    cbor::Value extensions = cbor::Value::map();
    if (ext.cred_protect && info.has_extension("credProtect"))
        extensions.set("credProtect", cbor::Value::unsigned_integer(*ext.cred_protect));
    bool const want_hmac = (ext.prf || ext.hmac_create_secret) && info.has_extension("hmac-secret");
    if (want_hmac)
        extensions.set("hmac-secret", cbor::Value::boolean(true));
    if (ext.cred_blob && info.has_extension("credBlob") && ext.cred_blob->size() <= info.max_cred_blob_length.value_or(0))
        extensions.set("credBlob", cbor::Value::bytes(*ext.cred_blob));
    if (ext.large_blob_key && rk && info.has_extension("largeBlobKey"))
        extensions.set("largeBlobKey", cbor::Value::boolean(true));
    if (ext.min_pin_length && info.has_extension("minPinLength"))
        extensions.set("minPinLength", cbor::Value::boolean(true));
    params.extensions = extensions;

    bool retried_token = false;
    while (!stopped()) {
        params.pin_uv_auth_param.reset();
        params.pin_uv_auth_protocol.reset();
        params.uv.reset();
        if (token) {
            if (token->uv_option)
                params.uv = true;
            else
                authorize(params.pin_uv_auth_param, params.pin_uv_auth_protocol, *token);
        }
        m_touch_again = token.has_value();
        if (m_touch_again && s.is_winner(m_slot.id))
            show(PromptState::TouchKeyAgain);
        auto const encoded = encode_make_credential(params);
        auto const asked = Clock::now();
        auto const reply = call(ctap2::cmd_make_credential, &encoded);
        if (reply.failure != HidFailure::None)
            return;
        Status const status = static_cast<Status>(reply.status);
        if (reply.ok()) {
            auto const response = parse_make_credential_response(reply.body);
            if (!response) {
                fail_chosen(not_allowed(Reason::DeviceError));
                return;
            }
            if (!s.claim(m_slot.id))
                return;
            auto const object = parse_attestation_object(encode_attestation_object(response->fmt, response->att_stmt, response->auth_data));
            if (!object || !object->parsed.attested) {
                s.fail(not_allowed(Reason::DeviceError));
                return;
            }
            Result result;
            result.operation = Operation::Create;
            result.device_path = m_slot.device.path;
            auto const conveyed = apply_conveyance(*object, m_request.attestation);
            result.attestation_object = encode_attestation_object(conveyed.fmt, conveyed.att_stmt, conveyed.auth_data);
            result.created = created_credential_fields(conveyed, result.transports);
            result.credential_id = object->parsed.attested->credential_id;
            ExtensionOutputs& out = result.extensions;
            cbor::Value const* const outputs = object->parsed.extensions ? &*object->parsed.extensions : nullptr;
            if (ext.cred_props)
                out.cred_props_rk = rk;
            if (ext.appid_exclude)
                out.appid_exclude = true;
            bool const hmac_enabled = outputs != nullptr && outputs->find("hmac-secret") != nullptr
                && outputs->find("hmac-secret")->as_bool() == true;
            if (ext.prf)
                out.prf_enabled = hmac_enabled;
            if (ext.hmac_create_secret)
                out.hmac_create_secret = hmac_enabled;
            if (outputs != nullptr) {
                if (cbor::Value const* v = outputs->find("credProtect"); v != nullptr && v->is_unsigned() && v->raw_number() <= 3)
                    out.cred_protect = static_cast<std::uint8_t>(v->raw_number());
                if (cbor::Value const* v = outputs->find("credBlob"); v != nullptr && v->is_bool() && ext.cred_blob)
                    out.cred_blob_stored = v->as_bool();
                if (cbor::Value const* v = outputs->find("minPinLength"); v != nullptr && v->is_unsigned() && ext.min_pin_length)
                    out.min_pin_length = v->raw_number();
            }
            if (ext.large_blob_key)
                out.large_blob_key = response->large_blob_key;
            s.finish(Outcome { std::move(result), std::nullopt });
            return;
        }
        switch (status) {
        case Status::CredentialExcluded:
            fail_chosen(Error { ErrorKind::InvalidState, Reason::CredentialExcluded, 0 }, PromptState::KeyAlreadyRegistered);
            return;
        case Status::KeepaliveCancel:
            return;
        case Status::OperationDenied:
        case Status::UserActionTimeout:
        case Status::ActionTimeout:
            pause_if_quick(asked);
            continue; // the key stopped waiting for its touch; ask again
        case Status::PuatRequired:
        case Status::PinAuthInvalid:
            // The key wants verification after all, or the token expired.
            if (retried_token)
                break;
            retried_token = true;
            if (!select())
                return;
            token = obtain_token(permissions);
            if (!token)
                return;
            continue;
        case Status::UnsupportedAlgorithm:
        case Status::UnsupportedOption:
        case Status::InvalidOption:
            if (select())
                fail_chosen(not_allowed(Reason::Unsuitable), PromptState::KeyUnsuitable);
            return;
        default:
            break;
        }
        if (s.is_winner(m_slot.id) || status == Status::KeyStoreFull)
            fail_chosen(not_allowed(Reason::DeviceError, reply.status));
        return;
    }
}

void KeyWorker::finish_get(GetAssertionResponse const& response, CredentialDescriptor const& fallback, std::optional<HmacSecret> const& hmac,
    bool used_appid)
{
    ExtensionInputs const& ext = m_request.extensions;
    Result result;
    result.operation = Operation::Get;
    result.device_path = m_slot.device.path;
    result.credential_id = response.credential ? response.credential->id : fallback.id;
    result.authenticator_data = response.auth_data;
    result.signature = response.signature;
    if (response.user)
        result.user_handle = response.user->id;
    auto const parsed = parse_authenticator_data(response.auth_data);
    if (!parsed) {
        s.fail(not_allowed(Reason::DeviceError));
        return;
    }
    ExtensionOutputs& out = result.extensions;
    if (ext.appid)
        out.appid = used_appid;
    cbor::Value const* const outputs = parsed->extensions ? &*parsed->extensions : nullptr;
    if (hmac && outputs != nullptr) {
        if (cbor::Value const* v = outputs->find("hmac-secret"); v != nullptr && v->is_bytes()) {
            auto const plain = pin::decrypt(hmac->protocol, hmac->shared_secret, v->as_bytes());
            if (plain && plain->size() == (hmac->two ? 64u : 32u)) {
                out.prf_first = std::vector<std::uint8_t>(plain->begin(), plain->begin() + 32);
                if (hmac->two)
                    out.prf_second = std::vector<std::uint8_t>(plain->begin() + 32, plain->end());
            }
        }
    }
    if (ext.get_cred_blob && outputs != nullptr) {
        if (cbor::Value const* v = outputs->find("credBlob"); v != nullptr && v->is_bytes())
            out.cred_blob = v->byte_vector();
    }
    if (ext.large_blob_key)
        out.large_blob_key = response.large_blob_key;
    s.finish(Outcome { std::move(result), std::nullopt });
}

void KeyWorker::ctap2_get()
{
    AuthenticatorInfo& info = *m_info;
    ExtensionInputs const& ext = m_request.extensions;
    bool const discoverable = m_request.credentials.empty();
    bool suitable = true;
    if (discoverable && !info.option_true("rk"))
        suitable = false;
    if (m_request.user_verification == Requirement::Required && !info.uv_protected())
        suitable = false;
    if (!suitable) {
        if (select())
            fail_chosen(not_allowed(Reason::Unsuitable), PromptState::KeyUnsuitable);
        return;
    }
    bool const need_uv = m_request.user_verification == Requirement::Required
        || (info.uv_protected() && (m_request.user_verification == Requirement::Preferred || info.option_true("alwaysUv")));

    std::optional<Token> token;
    if (need_uv) {
        if (!select())
            return;
        token = obtain_token(ctap2::permission_get_assertion);
        if (!token)
            return;
    }

    std::string rp_id = m_request.rp_id;
    bool used_appid = false;
    std::vector<CredentialDescriptor> allow;
    if (!discoverable) {
        auto found = probe(m_request.credentials, m_request.rp_id, token ? &*token : nullptr);
        if (!found && ext.appid && !stopped()) {
            found = probe(m_request.credentials, *ext.appid, nullptr);
            if (found) {
                rp_id = *ext.appid;
                used_appid = true;
            }
        }
        if (stopped())
            return;
        if (!found) {
            if (select())
                fail_chosen(not_allowed(Reason::NotRegistered), PromptState::KeyNotRegistered);
            return;
        }
        allow.push_back(*found);
    }

    bool retried_token = false;
    while (!stopped()) {
        GetAssertionParams params;
        params.rp_id = rp_id;
        params.client_data_hash = m_request.client_data_hash;
        params.allow_list = allow;
        if (token) {
            if (token->uv_option)
                params.uv = true;
            else if (!used_appid)
                authorize(params.pin_uv_auth_param, params.pin_uv_auth_protocol, *token);
        }
        cbor::Value extensions = cbor::Value::map();
        std::optional<HmacSecret> hmac;
        if (ext.prf && info.has_extension("hmac-secret")) {
            PrfInputs const* inputs = prf_inputs_for(allow.empty() ? std::span<std::uint8_t const>() : std::span<std::uint8_t const>(allow[0].id));
            if (inputs != nullptr)
                hmac = hmac_secret_input(extensions, *inputs);
        }
        if (ext.get_cred_blob && info.has_extension("credBlob"))
            extensions.set("credBlob", cbor::Value::boolean(true));
        if (ext.large_blob_key && info.has_extension("largeBlobKey"))
            extensions.set("largeBlobKey", cbor::Value::boolean(true));
        params.extensions = extensions;
        m_touch_again = token.has_value();
        if (m_touch_again && s.is_winner(m_slot.id))
            show(PromptState::TouchKeyAgain);
        auto const encoded = encode_get_assertion(params);
        auto const asked = Clock::now();
        auto const reply = call(ctap2::cmd_get_assertion, &encoded);
        if (reply.failure != HidFailure::None)
            return;
        Status const status = static_cast<Status>(reply.status);
        if (reply.ok()) {
            auto const first = parse_get_assertion_response(reply.body);
            if (!first) {
                fail_chosen(not_allowed(Reason::DeviceError));
                return;
            }
            if (!s.claim(m_slot.id))
                return;
            CredentialDescriptor const fallback = allow.empty() ? CredentialDescriptor {} : allow[0];
            if (!discoverable || first->number_of_credentials <= 1 || first->user_selected) {
                finish_get(*first, fallback, hmac, used_appid);
                return;
            }
            // Several accounts (§6.2.2 step 11, §6.3): collect them all,
            // then let the user choose.
            std::vector<GetAssertionResponse> all { *first };
            for (std::size_t i = 1; i < first->number_of_credentials && !stopped(); ++i) {
                auto const next = call(ctap2::cmd_get_next_assertion, nullptr);
                if (!next.ok())
                    break;
                auto const parsed = parse_get_assertion_response(next.body);
                if (!parsed)
                    break;
                all.push_back(*parsed);
            }
            std::vector<Account> accounts;
            for (auto const& response : all) {
                Account account;
                if (response.user) {
                    account.user_id = response.user->id;
                    account.name = response.user->name;
                    account.display_name = response.user->display_name;
                }
                accounts.push_back(std::move(account));
            }
            auto const choice = ask<std::size_t>([&](Prompt& p, auto callback) { p.ask_account(accounts, callback); });
            if (!choice)
                return;
            if (*choice >= all.size()) {
                s.fail(not_allowed(Reason::Cancelled));
                return;
            }
            // getNextAssertion answers under the parameters of the first
            // request, so every assertion's hmac-secret output is
            // encrypted under the same shared secret.
            finish_get(all[*choice], fallback, hmac, used_appid);
            return;
        }
        switch (status) {
        case Status::NoCredentials:
            if (select())
                fail_chosen(not_allowed(Reason::NotRegistered), PromptState::KeyNotRegistered);
            return;
        case Status::KeepaliveCancel:
            return;
        case Status::OperationDenied:
        case Status::UserActionTimeout:
        case Status::ActionTimeout:
            pause_if_quick(asked);
            continue;
        case Status::PuatRequired:
        case Status::PinAuthInvalid:
            if (retried_token)
                break;
            retried_token = true;
            if (!select())
                return;
            token = obtain_token(ctap2::permission_get_assertion);
            if (!token)
                return;
            continue;
        default:
            break;
        }
        if (s.is_winner(m_slot.id))
            fail_chosen(not_allowed(Reason::DeviceError, reply.status));
        return;
    }
}

void KeyWorker::u2f_create()
{
    ExtensionInputs const& ext = m_request.extensions;
    // CTAP 2.1 §10.2: ES256 only, no discoverable credential, no UV.
    bool const suitable = std::ranges::find(m_request.algorithms, cose::alg_es256) != m_request.algorithms.end()
        && m_request.resident_key != Requirement::Required && m_request.user_verification != Requirement::Required
        && !(ext.cred_protect && *ext.cred_protect > 1 && ext.enforce_cred_protect);
    if (!suitable) {
        if (select())
            fail_chosen(not_allowed(Reason::Unsuitable), PromptState::KeyUnsuitable);
        return;
    }
    auto const rp_hash = sha256_text(m_request.rp_id);
    if (!m_request.credentials.empty()) {
        auto found = u2f_probe(m_request.credentials, rp_hash);
        if (!found && ext.appid_exclude && !stopped())
            found = u2f_probe(m_request.credentials, sha256_text(*ext.appid_exclude));
        if (stopped())
            return;
        if (found) {
            if (select())
                fail_chosen(Error { ErrorKind::InvalidState, Reason::CredentialExcluded, 0 }, PromptState::KeyAlreadyRegistered);
            return;
        }
    }
    auto const request = u2f::register_request(m_request.client_data_hash, rp_hash);
    auto const answer = u2f::u2f_poll(*m_hid, request, s.deadline, &m_slot.cancel);
    if (!answer)
        return;
    if (answer->status != u2f::sw_no_error) {
        if (s.is_winner(m_slot.id))
            s.fail(not_allowed(Reason::DeviceError));
        return;
    }
    auto const registration = u2f::parse_registration(answer->data);
    if (!registration) {
        fail_chosen(not_allowed(Reason::DeviceError));
        return;
    }
    if (!s.claim(m_slot.id))
        return;
    auto const object = parse_attestation_object(u2f::registration_attestation_object(rp_hash, *registration));
    if (!object) {
        s.fail(not_allowed(Reason::DeviceError));
        return;
    }
    Result result;
    result.operation = Operation::Create;
    result.device_path = m_slot.device.path;
    auto const conveyed = apply_conveyance(*object, m_request.attestation);
    result.attestation_object = encode_attestation_object(conveyed.fmt, conveyed.att_stmt, conveyed.auth_data);
    result.created = created_credential_fields(conveyed, result.transports);
    result.credential_id = registration->key_handle;
    if (ext.cred_props)
        result.extensions.cred_props_rk = false;
    if (ext.appid_exclude)
        result.extensions.appid_exclude = true;
    if (ext.prf)
        result.extensions.prf_enabled = false;
    if (ext.hmac_create_secret)
        result.extensions.hmac_create_secret = false;
    s.finish(Outcome { std::move(result), std::nullopt });
}

void KeyWorker::u2f_get()
{
    ExtensionInputs const& ext = m_request.extensions;
    if (m_request.credentials.empty() || m_request.user_verification == Requirement::Required) {
        if (select())
            fail_chosen(not_allowed(Reason::Unsuitable), PromptState::KeyUnsuitable);
        return;
    }
    auto application = sha256_text(m_request.rp_id);
    bool used_appid = false;
    auto found = u2f_probe(m_request.credentials, application);
    if (!found && ext.appid && !stopped()) {
        auto const appid_hash = sha256_text(*ext.appid);
        found = u2f_probe(m_request.credentials, appid_hash);
        if (found) {
            application = appid_hash;
            used_appid = true;
        }
    }
    if (stopped())
        return;
    if (!found) {
        if (select())
            fail_chosen(not_allowed(Reason::NotRegistered), PromptState::KeyNotRegistered);
        return;
    }
    auto const request = u2f::authenticate_request(u2f::enforce_presence, m_request.client_data_hash, application, found->id);
    auto const answer = u2f::u2f_poll(*m_hid, *request, s.deadline, &m_slot.cancel);
    if (!answer)
        return;
    auto const authentication = answer->status == u2f::sw_no_error ? u2f::parse_authentication(answer->data) : std::nullopt;
    if (!authentication) {
        if (s.is_winner(m_slot.id))
            s.fail(not_allowed(Reason::DeviceError));
        return;
    }
    if (!s.claim(m_slot.id))
        return;
    Result result;
    result.operation = Operation::Get;
    result.device_path = m_slot.device.path;
    result.credential_id = found->id;
    result.authenticator_data = u2f::authentication_authenticator_data(application, *authentication);
    result.signature = authentication->signature;
    if (ext.appid)
        result.extensions.appid = used_appid;
    s.finish(Outcome { std::move(result), std::nullopt });
}

}

void Ceremony::State::run_key(Slot& slot)
{
    KeyWorker worker(*this, slot);
    worker.run();
    slot.finished = true;
    std::lock_guard lock(mutex);
    changed.notify_all();
}

void Ceremony::State::run()
{
    deadline = Clock::now() + request.timeout;
    int next_id = 0;
    bool first = true;
    bool look = true; // the transport says keys came or went
    Clock::time_point last_look;
    std::vector<DeviceInfo> present;
    while (true) {
        {
            std::unique_lock lock(mutex);
            if (outcome)
                break;
        }
        if (cancelled.load()) {
            fail(by_page.load() ? Error { ErrorKind::Abort, Reason::Aborted, 0 } : not_allowed(Reason::Cancelled));
            break;
        }
        if (Clock::now() >= deadline) {
            fail(not_allowed(Reason::Timeout));
            break;
        }
        // Keys that came: a worker each. A key whose worker ended (pulled
        // out, or not yet open to us) is tried again on the next look —
        // when the transport reports a change, and every second besides.
        if (look || Clock::now() - last_look >= std::chrono::seconds(1)) {
            present = transport->devices();
            last_look = Clock::now();
            std::lock_guard lock(mutex);
            for (auto const& device : present) {
                bool known = false;
                for (auto const& slot : slots)
                    known = known || (slot->device.path == device.path && !slot->finished.load());
                if (known || winner != -1)
                    continue;
                auto slot = std::make_unique<Slot>();
                slot->id = next_id++;
                slot->device = device;
                Slot* raw = slot.get();
                slots.push_back(std::move(slot));
                raw->thread = std::thread([self = shared_from_this(), raw] { self->run_key(*raw); });
            }
        }
        bool showing_insert = false;
        {
            std::lock_guard lock(prompt_mutex);
            showing_insert = shown == PromptState::InsertKey;
        }
        if (first || (!someone_won() && keys_present() == 0 && !showing_insert)) {
            if (present.empty() || keys_present() == 0) {
                PromptInfo info;
                info.state = PromptState::InsertKey;
                show(info);
            }
            first = false;
        }
        // Wait for keys to come or go, the outcome, a cancel or the end.
        auto const slice = std::min<Clock::duration>(deadline - Clock::now(), std::chrono::milliseconds(100));
        look = transport->wait_for_change(std::chrono::duration_cast<std::chrono::milliseconds>(slice) + std::chrono::milliseconds(1));
    }
    {
        std::lock_guard lock(mutex);
        for (auto& slot : slots)
            slot->cancel = true;
    }
    for (auto& slot : slots) {
        if (slot->thread.joinable())
            slot->thread.join();
    }
    Outcome result;
    {
        std::lock_guard lock(mutex);
        result = *outcome;
    }
    {
        std::lock_guard lock(prompt_mutex);
        if (prompt)
            prompt->close();
    }
    if (done)
        done(std::move(result));
    std::lock_guard lock(mutex);
    delivered = true;
    changed.notify_all();
}

Ceremony::Ceremony(Request request, std::shared_ptr<Transport> transport, std::shared_ptr<Prompt> prompt, Done done)
    : m_state(std::make_shared<State>())
{
    m_state->request = std::move(request);
    m_state->transport = std::move(transport);
    m_state->prompt = std::move(prompt);
    m_state->done = std::move(done);
}

Ceremony::~Ceremony()
{
    cancel(false);
    if (m_state->main.joinable()) {
        // Destroyed from the `done` callback, on the ceremony's own thread:
        // that thread is about to end and holds the state alive itself.
        if (m_state->main.get_id() == std::this_thread::get_id())
            m_state->main.detach();
        else
            m_state->main.join();
    }
}

void Ceremony::start()
{
    std::lock_guard lock(m_state->mutex);
    if (m_state->started)
        return;
    m_state->started = true;
    m_state->main = std::thread([state = m_state] { state->run(); });
}

void Ceremony::cancel(bool by_page)
{
    if (by_page)
        m_state->by_page = true;
    m_state->cancelled = true;
    std::lock_guard lock(m_state->mutex);
    for (auto& slot : m_state->slots)
        slot->cancel = true;
    m_state->changed.notify_all();
}

void Ceremony::wait()
{
    std::unique_lock lock(m_state->mutex);
    if (!m_state->started)
        return;
    m_state->changed.wait(lock, [&] { return m_state->delivered; });
}

}
