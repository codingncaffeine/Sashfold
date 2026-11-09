// libFuzzer harness for the software H.264 decoder: hostile bytes read as
// an Annex B stream (parameter sets, slices, anything) and decoded whole,
// then in access units whose lengths the input itself chooses, through the
// host's buffer bookkeeping (media/H264Decoder.h); no crash, no sanitizer
// finding, and no picture larger than the decoder was allowed.

#include "media/H264Decoder.h"
#include "media/H264Software.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>

namespace {

// 4096 macroblocks (1024 x 1024 samples, or any shape of that area):
// room for every tool, and seventeen slots of it stay well inside the
// fuzzer's memory.
constexpr int max_macroblocks = 64 * 64;

// A picture within the allowed area, its planes the sizes it says.
void check(std::optional<sashfold::media::Nv12Picture> const& picture)
{
    if (!picture)
        return;
    std::size_t const w = static_cast<std::size_t>(picture->width), h = static_cast<std::size_t>(picture->height);
    if (w * h > static_cast<std::size_t>(max_macroblocks) * 256 || picture->luma.size() != w * h
        || picture->chroma.size() != static_cast<std::size_t>(picture->chroma_width()) * 2 * static_cast<std::size_t>(picture->chroma_height()))
        std::abort();
}

}

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    using namespace sashfold::media;
    {
        H264Decoder whole(open_software_h264(max_macroblocks));
        check(whole.decode(std::span<std::uint8_t const>(data, size)));
    }
    H264Decoder pieces(open_software_h264(max_macroblocks));
    std::size_t at = 0;
    while (at < size) {
        std::size_t const length = std::min<std::size_t>(size - at, 1 + data[at] * 7 % 997);
        check(pieces.decode(std::span<std::uint8_t const>(data + at, length)));
        at += length;
    }
    pieces.reset();
    check(pieces.decode(std::span<std::uint8_t const>(data, size / 2)));
    return 0;
}
