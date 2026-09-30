#pragma once

#include <chrono>
#include <cstdio>
#include <string>

namespace sashfold {

// One clock for every trace line the process writes — the media, network,
// throw and turn traces — so a run's log reads in one order of events.
// Seconds since the first call; main() calls it first thing, so the
// origin is the process's start.
inline double trace_seconds()
{
    static auto const origin = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
}

// "[12.345] " — the stamp a trace line begins with.
inline std::string trace_stamp()
{
    char text[32];
    std::snprintf(text, sizeof text, "[%.3f] ", trace_seconds());
    return text;
}

}
