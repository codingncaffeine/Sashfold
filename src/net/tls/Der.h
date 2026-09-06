#pragma once
// A bounds-checked reader for DER (ITU-T X.690) as X.509 uses it: a tag,
// a definite length in its shortest form, the content. Every read checks
// its bytes and answers nullopt on any deviation — an indefinite length,
// a high tag number, a non-minimal length, an overrun — because the
// bytes come from a stranger's server. Decoders for the primitive types
// certificates and CRLs carry follow; nothing else is decoded.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sashfold::tls {

enum class DerClass : std::uint8_t { Universal = 0, Application = 1, Context = 2, Private = 3 };

enum class DerType : std::uint8_t {
    Boolean = 1,
    Integer = 2,
    BitString = 3,
    OctetString = 4,
    Null = 5,
    Oid = 6,
    Utf8String = 12,
    Sequence = 16,
    Set = 17,
    PrintableString = 19,
    T61String = 20,
    Ia5String = 22,
    UtcTime = 23,
    GeneralizedTime = 24,
    VisibleString = 26,
    BmpString = 30,
};

struct DerTag {
    DerClass cls = DerClass::Universal;
    bool constructed = false;
    std::uint8_t number = 0;

    bool is(DerType type) const { return cls == DerClass::Universal && number == static_cast<std::uint8_t>(type); }
    bool is_context(std::uint8_t n) const { return cls == DerClass::Context && number == n; }
};

struct DerElement {
    DerTag tag;
    std::span<std::uint8_t const> content;
    std::span<std::uint8_t const> whole; // tag, length and content: what a signature covers
};

class DerReader {
public:
    explicit DerReader(std::span<std::uint8_t const> bytes)
        : m_bytes(bytes)
    {
    }

    bool at_end() const { return m_offset >= m_bytes.size(); }
    std::size_t remaining() const { return m_bytes.size() - m_offset; }

    // The next element, consumed; nullopt on any malformation.
    std::optional<DerElement> next();
    // The next element's tag, without consuming it.
    std::optional<DerTag> peek_tag() const;
    // The next element when it has this universal type; consumed only then.
    std::optional<DerElement> next(DerType type);
    // The next element when it is context-specific [n]; consumed only then.
    std::optional<DerElement> next_context(std::uint8_t number);
    // Whether the next element is [n], consuming nothing.
    bool peek_context(std::uint8_t number) const;

private:
    std::span<std::uint8_t const> m_bytes;
    std::size_t m_offset = 0;
};

// Decoders of content bytes. Each refuses what X.690 forbids for DER.
std::optional<bool> der_boolean(DerElement const& element);
// An INTEGER's magnitude, big-endian, leading zero stripped; negatives refused.
std::optional<std::span<std::uint8_t const>> der_integer(DerElement const& element);
// An INTEGER that fits 64 bits.
std::optional<std::uint64_t> der_small_integer(DerElement const& element);
// An OBJECT IDENTIFIER in dotted form ("1.2.840.113549.1.1.11").
std::optional<std::string> der_oid(DerElement const& element);
// A BIT STRING's bytes when it has no unused bits (keys and signatures).
std::optional<std::span<std::uint8_t const>> der_bit_string(DerElement const& element);
// The unused-bit count and the bytes, for KeyUsage.
std::optional<std::pair<std::uint8_t, std::span<std::uint8_t const>>> der_bit_string_with_unused(DerElement const& element);
// A character string of any of the types above, as UTF-8 (BMPString is
// converted; T61String is taken as Latin-1).
std::optional<std::string> der_string(DerElement const& element);
// UTCTime (YYMMDDHHMMSSZ, 1950–2049) or GeneralizedTime (YYYYMMDDHHMMSSZ)
// as seconds since 1970-01-01T00:00:00Z.
std::optional<std::int64_t> der_time(DerElement const& element);

// The seconds since the epoch of a proleptic Gregorian date and time.
std::int64_t seconds_from_civil(int year, int month, int day, int hour, int minute, int second);

}
