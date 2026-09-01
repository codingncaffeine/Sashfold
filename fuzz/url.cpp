// libFuzzer harness for the URL parser: hostile bytes as input, and again
// against a fixed base. No crash, no sanitizer finding.

#include "net/Url.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::string_view const input(reinterpret_cast<char const*>(data), size);
    (void)sashfold::net::parse_url(input);
    static auto const base = sashfold::net::parse_url("http://example.com/a/b?c#d");
    if (base) {
        auto relative = sashfold::net::parse_url(input, &*base);
        if (relative)
            (void)relative->serialize();
    }
    return 0;
}
