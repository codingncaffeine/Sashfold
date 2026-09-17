#pragma once

// The process's memory as the OS accounts it, for the instruments: the
// resident set in bytes — what --bench reports as the RAM a page costs —
// or 0 where it cannot be read.

#include <cstddef>

namespace sashfold::platform {

std::size_t resident_set_bytes();

}
