#include "core/AnimatedImage.h"

#include <algorithm>
#include <utility>

namespace sashfold {

namespace {

// One straight-alpha pixel laid over another.
Color over(Color top, Color under)
{
    if (top.a == 255 || under.a == 0)
        return top;
    if (top.a == 0)
        return under;
    unsigned const ta = top.a;
    unsigned const ua = under.a * (255u - ta) / 255u;
    unsigned const alpha = ta + ua;
    auto const mix = [&](std::uint8_t t, std::uint8_t u) {
        return static_cast<std::uint8_t>((t * ta + u * ua + alpha / 2) / alpha);
    };
    return Color::rgba(mix(top.r, under.r), mix(top.g, under.g), mix(top.b, under.b), static_cast<std::uint8_t>(alpha));
}

}

std::optional<AnimatedImage> AnimatedImage::open(std::vector<std::uint8_t> bytes, std::size_t max_pixels)
{
    AnimatedImage image;
    if (looks_like_gif(bytes)) {
        image.m_gif = scan_gif(bytes, max_pixels);
        if (!image.m_gif || image.m_gif->frames.size() < 2)
            return std::nullopt;
        image.m_loops = image.m_gif->loops;
        for (GifFrame const& frame : image.m_gif->frames) {
            Step step { frame.left, frame.top, frame.width, frame.height, frame.delay_ms, Dispose::Keep };
            if (frame.disposal == 2)
                step.dispose = Dispose::Background;
            else if (frame.disposal == 3)
                step.dispose = Dispose::Previous;
            image.m_steps.push_back(step);
        }
        image.m_canvas = Bitmap(image.m_gif->width, image.m_gif->height, Color::rgba(0, 0, 0, 0));
    } else if (looks_like_png(bytes)) {
        image.m_apng = scan_apng(bytes, max_pixels);
        if (!image.m_apng || image.m_apng->frames.size() < 2)
            return std::nullopt;
        image.m_loops = image.m_apng->loops;
        for (ApngFrame const& frame : image.m_apng->frames) {
            Step step { frame.x, frame.y, frame.width, frame.height, frame.delay_ms, Dispose::Keep };
            if (frame.dispose == 1)
                step.dispose = Dispose::Background;
            else if (frame.dispose == 2)
                step.dispose = Dispose::Previous;
            image.m_steps.push_back(step);
        }
        // The first frame has nothing before it to be put back: taking it
        // away clears its area (APNG 1.0).
        if (image.m_steps.front().dispose == Dispose::Previous)
            image.m_steps.front().dispose = Dispose::Background;
        image.m_canvas = Bitmap(image.m_apng->width, image.m_apng->height, Color::rgba(0, 0, 0, 0));
    } else {
        return std::nullopt;
    }
    image.m_bytes = std::move(bytes);
    if (!image.draw(0))
        return std::nullopt;
    return image;
}

std::uint32_t AnimatedImage::delay_ms() const
{
    std::uint32_t const written = m_steps[m_index].delay_ms;
    return written <= 10 ? 100 : written;
}

bool AnimatedImage::draw(std::size_t index)
{
    Step const& step = m_steps[index];
    m_has_before = step.dispose == Dispose::Previous;
    if (m_has_before)
        m_before = m_canvas;
    if (m_gif) {
        if (!draw_gif_frame(m_bytes, *m_gif, index, m_canvas))
            return false;
    } else {
        std::optional<Bitmap> const frame = decode_apng_frame(m_bytes, *m_apng, index);
        if (!frame)
            return false;
        bool const lay_over = m_apng->frames[index].blend == 1;
        for (int y = 0; y < frame->height(); ++y) {
            for (int x = 0; x < frame->width(); ++x) {
                int const cx = step.x + x;
                int const cy = step.y + y;
                if (!m_canvas.contains(cx, cy))
                    continue;
                Color const pixel = frame->pixel(x, y);
                m_canvas.set_pixel(cx, cy, lay_over ? over(pixel, m_canvas.pixel(cx, cy)) : pixel);
            }
        }
    }
    m_index = index;
    return true;
}

bool AnimatedImage::advance()
{
    if (m_finished)
        return false;
    bool const last = m_index + 1 >= m_steps.size();
    if (last && m_loops != 0 && m_played + 1 >= m_loops) {
        m_finished = true; // played out: the last frame stays
        return false;
    }
    // A copy, to fall back on should the next frame's data be unreadable.
    Bitmap const as_it_stood = m_canvas;
    std::size_t const was = m_index;
    if (last) {
        ++m_played;
        m_canvas = Bitmap(m_canvas.width(), m_canvas.height(), Color::rgba(0, 0, 0, 0));
    } else {
        Step const& showing = m_steps[m_index];
        if (showing.dispose == Dispose::Background) {
            for (int y = showing.y; y < showing.y + showing.height; ++y) {
                for (int x = showing.x; x < showing.x + showing.width; ++x) {
                    if (m_canvas.contains(x, y))
                        m_canvas.set_pixel(x, y, Color::rgba(0, 0, 0, 0));
                }
            }
        } else if (showing.dispose == Dispose::Previous && m_has_before) {
            m_canvas = m_before;
        }
    }
    if (!draw(last ? 0 : m_index + 1)) {
        m_canvas = as_it_stood;
        m_index = was;
        m_finished = true;
        return false;
    }
    return true;
}

}
