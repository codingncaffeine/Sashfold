#pragma once

// What the element's sound path asks of a codec: a packet in, stereo
// samples at the speakers' rate out — Opus (media/Opus.h), which plays at
// 48 kHz by its nature, and AAC (media/Aac.h), whose channels are mixed to
// two and whose rate is brought to the device's by the resampler
// (media/Resampler.h). Made for a track's codec and configuration record.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sashfold::media {

class AudioDecoder {
public:
    static constexpr int output_rate = 48000;
    static constexpr int output_channels = 2;

    struct Result {
        bool ok = false; // the packet decoded; false: concealed or skipped
        std::size_t input_frames = 0; // of the stream's own rate, what the packet held
    };
    virtual ~AudioDecoder() = default;

    // The stream's own sampling rate, for timing by its packets.
    virtual int native_rate() const = 0;
    // Decodes one packet, appending stereo 48 kHz samples to `out`; a packet
    // that cannot be read appends what conceals it (silence or the codec's
    // own guess) and says so.
    virtual Result decode(std::span<std::uint8_t const> packet, std::vector<float>& out) = 0;
    // Forgets the state, as at a seek.
    virtual void reset() = 0;
};

// A decoder for a track by its codec id ("A_OPUS", "A_AAC") and
// configuration record; null with why when there is none here.
std::unique_ptr<AudioDecoder> open_audio_decoder(std::string const& codec_id, std::span<std::uint8_t const> codec_private, std::string& error);

}
