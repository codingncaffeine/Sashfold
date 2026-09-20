#pragma once

// WAVE, the sound a page can carry with no decoder between the file and
// the speakers: a RIFF file of chunks, of which the format and the data
// are the two that matter. Every depth the format allows — unsigned bytes,
// 16, 24 and 32 bit whole numbers, and 32 and 64 bit floats — becomes the
// interleaved floats the rest of the engine plays, so nothing downstream
// needs to know which it was.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::media {

struct Sound {
    unsigned rate = 48000;
    unsigned channels = 2;
    std::vector<float> samples; // interleaved, one per channel per frame
    std::size_t frames() const { return channels == 0 ? 0 : samples.size() / channels; }
    double seconds() const { return rate == 0 ? 0 : static_cast<double>(frames()) / static_cast<double>(rate); }
};

bool looks_like_wav(std::span<std::uint8_t const>);
// The sound in a WAVE file; nothing for anything malformed, a format no
// player reads, or more frames than `max_frames`.
std::optional<Sound> decode_wav(std::span<std::uint8_t const>, std::size_t max_frames = 48000u * 60u * 30u);

}
