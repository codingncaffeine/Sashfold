#pragma once
// The platform random seam: bytes from the operating system's own source
// (getentropy on POSIX, BCryptGenRandom on Windows), for TLS keys and
// nonces. There is no fallback of our own: a system that cannot supply
// random bytes cannot make a key, and the process stops rather than
// continue with a substitute.
#include <cstddef>
#include <cstdint>
#include <span>

namespace sashfold::platform {

void fill_random(std::span<std::uint8_t> out);

}
