#include "Test.h"

#include "media/VideoPipeline.h"
#include "media/Vp9.h"
#include "media/Vp9Accelerator.h"
#include "media/WebM.h"
#include "media/Yuv.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// A video's pictures on their way to the painter: the colour conversion
// against the standards' own numbers, and the pipeline that makes pictures
// off the page's thread — which picture it hands out for a moment of
// playback, and that it is the one the decoder made for that frame.

using namespace sashfold;
using media::Nv12Picture;
using media::YuvColour;
using media::YuvMatrix;

namespace {

using Bytes = std::vector<std::uint8_t>;

// A picture of one colour, coded as given.
Nv12Picture flat(int width, int height, std::uint8_t y, std::uint8_t cb, std::uint8_t cr)
{
    Nv12Picture picture;
    picture.width = width;
    picture.height = height;
    picture.luma.assign(static_cast<std::size_t>(width * height), y);
    for (int i = 0; i < picture.chroma_width() * picture.chroma_height(); ++i) {
        picture.chroma.push_back(cb);
        picture.chroma.push_back(cr);
    }
    return picture;
}

std::array<int, 3> rgb_of(Nv12Picture const& picture, YuvColour colour)
{
    Bytes rgba(static_cast<std::size_t>(picture.width * picture.height * 4));
    media::nv12_to_rgba(picture, colour, rgba);
    return { rgba[0], rgba[1], rgba[2] };
}

bool near(std::array<int, 3> got, std::array<int, 3> want, int within)
{
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::abs(got[i] - want[i]) > within)
            return false;
    }
    return true;
}

// A planar picture (Nv12Picture::Planar) of one colour at a depth and chroma format.
Nv12Picture flat_planar(int width, int height, int depth, int format, int y, int cb, int cr)
{
    Nv12Picture picture;
    picture.width = width;
    picture.height = height;
    Nv12Picture::Planar p;
    p.bit_depth_luma = p.bit_depth_chroma = depth;
    p.chroma_format = format;
    p.chroma_width = format == 0 ? 0 : (format == 3 ? width : (width + 1) / 2);
    p.chroma_height = format == 0 ? 0 : (format == 1 ? (height + 1) / 2 : height);
    p.y.assign(static_cast<std::size_t>(width * height), static_cast<std::uint16_t>(y));
    p.cb.assign(static_cast<std::size_t>(p.chroma_width * p.chroma_height), static_cast<std::uint16_t>(cb));
    p.cr.assign(static_cast<std::size_t>(p.chroma_width * p.chroma_height), static_cast<std::uint16_t>(cr));
    picture.planar = std::move(p);
    return picture;
}

