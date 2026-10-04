#include "platform/ExecutableMemory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <utility>

namespace sashfold::platform {

std::optional<ExecutableMemory> ExecutableMemory::allocate(std::size_t bytes)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    std::size_t const page = info.dwPageSize > 0 ? info.dwPageSize : 4096;
    std::size_t const size = ((bytes == 0 ? 1 : bytes) + page - 1) / page * page;
    void* const base = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (base == nullptr)
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
    DWORD previous = 0;
    if (!VirtualProtect(m_base, m_size, PAGE_EXECUTE_READ, &previous)) {
        VirtualProtect(m_base, m_size, PAGE_NOACCESS, &previous);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), m_base, m_size);
    m_sealed = true;
    return true;
}

void ExecutableMemory::release()
{
    if (m_base != nullptr)
        VirtualFree(m_base, 0, MEM_RELEASE);
    m_base = nullptr;
    m_size = 0;
    m_sealed = false;
}

}
