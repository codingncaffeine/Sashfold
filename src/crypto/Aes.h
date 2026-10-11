#pragma once
// AES (FIPS 197) with 128-, 192- and 256-bit keys, both directions: the
// block cipher of TLS_AES_128_GCM_SHA256 and of Web Crypto's AES-GCM,
// AES-CTR, AES-CBC and AES-KW (the last two need the inverse cipher).
//
// Constant time by construction. The state is held as eight bit planes,
// plane j carrying bit j of all sixteen state bytes, so SubBytes is a
// circuit and not a table: the field inverse is x^254 over GF(2^8) by an
// addition chain, and the affine map of §5.1.1 is a fixed set of XORs. No
// table is indexed by a secret byte and no branch depends on one, which is
// the promise a T-table implementation cannot make. Nothing here is of our
// own design.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sashfold::crypto {

using AesKey = std::array<std::uint8_t, 16>;
using AesBlock = std::array<std::uint8_t, 16>;

// Eight 16-bit planes: bit i of plane j is bit j of state byte i.
using AesPlanes = std::array<std::uint16_t, 8>;

class Aes {
public:
    // A key of 16, 24 or 32 bytes; nullopt for any other length.
    static std::optional<Aes> with_key(std::span<std::uint8_t const> key);

    AesBlock encrypt(AesBlock const& block) const;
    AesBlock decrypt(AesBlock const& block) const;

protected:
    explicit Aes(std::span<std::uint8_t const> key);

private:
    // Nr + 1 round keys of §5.2 (Nr = 10, 12 or 14), each in the plane form
    // the state uses.
    std::array<AesPlanes, 15> m_round_keys {};
    std::size_t m_rounds = 10;
};

class Aes128 : public Aes {
public:
    explicit Aes128(AesKey const& key)
        : Aes(key)
    {
    }
};

}
