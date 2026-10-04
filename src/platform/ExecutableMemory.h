#pragma once

// Memory for machine code the script engine writes (js/jit): mapped
// readable and writable, never executable, while the code is written;
// then sealed readable and executable, never writable, and never written
// again; unmapped when let go. It is never writable and executable at
// once. The OS places it, so where it lies is as random as the OS makes
// any mapping.

#include <cstddef>
#include <optional>

namespace sashfold::platform {

class ExecutableMemory {
public:
    // At least `bytes`, rounded up to whole pages; nothing when the OS
    // refuses.
    static std::optional<ExecutableMemory> allocate(std::size_t bytes);

    ExecutableMemory(ExecutableMemory&& other) noexcept;
    ExecutableMemory& operator=(ExecutableMemory&& other) noexcept;
    ExecutableMemory(ExecutableMemory const&) = delete;
    ExecutableMemory& operator=(ExecutableMemory const&) = delete;
    ~ExecutableMemory();

    // Writable until sealed.
    std::byte* data() { return m_sealed ? nullptr : m_base; }
    std::byte const* code() const { return m_base; }
    std::size_t size() const { return m_size; }
    // Readable and executable from here on, and the instruction cache told
    // (on machines whose caches need it); false when the OS refuses, and
    // the memory is then neither.
    bool seal();
    bool sealed() const { return m_sealed; }
    // On Windows, tells the system how to unwind the code's frames (for
    // exceptions, debuggers and stack walks): `entry` is the offset in this
    // memory of the code's function table, one RUNTIME_FUNCTION whose
    // unwind data lies in the memory too; once sealed. Taken back when the
    // memory is let go. Elsewhere nothing is needed, and it answers true.
    bool describe_frames(std::size_t entry);

private:
    ExecutableMemory(std::byte* base, std::size_t size)
        : m_base(base)
        , m_size(size)
    {
    }
    void release();

    std::byte* m_base = nullptr;
    std::size_t m_size = 0;
    bool m_sealed = false;
    void* m_frames = nullptr; // the function table given to the system (Windows)
};

}
