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

// The stack of the calling thread in bytes — the whole of it, as the OS
// reserved it — or 0 where it cannot be read.
std::size_t current_thread_stack_bytes();

// How much stack the process's first thread may grow to. On Linux that is
// a soft limit the kernel reads as the stack grows, so a process may raise
// it for itself before its first deep call; the stack it was started with
// (8 MB by the usual limit) holds fewer than three thousand script calls
// of the engine's, where V8 on a megabyte of its own frames reaches twelve
// thousand. Windows and macOS set the first thread's reserve at link time
// (CMakeLists.txt), and this only reports it. Returns the stack's size
// afterwards, as current_thread_stack_bytes would from that thread.
std::size_t widen_main_thread_stack(std::size_t bytes);

// The stack every thread that runs script gets: enough for a recursion as
// deep as V8 allows a page, and then some, so the engine's own guard ends
// it first. A reservation only — a thread touches the pages it uses.
inline constexpr std::size_t script_stack_bytes = 256u * 1024u * 1024u;

// The engine's budget for a script's recursion on a stack of that size:
// what the C++ stack may hold of script frames before RangeError, leaving
// four megabytes for the natives that run under the deepest frame — a
// throw and its unwinding, the collector tracing a deep structure — and
// never less than two. An unknown stack gets what an 8 MB one would.
inline std::size_t js_stack_budget_for(std::size_t stack_bytes)
{
    std::size_t const megabyte = 1024u * 1024u;
    if (stack_bytes == 0)
        stack_bytes = 8 * megabyte;
    return std::max(stack_bytes - std::min(stack_bytes, 4 * megabyte), 2 * megabyte);
}

}
