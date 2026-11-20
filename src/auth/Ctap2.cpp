#include "auth/Ctap2.h"

#include <algorithm>

namespace sashfold::auth {

namespace ctap2 {

std::string_view status_name(std::uint8_t status)
{
    switch (static_cast<Status>(status)) {
    case Status::Ok:
        return "CTAP2_OK";
    case Status::InvalidCommand:
        return "CTAP1_ERR_INVALID_COMMAND";
    case Status::InvalidParameter:
        return "CTAP1_ERR_INVALID_PARAMETER";
    case Status::InvalidLength:
        return "CTAP1_ERR_INVALID_LENGTH";
    case Status::InvalidSeq:
        return "CTAP1_ERR_INVALID_SEQ";
    case Status::Timeout:
        return "CTAP1_ERR_TIMEOUT";
    case Status::ChannelBusy:
        return "CTAP1_ERR_CHANNEL_BUSY";
    case Status::LockRequired:
        return "CTAP1_ERR_LOCK_REQUIRED";
    case Status::InvalidChannel:
        return "CTAP1_ERR_INVALID_CHANNEL";
    case Status::CborUnexpectedType:
        return "CTAP2_ERR_CBOR_UNEXPECTED_TYPE";
    case Status::InvalidCbor:
        return "CTAP2_ERR_INVALID_CBOR";
    case Status::MissingParameter:
        return "CTAP2_ERR_MISSING_PARAMETER";
    case Status::LimitExceeded:
        return "CTAP2_ERR_LIMIT_EXCEEDED";
    case Status::FpDatabaseFull:
        return "CTAP2_ERR_FP_DATABASE_FULL";
    case Status::LargeBlobStorageFull:
        return "CTAP2_ERR_LARGE_BLOB_STORAGE_FULL";
    case Status::CredentialExcluded:
        return "CTAP2_ERR_CREDENTIAL_EXCLUDED";
    case Status::Processing:
        return "CTAP2_ERR_PROCESSING";
    case Status::InvalidCredential:
        return "CTAP2_ERR_INVALID_CREDENTIAL";
    case Status::UserActionPending:
        return "CTAP2_ERR_USER_ACTION_PENDING";
    case Status::OperationPending:
        return "CTAP2_ERR_OPERATION_PENDING";
    case Status::NoOperations:
        return "CTAP2_ERR_NO_OPERATIONS";
    case Status::UnsupportedAlgorithm:
        return "CTAP2_ERR_UNSUPPORTED_ALGORITHM";
    case Status::OperationDenied:
        return "CTAP2_ERR_OPERATION_DENIED";
    case Status::KeyStoreFull:
        return "CTAP2_ERR_KEY_STORE_FULL";
    case Status::UnsupportedOption:
        return "CTAP2_ERR_UNSUPPORTED_OPTION";
    case Status::InvalidOption:
        return "CTAP2_ERR_INVALID_OPTION";
    case Status::KeepaliveCancel:
        return "CTAP2_ERR_KEEPALIVE_CANCEL";
    case Status::NoCredentials:
        return "CTAP2_ERR_NO_CREDENTIALS";
    case Status::UserActionTimeout:
        return "CTAP2_ERR_USER_ACTION_TIMEOUT";
    case Status::NotAllowed:
        return "CTAP2_ERR_NOT_ALLOWED";
    case Status::PinInvalid:
        return "CTAP2_ERR_PIN_INVALID";
    case Status::PinBlocked:
        return "CTAP2_ERR_PIN_BLOCKED";
    case Status::PinAuthInvalid:
        return "CTAP2_ERR_PIN_AUTH_INVALID";
    case Status::PinAuthBlocked:
        return "CTAP2_ERR_PIN_AUTH_BLOCKED";
    case Status::PinNotSet:
        return "CTAP2_ERR_PIN_NOT_SET";
    case Status::PuatRequired:
        return "CTAP2_ERR_PUAT_REQUIRED";
    case Status::PinPolicyViolation:
        return "CTAP2_ERR_PIN_POLICY_VIOLATION";
    case Status::RequestTooLarge:
        return "CTAP2_ERR_REQUEST_TOO_LARGE";
    case Status::ActionTimeout:
        return "CTAP2_ERR_ACTION_TIMEOUT";
    case Status::UpRequired:
        return "CTAP2_ERR_UP_REQUIRED";
    case Status::UvBlocked:
        return "CTAP2_ERR_UV_BLOCKED";
    case Status::IntegrityFailure:
        return "CTAP2_ERR_INTEGRITY_FAILURE";
    case Status::InvalidSubcommand:
        return "CTAP2_ERR_INVALID_SUBCOMMAND";
    case Status::UvInvalid:
        return "CTAP2_ERR_UV_INVALID";
    case Status::UnauthorizedPermission:
        return "CTAP2_ERR_UNAUTHORIZED_PERMISSION";
    case Status::Other:
        return "CTAP1_ERR_OTHER";
    }
    return "unknown";
}

}

namespace {

std::optional<std::uint64_t> unsigned_of(cbor::Value const* v)
{
    if (v == nullptr || !v->is_unsigned())
        return std::nullopt;
    return v->raw_number();
}

std::optional<std::size_t> size_of(cbor::Value const* v)
{
    auto const n = unsigned_of(v);
    if (!n || *n > 0xffffffffu)
        return std::nullopt;
    return static_cast<std::size_t>(*n);
}

std::vector<std::string> texts_of(cbor::Value const* v)
{
    std::vector<std::string> out;
    if (v == nullptr || !v->is_array())
        return out;
    for (auto const& item : v->items()) {
        if (item.is_text())
            out.emplace_back(item.as_text());
    }
    return out;
}

cbor::Value bytes_value(std::span<std::uint8_t const> bytes) { return cbor::Value::bytes(bytes); }

}

std::optional<bool> AuthenticatorInfo::option(std::string_view name) const
{
    auto const it = options.find(std::string(name));
    if (it == options.end())
        return std::nullopt;
    return it->second;
}

bool AuthenticatorInfo::has_version(std::string_view version) const
{
    return std::ranges::find(versions, version) != versions.end();
}

bool AuthenticatorInfo::has_extension(std::string_view extension) const
{
    return std::ranges::find(extensions, extension) != extensions.end();
}

std::optional<AuthenticatorInfo> parse_authenticator_info(cbor::Value const& map)
{
    if (!map.is_map())
        return std::nullopt;
    AuthenticatorInfo info;
    info.raw = map;
    cbor::Value const* const versions = map.find(0x01);
    if (versions == nullptr || !versions->is_array())
        return std::nullopt; // versions is the one required member with aaguid
    info.versions = texts_of(versions);
    info.extensions = texts_of(map.find(0x02));
    cbor::Value const* const aaguid = map.find(0x03);
    if (aaguid == nullptr || !aaguid->is_bytes() || aaguid->as_bytes().size() != 16)
        return std::nullopt;
    std::ranges::copy(aaguid->as_bytes(), info.aaguid.begin());
    if (cbor::Value const* const options = map.find(0x04); options != nullptr && options->is_map()) {
        for (auto const& [k, v] : options->entries()) {
            if (k.is_text() && v.is_bool())
                info.options[std::string(k.as_text())] = *v.as_bool();
        }
    }
    if (auto const n = size_of(map.find(0x05)))
        info.max_msg_size = std::max<std::size_t>(*n, 1024);
    if (cbor::Value const* const protocols = map.find(0x06); protocols != nullptr && protocols->is_array()) {
        for (auto const& item : protocols->items()) {
            if (item.is_unsigned() && item.raw_number() <= 0xff)
                info.pin_uv_auth_protocols.push_back(static_cast<std::uint8_t>(item.raw_number()));
        }
    }
    info.max_credential_count_in_list = size_of(map.find(0x07));
    info.max_credential_id_length = size_of(map.find(0x08));
    info.transports = texts_of(map.find(0x09));
    if (cbor::Value const* const algorithms = map.find(0x0a); algorithms != nullptr && algorithms->is_array()) {
        for (auto const& item : algorithms->items()) {
            cbor::Value const* const type = item.find("type");
            cbor::Value const* const alg = item.find("alg");
            if (type != nullptr && type->is_text() && type->as_text() == "public-key" && alg != nullptr && alg->as_int64())
                info.algorithms.push_back(*alg->as_int64());
        }
    }
    info.max_serialized_large_blob_array = size_of(map.find(0x0b));
    if (cbor::Value const* const force = map.find(0x0c); force != nullptr && force->is_bool())
        info.force_pin_change = *force->as_bool();
    info.min_pin_length = size_of(map.find(0x0d));
    info.firmware_version = unsigned_of(map.find(0x0e));
    info.max_cred_blob_length = size_of(map.find(0x0f));
    info.max_rp_ids_for_set_min_pin_length = size_of(map.find(0x10));
    info.preferred_platform_uv_attempts = size_of(map.find(0x11));
    info.uv_modality = unsigned_of(map.find(0x12));
    info.remaining_discoverable_credentials = size_of(map.find(0x14));
    return info;
}

cbor::Value descriptor_to_cbor(CredentialDescriptor const& descriptor)
{
    cbor::Value map = cbor::Value::map();
    map.set("id", bytes_value(descriptor.id));
    map.set("type", cbor::Value::text("public-key"));
    if (!descriptor.transports.empty()) {
        std::vector<cbor::Value> transports;
        for (auto const& t : descriptor.transports)
            transports.push_back(cbor::Value::text(t));
        map.set("transports", cbor::Value::array(std::move(transports)));
    }
    return map;
}

std::optional<CredentialDescriptor> descriptor_from_cbor(cbor::Value const& value)
{
    cbor::Value const* const id = value.find("id");
    cbor::Value const* const type = value.find("type");
    if (id == nullptr || !id->is_bytes() || type == nullptr || !type->is_text())
        return std::nullopt;
    CredentialDescriptor descriptor;
    descriptor.id = id->byte_vector();
    descriptor.transports = texts_of(value.find("transports"));
    return descriptor;
}

cbor::Value user_to_cbor(UserEntity const& user)
{
    cbor::Value map = cbor::Value::map();
    map.set("id", bytes_value(user.id));
    if (user.has_name)
        map.set("name", cbor::Value::text(user.name));
    if (user.has_display_name)
        map.set("displayName", cbor::Value::text(user.display_name));
    return map;
}

std::optional<UserEntity> user_from_cbor(cbor::Value const& value)
{
    cbor::Value const* const id = value.find("id");
    if (id == nullptr || !id->is_bytes())
        return std::nullopt;
    UserEntity user;
    user.id = id->byte_vector();
    if (cbor::Value const* const name = value.find("name"); name != nullptr && name->is_text()) {
        user.name = name->as_text();
        user.has_name = true;
    }
    if (cbor::Value const* const display = value.find("displayName"); display != nullptr && display->is_text()) {
        user.display_name = display->as_text();
        user.has_display_name = true;
    }
    return user;
}

namespace {

cbor::Value options_map(std::initializer_list<std::pair<char const*, std::optional<bool>>> options)
{
    cbor::Value map = cbor::Value::map();
    for (auto const& [name, value] : options) {
        if (value)
            map.set(name, cbor::Value::boolean(*value));
    }
    return map;
}

cbor::Value descriptors(std::vector<CredentialDescriptor> const& list)
{
    std::vector<cbor::Value> items;
    items.reserve(list.size());
    for (auto const& d : list)
        items.push_back(descriptor_to_cbor(d));
    return cbor::Value::array(std::move(items));
}

}

cbor::Value encode_make_credential(MakeCredentialParams const& params)
{
    cbor::Value map = cbor::Value::map();
    map.set(0x01, bytes_value(params.client_data_hash));
    cbor::Value rp = cbor::Value::map();
    rp.set("id", cbor::Value::text(params.rp.id));
    if (!params.rp.name.empty())
        rp.set("name", cbor::Value::text(params.rp.name));
    map.set(0x02, std::move(rp));
    map.set(0x03, user_to_cbor(params.user));
    std::vector<cbor::Value> algorithms;
    for (std::int64_t const alg : params.algorithms) {
        cbor::Value entry = cbor::Value::map();
        entry.set("alg", cbor::Value::integer(alg));
        entry.set("type", cbor::Value::text("public-key"));
        algorithms.push_back(std::move(entry));
    }
    map.set(0x04, cbor::Value::array(std::move(algorithms)));
    if (!params.exclude_list.empty())
        map.set(0x05, descriptors(params.exclude_list));
    if (params.extensions.is_map() && !params.extensions.entries().empty())
        map.set(0x06, params.extensions);
    cbor::Value options = options_map({ { "rk", params.rk }, { "up", params.up }, { "uv", params.uv } });
    if (!options.entries().empty())
        map.set(0x07, std::move(options));
    if (params.pin_uv_auth_param)
        map.set(0x08, bytes_value(*params.pin_uv_auth_param));
    if (params.pin_uv_auth_protocol)
        map.set(0x09, cbor::Value::unsigned_integer(*params.pin_uv_auth_protocol));
    if (params.enterprise_attestation)
        map.set(0x0a, cbor::Value::unsigned_integer(*params.enterprise_attestation));
    return map;
}

std::optional<MakeCredentialResponse> parse_make_credential_response(cbor::Value const& map)
{
    cbor::Value const* const fmt = map.find(0x01);
    cbor::Value const* const auth_data = map.find(0x02);
    cbor::Value const* const att_stmt = map.find(0x03);
    if (fmt == nullptr || !fmt->is_text() || auth_data == nullptr || !auth_data->is_bytes() || att_stmt == nullptr || !att_stmt->is_map())
        return std::nullopt;
    MakeCredentialResponse response;
    response.fmt = fmt->as_text();
    response.auth_data = auth_data->byte_vector();
    response.att_stmt = *att_stmt;
    if (cbor::Value const* const ep = map.find(0x04); ep != nullptr && ep->is_bool())
        response.ep_att = ep->as_bool();
    if (cbor::Value const* const key = map.find(0x05); key != nullptr && key->is_bytes())
        response.large_blob_key = key->byte_vector();
    return response;
}

cbor::Value encode_get_assertion(GetAssertionParams const& params)
{
    cbor::Value map = cbor::Value::map();
    map.set(0x01, cbor::Value::text(params.rp_id));
    map.set(0x02, bytes_value(params.client_data_hash));
    if (!params.allow_list.empty())
        map.set(0x03, descriptors(params.allow_list));
    if (params.extensions.is_map() && !params.extensions.entries().empty())
        map.set(0x04, params.extensions);
    cbor::Value options = options_map({ { "up", params.up }, { "uv", params.uv } });
    if (!options.entries().empty())
        map.set(0x05, std::move(options));
    if (params.pin_uv_auth_param)
        map.set(0x06, bytes_value(*params.pin_uv_auth_param));
    if (params.pin_uv_auth_protocol)
        map.set(0x07, cbor::Value::unsigned_integer(*params.pin_uv_auth_protocol));
    return map;
}

std::optional<GetAssertionResponse> parse_get_assertion_response(cbor::Value const& map)
{
    cbor::Value const* const auth_data = map.find(0x02);
    cbor::Value const* const signature = map.find(0x03);
    if (auth_data == nullptr || !auth_data->is_bytes() || signature == nullptr || !signature->is_bytes())
        return std::nullopt;
    GetAssertionResponse response;
    if (cbor::Value const* const credential = map.find(0x01)) {
        response.credential = descriptor_from_cbor(*credential);
        if (!response.credential)
            return std::nullopt;
    }
    response.auth_data = auth_data->byte_vector();
    response.signature = signature->byte_vector();
    if (cbor::Value const* const user = map.find(0x04)) {
        response.user = user_from_cbor(*user);
        if (!response.user)
            return std::nullopt;
    }
    if (auto const count = size_of(map.find(0x05)))
        response.number_of_credentials = std::max<std::size_t>(*count, 1);
    if (cbor::Value const* const selected = map.find(0x06); selected != nullptr && selected->is_bool())
        response.user_selected = *selected->as_bool();
    if (cbor::Value const* const key = map.find(0x07); key != nullptr && key->is_bytes())
        response.large_blob_key = key->byte_vector();
    return response;
}

cbor::Value encode_client_pin(ClientPinParams const& params)
{
    cbor::Value map = cbor::Value::map();
    if (params.protocol != 0)
        map.set(0x01, cbor::Value::unsigned_integer(params.protocol));
    map.set(0x02, cbor::Value::unsigned_integer(params.subcommand));
    if (params.key_agreement)
        map.set(0x03, cose_key_to_cbor(*params.key_agreement));
    if (params.pin_uv_auth_param)
        map.set(0x04, bytes_value(*params.pin_uv_auth_param));
    if (params.new_pin_enc)
        map.set(0x05, bytes_value(*params.new_pin_enc));
    if (params.pin_hash_enc)
        map.set(0x06, bytes_value(*params.pin_hash_enc));
    if (params.permissions)
        map.set(0x09, cbor::Value::unsigned_integer(*params.permissions));
    if (params.rp_id)
        map.set(0x0a, cbor::Value::text(*params.rp_id));
    return map;
}

std::optional<ClientPinResponse> parse_client_pin_response(cbor::Value const& map)
{
    if (!map.is_map())
        return std::nullopt;
    ClientPinResponse response;
    if (cbor::Value const* const key = map.find(0x01)) {
        response.key_agreement = cose_key_from_cbor(*key);
        if (!response.key_agreement)
            return std::nullopt;
    }
    if (cbor::Value const* const token = map.find(0x02)) {
        if (!token->is_bytes())
            return std::nullopt;
        response.pin_uv_auth_token = token->byte_vector();
    }
    response.pin_retries = unsigned_of(map.find(0x03));
    if (cbor::Value const* const cycle = map.find(0x04); cycle != nullptr && cycle->is_bool())
        response.power_cycle_state = cycle->as_bool();
    response.uv_retries = unsigned_of(map.find(0x05));
    return response;
}

Ctap2Reply Ctap2Device::call(std::uint8_t command, cbor::Value const* params, std::chrono::steady_clock::time_point deadline,
    CancelFlag const* cancel, CtapHidDevice::Keepalive const& keepalive)
{
    std::vector<std::uint8_t> request { command };
    if (params != nullptr)
        cbor::encode_into(*params, request);
    Ctap2Reply reply;
    auto response = m_hid.transact(ctaphid::cmd_cbor, request, deadline, cancel, keepalive);
    if (!response.ok()) {
        reply.failure = response.failure;
        reply.hid_error = response.error_code;
        return reply;
    }
    if (response.payload.empty()) {
        reply.malformed = true;
        return reply;
    }
    reply.status = response.payload[0];
    if (response.payload.size() > 1) {
        auto body = cbor::decode(std::span<std::uint8_t const>(response.payload).subspan(1), cbor::Mode::Ctap2);
        if (!body || !body->is_map())
            reply.malformed = true;
        else
            reply.body = std::move(*body);
    }
    return reply;
}

std::optional<AuthenticatorInfo> Ctap2Device::get_info(std::chrono::steady_clock::time_point deadline, CancelFlag const* cancel)
{
    auto const reply = call(ctap2::cmd_get_info, nullptr, deadline, cancel);
    if (!reply.ok())
        return std::nullopt;
    return parse_authenticator_info(reply.body);
}

}
