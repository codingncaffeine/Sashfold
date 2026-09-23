#pragma once

// Opus (RFC 6716): the sound of every WebM a video site streams, and of
// WebRTC. A packet is a table-of-contents byte naming the configuration —
// which layer coded it (SILK for speech, CELT for music, or the two
// together), the bandwidth, the frame length, mono or stereo — and one or
// more frames packed by one of four codes (§3.2).
//
// The CELT layer is decoded here; SILK and hybrid frames are reported as
// unsupported and stand in as silence of their length, so a stream that
// switches to them keeps its time.

#include "media/Celt.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::media {

struct OpusPacket {
    enum class Mode { Silk,
        Hybrid,
        Celt };
    enum class Bandwidth { Narrow,
        Medium,
        Wide,
        SuperWide,
        Full };

    int config = 0;
    Mode mode = Mode::Celt;
    Bandwidth bandwidth = Bandwidth::Full;
    int frame_samples = 0; // a channel, at 48 kHz
    bool stereo = false;
    std::vector<std::span<std::uint8_t const>> frames;
};

// The packet's frames, or nothing for one the specification calls invalid.
std::optional<OpusPacket> parse_opus_packet(std::span<std::uint8_t const> bytes);

class OpusDecoder {
public:
    // A decoder for 48 kHz output of one or two channels, whatever the
    // packets carry.
    explicit OpusDecoder(int channels);

    enum class Outcome { Decoded,
        Unsupported, // silence of the right length took its place
        Invalid };
    struct Result {
        Outcome outcome = Outcome::Invalid;
        int samples = 0; // a channel
    };

    // Decodes a packet, appending interleaved floats in [-1, 1] to `out`.
    Result decode(std::span<std::uint8_t const> packet, std::vector<float>& out);
    // A packet that never arrived, of `samples` a channel.
    void conceal(int samples, std::vector<float>& out);
    void reset();

    int channels() const { return m_channels; }
    // The range decoder's state after the last packet, which an encoder's
    // own must equal (§4.1.6): the proof that every bit was read as written.
    std::uint32_t final_range() const { return m_final_range; }

private:
    int m_channels;
    CeltDecoder m_celt;
    std::optional<OpusPacket::Mode> m_previous_mode;
    int m_previous_frame_samples = 960;
    std::uint32_t m_final_range = 0;
};

}
