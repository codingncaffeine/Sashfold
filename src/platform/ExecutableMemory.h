#pragma once

// Memory for machine code the script engine writes (js/jit), written in
// pieces one after another: a page is readable and writable, never
// executable, while code is written to it; then sealed readable and
// executable, never writable. A page is made writable again only to write
// more code after what it holds (the code space appends each block of code
// after the last), and is executable again only once sealed — it is never
// writable and executable at once. Unmapped when let go. The OS places it,
// so where it lies is as random as the OS makes any mapping.

#include <cstddef>
#include <optional>
#include <vector>

namespace sashfold::platform {

class ExecutableMemory {
public:
    // At least `bytes`, rounded up to whole pages, all of it writable and
    // none executable; nothing when the OS refuses.
    static std::optional<ExecutableMemory> allocate(std::size_t bytes);

    ExecutableMemory(ExecutableMemory&& other) noexcept;
    ExecutableMemory& operator=(ExecutableMemory&& other) noexcept;
    ExecutableMemory(ExecutableMemory const&) = delete;
    ExecutableMemory& operator=(ExecutableMemory const&) = delete;
    ~ExecutableMemory();

    std::byte* base() const { return m_base; }
    std::size_t size() const { return m_size; }
    // The pages that cover [offset, offset + bytes): writable again and not
    // executable (to write after what they hold), or sealed readable and
    // executable with the instruction cache told (on machines whose caches
    // need it). False when the OS refuses; a refused seal leaves the pages
    // neither writable nor executable.
    bool unseal(std::size_t offset, std::size_t bytes);
    bool seal(std::size_t offset, std::size_t bytes);
    // On Windows, tells the system how to unwind frames of a piece of code
    // in this memory (for exceptions, debuggers and stack walks): the code
    // starts at `code` and its function table, `count` RUNTIME_FUNCTIONs in
    // address order whose unwind data lies in the memory too and whose
    // addresses count from the code's start, `entry` bytes after it; once
    // sealed. Taken back when the memory is let go. Elsewhere nothing is
    // needed, and it answers true.
    bool describe_frames(std::size_t code, std::size_t entry, std::size_t count);

private:
    ExecutableMemory(std::byte* base, std::size_t size)
        : m_base(base)
        , m_size(size)
    {
    }
    void release();

    std::byte* m_base = nullptr;
    std::size_t m_size = 0;
    std::vector<void*> m_frames; // the function tables given to the system (Windows)
};

}
