#pragma once

// A watch on the process's own memory. One process holds every tab, and a
// runaway — a page's script past what the heap's ceiling sees, or a loop of
// the engine's own — fills the machine's memory at gigabytes a second: the
// kernel then kills whatever it picks, after the whole desktop has stalled,
// and nothing is left to say what ran away. The watch ends the process
// first, at a ceiling, and says where it was.
//
// A thread of its own (the one thread the engine has beside its own) reads
// the resident set four times a second. The first time it is over the
// ceiling it acts, once: by default it writes a line to stderr, has the
// main thread write where it stands — its call stack, on the systems that
// can walk one, which in a runaway IS the runaway — and aborts. The session
// file is at most a second old, so the tabs come back at the next start.
//
// Tests pass an action of their own, which runs on the watch's thread.

#include <cstddef>

namespace sashfold::platform {

class MemoryWatch {
public:
    using Tripped = void (*)(std::size_t resident_bytes, std::size_t ceiling_bytes);

    // Starts the watch; a ceiling of 0 starts nothing. One at a time: a
    // second start replaces the first. Call from the main thread — the
    // thread whose stack the default action reports.
    static void start(std::size_t ceiling_bytes, Tripped on_trip = nullptr, int interval_ms = 250);
    // Ends it and waits for its thread. Safe when none runs.
    static void stop();
    // Whether the watch has acted since it was started.
    static bool tripped();
};

}
