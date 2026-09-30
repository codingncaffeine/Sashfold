#pragma once

// A page's video, shown apart from the page's painting — the first of the
// compositor's layers. A page paints on its own thread, which its scripts
// keep for as long as they run: a quarter of a second at a time while a
// heavy page starts, seconds now and then. A video whose pictures waited
// for that thread stuttered exactly then. So the page's painting leaves
// each video's place open (paint::paint_page, Bitmap::punch) and says where
// the places are, and a thread of the compositor's own — which runs no
// script and waits for none — puts each picture there the moment it comes
// due by the video's own clock (media::VideoPipeline::show), with whatever
// the page painted over the video laid back over it. What the window's
// thread gets is the page's picture and, for each video, a finished patch
// to put on it: nothing of the work is the window's, and none of it is the
// page's.

#include "core/Bitmap.h"
#include "media/VideoPipeline.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace sashfold::ui {

// Where a video shows in a page's picture: `rect` is what is seen of it,
// and `drawn` the rectangle its whole frame is scaled into (the two differ
// where something clips the video).
struct VideoLayer {
    Rect rect;
    Rect drawn;
    std::shared_ptr<media::VideoPipeline> video;
};

// A part of a page's picture made apart from it: a video's frame, scaled
// into its place, under what the page painted over it. Opaque.
struct PicturePatch {
    Rect rect;
    std::shared_ptr<Bitmap const> bitmap;
};
using PicturePatches = std::vector<PicturePatch>;

// Makes a patch: into `patch`, which is the size of `rect`, the video's
// `frame` scaled into `drawn` (black where there is no frame yet), and over
// it what `page` — the page's painting, open where the video shows — has
// in `rect`. Both rectangles are in the page's coordinates.
void compose_patch(Bitmap& patch, Bitmap const& page, Rect const& rect, Rect const& drawn, Bitmap const* frame);
// The patches put on a page's picture that was put at (x, y) of `target`:
// what a reader is shown.
void put_patches(Bitmap& target, PicturePatches const& patches, int x, int y);

class PageCompositor {
public:
    // A picture to show, said on the compositor's thread: the page as it was
    // painted and the patches that go on it. `page_is_new` when the page's
    // picture is another than the last said, `whole` when that picture came
    // of a whole painting of the page.
    using Publish = std::function<void(std::shared_ptr<Bitmap const> page, std::shared_ptr<PicturePatches const> patches,
        bool page_is_new, bool whole)>;

    explicit PageCompositor(Publish publish);
    ~PageCompositor();
    PageCompositor(PageCompositor const&) = delete;
    PageCompositor& operator=(PageCompositor const&) = delete;

    // The page as it was just painted, with its videos' places; from the
    // page's thread, which does not wait.
    void submit(std::shared_ptr<Bitmap const> page, std::vector<VideoLayer> layers, bool whole);

    struct Counts {
        std::uint64_t published = 0; // pictures said
        std::uint64_t composed = 0; // patches made
        double compose_ms = 0; // spent making them
        double longest_compose_ms = 0;
    };
    Counts counts() const;

private:
    // What wakes the thread, held in common with the pipelines' wakers: a
    // pipeline may call after the compositor is gone.
    struct Signal {
        std::mutex mutex;
        std::condition_variable wake;
        bool poked = false;
        bool stop = false;
    };
    struct Submitted {
        std::shared_ptr<Bitmap const> page;
        std::vector<VideoLayer> layers;
        bool whole = false;
    };
    struct Shown {
        VideoLayer layer;
        std::uint64_t serial = ~std::uint64_t { 0 };
        std::shared_ptr<Bitmap> patch;
    };

    void run();
    std::shared_ptr<Bitmap> compose(Bitmap const& page, VideoLayer const& layer, media::ShownPicture const* shown);
    std::shared_ptr<Bitmap> buffer(int width, int height);

    Publish m_publish;
    std::shared_ptr<Signal> m_signal = std::make_shared<Signal>();
    std::optional<Submitted> m_submitted; // under the signal's mutex
    std::vector<std::shared_ptr<Bitmap>> m_buffers; // the thread's alone
    mutable std::mutex m_counts_mutex;
    Counts m_counts;
    std::thread m_thread; // last: it starts once everything above is ready
};

}