// Pictures beyond 8-bit 4:2:0: deeper samples (dithered to the 8-bit
// surface), 4:2:2, 4:4:4, monochrome and the identity matrix (G, B, R).
void test_planar_conversion()
{
    YuvColour const bt709 { YuvMatrix::Bt709, false };
    // 10 bits: studio black at 64, white at 940; BT.709's red at 252/408/960.
    CHECK(near(rgb_of(flat_planar(2, 2, 10, 1, 64, 512, 512), bt709), { 0, 0, 0 }, 0));
    CHECK(near(rgb_of(flat_planar(2, 2, 10, 1, 940, 512, 512), bt709), { 255, 255, 255 }, 0));
    CHECK(near(rgb_of(flat_planar(2, 2, 10, 2, 252, 408, 960), bt709), { 255, 0, 0 }, 2));
    CHECK(near(rgb_of(flat_planar(2, 2, 12, 3, 1008, 1632, 3840), bt709), { 255, 0, 0 }, 2));
    // At 8 bits a planar 4:2:0 picture is the NV12 one, pixel for pixel.
    Nv12Picture const nv12 = flat(4, 4, 81, 90, 240);
    Nv12Picture const planar = flat_planar(4, 4, 8, 1, 81, 90, 240);
    Bytes a(4 * 4 * 4), b(4 * 4 * 4);
    media::nv12_to_rgba(nv12, bt709, a);
    media::nv12_to_rgba(planar, bt709, b);
    CHECK(a == b);
    // A level between two 8-bit ones (10-bit grey half way between 8-bit
    // 125 and 126, full range) is dithered: an 8x8 block averages to it.
    YuvColour const full { YuvMatrix::Bt709, true };
    Nv12Picture const between = flat_planar(8, 8, 10, 3, 502, 512, 512);
    Bytes grey(8 * 8 * 4);
    media::nv12_to_rgba(between, full, grey);
    int low = 0, high = 0;
    for (std::size_t i = 0; i < grey.size(); i += 4) {
        low += grey[i] == 125 ? 1 : 0;
        high += grey[i] == 126 ? 1 : 0;
    }
    CHECK_EQ(low, 32);
    CHECK_EQ(high, 32);
    // 4:2:2: a colour difference covers two pixels of one row, not the row under it.
    Nv12Picture tall = flat_planar(2, 2, 8, 2, 126, 128, 128);
    tall.planar->cr[1] = 240; // the second row's
    Bytes rgba(2 * 2 * 4);
    media::nv12_to_rgba(tall, YuvColour { YuvMatrix::Bt601, false }, rgba);
    CHECK(rgba[0] < 140 && rgba[4] < 140 && rgba[8] > 200 && rgba[12] > 200);
    // Monochrome: grey whatever the matrix.
    CHECK(near(rgb_of(flat_planar(2, 2, 8, 0, 126, 0, 0), bt709), { 128, 128, 128 }, 1));
    // The identity matrix (4:4:4 RGB): G, B, R planes, studio range.
    CHECK(near(rgb_of(flat_planar(2, 2, 8, 3, 16, 235, 16), YuvColour { YuvMatrix::Identity, false }), { 0, 0, 255 }, 0));
    CHECK(near(rgb_of(flat_planar(2, 2, 10, 3, 940, 64, 940), YuvColour { YuvMatrix::Identity, false }), { 255, 255, 0 }, 0));
    // A large one split among the converter's threads comes out as one thread makes it.
    Nv12Picture large = flat_planar(1920, 1080, 10, 2, 0, 0, 0);
    for (std::size_t i = 0; i < large.planar->y.size(); ++i)
        large.planar->y[i] = static_cast<std::uint16_t>(64 + (i * 7) % 877);
    for (std::size_t i = 0; i < large.planar->cb.size(); ++i) {
        large.planar->cb[i] = static_cast<std::uint16_t>(64 + (i * 13) % 897);
        large.planar->cr[i] = static_cast<std::uint16_t>(64 + (i * 5) % 897);
    }
    Bytes one(1920 * 1080 * 4), split(1920 * 1080 * 4);
    media::nv12_to_rgba(large, bt709, one);
    media::Nv12Converter converter(4);
    converter.convert(large, bt709, split);
    CHECK(one == split);
    CHECK(media::h264_colour(true, 0, false, 1080).matrix == YuvMatrix::Identity);
    // The range is the video signal type's, colour description or not.
    CHECK(media::h264_colour(false, 2, true, 1080).full_range);
}

