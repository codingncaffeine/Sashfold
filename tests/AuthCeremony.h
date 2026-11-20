#pragma once
// Runs auth::Ceremony against software keys, with a scripted prompt: the
// PINs the user types, the account they pick, and a record of what was
// shown. Shared by the ceremony tests and the comparison tool.
#include "SoftAuthenticator.h"

#include "auth/Client.h"
#include "crypto/Sha2.h"

#include <deque>
#include <mutex>
#include <vector>

namespace sashfold::test {

class ScriptedPrompt final : public auth::Prompt {
public:
    void show(auth::PromptInfo const& info) override
    {
        std::lock_guard lock(m_mutex);
        states.push_back(info.state);
        infos.push_back(info);
    }
    void ask_pin(auth::PinQuestion const& question, std::function<void(std::optional<auth::PinAnswer>)> answer) override
    {
        std::optional<auth::PinAnswer> next;
        {
            std::lock_guard lock(m_mutex);
            questions.push_back(question);
            if (!pins.empty()) {
                next = pins.front();
                pins.pop_front();
            }
        }
        answer(next);
    }
    void ask_account(std::vector<auth::Account> const& offered, std::function<void(std::optional<std::size_t>)> answer) override
    {
        {
            std::lock_guard lock(m_mutex);
            accounts = offered;
        }
        answer(account_choice);
    }
    void close() override
    {
        std::lock_guard lock(m_mutex);
        closed = true;
    }

    bool saw(auth::PromptState state)
    {
        std::lock_guard lock(m_mutex);
        return std::ranges::find(states, state) != states.end();
    }

    std::mutex m_mutex;
    std::vector<auth::PromptState> states;
    std::vector<auth::PromptInfo> infos;
    std::vector<auth::PinQuestion> questions;
    std::deque<std::optional<auth::PinAnswer>> pins;
    std::vector<auth::Account> accounts;
    std::optional<std::size_t> account_choice { 0 };
    bool closed = false;
};

inline auth::PinAnswer pin_answer(std::string pin, std::string new_pin = {}) { return auth::PinAnswer { std::move(pin), std::move(new_pin) }; }

inline std::array<std::uint8_t, 32> client_data_hash_of(std::string_view json)
{
    return crypto::Sha256::hash(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(json.data()), json.size()));
}

inline auth::Request create_request(std::string rp_id = "example.org")
{
    auth::Request request;
    request.operation = auth::Operation::Create;
    request.rp_id = std::move(rp_id);
    request.rp_name = "Example";
    request.client_data_hash = client_data_hash_of("{\"type\":\"webauthn.create\",\"challenge\":\"AAAA\",\"origin\":\"https://example.org\"}");
    request.user.id = { 1, 2, 3, 4 };
    request.user.name = "alice";
    request.user.display_name = "Alice";
    request.user.has_name = true;
    request.user.has_display_name = true;
    request.algorithms = { auth::cose::alg_es256, auth::cose::alg_eddsa };
    request.resident_key = auth::Requirement::Discouraged;
    request.user_verification = auth::Requirement::Discouraged;
    request.attestation = auth::AttestationConveyance::Direct;
    request.timeout = std::chrono::milliseconds(5000);
    return request;
}

inline auth::Request get_request(std::vector<std::vector<std::uint8_t>> const& allow, std::string rp_id = "example.org")
{
    auth::Request request;
    request.operation = auth::Operation::Get;
    request.rp_id = std::move(rp_id);
    request.client_data_hash = client_data_hash_of("{\"type\":\"webauthn.get\",\"challenge\":\"BBBB\",\"origin\":\"https://example.org\"}");
    for (auto const& id : allow)
        request.credentials.push_back({ id, { "usb" } });
    request.user_verification = auth::Requirement::Discouraged;
    request.timeout = std::chrono::milliseconds(5000);
    return request;
}

// One ceremony to its end.
inline auth::Outcome run_ceremony(std::shared_ptr<auth::Transport> transport, auth::Request request, std::shared_ptr<ScriptedPrompt> prompt,
    std::function<void(auth::Ceremony&)> while_running = {})
{
    std::mutex mutex;
    std::optional<auth::Outcome> outcome;
    auth::Ceremony ceremony(std::move(request), std::move(transport), prompt, [&](auth::Outcome result) {
        std::lock_guard lock(mutex);
        outcome = std::move(result);
    });
    ceremony.start();
    if (while_running)
        while_running(ceremony);
    ceremony.wait();
    std::lock_guard lock(mutex);
    return *outcome;
}

}
