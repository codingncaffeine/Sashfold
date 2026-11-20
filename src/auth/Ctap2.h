#pragma once
// CTAP 2.1 commands (§6) as the platform sends them and reads their
// answers: authenticatorGetInfo, authenticatorMakeCredential,
// authenticatorGetAssertion and authenticatorGetNextAssertion,
// authenticatorClientPIN and authenticatorSelection. authenticatorReset is
// deliberately absent: a page must never be able to wipe a key.
//
// A command is one byte followed by a CBOR map with integer keys, sent in a
// CTAPHID_CBOR message; the answer is a status byte, followed on success by
// a CBOR map in the canonical form (CTAP 2.1 §8). Members a parser does not
// know are kept (`raw`) and otherwise ignored, as §8 requires.
#include "auth/Cbor.h"
#include "auth/Cose.h"
#include "auth/CtapHid.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::auth {

namespace ctap2 {
// Command bytes (§6).
inline constexpr std::uint8_t cmd_make_credential = 0x01;
inline constexpr std::uint8_t cmd_get_assertion = 0x02;
inline constexpr std::uint8_t cmd_get_info = 0x04;
inline constexpr std::uint8_t cmd_client_pin = 0x06;
inline constexpr std::uint8_t cmd_reset = 0x07;
inline constexpr std::uint8_t cmd_get_next_assertion = 0x08;
inline constexpr std::uint8_t cmd_selection = 0x0b;

// clientPIN subcommands (§6.5.5).
inline constexpr std::uint8_t pin_get_retries = 0x01;
inline constexpr std::uint8_t pin_get_key_agreement = 0x02;
inline constexpr std::uint8_t pin_set = 0x03;
inline constexpr std::uint8_t pin_change = 0x04;
inline constexpr std::uint8_t pin_get_token = 0x05;
inline constexpr std::uint8_t pin_get_token_using_uv = 0x06;
inline constexpr std::uint8_t pin_get_uv_retries = 0x07;
inline constexpr std::uint8_t pin_get_token_using_pin = 0x09;

// pinUvAuthToken permissions (§6.5.5.7).
inline constexpr std::uint8_t permission_make_credential = 0x01;
inline constexpr std::uint8_t permission_get_assertion = 0x02;
inline constexpr std::uint8_t permission_credential_management = 0x04;
inline constexpr std::uint8_t permission_bio_enrollment = 0x08;
inline constexpr std::uint8_t permission_large_blob_write = 0x10;
inline constexpr std::uint8_t permission_authenticator_config = 0x20;

// Status codes (§8.2).
enum class Status : std::uint8_t {
    Ok = 0x00,
    InvalidCommand = 0x01,
    InvalidParameter = 0x02,
    InvalidLength = 0x03,
    InvalidSeq = 0x04,
    Timeout = 0x05,
    ChannelBusy = 0x06,
    LockRequired = 0x0a,
    InvalidChannel = 0x0b,
    CborUnexpectedType = 0x11,
    InvalidCbor = 0x12,
    MissingParameter = 0x14,
    LimitExceeded = 0x15,
    FpDatabaseFull = 0x17,
    LargeBlobStorageFull = 0x18,
    CredentialExcluded = 0x19,
    Processing = 0x21,
    InvalidCredential = 0x22,
    UserActionPending = 0x23,
    OperationPending = 0x24,
    NoOperations = 0x25,
    UnsupportedAlgorithm = 0x26,
    OperationDenied = 0x27,
    KeyStoreFull = 0x28,
    UnsupportedOption = 0x2b,
    InvalidOption = 0x2c,
    KeepaliveCancel = 0x2d,
    NoCredentials = 0x2e,
    UserActionTimeout = 0x2f,
    NotAllowed = 0x30,
    PinInvalid = 0x31,
    PinBlocked = 0x32,
    PinAuthInvalid = 0x33,
    PinAuthBlocked = 0x34,
    PinNotSet = 0x35,
    PuatRequired = 0x36,
    PinPolicyViolation = 0x37,
    RequestTooLarge = 0x39,
    ActionTimeout = 0x3a,
    UpRequired = 0x3b,
    UvBlocked = 0x3c,
    IntegrityFailure = 0x3d,
    InvalidSubcommand = 0x3e,
    UvInvalid = 0x3f,
    UnauthorizedPermission = 0x40,
    Other = 0x7f,
};

// "CTAP2_ERR_PIN_INVALID" and so on, for logs; "unknown" for the rest.
std::string_view status_name(std::uint8_t status);
}

