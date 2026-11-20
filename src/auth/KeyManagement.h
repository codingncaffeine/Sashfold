#pragma once
// The browser's own business with a security key, outside any page's
// ceremony: the reader picks a key by touching it, then sets or changes its
// PIN (CTAP 2.1 §6.5.5.5, §6.5.5.6), sees how many discoverable credentials
// ("passkeys") it holds and has room for, lists them by site and deletes
// one (§6.8). authenticatorReset is not offered: wiping a key is the key
// maker's tool's business, not a browser setting.
//
// How it runs. A KeyManagement works on threads of its own and never on
// the caller's: one watches keys come and go and, until one is chosen,
// each key present has one that waits for its touch
// (authenticatorSelection, or the CTAP 2.0 and U2F ways of waiting for a
// touch the ceremonies use, auth/Client.h). The first key touched is
// chosen and the others let go; its thread then carries out the caller's
// commands one at a time. Every change of state is published as a whole
// (`state()`), with a call of `wake` so the caller looks; the caller's
// commands return at once. `cancel()` ends it from any thread without
// waiting; `finished()` says when its threads are done (joining them then
// takes no time); destroying it cancels and waits.
//
// The token the PIN (or the key's own user verification) gives has the
// credential management permission and no permissions RP ID; it stays in
// this object's memory until the next PIN command, the end, or the key's
// own refusal of it (its usage period over), when the PIN is asked again.
#include "auth/ClientPin.h"
#include "auth/CredentialManagement.h"
#include "auth/Transport.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::auth {

// What the chosen key is and can do, from its getInfo.
struct ManagedKey {
    std::string device; // the product's name, when the system gives one
    bool ctap2 = false; // false: a U2F-only key, which has neither PIN nor passkeys
    bool pin_supported = false; // the clientPin option is there
    bool pin_set = false;
    bool force_pin_change = false;
    std::size_t min_pin_length = 4; // in code points
    bool built_in_uv = false; // a fingerprint reader, enrolled and usable for a token
    cred_mgmt::Support credential_management = cred_mgmt::Support::None;
};

struct KeyManagementState {
    enum class Phase : std::uint8_t {
        InsertKey, // no key is plugged in
        TouchKey, // keys are present: touch the one to manage
        Busy, // the chosen key is being read, or a command is on its way
        VerifyFingerprint, // the key's own user verification is waiting
        Ready, // the chosen key is known and waits for a command
        Ended, // nothing more can be done with this key: `failure` says why
    };
    // The outcome of the last command.
    enum class Outcome : std::uint8_t {
        None,
        PinSet,
        PinChanged,
        PinWrong, // `retries` left
        PinPolicy, // the key refused the new PIN (too short for it, the same as before, ...)
        PinNeedsChange, // the key insists on a new PIN before anything else
        Unlocked, // `metadata` and `passkeys` are read
        TokenExpired, // the key no longer takes the token: the PIN is asked again
        UvWrong, // the fingerprint was not recognized; `retries` left
        UvBlocked, // the fingerprint check is locked: the PIN instead
        Deleted,
        NotFound, // the credential to delete is no longer there
        NotSupported, // the key does not do that
    };
    enum class Failure : std::uint8_t {
        None,
        KeyGone, // the chosen key was pulled out
        PinBlocked, // every try used: only a reset can bring the key back
        PinAuthBlocked, // three wrong in a row: remove and insert the key again
        DeviceError, // an answer the settings cannot go on from
        Cancelled,
    };

    Phase phase = Phase::InsertKey;
    std::size_t keys_present = 0;
    std::optional<ManagedKey> key; // once chosen and read
    Outcome outcome = Outcome::None;
    int retries = -1; // PinWrong, UvWrong: tries left, when known
    Failure failure = Failure::None;
    std::uint8_t ctap_status = 0; // the status behind DeviceError, when there was one
    bool unlocked = false; // a token is held and the list below is the key's
    std::optional<CredentialsMetadata> metadata;
    std::vector<Passkey> passkeys;
    // Moves with every change, and once each command is done.
    std::uint64_t generation = 0;
    std::uint64_t commands_done = 0;
};

class KeyManagement {
public:
    KeyManagement(std::shared_ptr<Transport> transport, std::function<void()> wake);
    ~KeyManagement();
    KeyManagement(KeyManagement const&) = delete;
    KeyManagement& operator=(KeyManagement const&) = delete;

    void start();
    KeyManagementState state() const;

    // The commands, carried out in order on the chosen key's thread. Each
    // is ignored before a key is chosen or after the end.
    void set_pin(std::string new_pin);
    void change_pin(std::string current, std::string new_pin);
    // Asks for the credential management token (the PIN given, or the
    // key's own user verification with none) and reads the passkeys.
    void unlock(std::optional<std::string> pin);
    void remove(CredentialDescriptor credential);
    // Reads the passkeys again with the token held.
    void refresh();

    void cancel();
    bool finished() const;

    // How long one command may take with the key before it is given up
    // (none of them waits for a touch).
    static constexpr std::chrono::seconds command_time { 30 };

    struct State;

private:
    std::shared_ptr<State> m_state;
};

}
