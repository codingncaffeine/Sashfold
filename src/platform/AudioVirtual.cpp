#include "platform/Audio.h"

#include <algorithm>
#include <cmath>

// The device a headless run plays through (Audio.h): no speakers, a clock
// the host gives, and the same bookkeeping as the real stream — what was
// written, what has been heard, room for two seconds, a flush that starts
// the count again — so that the page and the element cannot tell.

namespace sashfold::platform {

namespace {

class VirtualAudioDevice final : public AudioDevice {
public:
    VirtualAudioDevice(AudioFormat const& format, std::function<double()> now_ms, std::function<void(std::span<float const>)> tap)
        : AudioDevice(format)
        , m_now(std::move(now_ms))
        , m_tap(std::move(tap))
        , m_last_ms(m_now())
        , m_capacity(static_cast<double>(format.rate) * 2)
    {
    }

    std::size_t write(std::span<float const> interleaved) override
    {
        advance();
        double const frames = static_cast<double>(interleaved.size() / std::max(1u, m_format.channels));
        double const taken = std::min(frames, room());
        if (m_tap)
            m_tap(interleaved.first(static_cast<std::size_t>(taken) * m_format.channels));
        m_written += taken;
        return static_cast<std::size_t>(taken);
    }

    std::size_t writable_frames() const override
    {
        advance();
        return static_cast<std::size_t>(room());
    }

    AudioClock clock() const override
    {
        advance();
        AudioClock clock;
        double const rate = static_cast<double>(m_format.rate);
        clock.written_seconds = m_written / rate;
        clock.played_seconds = m_played / rate;
        clock.latency_seconds = clock.written_seconds - clock.played_seconds;
        clock.valid = true;
        return clock;
    }

    void set_paused(bool paused) override
    {
        advance();
        m_paused = paused;
    }

    void set_volume(double) override { }

    void flush() override
    {
        advance();
        m_written = 0;
        m_played = 0;
    }

    bool ok() const override { return true; }

private:
    // Whole frames only: what write() takes is a count of frames.
    double room() const { return std::max(0.0, std::floor(m_capacity - (m_written - m_played))); }

    // What the clock has let be heard since it was last asked.
    void advance() const
    {
        double const now = m_now();
        if (!m_paused && now > m_last_ms)
            m_played = std::min(m_written, m_played + (now - m_last_ms) / 1000.0 * static_cast<double>(m_format.rate));
        m_last_ms = now;
    }

    std::function<double()> m_now;
    std::function<void(std::span<float const>)> m_tap;
    mutable double m_last_ms;
    double m_capacity; // frames
    double m_written = 0; // frames
    mutable double m_played = 0; // frames
    bool m_paused = false;
};

}

std::unique_ptr<AudioDevice> open_virtual_audio(AudioFormat const& format, std::function<double()> now_ms,
    std::function<void(std::span<float const>)> tap)
{
    return std::make_unique<VirtualAudioDevice>(format, std::move(now_ms), std::move(tap));
}

}
