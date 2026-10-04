#include "platform/ExecutableMemory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <utility>

// Apple silicon allows no page to change from writable to executable: code
// memory is one MAP_JIT region, readable, writable and executable as
// mapped, and each thread sees it either writable or executable as it
// toggles (pthread_jit_write_protect_np) — never both at once for any
// thread. Unsealing and sealing are that toggle there, for the calling
// thread, which is the one thread that writes and runs an interpreter's code.
#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#define SASHFOLD_MAP_JIT 1
#else
#define SASHFOLD_MAP_JIT 0
#endif

namespace sashfold::platform {

namespace {

std::size_t page_size()
{
    long const size = sysconf(_SC_PAGESIZE);
    return size > 0 ? static_cast<std::size_t>(size) : 4096;
}

}

std::optional<ExecutableMemory> ExecutableMemory::allocate(std::size_t bytes)
{
    std::size_t const page = page_size();
    std::size_t const size = ((bytes == 0 ? 1 : bytes) + page - 1) / page * page;
#if SASHFOLD_MAP_JIT
    void* const base = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
#else
    void* const base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (base == MAP_FAILED)
        return std::nullopt;
    return ExecutableMemory(static_cast<std::byte*>(base), size);
}

ExecutableMemory::ExecutableMemory(ExecutableMemory&& other) noexcept
    : m_base(std::exchange(other.m_base, nullptr))
    , m_size(std::exchange(other.m_size, 0))
    , m_frames(std::exchange(other.m_frames, {}))
{
}

ExecutableMemory& ExecutableMemory::operator=(ExecutableMemory&& other) noexcept
{
    if (this != &other) {
        release();
        m_base = std::exchange(other.m_base, nullptr);
        m_size = std::exchange(other.m_size, 0);
        m_frames = std::exchange(other.m_frames, {});
    }
    return *this;
}

ExecutableMemory::~ExecutableMemory()
{
    release();
}

bool ExecutableMemory::unseal(std::size_t offset, std::size_t bytes)
{
#if SASHFOLD_MAP_JIT
    if (m_base == nullptr || bytes == 0 || offset + bytes > m_size)
        return false;
    pthread_jit_write_protect_np(0);
    return true;
#else
    std::size_t const page = page_size();
    std::size_t const first = offset / page * page;
    std::size_t const end = (offset + bytes + page - 1) / page * page;
    if (m_base == nullptr || bytes == 0 || end > m_size)
        return false;
    return mprotect(m_base + first, end - first, PROT_READ | PROT_WRITE) == 0;
#endif
}

bool ExecutableMemory::seal(std::size_t offset, std::size_t bytes)
{
#if SASHFOLD_MAP_JIT
    if (m_base == nullptr || bytes == 0 || offset + bytes > m_size)
        return false;
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(m_base + offset, bytes);
    return true;
#else
    std::size_t const page = page_size();
    std::size_t const first = offset / page * page;
    std::size_t const end = (offset + bytes + page - 1) / page * page;
    if (m_base == nullptr || bytes == 0 || end > m_size)
        return false;
    if (mprotect(m_base + first, end - first, PROT_READ | PROT_EXEC) != 0) {
        mprotect(m_base + first, end - first, PROT_NONE);
        return false;
    }
    // A machine whose instruction cache does not snoop its data cache
    // (AArch64) must be told what changed; elsewhere this is nothing.
    char* const begin = reinterpret_cast<char*>(m_base + offset);
    __builtin___clear_cache(begin, begin + bytes);
    return true;
#endif
}

bool ExecutableMemory::describe_frames(std::size_t, std::size_t, std::size_t)
{
    return true;
}

void ExecutableMemory::release()
{
    if (m_base != nullptr)
        munmap(m_base, m_size);
    m_base = nullptr;
    m_size = 0;
    m_frames.clear();
}

}
