#pragma once

// Forgiving base64 (WHATWG infra): ASCII whitespace is removed, up to two
// trailing '=' padding characters are accepted when the length works out,
// anything else outside the alphabet refuses the input. First consumer:
// data: URLs; later ones: inline images, Basic auth.

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace sashfold {

std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view input);

}
