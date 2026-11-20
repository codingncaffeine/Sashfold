#include "auth/U2f.h"

#include "auth/Attestation.h"
#include "auth/Cose.h"
#include "crypto/Ec.h"
#include "crypto/Sha2.h"

#include <algorithm>
#include <thread>

namespace sashfold::auth::u2f {

std::vector<std::uint8_t> apdu(std::uint8_t ins, std::uint8_t p1, std::span<std::uint8_t const> data)
{
    std::vector<std::uint8_t> out { 0x00, ins, p1, 0x00 };
    if (!data.empty()) {
        out.push_back(0x00);
        out.push_back(static_cast<std::uint8_t>(data.size() >> 8));
        out.push_back(static_cast<std::uint8_t>(data.size()));
        out.insert(out.end(), data.begin(), data.end());
        // Le: up to 65536 bytes of answer, in the two-byte form that
        // follows an extended Lc.
        out.push_back(0x00);
        out.push_back(0x00);
    } else {
        // No data: Le alone, extended (00 00 00).
        out.insert(out.end(), { 0x00, 0x00, 0x00 });
    }
    return out;
}

std::vector<std::uint8_t> register_request(std::span<std::uint8_t const, 32> challenge, std::span<std::uint8_t const, 32> application)
{
    std::vector<std::uint8_t> data(challenge.begin(), challenge.end());
    data.insert(data.end(), application.begin(), application.end());
    return apdu(ins_register, 0x00, data);
}

std::optional<std::vector<std::uint8_t>> authenticate_request(std::uint8_t control, std::span<std::uint8_t const, 32> challenge,
    std::span<std::uint8_t const, 32> application, std::span<std::uint8_t const> key_handle)
{
    if (key_handle.size() > 255)
        return std::nullopt;
    std::vector<std::uint8_t> data(challenge.begin(), challenge.end());
    data.insert(data.end(), application.begin(), application.end());
    data.push_back(static_cast<std::uint8_t>(key_handle.size()));
    data.insert(data.end(), key_handle.begin(), key_handle.end());
    return apdu(ins_authenticate, control, data);
}

std::optional<Answer> split_answer(std::span<std::uint8_t const> response)
{
    if (response.size() < 2)
        return std::nullopt;
    Answer answer;
    answer.data.assign(response.begin(), response.end() - 2);
    answer.status = static_cast<std::uint16_t>(response[response.size() - 2] << 8 | response[response.size() - 1]);
    return answer;
}

std::optional<Registration> parse_registration(std::span<std::uint8_t const> data)
{
    if (data.size() < 1 + 65 + 1 || data[0] != 0x05)
        return std::nullopt;
    Registration registration;
    std::copy_n(data.begin() + 1, 65, registration.public_key.begin());
    if (!crypto::ec_decode_point(crypto::CurveId::P256, registration.public_key))
        return std::nullopt;
    std::size_t offset = 66;
    std::size_t const handle_length = data[offset++];
    if (handle_length == 0 || data.size() - offset < handle_length)
        return std::nullopt;
    registration.key_handle.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
        data.begin() + static_cast<std::ptrdiff_t>(offset + handle_length));
    offset += handle_length;
    // The certificate is one DER SEQUENCE; its length is in its header.
    if (data.size() - offset < 2 || data[offset] != 0x30)
        return std::nullopt;
    std::size_t header = 2;
    std::size_t content = data[offset + 1];
    if ((content & 0x80) != 0) {
        std::size_t const count = content & 0x7f;
        if (count == 0 || count > 3 || data.size() - offset < 2 + count)
            return std::nullopt;
        content = 0;
        for (std::size_t i = 0; i < count; ++i)
            content = content << 8 | data[offset + 2 + i];
        header += count;
    }
    if (data.size() - offset < header + content)
        return std::nullopt;
    registration.certificate.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
        data.begin() + static_cast<std::ptrdiff_t>(offset + header + content));
    offset += header + content;
    // CTAP 2.1 §10.2: an ECDSA signature is 8 to 72 bytes.
    std::size_t const signature_length = data.size() - offset;
    if (signature_length < 8 || signature_length > 72)
        return std::nullopt;
    registration.signature.assign(data.begin() + static_cast<std::ptrdiff_t>(offset), data.end());
    return registration;
}

