#include "auth/ClientPin.h"

#include "platform/Random.h"

namespace sashfold::auth::client_pin {

Answer Answer::of(Ctap2Reply const& reply)
{
    Answer answer;
    answer.failure = reply.malformed ? HidFailure::Malformed : reply.failure;
    answer.status = reply.status;
    return answer;
}

namespace {

// An exchange that could not go on for a reason of the platform's own (a
// key agreement that did not decrypt, a key that is not on the curve).
Answer other() { return Answer { HidFailure::None, static_cast<std::uint8_t>(ctap2::Status::Other) }; }

}

std::optional<pin::Protocol> preferred_protocol(AuthenticatorInfo const& info)
{
    for (std::uint8_t const p : info.pin_uv_auth_protocols) {
        if (auto const protocol = pin::protocol_from_number(p))
            return protocol;
    }
    // A CTAP 2.0 key that names no protocol speaks protocol one.
    if (info.pin_uv_auth_protocols.empty() && !info.is_ctap21())
        return pin::Protocol::One;
    return std::nullopt;
}

namespace {

// getKeyAgreement, keeping the answer's status when it failed.
std::optional<pin::Encapsulation> agree(Call const& call, pin::Protocol protocol, Answer& failed)
{
    ClientPinParams params;
    params.protocol = static_cast<std::uint8_t>(protocol);
    params.subcommand = ctap2::pin_get_key_agreement;
    auto const encoded = encode_client_pin(params);
    auto const reply = call(ctap2::cmd_client_pin, &encoded);
    if (!reply.ok()) {
        failed = Answer::of(reply);
        return std::nullopt;
    }
    auto const response = parse_client_pin_response(reply.body);
    if (!response || !response->key_agreement) {
        failed = Answer { HidFailure::Malformed, 0 };
        return std::nullopt;
    }
    auto shared = pin::encapsulate(protocol, *response->key_agreement, platform::fill_random);
    if (!shared)
        failed = other();
    return shared;
}

TokenAnswer token_from(pin::Protocol protocol, pin::Encapsulation const& shared, Ctap2Reply const& reply)
{
    TokenAnswer out;
    out.answer = Answer::of(reply);
    if (!reply.ok())
        return out;
    auto const response = parse_client_pin_response(reply.body);
    if (!response || !response->pin_uv_auth_token) {
        out.answer = Answer { HidFailure::Malformed, 0 };
        return out;
    }
    auto token = pin::decrypt(protocol, shared.shared_secret, *response->pin_uv_auth_token);
    // 32 bytes; protocol one allows 16 as well (§6.5.6).
    if (!token || (token->size() != 32 && !(protocol == pin::Protocol::One && token->size() == 16))) {
        out.answer = Answer { HidFailure::Malformed, 0 };
        return out;
    }
    out.token = Token { protocol, std::move(*token) };
    return out;
}

}

std::optional<pin::Encapsulation> key_agreement(Call const& call, pin::Protocol protocol)
{
    Answer ignored;
    return agree(call, protocol, ignored);
}

std::optional<Retries> retries(Call const& call, pin::Protocol protocol, std::uint8_t subcommand)
{
    ClientPinParams params;
    params.protocol = static_cast<std::uint8_t>(protocol);
    params.subcommand = subcommand;
    auto const encoded = encode_client_pin(params);
    auto const reply = call(ctap2::cmd_client_pin, &encoded);
    if (!reply.ok())
        return std::nullopt;
    auto const response = parse_client_pin_response(reply.body);
    if (!response)
        return std::nullopt;
    Retries out;
    out.left = subcommand == ctap2::pin_get_uv_retries ? response->uv_retries : response->pin_retries;
    out.power_cycle_needed = response->power_cycle_state.value_or(false);
    return out;
}

Answer set_pin(Call const& call, pin::Protocol protocol, std::string_view new_pin)
{
    auto const padded = pin::padded_pin(new_pin);
    if (!padded)
        return Answer { HidFailure::None, static_cast<std::uint8_t>(ctap2::Status::PinPolicyViolation) };
    Answer failed;
    auto const shared = agree(call, protocol, failed);
    if (!shared)
        return failed;
    // newPinEnc = encrypt(sharedSecret, paddedPin); pinUvAuthParam =
    // authenticate(sharedSecret, newPinEnc) (§6.5.5.5).
    auto const encrypted = pin::encrypt(protocol, shared->shared_secret, *padded, platform::fill_random);
    if (!encrypted)
        return other();
    ClientPinParams params;
    params.protocol = static_cast<std::uint8_t>(protocol);
    params.subcommand = ctap2::pin_set;
    params.key_agreement = shared->platform_key;
    params.new_pin_enc = *encrypted;
    params.pin_uv_auth_param = pin::authenticate(protocol, shared->shared_secret, *encrypted);
    auto const encoded = encode_client_pin(params);
    return Answer::of(call(ctap2::cmd_client_pin, &encoded));
}

Answer change_pin(Call const& call, pin::Protocol protocol, std::string_view current, std::string_view new_pin)
{
    auto const padded = pin::padded_pin(new_pin);
    if (!padded)
        return Answer { HidFailure::None, static_cast<std::uint8_t>(ctap2::Status::PinPolicyViolation) };
    Answer failed;
    auto const shared = agree(call, protocol, failed);
    if (!shared)
        return failed;
    // newPinEnc and pinHashEnc = encrypt(sharedSecret, LEFT(SHA-256(curPin),
    // 16)); pinUvAuthParam = authenticate(sharedSecret, newPinEnc ||
    // pinHashEnc) (§6.5.5.6).
    auto const new_pin_enc = pin::encrypt(protocol, shared->shared_secret, *padded, platform::fill_random);
    auto const hash = pin::pin_hash(current);
    auto const pin_hash_enc = pin::encrypt(protocol, shared->shared_secret, hash, platform::fill_random);
    if (!new_pin_enc || !pin_hash_enc)
        return other();
    ClientPinParams params;
    params.protocol = static_cast<std::uint8_t>(protocol);
    params.subcommand = ctap2::pin_change;
    params.key_agreement = shared->platform_key;
    params.new_pin_enc = *new_pin_enc;
    params.pin_hash_enc = *pin_hash_enc;
    std::vector<std::uint8_t> message = *new_pin_enc;
    message.insert(message.end(), pin_hash_enc->begin(), pin_hash_enc->end());
    params.pin_uv_auth_param = pin::authenticate(protocol, shared->shared_secret, message);
    auto const encoded = encode_client_pin(params);
    return Answer::of(call(ctap2::cmd_client_pin, &encoded));
}

TokenAnswer token_using_pin(Call const& call, AuthenticatorInfo const& info, pin::Protocol protocol, std::string_view pin, std::uint8_t permissions,
    std::optional<std::string> const& rp_id)
{
    TokenAnswer out;
    auto const shared = agree(call, protocol, out.answer);
    if (!shared)
        return out;
    auto const hash = pin::pin_hash(pin);
    auto const pin_hash_enc = pin::encrypt(protocol, shared->shared_secret, hash, platform::fill_random);
    if (!pin_hash_enc) {
        out.answer = other();
        return out;
    }
    ClientPinParams params;
    params.protocol = static_cast<std::uint8_t>(protocol);
    params.key_agreement = shared->platform_key;
    params.pin_hash_enc = *pin_hash_enc;
    if (info.is_ctap21() && info.option_true("pinUvAuthToken")) {
        params.subcommand = ctap2::pin_get_token_using_pin;
        params.permissions = permissions;
        params.rp_id = rp_id;
    } else {
        params.subcommand = ctap2::pin_get_token;
    }
    auto const encoded = encode_client_pin(params);
    return token_from(protocol, *shared, call(ctap2::cmd_client_pin, &encoded));
}

TokenAnswer token_using_uv(Call const& call, pin::Protocol protocol, std::uint8_t permissions, std::optional<std::string> const& rp_id)
{
    TokenAnswer out;
    auto const shared = agree(call, protocol, out.answer);
    if (!shared)
        return out;
    ClientPinParams params;
    params.protocol = static_cast<std::uint8_t>(protocol);
    params.subcommand = ctap2::pin_get_token_using_uv;
    params.key_agreement = shared->platform_key;
    params.permissions = permissions;
    params.rp_id = rp_id;
    auto const encoded = encode_client_pin(params);
    return token_from(protocol, *shared, call(ctap2::cmd_client_pin, &encoded));
}

}
