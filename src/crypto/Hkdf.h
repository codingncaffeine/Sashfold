#pragma once
// HKDF (RFC 5869): Extract and Expand over any of the SHA-2 classes. The
// TLS 1.3 labels (HKDF-Expand-Label, Derive-Secret) are built on these in
// net/tls/Tls13.cpp, where RFC 8446 defines them.
#include "crypto/Hmac.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sashfold::crypto {

// PRK = HMAC-Hash(salt, IKM). An absent salt is HashLen zeros in the RFC;
// HMAC pads any shorter key with zeros, so an empty span is the same key.
template <class Hash>
typename Hash::Digest hkdf_extract(std::span<std::uint8_t const> salt, std::span<std::uint8_t const> ikm)
{
    return Hmac<Hash>::mac(salt, ikm);
}

// OKM = the first `length` bytes of T(1) | T(2) | …, T(i) = HMAC-Hash(PRK,
// T(i-1) | info | i). `length` may be at most 255 × HashLen; more comes
// back empty.
template <class Hash>
std::vector<std::uint8_t> hkdf_expand(std::span<std::uint8_t const> prk, std::span<std::uint8_t const> info, std::size_t length)
{
    if (length > 255 * Hash::digest_size)
        return {};
    std::vector<std::uint8_t> out;
    out.reserve(length);
    typename Hash::Digest previous {};
    std::size_t previous_size = 0;
    for (std::uint8_t counter = 1; out.size() < length; ++counter) {
        Hmac<Hash> mac(prk);
        mac.update(std::span<std::uint8_t const>(previous.data(), previous_size));
        mac.update(info);
        mac.update(std::span<std::uint8_t const>(&counter, 1));
        previous = mac.finish();
        previous_size = previous.size();
        std::size_t const take = std::min(length - out.size(), previous_size);
        out.insert(out.end(), previous.begin(), previous.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return out;
}

}
