// libFuzzer harness for the BMP and ICO decoders: hostile bytes through
// both — each reads only what its signature claims — with a small pixel
// budget; no crash, no sanitizer finding.

#include "core/Bitmap.h"
#include "core/Bmp.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::vector<std::uint8_t> const input(data, data + size);
    (void)sashfold::decode_bmp(input, 1u << 20);
    (void)sashfold::decode_ico(input, 16, 1u << 20);
    return 0;
}
