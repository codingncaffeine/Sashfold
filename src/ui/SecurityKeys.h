#pragma once

// The window's side of Web Authentication: every ceremony the pages of its
// tabs ask for, each run by auth::Ceremony on threads of its own over the
// window's transport (the system's keys, or a test's), and what each one's
// prompt shows now. A page's hook starts one for its tab (its owner) from
// whatever thread the page runs on; the window reads the state of the tab
// in front to draw its sheet, and gives the reader's answers back: a PIN,
// an account, consent to show the key's make and model, Cancel. Nothing
// here waits on a device: the ceremonies' threads do that, and every call
// returns at once.
//
// A ceremony that ends in a state the reader should see (this key is
// already registered, is not registered, cannot be used, is locked) is
// held on the sheet until the reader closes it, and only then told to the
// page (or when the page's own timeout ends it): what the page learns comes
// no sooner than the reader's dismissal (WebAuthn §14.5.1, the privacy of
// the ceremony's failures). A new credential whose page asked for
// attestation ("direct" or "enterprise") waits for the reader's consent;
// refused, it goes to the page anonymized as "none" asks (§5.1.3 step 20).
//
// An owner is compared, never read: it names the tab's engine, or its
// document where pages are not in engines, and a request whose owner goes
// is ended by the page's hook (its realm ending) or by the window
// (cancel_owner), never by looking at the owner.

#include "auth/Client.h"
#include "bindings/Realm.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::ui {

// What a tab's sheet shows.
struct KeySheetState {
    enum class View : std::uint8_t {
        InsertKey,
        TouchKey,
        TouchKeyAgain,
        Processing,
        VerifyFingerprint,
        FingerprintRetry,
        Pin, // `pin` says which question
        Account, // `accounts` to choose from
        AlreadyRegistered,
        NotRegistered,
        Unsuitable,
        PinBlocked,
        PinAuthBlocked,
        UvBlocked,
        Failed, // the key went away, refused a new PIN, or failed
        Consent, // the page asked for attestation
    };
    std::uint64_t id = 0;
    // Moves each time the ceremony asks the reader something new: the
    // sheet's fields are emptied when it moves.
    std::uint64_t question = 0;
    std::string site; // the RP ID
    bool create = false;
    View view = View::InsertKey;
    int retries = -1;
    std::size_t keys_present = 0;
    std::optional<auth::PinQuestion> pin;
    std::vector<auth::Account> accounts;
    std::optional<auth::Reason> reason; // Failed: why
    bool ended = false; // the ceremony ends here; the sheet offers Close
    bool held = false; // it has ended, and its page is not told until the sheet closes
};

// The view's name, as scripts and tests say it.
char const* view_name(KeySheetState const& state);

class SecurityKeyRequests : public std::enable_shared_from_this<SecurityKeyRequests> {
public:
    using Owner = void const*;

    SecurityKeyRequests() = default;
    ~SecurityKeyRequests();
    SecurityKeyRequests(SecurityKeyRequests const&) = delete;
    SecurityKeyRequests& operator=(SecurityKeyRequests const&) = delete;

    // Where keys are found. Unset, a ceremony waits for a key that never
    // comes (and ends at its timeout), as on a machine with no keys.
    void set_transport(std::shared_ptr<auth::Transport> transport);
    std::shared_ptr<auth::Transport> transport() const;
    // Called, on any thread, each time a sheet's state changes.
    void set_wake(std::function<void()> wake);

    // A page's hook: a ceremony for `owner`, named by the number returned.
    std::uint64_t start(Owner owner, bindings::HostHooks::SecurityKeyAsk const& ask, auth::Request const& request,
        std::function<void(auth::Outcome)> done);
    // The page's abort, its timeout, its document going away.
    void cancel(std::uint64_t id, bool by_page);
    // The window: the tab is closing; its requests end now.
    void cancel_owner(Owner owner);

    // The window's side. The oldest request of the owner, if it has one.
    std::optional<KeySheetState> state_for(Owner owner) const;
    bool has_request(Owner owner) const;
    // Requests not yet told to their page, of any owner.
    std::size_t active() const;
    // Moves with every change of any request.
    std::uint64_t generation() const;

    // The reader's answers to request `id`.
    void answer_pin(std::uint64_t id, std::optional<auth::PinAnswer> answer);
    void choose_account(std::uint64_t id, std::optional<std::size_t> index);
    void consent(std::uint64_t id, bool allow);
    // Cancel, Escape, or Close on an ended sheet.
    void dismiss(std::uint64_t id);

    // The prompt's calls (the ceremony's threads).
    void shown(std::uint64_t id, auth::PromptInfo const& info);
    void asked_pin(std::uint64_t id, auth::PinQuestion const& question, std::function<void(std::optional<auth::PinAnswer>)> answer);
    void asked_account(std::uint64_t id, std::vector<auth::Account> const& accounts, std::function<void(std::optional<std::size_t>)> answer);
    void finished(std::uint64_t id, auth::Outcome outcome);

private:
    struct Entry {
        Owner owner = nullptr;
        auth::AttestationConveyance attestation = auth::AttestationConveyance::None;
        std::unique_ptr<auth::Ceremony> ceremony;
        std::function<void(auth::Outcome)> done;
        KeySheetState state;
        std::function<void(std::optional<auth::PinAnswer>)> pin_answer;
        std::function<void(std::optional<std::size_t>)> account_answer;
        std::optional<auth::Outcome> held; // ended, waiting for the reader
        bool cancelled = false; // by the page or the window: nothing is held for the reader
    };
    void changed();
    // Ends the entry and tells its page (the held outcome, or `replacement`);
    // called without the lock held.
    void deliver(std::uint64_t id, std::optional<auth::Outcome> replacement);

    mutable std::mutex m_mutex;
    std::shared_ptr<auth::Transport> m_transport;
    std::function<void()> m_wake;
    std::map<std::uint64_t, Entry> m_entries;
    std::uint64_t m_next = 1;
    std::uint64_t m_generation = 0;
};

}