// authenticatorGetInfo (§6.4): every member kept in `raw`; the ones the
// client acts on parsed.
struct AuthenticatorInfo {
    std::vector<std::string> versions; // "U2F_V2", "FIDO_2_0", "FIDO_2_1_PRE", "FIDO_2_1"
    std::vector<std::string> extensions;
    std::array<std::uint8_t, 16> aaguid {};
    std::map<std::string, bool> options; // rk, up, uv, plat, clientPin, pinUvAuthToken, ...
    std::size_t max_msg_size = 1024; // §8: 1024 unless the key says more
    std::vector<std::uint8_t> pin_uv_auth_protocols; // in the key's order of preference
    std::optional<std::size_t> max_credential_count_in_list;
    std::optional<std::size_t> max_credential_id_length;
    std::vector<std::string> transports;
    std::vector<std::int64_t> algorithms; // the public-key algorithms, in order
    std::optional<std::size_t> max_serialized_large_blob_array;
    bool force_pin_change = false;
    std::optional<std::size_t> min_pin_length;
    std::optional<std::uint64_t> firmware_version;
    std::optional<std::size_t> max_cred_blob_length;
    std::optional<std::size_t> max_rp_ids_for_set_min_pin_length;
    std::optional<std::size_t> preferred_platform_uv_attempts;
    std::optional<std::uint64_t> uv_modality;
    std::optional<std::size_t> remaining_discoverable_credentials;
    cbor::Value raw;

    // An option's value; nullopt when the key does not name it (which for
    // most options means "not supported", §6.4).
    std::optional<bool> option(std::string_view name) const;
    bool option_true(std::string_view name) const { return option(name).value_or(false); }
    bool has_version(std::string_view version) const;
    bool has_extension(std::string_view extension) const;
    // CTAP 2.1 or its pre-release: authenticatorSelection, permissions,
    // getPinUvAuthTokenUsing...WithPermissions.
    bool is_ctap21() const { return has_version("FIDO_2_1") || has_version("FIDO_2_1_PRE"); }
    // The key is protected by some form of user verification (§6.5.1).
    bool uv_protected() const { return option_true("clientPin") || option_true("uv"); }
};

std::optional<AuthenticatorInfo> parse_authenticator_info(cbor::Value const& map);

struct RelyingParty {
    std::string id;
    std::string name;
};

struct UserEntity {
    std::vector<std::uint8_t> id; // 1 to 64 bytes
    std::string name;
    std::string display_name;
    bool has_name = false;
    bool has_display_name = false;
};

struct CredentialDescriptor {
    std::vector<std::uint8_t> id;
    std::vector<std::string> transports;

    bool operator==(CredentialDescriptor const&) const = default;
};

cbor::Value descriptor_to_cbor(CredentialDescriptor const& descriptor);
std::optional<CredentialDescriptor> descriptor_from_cbor(cbor::Value const& value);
cbor::Value user_to_cbor(UserEntity const& user);
std::optional<UserEntity> user_from_cbor(cbor::Value const& value);

struct MakeCredentialParams {
    std::array<std::uint8_t, 32> client_data_hash {};
    RelyingParty rp;
    UserEntity user;
    std::vector<std::int64_t> algorithms; // pubKeyCredParams, "public-key" each
    std::vector<CredentialDescriptor> exclude_list;
    cbor::Value extensions = cbor::Value::map(); // sent when not empty
    std::optional<bool> rk;
    std::optional<bool> uv;
    std::optional<bool> up; // only false is meaningful, and keys refuse it
    std::optional<std::vector<std::uint8_t>> pin_uv_auth_param;
    std::optional<std::uint8_t> pin_uv_auth_protocol;
    std::optional<std::uint64_t> enterprise_attestation;
};
cbor::Value encode_make_credential(MakeCredentialParams const& params);

