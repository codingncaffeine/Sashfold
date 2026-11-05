// A preload for a test run on glibc: every fresh block and every freed block
// is filled with an 8-byte word that the engine reads as a cell (top fifteen
// bits and bit 1 clear) and that faults when followed, and a freed block is
// kept back from the allocator for a while.
//
// Why both. A value swept while something still names it is a crash only if
// the block is given to something else, and glibc gives the last freed block
// of a size to the next request of that size: the dangling name then points
// at a live cell, and the run passes with a wrong value in it. Windows picks
// among the free blocks, so the same fault crashed there now and then for
// six weeks (import.meta.url, 2026-11). The address sanitizer keeps freed
// blocks back as well, but its build orders a call's arguments differently
// from the release build, and it was the release build's order that was
// wrong. A fill of one repeated byte cannot stand in for this either: every
// such word but zero is a number to the engine, never a cell.
//
//   fresh 0x00006b6bdeadbee0    freed 0x00006b6bf4eed0e0
//   POISON_FRESH=0, POISON_FREED=0 switch a fill off
//   POISON_QUARANTINE=<blocks> freed blocks kept back (200000; 0 keeps none)
//
// Run through tools/poison.sh, with the pages collecting at every allocation
// (SASHFOLD_GC_STRESS=1; test_bindings' pages always do).

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <pthread.h>

extern "C" {
void* __libc_malloc(std::size_t);
void __libc_free(void*);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
}

namespace {

constexpr std::uint64_t fresh_word = 0x00006b6bdeadbee0ull;
constexpr std::uint64_t freed_word = 0x00006b6bf4eed0e0ull;

int g_fresh = -1;
int g_freed = -1;
void** g_ring = nullptr;
std::size_t g_ring_size = 0;
std::size_t g_ring_at = 0;
bool g_ring_ready = false;
pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;

void setup()
{
    char const* const fresh = std::getenv("POISON_FRESH");
    char const* const freed = std::getenv("POISON_FREED");
    g_fresh = !(fresh != nullptr && fresh[0] == '0');
    g_freed = !(freed != nullptr && freed[0] == '0');
}

// Whole words only, from the first 8-aligned offset at or after `from`.
void fill(void* block, std::size_t from, std::size_t to, std::uint64_t word)
{
    for (std::size_t at = (from + 7) & ~static_cast<std::size_t>(7); at + 8 <= to; at += 8)
        std::memcpy(static_cast<char*>(block) + at, &word, 8);
}

}

extern "C" {

void* malloc(std::size_t bytes)
{
    if (g_fresh < 0)
        setup();
    void* const block = __libc_malloc(bytes);
    if (block != nullptr && g_fresh)
        fill(block, 0, malloc_usable_size(block), fresh_word);
    return block;
}

void free(void* block)
{
    if (block == nullptr)
        return;
    if (g_freed < 0)
        setup();
    if (g_freed)
        fill(block, 0, malloc_usable_size(block), freed_word);
    pthread_mutex_lock(&g_ring_lock);
    if (!g_ring_ready) {
        char const* const asked = std::getenv("POISON_QUARANTINE");
        g_ring_size = asked != nullptr ? static_cast<std::size_t>(std::atol(asked)) : 200000;
        if (g_ring_size != 0) {
            g_ring = static_cast<void**>(__libc_malloc(g_ring_size * sizeof(void*)));
            std::memset(g_ring, 0, g_ring_size * sizeof(void*));
        }
        g_ring_ready = true;
    }
    void* leaving = block;
    if (g_ring_size != 0) {
        leaving = g_ring[g_ring_at];
        g_ring[g_ring_at] = block;
        g_ring_at = (g_ring_at + 1) % g_ring_size;
    }
    pthread_mutex_unlock(&g_ring_lock);
    if (leaving != nullptr)
        __libc_free(leaving);
}

void* realloc(void* block, std::size_t bytes)
{
    if (g_fresh < 0)
        setup();
    if (block == nullptr)
        return malloc(bytes);
    std::size_t const before = malloc_usable_size(block);
    void* const moved = __libc_realloc(block, bytes);
    if (moved != nullptr && g_fresh) {
        std::size_t const after = malloc_usable_size(moved);
        if (after > before)
            fill(moved, before, after, fresh_word);
    }
    return moved;
}

void* memalign(std::size_t alignment, std::size_t bytes)
{
    if (g_fresh < 0)
        setup();
    void* const block = __libc_memalign(alignment, bytes);
    if (block != nullptr && g_fresh)
        fill(block, 0, malloc_usable_size(block), fresh_word);
    return block;
}

void* aligned_alloc(std::size_t alignment, std::size_t bytes)
{
    return memalign(alignment, bytes);
}

int posix_memalign(void** out, std::size_t alignment, std::size_t bytes)
{
    void* const block = memalign(alignment, bytes);
    if (block == nullptr)
        return 12; // ENOMEM
    *out = block;
    return 0;
}

}