void test_colour_conversion()
{
    YuvColour const bt709 { YuvMatrix::Bt709, false };
    YuvColour const bt601 { YuvMatrix::Bt601, false };
    // Studio range: black at 16, white at 235, the differences centred on 128.
    CHECK(near(rgb_of(flat(2, 2, 16, 128, 128), bt709), { 0, 0, 0 }, 0));
    CHECK(near(rgb_of(flat(2, 2, 235, 128, 128), bt709), { 255, 255, 255 }, 0));
    CHECK(near(rgb_of(flat(2, 2, 126, 128, 128), bt601), { 128, 128, 128 }, 1));
    // Red, as each matrix codes it (Y = 16 + 219 Kr, Cb = 128 - 112 Kr / (1 - Kb),
    // Cr = 240): 63/102/240 in BT.709, 81/90/240 in BT.601. Read with the
    // other matrix, it is not red.
    CHECK(near(rgb_of(flat(2, 2, 63, 102, 240), bt709), { 255, 0, 0 }, 2));
    CHECK(near(rgb_of(flat(2, 2, 81, 90, 240), bt601), { 255, 0, 0 }, 2));
    CHECK(!near(rgb_of(flat(2, 2, 81, 90, 240), bt709), { 255, 0, 0 }, 10));
    // Full range: black at 0, white at 255, and BT.709's red at 54/99/255.
    YuvColour const full { YuvMatrix::Bt709, true };
    CHECK(near(rgb_of(flat(2, 2, 0, 128, 128), full), { 0, 0, 0 }, 0));
    CHECK(near(rgb_of(flat(2, 2, 255, 128, 128), full), { 255, 255, 255 }, 0));
    CHECK(near(rgb_of(flat(2, 2, 54, 99, 255), full), { 255, 0, 0 }, 3));
    // Below black and above white is clamped, not wrapped.
    CHECK(near(rgb_of(flat(2, 2, 0, 128, 128), bt709), { 0, 0, 0 }, 0));
    CHECK(near(rgb_of(flat(2, 2, 255, 128, 128), bt709), { 255, 255, 255 }, 0));

    // Each colour difference covers the two by two pixels it was taken
    // from, including the odd last column and row of an odd size.
    Nv12Picture odd = flat(3, 3, 126, 128, 128);
    std::size_t const last = odd.chroma.size() - 2;
    odd.chroma[last + 1] = 240; // the bottom right sample: towards red
    Bytes rgba(3 * 3 * 4);
    media::nv12_to_rgba(odd, bt601, rgba);
    auto const pixel = [&rgba](int x, int y) {
        std::size_t const at = static_cast<std::size_t>((y * 3 + x) * 4);
        return std::array<int, 3> { rgba[at], rgba[at + 1], rgba[at + 2] };
    };
    CHECK(near(pixel(0, 0), { 128, 128, 128 }, 1));
    CHECK(pixel(2, 2)[0] > 200 && pixel(2, 2)[1] < 100);
    CHECK(near(pixel(1, 1), { 128, 128, 128 }, 1));
    CHECK_EQ(static_cast<int>(rgba[3]), 255);

    // What VP9's header says, as a matrix and a range; a stream that does
    // not say is BT.601 at standard definition and BT.709 above it.
    CHECK(media::vp9_colour(2, false, 144).matrix == YuvMatrix::Bt709);
    CHECK(media::vp9_colour(1, false, 1080).matrix == YuvMatrix::Bt601);
    CHECK(media::vp9_colour(3, true, 480).matrix == YuvMatrix::Bt601 && media::vp9_colour(3, true, 480).full_range);
    CHECK(media::vp9_colour(5, false, 2160).matrix == YuvMatrix::Bt2020);
    CHECK(media::vp9_colour(0, false, 576).matrix == YuvMatrix::Bt601);
    CHECK(media::vp9_colour(0, false, 720).matrix == YuvMatrix::Bt709);
}