struct MakeCredentialResponse {
    std::string fmt;
    std::vector<std::uint8_t> auth_data;
    cbor::Value att_stmt = cbor::Value::map();
    std::optional<bool> ep_att;
    std::optional<std::vector<std::uint8_t>> large_blob_key;
};
std::optional<MakeCredentialResponse> parse_make_credential_response(cbor::Value const& map);

struct GetAssertionParams {
    std::string rp_id;
    std::array<std::uint8_t, 32> client_data_hash {};
    std::vector<CredentialDescriptor> allow_list;
    cbor::Value extensions = cbor::Value::map();
    std::optional<bool> up;
    std::optional<bool> uv;
    std::optional<std::vector<std::uint8_t>> pin_uv_auth_param;
    std::optional<std::uint8_t> pin_uv_auth_protocol;
};
cbor::Value encode_get_assertion(GetAssertionParams const& params);

struct GetAssertionResponse {
    std::optional<CredentialDescriptor> credential; // absent when the allow list had one entry (§6.2)
    std::vector<std::uint8_t> auth_data;
    std::vector<std::uint8_t> signature;
    std::optional<UserEntity> user;
    std::size_t number_of_credentials = 1;
    bool user_selected = false;
    std::optional<std::vector<std::uint8_t>> large_blob_key;
};
std::optional<GetAssertionResponse> parse_get_assertion_response(cbor::Value const& map);

struct ClientPinParams {
    std::uint8_t protocol = 0; // 0: absent (getPINRetries may omit it)
    std::uint8_t subcommand = 0;
    std::optional<CoseKey> key_agreement;
    std::optional<std::vector<std::uint8_t>> pin_uv_auth_param;
    std::optional<std::vector<std::uint8_t>> new_pin_enc;
    std::optional<std::vector<std::uint8_t>> pin_hash_enc;
    std::optional<std::uint8_t> permissions;
    std::optional<std::string> rp_id;
};
cbor::Value encode_client_pin(ClientPinParams const& params);

struct ClientPinResponse {
    std::optional<CoseKey> key_agreement;
    std::optional<std::vector<std::uint8_t>> pin_uv_auth_token; // encrypted
    std::optional<std::uint64_t> pin_retries;
    std::optional<bool> power_cycle_state;
    std::optional<std::uint64_t> uv_retries;
};
std::optional<ClientPinResponse> parse_client_pin_response(cbor::Value const& map);

// One CTAP2 command's outcome on the wire.
struct Ctap2Reply {
    HidFailure failure = HidFailure::None; // the transport's, when it failed
    std::uint8_t hid_error = 0;
    std::uint8_t status = 0; // the CTAP status byte when the transport delivered one
    cbor::Value body; // the decoded map, or null when there was none
    bool malformed = false; // a body that is not canonical CBOR
    bool ok() const { return failure == HidFailure::None && status == 0 && !malformed; }
};

// A CTAP2 device on a CTAPHID channel.
class Ctap2Device {
public:
    explicit Ctap2Device(CtapHidDevice& hid)
        : m_hid(hid)
    {
    }
    CtapHidDevice& hid() { return m_hid; }

    // Sends command + CBOR(params) (no params: the bare command byte) and
    // decodes the answer strictly.
    Ctap2Reply call(std::uint8_t command, cbor::Value const* params, std::chrono::steady_clock::time_point deadline, CancelFlag const* cancel,
        CtapHidDevice::Keepalive const& keepalive = {});

    std::optional<AuthenticatorInfo> get_info(std::chrono::steady_clock::time_point deadline, CancelFlag const* cancel);

private:
    CtapHidDevice& m_hid;
};

}
