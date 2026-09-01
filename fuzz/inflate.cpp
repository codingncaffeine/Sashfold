// libFuzzer harness for the inflate paths: hostile bytes through raw deflate,
// zlib, and gzip decoding with a small output cap. No crash, no hang, no
// sanitizer finding.

#include "core/Inflate.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::vector<std::uint8_t> const input(data, data + size);
    (void)sashfold::inflate(input, 1u << 20);
    (void)sashfold::zlib_decompress(input, 1u << 20);
    (void)sashfold::gzip_decompress(input, 1u << 20);
    return 0;
}
