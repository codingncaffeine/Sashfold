// libFuzzer harness for the full HTML parsing pipeline: tokenizer plus tree
// construction. Hostile bytes in, a document out, no crash, no finding.

#include "core/Unicode.h"
#include "html/TreeBuilder.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::string_view const input(reinterpret_cast<char const*>(data), size);
    auto document = sashfold::html::parse_document(input);
    (void)document;
    return 0;
}
