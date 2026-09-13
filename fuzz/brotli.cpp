// libFuzzer harness for the brotli decoder: hostile bytes through a whole
// stream — prefix codes, context maps, block switches, distances reaching
// back past the data into the dictionary — with a small output cap; no
// crash, no sanitizer finding.

#include "core/Brotli.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    (void)sashfold::brotli_decompress(data, size, 1u << 20);
    return 0;
}
