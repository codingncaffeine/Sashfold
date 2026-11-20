#include "auth/KeyManagement.h"

#include "auth/CtapHid.h"
#include "auth/U2f.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace sashfold::auth {

namespace {

using Clock = std::chrono::steady_clock;
using Status = ctap2::Status;
using Phase = KeyManagementState::Phase;
using Outcome = KeyManagementState::Outcome;
using Failure = KeyManagementState::Failure;

struct Command {
    enum class Kind : std::uint8_t { SetPin, ChangePin, Unlock, Remove, Refresh };
    Kind kind = Kind::Refresh;
    std::string pin; // SetPin: the new one; ChangePin, Unlock: the current one
    std::string new_pin;
    bool with_pin = false; // Unlock: a PIN was given (otherwise the key's own UV)
    CredentialDescriptor credential;
};

void forget(std::string& secret)
{
    std::fill(secret.begin(), secret.end(), '\0');
    secret.clear();
}

}

struct KeyManagement::State : std::enable_shared_from_this<KeyManagement::State> {
    std::shared_ptr<Transport> transport;
    std::function<void()> wake;

    mutable std::mutex mutex;
    std::condition_variable changed;
    KeyManagementState published;
    std::atomic<bool> cancelled { false };
    std::atomic<bool> done { false };

    struct Slot {
        int id = 0;
        DeviceInfo device;
        CancelFlag cancel { false };
        std::atomic<bool> finished { false };
        std::thread thread;
    };
    std::vector<std::unique_ptr<Slot>> slots; // under `mutex`
    int winner = -1;
    std::deque<Command> commands;
    std::thread main;
    bool started = false;

    void run();
    void run_key(Slot& slot);

    // A change of the published state, then a word to the caller.
    template <class Change>
    void publish(Change const& change)
    {
        std::function<void()> call;
        {
            std::lock_guard lock(mutex);
            change(published);
            ++published.generation;
            call = wake;
        }
        if (call)
            call();
    }

    bool ended() const
    {
        std::lock_guard lock(mutex);
        return published.phase == Phase::Ended;
    }

    // The end, with a reason; the first one stays.
    void end(Failure failure, std::uint8_t status = 0)
    {
        publish([&](KeyManagementState& s) {
            if (s.phase == Phase::Ended)
                return;
            s.phase = Phase::Ended;
            s.failure = failure;
            s.ctap_status = status;
        });
        std::lock_guard lock(mutex);
        for (auto& slot : slots)
            slot->cancel = true;
        changed.notify_all();
    }

    bool claim(int id)
    {
        std::lock_guard lock(mutex);
        if (published.phase == Phase::Ended || cancelled.load())
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

    std::size_t live_slots() const
    {
        return static_cast<std::size_t>(std::ranges::count_if(slots, [](auto const& s) { return !s->finished.load(); }));
    }
};

namespace {

// One key: identified, touched to be chosen, then the commands.
class KeyWorker {
public:
    KeyWorker(KeyManagement::State& state, KeyManagement::State::Slot& slot)
        : s(state)
        , m_slot(slot)
    {
    }

    void run()
    {
        auto connection = s.transport->open(m_slot.device);
        if (!connection)
            return;
        m_hid = std::make_unique<CtapHidDevice>(std::move(connection), m_slot.device);
        if (!m_hid->init(Clock::now() + std::chrono::seconds(5), &m_slot.cancel))
            return;
        if (m_hid->has_cbor()) {
            Ctap2Device device(*m_hid);
            m_info = device.get_info(Clock::now() + KeyManagement::command_time, &m_slot.cancel);
            if (stopped() || (!m_info && !m_hid->has_msg()))
                return;
        } else if (!m_hid->has_msg()) {
            return;
        }
        if (!select())
            return;
        s.publish([&](KeyManagementState& state) {
            state.phase = Phase::Ready;
            state.key = describe();
        });
        serve();
        forget_token();
        if (m_hid->gone())
            s.end(Failure::KeyGone);
    }

private:
    bool stopped() const { return m_slot.cancel.load() || s.cancelled.load(); }

    Ctap2Reply call(std::uint8_t command, cbor::Value const* params)
    {
        Ctap2Device device(*m_hid);
        return device.call(command, params, Clock::now() + KeyManagement::command_time, &m_slot.cancel);
    }

    client_pin::Call caller()
    {
        return [this](std::uint8_t command, cbor::Value const* params) { return call(command, params); };
    }

