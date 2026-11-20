#include "ui/SecurityKeys.h"

#include "auth/Attestation.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace sashfold::ui {

namespace {

// Where there is no transport: no key is ever present.
class NoKeys final : public auth::Transport {
public:
    std::vector<auth::DeviceInfo> devices() override { return {}; }
    std::unique_ptr<auth::HidConnection> open(auth::DeviceInfo const&) override { return nullptr; }
    bool wait_for_change(std::chrono::milliseconds timeout) override
    {
        std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(100)));
        return false;
    }
};

// The ceremony's prompt: each call handed to the requests, which keep it
// for the window to draw and answer. It holds them weakly: a ceremony that
// outlives them (the window closing) is answered "no" and ends.
class SheetPrompt final : public auth::Prompt {
public:
    SheetPrompt(std::weak_ptr<SecurityKeyRequests> requests, std::uint64_t id)
        : m_requests(std::move(requests))
        , m_id(id)
    {
    }
    void show(auth::PromptInfo const& info) override
    {
        if (auto requests = m_requests.lock())
            requests->shown(m_id, info);
    }
    void ask_pin(auth::PinQuestion const& question, std::function<void(std::optional<auth::PinAnswer>)> answer) override
    {
        if (auto requests = m_requests.lock())
            requests->asked_pin(m_id, question, std::move(answer));
        else
            answer(std::nullopt);
    }
    void ask_account(std::vector<auth::Account> const& accounts, std::function<void(std::optional<std::size_t>)> answer) override
    {
        if (auto requests = m_requests.lock())
            requests->asked_account(m_id, accounts, std::move(answer));
        else
            answer(std::nullopt);
    }
    void close() override { }

private:
    std::weak_ptr<SecurityKeyRequests> m_requests;
    std::uint64_t m_id;
};

KeySheetState::View view_of(auth::PromptState state)
{
    using View = KeySheetState::View;
    switch (state) {
    case auth::PromptState::InsertKey: return View::InsertKey;
    case auth::PromptState::TouchKey: return View::TouchKey;
    case auth::PromptState::TouchKeyAgain: return View::TouchKeyAgain;
    case auth::PromptState::Processing: return View::Processing;
    case auth::PromptState::VerifyFingerprint: return View::VerifyFingerprint;
    case auth::PromptState::UvRetry: return View::FingerprintRetry;
    case auth::PromptState::KeyNotRegistered: return View::NotRegistered;
    case auth::PromptState::KeyAlreadyRegistered: return View::AlreadyRegistered;
    case auth::PromptState::KeyUnsuitable: return View::Unsuitable;
    case auth::PromptState::PinBlocked: return View::PinBlocked;
    case auth::PromptState::PinAuthBlocked: return View::PinAuthBlocked;
    case auth::PromptState::UvBlocked: return View::UvBlocked;
    }
    return View::TouchKey;
}

// The ended states the reader is shown until they close the sheet. The
// reader's own Cancel, the page's abort and the timeout are not held: the
// first two are the page's to hear at once, and the last is the page's.
std::optional<KeySheetState::View> held_view(auth::Reason reason)
{
    using View = KeySheetState::View;
    switch (reason) {
    case auth::Reason::CredentialExcluded: return View::AlreadyRegistered;
    case auth::Reason::NotRegistered: return View::NotRegistered;
    case auth::Reason::Unsuitable: return View::Unsuitable;
    case auth::Reason::PinBlocked: return View::PinBlocked;
    case auth::Reason::PinAuthBlocked: return View::PinAuthBlocked;
    case auth::Reason::UvBlocked: return View::UvBlocked;
    case auth::Reason::PinPolicy:
    case auth::Reason::DeviceGone:
    case auth::Reason::DeviceError: return View::Failed;
    case auth::Reason::Timeout:
    case auth::Reason::Cancelled:
    case auth::Reason::Aborted: return std::nullopt;
    }
    return std::nullopt;
}

}

