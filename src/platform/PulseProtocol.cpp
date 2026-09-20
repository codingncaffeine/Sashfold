#include "platform/PulseProtocol.h"

#include <cstring>

namespace sashfold::platform::pulse {

namespace {

// The tags, each one byte, that say what the value after it is.
constexpr char tag_string = 't';
constexpr char tag_string_null = 'N';
constexpr char tag_u32 = 'L';
constexpr char tag_u8 = 'B';
constexpr char tag_u64 = 'R';
constexpr char tag_s64 = 'r';
constexpr char tag_sample_spec = 'a';
constexpr char tag_arbitrary = 'x';
constexpr char tag_true = '1';
constexpr char tag_false = '0';
constexpr char tag_timeval = 'T';
constexpr char tag_usec = 'U';
constexpr char tag_channel_map = 'm';
constexpr char tag_cvolume = 'v';
constexpr char tag_proplist = 'P';
constexpr char tag_volume = 'V';
constexpr char tag_format_info = 'f';

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t get_u32(std::span<std::uint8_t const> bytes, std::size_t at)
{
    return (static_cast<std::uint32_t>(bytes[at]) << 24) | (static_cast<std::uint32_t>(bytes[at + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[at + 2]) << 8) | static_cast<std::uint32_t>(bytes[at + 3]);
}

}

Writer::Writer(Command command, std::uint32_t serial)
{
    u32(static_cast<std::uint32_t>(command));
    u32(serial);
}

Writer& Writer::u8(std::uint8_t value)
{
    m_payload.push_back(static_cast<std::uint8_t>(tag_u8));
    m_payload.push_back(value);
    return *this;
}

Writer& Writer::u32(std::uint32_t value)
{
    m_payload.push_back(static_cast<std::uint8_t>(tag_u32));
    put_u32(m_payload, value);
    return *this;
}

Writer& Writer::u64(std::uint64_t value)
{
    m_payload.push_back(static_cast<std::uint8_t>(tag_u64));
    put_u32(m_payload, static_cast<std::uint32_t>(value >> 32));
    put_u32(m_payload, static_cast<std::uint32_t>(value));
    return *this;
}

Writer& Writer::boolean(bool value)
{
    m_payload.push_back(static_cast<std::uint8_t>(value ? tag_true : tag_false));
    return *this;
}

Writer& Writer::string(std::string_view text)
{
    m_payload.push_back(static_cast<std::uint8_t>(tag_string));
    m_payload.insert(m_payload.end(), text.begin(), text.end());
    m_payload.push_back(0);
    return *this;
}

Writer& Writer::null_string()
{
    m_payload.push_back(static_cast<std::uint8_t>(tag_string_null));
    return *this;
}

Writer& Writer::arbitrary(std::span<std::uint8_t const> bytes)
{
    m_payload.push_back(static_cast<std::uint8_t>(tag_arbitrary));
    put_u32(m_payload, static_cast<std::uint32_t>(bytes.size()));
    m_payload.insert(m_payload.end(), bytes.begin(), bytes.end());
    return *this;
}

Writer& Writer::sample_spec(SampleFormat format, std::uint8_t channels, std::uint32_t rate)
{
    m_payload.push_back(static_cast<std::uint8_t>(tag_sample_spec));
    m_payload.push_back(static_cast<std::uint8_t>(format));
    m_payload.push_back(channels);
    put_u32(m_payload, rate);
    return *this;
}

Writer& Writer::channel_map(std::span<std::uint8_t const> positions)
{
    m_payload.push_back(static_cast<std::uint8_t>(tag_channel_map));
    m_payload.push_back(static_cast<std::uint8_t>(positions.size()));
    m_payload.insert(m_payload.end(), positions.begin(), positions.end());
    return *this;
}

Writer& Writer::cvolume(std::span<std::uint32_t const> volumes)
{
    m_payload.push_back(static_cast<std::uint8_t>(tag_cvolume));
    m_payload.push_back(static_cast<std::uint8_t>(volumes.size()));
    for (std::uint32_t const volume : volumes)
        put_u32(m_payload, volume);
    return *this;
}

Writer& Writer::timeval(std::uint32_t seconds, std::uint32_t microseconds)
{
    m_payload.push_back(static_cast<std::uint8_t>(tag_timeval));
    put_u32(m_payload, seconds);
    put_u32(m_payload, microseconds);
    return *this;
}

Writer& Writer::proplist(std::span<std::pair<std::string, std::string> const> entries)
{
    // Each entry is its key, the length of its value, and the value as
    // bytes — with the terminator counted, the way a C string is stored —
    // and a null string closes the list.
    m_payload.push_back(static_cast<std::uint8_t>(tag_proplist));
    for (auto const& [key, value] : entries) {
        string(key);
        u32(static_cast<std::uint32_t>(value.size() + 1));
        std::vector<std::uint8_t> bytes(value.begin(), value.end());
        bytes.push_back(0);
        arbitrary(bytes);
    }
    null_string();
    return *this;
}

std::vector<std::uint8_t> Writer::packet() const
{
    std::vector<std::uint8_t> out;
    out.reserve(header_size + m_payload.size());
    put_u32(out, static_cast<std::uint32_t>(m_payload.size()));
    put_u32(out, command_channel);
    put_u32(out, 0); // offset, high
    put_u32(out, 0); // offset, low
    put_u32(out, 0); // flags
    out.insert(out.end(), m_payload.begin(), m_payload.end());
    return out;
}

std::optional<char> Reader::peek() const
{
    if (!m_ok || m_offset >= m_payload.size())
        return std::nullopt;
    return static_cast<char>(m_payload[m_offset]);
}

bool Reader::take(char tag)
{
    std::optional<char> const next = peek();
    if (!next || *next != tag) {
        m_ok = false;
        return false;
    }
    ++m_offset;
    return true;
}

std::optional<std::uint8_t> Reader::u8()
{
    if (!take(tag_u8) || m_offset + 1 > m_payload.size()) {
        m_ok = false;
        return std::nullopt;
    }
    return m_payload[m_offset++];
}

std::optional<std::uint32_t> Reader::u32()
{
    if (!take(tag_u32) || m_offset + 4 > m_payload.size()) {
        m_ok = false;
        return std::nullopt;
    }
    std::uint32_t const value = get_u32(m_payload, m_offset);
    m_offset += 4;
    return value;
}

std::optional<std::uint64_t> Reader::u64()
{
    std::optional<char> const next = peek();
    if (!next || (*next != tag_u64 && *next != tag_usec && *next != tag_s64)) {
        m_ok = false;
        return std::nullopt;
    }
    ++m_offset;
    if (m_offset + 8 > m_payload.size()) {
        m_ok = false;
        return std::nullopt;
    }
    std::uint64_t const value = (static_cast<std::uint64_t>(get_u32(m_payload, m_offset)) << 32) | get_u32(m_payload, m_offset + 4);
    m_offset += 8;
    return value;
}

std::optional<bool> Reader::boolean()
{
    std::optional<char> const next = peek();
    if (!next || (*next != tag_true && *next != tag_false)) {
        m_ok = false;
        return std::nullopt;
    }
    ++m_offset;
    return *next == tag_true;
}

std::optional<std::string> Reader::string()
{
    std::optional<char> const next = peek();
    if (next && *next == tag_string_null) {
        ++m_offset;
        return std::nullopt;
    }
    if (!take(tag_string))
        return std::nullopt;
    std::size_t end = m_offset;
    while (end < m_payload.size() && m_payload[end] != 0)
        ++end;
    if (end >= m_payload.size()) {
        m_ok = false;
        return std::nullopt;
    }
    std::string text(reinterpret_cast<char const*>(m_payload.data() + m_offset), end - m_offset);
    m_offset = end + 1;
    return text;
}

bool Reader::skip()
{
    std::optional<char> const next = peek();
    if (!next) {
        m_ok = false;
        return false;
    }
    // How many bytes follow the tag, for the fixed-width kinds.
    auto const fixed = [&](std::size_t width) {
        if (m_offset + 1 + width > m_payload.size()) {
            m_ok = false;
            return false;
        }
        m_offset += 1 + width;
        return true;
    };
    switch (*next) {
    case tag_true:
    case tag_false:
    case tag_string_null:
        ++m_offset;
        return true;
    case tag_u8:
        return fixed(1);
    case tag_u32:
    case tag_volume:
        return fixed(4);
    case tag_u64:
    case tag_s64:
    case tag_usec:
    case tag_timeval:
        return fixed(8);
    case tag_sample_spec:
        return fixed(6);
    case tag_string:
        return string().has_value() || m_ok;
    case tag_arbitrary: {
        if (m_offset + 5 > m_payload.size()) {
            m_ok = false;
            return false;
        }
        std::uint32_t const length = get_u32(m_payload, m_offset + 1);
        if (m_offset + 5 + length > m_payload.size()) {
            m_ok = false;
            return false;
        }
        m_offset += 5 + length;
        return true;
    }
    case tag_channel_map: {
        if (m_offset + 2 > m_payload.size()) {
            m_ok = false;
            return false;
        }
        return fixed(1 + m_payload[m_offset + 1]);
    }
    case tag_cvolume: {
        if (m_offset + 2 > m_payload.size()) {
            m_ok = false;
            return false;
        }
        return fixed(1 + static_cast<std::size_t>(m_payload[m_offset + 1]) * 4);
    }
    case tag_proplist: {
        ++m_offset;
        // Key, length, value, until the null string that ends the list.
        for (;;) {
            std::optional<char> const inner = peek();
            if (!inner) {
                m_ok = false;
                return false;
            }
            if (*inner == tag_string_null) {
                ++m_offset;
                return true;
            }
            if (!skip() || !skip() || !skip())
                return false;
        }
    }
    case tag_format_info:
        // An encoding and then a property list.
        if (m_offset + 2 > m_payload.size()) {
            m_ok = false;
            return false;
        }
        m_offset += 2;
        return skip();
    default:
        m_ok = false;
        return false;
    }
}

std::optional<Header> read_header(std::span<std::uint8_t const> bytes)
{
    if (bytes.size() < header_size)
        return std::nullopt;
    Header header;
    header.length = get_u32(bytes, 0);
    header.channel = get_u32(bytes, 4);
    header.offset = (static_cast<std::uint64_t>(get_u32(bytes, 8)) << 32) | get_u32(bytes, 12);
    header.flags = get_u32(bytes, 16);
    return header;
}

std::vector<std::uint8_t> block_packet(std::uint32_t channel, std::span<std::uint8_t const> samples)
{
    std::vector<std::uint8_t> out;
    out.reserve(header_size + samples.size());
    put_u32(out, static_cast<std::uint32_t>(samples.size()));
    put_u32(out, channel);
    put_u32(out, 0);
    put_u32(out, 0);
    put_u32(out, 0); // written where the stream stands: no seek
    out.insert(out.end(), samples.begin(), samples.end());
    return out;
}

}
