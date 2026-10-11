#pragma once
// The random source a key generation or a randomized encoding draws from.
// The crypto library takes no dependency on the platform: its caller hands
// in the operating system's source (platform::fill_random), a test hands in
// a fixed sequence.
#include <cstdint>
#include <span>

namespace sashfold::crypto {

using RandomFill = void (*)(std::span<std::uint8_t> out);

}
