#include "platform/MemoryWatch.h"

#include "platform/Memory.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#if defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#define SASHFOLD_WALKS_STACKS 1
#endif
#endif

namespace sashfold::platform {

namespace {

std::atomic<bool> g_running { false };
std::atomic<bool> g_tripped { false };
std::size_t g_ceiling = 0;
int g_interval_ms = 250;
MemoryWatch::Tripped g_on_trip = nullptr;

void pause_ms(int milliseconds)
{
#ifdef _WIN32
    Sleep(static_cast<DWORD>(milliseconds));
#else
    timespec wait { milliseconds / 1000, static_cast<long>(milliseconds % 1000) * 1000000L };
    nanosleep(&wait, nullptr);
#endif
}

#ifndef _WIN32
pthread_t g_main_thread;
pthread_t g_watch_thread;
std::mutex g_page_threads_mutex;
std::vector<pthread_t> g_page_threads;

#ifdef SASHFOLD_WALKS_STACKS
// On the main thread, in the middle of whatever it was doing — which, when
// memory ran away, is the running away. Nothing here allocates: the frames
// go to stderr as the dynamic loader knows them (a name where the binary
// exports one, else the offset to give addr2line).
void write_where_it_stands(int)
{
    bool const main_thread = pthread_equal(pthread_self(), g_main_thread) != 0;
    if (!main_thread) {
        static constexpr char heading[] = "A page's thread was here:\n";
        ssize_t const written = write(STDERR_FILENO, heading, sizeof heading - 1);
        (void)written;
    }
    void* frames[64];
    int const count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    // The main thread's is the last word; a page's thread goes on until it is.
    if (main_thread)
        std::abort();
}
#endif
#endif

// What the watch does unless told otherwise: says so, has the main thread
// say where it was, and ends the process.
void say_and_abort(std::size_t resident, std::size_t ceiling)
{
    char line[320];
    int const length = std::snprintf(line, sizeof line,
        "sashfold: memory ceiling reached: %zu MB resident, the ceiling is %zu MB (--memory-ceiling). "
        "Ending now, before the machine runs out; the tabs come back at the next start.\n",
        resident >> 20, ceiling >> 20);
    static constexpr char main_heading[] = "The main thread was here:\n";
#ifdef _WIN32
    if (length > 0)
        std::fwrite(line, 1, static_cast<std::size_t>(length), stderr);
    std::fwrite(main_heading, 1, sizeof main_heading - 1, stderr);
    std::fflush(stderr);
#else
    if (length > 0) {
        ssize_t const written = write(STDERR_FILENO, line, static_cast<std::size_t>(length));
        (void)written;
    }
#ifdef SASHFOLD_WALKS_STACKS
    // The threads that run the pages write where they stand first, each in
    // the middle of whatever it was doing.
    std::vector<pthread_t> pages;
    {
        std::lock_guard<std::mutex> const lock(g_page_threads_mutex);
        pages = g_page_threads;
    }
    for (pthread_t const thread : pages)
        pthread_kill(thread, SIGUSR2);
    if (!pages.empty())
        pause_ms(300);
#endif
    {
        ssize_t const written = write(STDERR_FILENO, main_heading, sizeof main_heading - 1);
        (void)written;
    }
#ifdef SASHFOLD_WALKS_STACKS
    // The main thread writes its own stack and aborts. Should the signal
    // never reach it, this thread ends the process itself.
    pthread_kill(g_main_thread, SIGUSR2);
    pause_ms(2000);
#endif
#endif
    std::abort();
}

void watch_once_tripped(std::size_t resident)
{
    g_tripped.store(true);
    if (g_on_trip)
        g_on_trip(resident, g_ceiling);
    else
        say_and_abort(resident, g_ceiling);
}

#ifdef _WIN32
HANDLE g_watch_handle = nullptr;

DWORD WINAPI watch(LPVOID)
#else
void* watch(void*)
#endif
{
    while (g_running.load()) {
        std::size_t const resident = resident_set_bytes();
        if (resident > g_ceiling) {
            watch_once_tripped(resident);
            break;
        }
        pause_ms(g_interval_ms);
    }
#ifdef _WIN32
    return 0;
#else
    return nullptr;
#endif
}

}

void MemoryWatch::start(std::size_t ceiling_bytes, Tripped on_trip, int interval_ms)
{
    stop();
    if (ceiling_bytes == 0)
        return;
    g_ceiling = ceiling_bytes;
    g_on_trip = on_trip;
    g_interval_ms = interval_ms > 0 ? interval_ms : 250;
    g_tripped.store(false);
    g_running.store(true);
#ifdef _WIN32
    g_watch_handle = CreateThread(nullptr, 0, watch, nullptr, 0, nullptr);
    if (!g_watch_handle)
        g_running.store(false);
#else
    g_main_thread = pthread_self();
#ifdef SASHFOLD_WALKS_STACKS
    // The first walk loads what walking needs; done here, no walk from the
    // signal's handler has to allocate.
    void* warm[4];
    (void)backtrace(warm, 4);
    struct sigaction action {};
    action.sa_handler = write_where_it_stands;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR2, &action, nullptr);
#endif
    if (pthread_create(&g_watch_thread, nullptr, watch, nullptr) != 0)
        g_running.store(false);
#endif
}

void MemoryWatch::stop()
{
    if (!g_running.exchange(false))
        return;
#ifdef _WIN32
    if (g_watch_handle) {
        WaitForSingleObject(g_watch_handle, INFINITE);
        CloseHandle(g_watch_handle);
        g_watch_handle = nullptr;
    }
#else
    pthread_join(g_watch_thread, nullptr);
#endif
}

bool MemoryWatch::tripped() { return g_tripped.load(); }

void MemoryWatch::add_page_thread()
{
#ifndef _WIN32
    std::lock_guard<std::mutex> const lock(g_page_threads_mutex);
    g_page_threads.push_back(pthread_self());
#endif
}

void MemoryWatch::remove_page_thread()
{
#ifndef _WIN32
    std::lock_guard<std::mutex> const lock(g_page_threads_mutex);
    pthread_t const self = pthread_self();
    g_page_threads.erase(std::remove_if(g_page_threads.begin(), g_page_threads.end(),
                             [self](pthread_t const thread) { return pthread_equal(thread, self) != 0; }),
        g_page_threads.end());
#endif
}

}
