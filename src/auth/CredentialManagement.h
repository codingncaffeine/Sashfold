#pragma once
// authenticatorCredentialManagement (CTAP 2.1 §6.8): what a key holds of
// discoverable credentials ("passkeys"), listed by relying party, and
// deleting one or changing its user's names. The browser's key settings
// use it; a page never reaches it.
//
// Every subcommand but the "next" ones is authorized with a pinUvAuthToken
// that has the credential management permission (`cm`, §6.5.5.7):
// pinUvAuthParam = authenticate(token, subCommand || subCommandParams),
// the parameters in their canonical CBOR (§6.8.2 to §6.8.6). A token
// asked for with a permissions RP ID can list and change only that RP's
// credentials; the settings ask for one without, which lists everything.
//
// Keys of the CTAP 2.1 pre-release that name the "credentialMgmtPreview"
// option have the prototype of the command under 0x41, with the same
// subcommands and parameters, authorized by a getPinToken token (it has no
// permissions).
#include "auth/ClientPin.h"
#include "auth/Ctap2.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::auth {

namespace ctap2 {
inline constexpr std::uint8_t cmd_credential_management = 0x0a;
inline constexpr std::uint8_t cmd_credential_management_preview = 0x41;
}

namespace cred_mgmt {

// Subcommands (§6.8).
inline constexpr std::uint8_t get_creds_metadata = 0x01;
inline constexpr std::uint8_t enumerate_rps_begin = 0x02;
inline constexpr std::uint8_t enumerate_rps_get_next_rp = 0x03;
inline constexpr std::uint8_t enumerate_credentials_begin = 0x04;
inline constexpr std::uint8_t enumerate_credentials_get_next_credential = 0x05;
inline constexpr std::uint8_t delete_credential = 0x06;
inline constexpr std::uint8_t update_user_information = 0x07;

// How a key offers the command, from its getInfo (§6.4: "credMgmt", or the
// pre-release's "credentialMgmtPreview").
enum class Support : std::uint8_t { None, Preview, Full };
Support support_of(AuthenticatorInfo const& info);
// The command byte for that support (0 for None).
std::uint8_t command_for(Support support);

// subCommandParams (§6.8): the members a subcommand takes.
struct SubCommandParams {
    std::optional<std::array<std::uint8_t, 32>> rp_id_hash; // 0x01
    std::optional<CredentialDescriptor> credential_id; // 0x02
    std::optional<UserEntity> user; // 0x03
};
cbor::Value encode_sub_command_params(SubCommandParams const& params);

// The bytes pinUvAuthParam authenticates: the subcommand, then its
// parameters' CBOR when it has any.
std::vector<std::uint8_t> auth_message(std::uint8_t sub_command, std::optional<cbor::Value> const& params);

// Whether the subcommand is authorized (all but the two "next" ones).
bool needs_token(std::uint8_t sub_command);

// The request's map (§6.8: 0x01 subCommand, 0x02 subCommandParams, 0x03
// pinUvAuthProtocol, 0x04 pinUvAuthParam). With a token, the last two are
// filled for a subcommand that needs them.
cbor::Value encode_request(std::uint8_t sub_command, std::optional<cbor::Value> const& params, client_pin::Token const* token);

// The answer (§6.8): every member the subcommands return, as found. A
// member of the wrong type makes the whole answer malformed.
struct Response {
    std::optional<std::uint64_t> existing_resident_credentials_count; // 0x01
    std::optional<std::uint64_t> max_possible_remaining_resident_credentials_count; // 0x02
    std::optional<RelyingParty> rp; // 0x03
    std::optional<std::array<std::uint8_t, 32>> rp_id_hash; // 0x04
    std::optional<std::uint64_t> total_rps; // 0x05
    std::optional<UserEntity> user; // 0x06
    std::optional<CredentialDescriptor> credential_id; // 0x07
    std::optional<CoseKey> public_key; // 0x08
    std::optional<std::uint64_t> total_credentials; // 0x09
    std::optional<std::uint64_t> cred_protect; // 0x0a
    std::optional<std::vector<std::uint8_t>> large_blob_key; // 0x0b
    std::optional<bool> third_party_payment; // 0x0c (CTAP 2.2)
};
std::optional<Response> parse_response(cbor::Value const& map);

}

// What getCredsMetadata says.
struct CredentialsMetadata {
    std::uint64_t existing = 0;
    std::uint64_t remaining = 0; // room for this many more
};

// One discoverable credential, under its relying party.
struct Passkey {
    std::string rp_id; // as the key keeps it (keys may shorten a long one)
    std::array<std::uint8_t, 32> rp_id_hash {};
    UserEntity user;
    CredentialDescriptor credential;
    std::optional<std::uint64_t> cred_protect;
};

// The subcommands as whole operations on one key, with a token it gave.
// Each call blocks on the device (a thread of the caller's own).
class CredentialManager {
public:
    // A key that claims more than this is not believed: the enumeration
    // ends there with a malformed answer rather than looping on its word.
    static constexpr std::uint64_t max_relying_parties = 1024;
    static constexpr std::uint64_t max_credentials = 4096;

    CredentialManager(client_pin::Call call, cred_mgmt::Support support, client_pin::Token token);

    client_pin::Answer metadata(CredentialsMetadata& out);
    // enumerateRPs, then enumerateCredentials for each; a key with none
    // answers NO_CREDENTIALS, which is an empty list here. Sorted by RP ID,
    // then user name.
    client_pin::Answer passkeys(std::vector<Passkey>& out);
    client_pin::Answer remove(CredentialDescriptor const& credential);
    // updateUserInformation: the key keeps `user`'s names (empty or absent
    // ones removed); `user.id` must be the credential's.
    client_pin::Answer update_user(CredentialDescriptor const& credential, UserEntity const& user);

private:
    client_pin::Answer send(std::uint8_t sub_command, std::optional<cbor::Value> const& params, std::optional<cred_mgmt::Response>& out);

    client_pin::Call m_call;
    cred_mgmt::Support m_support;
    client_pin::Token m_token;
};

}
