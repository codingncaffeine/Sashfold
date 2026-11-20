#pragma once
// The platform's side of a WebAuthn ceremony with roaming security keys
// (WebAuthn Level 3 §5.1.3 and §5.1.4, from "issue the request to each
// authenticator" on; CTAP 2.1 §6 and §10 for how each key is spoken to).
// This is the interface navigator.credentials.create() and .get() call
// once the options are validated and the client data hashed.
//
// How it runs. A Ceremony is built from a Request, a Transport and a
// Prompt, and started: from then on it works on threads of its own — one
// that watches keys come and go, one per key — and never on the caller's.
// Every key present, and every key plugged in while the ceremony waits, is
// asked; the first one the user touches wins and the others are cancelled
// (CTAPHID_CANCEL). The ceremony ends exactly once, through the `done`
// callback (called on a ceremony thread), with a Result or an Error. It
// can be cancelled at any time from any thread; destroying it cancels it
// and waits for its threads.
//
// What it does with each key, in order:
//   1. CTAPHID INIT, then authenticatorGetInfo; a key that has no CBOR, or
//      answers getInfo with an error, is spoken to in U2F (CTAP 2.1 §10).
//   2. Whether the key can serve the request at all: an algorithm in
//      common, a discoverable credential if residentKey is "required", a
//      way to verify the user if userVerification is "required" (a PIN can
//      be set during create()), credProtect if the page enforces it. A key
//      that cannot is still offered: touching it ends the ceremony with
//      the KeyUnsuitable state and NotAllowedError, as the browsers do.
//   3. User verification: built-in UV (getPinUvAuthTokenUsingUv...), the
//      PIN (getPinUvAuthTokenUsingPin... or getPinToken), or none — chosen
//      from the key's options and the request's userVerification, with a
//      touch (authenticatorSelection, or the zero-length pinUvAuthParam of
//      CTAP 2.0) first so that the PIN is asked of the key the user chose.
//   4. The exclude list (create) or allow list (get) probed silently
//      (getAssertion with up=false, split by maxCredentialCountInList and
//      maxCredentialIdLength; U2F check-only), under the RP ID and, with
//      the appid / appidExclude extensions, the AppID. A key that already
//      holds an excluded credential ends the ceremony with
//      KeyAlreadyRegistered (InvalidStateError) once touched; a key that
//      holds none of the allowed ones ends it with KeyNotRegistered
//      (NotAllowedError) once touched.
//   5. The command itself, the touch, several accounts (getNextAssertion,
//      then the prompt's choice), the extensions' outputs, and the
//      attestation conveyance preference.
//
// The privacy rule of WebAuthn §5.1.3/§5.1.4 holds: every failure the page
// sees is NotAllowedError, except an excluded credential (InvalidStateError)
// and the page's own abort (AbortError). The detailed reason is kept for
// the browser's own UI and logs.
#include "auth/Attestation.h"
#include "auth/Ctap2.h"
#include "auth/Transport.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::auth {

enum class Operation : std::uint8_t { Create, Get };
enum class Requirement : std::uint8_t { Required, Preferred, Discouraged };

// The prf extension's two inputs, as the page gave them (not yet hashed).
struct PrfInputs {
    std::vector<std::uint8_t> first;
    std::optional<std::vector<std::uint8_t>> second;
};

// The extension inputs the page asked for, already validated (WebAuthn §9,
// §10; CTAP 2.1 §12 for the ones CTAP defines).
struct ExtensionInputs {
    // create()
    std::optional<std::uint8_t> cred_protect; // credentialProtectionPolicy as 1, 2 or 3
    bool enforce_cred_protect = false;
    bool hmac_create_secret = false;
    std::optional<std::vector<std::uint8_t>> cred_blob;
    bool min_pin_length = false;
    bool cred_props = false;
    std::optional<std::string> appid_exclude;
    // get()
    std::optional<std::string> appid;
    bool get_cred_blob = false;
    // both
    bool prf = false; // the prf extension is present
    std::optional<PrfInputs> prf_eval;
    std::vector<std::pair<std::vector<std::uint8_t>, PrfInputs>> prf_eval_by_credential; // credential ID, inputs
    bool large_blob_key = false; // largeBlob: support "required"/"preferred" (create) or read/write (get)
};

struct Request {
    Operation operation = Operation::Get;
    std::string rp_id;
    std::string rp_name; // create()
    std::array<std::uint8_t, 32> client_data_hash {};
    UserEntity user; // create()
    std::vector<std::int64_t> algorithms; // create(): pubKeyCredParams' algorithms, the page's order
    std::vector<CredentialDescriptor> credentials; // excludeCredentials (create) or allowCredentials (get)
    Requirement resident_key = Requirement::Discouraged; // create()
    Requirement user_verification = Requirement::Preferred;
    AttestationConveyance attestation = AttestationConveyance::None; // create()
    ExtensionInputs extensions;
    // How long the ceremony may take; the caller applies WebAuthn's
    // recommended range to the page's value first.
    std::chrono::milliseconds timeout { 300000 };
};

