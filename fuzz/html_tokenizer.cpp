// libFuzzer harness for the HTML tokenizer: hostile bytes in, no crash, no
// sanitizer finding, bounded output. Build with -DSASHFOLD_FUZZ=ON (clang).

#include "html/Tokenizer.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::string_view const input(reinterpret_cast<char const*>(data), size);
    sashfold::html::Tokenizer tokenizer(input);
    while (auto token = tokenizer.next_token()) {
        // Tokens only need to be produced; the tree stage consumes them later.
    }
    return 0;
}
