#include "auth/PinProtocol.h"

#include "crypto/Aes.h"
#include "crypto/AesModes.h"
#include "crypto/Hkdf.h"
#include "crypto/Hmac.h"
#include "crypto/Sha2.h"

#include <algorithm>
#include <string_view>

namespace sashfold::auth::pin {

std::optional<Protocol> protocol_from_number(std::int64_t number)
{
    if (number == 1)
        return Protocol::One;
    if (number == 2)
        return Protocol::Two;
    return std::nullopt;
}

std::vector<std::uint8_t> kdf(Protocol protocol, std::span<std::uint8_t const> z)
{
    if (protocol == Protocol::One) {
        auto const digest = crypto::Sha256::hash(z);
        return std::vector<std::uint8_t>(digest.begin(), digest.end());
    }
    // Two separate HKDF invocations (§6.5.7), not one of 64 bytes.
    std::array<std::uint8_t, 32> const salt {};
    auto const prk = crypto::hkdf_extract<crypto::Sha256>(salt, z);
    std::string_view const hmac_info = "CTAP2 HMAC key";
    std::string_view const aes_info = "CTAP2 AES key";
    auto out = crypto::hkdf_expand<crypto::Sha256>(prk,
        std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(hmac_info.data()), hmac_info.size()), 32);
    auto const aes_key = crypto::hkdf_expand<crypto::Sha256>(prk,
        std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(aes_info.data()), aes_info.size()), 32);
    out.insert(out.end(), aes_key.begin(), aes_key.end());
    return out;
}

std::optional<KeyAgreementKey> key_agreement_key_from_scalar(std::span<std::uint8_t const, 32> scalar)
{
    if (!crypto::ec_private_scalar_valid(crypto::CurveId::P256, scalar))
        return std::nullopt;
    auto const point = crypto::ec_public_point(crypto::CurveId::P256, scalar);
    if (!point)
        return std::nullopt;
    KeyAgreementKey key;
    std::copy(scalar.begin(), scalar.end(), key.scalar.begin());
    key.public_key = cose_key_from_ec(crypto::CurveId::P256, *point, cose::alg_ecdh_es_hkdf_256);
    return key;
}

std::optional<KeyAgreementKey> generate_key_agreement_key(crypto::RandomFill random)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::array<std::uint8_t, 32> scalar {};
        random(scalar);
        if (auto key = key_agreement_key_from_scalar(scalar))
            return key;
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> ecdh(Protocol protocol, KeyAgreementKey const& own, CoseKey const& peer)
{
    // The peer's alg is -25 by the specification, though not the algorithm
    // actually used; only the curve and the point matter.
    CoseKey point_only = peer;
    point_only.alg.reset();
    auto const point = cose_key_ec_point(point_only);
    if (!point || point->first != crypto::CurveId::P256)
        return std::nullopt;
    auto const z = crypto::ecdh_shared_x(crypto::CurveId::P256, own.scalar, point->second);
    if (!z)
        return std::nullopt;
    return kdf(protocol, *z);
}

std::optional<Encapsulation> encapsulate(Protocol protocol, CoseKey const& authenticator_key, crypto::RandomFill random)
{
    auto const own = generate_key_agreement_key(random);
    if (!own)
        return std::nullopt;
    auto shared = ecdh(protocol, *own, authenticator_key);
    if (!shared)
        return std::nullopt;
    return Encapsulation { own->public_key, std::move(*shared) };
}

namespace {

std::optional<crypto::Aes> aes_for(Protocol protocol, std::span<std::uint8_t const> key)
{
    // Protocol two's shared secret is HMAC key || AES key; its tokens are
    // 32 bytes and are never used to encrypt.
    if (protocol == Protocol::Two) {
        if (key.size() != 64)
            return std::nullopt;
        return crypto::Aes::with_key(key.subspan(32));
    }
    if (key.size() != 32)
        return std::nullopt;
    return crypto::Aes::with_key(key);
}

}

std::optional<std::vector<std::uint8_t>> encrypt_with_iv(std::span<std::uint8_t const> key, std::span<std::uint8_t const, 16> iv,
    std::span<std::uint8_t const> plaintext)
{
    auto const aes = aes_for(Protocol::Two, key);
    if (!aes)
        return std::nullopt;
    crypto::AesBlock block {};
    std::copy(iv.begin(), iv.end(), block.begin());
    auto ciphertext = crypto::aes_cbc_encrypt_unpadded(*aes, block, plaintext);
    if (!ciphertext)
        return std::nullopt;
    std::vector<std::uint8_t> out(iv.begin(), iv.end());
    out.insert(out.end(), ciphertext->begin(), ciphertext->end());
    return out;
}

std::optional<std::vector<std::uint8_t>> encrypt(Protocol protocol, std::span<std::uint8_t const> key, std::span<std::uint8_t const> plaintext,
    crypto::RandomFill random)
{
    if (protocol == Protocol::Two) {
        std::array<std::uint8_t, 16> iv {};
        random(iv);
        return encrypt_with_iv(key, iv, plaintext);
    }
    auto const aes = aes_for(protocol, key);
    if (!aes)
        return std::nullopt;
    return crypto::aes_cbc_encrypt_unpadded(*aes, crypto::AesBlock {}, plaintext);
}

std::optional<std::vector<std::uint8_t>> decrypt(Protocol protocol, std::span<std::uint8_t const> key, std::span<std::uint8_t const> ciphertext)
{
    auto const aes = aes_for(protocol, key);
    if (!aes)
        return std::nullopt;
    if (protocol == Protocol::Two) {
        if (ciphertext.size() < 16)
            return std::nullopt;
        crypto::AesBlock iv {};
        std::copy_n(ciphertext.begin(), 16, iv.begin());
        return crypto::aes_cbc_decrypt_unpadded(*aes, iv, ciphertext.subspan(16));
    }
    return crypto::aes_cbc_decrypt_unpadded(*aes, crypto::AesBlock {}, ciphertext);
}

std::vector<std::uint8_t> authenticate(Protocol protocol, std::span<std::uint8_t const> key, std::span<std::uint8_t const> message)
{
    if (protocol == Protocol::Two && key.size() > 32)
        key = key.first(32);
    auto const mac = crypto::Hmac<crypto::Sha256>::mac(key, message);
    std::size_t const length = protocol == Protocol::One ? 16 : 32;
    return std::vector<std::uint8_t>(mac.begin(), mac.begin() + static_cast<std::ptrdiff_t>(length));
}

bool verify(Protocol protocol, std::span<std::uint8_t const> key, std::span<std::uint8_t const> message, std::span<std::uint8_t const> signature)
{
    auto const expected = authenticate(protocol, key, message);
    return crypto::constant_time_equal(expected, signature);
}

std::optional<std::vector<std::uint8_t>> padded_pin(std::string_view pin)
{
    std::size_t code_points = 0;
    for (char const c : pin) {
        if ((static_cast<unsigned char>(c) & 0xc0) != 0x80)
            ++code_points;
    }
    if (code_points < 4 || pin.size() > 63)
        return std::nullopt;
    std::vector<std::uint8_t> out(64, 0);
    std::copy(pin.begin(), pin.end(), out.begin());
    return out;
}

std::array<std::uint8_t, 16> pin_hash(std::string_view pin)
{
    auto const digest = crypto::Sha256::hash(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(pin.data()), pin.size()));
    std::array<std::uint8_t, 16> out {};
    std::copy_n(digest.begin(), 16, out.begin());
    return out;
}

}
