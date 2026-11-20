#include "auth/Cose.h"

#include "auth/DerWriter.h"
#include "crypto/Ed25519.h"
#include "crypto/Sha2.h"
#include "net/tls/X509.h"

#include <algorithm>

namespace sashfold::auth {

namespace {

std::optional<crypto::CurveId> curve_of(std::int64_t crv)
{
    switch (crv) {
    case cose::crv_p256:
        return crypto::CurveId::P256;
    case cose::crv_p384:
        return crypto::CurveId::P384;
    case cose::crv_p521:
        return crypto::CurveId::P521;
    }
    return std::nullopt;
}

std::int64_t crv_of(crypto::CurveId curve)
{
    switch (curve) {
    case crypto::CurveId::P256:
        return cose::crv_p256;
    case crypto::CurveId::P384:
        return cose::crv_p384;
    case crypto::CurveId::P521:
        return cose::crv_p521;
    }
    return 0;
}

std::optional<std::int64_t> int_at(cbor::Value const& map, std::int64_t label)
{
    cbor::Value const* const v = map.find(label);
    if (v == nullptr)
        return std::nullopt;
    return v->as_int64();
}

std::optional<std::vector<std::uint8_t>> bytes_at(cbor::Value const& map, std::int64_t label)
{
    cbor::Value const* const v = map.find(label);
    if (v == nullptr || !v->is_bytes())
        return std::nullopt;
    return v->byte_vector();
}

}

std::optional<CoseKey> cose_key_from_cbor(cbor::Value const& value)
{
    if (!value.is_map())
        return std::nullopt;
    CoseKey key;
    auto const kty = int_at(value, 1);
    if (!kty)
        return std::nullopt;
    key.kty = *kty;
    if (cbor::Value const* const alg = value.find(3)) {
        // An alg may be a text name in COSE; WebAuthn's are integers.
        if (!alg->is_integer() || !alg->as_int64())
            return std::nullopt;
        key.alg = *alg->as_int64();
    }
    switch (key.kty) {
    case cose::kty_ec2: {
        auto const crv = int_at(value, -1);
        auto x = bytes_at(value, -2);
        auto y = bytes_at(value, -3);
        if (!crv || !x || !y)
            return std::nullopt; // compressed points (y a boolean) are not used by WebAuthn
        auto const curve = curve_of(*crv);
        if (!curve)
            return std::nullopt;
        std::size_t const size = crypto::curve(*curve).field_bytes;
        if (x->size() != size || y->size() != size)
            return std::nullopt;
        key.crv = *crv;
        key.x = std::move(*x);
        key.y = std::move(*y);
        return key;
    }
    case cose::kty_okp: {
        auto const crv = int_at(value, -1);
        auto x = bytes_at(value, -2);
        if (!crv || !x || *crv != cose::crv_ed25519 || x->size() != 32)
            return std::nullopt;
        key.crv = *crv;
        key.x = std::move(*x);
        return key;
    }
    case cose::kty_rsa: {
        auto n = bytes_at(value, -1);
        auto e = bytes_at(value, -2);
        if (!n || !e || n->empty() || e->empty() || n->size() > 512 || e->size() > 8)
            return std::nullopt;
        key.n = std::move(*n);
        key.e = std::move(*e);
        return key;
    }
    }
    return std::nullopt;
}

cbor::Value cose_key_to_cbor(CoseKey const& key)
{
    cbor::Value map = cbor::Value::map();
    map.set(1, cbor::Value::integer(key.kty));
    if (key.alg)
        map.set(3, cbor::Value::integer(*key.alg));
    if (key.kty == cose::kty_ec2) {
        map.set(-1, cbor::Value::integer(key.crv));
        map.set(-2, cbor::Value::bytes(key.x));
        map.set(-3, cbor::Value::bytes(key.y));
    } else if (key.kty == cose::kty_okp) {
        map.set(-1, cbor::Value::integer(key.crv));
        map.set(-2, cbor::Value::bytes(key.x));
    } else if (key.kty == cose::kty_rsa) {
        map.set(-1, cbor::Value::bytes(key.n));
        map.set(-2, cbor::Value::bytes(key.e));
    }
    return map;
}

CoseKey cose_key_from_ec(crypto::CurveId curve, crypto::EcPoint const& point, std::int64_t alg)
{
    CoseKey key;
    key.kty = cose::kty_ec2;
    key.alg = alg;
    key.crv = crv_of(curve);
    std::size_t const size = crypto::curve(curve).field_bytes;
    key.x = point.x.to_bytes(size).value_or(std::vector<std::uint8_t>(size));
    key.y = point.y.to_bytes(size).value_or(std::vector<std::uint8_t>(size));
    return key;
}

CoseKey cose_key_from_ed25519(std::span<std::uint8_t const, 32> public_key)
{
    CoseKey key;
    key.kty = cose::kty_okp;
    key.alg = cose::alg_eddsa;
    key.crv = cose::crv_ed25519;
    key.x.assign(public_key.begin(), public_key.end());
    return key;
}

CoseKey cose_key_from_rsa(crypto::RsaPublicKey const& rsa)
{
    CoseKey key;
    key.kty = cose::kty_rsa;
    key.alg = cose::alg_rs256;
    key.n = rsa.n.to_bytes();
    key.e = rsa.e.to_bytes();
    return key;
}

std::optional<std::pair<crypto::CurveId, crypto::EcPoint>> cose_key_ec_point(CoseKey const& key)
{
    if (key.kty != cose::kty_ec2)
        return std::nullopt;
    auto const curve = curve_of(key.crv);
    if (!curve)
        return std::nullopt;
    std::size_t const size = crypto::curve(*curve).field_bytes;
    if (key.x.size() != size || key.y.size() != size)
        return std::nullopt;
    std::vector<std::uint8_t> encoded;
    encoded.reserve(1 + 2 * size);
    encoded.push_back(0x04);
    encoded.insert(encoded.end(), key.x.begin(), key.x.end());
    encoded.insert(encoded.end(), key.y.begin(), key.y.end());
    auto point = crypto::ec_decode_point(*curve, encoded);
    if (!point)
        return std::nullopt;
    return std::pair<crypto::CurveId, crypto::EcPoint>(*curve, std::move(*point));
}

std::optional<crypto::RsaPublicKey> cose_key_rsa(CoseKey const& key)
{
    if (key.kty != cose::kty_rsa)
        return std::nullopt;
    auto n = crypto::BigInt::from_bytes(key.n);
    auto e = crypto::BigInt::from_bytes(key.e);
    if (!n || !e || !n->is_odd() || !e->is_odd() || n->bit_length() < 512 || n->bit_length() > 4096 || e->bit_length() < 2)
        return std::nullopt;
    return crypto::RsaPublicKey { *n, *e };
}

std::optional<std::int64_t> cose_key_algorithm(CoseKey const& key) { return key.alg; }

std::optional<std::vector<std::uint8_t>> cose_key_to_spki(CoseKey const& key)
{
    if (key.kty == cose::kty_ec2) {
        auto const point = cose_key_ec_point(key);
        if (!point)
            return std::nullopt;
        std::string_view curve_oid;
        switch (point->first) {
        case crypto::CurveId::P256:
            curve_oid = tls::oid::prime256v1;
            break;
        case crypto::CurveId::P384:
            curve_oid = tls::oid::secp384r1;
            break;
        case crypto::CurveId::P521:
            curve_oid = "1.3.132.0.35";
            break;
        }
        auto const encoded = crypto::ec_encode_point(point->first, point->second);
        return der::sequence({ der::sequence({ der::oid(tls::oid::ec_public_key), der::oid(curve_oid) }), der::bit_string(encoded) });
    }
    if (key.kty == cose::kty_okp) {
        if (key.crv != cose::crv_ed25519 || key.x.size() != 32)
            return std::nullopt;
        crypto::Ed25519PublicKey public_key;
        std::copy(key.x.begin(), key.x.end(), public_key.begin());
        if (!crypto::ed25519_point_valid(public_key))
            return std::nullopt;
        return der::sequence({ der::sequence({ der::oid("1.3.101.112") }), der::bit_string(key.x) });
    }
    if (key.kty == cose::kty_rsa) {
        if (!cose_key_rsa(key))
            return std::nullopt;
        auto const rsa_key = der::sequence({ der::integer(key.n), der::integer(key.e) });
        return der::sequence({ der::sequence({ der::oid(tls::oid::rsa_encryption), der::null() }), der::bit_string(rsa_key) });
    }
    return std::nullopt;
}

bool cose_verify(CoseKey const& key, std::int64_t alg, std::span<std::uint8_t const> data, std::span<std::uint8_t const> signature)
{
    if (key.alg && *key.alg != alg)
        return false;
    switch (alg) {
    case cose::alg_es256:
    case cose::alg_es384:
    case cose::alg_es512: {
        auto const point = cose_key_ec_point(key);
        if (!point)
            return false;
        crypto::CurveId const wanted = alg == cose::alg_es256 ? crypto::CurveId::P256
            : alg == cose::alg_es384                          ? crypto::CurveId::P384
                                                              : crypto::CurveId::P521;
        if (point->first != wanted)
            return false;
        auto const rs = der::parse_ecdsa_signature(signature);
        if (!rs)
            return false;
        crypto::HashId const hash = alg == cose::alg_es256 ? crypto::HashId::Sha256
            : alg == cose::alg_es384                       ? crypto::HashId::Sha384
                                                           : crypto::HashId::Sha512;
        auto const digest = crypto::hash_with(hash, data);
        return crypto::ecdsa_verify(point->first, point->second, digest, rs->first, rs->second);
    }
    case cose::alg_eddsa: {
        if (key.kty != cose::kty_okp || key.crv != cose::crv_ed25519 || key.x.size() != 32 || signature.size() != 64)
            return false;
        crypto::Ed25519PublicKey public_key;
        std::copy(key.x.begin(), key.x.end(), public_key.begin());
        return crypto::ed25519_verify(public_key, data, signature);
    }
    case cose::alg_rs256: {
        auto const rsa = cose_key_rsa(key);
        if (!rsa)
            return false;
        auto const digest = crypto::Sha256::hash(data);
        return crypto::rsa_verify_pkcs1_v15_any(*rsa, crypto::HashId::Sha256, digest, signature);
    }
    }
    return false;
}

}
