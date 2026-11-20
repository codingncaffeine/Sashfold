#include "auth/CredentialManagement.h"

#include "auth/Cose.h"
#include "auth/PinProtocol.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace sashfold::auth {

namespace cred_mgmt {

Support support_of(AuthenticatorInfo const& info)
{
    if (info.option_true("credMgmt"))
        return Support::Full;
    if (info.option_true("credentialMgmtPreview"))
        return Support::Preview;
    return Support::None;
}

std::uint8_t command_for(Support support)
{
    switch (support) {
    case Support::Full: return ctap2::cmd_credential_management;
    case Support::Preview: return ctap2::cmd_credential_management_preview;
    case Support::None: return 0;
    }
    return 0;
}

cbor::Value encode_sub_command_params(SubCommandParams const& params)
{
    cbor::Value map = cbor::Value::map();
    if (params.rp_id_hash)
        map.set(0x01, cbor::Value::bytes(std::span<std::uint8_t const>(*params.rp_id_hash)));
    if (params.credential_id)
        map.set(0x02, descriptor_to_cbor(*params.credential_id));
    if (params.user)
        map.set(0x03, user_to_cbor(*params.user));
    return map;
}

std::vector<std::uint8_t> auth_message(std::uint8_t sub_command, std::optional<cbor::Value> const& params)
{
    std::vector<std::uint8_t> message { sub_command };
    if (params)
        cbor::encode_into(*params, message);
    return message;
}

bool needs_token(std::uint8_t sub_command)
{
    return sub_command != enumerate_rps_get_next_rp && sub_command != enumerate_credentials_get_next_credential;
}

cbor::Value encode_request(std::uint8_t sub_command, std::optional<cbor::Value> const& params, client_pin::Token const* token)
{
    cbor::Value map = cbor::Value::map();
    map.set(0x01, cbor::Value::unsigned_integer(sub_command));
    if (params)
        map.set(0x02, *params);
    if (token != nullptr && needs_token(sub_command)) {
        map.set(0x03, cbor::Value::unsigned_integer(static_cast<std::uint8_t>(token->protocol)));
        map.set(0x04, cbor::Value::bytes(pin::authenticate(token->protocol, token->token, auth_message(sub_command, params))));
    }
    return map;
}

namespace {

// An unsigned member; false when it is there with another type.
bool read_unsigned(cbor::Value const& map, std::int64_t key, std::optional<std::uint64_t>& out)
{
    cbor::Value const* const value = map.find(key);
    if (value == nullptr)
        return true;
    if (!value->is_unsigned())
        return false;
    out = value->raw_number();
    return true;
}

}

std::optional<Response> parse_response(cbor::Value const& map)
{
    if (!map.is_map())
        return std::nullopt;
    Response response;
    if (!read_unsigned(map, 0x01, response.existing_resident_credentials_count)
        || !read_unsigned(map, 0x02, response.max_possible_remaining_resident_credentials_count) || !read_unsigned(map, 0x05, response.total_rps)
        || !read_unsigned(map, 0x09, response.total_credentials) || !read_unsigned(map, 0x0a, response.cred_protect))
        return std::nullopt;
    if (cbor::Value const* const rp = map.find(0x03)) {
        cbor::Value const* const id = rp->find("id");
        if (!rp->is_map() || id == nullptr || !id->is_text())
            return std::nullopt;
        RelyingParty entity;
        entity.id = id->as_text();
        if (cbor::Value const* const name = rp->find("name"); name != nullptr && name->is_text())
            entity.name = name->as_text();
        response.rp = std::move(entity);
    }
    if (cbor::Value const* const hash = map.find(0x04)) {
        if (!hash->is_bytes() || hash->as_bytes().size() != 32)
            return std::nullopt;
        std::array<std::uint8_t, 32> bytes {};
        std::ranges::copy(hash->as_bytes(), bytes.begin());
        response.rp_id_hash = bytes;
    }
    if (cbor::Value const* const user = map.find(0x06)) {
        response.user = user_from_cbor(*user);
        if (!response.user)
            return std::nullopt;
    }
    if (cbor::Value const* const credential = map.find(0x07)) {
        response.credential_id = descriptor_from_cbor(*credential);
        if (!response.credential_id)
            return std::nullopt;
    }
    if (cbor::Value const* const key = map.find(0x08)) {
        // A key of an algorithm this engine does not read is still the
        // key's to hold: only its shape is required.
        if (!key->is_map())
            return std::nullopt;
        response.public_key = cose_key_from_cbor(*key);
    }
    if (cbor::Value const* const blob = map.find(0x0b)) {
        if (!blob->is_bytes())
            return std::nullopt;
        response.large_blob_key = blob->byte_vector();
    }
    if (cbor::Value const* const payment = map.find(0x0c)) {
        if (!payment->is_bool())
            return std::nullopt;
        response.third_party_payment = payment->as_bool();
    }
    return response;
}

}

using client_pin::Answer;

namespace {

Answer malformed() { return Answer { HidFailure::Malformed, 0 }; }

}

