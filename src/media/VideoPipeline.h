#pragma once

// A video track's pictures, made off the page's thread. The page's media
// tick hands over the coded frames a little ahead of playback, in decoding
// order; a thread of the pipeline's own decodes them on the machine's video
// hardware (media/Vp9Accelerator.h), reads each picture back and converts
// it for the painter (media/Yuv.h) — a few milliseconds a picture, none of
// which the page waits for — and the tick takes the newest picture whose
// time has come, as a browser's compositor takes the frame due at each
// display refresh.

#include "media/Vp9Accelerator.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sashfold::media {

struct VideoPicture {
    std::int64_t time_ns = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4
};

class VideoPipeline {
public:
    // A pipeline on the machine's VP9 decoder; null, with why, where it has none.
    static std::unique_ptr<VideoPipeline> open(std::string& error);
    explicit VideoPipeline(std::unique_ptr<Vp9Accelerator> accelerator);
    ~VideoPipeline();
    VideoPipeline(VideoPipeline const&) = delete;
    VideoPipeline& operator=(VideoPipeline const&) = delete;

    // What decodes, for the media trace.
    std::string const& device() const { return m_device; }

    // A coded frame — a block of the container, which may be a superframe
    // ending in the frame it shows — in decoding order. After a flush the
    // first must be a key frame.
    void push(std::int64_t time_ns, std::vector<std::uint8_t> data);
    // Where playback is, and how far ahead of it pictures are made: a frame
    // is decoded once its time is before `until_ns`, and one whose picture
    // a later frame due by `now_ns` would replace is decoded without being
    // made.
    void set_clock(std::int64_t now_ns, std::int64_t until_ns);
    // Forgets what is queued and made, as for a seek.
    void flush();
    // The newest picture made whose time is at or before `now_ns`, when one
    // has been made since the last taken; those before it are let go.
    std::optional<VideoPicture> take(std::int64_t now_ns);
    // A taken picture's buffer, to be written again.
    void recycle(std::vector<std::uint8_t> rgba);

    struct Counts {
        std::uint64_t decoded = 0; // frames the hardware decoded
        std::uint64_t failed = 0; // blocks that could not be read or decoded
        std::uint64_t made = 0; // pictures converted for the painter
        std::uint64_t skipped = 0; // shown frames that were late, so not made
    };
    Counts counts() const;
    // Frames handed over and not yet decoded.
    std::size_t queued() const;
    // Whether everything due by `now_ns` is decoded: no frame at or before
    // it waits, and none is being decoded — the picture due, if any, is
    // there to be taken.
    bool caught_up(std::int64_t now_ns) const;

private:
    struct Coded {
        std::int64_t time_ns;
        std::vector<std::uint8_t> data;
    };

    void run();

    std::unique_ptr<Vp9Accelerator> m_accelerator; // the thread's alone once it runs
    std::string m_device;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Coded> m_queue;
    std::deque<VideoPicture> m_ready;
    std::vector<std::vector<std::uint8_t>> m_spare;
    std::int64_t m_now_ns = 0;
    std::int64_t m_until_ns = 0;
    std::uint64_t m_generation = 0; // one more at every flush
    bool m_busy = false; // the thread is decoding a frame it took
    bool m_stop = false;
    Counts m_counts;

    std::thread m_thread; // last: it starts once everything above is ready
};

}
