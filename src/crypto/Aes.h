#pragma once
// AES-128 (FIPS 197) as the block cipher of TLS_AES_128_GCM_SHA256.
// Encryption only: GCM uses the forward direction alone — counter mode and
// the hash subkey — so no inverse cipher is written.
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

namespace sashfold::crypto {

using AesKey = std::array<std::uint8_t, 16>;
using AesBlock = std::array<std::uint8_t, 16>;

// Eight 16-bit planes: bit i of plane j is bit j of state byte i.
using AesPlanes = std::array<std::uint16_t, 8>;

class Aes128 {
public:
    explicit Aes128(AesKey const& key);

    AesBlock encrypt(AesBlock const& block) const;

private:
    // The eleven round keys of §5.2, each in the plane form the state uses.
    std::array<AesPlanes, 11> m_round_keys {};
};

}