    ManagedKey describe() const
    {
        ManagedKey key;
        key.device = m_slot.device.product;
        key.ctap2 = m_info.has_value();
        if (!m_info)
            return key;
        key.pin_supported = m_info->options.contains("clientPin");
        key.pin_set = m_info->option_true("clientPin");
        key.force_pin_change = m_info->force_pin_change;
        key.min_pin_length = std::max<std::size_t>(4, m_info->min_pin_length.value_or(4));
        key.built_in_uv = m_info->option_true("uv") && m_info->is_ctap21() && m_info->option_true("pinUvAuthToken");
        key.credential_management = cred_mgmt::support_of(*m_info);
        return key;
    }

    // ----- Touch to choose -----

    // Waits for a touch on this key without making anything a site could
    // use, as the ceremonies do (auth/Client.cpp), asking again each time
    // the key gives up waiting. True when this key is the one chosen.
    bool select()
    {
        bool selection = m_info && m_info->is_ctap21();
        while (!stopped()) {
            auto const asked = Clock::now();
            if (!m_info) {
                auto const request = u2f::register_request(u2f::bogus_challenge(), u2f::bogus_application());
                auto const answer = u2f::u2f_poll(*m_hid, request, Clock::now() + KeyManagement::command_time, &m_slot.cancel);
                if (answer && answer->status == u2f::sw_no_error)
                    return s.claim(m_slot.id);
                if (m_hid->gone() || stopped())
                    return false;
                pause_after(asked);
                continue;
            }
            Ctap2Reply reply;
            if (selection) {
                reply = call(ctap2::cmd_selection, nullptr);
                if (reply.ok())
                    return s.claim(m_slot.id);
                // A pre-release key without the command: the CTAP 2.0 way.
                if (reply.failure == HidFailure::None && reply.status == std::uint8_t(Status::InvalidCommand)) {
                    selection = false;
                    continue;
                }
            } else if (m_info->options.contains("clientPin")) {
                // A zero-length pinUvAuthParam waits for a touch and answers
                // PIN_NOT_SET or PIN_INVALID (CTAP 2.1 §6.1.2 step 1).
                MakeCredentialParams params = bogus_make_credential();
                params.pin_uv_auth_param = std::vector<std::uint8_t> {};
                params.pin_uv_auth_protocol = 1;
                auto const encoded = encode_make_credential(params);
                reply = call(ctap2::cmd_make_credential, &encoded);
                if (reply.failure == HidFailure::None
                    && (reply.status == std::uint8_t(Status::PinNotSet) || reply.status == std::uint8_t(Status::PinInvalid)))
                    return s.claim(m_slot.id);
            } else {
                auto const encoded = encode_make_credential(bogus_make_credential());
                reply = call(ctap2::cmd_make_credential, &encoded);
                if (reply.ok())
                    return s.claim(m_slot.id);
            }
            if (reply.failure != HidFailure::None && reply.failure != HidFailure::Timeout)
                return false;
            if (reply.failure == HidFailure::None && reply.status != std::uint8_t(Status::OperationDenied)
                && reply.status != std::uint8_t(Status::UserActionTimeout) && reply.status != std::uint8_t(Status::ActionTimeout))
                return false;
            pause_after(asked);
        }
        return false;
    }

