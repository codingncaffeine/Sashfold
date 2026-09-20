#pragma once

// The sound seam: a stream of samples out to whatever the machine plays
// through. On Linux that is the sound server's own protocol, spoken
// directly over its socket (platform/linux/AudioPulse.cpp) the same way the
// window speaks the compositor's — no client library, per the pledge. Where
// no implementation exists yet the seam opens nothing and says so, and the
// engine plays no sound rather than pretending to.
//
// Samples are interleaved 32-bit floats, which is what every decoder here
// produces and what the servers take without converting. A device is fed
// from whatever thread decodes; it never runs on the window's.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace sashfold::platform {

struct AudioFormat {
    unsigned rate = 48000;
    unsigned channels = 2;
};

// Where playback has reached, as the device knows it: what it has been
// given, and what has actually left for the speakers. The difference is the
// sound still in flight, which is what a picture must be shown against.
struct AudioClock {
    double written_seconds = 0; // samples handed over
    double played_seconds = 0; // of those, heard by now
    double latency_seconds = 0; // written - played, as the server reports it
    bool valid = false; // false until the server has answered once
};

class AudioDevice {
public:
    // Opens a playback stream under `name` (what a mixer shows). Null when
    // this build has no sound, or the machine has no server, with why in
    // `error`. The device does not begin to play until it is given samples.
    static std::unique_ptr<AudioDevice> open(AudioFormat const&, std::string const& name, std::string& error);

    virtual ~AudioDevice() = default;
    AudioDevice(AudioDevice const&) = delete;
    AudioDevice& operator=(AudioDevice const&) = delete;

    // Takes interleaved frames, as many as there is room for; answers how
    // many FRAMES it took. Never blocks and never waits on the server.
    virtual std::size_t write(std::span<float const> interleaved) = 0;
    // Room for this many more frames right now.
    virtual std::size_t writable_frames() const = 0;
    virtual AudioClock clock() const = 0;
    // Playing or held. A held stream keeps what it has.
    virtual void set_paused(bool) = 0;
    // 0 silent, 1 as loud as the sample says; the mixer shows it.
    virtual void set_volume(double) = 0;
    // Everything given so far is thrown away (a seek).
    virtual void flush() = 0;
    // False once the server has gone or the stream was killed; what is
    // written after that is dropped.
    virtual bool ok() const = 0;
    AudioFormat const& format() const { return m_format; }

protected:
    explicit AudioDevice(AudioFormat const& format)
        : m_format(format)
    {
    }
    AudioFormat m_format;
};

}
