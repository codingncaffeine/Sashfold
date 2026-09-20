#pragma once

// The PulseAudio native protocol's wire format: packets and the tagged
// values their payloads carry. This is the transport only — which commands
// exist and what the server does with them is the device's business
// (platform/linux/AudioPulse.cpp). It is here, built on every system rather
// than only where there is a sound server, because it is pure bytes and is
// tested as such.
//
// A packet is a twenty-byte header — payload length, channel, a 64-bit
// offset and flags, all big-endian — and then the payload. A channel of all
// ones marks a command packet, whose payload is a sequence of tagged
// values beginning with the command and the serial that its reply will
// carry; any other channel marks samples for the stream of that number.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::platform::pulse {

inline constexpr std::size_t header_size = 20;
inline constexpr std::uint32_t command_channel = 0xFFFFFFFFu;
// A field that names nothing: the default sink, an unset buffer size.
inline constexpr std::uint32_t invalid_index = 0xFFFFFFFFu;
// The protocol this client speaks. Servers answer with their own and keep
// to the lower of the two; 35 is what the sound servers of the day speak.
inline constexpr std::uint32_t protocol_version = 35;
// Volume as the protocol counts it: 0 silent, 0x10000 the sample's own.
inline constexpr std::uint32_t volume_normal = 0x10000;

// The sample formats we can be asked for. The decoders here make floats.
enum class SampleFormat : std::uint8_t {
    U8 = 0,
    S16Le = 3,
    Float32Le = 5,
};

// The commands used here, by the numbers the protocol gives them.
enum class Command : std::uint32_t {
    Error = 0,
    Reply = 2,
    CreatePlaybackStream = 3,
    DeletePlaybackStream = 4,
    Auth = 8,
    SetClientName = 9,
    DrainPlaybackStream = 12,
    GetPlaybackLatency = 14,
    CorkPlaybackStream = 41,
    FlushPlaybackStream = 42,
    SetSinkInputVolume = 37,
    Request = 61,
    Overflow = 62,
    Underflow = 63,
    PlaybackStreamKilled = 64,
    PlaybackStreamMoved = 78,
    Started = 86,
};

// Builds one command packet, value by value.
class Writer {
public:
    Writer(Command command, std::uint32_t serial);

    Writer& u8(std::uint8_t);
    Writer& u32(std::uint32_t);
    Writer& u64(std::uint64_t);
    Writer& boolean(bool);
    Writer& string(std::string_view);
    Writer& null_string();
    Writer& arbitrary(std::span<std::uint8_t const>);
    Writer& sample_spec(SampleFormat, std::uint8_t channels, std::uint32_t rate);
    // The positions of the channels, in the protocol's numbering: 1 is the
    // left of a pair, 2 the right, 0 mono.
    Writer& channel_map(std::span<std::uint8_t const>);
    Writer& cvolume(std::span<std::uint32_t const>);
    // A moment by the machine's clock, which the server hands back untouched.
    Writer& timeval(std::uint32_t seconds, std::uint32_t microseconds);
    Writer& proplist(std::span<std::pair<std::string, std::string> const>);

    // The header and the payload, ready for the socket.
    std::vector<std::uint8_t> packet() const;
    std::span<std::uint8_t const> payload() const { return m_payload; }

private:
    std::vector<std::uint8_t> m_payload;
};

// Reads the tagged values of a payload. A read of the wrong type, or past
// the end, answers nothing and leaves the reader failed, so a short or
// bent packet cannot walk off the buffer.
class Reader {
public:
    explicit Reader(std::span<std::uint8_t const> payload)
        : m_payload(payload)
    {
    }

    std::optional<std::uint8_t> u8();
    std::optional<std::uint32_t> u32();
    std::optional<std::uint64_t> u64(); // a usec or a 64-bit index
    std::optional<bool> boolean();
    std::optional<std::string> string(); // nothing for a null string
    // Passes over one value of any type, whatever it is.
    bool skip();
    bool ok() const { return m_ok; }
    std::size_t remaining() const { return m_ok ? m_payload.size() - m_offset : 0; }

private:
    std::optional<char> peek() const;
    bool take(char tag);
    std::span<std::uint8_t const> m_payload;
    std::size_t m_offset = 0;
    bool m_ok = true;
};

// A packet's header, read from its first twenty bytes.
struct Header {
    std::uint32_t length = 0; // of the payload that follows
    std::uint32_t channel = command_channel;
    std::uint64_t offset = 0;
    std::uint32_t flags = 0;
};

std::optional<Header> read_header(std::span<std::uint8_t const>);
// The header and the samples of one block for a stream.
std::vector<std::uint8_t> block_packet(std::uint32_t channel, std::span<std::uint8_t const> samples);

}
