#pragma once

// Where a media element's playback is, for whoever needs to know between
// the element's own looks at it: the thread that makes a video's pictures,
// and the one that shows them. The element says where it stands each time
// it brings itself up to date — on its page's thread, which a busy page
// keeps for seconds at a time — and the clock runs on from there as the
// element itself would: by what has been heard where there are speakers,
// by the time since where there are none, and never past the end of what
// is buffered.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace sashfold::media {

class PlaybackClock {
public:
    struct State {
        bool advancing = false;
        double position = 0; // seconds, as of `at`
        std::chrono::steady_clock::time_point at {};
        double rate = 1;
        // Playback does not pass this: the end of the buffered range the
        // position is in, or the duration.
        double limit = 0;
        // The position by what the speakers have played, while there is a
        // stream through them; called from any thread.
        std::function<std::optional<double>()> heard;
    };

    void set(State state)
    {
        std::lock_guard const lock(m_mutex);
        m_state = std::move(state);
    }

    double seconds() const
    {
        std::lock_guard const lock(m_mutex);
        State const& state = m_state;
        if (!state.advancing)
            return state.position;
        double target = state.position
            + std::chrono::duration<double>(std::chrono::steady_clock::now() - state.at).count() * state.rate;
        if (state.heard) {
            if (std::optional<double> const heard = state.heard())
                target = std::max(state.position, *heard);
        }
        return std::clamp(target, 0.0, std::max(state.limit, 0.0));
    }

    std::int64_t now_ns() const { return static_cast<std::int64_t>(seconds() * 1e9); }

    bool advancing() const
    {
        std::lock_guard const lock(m_mutex);
        return m_state.advancing;
    }

    double rate() const
    {
        std::lock_guard const lock(m_mutex);
        return m_state.rate;
    }

    // What the clock was last told, in words, for the media trace.
    std::string told() const
    {
        std::lock_guard const lock(m_mutex);
        State const& state = m_state;
        double const since = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.at).count();
        std::string words = std::string(state.advancing ? "running" : "standing") + " from " + std::to_string(state.position)
            + " said " + std::to_string(since) + " s ago, rate " + std::to_string(state.rate) + ", not past " + std::to_string(state.limit);
        if (state.heard) {
            std::optional<double> const heard = state.heard();
            words += heard ? ", heard " + std::to_string(*heard) : std::string(", nothing heard yet");
        }
        return words;
    }

private:
    mutable std::mutex m_mutex;
    State m_state;
};

}
