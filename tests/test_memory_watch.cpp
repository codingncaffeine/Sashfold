// The memory watch and the ceilings it and the script heaps are given: the
// arithmetic against values worked out by hand, the watch against memory
// this test takes and touches itself — and, where a process can be forked,
// the default action for real: a child that runs away ends by SIGABRT with
// the watch's line (and, where stacks can be walked, its stack) on stderr.

#include "platform/Memory.h"
#include "platform/MemoryWatch.h"
#include "Test.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

using namespace sashfold;

namespace {

void pause_ms(int milliseconds)
{
#ifdef _WIN32
    Sleep(static_cast<DWORD>(milliseconds));
#else
    timespec wait { milliseconds / 1000, static_cast<long>(milliseconds % 1000) * 1000000L };
    nanosleep(&wait, nullptr);
#endif
}

std::atomic<std::size_t> seen_resident { 0 };
std::atomic<std::size_t> seen_ceiling { 0 };
std::atomic<int> trips { 0 };

void note_trip(std::size_t resident, std::size_t ceiling)
{
    seen_resident.store(resident);
    seen_ceiling.store(ceiling);
    trips.fetch_add(1);
}

// Memory that is the process's for real: taken, and every page written to
// — through a volatile pointer, or a compiler that sees nothing read the
// bytes back leaves the pages untouched, and untouched pages are nobody's.
std::vector<char> take(std::size_t bytes)
{
    std::vector<char> block(bytes);
    volatile char* const page = block.data();
    for (std::size_t at = 0; at < bytes; at += 1024)
        page[at] = static_cast<char>(1 + at % 100);
    return block;
}

}

int main()
{
    std::uint64_t const gigabyte = 1024ull * 1024ull * 1024ull;
    std::uint64_t const megabyte = 1024ull * 1024ull;

    // --- The ceilings, from the machine's memory -----------------------------
    if (sizeof(void*) >= 8) {
        // A page's script heap: a quarter, within 512 MB and 4 GB.
        CHECK_EQ(platform::js_heap_limit_for(64 * gigabyte), static_cast<std::size_t>(4 * gigabyte));
        CHECK_EQ(platform::js_heap_limit_for(16 * gigabyte), static_cast<std::size_t>(4 * gigabyte));
        CHECK_EQ(platform::js_heap_limit_for(8 * gigabyte), static_cast<std::size_t>(2 * gigabyte));
        CHECK_EQ(platform::js_heap_limit_for(4 * gigabyte), static_cast<std::size_t>(1 * gigabyte));
        CHECK_EQ(platform::js_heap_limit_for(1 * gigabyte), static_cast<std::size_t>(512 * megabyte));
        CHECK_EQ(platform::js_heap_limit_for(0), static_cast<std::size_t>(4 * gigabyte));
        // The whole process: half, within 2 GB and 16 GB.
        CHECK_EQ(platform::memory_ceiling_for(64 * gigabyte), static_cast<std::size_t>(16 * gigabyte));
        CHECK_EQ(platform::memory_ceiling_for(32 * gigabyte), static_cast<std::size_t>(16 * gigabyte));
        CHECK_EQ(platform::memory_ceiling_for(16 * gigabyte), static_cast<std::size_t>(8 * gigabyte));
        CHECK_EQ(platform::memory_ceiling_for(6 * gigabyte), static_cast<std::size_t>(3 * gigabyte));
        CHECK_EQ(platform::memory_ceiling_for(2 * gigabyte), static_cast<std::size_t>(2 * gigabyte));
        CHECK_EQ(platform::memory_ceiling_for(0), static_cast<std::size_t>(8 * gigabyte));
    }

    // --- What the OS says ------------------------------------------------------
    std::size_t const resident = platform::resident_set_bytes();
    std::uint64_t const physical = platform::physical_memory_bytes();
    CHECK(resident > 0);
    CHECK(physical > resident);
    CHECK(physical >= 256 * megabyte); // no machine this runs on has less

    // --- A ceiling of none starts nothing --------------------------------------
    platform::MemoryWatch::start(0, note_trip, 10);
    pause_ms(60);
    CHECK(!platform::MemoryWatch::tripped());
    platform::MemoryWatch::stop(); // and stopping nothing is safe
    platform::MemoryWatch::stop();

    // --- Under the ceiling nothing happens ------------------------------------
    platform::MemoryWatch::start(resident + static_cast<std::size_t>(gigabyte), note_trip, 10);
    {
        std::vector<char> const some = take(static_cast<std::size_t>(8 * megabyte));
        pause_ms(120);
        CHECK(!platform::MemoryWatch::tripped());
        CHECK_EQ(trips.load(), 0);
        CHECK(some.size() == 8 * megabyte);
    }
    platform::MemoryWatch::stop();

    // --- Over it, the watch acts: once, with what it read ----------------------
    std::size_t const ceiling = platform::resident_set_bytes() + static_cast<std::size_t>(48 * megabyte);
    platform::MemoryWatch::start(ceiling, note_trip, 10);
    {
        std::vector<char> const much = take(static_cast<std::size_t>(160 * megabyte));
        for (int waited = 0; waited < 5000 && !platform::MemoryWatch::tripped(); waited += 10)
            pause_ms(10);
        CHECK(platform::MemoryWatch::tripped());
        pause_ms(60); // time in which a watch that acts twice would
        CHECK_EQ(trips.load(), 1);
        CHECK_EQ(seen_ceiling.load(), ceiling);
        CHECK(seen_resident.load() > ceiling);
        CHECK(much.size() == 160 * megabyte);
    }
    platform::MemoryWatch::stop();
    // A watch started again starts clean.
    platform::MemoryWatch::start(platform::resident_set_bytes() + static_cast<std::size_t>(gigabyte), note_trip, 10);
    CHECK(!platform::MemoryWatch::tripped());
    platform::MemoryWatch::stop();

#ifndef _WIN32
    // --- The default action, for real, in a child -----------------------------
    // The child starts the watch 48 MB over what it holds and then takes
    // memory without end, 16 MB at a time, as a runaway does. It must end by
    // SIGABRT — not by this test's kill, not by running out — with the
    // watch's line on stderr; where stacks are walked, frames follow it.
    int channel[2];
    CHECK(pipe(channel) == 0);
    pid_t const child = fork();
    if (child == 0) {
        close(channel[0]);
        dup2(channel[1], STDERR_FILENO);
        platform::MemoryWatch::start(platform::resident_set_bytes() + static_cast<std::size_t>(48 * megabyte), nullptr, 10);
        std::vector<std::vector<char>> kept;
        for (int round = 0; round < 64; ++round) { // a gigabyte at most, should the watch never act
            kept.push_back(take(static_cast<std::size_t>(16 * megabyte)));
            pause_ms(20);
        }
        _exit(0);
    }
    close(channel[1]);
    std::string said;
    char buffer[4096];
    for (ssize_t got = 0; (got = read(channel[0], buffer, sizeof buffer)) > 0;)
        said.append(buffer, static_cast<std::size_t>(got));
    close(channel[0]);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    CHECK(said.find("memory ceiling reached") != std::string::npos);
    CHECK(said.find("--memory-ceiling") != std::string::npos);
#if defined(__GLIBC__) || defined(__APPLE__)
    // Frames, as backtrace_symbols_fd writes them: an address in brackets
    // (glibc) or a column of 0x… (macOS) on the lines after the watch's own.
    CHECK(said.find("0x", said.find("The main thread was here:")) != std::string::npos);
#endif
#endif

    return sashfold::test::report("memory_watch");
}
