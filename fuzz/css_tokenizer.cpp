// libFuzzer harness for the CSS tokenizer: hostile bytes in, no crash, no
// sanitizer finding, guaranteed forward progress. Build with -DSASHFOLD_FUZZ=ON.

#include "css/Tokenizer.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    // The first byte steers the "unicode ranges allowed" gate.
    bool unicode_ranges_allowed = false;
    if (size > 0) {
        unicode_ranges_allowed = (data[0] & 1) != 0;
        ++data;
        --size;
    }
    std::string_view const input(reinterpret_cast<char const*>(data), size);
    sashfold::css::Tokenizer tokenizer(input);
    while (true) {
        sashfold::css::Token const token = tokenizer.next(unicode_ranges_allowed);
        if (token.type == sashfold::css::Token::Type::EndOfFile)
            break;
    }
    return 0;
}
