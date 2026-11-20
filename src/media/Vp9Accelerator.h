#pragma once

// VP9 decoded by the machine's own video hardware, as every browser does
// where there is some: the GPU's video engine reads the arithmetic-coded
// part of each frame, and all it must be told is what the frame's
// uncompressed header says (media/Vp9.h). On Linux there are two ways to
// it, both loaded at run time so that a machine without them simply has no
// accelerator and plays through the software decoder instead: Vulkan Video
// first — the only way to NVIDIA's decoder with the driver it ships, and
// Mesa's for AMD and Intel — then VA-API, the interface Intel's and AMD's
// drivers have long had.
//
// A decoded frame lives on the GPU; read_last() and read_slot() bring it
// back as NV12 for the painter, until the compositor (plan 7.5.4) takes the
// surface as a texture where it is.

#include "media/Vp9.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sashfold::media {

// A picture at 8 bits a sample, 4:2:0, as NV12: the luma plane, then a
// plane of interleaved blue- and red-difference samples at half the size
// each way.
struct Nv12Picture {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> luma; // width * height
    std::vector<std::uint8_t> chroma; // chroma_width() * 2 * chroma_height()
    int chroma_width() const { return (width + 1) / 2; }
    int chroma_height() const { return (height + 1) / 2; }
    // A picture that is not 8 bits 4:2:0 (H.264's High 10, High 4:2:2 and
    // High 4:4:4 profiles, which the processor's decoder reads) comes as
    // its planes at its own depth and chroma size instead, `luma` and
    // `chroma` left empty.
    struct Planar {
        int bit_depth_luma = 8;
        int bit_depth_chroma = 8;
        int chroma_format = 1; // chroma_format_idc: 0 4:0:0 (no chroma planes), 1 4:2:0, 2 4:2:2, 3 4:4:4
        int chroma_width = 0;
        int chroma_height = 0;
        std::vector<std::uint16_t> y; // width * height
        std::vector<std::uint16_t> cb; // chroma_width * chroma_height
        std::vector<std::uint16_t> cr;
    };
    std::optional<Planar> planar;
};

// The ways to the hardware, for a caller that wants one in particular.
enum class VideoApi {
    Any, // the best there is: Vulkan Video, then VA-API
    Vulkan,
    Vaapi,
    Software, // decoded on the processor (H.264; VP9 has no software path yet)
};

class Vp9Accelerator {
public:
    // The hardware decoder for VP9 profile 0, or null with why in `error`.
    static std::unique_ptr<Vp9Accelerator> open(std::string& error, VideoApi = VideoApi::Any);
    virtual ~Vp9Accelerator() = default;

    // What decodes: the device and its driver, for the media trace.
    virtual std::string const& device() const = 0;
    // Decodes one frame of a stream (a superframe's part; never a
    // show_existing_frame, which decodes nothing) whose header the reader
    // gave, filing it in the reference slots the header refreshes. False
    // when the hardware refuses it.
    virtual bool decode(std::span<std::uint8_t const> frame, Vp9FrameHeader const& header) = 0;
    // The frame decoded last, and a reference slot's frame, read back.
    virtual std::optional<Nv12Picture> read_last() = 0;
    virtual std::optional<Nv12Picture> read_slot(int slot) = 0;
    // Forgets the references, as at a new stream.
    virtual void reset() = 0;
};

}
