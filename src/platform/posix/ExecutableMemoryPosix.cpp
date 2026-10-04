#include "platform/ExecutableMemory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <utility>

namespace sashfold::platform {

std::optional<ExecutableMemory> ExecutableMemory::allocate(std::size_t bytes)
{
    long const page_size = sysconf(_SC_PAGESIZE);
    std::size_t const page = page_size > 0 ? static_cast<std::size_t>(page_size) : 4096;
    std::size_t const size = ((bytes == 0 ? 1 : bytes) + page - 1) / page * page;
    void* const base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return std::nullopt;
    return ExecutableMemory(static_cast<std::byte*>(base), size);
}

ExecutableMemory::ExecutableMemory(ExecutableMemory&& other) noexcept
    : m_base(std::exchange(other.m_base, nullptr))
    , m_size(std::exchange(other.m_size, 0))
    , m_sealed(std::exchange(other.m_sealed, false))
{
}

ExecutableMemory& ExecutableMemory::operator=(ExecutableMemory&& other) noexcept
{
    if (this != &other) {
        release();
        m_base = std::exchange(other.m_base, nullptr);
        m_size = std::exchange(other.m_size, 0);
        m_sealed = std::exchange(other.m_sealed, false);
    }
    return *this;
}

ExecutableMemory::~ExecutableMemory()
{
    release();
}

bool ExecutableMemory::seal()
{
    if (m_base == nullptr || m_sealed)
        return m_sealed;
    if (mprotect(m_base, m_size, PROT_READ | PROT_EXEC) != 0) {
        mprotect(m_base, m_size, PROT_NONE);
        return false;
    }
    // A machine whose instruction cache does not snoop its data cache
    // (AArch64) must be told what changed; elsewhere this is nothing.
    char* const begin = reinterpret_cast<char*>(m_base);
    __builtin___clear_cache(begin, begin + m_size);
    m_sealed = true;
    return true;
}

void ExecutableMemory::release()
{
    if (m_base != nullptr)
        munmap(m_base, m_size);
    m_base = nullptr;
    m_size = 0;
    m_sealed = false;
}

}
