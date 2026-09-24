#include "platform/Memory.h"

#include <cstdio>

#include <pthread.h>
#include <sys/resource.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

namespace sashfold::platform {

#ifdef __APPLE__

// The size pthreads keeps for each thread; for the first thread, what the
// executable was linked with (-stack_size) or the limit at exec.
std::size_t current_thread_stack_bytes()
{
    return pthread_get_stacksize_np(pthread_self());
}

// The first thread's stack is fixed at exec here: report it.
std::size_t widen_main_thread_stack(std::size_t)
{
    return current_thread_stack_bytes();
}

#else

// glibc answers for the first thread from the stack limit as it is now
// and the mappings, so a limit raised at run time is seen at once.
std::size_t current_thread_stack_bytes()
{
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0)
        return 0;
    std::size_t bytes = 0;
    if (pthread_attr_getstacksize(&attributes, &bytes) != 0)
        bytes = 0;
    pthread_attr_destroy(&attributes);
    return bytes;
}

// The soft stack limit, raised to `bytes` or as far as the hard limit
// allows. The kernel keeps a gap of at least 128 MB below the stack of a
// process that started under the usual 8 MB limit, so growth to that much
// is room the mappings never took; asking for more than the gap held is
// answered by the limit, and a stack that meets a mapping first ends the
// process as it always did — the engine's budget stays under 256 MB.
std::size_t widen_main_thread_stack(std::size_t bytes)
{
    rlimit limit {};
    if (getrlimit(RLIMIT_STACK, &limit) != 0)
        return current_thread_stack_bytes();
    rlim_t wanted = static_cast<rlim_t>(bytes);
    if (limit.rlim_max != RLIM_INFINITY && wanted > limit.rlim_max)
        wanted = limit.rlim_max;
    if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur >= wanted)
        return current_thread_stack_bytes();
    limit.rlim_cur = wanted;
    setrlimit(RLIMIT_STACK, &limit);
    return current_thread_stack_bytes();
}

#endif

#ifdef __APPLE__

std::size_t resident_set_bytes()
{
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return static_cast<std::size_t>(info.resident_size);
}

std::uint64_t physical_memory_bytes()
{
    std::uint64_t bytes = 0;
    std::size_t size = sizeof bytes;
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0)
        return 0;
    return bytes;
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

std::uint64_t physical_memory_bytes()
{
    long const pages = sysconf(_SC_PHYS_PAGES);
    long const page = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page <= 0)
        return 0;
    return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
}

#endif

}