char const* view_name(KeySheetState const& state)
{
    using View = KeySheetState::View;
    switch (state.view) {
    case View::InsertKey: return "insert-key";
    case View::TouchKey: return "touch-key";
    case View::TouchKeyAgain: return "touch-again";
    case View::Processing: return "processing";
    case View::VerifyFingerprint: return "fingerprint";
    case View::FingerprintRetry: return "fingerprint-retry";
    case View::Pin:
        if (!state.pin)
            return "pin-enter";
        switch (state.pin->kind) {
        case auth::PinQuestion::Kind::Enter: return "pin-enter";
        case auth::PinQuestion::Kind::Retry: return "pin-retry";
        case auth::PinQuestion::Kind::Set: return "pin-set";
        case auth::PinQuestion::Kind::Change: return "pin-change";
        }
        return "pin-enter";
    case View::Account: return "account";
    case View::AlreadyRegistered: return "already-registered";
    case View::NotRegistered: return "not-registered";
    case View::Unsuitable: return "unsuitable";
    case View::PinBlocked: return "pin-blocked";
    case View::PinAuthBlocked: return "pin-auth-blocked";
    case View::UvBlocked: return "uv-blocked";
    case View::Failed: return "failed";
    case View::Consent: return "consent";
    }
    return "";
}

SecurityKeyRequests::~SecurityKeyRequests()
{
    std::map<std::uint64_t, Entry> entries;
    {
        std::lock_guard lock(m_mutex);
        entries.swap(m_entries);
    }
    // The window is closing: each question waiting is answered "no", each
    // ceremony cancelled and its threads waited for. Nothing is told to
    // pages, which are ending too.
    for (auto& [id, entry] : entries) {
        if (entry.pin_answer)
            entry.pin_answer(std::nullopt);
        if (entry.account_answer)
            entry.account_answer(std::nullopt);
        if (entry.ceremony)
            entry.ceremony->cancel(false);
    }
    entries.clear();
}

void SecurityKeyRequests::set_transport(std::shared_ptr<auth::Transport> transport)
{
    std::lock_guard lock(m_mutex);
    m_transport = std::move(transport);
}

std::shared_ptr<auth::Transport> SecurityKeyRequests::transport() const
{
    std::lock_guard lock(m_mutex);
    return m_transport;
}

void SecurityKeyRequests::set_wake(std::function<void()> wake)
{
    std::lock_guard lock(m_mutex);
    m_wake = std::move(wake);
}

void SecurityKeyRequests::changed()
{
    std::function<void()> wake;
    {
        std::lock_guard lock(m_mutex);
        ++m_generation;
        wake = m_wake;
    }
    if (wake)
        wake();
}

std::uint64_t SecurityKeyRequests::start(Owner owner, bindings::HostHooks::SecurityKeyAsk const& ask, auth::Request const& request,
    std::function<void(auth::Outcome)> done)
{
    std::uint64_t id = 0;
    {
        std::lock_guard lock(m_mutex);
        id = m_next++;
        Entry& entry = m_entries[id];
        entry.owner = owner;
        entry.attestation = request.operation == auth::Operation::Create ? request.attestation : auth::AttestationConveyance::None;
        entry.done = std::move(done);
        entry.state.id = id;
        entry.state.site = ask.site;
        entry.state.create = request.operation == auth::Operation::Create;
        entry.state.view = KeySheetState::View::InsertKey;
        std::shared_ptr<auth::Transport> transport = m_transport ? m_transport : std::make_shared<NoKeys>();
        std::weak_ptr<SecurityKeyRequests> const self = weak_from_this();
        entry.ceremony = std::make_unique<auth::Ceremony>(request, std::move(transport), std::make_shared<SheetPrompt>(self, id),
            [self, id](auth::Outcome outcome) {
                if (auto requests = self.lock())
                    requests->finished(id, std::move(outcome));
            });
        // Started under the lock: its first report waits for the entry.
        entry.ceremony->start();
    }
    changed();
    return id;
}

void SecurityKeyRequests::cancel(std::uint64_t id, bool by_page)
{
    bool held = false;
    std::function<void(std::optional<auth::PinAnswer>)> pin;
    std::function<void(std::optional<std::size_t>)> account;
    {
        std::lock_guard lock(m_mutex);
        auto const found = m_entries.find(id);
        if (found == m_entries.end())
            return;
        Entry& entry = found->second;
        entry.cancelled = true;
        held = entry.held.has_value();
        pin = std::exchange(entry.pin_answer, nullptr);
        account = std::exchange(entry.account_answer, nullptr);
        if (!held && entry.ceremony)
            entry.ceremony->cancel(by_page);
    }
    if (pin)
        pin(std::nullopt);
    if (account)
        account(std::nullopt);
    // A sheet held for the reader goes: the page has its answer already.
    if (held)
        deliver(id, std::nullopt);
    else
        changed();
}

void SecurityKeyRequests::cancel_owner(Owner owner)
{
    std::vector<std::uint64_t> ids;
    {
        std::lock_guard lock(m_mutex);
        for (auto const& [id, entry] : m_entries) {
            if (entry.owner == owner)
                ids.push_back(id);
        }
    }
    for (std::uint64_t const id : ids)
        cancel(id, false);
}