struct ExtensionOutputs {
    std::optional<bool> appid; // get(): the AppID was used
    std::optional<bool> appid_exclude; // create(): the extension was processed
    std::optional<bool> cred_props_rk; // create(): a discoverable credential was made
    std::optional<bool> prf_enabled; // create()
    std::optional<std::vector<std::uint8_t>> prf_first; // get()
    std::optional<std::vector<std::uint8_t>> prf_second;
    std::optional<bool> hmac_create_secret; // create()
    std::optional<std::uint8_t> cred_protect; // the authenticator's output
    std::optional<bool> cred_blob_stored; // create(): the authenticator's "credBlob" output
    std::optional<std::vector<std::uint8_t>> cred_blob; // get()
    std::optional<std::uint64_t> min_pin_length; // create(), for RPs the key allows
    std::optional<std::vector<std::uint8_t>> large_blob_key; // for the largeBlob extension's later use
};

struct Result {
    Operation operation = Operation::Get;
    std::vector<std::uint8_t> credential_id;
    // create(): the attestation object after the conveyance preference,
    // and what getAuthenticatorData() and the rest return.
    std::vector<std::uint8_t> attestation_object;
    std::optional<CreatedCredentialFields> created;
    // get()
    std::vector<std::uint8_t> authenticator_data;
    std::vector<std::uint8_t> signature;
    std::optional<std::vector<std::uint8_t>> user_handle;
    ExtensionOutputs extensions;
    std::vector<std::string> transports { "usb" };
    std::string device_path; // which key, for the browser's own use
};

// Why a ceremony ended without a credential, in detail.
enum class Reason : std::uint8_t {
    Timeout,
    Cancelled, // by the user, through the prompt
    Aborted, // by the page (AbortSignal)
    CredentialExcluded, // the key already holds one of excludeCredentials
    NotRegistered, // the key holds none of allowCredentials, or no discoverable one
    Unsuitable, // the key cannot do what the request needs
    PinBlocked,
    PinAuthBlocked, // too many wrong PINs in a row: the key must be reinserted
    UvBlocked,
    PinPolicy, // a new PIN the key refused
    DeviceGone, // the chosen key was pulled out
    DeviceError, // a status or framing the ceremony cannot go on from
};

enum class ErrorKind : std::uint8_t { NotAllowed, InvalidState, Abort, Unknown };

struct Error {
    ErrorKind kind = ErrorKind::NotAllowed;
    Reason reason = Reason::DeviceError;
    std::uint8_t ctap_status = 0; // the status behind DeviceError, when there was one
    // The DOMException name WebAuthn requires: "NotAllowedError",
    // "InvalidStateError", "AbortError", "UnknownError".
    char const* dom_exception_name() const;
};

struct Outcome {
    std::optional<Result> result;
    std::optional<Error> error;
};

// What the prompt shows. Each state replaces the one before.
enum class PromptState : std::uint8_t {
    InsertKey, // no key is plugged in
    TouchKey, // keys are present; touch the one to use
    TouchKeyAgain, // the chosen key wants the touch that makes the credential or signs
    Processing, // the key says it is busy (KEEPALIVE "processing")
    VerifyFingerprint, // built-in UV is waiting for the user
    UvRetry, // the fingerprint was not recognized; `retries` left
    KeyNotRegistered,
    KeyAlreadyRegistered,
    KeyUnsuitable,
    PinBlocked,
    PinAuthBlocked, // remove and reinsert the key
    UvBlocked, // built-in UV is blocked; the PIN is asked instead
};

struct PromptInfo {
    PromptState state = PromptState::TouchKey;
    int retries = -1; // UvRetry: tries left, when known
    std::size_t keys_present = 0;
    std::string device; // the key's product name, when it matters
};

struct PinQuestion {
    enum class Kind : std::uint8_t {
        Enter, // the PIN of a key that has one
        Retry, // the last one was wrong; `retries` left
        Set, // the key has none and the request needs one: choose a new PIN
        Change, // the key insists on a new PIN (forcePINChange): the current one and a new one
    };
    Kind kind = Kind::Enter;
    int retries = -1;
    std::size_t min_length = 4; // in code points
    std::string device;
};

struct PinAnswer {
    std::string pin; // Enter, Retry, Change: the current PIN; Set: the new one
    std::string new_pin; // Change: the new one
};

struct Account {
    std::vector<std::uint8_t> user_id;
    std::string name;
    std::string display_name;
};

// The UI's side, implemented by the browser's prompt (another packet).
// Every call comes from a ceremony thread; an implementation hands the
// work to its own thread and returns at once. An answer may be given from
// any thread, at most once; an answer of nullopt is the user cancelling.
// An answer that comes after the ceremony ended is ignored.
class Prompt {
public:
    virtual ~Prompt() = default;
    virtual void show(PromptInfo const& info) = 0;
    virtual void ask_pin(PinQuestion const& question, std::function<void(std::optional<PinAnswer>)> answer) = 0;
    virtual void ask_account(std::vector<Account> const& accounts, std::function<void(std::optional<std::size_t>)> answer) = 0;
    // The ceremony is over (the outcome goes to `done` as well).
    virtual void close() = 0;
};

class Ceremony {
public:
    using Done = std::function<void(Outcome)>;

    Ceremony(Request request, std::shared_ptr<Transport> transport, std::shared_ptr<Prompt> prompt, Done done);
    ~Ceremony();
    Ceremony(Ceremony const&) = delete;
    Ceremony& operator=(Ceremony const&) = delete;

    void start();
    // From any thread. `by_page`: the page's AbortSignal (AbortError);
    // otherwise the user dismissed the prompt (NotAllowedError).
    void cancel(bool by_page);
    // Blocks until the outcome has been delivered (for tests and shutdown).
    void wait();

    struct State;

private:
    std::shared_ptr<State> m_state;
};

}
