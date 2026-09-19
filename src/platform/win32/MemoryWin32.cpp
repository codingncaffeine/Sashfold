#include "platform/Memory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <psapi.h>

namespace sashfold::platform {

// The working set, through the function kernel32 has carried since
// Windows 7 — looked up by name, so the import table stays as the pledge
// lists it and psapi.dll is never asked for.
std::size_t resident_set_bytes()
{
    using Query = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    static Query const query = [] {
        HMODULE const kernel32 = GetModuleHandleW(L"kernel32.dll");
        return kernel32 ? reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(kernel32, "K32GetProcessMemoryInfo")))
                        : nullptr;
    }();
    if (!query)
        return 0;
    PROCESS_MEMORY_COUNTERS counters {};
    counters.cb = sizeof counters;
    if (!query(GetCurrentProcess(), &counters, sizeof counters))
        return 0;
    return static_cast<std::size_t>(counters.WorkingSetSize);
}

// GlobalMemoryStatusEx is kernel32's own.
std::uint64_t physical_memory_bytes()
{
    MEMORYSTATUSEX status {};
    status.dwLength = sizeof status;
    if (!GlobalMemoryStatusEx(&status))
        return 0;
    return static_cast<std::uint64_t>(status.ullTotalPhys);
}

}
