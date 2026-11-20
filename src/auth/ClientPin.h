#pragma once
// authenticatorClientPIN (CTAP 2.1 §6.5.5) from the platform's side, as
// single exchanges: the key agreement, the tries left, setting and changing
// the PIN, and asking for a pinUvAuthToken with the PIN or the key's own
// user verification. A WebAuthn ceremony (auth/Client.h) and the browser's
// key settings (auth/KeyManagement.h) both speak to keys through these.
//
// Every call is one or two commands on the device `call` reaches, and may
// block: they run on a thread of the caller's own, never the window's.
#include "auth/Ctap2.h"
#include "auth/PinProtocol.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::auth::client_pin {

// One CTAP2 command on the caller's device, with the caller's deadline,
// cancel flag and keepalive handling.
using Call = std::function<Ctap2Reply(std::uint8_t command, cbor::Value const* params)>;

// The PIN/UV auth protocol to speak with a key: the first of its list that
// this platform knows (the key lists them in its order of preference,
// §6.4); protocol one for a CTAP 2.0 key that names none.
std::optional<pin::Protocol> preferred_protocol(AuthenticatorInfo const& info);

// getKeyAgreement (§6.5.5.4), then encapsulate() with a fresh platform key.
std::optional<pin::Encapsulation> key_agreement(Call const& call, pin::Protocol protocol);

// getPINRetries or getUVRetries (§6.5.5.2, §6.5.5.3).
struct Retries {
    std::optional<std::uint64_t> left;
    bool power_cycle_needed = false; // getPINRetries' powerCycleState
};
std::optional<Retries> retries(Call const& call, pin::Protocol protocol, std::uint8_t subcommand);

// How a command ended: a transport failure, or the key's status byte.
struct Answer {
    HidFailure failure = HidFailure::None;
    std::uint8_t status = 0;
    bool ok() const { return failure == HidFailure::None && status == 0; }
    static Answer of(Ctap2Reply const& reply);
};

// setPIN (§6.5.5.5) on a key without one. A PIN under 4 code points or
// over 63 bytes is refused here as the key would (PIN_POLICY_VIOLATION).
Answer set_pin(Call const& call, pin::Protocol protocol, std::string_view new_pin);
// changePIN (§6.5.5.6): the current PIN's hash and the new PIN.
Answer change_pin(Call const& call, pin::Protocol protocol, std::string_view current, std::string_view new_pin);

struct Token {
    pin::Protocol protocol = pin::Protocol::One;
    std::vector<std::uint8_t> token;
};
struct TokenAnswer {
    Answer answer;
    std::optional<Token> token; // when answer.ok() and the token decrypted
};

// The PIN's token: getPinUvAuthTokenUsingPinWithPermissions (§6.5.5.7.2)
// with `permissions` and, when given, the permissions RP ID, on a CTAP 2.1
// key with the pinUvAuthToken option; otherwise getPinToken (§6.5.5.7.1),
// whose token has the default permissions of the CTAP 2.0 commands.
TokenAnswer token_using_pin(Call const& call, AuthenticatorInfo const& info, pin::Protocol protocol, std::string_view pin, std::uint8_t permissions,
    std::optional<std::string> const& rp_id);
// The key's built-in user verification:
// getPinUvAuthTokenUsingUvWithPermissions (§6.5.5.7.3).
TokenAnswer token_using_uv(Call const& call, pin::Protocol protocol, std::uint8_t permissions, std::optional<std::string> const& rp_id);

}
