#pragma once
// CTAP1/U2F (FIDO U2F Raw Message Formats v1.2, and CTAP 2.1 §10 for how
// WebAuthn uses it) for keys that speak nothing newer: the REGISTER,
// AUTHENTICATE and VERSION command APDUs in the extended-length form
// (CLA INS P1 P2 00 Lc1 Lc2 data 00 00), their answers, and the mapping of
// those answers to what WebAuthn returns — a "fido-u2f" attestation object
// for a registration (§10.2), authenticator data for an assertion (§10.3).
//
// A U2F key answers SW_CONDITIONS_NOT_SATISFIED ("test of user presence
// required") until it is touched; the client asks again every so often
// (`u2f_poll`) until it answers, the deadline passes or the request is
// cancelled.
#include "auth/CtapHid.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::auth::u2f {

inline constexpr std::uint8_t ins_register = 0x01;
inline constexpr std::uint8_t ins_authenticate = 0x02;
inline constexpr std::uint8_t ins_version = 0x03;

inline constexpr std::uint8_t check_only = 0x07;
inline constexpr std::uint8_t enforce_presence = 0x03;
inline constexpr std::uint8_t dont_enforce_presence = 0x08;

inline constexpr std::uint16_t sw_no_error = 0x9000;
inline constexpr std::uint16_t sw_conditions_not_satisfied = 0x6985;
inline constexpr std::uint16_t sw_wrong_data = 0x6a80;
inline constexpr std::uint16_t sw_wrong_length = 0x6700;
inline constexpr std::uint16_t sw_command_not_allowed = 0x6986;
inline constexpr std::uint16_t sw_ins_not_supported = 0x6d00;
inline constexpr std::uint16_t sw_cla_not_supported = 0x6e00;

// The command APDU.
std::vector<std::uint8_t> apdu(std::uint8_t ins, std::uint8_t p1, std::span<std::uint8_t const> data);
std::vector<std::uint8_t> register_request(std::span<std::uint8_t const, 32> challenge, std::span<std::uint8_t const, 32> application);
// Nullopt for a key handle over 255 bytes, which U2F cannot carry.
std::optional<std::vector<std::uint8_t>> authenticate_request(std::uint8_t control, std::span<std::uint8_t const, 32> challenge,
    std::span<std::uint8_t const, 32> application, std::span<std::uint8_t const> key_handle);

// An answer split into its data and status word; nullopt when shorter than
// the status word.
struct Answer {
    std::vector<std::uint8_t> data;
    std::uint16_t status = 0;
};
std::optional<Answer> split_answer(std::span<std::uint8_t const> response);

struct Registration {
    std::array<std::uint8_t, 65> public_key {}; // uncompressed P-256 point
    std::vector<std::uint8_t> key_handle;
    std::vector<std::uint8_t> certificate; // DER
    std::vector<std::uint8_t> signature; // DER ECDSA
};
// The registration answer's data (U2F Raw Message Formats §4.3): the
// reserved 0x05, the key, the handle, the certificate (its DER length read
// from its own header) and the signature. The key is checked on the curve.
std::optional<Registration> parse_registration(std::span<std::uint8_t const> data);

struct Authentication {
    std::uint8_t user_presence = 0;
    std::uint32_t counter = 0;
    std::vector<std::uint8_t> signature;
};
std::optional<Authentication> parse_authentication(std::span<std::uint8_t const> data);

// CTAP 2.1 §10.2: the authenticator data (zero AAGUID, flags UP | AT, count
// zero, the key as a COSE ES256 key) and the "fido-u2f" attestation object.
std::vector<std::uint8_t> registration_authenticator_data(std::span<std::uint8_t const, 32> rp_id_hash, Registration const& registration);
std::vector<std::uint8_t> registration_attestation_object(std::span<std::uint8_t const, 32> rp_id_hash, Registration const& registration);
// CTAP 2.1 §10.3: rpIdHash || the presence byte's UP bit || the counter.
std::vector<std::uint8_t> authentication_authenticator_data(std::span<std::uint8_t const, 32> application, Authentication const& authentication);

// Sends one U2F message and, while the key answers "test of user presence
// required", asks again every `interval` until it answers otherwise, the
// deadline passes or `cancel` rises. `on_waiting` runs once, at the first
// such answer (the moment to ask for a touch). Nullopt for a transport
// failure, a timeout or a cancel (`failure` says which).
std::optional<Answer> u2f_poll(CtapHidDevice& device, std::span<std::uint8_t const> request, std::chrono::steady_clock::time_point deadline,
    CancelFlag const* cancel, HidFailure* failure = nullptr, std::function<void()> const& on_waiting = {},
    std::chrono::milliseconds interval = std::chrono::milliseconds(200));

// One message, one answer, no polling.
std::optional<Answer> u2f_send(CtapHidDevice& device, std::span<std::uint8_t const> request, std::chrono::steady_clock::time_point deadline,
    CancelFlag const* cancel, HidFailure* failure = nullptr);

// The application parameter and challenge of the "bogus" registration
// that collects a touch from an excluded or unregistered key (CTAP 2.1
// §10.2: a dummy appid and an invalid challenge).
std::array<std::uint8_t, 32> bogus_application();
std::array<std::uint8_t, 32> bogus_challenge();

}
