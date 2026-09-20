#pragma once

// WebM streams written byte by byte, for the tests that need one whose every
// frame and time they chose: the parser's, the buffer's and the bindings'.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace sashfold::test::webm {

using Bytes = std::vector<std::uint8_t>;

inline void append(Bytes& out, Bytes const& more)
{
    out.insert(out.end(), more.begin(), more.end());
}

inline Bytes id_bytes(std::uint32_t id)
{
    Bytes out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        if (!out.empty() || ((id >> shift) & 0xFF) != 0)
            out.push_back(static_cast<std::uint8_t>(id >> shift));
    }
    return out;
}

// A size in the fewest bytes that hold it (never the all-ones pattern).
inline Bytes size_bytes(std::uint64_t size)
{
    for (int length = 1; length <= 8; ++length) {
        std::uint64_t const limit = (std::uint64_t { 1 } << (7 * length)) - 1;
        if (size < limit) {
            Bytes out(static_cast<std::size_t>(length));
            std::uint64_t value = size | (std::uint64_t { 1 } << (7 * length));
            for (int i = length - 1; i >= 0; --i) {
                out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value);
                value >>= 8;
            }
            return out;
        }
    }
    return {};
}

inline Bytes element(std::uint32_t id, Bytes const& body)
{
    Bytes out = id_bytes(id);
    append(out, size_bytes(body.size()));
    append(out, body);
    return out;
}

inline Bytes unsized(std::uint32_t id)
{
    Bytes out = id_bytes(id);
    out.push_back(0xFF);
    return out;
}

inline Bytes uint_body(std::uint64_t value)
{
    Bytes out;
    for (int shift = 56; shift >= 0; shift -= 8) {
        if (!out.empty() || ((value >> shift) & 0xFF) != 0 || shift == 0)
            out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    return out;
}

inline Bytes text(std::string const& value)
{
    return Bytes(value.begin(), value.end());
}

inline Bytes float_body(double value)
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    Bytes out;
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(bits >> shift));
    return out;
}

inline Bytes concat(std::initializer_list<Bytes> parts)
{
    Bytes out;
    for (Bytes const& part : parts)
        append(out, part);
    return out;
}

// The header of a block: track 1..126, the relative time, the flags.
inline Bytes block(int track, int relative, std::uint8_t flags, Bytes const& payload)
{
    Bytes out;
    out.reserve(4 + payload.size());
    out.push_back(static_cast<std::uint8_t>(0x80 | track));
    out.push_back(static_cast<std::uint8_t>(relative >> 8));
    out.push_back(static_cast<std::uint8_t>(relative));
    out.push_back(flags);
    append(out, payload);
    return out;
}

// Info (1 ms ticks, 2500 ticks long) and two tracks: VP9 320x180 as track 1,
// Opus 48 kHz stereo as track 2 with a 20 ms default duration.
inline Bytes init_segment(bool with_video = true, bool with_audio = true, std::uint64_t video_default_ns = 0)
{
    Bytes const header = element(0x1A45DFA3, element(0x4282, text("webm")));
    Bytes const info = element(0x1549A966, concat({ element(0x2AD7B1, uint_body(1000000)), element(0x4489, float_body(2500.0)) }));
    Bytes const video = element(0xAE,
        concat({ element(0xD7, uint_body(1)), element(0x83, uint_body(1)), element(0x86, text("V_VP9")),
            video_default_ns != 0 ? element(0x23E383, uint_body(video_default_ns)) : Bytes {},
            element(0xE0, concat({ element(0xB0, uint_body(320)), element(0xBA, uint_body(180)) })) }));
    Bytes const audio = element(0xAE,
        concat({ element(0xD7, uint_body(2)), element(0x83, uint_body(2)), element(0x86, text("A_OPUS")),
            element(0x63A2, Bytes { 'O', 'p', 'u', 's' }), element(0x23E383, uint_body(20000000)), element(0x56AA, uint_body(6500000)),
            element(0x56BB, uint_body(80000000)),
            element(0xE1, concat({ element(0xB5, float_body(48000.0)), element(0x9F, uint_body(2)) })) }));
    Bytes const tracks = element(0x1654AE6B, concat({ with_video ? video : Bytes {}, with_audio ? audio : Bytes {} }));
    return concat({ header, unsized(0x18538067), info, tracks });
}

// A sized Cluster at `time` of video frames (track 1) at the offsets given;
// a negative offset is a dependent frame at its absolute value.
inline Bytes video_cluster(std::uint64_t time, std::initializer_list<int> offsets, std::size_t payload = 4)
{
    Bytes body = element(0xE7, uint_body(time));
    for (int const offset : offsets)
        append(body, element(0xA3, block(1, std::abs(offset), offset >= 0 ? 0x80 : 0x00, Bytes(payload, 'v'))));
    return element(0x1F43B675, body);
}

}
