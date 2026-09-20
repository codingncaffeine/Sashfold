#include "Test.h"

#include "core/AnimatedImage.h"
#include "core/Bitmap.h"
#include "core/Gif.h"
#include "core/Png.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

// Pictures that move, held to what other decoders make of the same files.
// The fixtures were drawn for this test — a block crossing a field, a test
// pattern — and written by ImageMagick (the GIF: partial frames, each put
// back as it was before the next, one with no delay written) and by ffmpeg
// (the animated PNGs: truecolor and palette). Beside each is every frame as
// the other decoder composes it (`magick -coalesce`, `ffmpeg -i`), read here
// as still PNGs: the canvas must equal them pixel for pixel, frame after
// frame and again after the last.

using namespace sashfold;

namespace {

std::vector<std::uint8_t> read_file(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// How many pixels differ; -1 when the sizes do.
long differing(Bitmap const& a, Bitmap const& b)
{
    if (a.width() != b.width() || a.height() != b.height())
        return -1;
    long count = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            Color const p = a.pixel(x, y);
            Color const q = b.pixel(x, y);
            // A pixel that shows nothing has no color to compare.
            if (p.a == 0 && q.a == 0)
                continue;
            if (p.r != q.r || p.g != q.g || p.b != q.b || p.a != q.a)
                ++count;
        }
    }
    return count;
}

void plays_as(std::string const& folder, std::string const& file, std::string const& frames_prefix, int first_number,
    std::size_t frames)
{
    std::optional<AnimatedImage> image = AnimatedImage::open(read_file(folder + "/" + file));
    CHECK(image.has_value());
    if (!image)
        return;
    CHECK_EQ(image->frame_count(), frames);
    // Twice round: the second time is the loop, from a cleared canvas.
    for (std::size_t step = 0; step < frames * 2; ++step) {
        std::size_t const index = step % frames;
        CHECK_EQ(image->frame_index(), index);
        std::optional<Bitmap> const expected = decode_png(
            read_file(folder + "/" + frames_prefix + std::to_string(first_number + static_cast<int>(index)) + ".png"));
        CHECK(expected.has_value());
        if (expected)
            CHECK_EQ(differing(image->canvas(), *expected), 0L);
        CHECK(image->advance());
    }
}

}

int main(int argc, char** argv)
{
    std::string const folder = argc > 1 ? argv[1] : "tests/fixtures/animated";

    // The GIF: four frames, three of them small and each taken away by
    // putting back what was under it.
    plays_as(folder, "moving.gif", "gif-frame-", 0, 4);
    {
        std::vector<std::uint8_t> const bytes = read_file(folder + "/moving.gif");
        std::optional<GifAnimation> const scanned = scan_gif(bytes);
        CHECK(scanned.has_value());
        if (scanned) {
            CHECK_EQ(scanned->frames.size(), std::size_t { 4 });
            CHECK_EQ(scanned->loops, 0u); // without end
            CHECK_EQ(scanned->frames[1].width, 5);
            CHECK_EQ(scanned->frames[1].disposal, 3);
            CHECK_EQ(scanned->frames[0].delay_ms, 70u);
            CHECK_EQ(scanned->frames[1].delay_ms, 0u);
        }
        // A delay of nothing is played as a tenth of a second.
        std::optional<AnimatedImage> image = AnimatedImage::open(bytes);
        CHECK(image.has_value());
        if (image) {
            CHECK_EQ(image->delay_ms(), 70u);
            CHECK(image->advance());
            CHECK_EQ(image->delay_ms(), 100u);
        }
        // The still decoder's picture is the first frame.
        std::optional<Bitmap> const still = decode_gif(bytes);
        std::optional<Bitmap> const first = decode_png(read_file(folder + "/gif-frame-0.png"));
        CHECK(still.has_value() && first.has_value());
        if (still && first)
            CHECK_EQ(differing(*still, *first), 0L);
    }

    // The animated PNGs, truecolor and palette.
    plays_as(folder, "moving.png", "apng-moving-", 1, 4);
    plays_as(folder, "palette.png", "apng-palette-", 1, 4);
    {
        std::vector<std::uint8_t> const bytes = read_file(folder + "/moving.png");
        std::optional<Apng> const scanned = scan_apng(bytes);
        CHECK(scanned.has_value());
        if (scanned) {
            CHECK_EQ(scanned->frames.size(), std::size_t { 4 });
            CHECK_EQ(scanned->loops, 0u);
            CHECK_EQ(scanned->frames[0].delay_ms, 200u); // five frames a second
        }
        // A still PNG is not an animation, and neither is one frame of one.
        CHECK(!AnimatedImage::open(read_file(folder + "/gif-frame-0.png")).has_value());
        CHECK(!scan_apng(read_file(folder + "/gif-frame-0.png")).has_value());
        // Cut short inside a later frame, the frames that came whole remain.
        std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() * 2 / 3));
        std::optional<Apng> const partial = scan_apng(cut);
        CHECK(!partial.has_value() || partial->frames.size() < 4);
        // Over the pixel budget.
        CHECK(!AnimatedImage::open(bytes, 100).has_value());
    }

    // A file that plays a number of times stops on its last frame.
    {
        std::vector<std::uint8_t> bytes = read_file(folder + "/moving.gif");
        // NETSCAPE2.0's count, two bytes after its sub-block's leading 1: once more.
        std::string const name = "NETSCAPE2.0";
        for (std::size_t i = 0; i + name.size() + 4 < bytes.size(); ++i) {
            if (std::string(bytes.begin() + static_cast<std::ptrdiff_t>(i), bytes.begin() + static_cast<std::ptrdiff_t>(i + name.size())) == name) {
                bytes[i + name.size() + 2] = 1;
                bytes[i + name.size() + 3] = 0;
                break;
            }
        }
        std::optional<AnimatedImage> image = AnimatedImage::open(bytes);
        CHECK(image.has_value());
        if (image) {
            int advanced = 0;
            while (image->advance() && advanced < 100)
                ++advanced;
            CHECK_EQ(advanced, 7); // two plays of four frames: seven steps on from the first
            CHECK(image->finished());
            CHECK_EQ(image->frame_index(), std::size_t { 3 });
        }
    }

    return test::report("animated image");
}
