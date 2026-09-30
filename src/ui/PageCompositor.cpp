#include "ui/PageCompositor.h"

#include "core/TraceClock.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace sashfold::ui {

namespace {

using steady = std::chrono::steady_clock;

// The compositor's own account on stderr, with the media pipeline's
// (SASHFOLD_MEDIA_TRACE=1): how many pictures it put on the page each
// second, the longest a reader waited for one, and what making them cost.
bool tracing()
{
    static bool const enabled = [] {
        char const* const value = std::getenv("SASHFOLD_MEDIA_TRACE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

}

void compose_patch(Bitmap& patch, Bitmap const& page, Rect const& rect, Rect const& drawn, Bitmap const* frame)
{
    Rect const within { drawn.x - rect.x, drawn.y - rect.y, drawn.width, drawn.height };
    bool const covered = frame != nullptr && within.x <= 0 && within.y <= 0 && within.right() >= rect.width && within.bottom() >= rect.height;
    if (!covered)
        patch.fill_rect(Rect { 0, 0, rect.width, rect.height }, Color::rgb(0, 0, 0)); // no picture yet: black, as a video begins
    if (frame != nullptr)
        patch.draw_scaled_opaque(*frame, within);
    // The page's picture is open where the frame shows, and says by its
    // alpha how much of it, pixel by pixel.
    patch.draw(page, -rect.x, -rect.y);
}

void put_patches(Bitmap& target, PicturePatches const& patches, int x, int y)
{
    for (PicturePatch const& patch : patches) {
        if (patch.bitmap)
            target.blit(*patch.bitmap, x + patch.rect.x, y + patch.rect.y);
    }
}

PageCompositor::PageCompositor(Publish publish)
    : m_publish(std::move(publish))
    , m_thread([this] { run(); })
{
}

PageCompositor::~PageCompositor()
{
    {
        std::lock_guard const lock(m_signal->mutex);
        m_signal->stop = true;
    }
    m_signal->wake.notify_all();
    m_thread.join();
}

void PageCompositor::submit(std::shared_ptr<Bitmap const> page, std::vector<VideoLayer> layers, bool whole)
{
    {
        std::lock_guard const lock(m_signal->mutex);
        // A picture the thread has not got to is replaced by this one; that
        // it was painted whole is still to be said.
        bool const was_whole = m_submitted && m_submitted->whole;
        m_submitted = Submitted { std::move(page), std::move(layers), whole || was_whole };
    }
    m_signal->wake.notify_all();
}

PageCompositor::Counts PageCompositor::counts() const
{
    std::lock_guard const lock(m_counts_mutex);
    return m_counts;
}

// A bitmap of that size that nobody is showing any more, or a new one.
std::shared_ptr<Bitmap> PageCompositor::buffer(int width, int height)
{
    for (std::shared_ptr<Bitmap> const& kept : m_buffers) {
        if (kept.use_count() == 1 && kept->width() == width && kept->height() == height)
            return kept;
    }
    // Those of another size, or more than a few, are let go as they fall
    // out of use.
    std::erase_if(m_buffers, [&](std::shared_ptr<Bitmap> const& kept) {
        return kept.use_count() == 1 && (kept->width() != width || kept->height() != height || m_buffers.size() > 4);
    });
    m_buffers.push_back(std::make_shared<Bitmap>(width, height, Color::rgb(0, 0, 0)));
    return m_buffers.back();
}

std::shared_ptr<Bitmap> PageCompositor::compose(Bitmap const& page, VideoLayer const& layer, media::ShownPicture const* shown)
{
    std::shared_ptr<Bitmap> patch = buffer(layer.rect.width, layer.rect.height);
    compose_patch(*patch, page, layer.rect, layer.drawn, shown != nullptr ? &shown->bitmap : nullptr);
    return patch;
}

void PageCompositor::run()
{
    std::shared_ptr<Bitmap const> page;
    std::vector<Shown> layers;
    std::optional<steady::time_point> deadline;
    // For the trace: the second being counted.
    steady::time_point traced_at = steady::now();
    steady::time_point shown_at {};
    std::uint64_t traced_pictures = 0;
    double traced_longest_wait_ms = 0;
    double traced_compose_ms = 0;
    double traced_longest_compose_ms = 0;
    bool wait_said = false;
    for (;;) {
        bool page_is_new = false;
        bool whole = false;
        std::optional<Submitted> taken;
        {
            std::unique_lock lock(m_signal->mutex);
            auto const something = [&] { return m_signal->stop || m_signal->poked || m_submitted.has_value(); };
            if (deadline)
                m_signal->wake.wait_until(lock, *deadline, something);
            else
                m_signal->wake.wait(lock, something);
            if (m_signal->stop)
                break;
            m_signal->poked = false;
            taken = std::exchange(m_submitted, std::nullopt);
        }
        if (taken) {
            // The pipelines say when they have a picture: those of this
            // picture's videos, and no longer those that have left it.
            for (Shown const& was : layers) {
                bool const stays = std::any_of(taken->layers.begin(), taken->layers.end(),
                    [&](VideoLayer const& layer) { return layer.video == was.layer.video; });
                if (!stays)
                    was.layer.video->set_waker({});
            }
            std::vector<Shown> next;
            next.reserve(taken->layers.size());
            for (VideoLayer& layer : taken->layers) {
                if (!layer.video || layer.rect.is_empty())
                    continue;
                layer.video->set_waker([signal = m_signal] {
                    {
                        std::lock_guard const lock(signal->mutex);
                        signal->poked = true;
                    }
                    signal->wake.notify_all();
                });
                // (The picture it last showed is remembered across the
                // page's paintings: only another one counts as new.)
                std::uint64_t serial = ~std::uint64_t { 0 };
                for (Shown const& was : layers) {
                    if (was.layer.video == layer.video)
                        serial = was.serial;
                }
                next.push_back(Shown { std::move(layer), serial, nullptr });
            }
            if (tracing() && next.size() != layers.size()) {
                std::string line = "media: " + trace_stamp() + "layers: the page's picture has " + std::to_string(next.size()) + " video(s) on it";
                for (Shown const& shown : next) {
                    line += ", one at " + std::to_string(shown.layer.rect.x) + "," + std::to_string(shown.layer.rect.y) + " "
                        + std::to_string(shown.layer.rect.width) + "x" + std::to_string(shown.layer.rect.height);
                }
                std::cerr << line + "\n";
                shown_at = {}; // (the wait across a page shown without its video is not a stutter)
            }
            layers = std::move(next);
            page = std::move(taken->page);
            page_is_new = true;
            whole = taken->whole;
        }
        if (!page)
            continue;

        bool changed = page_is_new;
        std::uint64_t composed = 0;
        double compose_ms = 0;
        bool new_frame = false;
        for (Shown& shown : layers) {
            std::shared_ptr<media::ShownPicture const> const picture = shown.layer.video->show();
            std::uint64_t const serial = picture ? picture->serial : 0;
            if (!page_is_new && shown.patch && serial == shown.serial)
                continue;
            if (serial != shown.serial && picture)
                new_frame = true;
            auto const started = steady::now();
            shown.patch = compose(*page, shown.layer, picture.get());
            shown.serial = serial;
            compose_ms += std::chrono::duration<double, std::milli>(steady::now() - started).count();
            ++composed;
            changed = true;
        }
        if (changed) {
            auto patches = std::make_shared<PicturePatches>();
            patches->reserve(layers.size());
            for (Shown const& shown : layers)
                patches->push_back({ shown.layer.rect, shown.patch });
            m_publish(page, std::move(patches), page_is_new, whole);
            std::lock_guard const lock(m_counts_mutex);
            ++m_counts.published;
            m_counts.composed += composed;
            m_counts.compose_ms += compose_ms;
            m_counts.longest_compose_ms = std::max(m_counts.longest_compose_ms, compose_ms);
        }
        if (tracing()) {
            auto const now = steady::now();
            if (new_frame) {
                if (shown_at != steady::time_point {})
                    traced_longest_wait_ms = std::max(traced_longest_wait_ms, std::chrono::duration<double, std::milli>(now - shown_at).count());
                shown_at = now;
                ++traced_pictures;
            }
            traced_compose_ms += compose_ms;
            traced_longest_compose_ms = std::max(traced_longest_compose_ms, compose_ms);
            // A wait a reader would see, said while it lasts and once: where
            // each video's pipeline stands is why.
            if (!new_frame && !wait_said && shown_at != steady::time_point {} && now - shown_at >= std::chrono::milliseconds(100)) {
                wait_said = true;
                for (Shown const& shown : layers)
                    std::cerr << "media: " + trace_stamp() + "layers: no picture for 100 ms \xe2\x80\x94 " + shown.layer.video->standing() + "\n";
            }
            if (new_frame)
                wait_said = false;
            if (now - traced_at >= std::chrono::seconds(1)) {
                if (traced_pictures > 0) {
                    std::ostringstream line;
                    line << "media: " << trace_stamp() << "layers: " << traced_pictures << " pictures put on the page in "
                         << std::fixed << std::setprecision(2) << std::chrono::duration<double>(now - traced_at).count()
                         << " s, longest wait " << std::setprecision(0) << traced_longest_wait_ms << " ms, composing "
                         << std::setprecision(1) << traced_compose_ms / static_cast<double>(traced_pictures) << " ms each (longest "
                         << traced_longest_compose_ms << " ms)\n";
                    std::cerr << line.str();
                } else {
                    shown_at = {}; // nothing moved for a second: the wait that ends it is not a stutter
                }
                traced_at = now;
                traced_pictures = 0;
                traced_longest_wait_ms = 0;
                traced_compose_ms = 0;
                traced_longest_compose_ms = 0;
            }
        }

        // Woken again when the next picture already made comes due; one
        // not yet made, or a clock that stands, wakes it by the pipeline.
        deadline.reset();
        for (Shown const& shown : layers) {
            if (std::optional<std::chrono::nanoseconds> const wait = shown.layer.video->next_picture_in()) {
                steady::time_point const at = steady::now() + *wait;
                if (!deadline || at < *deadline)
                    deadline = at;
            }
        }
        // (Under the trace it looks again shortly whatever happens, so that
        // a wait is seen while it lasts.)
        if (tracing() && !layers.empty()) {
            steady::time_point const at = steady::now() + std::chrono::milliseconds(50);
            if (!deadline || at < *deadline)
                deadline = at;
        }
    }
    for (Shown const& shown : layers)
        shown.layer.video->set_waker({});
}

}
