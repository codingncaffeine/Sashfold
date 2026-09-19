// libFuzzer harness for the zip reader: hostile bytes as an archive — its
// directory found from the end, every entry it lists read under a small
// cap — and as a browser theme's manifest; no crash, no sanitizer finding.

#include "core/Zip.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::vector<std::uint8_t> input(data, data + size);
    std::optional<sashfold::ZipArchive> const archive = sashfold::ZipArchive::open(std::move(input));
    if (!archive)
        return 0;
    for (sashfold::ZipEntry const& entry : archive->entries())
        (void)archive->read(entry, 1u << 20);
    (void)archive->find("manifest.json");
    return 0;
}