    // A key that refuses at once is not asked in a tight loop.
    void pause_after(Clock::time_point asked) const
    {
        while (Clock::now() < asked + std::chrono::milliseconds(250) && !stopped())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    static MakeCredentialParams bogus_make_credential()
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

    // ----- Commands -----

    void serve()
    {
        while (true) {
            Command command;
            {
                std::unique_lock lock(s.mutex);
                s.changed.wait(lock, [&] { return !s.commands.empty() || m_slot.cancel.load() || s.cancelled.load(); });
                if (m_slot.cancel.load() || s.cancelled.load())
                    return;
                command = std::move(s.commands.front());
                s.commands.pop_front();
            }
            s.publish([](KeyManagementState& state) {
                state.phase = Phase::Busy;
                state.outcome = Outcome::None;
                state.retries = -1;
            });
            bool const go_on = carry_out(command);
            forget(command.pin);
            forget(command.new_pin);
            if (!go_on)
                return;
            s.publish([&](KeyManagementState& state) {
                if (state.phase != Phase::Ended)
                    state.phase = Phase::Ready;
                state.key = describe();
                ++state.commands_done;
            });
        }
    }

    void outcome(Outcome what, int retries = -1)
    {
        s.publish([&](KeyManagementState& state) {
            state.outcome = what;
            state.retries = retries;
        });
    }

    // A failure no command can go on from. False: the worker stops.
    bool failed(client_pin::Answer const& answer)
    {
        if (answer.failure == HidFailure::Cancelled || stopped())
            return false;
        if (answer.failure == HidFailure::Gone || m_hid->gone()) {
            s.end(Failure::KeyGone);
            return false;
        }
        if (answer.failure == HidFailure::None && answer.status == std::uint8_t(Status::PinBlocked)) {
            s.end(Failure::PinBlocked);
            return false;
        }
        if (answer.failure == HidFailure::None && answer.status == std::uint8_t(Status::PinAuthBlocked)) {
            s.end(Failure::PinAuthBlocked);
            return false;
        }
        s.end(Failure::DeviceError, answer.failure == HidFailure::None ? answer.status : 0);
        return false;
    }

    // The key's word on a wrong PIN: the tries left, or the end.
    bool wrong_pin(pin::Protocol protocol)
    {
        auto const left = client_pin::retries(caller(), protocol, ctap2::pin_get_retries);
        if (left && left->left && *left->left == 0) {
            s.end(Failure::PinBlocked);
            return false;
        }
        outcome(Outcome::PinWrong, left && left->left ? static_cast<int>(*left->left) : -1);
        return true;
    }

    void forget_token()
    {
        std::fill(m_token.token.begin(), m_token.token.end(), std::uint8_t { 0 });
        m_token.token.clear();
        m_has_token = false;
    }

    void lock_list(bool clear)
    {
        forget_token();
        s.publish([&](KeyManagementState& state) {
            state.unlocked = false;
            if (clear) {
                state.passkeys.clear();
                state.metadata.reset();
            }
        });
    }

    void reread_info()
    {
        Ctap2Device device(*m_hid);
        if (auto info = device.get_info(Clock::now() + KeyManagement::command_time, &m_slot.cancel))
            m_info = std::move(info);
    }

    bool carry_out(Command const& command)
    {
        if (!m_info) {
            outcome(Outcome::NotSupported);
            return true;
        }
        auto const protocol = client_pin::preferred_protocol(*m_info);
        switch (command.kind) {
        case Command::Kind::SetPin: {
            if (!protocol || !m_info->options.contains("clientPin") || m_info->option_true("clientPin")) {
                outcome(Outcome::NotSupported);
                return true;
            }
            auto const answer = client_pin::set_pin(caller(), *protocol, command.pin);
            if (answer.failure == HidFailure::None && answer.status == std::uint8_t(Status::PinPolicyViolation)) {
                outcome(Outcome::PinPolicy);
                return true;
            }
            if (!answer.ok())
                return failed(answer);
            // A new PIN ends every token the key gave (§6.5.5.5).
            lock_list(true);
            reread_info();
            m_info->options["clientPin"] = true;
            outcome(Outcome::PinSet);
            return true;
        }
        case Command::Kind::ChangePin: {
            if (!protocol || !m_info->option_true("clientPin")) {
                outcome(Outcome::NotSupported);
                return true;
            }
            auto const answer = client_pin::change_pin(caller(), *protocol, command.pin, command.new_pin);
            if (answer.failure == HidFailure::None && answer.status == std::uint8_t(Status::PinInvalid))
                return wrong_pin(*protocol);
            if (answer.failure == HidFailure::None && answer.status == std::uint8_t(Status::PinPolicyViolation)) {
                outcome(Outcome::PinPolicy);
                return true;
            }
            if (!answer.ok())
                return failed(answer);
            lock_list(true);
            reread_info();
            m_info->force_pin_change = false;
            outcome(Outcome::PinChanged);
            return true;
        }
        case Command::Kind::Unlock: return unlock(command, protocol);
        case Command::Kind::Remove: {
            if (!m_has_token) {
                outcome(Outcome::TokenExpired);
                return true;
            }
            CredentialManager manager(caller(), cred_mgmt::support_of(*m_info), m_token);
            auto const answer = manager.remove(command.credential);
            if (answer.failure == HidFailure::None && answer.status == std::uint8_t(Status::NoCredentials)) {
                if (!read_list())
                    return false;
                outcome(Outcome::NotFound);
                return true;
            }
            if (expired(answer))
                return true;
            if (!answer.ok())
                return failed(answer);
            if (!read_list())
                return false;
            outcome(Outcome::Deleted);
            return true;
        }
        case Command::Kind::Refresh:
            if (!m_has_token) {
                outcome(Outcome::TokenExpired);
                return true;
            }
            return read_list();
        }
        return true;
    }

    // The key no longer takes the token (its usage period is over, §6.5.2.1):
    // the list stays shown, locked, and the PIN is asked again.
    bool expired(client_pin::Answer const& answer)
    {
        if (answer.failure != HidFailure::None || answer.status != std::uint8_t(Status::PinAuthInvalid))
            return false;
        lock_list(false);
        outcome(Outcome::TokenExpired);
        return true;
    }

    bool unlock(Command const& command, std::optional<pin::Protocol> protocol)
    {
        cred_mgmt::Support const support = cred_mgmt::support_of(*m_info);
        if (support == cred_mgmt::Support::None || !protocol) {
            outcome(Outcome::NotSupported);
            return true;
        }
        client_pin::TokenAnswer asked;
        if (command.with_pin) {
            if (!m_info->option_true("clientPin")) {
                outcome(Outcome::NotSupported);
                return true;
            }
            asked = client_pin::token_using_pin(caller(), *m_info, *protocol, command.pin, ctap2::permission_credential_management, std::nullopt);
            auto const& answer = asked.answer;
            if (answer.failure == HidFailure::None && answer.status == std::uint8_t(Status::PinInvalid))
                return wrong_pin(*protocol);
            // A key that insists on a new PIN gives no token for the old one
            // (§6.5.5.7.2 with forcePINChange).
            if (answer.failure == HidFailure::None && answer.status == std::uint8_t(Status::PinPolicyViolation)) {
                m_info->force_pin_change = true;
                outcome(Outcome::PinNeedsChange);
                return true;
            }
        } else {
            if (!describe().built_in_uv) {
                outcome(Outcome::NotSupported);
                return true;
            }
            s.publish([](KeyManagementState& state) { state.phase = Phase::VerifyFingerprint; });
            asked = client_pin::token_using_uv(caller(), *protocol, ctap2::permission_credential_management, std::nullopt);
            auto const& answer = asked.answer;
            if (answer.failure == HidFailure::None && answer.status == std::uint8_t(Status::UvInvalid)) {
                auto const left = client_pin::retries(caller(), *protocol, ctap2::pin_get_uv_retries);
                outcome(Outcome::UvWrong, left && left->left ? static_cast<int>(*left->left) : -1);
                return true;
            }
            if (answer.failure == HidFailure::None
                && (answer.status == std::uint8_t(Status::UvBlocked) || answer.status == std::uint8_t(Status::UserActionTimeout))) {
                outcome(answer.status == std::uint8_t(Status::UvBlocked) ? Outcome::UvBlocked : Outcome::UvWrong);
                return true;
            }
        }
        if (!asked.token)
            return failed(asked.answer);
        forget_token();
        m_token.protocol = asked.token->protocol;
        m_token.token.swap(asked.token->token);
        m_has_token = true;
        if (!read_list())
            return false;
        outcome(Outcome::Unlocked);
        return true;
    }

    // getCredsMetadata and the list, with the token held.
    bool read_list()
    {
        CredentialManager manager(caller(), cred_mgmt::support_of(*m_info), m_token);
        CredentialsMetadata metadata;
        auto answer = manager.metadata(metadata);
        std::vector<Passkey> passkeys;
        if (answer.ok())
            answer = manager.passkeys(passkeys);
        if (expired(answer))
            return true;
        if (!answer.ok())
            return failed(answer);
        s.publish([&](KeyManagementState& state) {
            state.unlocked = true;
            state.metadata = metadata;
            state.passkeys = std::move(passkeys);
        });
        return true;
    }

    KeyManagement::State& s;
    KeyManagement::State::Slot& m_slot;
    std::unique_ptr<CtapHidDevice> m_hid;
    std::optional<AuthenticatorInfo> m_info;
    client_pin::Token m_token;
    bool m_has_token = false;
};

}

void KeyManagement::State::run_key(Slot& slot)
{
    {
        KeyWorker worker(*this, slot);
        worker.run();
    }
    slot.finished = true;
    std::function<void()> call;
    {
        std::lock_guard lock(mutex);
        call = wake;
        changed.notify_all();
    }
    // The watcher looks again: the count of keys present moved.
    if (call)
        call();
}

void KeyManagement::State::run()
{
    int next_id = 0;
    bool look = true;
    Clock::time_point last_look;
    while (!cancelled.load() && !ended()) {
        if (look || Clock::now() - last_look >= std::chrono::seconds(1)) {
            std::vector<DeviceInfo> const present = transport->devices();
            last_look = Clock::now();
            bool chosen_gone = false;
            {
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
                // The chosen key pulled out while it waits for the reader:
                // its thread sees nothing until its next command.
                if (winner != -1) {
                    for (auto const& slot : slots) {
                        if (slot->id == winner)
                            chosen_gone = std::ranges::none_of(present, [&](DeviceInfo const& d) { return d.path == slot->device.path; });
                    }
                }
            }
            if (chosen_gone) {
                end(Failure::KeyGone);
                break;
            }
        }
        // Until a key is chosen: insert one, or touch one.
        bool someone = false;
        std::size_t live = 0;
        {
            std::lock_guard lock(mutex);
            someone = winner != -1;
            live = live_slots();
        }
        if (!someone) {
            Phase const phase = live == 0 ? Phase::InsertKey : Phase::TouchKey;
            bool moved = false;
            {
                std::lock_guard lock(mutex);
                moved = (published.phase == Phase::InsertKey || published.phase == Phase::TouchKey)
                    && (published.phase != phase || published.keys_present != live || published.generation == 0);
            }
            if (moved) {
                publish([&](KeyManagementState& state) {
                    state.phase = phase;
                    state.keys_present = live;
                });
            }
        }
        look = transport->wait_for_change(std::chrono::milliseconds(100));
    }
    if (cancelled.load())
        end(Failure::Cancelled);
    {
        std::lock_guard lock(mutex);
        for (auto& slot : slots)
            slot->cancel = true;
        changed.notify_all();
    }
    for (auto& slot : slots) {
        if (slot->thread.joinable())
            slot->thread.join();
    }
    done = true;
    std::function<void()> call;
    {
        std::lock_guard lock(mutex);
        call = wake;
    }
    if (call)
        call();
}

KeyManagement::KeyManagement(std::shared_ptr<Transport> transport, std::function<void()> wake)
    : m_state(std::make_shared<State>())
{
    m_state->transport = std::move(transport);
    m_state->wake = std::move(wake);
}

KeyManagement::~KeyManagement()
{
    cancel();
    if (m_state->main.joinable())
        m_state->main.join();
}

void KeyManagement::start()
{
    std::lock_guard lock(m_state->mutex);
    if (m_state->started)
        return;
    m_state->started = true;
    m_state->main = std::thread([self = m_state] { self->run(); });
}

KeyManagementState KeyManagement::state() const
{
    std::lock_guard lock(m_state->mutex);
    return m_state->published;
}

namespace {

void enqueue(KeyManagement::State& state, Command command)
{
    std::lock_guard lock(state.mutex);
    if (state.winner == -1 || state.published.phase == KeyManagementState::Phase::Ended)
        return;
    state.commands.push_back(std::move(command));
    state.changed.notify_all();
}

}

void KeyManagement::set_pin(std::string new_pin)
{
    Command command;
    command.kind = Command::Kind::SetPin;
    command.pin = std::move(new_pin);
    enqueue(*m_state, std::move(command));
}

void KeyManagement::change_pin(std::string current, std::string new_pin)
{
    Command command;
    command.kind = Command::Kind::ChangePin;
    command.pin = std::move(current);
    command.new_pin = std::move(new_pin);
    enqueue(*m_state, std::move(command));
}

void KeyManagement::unlock(std::optional<std::string> pin)
{
    Command command;
    command.kind = Command::Kind::Unlock;
    command.with_pin = pin.has_value();
    if (pin)
        command.pin = std::move(*pin);
    enqueue(*m_state, std::move(command));
}

void KeyManagement::remove(CredentialDescriptor credential)
{
    Command command;
    command.kind = Command::Kind::Remove;
    command.credential = std::move(credential);
    enqueue(*m_state, std::move(command));
}

void KeyManagement::refresh()
{
    Command command;
    command.kind = Command::Kind::Refresh;
    enqueue(*m_state, std::move(command));
}

void KeyManagement::cancel()
{
    m_state->cancelled = true;
    std::lock_guard lock(m_state->mutex);
    for (auto& slot : m_state->slots)
        slot->cancel = true;
    m_state->changed.notify_all();
}

bool KeyManagement::finished() const
{
    std::lock_guard lock(m_state->mutex);
    return !m_state->started || m_state->done.load();
}

}
