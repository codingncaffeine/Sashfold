#include "media/VideoPipeline.h"

#include "media/Vp9.h"
#include "media/Yuv.h"

#include <algorithm>
#include <utility>

namespace sashfold::media {

namespace {

// Pictures made and not yet taken: a frame or two ahead of the tick, and
// few enough that a 4K stream's hold no more than a hundred megabytes.
constexpr std::size_t ready_limit = 3;

}

std::unique_ptr<VideoPipeline> VideoPipeline::open(std::string& error)
{
    std::unique_ptr<Vp9Accelerator> accelerator = Vp9Accelerator::open(error);
    if (!accelerator)
        return nullptr;
    return std::make_unique<VideoPipeline>(std::move(accelerator));
}

VideoPipeline::VideoPipeline(std::unique_ptr<Vp9Accelerator> accelerator)
    : m_accelerator(std::move(accelerator))
    , m_device(m_accelerator->device())
    , m_thread([this] { run(); })
{
}

VideoPipeline::~VideoPipeline()
{
    {
        std::lock_guard const lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

std::int64_t VideoPipeline::now_locked() const
{
    return m_clock ? m_clock->now_ns() : m_now_ns;
}

std::int64_t VideoPipeline::until_locked() const
{
    return m_clock ? m_clock->now_ns() + m_ahead_ns : m_until_ns;
}

void VideoPipeline::note_shown_locked()
{
    auto const now = std::chrono::steady_clock::now();
    if (m_shown_at != std::chrono::steady_clock::time_point {})
        m_longest_wait_ms = std::max(m_longest_wait_ms, std::chrono::duration<double, std::milli>(now - m_shown_at).count());
    m_shown_at = now;
}

void VideoPipeline::push(std::int64_t time_ns, std::vector<std::uint8_t> data)
{
    {
        std::lock_guard const lock(m_mutex);
        m_queue.push_back({ time_ns, std::move(data) });
    }
    m_wake.notify_all();
}

void VideoPipeline::set_clock(std::int64_t now_ns, std::int64_t until_ns)
{
    {
        std::lock_guard const lock(m_mutex);
        if (m_now_ns == now_ns && m_until_ns == until_ns) {
            // Playback stands: the wait for the next picture is not a stutter.
            m_shown_at = {};
            return;
        }
        m_now_ns = now_ns;
        m_until_ns = until_ns;
        // Pictures a later one due by now replaces will never be shown:
        // they give their room to the frames after.
        while (m_ready.size() >= 2 && m_ready[1].time_ns <= now_ns) {
            m_spare.push_back(std::move(m_ready.front().rgba));
            m_ready.pop_front();
        }
    }
    m_wake.notify_all();
}

void VideoPipeline::flush()
{
    std::shared_ptr<std::function<void()> const> waker;
    {
        std::lock_guard const lock(m_mutex);
        m_queue.clear();
        for (VideoPicture& picture : m_ready)
            m_spare.push_back(std::move(picture.rgba));
        m_ready.clear();
        m_generation++;
        m_decoded_to_ns.reset();
        m_shown_at = {};
        waker = m_waker;
    }
    m_wake.notify_all();
    if (waker)
        (*waker)();
}

bool VideoPipeline::drop_from(std::int64_t time_ns)
{
    std::lock_guard const lock(m_mutex);
    if (m_decoded_to_ns && *m_decoded_to_ns >= time_ns)
        return false;
    while (!m_queue.empty() && m_queue.back().time_ns >= time_ns)
        m_queue.pop_back();
    return true;
}

std::optional<VideoPicture> VideoPipeline::take(std::int64_t now_ns)
{
    std::optional<VideoPicture> chosen;
    {
        std::lock_guard const lock(m_mutex);
        while (!m_ready.empty() && m_ready.front().time_ns <= now_ns) {
            if (chosen)
                m_spare.push_back(std::move(chosen->rgba));
            chosen = std::move(m_ready.front());
            m_ready.pop_front();
        }
        if (chosen) {
            m_counts.shown++;
            note_shown_locked();
        }
    }
    if (chosen)
        m_wake.notify_all(); // room to make more
    return chosen;
}

void VideoPipeline::recycle(std::vector<std::uint8_t> rgba)
{
    std::lock_guard const lock(m_mutex);
    if (m_spare.size() <= ready_limit)
        m_spare.push_back(std::move(rgba));
}

void VideoPipeline::follow(std::shared_ptr<PlaybackClock const> clock, std::int64_t ahead_ns)
{
    {
        std::lock_guard const lock(m_mutex);
        m_clock = std::move(clock);
        m_ahead_ns = ahead_ns;
    }
    m_wake.notify_all();
}

bool VideoPipeline::follows() const
{
    std::lock_guard const lock(m_mutex);
    return m_clock != nullptr;
}

void VideoPipeline::clock_changed()
{
    std::shared_ptr<std::function<void()> const> waker;
    {
        std::lock_guard const lock(m_mutex);
        if (m_clock && !m_clock->advancing())
            m_shown_at = {}; // playback stands: the wait for the next picture is not a stutter
        waker = m_waker;
    }
    m_wake.notify_all();
    if (waker)
        (*waker)();
}

std::shared_ptr<ShownPicture const> VideoPipeline::show()
{
    bool took = false;
    std::shared_ptr<ShownPicture> current;
    {
        std::lock_guard const lock(m_mutex);
        std::int64_t const now = now_locked();
        std::optional<VideoPicture> chosen;
        while (!m_ready.empty() && m_ready.front().time_ns <= now) {
            if (chosen)
                m_spare.push_back(std::move(chosen->rgba));
            chosen = std::move(m_ready.front());
            m_ready.pop_front();
        }
        if (chosen) {
            // The picture shown until now gives its buffer back, unless
            // someone is still showing it.
            if (m_shown && m_shown.use_count() == 1 && m_spare.size() <= ready_limit)
                m_spare.push_back(std::move(m_shown->bitmap.writable_pixels()));
            m_shown = std::make_shared<ShownPicture>(
                ShownPicture { chosen->time_ns, ++m_counts.shown, Bitmap(chosen->width, chosen->height, std::move(chosen->rgba)) });
            note_shown_locked();
            took = true;
        }
        current = m_shown;
    }
    if (took)
        m_wake.notify_all(); // room to make more
    return current;
}

std::shared_ptr<ShownPicture const> VideoPipeline::shown() const
{
    std::lock_guard const lock(m_mutex);
    return m_shown;
}

std::optional<std::chrono::nanoseconds> VideoPipeline::next_picture_in() const
{
    std::lock_guard const lock(m_mutex);
    if (!m_clock || m_ready.empty())
        return std::nullopt;
    std::int64_t const now = now_locked();
    std::int64_t const due = m_ready.front().time_ns;
    if (due <= now)
        return std::chrono::nanoseconds(0);
    double const rate = m_clock->rate();
    if (!m_clock->advancing() || !(rate > 0))
        return std::nullopt;
    return std::chrono::nanoseconds(static_cast<std::int64_t>(static_cast<double>(due - now) / rate));
}

void VideoPipeline::set_waker(std::function<void()> waker)
{
    std::lock_guard const lock(m_mutex);
    m_waker = waker ? std::make_shared<std::function<void()> const>(std::move(waker)) : nullptr;
}

VideoPipeline::Counts VideoPipeline::counts() const
{
    std::lock_guard const lock(m_mutex);
    return m_counts;
}

double VideoPipeline::take_longest_wait_ms()
{
    std::lock_guard const lock(m_mutex);
    return std::exchange(m_longest_wait_ms, 0.0);
}

std::string VideoPipeline::standing() const
{
    std::lock_guard const lock(m_mutex);
    auto const seconds = [](std::int64_t ns) { return std::to_string(static_cast<double>(ns) / 1e9); };
    std::string words = "clock " + seconds(now_locked());
    if (m_clock)
        words += " (" + m_clock->told() + ")";
    words += ", shown " + (m_shown ? seconds(m_shown->time_ns) : std::string("nothing"));
    words += ", " + std::to_string(m_ready.size()) + " made";
    if (!m_ready.empty())
        words += " (" + seconds(m_ready.front().time_ns) + " to " + seconds(m_ready.back().time_ns) + ")";
    words += ", " + std::to_string(m_queue.size()) + " coded";
    if (!m_queue.empty())
        words += " (" + seconds(m_queue.front().time_ns) + " to " + seconds(m_queue.back().time_ns) + ")";
    if (m_busy)
        words += ", decoding";
    return words;
}

std::size_t VideoPipeline::queued() const
{
    std::lock_guard const lock(m_mutex);
    return m_queue.size();
}

bool VideoPipeline::caught_up(std::int64_t now_ns) const
{
    std::lock_guard const lock(m_mutex);
    return !m_busy && (m_queue.empty() || m_queue.front().time_ns > now_ns);
}

void VideoPipeline::run()
{
    Vp9HeaderReader reader;
    YuvColour colour; // a key frame's, for the frames after it
    std::uint64_t generation = 0;
    std::unique_lock lock(m_mutex);
    for (;;) {
        for (;;) {
            if (m_stop || m_generation != generation)
                break;
            bool const waiting = !m_queue.empty() && m_ready.size() < ready_limit;
            if (waiting && m_queue.front().time_ns < until_locked())
                break;
            // A frame that waits for its time, under a clock that runs by
            // itself: nobody says when the time has come, so it is looked
            // for again shortly.
            if (waiting && m_clock && m_clock->advancing())
                m_wake.wait_for(lock, std::chrono::milliseconds(4));
            else
                m_wake.wait(lock);
        }
        if (m_stop)
            return;
        if (m_generation != generation) {
            generation = m_generation;
            reader.reset();
            m_accelerator->reset();
            continue;
        }
        Coded const coded = std::move(m_queue.front());
        m_queue.pop_front();
        m_decoded_to_ns = coded.time_ns;
        m_busy = true;
        // A later frame already due would be shown in this one's place.
        bool const wanted = m_queue.empty() || m_queue.front().time_ns > now_locked();
        std::vector<std::uint8_t> buffer;
        if (!m_spare.empty()) {
            buffer = std::move(m_spare.back());
            m_spare.pop_back();
        }
        lock.unlock();

        Counts counts;
        std::optional<Nv12Picture> shown;
        bool ok = true;
        for (auto const frame : split_vp9_superframe(coded.data)) {
            std::optional<Vp9FrameHeader> const header = reader.read(frame);
            if (!header) {
                ok = false;
                break;
            }
            if (header->key_frame)
                colour = vp9_colour(header->color_space, header->color_range, header->height);
            if (header->show_existing_frame) {
                if (wanted)
                    shown = m_accelerator->read_slot(header->frame_to_show);
                else
                    counts.skipped++;
                continue;
            }
            if (!m_accelerator->decode(frame, *header)) {
                ok = false;
                break;
            }
            counts.decoded++;
            if (!header->show_frame)
                continue;
            if (wanted)
                shown = m_accelerator->read_last();
            else
                counts.skipped++;
        }
        if (!ok)
            counts.failed++;
        std::optional<VideoPicture> made;
        if (ok && shown) {
            made.emplace();
            made->time_ns = coded.time_ns;
            made->width = shown->width;
            made->height = shown->height;
            made->rgba = std::move(buffer);
            made->rgba.resize(static_cast<std::size_t>(shown->width) * static_cast<std::size_t>(shown->height) * 4);
            nv12_to_rgba(*shown, colour, made->rgba);
            counts.made++;
        }

        lock.lock();
        m_busy = false;
        m_counts.decoded += counts.decoded;
        m_counts.failed += counts.failed;
        m_counts.made += counts.made;
        m_counts.skipped += counts.skipped;
        if (made && generation == m_generation) {
            m_ready.push_back(std::move(*made));
            if (std::shared_ptr<std::function<void()> const> const waker = m_waker) {
                lock.unlock();
                (*waker)();
                lock.lock();
            }
        } else if (made) {
            m_spare.push_back(std::move(made->rgba));
        } else if (buffer.capacity() > 0) {
            m_spare.push_back(std::move(buffer));
        }
    }
}

}
