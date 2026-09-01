// libFuzzer harness for the CSS parser: hostile bytes into parse_stylesheet
// and the style-attribute entry, no crash, no sanitizer finding.

#include "css/Parser.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::string_view const input(reinterpret_cast<char const*>(data), size);
    (void)sashfold::css::parse_stylesheet(input);
    (void)sashfold::css::parse_declaration_list(input);
    return 0;
}
