#pragma once

// The process's memory as the OS accounts it, for the instruments: the
// resident set in bytes — what --bench reports as the RAM a page costs —
// or 0 where it cannot be read. And the machine's own memory, from which
// the ceilings the window runs under are taken.

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace sashfold::platform {

std::size_t resident_set_bytes();

// The machine's memory in bytes, or 0 where it cannot be read.
std::uint64_t physical_memory_bytes();

// The ceiling on one page's script heap: a quarter of the machine's memory,
// no less than 512 MB and no more than 4 GB — the rule the engine under
// Chrome gives a page its heap by. With the machine's memory unknown, 4 GB
// where a pointer is eight bytes and 512 MB where it is four.
inline std::size_t js_heap_limit_for(std::uint64_t physical_bytes)
{
    std::uint64_t const megabyte = 1024u * 1024u;
    std::uint64_t const most = sizeof(void*) >= 8 ? 4096 * megabyte : 512 * megabyte;
    if (physical_bytes == 0)
        return static_cast<std::size_t>(most);
    return static_cast<std::size_t>(std::clamp(physical_bytes / 4, 512 * megabyte, most));
}

// The ceiling on the whole process, which the memory watch holds it to:
// half the machine's memory, no less than 2 GB and no more than 16 GB.
// One process holds every tab, so this is the one thing standing between
// a runaway — a page's or the engine's own — and the machine's memory.
inline std::size_t memory_ceiling_for(std::uint64_t physical_bytes)
{
    std::uint64_t const gigabyte = 1024u * 1024u * 1024u;
    if (sizeof(void*) < 8)
        return static_cast<std::size_t>(2 * gigabyte - 1);
    if (physical_bytes == 0)
        return static_cast<std::size_t>(8 * gigabyte);
    return static_cast<std::size_t>(std::clamp(physical_bytes / 2, 2 * gigabyte, 16 * gigabyte));
}

}
