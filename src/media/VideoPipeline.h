#pragma once

// A video track's pictures, made off the page's thread. The page's media
// tick hands over the coded frames a little ahead of playback, in decoding
// order; a thread of the pipeline's own decodes them on the machine's video
// hardware (media/Vp9Accelerator.h), reads each picture back and converts
// it for the painter (media/Yuv.h) — a few milliseconds a picture, none of
// which the page waits for.
//
// Who takes the pictures is one of two. A page that paints its own video
// takes the newest picture whose time has come at each tick (set_clock,
// take). A host that shows video apart from the page's painting — a
// compositor, as every browser has — gives the pipeline the element's clock
// (follow): the pipeline then keeps its own time, and whoever shows the
// pictures asks for the one due (show) from whatever thread it shows them
// on, however long the page's thread is kept by its scripts.

#include "core/Bitmap.h"
#include "media/PlaybackClock.h"
#include "media/Vp9Accelerator.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
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

// A picture as it is shown by a pipeline that follows a clock: held by
// whoever shows it for as long as it does, and never written again.
struct ShownPicture {
    std::int64_t time_ns = 0;
    std::uint64_t serial = 0; // one more with every picture shown
    Bitmap bitmap;
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
    // made. (Not for a pipeline that follows a clock.)
    void set_clock(std::int64_t now_ns, std::int64_t until_ns);
    // Forgets what is queued and made, as for a seek.
    void flush();
    // Forgets the coded frames from `time_ns` on, which the page has
    // replaced, so that what takes their place can be pushed — when none of
    // them has been decoded yet: false, and nothing forgotten, when one
    // has (the decoder then holds what is no longer the stream: flush).
    bool drop_from(std::int64_t time_ns);
    // The newest picture made whose time is at or before `now_ns`, when one
    // has been made since the last taken; those before it are let go.
    std::optional<VideoPicture> take(std::int64_t now_ns);
    // A taken picture's buffer, to be written again.
    void recycle(std::vector<std::uint8_t> rgba);

    // --- Following a clock ---------------------------------------------
    // From here on the pipeline reads the time itself: pictures are made
    // `ahead_ns` before they are due by the clock, and shown by it.
    void follow(std::shared_ptr<PlaybackClock const> clock, std::int64_t ahead_ns);
    bool follows() const;
    // The clock was set again (playback began, stopped, or moved).
    void clock_changed();
    // The picture due by the clock: the newest made whose time has come,
    // or the one shown last while none newer is due; null until the first.
    // From any thread.
    std::shared_ptr<ShownPicture const> show();
    // The picture shown last, the clock not looked at.
    std::shared_ptr<ShownPicture const> shown() const;
    // How long until a picture already made comes due, as the clock runs
    // now; nothing when none is waiting or the clock stands.
    std::optional<std::chrono::nanoseconds> next_picture_in() const;
    // Called, from the pipeline's thread or the one that set the clock,
    // whenever there may be a new picture to show: one was made, the clock
    // changed, or everything was flushed.
    void set_waker(std::function<void()> waker);

    struct Counts {
        std::uint64_t decoded = 0; // frames the hardware decoded
        std::uint64_t failed = 0; // blocks that could not be read or decoded
        std::uint64_t made = 0; // pictures converted for the painter
        std::uint64_t skipped = 0; // shown frames that were late, so not made
        std::uint64_t shown = 0; // pictures taken or shown
    };
    Counts counts() const;
    // The longest wait between two pictures shown since this was last
    // asked, in milliseconds: what a reader sees as a stutter.
    double take_longest_wait_ms();
    // Where the pipeline stands, in words, for the media trace: the clock,
    // the picture shown, what is made and waiting, what is still coded.
    std::string standing() const;
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
    // Called with the mutex held.
    std::int64_t now_locked() const;
    std::int64_t until_locked() const;
    void note_shown_locked();

    std::unique_ptr<Vp9Accelerator> m_accelerator; // the thread's alone once it runs
    std::string m_device;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Coded> m_queue;
    std::deque<VideoPicture> m_ready;
    std::vector<std::vector<std::uint8_t>> m_spare;
    std::int64_t m_now_ns = 0;
    std::int64_t m_until_ns = 0;
    std::optional<std::int64_t> m_decoded_to_ns; // the time of the last frame taken to be decoded, since the last flush
    std::shared_ptr<PlaybackClock const> m_clock; // set once, by follow()
    std::int64_t m_ahead_ns = 0;
    std::shared_ptr<ShownPicture> m_shown;
    std::shared_ptr<std::function<void()> const> m_waker;
    std::chrono::steady_clock::time_point m_shown_at {};
    double m_longest_wait_ms = 0;
    std::uint64_t m_generation = 0; // one more at every flush
    bool m_busy = false; // the thread is decoding a frame it took
    bool m_stop = false;
    Counts m_counts;

    std::thread m_thread; // last: it starts once everything above is ready
};

}