std::optional<KeySheetState> SecurityKeyRequests::state_for(Owner owner) const
{
    std::lock_guard lock(m_mutex);
    for (auto const& [id, entry] : m_entries) {
        if (entry.owner == owner && !entry.cancelled)
            return entry.state;
    }
    return std::nullopt;
}

bool SecurityKeyRequests::has_request(Owner owner) const { return state_for(owner).has_value(); }

std::size_t SecurityKeyRequests::active() const
{
    std::lock_guard lock(m_mutex);
    return m_entries.size();
}

std::uint64_t SecurityKeyRequests::generation() const
{
    std::lock_guard lock(m_mutex);
    return m_generation;
}

void SecurityKeyRequests::shown(std::uint64_t id, auth::PromptInfo const& info)
{
    {
        std::lock_guard lock(m_mutex);
        auto const found = m_entries.find(id);
        if (found == m_entries.end() || found->second.held)
            return;
        KeySheetState& state = found->second.state;
        state.view = view_of(info.state);
        // A state the ceremony ends in is shown just before it ends: the
        // sheet offers Close from then on.
        switch (info.state) {
        case auth::PromptState::KeyNotRegistered:
        case auth::PromptState::KeyAlreadyRegistered:
        case auth::PromptState::KeyUnsuitable:
        case auth::PromptState::PinBlocked:
        case auth::PromptState::PinAuthBlocked: state.ended = true; break;
        default: break;
        }
        state.retries = info.retries;
        state.keys_present = info.keys_present;
        state.pin.reset();
        state.accounts.clear();
    }
    changed();
}

void SecurityKeyRequests::asked_pin(std::uint64_t id, auth::PinQuestion const& question, std::function<void(std::optional<auth::PinAnswer>)> answer)
{
    {
        std::unique_lock lock(m_mutex);
        auto const found = m_entries.find(id);
        if (found == m_entries.end() || found->second.cancelled) {
            lock.unlock();
            answer(std::nullopt);
            return;
        }
        Entry& entry = found->second;
        entry.pin_answer = std::move(answer);
        entry.state.view = KeySheetState::View::Pin;
        entry.state.pin = question;
        entry.state.retries = question.retries;
        entry.state.accounts.clear();
        ++entry.state.question;
    }
    changed();
}

void SecurityKeyRequests::asked_account(std::uint64_t id, std::vector<auth::Account> const& accounts,
    std::function<void(std::optional<std::size_t>)> answer)
{
    {
        std::unique_lock lock(m_mutex);
        auto const found = m_entries.find(id);
        if (found == m_entries.end() || found->second.cancelled) {
            lock.unlock();
            answer(std::nullopt);
            return;
        }
        Entry& entry = found->second;
        entry.account_answer = std::move(answer);
        entry.state.view = KeySheetState::View::Account;
        entry.state.accounts = accounts;
        entry.state.pin.reset();
        ++entry.state.question;
    }
    changed();
}

void SecurityKeyRequests::finished(std::uint64_t id, auth::Outcome outcome)
{
    bool hold = false;
    {
        std::lock_guard lock(m_mutex);
        auto const found = m_entries.find(id);
        if (found == m_entries.end())
            return;
        Entry& entry = found->second;
        if (!entry.cancelled) {
            std::optional<KeySheetState::View> const view = outcome.error ? held_view(outcome.error->reason) : std::nullopt;
            bool const consent = outcome.result
                && (entry.attestation == auth::AttestationConveyance::Direct || entry.attestation == auth::AttestationConveyance::Enterprise);
            if (view || consent) {
                entry.state.view = view ? *view : KeySheetState::View::Consent;
                if (view == KeySheetState::View::Failed)
                    entry.state.reason = outcome.error->reason;
                entry.state.ended = true;
                entry.state.held = true;
                entry.state.pin.reset();
                entry.state.accounts.clear();
                ++entry.state.question;
                hold = true;
            }
        }
        entry.held = std::move(outcome);
    }
    // Held for the reader, or told to the page now.
    if (hold)
        changed();
    else
        deliver(id, std::nullopt);
}