// Waits, a little at a time, for the pipeline to finish what is due.
bool settle(media::VideoPipeline const& pipeline, std::int64_t now_ns)
{
    for (int i = 0; i < 1500; ++i) {
        if (pipeline.caught_up(now_ns))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// The pipeline on the machine's decoder, where there is one, over the
// 144p stream: each picture it hands out is the one due at the moment
// asked, and is what the decoder makes of that frame, converted.
void test_pipeline()
{
    std::string why;
    std::unique_ptr<media::Vp9Accelerator> reference = media::Vp9Accelerator::open(why);
    if (!reference) {
        std::cerr << "test_video: no VP9 hardware here (" << why << "); the pipeline is not tested\n";
        char const* const expected = std::getenv("SASHFOLD_EXPECT_VIDEO_HARDWARE");
        CHECK(expected == nullptr || *expected == '\0');
        return;
    }
    auto const fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "media" / "vp9-144p.webm";
    std::ifstream in(fixture, std::ios::binary);
    Bytes const stream { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    media::WebmParser parser;
    CHECK(parser.append(stream));
    std::vector<media::WebmFrame> const blocks = parser.take_frames();
    CHECK_EQ(blocks.size(), 30u);

    // What each block shows, decoded directly and converted: the pictures
    // the pipeline must hand out.
    std::vector<Bytes> expected;
    {
        media::Vp9HeaderReader reader;
        YuvColour colour;
        for (media::WebmFrame const& block : blocks) {
            std::optional<Nv12Picture> shown;
            for (auto const frame : media::split_vp9_superframe(block.data)) {
                std::optional<media::Vp9FrameHeader> const header = reader.read(frame);
                if (!header)
                    continue;
                if (header->key_frame)
                    colour = media::vp9_colour(header->color_space, header->color_range, header->height);
                CHECK(reference->decode(frame, *header));
                if (header->show_frame)
                    shown = reference->read_last();
            }
            Bytes rgba;
            if (shown) {
                rgba.resize(static_cast<std::size_t>(shown->width * shown->height * 4));
                media::nv12_to_rgba(*shown, colour, rgba);
            }
            expected.push_back(std::move(rgba));
        }
    }

    std::unique_ptr<media::VideoPipeline> pipeline = media::VideoPipeline::open("V_VP9", {}, why);
    CHECK(pipeline != nullptr);
    if (!pipeline)
        return;
    for (media::WebmFrame const& block : blocks)
        pipeline->push(block.time_ns, block.data);
    auto const index_of = [&blocks](std::int64_t time_ns) {
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            if (blocks[i].time_ns == time_ns)
                return static_cast<int>(i);
        }
        return -1;
    };

    // At the start: the first frame.
    pipeline->set_clock(0, 250'000'000);
    CHECK(settle(*pipeline, 0));
    std::optional<media::VideoPicture> picture = pipeline->take(0);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(index_of(picture->time_ns), 0);
        CHECK(picture->width == 256 && picture->height == 144);
        CHECK(picture->rgba == expected[0]);
        pipeline->recycle(std::move(picture->rgba));
    }
    // Nothing newer is due yet, so nothing is handed out.
    CHECK(!pipeline->take(0).has_value());

    // Half a second on: the frame due then (the 16th, at 500 ms), with the
    // ones before it decoded — the stream needs them — but not converted.
    std::int64_t const half = blocks[15].time_ns + 1'000'000;
    pipeline->set_clock(half, half + 250'000'000);
    CHECK(settle(*pipeline, half));
    picture = pipeline->take(half);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(index_of(picture->time_ns), 15);
        CHECK(picture->rgba == expected[15]);
    }
    media::VideoPipeline::Counts counts = pipeline->counts();
    CHECK(counts.skipped >= 10);
    CHECK_EQ(counts.failed, 0u);

    // A seek back: after a flush the frames come again from a key frame,
    // and the picture is the one due at the new moment.
    pipeline->flush();
    for (media::WebmFrame const& block : blocks)
        pipeline->push(block.time_ns, block.data);
    std::int64_t const early = blocks[4].time_ns + 1'000'000;
    pipeline->set_clock(early, early + 250'000'000);
    CHECK(settle(*pipeline, early));
    picture = pipeline->take(early);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(index_of(picture->time_ns), 4);
        CHECK(picture->rgba == expected[4]);
    }
    CHECK_EQ(pipeline->counts().failed, 0u);
}

}

int main()
{
    test_planar_conversion();
    test_colour_conversion();
    test_pipeline();
    return test::report("test_video");
}
