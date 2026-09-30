#pragma once

// Where a sound stream has played to, between a sound server's answers.
//
// A server counts in the blocks its device takes — a fiftieth of a second,
// a twentieth, as it pleases — and says the same until the next block: its
// word is a staircase, right at the moment of each step and further behind
// with every millisecond after. A picture shown by that word alone is shown
// in steps too: a video of thirty frames a second held for one block and
// hurried through the next. So the clock runs by the machine's own between
// answers, and is set by the answers that catch a step soonest: of those of
// the last seconds, the one furthest ahead of the machine's clock. (Whoever
// asks the server asks at uneven intervals, so that the answers do not all
// fall at one place in a block.) What the answers say is approached, not
// jumped to: the clock runs a little fast or slow for a moment, and never
// stands or leaps. Answers that stand still for longer than any block are a
// stream that is not playing, and the clock stands with them until they
// move; a pause stands it at once.

#include <algorithm>
#include <chrono>
#include <deque>

namespace sashfold::platform {

class SoundClock {
public:
    using time_point = std::chrono::steady_clock::time_point;

    // The server's answers standing still for this long is a stream that is
    // not playing: longer than the largest block a device takes at a time.
    static constexpr std::chrono::milliseconds stall { 250 };
    // The answers the clock is set by: those of this long.
    static constexpr std::chrono::seconds answers_kept { 2 };
    // How fast the clock gives way to what the answers say: three parts in
    // a hundred of the time that passes, which nobody sees in a picture's
    // timing.
    static constexpr double slew = 0.03;

    explicit SoundClock(time_point epoch = std::chrono::steady_clock::now())
        : m_epoch(epoch)
    {
    }

    // An answer from the server: where it says the stream has played to,
    // in seconds, and when the answer came.
    void take_answer(double heard, time_point now)
    {
        if (!m_answered) {
            m_answered = true;
            m_server_played = heard;
            m_server_moved_at = now;
            m_standing = heard;
            return;
        }
        if (heard > m_server_played + 1e-6) {
            m_server_played = heard;
            m_server_moved_at = now;
            if (m_paused)
                return; // (the server has not heard of the pause yet)
            double const had = ahead_by(now);
            m_answers.push_back({ now, heard - seconds_at(now) });
            while (now - m_answers.front().at > answers_kept)
                m_answers.pop_front();
            m_ahead_wanted = m_answers.front().ahead;
            for (Answer const& answer : m_answers)
                m_ahead_wanted = std::max(m_ahead_wanted, answer.ahead);
            m_ahead = m_running ? had : m_ahead_wanted; // a clock that was standing starts where the answer says
            m_ahead_at = now;
            m_running = true;
        } else if (m_running && now - m_server_moved_at > stall) {
            m_standing = heard;
            m_running = false;
            m_answers.clear();
        }
    }

    // The stream is held, or let go again: held, the clock stands where it
    // has reached; let go, it runs again when the answers move again.
    void set_paused(bool paused, time_point now)
    {
        if (paused == m_paused)
            return;
        m_standing = played_by(now);
        m_running = false;
        m_answers.clear();
        m_paused = paused;
    }

    // Everything forgotten, as when the stream is emptied for a seek: the
    // count begins again from nothing.
    void reset()
    {
        bool const paused = m_paused;
        *this = SoundClock(m_epoch);
        m_paused = paused;
    }

    // Where the stream has played to by `now`.
    double played_by(time_point now) const
    {
        if (!m_running || m_paused)
            return m_standing;
        return seconds_at(now) + ahead_by(now);
    }

    bool running() const { return m_running && !m_paused; }

private:
    struct Answer {
        time_point at;
        double ahead; // how far what it said was ahead of the machine's clock
    };

    double seconds_at(time_point at) const { return std::chrono::duration<double>(at - m_epoch).count(); }

    // How far the stream is ahead of the machine's clock, as the clock has
    // it by `now`: on its way to what the answers say, no faster than slew.
    double ahead_by(time_point now) const
    {
        double const room = slew * std::chrono::duration<double>(now - m_ahead_at).count();
        return m_ahead + std::clamp(m_ahead_wanted - m_ahead, -room, room);
    }

    time_point m_epoch;
    std::deque<Answer> m_answers; // those of the last seconds that moved
    double m_ahead_wanted = 0; // what the answers say
    double m_ahead = 0; // what the clock had at m_ahead_at, on its way there
    time_point m_ahead_at {};
    double m_standing = 0; // where the clock stands while the stream does not play
    double m_server_played = -1; // what the server last said
    time_point m_server_moved_at {}; // and when that last moved
    bool m_answered = false;
    bool m_running = false;
    bool m_paused = false;
};

}
