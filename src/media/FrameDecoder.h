#pragma once

// What the video pipeline asks of a codec: a coded block in, the picture
// it shows out — VP9 through the superframe and the uncompressed header
// to the VP9 accelerator (media/Vp9Accelerator.h), H.264 through its
// access units to the H.264 decoder (media/H264Decoder.h). Each is made
// for a track's codec and configuration record, and lives on the
// pipeline's thread.

#include "media/H264Accelerator.h"
#include "media/Vp9Accelerator.h"
#include "media/Yuv.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sashfold::media {

class FrameDecoder {
public:
    struct Result {
        bool ok = true; // the block was read and decoded
        std::uint64_t decoded = 0; // frames the hardware decoded (a superframe holds several)
        std::uint64_t skipped = 0; // frames shown that were not made, not being wanted
        std::optional<Nv12Picture> shown; // the picture the block shows, when wanted
        YuvColour colour; // how to read its samples
    };
    virtual ~FrameDecoder() = default;

    // What decodes: the device and its driver, for the media trace.
    virtual std::string const& device() const = 0;
    // One coded block in decoding order. `wanted` false means the picture
    // would be replaced before being shown: decoded for what follows, not
    // read back.
    virtual Result decode(std::span<std::uint8_t const> block, bool wanted) = 0;
    // Forgets the stream: the next block begins at a key frame.
    virtual void reset() = 0;
};

// A decoder for a track by its codec id ("V_VP9", "V_H264") and
// configuration record (H.264's avcC); null with why where the machine
// has no way to decode it.
std::unique_ptr<FrameDecoder> open_frame_decoder(std::string const& codec_id, std::span<std::uint8_t const> codec_private, std::string& error);
// VP9 on an accelerator already opened (a test's own, for one).
std::unique_ptr<FrameDecoder> vp9_frame_decoder(std::unique_ptr<Vp9Accelerator> accelerator);

}
