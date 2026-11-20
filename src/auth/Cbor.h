#pragma once
// CBOR (RFC 8949) as the security-key protocols use it.
//
// Every CTAP2 message, both ways, is in the "CTAP2 canonical CBOR encoding
// form" (CTAP 2.1 §8): integers and lengths in their shortest form,
// definite lengths only, map keys sorted by major type, then by encoded
// length, then byte by byte, no duplicate keys, no tags, and no more than
// four levels of nested arrays and maps. The encoder writes only that form;
// the strict decoder takes only that form and nothing after it.
//
// Attestation objects and COSE keys reach the page and the relying party
// in the same form when they come from a CTAP2 key, but WebAuthn names the
// general CBOR of RFC 8949 for them, so a lenient mode reads any
// well-formed item (indefinite lengths, tags, unsorted keys, longer
// integer forms) — still refusing duplicate keys, invalid UTF-8 and
// anything that runs past the end.
//
// The bytes come from a device or a page, so every length is checked
// against what remains before anything is read or reserved, and nesting is
// bounded so that no input can exhaust the stack.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::auth::cbor {

enum class Type : std::uint8_t {
    Unsigned, // major type 0
    Negative, // major type 1: the value is −1 − the stored number
    Bytes, // 2
    Text, // 3
    Array, // 4
    Map, // 5
    Tag, // 6: one tagged item
    Simple, // 7: false (20), true (21), null (22), undefined (23), others
    Float, // 7: half, single or double, its width kept (CTAP 2.1 §8)
};

class Value {
public:
    Value() = default; // null

    static Value unsigned_integer(std::uint64_t value);
    // A negative number −1 − n, which reaches −2^64.
    static Value negative_minus_one(std::uint64_t n);
    static Value integer(std::int64_t value);
    static Value bytes(std::span<std::uint8_t const> value);
    static Value bytes(std::vector<std::uint8_t> value);
    static Value text(std::string_view value);
    static Value array(std::vector<Value> items = {});
    static Value map(std::vector<std::pair<Value, Value>> entries = {});
    static Value tag(std::uint64_t number, Value item);
    static Value boolean(bool value);
    static Value null();
    static Value undefined();
    static Value simple(std::uint8_t value);
    // width is 2, 4 or 8 bytes; a half or single holds what the double
    // given rounds to (only lossless values are written by this codec).
    static Value floating(double value, std::uint8_t width = 8);

    Type type() const { return m_type; }
    bool is_integer() const { return m_type == Type::Unsigned || m_type == Type::Negative; }
    bool is_unsigned() const { return m_type == Type::Unsigned; }
    bool is_bytes() const { return m_type == Type::Bytes; }
    bool is_text() const { return m_type == Type::Text; }
    bool is_array() const { return m_type == Type::Array; }
    bool is_map() const { return m_type == Type::Map; }
    bool is_bool() const { return m_type == Type::Simple && (m_number == 20 || m_number == 21); }
    bool is_null() const { return m_type == Type::Simple && m_number == 22; }

    // The integer when it fits int64_t; nullopt for other types and for
    // magnitudes beyond it.
    std::optional<std::int64_t> as_int64() const;
    // The stored number: an unsigned value, n of −1 − n, a tag number, a
    // simple value.
    std::uint64_t raw_number() const { return m_number; }
    std::optional<bool> as_bool() const;
    double as_double() const { return m_float; }
    std::uint8_t float_width() const { return static_cast<std::uint8_t>(m_number); }

    // Bytes and text share their storage; text is UTF-8.
    std::span<std::uint8_t const> as_bytes() const { return m_bytes; }
    std::vector<std::uint8_t> const& byte_vector() const { return m_bytes; }
    std::string_view as_text() const
    {
        return std::string_view(reinterpret_cast<char const*>(m_bytes.data()), m_bytes.size());
    }

    std::vector<Value> const& items() const { return m_items; }
    std::vector<Value>& items() { return m_items; }
    std::vector<std::pair<Value, Value>> const& entries() const { return m_entries; }
    std::vector<std::pair<Value, Value>>& entries() { return m_entries; }
    // The tagged item (Type::Tag).
    Value const& tagged() const { return m_items.front(); }

    // A map's value under an integer or a text key; null when absent or
    // when this is not a map.
    Value const* find(std::int64_t key) const;
    Value const* find(std::string_view key) const;
    // Adds or replaces an entry of a map.
    Value& set(Value key, Value value);
    Value& set(std::int64_t key, Value value) { return set(Value::integer(key), std::move(value)); }
    Value& set(std::string_view key, Value value) { return set(Value::text(key), std::move(value)); }

    // Structural equality (map entries compared in order).
    bool operator==(Value const& other) const;

private:
    Type m_type = Type::Simple;
    std::uint64_t m_number = 22; // null
    double m_float = 0;
    std::vector<std::uint8_t> m_bytes;
    std::vector<Value> m_items;
    std::vector<std::pair<Value, Value>> m_entries;
};

// The canonical form of CTAP 2.1 §8: maps are written with their keys
// sorted (the order they were added in does not matter); duplicates are
// the caller's error and are written as given.
std::vector<std::uint8_t> encode(Value const& value);
void encode_into(Value const& value, std::vector<std::uint8_t>& out);

enum class Mode : std::uint8_t {
    // CTAP2 canonical form; refuses tags and more than four levels of
    // nested arrays and maps.
    Ctap2,
    // Any well-formed item, up to 16 levels deep.
    General,
};

// One whole item and nothing after it; nullopt for anything malformed,
// non-canonical (in Ctap2 mode), truncated or followed by trailing bytes.
std::optional<Value> decode(std::span<std::uint8_t const> bytes, Mode mode = Mode::Ctap2);

// One item at the start of `bytes` and the number of bytes it took, for an
// item followed by others (the credential public key in authenticator data
// is followed by the extensions map).
std::optional<std::pair<Value, std::size_t>> decode_prefix(std::span<std::uint8_t const> bytes, Mode mode = Mode::Ctap2);

// Whether a map key `a` sorts before `b` in the canonical order.
bool canonical_key_less(std::span<std::uint8_t const> a, std::span<std::uint8_t const> b);

}
