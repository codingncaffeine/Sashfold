// libFuzzer harness for the MP4 stream parser: hostile bytes fed whole and
// then in pieces whose lengths the input itself chooses, after a moov the
// fuzzer may or may not have made; no crash, no sanitizer finding, and no
// frame larger than the input it came from.

#include "media/Mp4.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    using sashfold::media::Mp4Parser;
    {
        Mp4Parser whole;
        (void)whole.append(std::span<std::uint8_t const>(data, size));
        for (auto const& frame : whole.take_frames()) {
            if (frame.data.size() > size)
                std::abort();
        }
    }
    Mp4Parser pieces;
    std::size_t at = 0;
    while (at < size) {
        std::size_t const length = std::min<std::size_t>(size - at, 1 + data[at] % 61);
        if (!pieces.append(std::span<std::uint8_t const>(data + at, length)))
            break;
        (void)pieces.take_frames();
        at += length;
    }
    pieces.reset();
    (void)pieces.append(std::span<std::uint8_t const>(data, size / 2));
    return 0;
}
