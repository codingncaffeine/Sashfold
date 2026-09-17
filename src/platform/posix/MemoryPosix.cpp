#include "platform/Memory.h"

#include <cstdio>

#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

namespace sashfold::platform {

#ifdef __APPLE__

std::size_t resident_set_bytes()
{
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return static_cast<std::size_t>(info.resident_size);
}

#else

// /proc/self/statm: the process's size and resident set in pages, then
// the shares nobody asked for.
std::size_t resident_set_bytes()
{
    std::FILE* const statm = std::fopen("/proc/self/statm", "r");
    if (!statm)
        return 0;
    unsigned long size = 0;
    unsigned long resident = 0;
    int const read = std::fscanf(statm, "%lu %lu", &size, &resident);
    std::fclose(statm);
    if (read != 2)
        return 0;
    long const page = sysconf(_SC_PAGESIZE);
    return static_cast<std::size_t>(resident) * static_cast<std::size_t>(page > 0 ? page : 4096);
}

#endif

}
