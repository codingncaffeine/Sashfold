// libFuzzer harness for the WebM stream parser: hostile bytes fed whole and
// then in pieces whose lengths the input itself chooses; no crash, no
// sanitizer finding, and no frame larger than the input it came from.

#include "media/WebM.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    using sashfold::media::WebmParser;
    {
        WebmParser whole;
        (void)whole.append(std::span<std::uint8_t const>(data, size));
        for (auto const& frame : whole.take_frames()) {
            if (frame.data.size() > size)
                std::abort();
        }
    }
    WebmParser pieces;
    std::size_t at = 0;
    while (at < size) {
        std::size_t const length = std::min<std::size_t>(size - at, 1 + data[at] % 61);
        if (!pieces.append(std::span<std::uint8_t const>(data + at, length)))
            break;
        (void)pieces.take_frames();
        at += length;
    }
    return 0;
}
