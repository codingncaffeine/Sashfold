#include "auth/DerWriter.h"

#include "net/tls/Der.h"

#include <algorithm>
#include <charconv>

namespace sashfold::auth::der {

std::vector<std::uint8_t> element(std::uint8_t tag, std::span<std::uint8_t const> content)
{
    // The tag and the length octets first, in an array of their own: the
    // result is then made at its final size and never grows.
    std::uint8_t header[2 + sizeof(std::size_t)];
    std::size_t used = 0;
    header[used++] = tag;
    std::size_t const length = content.size();
    if (length < 0x80) {
        header[used++] = static_cast<std::uint8_t>(length);
    } else {
        std::uint8_t length_bytes[sizeof(std::size_t)];
        std::size_t count = 0;
        for (std::size_t value = length; value != 0; value >>= 8)
            length_bytes[count++] = static_cast<std::uint8_t>(value);
        header[used++] = static_cast<std::uint8_t>(0x80 | count);
        while (count > 0)
            header[used++] = length_bytes[--count];
    }
    std::vector<std::uint8_t> out(used + length);
    std::copy_n(header, used, out.begin());
    std::copy(content.begin(), content.end(), out.begin() + static_cast<std::ptrdiff_t>(used));
    return out;
}

namespace {

std::vector<std::uint8_t> joined(std::initializer_list<std::span<std::uint8_t const>> parts)
{
    std::vector<std::uint8_t> content;
    for (auto const part : parts)
        content.insert(content.end(), part.begin(), part.end());
    return content;
}

}

std::vector<std::uint8_t> sequence(std::initializer_list<std::span<std::uint8_t const>> parts) { return element(0x30, joined(parts)); }
std::vector<std::uint8_t> set(std::initializer_list<std::span<std::uint8_t const>> parts) { return element(0x31, joined(parts)); }

std::vector<std::uint8_t> integer(std::span<std::uint8_t const> magnitude)
{
    std::size_t start = 0;
    while (start < magnitude.size() && magnitude[start] == 0)
        ++start;
    std::vector<std::uint8_t> content;
    if (start == magnitude.size() || (magnitude[start] & 0x80) != 0)
        content.push_back(0);
    content.insert(content.end(), magnitude.begin() + static_cast<std::ptrdiff_t>(start), magnitude.end());
    return element(0x02, content);
}

std::vector<std::uint8_t> small_integer(std::uint64_t value)
{
    std::uint8_t bytes[8];
    for (int i = 7; i >= 0; --i) {
        bytes[i] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
    return integer(bytes);
}

std::vector<std::uint8_t> oid(std::string_view dotted)
{
    std::vector<std::uint64_t> arcs;
    while (!dotted.empty()) {
        std::size_t const dot = dotted.find('.');
        std::string_view const part = dotted.substr(0, dot);
        std::uint64_t arc = 0;
        std::from_chars(part.data(), part.data() + part.size(), arc);
        arcs.push_back(arc);
        if (dot == std::string_view::npos)
            break;
        dotted.remove_prefix(dot + 1);
    }
    std::vector<std::uint8_t> content;
    auto const base128 = [&content](std::uint64_t value) {
        std::uint8_t groups[10];
        std::size_t count = 0;
        do {
            groups[count++] = static_cast<std::uint8_t>(value & 0x7f);
            value >>= 7;
        } while (value != 0);
        while (count > 1)
            content.push_back(static_cast<std::uint8_t>(groups[--count] | 0x80));
        content.push_back(groups[0]);
    };
    if (arcs.size() >= 2) {
        base128(arcs[0] * 40 + arcs[1]);
        for (std::size_t i = 2; i < arcs.size(); ++i)
            base128(arcs[i]);
    }
    return element(0x06, content);
}

std::vector<std::uint8_t> null() { return { 0x05, 0x00 }; }

std::vector<std::uint8_t> bit_string(std::span<std::uint8_t const> bytes)
{
    // The first octet counts the unused bits of the last one: none.
    std::vector<std::uint8_t> content(bytes.size() + 1);
    std::copy(bytes.begin(), bytes.end(), content.begin() + 1);
    return element(0x03, content);
}

std::vector<std::uint8_t> octet_string(std::span<std::uint8_t const> bytes) { return element(0x04, bytes); }

std::vector<std::uint8_t> utf8_string(std::string_view text)
{
    return element(0x0c, std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(text.data()), text.size()));
}

std::vector<std::uint8_t> generalized_time(std::string_view text)
{
    return element(0x18, std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(text.data()), text.size()));
}

std::vector<std::uint8_t> explicit_tag(std::uint8_t number, std::span<std::uint8_t const> content)
{
    return element(static_cast<std::uint8_t>(0xa0 | number), content);
}

std::vector<std::uint8_t> ecdsa_signature(crypto::BigInt const& r, crypto::BigInt const& s)
{
    return sequence({ integer(r.to_bytes()), integer(s.to_bytes()) });
}

std::optional<std::pair<crypto::BigInt, crypto::BigInt>> parse_ecdsa_signature(std::span<std::uint8_t const> bytes)
{
    tls::DerReader outer(bytes);
    auto const seq = outer.next(tls::DerType::Sequence);
    if (!seq || !outer.at_end())
        return std::nullopt;
    tls::DerReader inner(seq->content);
    auto const r_element = inner.next(tls::DerType::Integer);
    auto const s_element = inner.next(tls::DerType::Integer);
    if (!r_element || !s_element || !inner.at_end())
        return std::nullopt;
    auto const r = tls::der_integer(*r_element);
    auto const s = tls::der_integer(*s_element);
    if (!r || !s || r->size() > 66 || s->size() > 66)
        return std::nullopt;
    auto r_value = crypto::BigInt::from_bytes(*r);
    auto s_value = crypto::BigInt::from_bytes(*s);
    if (!r_value || !s_value)
        return std::nullopt;
    return std::pair<crypto::BigInt, crypto::BigInt>(*r_value, *s_value);
}

}
