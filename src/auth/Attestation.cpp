#include "auth/Attestation.h"

#include "net/tls/X509.h"

#include <algorithm>

namespace sashfold::auth {

std::optional<AuthenticatorData> parse_authenticator_data(std::span<std::uint8_t const> bytes)
{
    if (bytes.size() < 37)
        return std::nullopt;
    AuthenticatorData data;
    std::copy_n(bytes.begin(), 32, data.rp_id_hash.begin());
    data.flags = bytes[32];
    data.sign_count = std::uint32_t { bytes[33] } << 24 | std::uint32_t { bytes[34] } << 16 | std::uint32_t { bytes[35] } << 8 | bytes[36];
    std::size_t offset = 37;
    if ((data.flags & flags::at) != 0) {
        if (bytes.size() - offset < 18)
            return std::nullopt;
        AttestedCredentialData attested;
        std::copy_n(bytes.begin() + 37, 16, attested.aaguid.begin());
        std::size_t const id_length = std::size_t { bytes[53] } << 8 | bytes[54];
        offset = 55;
        // §6.5.1: credentialIdLength is at most 1023.
        if (id_length > 1023 || bytes.size() - offset < id_length)
            return std::nullopt;
        attested.credential_id.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + id_length));
        offset += id_length;
        auto key = cbor::decode_prefix(bytes.subspan(offset), cbor::Mode::General);
        if (!key)
            return std::nullopt;
        auto cose = cose_key_from_cbor(key->first);
        if (!cose)
            return std::nullopt;
        attested.public_key = std::move(*cose);
        attested.public_key_cbor.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + key->second));
        offset += key->second;
        data.attested = std::move(attested);
    }
    if ((data.flags & flags::ed) != 0) {
        auto extensions = cbor::decode_prefix(bytes.subspan(offset), cbor::Mode::General);
        if (!extensions || !extensions->first.is_map())
            return std::nullopt;
        data.extensions = std::move(extensions->first);
        offset += extensions->second;
    }
    if (offset != bytes.size())
        return std::nullopt;
    return data;
}

std::optional<AttestationObject> parse_attestation_object(std::span<std::uint8_t const> bytes)
{
    auto const map = cbor::decode(bytes, cbor::Mode::General);
    if (!map || !map->is_map())
        return std::nullopt;
    cbor::Value const* const fmt = map->find("fmt");
    cbor::Value const* const att_stmt = map->find("attStmt");
    cbor::Value const* const auth_data = map->find("authData");
    if (fmt == nullptr || !fmt->is_text() || att_stmt == nullptr || !att_stmt->is_map() || auth_data == nullptr || !auth_data->is_bytes())
        return std::nullopt;
    auto parsed = parse_authenticator_data(auth_data->as_bytes());
    if (!parsed)
        return std::nullopt;
    AttestationObject object;
    object.fmt = fmt->as_text();
    object.att_stmt = *att_stmt;
    object.auth_data = auth_data->byte_vector();
    object.parsed = std::move(*parsed);
    return object;
}

std::vector<std::uint8_t> encode_attestation_object(std::string const& fmt, cbor::Value const& att_stmt, std::span<std::uint8_t const> auth_data)
{
    cbor::Value map = cbor::Value::map();
    map.set("fmt", cbor::Value::text(fmt));
    map.set("attStmt", att_stmt);
    map.set("authData", cbor::Value::bytes(auth_data));
    return cbor::encode(map);
}

AttestationObject apply_conveyance(AttestationObject const& object, AttestationConveyance preference)
{
    if (preference != AttestationConveyance::None)
        return object;
    bool const zero_aaguid = object.parsed.attested
        && std::ranges::all_of(object.parsed.attested->aaguid, [](std::uint8_t b) { return b == 0; });
    if (zero_aaguid && object.fmt == "packed" && object.att_stmt.find("x5c") == nullptr)
        return object; // self attestation: nothing identifies the key
    AttestationObject none = object;
    none.fmt = "none";
    none.att_stmt = cbor::Value::map();
    return none;
}

std::optional<CreatedCredentialFields> created_credential_fields(AttestationObject const& object, std::vector<std::string> transports)
{
    if (!object.parsed.attested)
        return std::nullopt;
    CreatedCredentialFields fields;
    fields.credential_id = object.parsed.attested->credential_id;
    fields.authenticator_data = object.auth_data;
    auto const& key = object.parsed.attested->public_key;
    fields.public_key_algorithm = key.alg.value_or(0);
    // getPublicKey() is null for an algorithm the user agent does not
    // know (§5.2.1.1); these are the ones it does.
    if (key.alg == cose::alg_es256 || key.alg == cose::alg_es384 || key.alg == cose::alg_es512 || key.alg == cose::alg_eddsa
        || key.alg == cose::alg_rs256)
        fields.public_key_spki = cose_key_to_spki(key);
    std::ranges::sort(transports);
    transports.erase(std::unique(transports.begin(), transports.end()), transports.end());
    fields.transports = std::move(transports);
    return fields;
}