void SecurityKeyRequests::deliver(std::uint64_t id, std::optional<auth::Outcome> replacement)
{
    std::function<void(auth::Outcome)> done;
    std::optional<auth::Outcome> outcome;
    std::unique_ptr<auth::Ceremony> ceremony;
    {
        std::lock_guard lock(m_mutex);
        auto const found = m_entries.find(id);
        if (found == m_entries.end())
            return;
        Entry& entry = found->second;
        done = std::move(entry.done);
        outcome = replacement ? std::move(replacement) : std::move(entry.held);
        ceremony = std::move(entry.ceremony);
    }
    // The page is told before the request is gone from the list, so that
    // whoever sees it gone knows its page has its answer on the way.
    if (done && outcome)
        done(std::move(*outcome));
    {
        std::lock_guard lock(m_mutex);
        m_entries.erase(id);
        ++m_generation;
    }
    // Ended on whichever thread ends it. On the ceremony's own (from its
    // `done`) its thread finishes by itself; elsewhere the ceremony has
    // delivered its outcome already and its thread has only to return, its
    // keys' threads joined before that delivery.
    ceremony.reset();
    changed();
}

void SecurityKeyRequests::answer_pin(std::uint64_t id, std::optional<auth::PinAnswer> answer)
{
    std::function<void(std::optional<auth::PinAnswer>)> reply;
    {
        std::lock_guard lock(m_mutex);
        auto const found = m_entries.find(id);
        if (found == m_entries.end() || !found->second.pin_answer)
            return;
        reply = std::exchange(found->second.pin_answer, nullptr);
        // Back to the key while it checks the PIN.
        found->second.state.view = KeySheetState::View::Processing;
        found->second.state.pin.reset();
    }
    reply(std::move(answer));
    changed();
}

void SecurityKeyRequests::choose_account(std::uint64_t id, std::optional<std::size_t> index)
{
    std::function<void(std::optional<std::size_t>)> reply;
    {
        std::lock_guard lock(m_mutex);
        auto const found = m_entries.find(id);
        if (found == m_entries.end() || !found->second.account_answer)
            return;
        reply = std::exchange(found->second.account_answer, nullptr);
        found->second.state.view = KeySheetState::View::Processing;
        found->second.state.accounts.clear();
    }
    reply(index);
    changed();
}

void SecurityKeyRequests::consent(std::uint64_t id, bool allow)
{
    std::optional<auth::Outcome> outcome;
    {
        std::lock_guard lock(m_mutex);
        auto const found = m_entries.find(id);
        if (found == m_entries.end() || found->second.state.view != KeySheetState::View::Consent || !found->second.held)
            return;
        outcome = found->second.held;
    }
    if (!allow && outcome && outcome->result) {
        // Anonymized: the statement and its format go, as "none" asks
        // (§5.1.3 step 20); what the page reads off the credential follows.
        auth::Result& result = *outcome->result;
        if (std::optional<auth::AttestationObject> const object = auth::parse_attestation_object(result.attestation_object)) {
            auth::AttestationObject const conveyed = auth::apply_conveyance(*object, auth::AttestationConveyance::None);
            result.attestation_object = auth::encode_attestation_object(conveyed.fmt, conveyed.att_stmt, conveyed.auth_data);
            result.created = auth::created_credential_fields(conveyed, result.transports);
        }
    }
    deliver(id, std::move(outcome));
}

void SecurityKeyRequests::dismiss(std::uint64_t id)
{
    bool held = false;
    bool consent_view = false;
    std::function<void(std::optional<auth::PinAnswer>)> pin;
    std::function<void(std::optional<std::size_t>)> account;
    {
        std::lock_guard lock(m_mutex);
        auto const found = m_entries.find(id);
        if (found == m_entries.end())
            return;
        Entry& entry = found->second;
        held = entry.held.has_value();
        consent_view = entry.state.view == KeySheetState::View::Consent;
        if (!held && entry.state.ended) {
            // Closed in the moment between the ceremony showing how it ends
            // and its ending: the outcome goes to the page when it comes.
            entry.cancelled = true;
        } else if (!held) {
            // The reader cancels: the ceremony ends with NotAllowedError,
            // and a question it is waiting on is answered "no".
            pin = std::exchange(entry.pin_answer, nullptr);
            account = std::exchange(entry.account_answer, nullptr);
            if (entry.ceremony)
                entry.ceremony->cancel(false);
        }
    }
    if (pin)
        pin(std::nullopt);
    if (account)
        account(std::nullopt);
    if (held && consent_view) {
        // The new credential is not handed over: the reader cancelled.
        deliver(id, auth::Outcome { std::nullopt, auth::Error { auth::ErrorKind::NotAllowed, auth::Reason::Cancelled, 0 } });
        return;
    }
    if (held)
        deliver(id, std::nullopt);
    else
        changed();
}

}
