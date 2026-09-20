#pragma once

// A picture that moves: an animated GIF or an animated PNG. It is held as
// the file's bytes and one canvas — the picture as it now stands — and a
// frame is decoded when it is due, so seventy frames the size of a window's
// header cost one of them in memory, not seventy. The frames are composed the
// way each format says: a GIF's by their disposal and their transparent
// index, an animated PNG's by its dispose and blend operations.

#include "core/Bitmap.h"
#include "core/Gif.h"
#include "core/Png.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold {

class AnimatedImage {
public:
    // nullopt unless the bytes are a GIF or an animated PNG with more than
    // one frame to show: a picture that does not move is decoded as one.
    static std::optional<AnimatedImage> open(std::vector<std::uint8_t> bytes,
        std::size_t max_pixels = 32u * 1024u * 1024u);

    int width() const { return m_canvas.width(); }
    int height() const { return m_canvas.height(); }
    std::size_t frame_count() const { return m_steps.size(); }
    std::size_t frame_index() const { return m_index; }

    // The picture as it now stands: the first frame after open().
    Bitmap const& canvas() const { return m_canvas; }

    // How long the frame now showing is shown, in milliseconds, as browsers
    // play it: a delay of a hundredth of a second or less is a tenth.
    std::uint32_t delay_ms() const;

    // Composes the next frame — the first again after the last, the canvas
    // cleared between plays. False, the canvas left on its last frame, when
    // a file that plays a number of times has played them, or when a frame's
    // data cannot be read.
    bool advance();
    bool finished() const { return m_finished; }

private:
    enum class Dispose : std::uint8_t { Keep, Background, Previous };
    struct Step {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        std::uint32_t delay_ms = 0;
        Dispose dispose = Dispose::Keep;
    };

    AnimatedImage() = default;
    bool draw(std::size_t index);

    std::vector<std::uint8_t> m_bytes;
    std::optional<GifAnimation> m_gif;
    std::optional<Apng> m_apng;
    std::vector<Step> m_steps;
    std::uint32_t m_loops = 0; // 0 is without end
    std::uint32_t m_played = 0;
    std::size_t m_index = 0;
    bool m_finished = false;
    Bitmap m_canvas { 1, 1, Color::rgba(0, 0, 0, 0) };
    // What the canvas held under the frame now showing, when that frame is
    // to be taken away again by putting it back.
    Bitmap m_before { 1, 1, Color::rgba(0, 0, 0, 0) };
    bool m_has_before = false;
};

}