namespace {

std::optional<std::vector<std::uint8_t>> certificate_at(cbor::Value const& statement, std::size_t index)
{
    cbor::Value const* const x5c = statement.find("x5c");
    if (x5c == nullptr || !x5c->is_array() || x5c->items().size() <= index || !x5c->items()[index].is_bytes())
        return std::nullopt;
    return x5c->items()[index].byte_vector();
}

}

StatementCheck verify_attestation_statement(AttestationObject const& object, std::span<std::uint8_t const> client_data_hash)
{
    if (!object.parsed.attested)
        return StatementCheck::Invalid;
    auto const& credential = *object.parsed.attested;
    std::vector<std::uint8_t> signed_data = object.auth_data;
    signed_data.insert(signed_data.end(), client_data_hash.begin(), client_data_hash.end());

    if (object.fmt == "none")
        return object.att_stmt.entries().empty() ? StatementCheck::Valid : StatementCheck::Invalid;

    if (object.fmt == "packed") {
        cbor::Value const* const alg = object.att_stmt.find("alg");
        cbor::Value const* const sig = object.att_stmt.find("sig");
        if (alg == nullptr || !alg->as_int64() || sig == nullptr || !sig->is_bytes())
            return StatementCheck::Invalid;
        if (object.att_stmt.find("x5c") == nullptr) {
            // Self attestation (§8.2 verification procedure, step 3).
            if (credential.public_key.alg != *alg->as_int64())
                return StatementCheck::Invalid;
            return cose_verify(credential.public_key, *alg->as_int64(), signed_data, sig->as_bytes()) ? StatementCheck::Valid
                                                                                                     : StatementCheck::Invalid;
        }
        auto const der = certificate_at(object.att_stmt, 0);
        if (!der)
            return StatementCheck::Invalid;
        auto const certificate = tls::parse_certificate(*der);
        if (!certificate || certificate->public_key.kind != tls::PublicKey::Kind::Ec)
            return StatementCheck::Unsupported;
        CoseKey const key = cose_key_from_ec(certificate->public_key.curve, certificate->public_key.ec, *alg->as_int64());
        return cose_verify(key, *alg->as_int64(), signed_data, sig->as_bytes()) ? StatementCheck::Valid : StatementCheck::Invalid;
    }

    if (object.fmt == "fido-u2f") {
        cbor::Value const* const sig = object.att_stmt.find("sig");
        cbor::Value const* const x5c = object.att_stmt.find("x5c");
        if (sig == nullptr || !sig->is_bytes() || x5c == nullptr || !x5c->is_array() || x5c->items().size() != 1)
            return StatementCheck::Invalid;
        auto const der = certificate_at(object.att_stmt, 0);
        auto const certificate = der ? tls::parse_certificate(*der) : std::nullopt;
        if (!certificate || certificate->public_key.kind != tls::PublicKey::Kind::Ec || certificate->public_key.curve != crypto::CurveId::P256)
            return StatementCheck::Invalid;
        auto const point = cose_key_ec_point(credential.public_key);
        if (!point || point->first != crypto::CurveId::P256)
            return StatementCheck::Invalid;
        // §8.6 step 5: 0x00 || rpIdHash || clientDataHash || credentialId || publicKeyU2F.
        std::vector<std::uint8_t> data { 0x00 };
        data.insert(data.end(), object.parsed.rp_id_hash.begin(), object.parsed.rp_id_hash.end());
        data.insert(data.end(), client_data_hash.begin(), client_data_hash.end());
        data.insert(data.end(), credential.credential_id.begin(), credential.credential_id.end());
        auto const u2f_key = crypto::ec_encode_point(crypto::CurveId::P256, point->second);
        data.insert(data.end(), u2f_key.begin(), u2f_key.end());
        CoseKey const key = cose_key_from_ec(crypto::CurveId::P256, certificate->public_key.ec, cose::alg_es256);
        return cose_verify(key, cose::alg_es256, data, sig->as_bytes()) ? StatementCheck::Valid : StatementCheck::Invalid;
    }
    return StatementCheck::Unsupported;
}

bool verify_assertion_signature(CoseKey const& key, std::int64_t alg, std::span<std::uint8_t const> authenticator_data,
    std::span<std::uint8_t const> client_data_hash, std::span<std::uint8_t const> signature)
{
    std::vector<std::uint8_t> data(authenticator_data.begin(), authenticator_data.end());
    data.insert(data.end(), client_data_hash.begin(), client_data_hash.end());
    return cose_verify(key, alg, data, signature);
}

}