std::optional<Authentication> parse_authentication(std::span<std::uint8_t const> data)
{
    if (data.size() < 5 + 8 || data.size() > 5 + 72)
        return std::nullopt;
    Authentication authentication;
    authentication.user_presence = data[0];
    authentication.counter = std::uint32_t { data[1] } << 24 | std::uint32_t { data[2] } << 16 | std::uint32_t { data[3] } << 8 | data[4];
    authentication.signature.assign(data.begin() + 5, data.end());
    return authentication;
}

std::vector<std::uint8_t> registration_authenticator_data(std::span<std::uint8_t const, 32> rp_id_hash, Registration const& registration)
{
    std::vector<std::uint8_t> out(rp_id_hash.begin(), rp_id_hash.end());
    out.push_back(flags::up | flags::at);
    out.insert(out.end(), 4, 0x00); // signCount
    out.insert(out.end(), 16, 0x00); // AAGUID
    out.push_back(static_cast<std::uint8_t>(registration.key_handle.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(registration.key_handle.size()));
    out.insert(out.end(), registration.key_handle.begin(), registration.key_handle.end());
    auto const point = crypto::ec_decode_point(crypto::CurveId::P256, registration.public_key);
    if (point)
        cbor::encode_into(cose_key_to_cbor(cose_key_from_ec(crypto::CurveId::P256, *point, cose::alg_es256)), out);
    return out;
}

std::vector<std::uint8_t> registration_attestation_object(std::span<std::uint8_t const, 32> rp_id_hash, Registration const& registration)
{
    cbor::Value statement = cbor::Value::map();
    statement.set("sig", cbor::Value::bytes(registration.signature));
    statement.set("x5c", cbor::Value::array({ cbor::Value::bytes(registration.certificate) }));
    return encode_attestation_object("fido-u2f", statement, registration_authenticator_data(rp_id_hash, registration));
}

std::vector<std::uint8_t> authentication_authenticator_data(std::span<std::uint8_t const, 32> application, Authentication const& authentication)
{
    std::vector<std::uint8_t> out(application.begin(), application.end());
    out.push_back(static_cast<std::uint8_t>(authentication.user_presence & flags::up));
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(authentication.counter >> shift));
    return out;
}

std::optional<Answer> u2f_send(CtapHidDevice& device, std::span<std::uint8_t const> request, std::chrono::steady_clock::time_point deadline,
    CancelFlag const* cancel, HidFailure* failure)
{
    auto const response = device.transact(ctaphid::cmd_msg, request, deadline, cancel);
    if (!response.ok()) {
        if (failure != nullptr)
            *failure = response.failure;
        return std::nullopt;
    }
    auto answer = split_answer(response.payload);
    if (!answer && failure != nullptr)
        *failure = HidFailure::Malformed;
    return answer;
}

std::optional<Answer> u2f_poll(CtapHidDevice& device, std::span<std::uint8_t const> request, std::chrono::steady_clock::time_point deadline,
    CancelFlag const* cancel, HidFailure* failure, std::function<void()> const& on_waiting, std::chrono::milliseconds interval)
{
    bool told = false;
    while (true) {
        auto answer = u2f_send(device, request, deadline, cancel, failure);
        if (!answer || answer->status != sw_conditions_not_satisfied)
            return answer;
        if (!told && on_waiting) {
            on_waiting();
            told = true;
        }
        // Wait before asking again, in slices so a cancel is prompt.
        auto const next = std::chrono::steady_clock::now() + interval;
        while (std::chrono::steady_clock::now() < next) {
            if (cancel != nullptr && cancel->load()) {
                if (failure != nullptr)
                    *failure = HidFailure::Cancelled;
                return std::nullopt;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                if (failure != nullptr)
                    *failure = HidFailure::Timeout;
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

std::array<std::uint8_t, 32> bogus_application()
{
    // SHA-256 of a name no relying party can have.
    std::string_view const name = "bogus";
    return crypto::Sha256::hash(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(name.data()), name.size()));
}

std::array<std::uint8_t, 32> bogus_challenge()
{
    std::array<std::uint8_t, 32> challenge {};
    challenge.fill(0x42);
    return challenge;
}

}
