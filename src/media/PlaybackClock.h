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
#include <cstddef>
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
        // Playback would advance but waits for the video's pictures (its
        // preroll): standing until the thread that makes them has made
        // `preroll_pictures` beyond the one shown, or all there is, and
        // says so (release_preroll); from then it runs, from that moment.
        // `preroll_generation` names the start or the seek it waits for: a
        // state said again with the same one keeps a release already made.
        // Where there is sound to start with the pictures, the clock only
        // marks them made (preroll_ready) and the element starts both.
        bool preroll = false;
        bool preroll_alone = true;
        std::uint64_t preroll_generation = 0;
        std::size_t preroll_pictures = 0;
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
        bool const released = released_locked();
        if (!state.advancing && !released)
            return state.position;
        auto const from = released ? m_released_at : state.at;
        double target = state.position
            + std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count() * state.rate;
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
        return m_state.advancing || released_locked();
    }

    // Waiting for pictures, and how many: for the thread that makes them.
    bool prerolling() const
    {
        std::lock_guard const lock(m_mutex);
        return m_state.preroll && m_ready_generation != m_state.preroll_generation;
    }

    std::size_t preroll_pictures() const
    {
        std::lock_guard const lock(m_mutex);
        return m_state.preroll_pictures;
    }

    // The pictures waited for are made: playback runs from now. (Said by
    // the thread that makes them, which otherwise only reads the clock.)
    void release_preroll() const
    {
        std::lock_guard const lock(m_mutex);
        if (!m_state.preroll || m_ready_generation == m_state.preroll_generation)
            return;
        m_ready_generation = m_state.preroll_generation;
        if (m_state.preroll_alone) {
            m_released_generation = m_state.preroll_generation;
            m_released_at = std::chrono::steady_clock::now();
        }
    }

    // Whether the pictures the preroll of `generation` waits for are made.
    bool preroll_ready(std::uint64_t generation) const
    {
        std::lock_guard const lock(m_mutex);
        return generation != 0 && m_ready_generation == generation;
    }

    // Whether the preroll of `generation` was released.
    bool released(std::uint64_t generation) const
    {
        std::lock_guard const lock(m_mutex);
        return generation != 0 && m_released_generation == generation;
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
    bool released_locked() const { return m_state.preroll && m_state.preroll_generation != 0 && m_released_generation == m_state.preroll_generation; }

    mutable std::mutex m_mutex;
    State m_state;
    mutable std::uint64_t m_ready_generation = 0;
    mutable std::uint64_t m_released_generation = 0;
    mutable std::chrono::steady_clock::time_point m_released_at {};
};

}
