// libFuzzer harness for the HTTP response parser: hostile bytes as a full
// response stream. No crash, no hang, no sanitizer finding.

#include "net/Http.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::vector<std::uint8_t> const input(data, data + size);
    std::size_t at = 0;
    auto const read = [&](std::uint8_t* buffer, std::size_t want) -> std::ptrdiff_t {
        if (at >= input.size())
            return 0;
        std::size_t const take = std::min(want, input.size() - at);
        std::memcpy(buffer, input.data() + at, take);
        at += take;
        return static_cast<std::ptrdiff_t>(take);
    };
    (void)sashfold::net::read_response(read, 1u << 20);
    return 0;
}
