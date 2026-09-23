#include "media/Opus.h"

#include <algorithm>

namespace sashfold::media {

namespace {

// §3.2.1: a frame length of 0 to 251 in one byte, larger in two.
int parse_size(std::span<std::uint8_t const> bytes, int& size)
{
    if (bytes.empty()) {
        size = -1;
        return -1;
    }
    if (bytes[0] < 252) {
        size = bytes[0];
        return 1;
    }
    if (bytes.size() < 2) {
        size = -1;
        return -1;
    }
    size = 4 * bytes[1] + bytes[0];
    return 2;
}

}

std::optional<OpusPacket> parse_opus_packet(std::span<std::uint8_t const> bytes)
{
    if (bytes.empty())
        return std::nullopt;
    OpusPacket packet;
    std::uint8_t const toc = bytes[0];
    packet.config = toc >> 3;
    packet.stereo = (toc & 0x4) != 0;
    // §3.1, Table 2: configurations 0-11 SILK, 12-15 hybrid, 16-31 CELT.
    if (packet.config < 12) {
        packet.mode = OpusPacket::Mode::Silk;
        packet.bandwidth = static_cast<OpusPacket::Bandwidth>(packet.config >> 2);
        int const lengths[4] = { 480, 960, 1920, 2880 };
        packet.frame_samples = lengths[packet.config & 3];
    } else if (packet.config < 16) {
        packet.mode = OpusPacket::Mode::Hybrid;
        packet.bandwidth = packet.config < 14 ? OpusPacket::Bandwidth::SuperWide : OpusPacket::Bandwidth::Full;
        packet.frame_samples = (packet.config & 1) ? 960 : 480;
    } else {
        packet.mode = OpusPacket::Mode::Celt;
        int const band = (packet.config - 16) >> 2;
        packet.bandwidth = band == 0 ? OpusPacket::Bandwidth::Narrow : static_cast<OpusPacket::Bandwidth>(band + 1);
        packet.frame_samples = 120 << (packet.config & 3);
    }

    std::span<std::uint8_t const> rest = bytes.subspan(1);
    int len = static_cast<int>(rest.size());
    std::vector<int> sizes;
    int count = 0;
    int last_size = len;
    switch (toc & 0x3) {
    case 0:
        count = 1;
        break;
    case 1:
        // Two frames of equal size.
        count = 2;
        if (len & 1)
            return std::nullopt;
        sizes.push_back(len / 2);
        last_size = len / 2;
        break;
    case 2: {
        // Two frames, the first's size given.
        count = 2;
        int size = 0;
        int const read = parse_size(rest, size);
        if (read < 0)
            return std::nullopt;
        len -= read;
        if (size < 0 || size > len)
            return std::nullopt;
        rest = rest.subspan(static_cast<std::size_t>(read));
        sizes.push_back(size);
        last_size = len - size;
        break;
    }
    default: {
        // Up to 48 frames, with optional padding, equal or each sized.
        if (len < 1)
            return std::nullopt;
        std::uint8_t const ch = rest[0];
        rest = rest.subspan(1);
        len--;
        count = ch & 0x3F;
        if (count <= 0 || packet.frame_samples * count > 5760)
            return std::nullopt;
        if (ch & 0x40) {
            // RFC 8251 §4: the padding comes off the length as it is read,
            // so a huge claimed padding cannot overflow a counter.
            int p;
            do {
                if (len <= 0)
                    return std::nullopt;
                p = rest[0];
                rest = rest.subspan(1);
                len--;
                len -= p == 255 ? 254 : p;
            } while (p == 255);
        }
        if (len < 0)
            return std::nullopt;
        bool const vbr = (ch & 0x80) != 0;
        if (vbr) {
            last_size = len;
            for (int i = 0; i < count - 1; ++i) {
                int size = 0;
                int const read = parse_size(rest, size);
                if (read < 0)
                    return std::nullopt;
                len -= read;
                if (size < 0 || size > len)
                    return std::nullopt;
                rest = rest.subspan(static_cast<std::size_t>(read));
                sizes.push_back(size);
                last_size -= read + size;
            }
            if (last_size < 0)
                return std::nullopt;
        } else {
            last_size = len / count;
            if (last_size * count != len)
                return std::nullopt;
            for (int i = 0; i < count - 1; ++i)
                sizes.push_back(last_size);
        }
        break;
    }
    }
    // The last frame's size is what is left, and no frame is over 1275.
    if (last_size > 1275)
        return std::nullopt;
    sizes.push_back(last_size);
    std::size_t at = 0;
    for (int const size : sizes) {
        if (at + static_cast<std::size_t>(size) > rest.size())
            return std::nullopt;
        packet.frames.push_back(rest.subspan(at, static_cast<std::size_t>(size)));
        at += static_cast<std::size_t>(size);
    }
    (void)count;
    return packet;
}

OpusDecoder::OpusDecoder(int channels)
    : m_channels(channels)
    , m_celt(channels)
{
}

void OpusDecoder::reset()
{
    m_celt.reset();
    m_previous_mode.reset();
    m_final_range = 0;
}

OpusDecoder::Result OpusDecoder::decode(std::span<std::uint8_t const> bytes, std::vector<float>& out)
{
    std::optional<OpusPacket> const packet = parse_opus_packet(bytes);
    if (!packet)
        return {};
    int const coded_channels = packet->stereo ? 2 : 1;
    Result result { Outcome::Decoded, 0 };
    for (std::span<std::uint8_t const> const frame : packet->frames) {
        std::size_t const at = out.size();
        out.resize(at + static_cast<std::size_t>(packet->frame_samples * m_channels), 0.0f);
        if (packet->mode != OpusPacket::Mode::Celt) {
            // Not decoded yet: silence of its length keeps the time.
            result.outcome = Outcome::Unsupported;
            m_final_range = 0;
            m_previous_mode = packet->mode;
            result.samples += packet->frame_samples;
            continue;
        }
        // A switch into CELT from another layer starts CELT afresh.
        if (m_previous_mode && *m_previous_mode != OpusPacket::Mode::Celt)
            m_celt.reset();
        int end_band = 21;
        switch (packet->bandwidth) {
        case OpusPacket::Bandwidth::Narrow:
            end_band = 13;
            break;
        case OpusPacket::Bandwidth::Medium:
        case OpusPacket::Bandwidth::Wide:
            end_band = 17;
            break;
        case OpusPacket::Bandwidth::SuperWide:
            end_band = 19;
            break;
        case OpusPacket::Bandwidth::Full:
            end_band = 21;
            break;
        }
        if (frame.size() <= 1) {
            m_celt.conceal(out.data() + at, packet->frame_samples);
            m_final_range = 0;
        } else {
            RangeDecoder range(frame);
            if (!m_celt.decode(range, static_cast<int>(frame.size()), out.data() + at, packet->frame_samples, coded_channels, 0, end_band))
                return {};
            m_final_range = range.range();
        }
        m_previous_mode = OpusPacket::Mode::Celt;
        m_previous_frame_samples = packet->frame_samples;
        result.samples += packet->frame_samples;
    }
    return result;
}

void OpusDecoder::conceal(int samples, std::vector<float>& out)
{
    while (samples > 0) {
        int const n = std::min(samples, 960);
        int const frame = n >= 960 ? 960 : n >= 480 ? 480 : n >= 240 ? 240 : 120;
        std::size_t const at = out.size();
        out.resize(at + static_cast<std::size_t>(frame * m_channels), 0.0f);
        m_celt.conceal(out.data() + at, frame);
        samples -= frame;
    }
}

}
