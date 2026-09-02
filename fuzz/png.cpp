// libFuzzer harness for the PNG decoder: hostile bytes through decode_png
// with a small pixel budget — no crash, no sanitizer finding.

#include "core/Bitmap.h"
#include "core/Png.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::vector<std::uint8_t> const input(data, data + size);
    (void)sashfold::decode_png(input, 1u << 20);
    return 0;
}
