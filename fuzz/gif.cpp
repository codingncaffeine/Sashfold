// libFuzzer harness for the GIF decoder: hostile bytes through decode_gif
// with a small pixel budget — no crash, no sanitizer finding.

#include "core/Bitmap.h"
#include "core/Gif.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::vector<std::uint8_t> const input(data, data + size);
    (void)sashfold::decode_gif(input, 1u << 20);
    return 0;
}