CredentialManager::CredentialManager(client_pin::Call call, cred_mgmt::Support support, client_pin::Token token)
    : m_call(std::move(call))
    , m_support(support)
    , m_token(std::move(token))
{
}

Answer CredentialManager::send(std::uint8_t sub_command, std::optional<cbor::Value> const& params, std::optional<cred_mgmt::Response>& out)
{
    out.reset();
    std::uint8_t const command = cred_mgmt::command_for(m_support);
    if (command == 0)
        return Answer { HidFailure::None, static_cast<std::uint8_t>(ctap2::Status::InvalidCommand) };
    auto const request = cred_mgmt::encode_request(sub_command, params, &m_token);
    auto const reply = m_call(command, &request);
    Answer const answer = Answer::of(reply);
    if (!answer.ok())
        return answer;
    // A subcommand with nothing to say answers with no map at all.
    out = reply.body.is_null() ? cred_mgmt::Response {} : cred_mgmt::parse_response(reply.body);
    if (!out)
        return malformed();
    return answer;
}

Answer CredentialManager::metadata(CredentialsMetadata& out)
{
    std::optional<cred_mgmt::Response> response;
    Answer const answer = send(cred_mgmt::get_creds_metadata, std::nullopt, response);
    if (!answer.ok())
        return answer;
    if (!response->existing_resident_credentials_count || !response->max_possible_remaining_resident_credentials_count)
        return malformed();
    out.existing = *response->existing_resident_credentials_count;
    out.remaining = *response->max_possible_remaining_resident_credentials_count;
    return answer;
}

Answer CredentialManager::passkeys(std::vector<Passkey>& out)
{
    out.clear();
    // The relying parties (§6.8.3): the first with the total, the rest one
    // by one.
    struct Party {
        std::string id;
        std::array<std::uint8_t, 32> hash {};
    };
    std::vector<Party> parties;
    std::optional<cred_mgmt::Response> response;
    Answer answer = send(cred_mgmt::enumerate_rps_begin, std::nullopt, response);
    if (answer.failure == HidFailure::None && answer.status == static_cast<std::uint8_t>(ctap2::Status::NoCredentials))
        return Answer {};
    if (!answer.ok())
        return answer;
    if (!response->rp || !response->rp_id_hash || !response->total_rps || *response->total_rps == 0
        || *response->total_rps > CredentialManager::max_relying_parties)
        return malformed();
    std::uint64_t const total_rps = *response->total_rps;
    parties.push_back(Party { response->rp->id, *response->rp_id_hash });
    while (parties.size() < total_rps) {
        answer = send(cred_mgmt::enumerate_rps_get_next_rp, std::nullopt, response);
        if (!answer.ok())
            return answer;
        if (!response->rp || !response->rp_id_hash)
            return malformed();
        parties.push_back(Party { response->rp->id, *response->rp_id_hash });
    }

    // Each one's credentials (§6.8.4), under its RP ID hash as the key gave it.
    std::uint64_t count = 0;
    for (Party const& party : parties) {
        cred_mgmt::SubCommandParams params;
        params.rp_id_hash = party.hash;
        answer = send(cred_mgmt::enumerate_credentials_begin, cred_mgmt::encode_sub_command_params(params), response);
        // A party whose credentials went in the meantime has none.
        if (answer.failure == HidFailure::None && answer.status == static_cast<std::uint8_t>(ctap2::Status::NoCredentials))
            continue;
        if (!answer.ok())
            return answer;
        if (!response->total_credentials || *response->total_credentials == 0 || *response->total_credentials > CredentialManager::max_credentials)
            return malformed();
        std::uint64_t const total = *response->total_credentials;
        for (std::uint64_t i = 0; i < total; ++i) {
            if (i > 0) {
                answer = send(cred_mgmt::enumerate_credentials_get_next_credential, std::nullopt, response);
                if (!answer.ok())
                    return answer;
            }
            if (!response->user || !response->credential_id || ++count > CredentialManager::max_credentials)
                return malformed();
            Passkey passkey;
            passkey.rp_id = party.id;
            passkey.rp_id_hash = party.hash;
            passkey.user = *response->user;
            passkey.credential = *response->credential_id;
            passkey.cred_protect = response->cred_protect;
            out.push_back(std::move(passkey));
        }
    }
    std::ranges::stable_sort(out, [](Passkey const& a, Passkey const& b) { return std::tie(a.rp_id, a.user.name) < std::tie(b.rp_id, b.user.name); });
    return Answer {};
}

Answer CredentialManager::remove(CredentialDescriptor const& credential)
{
    cred_mgmt::SubCommandParams params;
    params.credential_id = CredentialDescriptor { credential.id, {} };
    std::optional<cred_mgmt::Response> response;
    return send(cred_mgmt::delete_credential, cred_mgmt::encode_sub_command_params(params), response);
}

Answer CredentialManager::update_user(CredentialDescriptor const& credential, UserEntity const& user)
{
    cred_mgmt::SubCommandParams params;
    params.credential_id = CredentialDescriptor { credential.id, {} };
    params.user = user;
    std::optional<cred_mgmt::Response> response;
    return send(cred_mgmt::update_user_information, cred_mgmt::encode_sub_command_params(params), response);
}

}
