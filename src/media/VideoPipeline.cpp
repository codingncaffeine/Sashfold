#include "media/VideoPipeline.h"

#include "media/Vp9.h"
#include "media/Yuv.h"

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
        if (m_now_ns == now_ns && m_until_ns == until_ns)
            return;
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
    {
        std::lock_guard const lock(m_mutex);
        m_queue.clear();
        for (VideoPicture& picture : m_ready)
            m_spare.push_back(std::move(picture.rgba));
        m_ready.clear();
        m_generation++;
    }
    m_wake.notify_all();
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

VideoPipeline::Counts VideoPipeline::counts() const
{
    std::lock_guard const lock(m_mutex);
    return m_counts;
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
        m_wake.wait(lock, [&] {
            return m_stop || m_generation != generation
                || (!m_queue.empty() && m_queue.front().time_ns < m_until_ns && m_ready.size() < ready_limit);
        });
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
        m_busy = true;
        // A later frame already due would be shown in this one's place.
        bool const wanted = m_queue.empty() || m_queue.front().time_ns > m_now_ns;
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
        if (made && generation == m_generation)
            m_ready.push_back(std::move(*made));
        else if (made)
            m_spare.push_back(std::move(made->rgba));
        else if (buffer.capacity() > 0)
            m_spare.push_back(std::move(buffer));
    }
}

}
