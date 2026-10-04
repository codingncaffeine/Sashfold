#include "platform/ExecutableMemory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <utility>

namespace sashfold::platform {

namespace {

std::size_t page_size()
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize > 0 ? info.dwPageSize : 4096;
}

}

std::optional<ExecutableMemory> ExecutableMemory::allocate(std::size_t bytes)
{
    std::size_t const page = page_size();
    std::size_t const size = ((bytes == 0 ? 1 : bytes) + page - 1) / page * page;
    void* const base = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (base == nullptr)
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
    std::size_t const page = page_size();
    std::size_t const first = offset / page * page;
    std::size_t const end = (offset + bytes + page - 1) / page * page;
    if (m_base == nullptr || bytes == 0 || end > m_size)
        return false;
    DWORD previous = 0;
    return VirtualProtect(m_base + first, end - first, PAGE_READWRITE, &previous) != 0;
}

bool ExecutableMemory::seal(std::size_t offset, std::size_t bytes)
{
    std::size_t const page = page_size();
    std::size_t const first = offset / page * page;
    std::size_t const end = (offset + bytes + page - 1) / page * page;
    if (m_base == nullptr || bytes == 0 || end > m_size)
        return false;
    DWORD previous = 0;
    if (!VirtualProtect(m_base + first, end - first, PAGE_EXECUTE_READ, &previous)) {
        VirtualProtect(m_base + first, end - first, PAGE_NOACCESS, &previous);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), m_base + offset, bytes);
    return true;
}

bool ExecutableMemory::describe_frames(std::size_t code, std::size_t entry, std::size_t count)
{
    if (m_base == nullptr || count == 0 || code + entry + count * sizeof(RUNTIME_FUNCTION) > m_size)
        return false;
    auto* const table = reinterpret_cast<PRUNTIME_FUNCTION>(m_base + code + entry);
    if (!RtlAddFunctionTable(table, static_cast<DWORD>(count), reinterpret_cast<DWORD64>(m_base + code)))
        return false;
    m_frames.push_back(table);
    return true;
}

void ExecutableMemory::release()
{
    for (void* const table : m_frames)
        RtlDeleteFunctionTable(static_cast<PRUNTIME_FUNCTION>(table));
    m_frames.clear();
    if (m_base != nullptr)
        VirtualFree(m_base, 0, MEM_RELEASE);
    m_base = nullptr;
    m_size = 0;
}

}
