// libFuzzer harness for the SVG decoder: hostile bytes through the HTML
// parser in the SVG's namespace, the cascade, the path and transform
// microsyntaxes and the rasterizer, with a small pixel budget; no crash,
// no sanitizer finding, and a bounded amount of work per input.

#include "core/Bitmap.h"
#include "svg/Svg.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::vector<std::uint8_t> input(data, data + size);
    // Most random inputs would fail the sniff; lead with what passes it so
    // the whole pipeline is exercised.
    static constexpr char prefix[] = "<svg xmlns=\"http://www.w3.org/2000/svg\">";
    input.insert(input.begin(), prefix, prefix + sizeof prefix - 1);
    (void)sashfold::svg::decode_svg(input, 1u << 18);
    return 0;
}
