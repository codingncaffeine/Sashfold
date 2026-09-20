#include "media/Wav.h"

#include <algorithm>
#include <cstring>

namespace sashfold::media {

namespace {

// The formats the fmt chunk names. Extensible hands the real one to a
// sub-format field further in.
constexpr std::uint16_t format_pcm = 1;
constexpr std::uint16_t format_float = 3;
constexpr std::uint16_t format_extensible = 0xFFFE;

std::uint16_t u16(std::span<std::uint8_t const> bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(bytes[at] | (static_cast<std::uint16_t>(bytes[at + 1]) << 8));
}

std::uint32_t u32(std::span<std::uint8_t const> bytes, std::size_t at)
{
    return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[at + 2]) << 16) | (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

bool four_cc(std::span<std::uint8_t const> bytes, std::size_t at, char const* name)
{
    return at + 4 <= bytes.size() && std::memcmp(bytes.data() + at, name, 4) == 0;
}

// One sample, read at its own width and brought into -1 to 1. Whole
// numbers are divided by the largest their width holds, which is what
// every player does; the floats in a file are already in that range.
float sample_at(std::span<std::uint8_t const> data, std::size_t at, std::uint16_t bits, bool is_float)
{
    if (is_float) {
        if (bits == 64) {
            std::uint64_t whole = 0;
            for (int i = 0; i < 8; ++i)
                whole |= static_cast<std::uint64_t>(data[at + static_cast<std::size_t>(i)]) << (8 * i);
            double value = 0;
            std::memcpy(&value, &whole, sizeof value);
            return static_cast<float>(value);
        }
        std::uint32_t const whole = u32(data, at);
        float value = 0;
        std::memcpy(&value, &whole, sizeof value);
        return value;
    }
    switch (bits) {
    case 8:
        // Eight bits are unsigned, with silence at the middle of the range.
        return (static_cast<float>(data[at]) - 128.0f) / 128.0f;
    case 16: {
        std::int16_t const value = static_cast<std::int16_t>(u16(data, at));
        return static_cast<float>(value) / 32768.0f;
    }
    case 24: {
        std::int32_t value = static_cast<std::int32_t>(static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1]) << 8)
            | (static_cast<std::uint32_t>(data[at + 2]) << 16));
        if ((value & 0x800000) != 0)
            value -= 0x1000000;
        return static_cast<float>(value) / 8388608.0f;
    }
    case 32: {
        std::int32_t const value = static_cast<std::int32_t>(u32(data, at));
        return static_cast<float>(static_cast<double>(value) / 2147483648.0);
    }
    default:
        return 0;
    }
}

}

bool looks_like_wav(std::span<std::uint8_t const> bytes)
{
    return bytes.size() >= 12 && four_cc(bytes, 0, "RIFF") && four_cc(bytes, 8, "WAVE");
}

std::optional<Sound> decode_wav(std::span<std::uint8_t const> bytes, std::size_t max_frames)
{
    if (!looks_like_wav(bytes))
        return std::nullopt;
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint32_t rate = 0;
    std::uint16_t bits = 0;
    bool have_format = false;
    std::span<std::uint8_t const> data;
    bool have_data = false;

    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        std::uint32_t const length = u32(bytes, at + 4);
        std::size_t const body = at + 8;
        // A chunk that claims more than is left is read as far as it goes:
        // a file cut short still plays what arrived.
        std::size_t const have = std::min(static_cast<std::size_t>(length), bytes.size() - body);
        if (four_cc(bytes, at, "fmt ") && have >= 16) {
            format = u16(bytes, body);
            channels = u16(bytes, body + 2);
            rate = u32(bytes, body + 4);
            bits = u16(bytes, body + 14);
            if (format == format_extensible && have >= 40) {
                // The real format is the first two bytes of the sub-format.
                format = u16(bytes, body + 24);
                std::uint16_t const valid_bits = u16(bytes, body + 18);
                if (valid_bits != 0 && valid_bits <= bits)
                    bits = valid_bits;
            }
            have_format = true;
        } else if (four_cc(bytes, at, "data")) {
            data = bytes.subspan(body, have);
            have_data = true;
        }
        // Chunks sit at even offsets.
        std::size_t const step = 8 + static_cast<std::size_t>(length) + (length % 2);
        if (step <= 8)
            break;
        at += step;
    }
    if (!have_format || !have_data || channels == 0 || channels > 8 || rate < 1000 || rate > 384000)
        return std::nullopt;
    bool const is_float = format == format_float;
    if (!is_float && format != format_pcm)
        return std::nullopt;
    if (is_float ? (bits != 32 && bits != 64) : (bits != 8 && bits != 16 && bits != 24 && bits != 32))
        return std::nullopt;

    std::size_t const width = bits / 8u;
    std::size_t const frame_bytes = width * channels;
    std::size_t const frames = frame_bytes == 0 ? 0 : data.size() / frame_bytes;
    if (frames == 0 || frames > max_frames)
        return std::nullopt;
    Sound sound;
    sound.rate = rate;
    sound.channels = channels;
    sound.samples.resize(frames * channels);
    for (std::size_t i = 0; i < sound.samples.size(); ++i)
        sound.samples[i] = sample_at(data, i * width, bits, is_float);
    return sound;
}

}
