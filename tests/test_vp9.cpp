#include "Test.h"

#include "media/Vp9.h"
#include "media/WebM.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

// VP9 before the arithmetic-coded part of a frame: the quantizer tables
// against the reference's own sums, superframes split by their index, and
// the uncompressed headers of a real stream (ffmpeg's test source, 64x48
// at ten frames a second, a key frame every five) read frame by frame with
// what they carry from one to the next.

using namespace sashfold;
using media::Vp9FrameHeader;

namespace {

using Bytes = std::vector<std::uint8_t>;

void test_quantizer_tables()
{
    // The sums of the reference decoder's tables, plain and weighted by
    // position, so a value typed wrong or out of place shows.
    long dc = 0, dc_weighted = 0, ac = 0, ac_weighted = 0;
    for (int i = 0; i < 256; ++i) {
        dc += media::vp9_dc_quant(i);
        dc_weighted += static_cast<long>(media::vp9_dc_quant(i)) * (i + 1);
        ac += media::vp9_ac_quant(i);
        ac_weighted += static_cast<long>(media::vp9_ac_quant(i)) * (i + 1);
    }
    CHECK_EQ(dc, 63571);
    CHECK_EQ(dc_weighted, 12574155);
    CHECK_EQ(ac, 98156);
    CHECK_EQ(ac_weighted, 19947543);
    CHECK_EQ(media::vp9_dc_quant(-5), 4);
    CHECK_EQ(media::vp9_ac_quant(300), 1828);
}

void test_superframes()
{
    // Two frames of 3 and 2 bytes, then an index: the marker (two frames,
    // sizes in one byte), the sizes, the marker again.
    Bytes const block = { 0xAA, 0xAA, 0xAA, 0xBB, 0xBB, 0xC1, 3, 2, 0xC1 };
    auto frames = media::split_vp9_superframe(block);
    CHECK_EQ(frames.size(), 2u);
    CHECK(frames.size() == 2 && frames[0].size() == 3 && frames[1].size() == 2 && frames[1][0] == 0xBB);
    // A last byte that looks like a marker without its twin is one frame.
    Bytes const lone = { 0x82, 0x49, 0x83, 0x42, 0x00, 0xC1 };
    CHECK_EQ(media::split_vp9_superframe(lone).size(), 1u);
    // Sizes that run past the frames split nothing.
    Bytes const liar = { 0xAA, 0xC1, 9, 9, 0xC1 };
    CHECK(media::split_vp9_superframe(liar).empty());
}

void test_stream_headers()
{
    std::filesystem::path const fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "media" / "tiny-vp9.webm";
    std::ifstream in(fixture, std::ios::binary);
    Bytes const stream { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    media::WebmParser parser;
    CHECK(parser.append(stream));
    std::vector<media::WebmFrame> const blocks = parser.take_frames();
    CHECK_EQ(blocks.size(), 10u);

    media::Vp9HeaderReader reader;
    int shown = 0;
    int hidden = 0;
    int key_frames = 0;
    bool all_read = true;
    for (media::WebmFrame const& block : blocks) {
        int shown_here = 0;
        for (auto const frame : media::split_vp9_superframe(block.data)) {
            std::optional<Vp9FrameHeader> const header = reader.read(frame);
            if (!header) {
                all_read = false;
                continue;
            }
            if (header->key_frame) {
                key_frames++;
                CHECK_EQ(header->width, 64);
                CHECK_EQ(header->height, 48);
                CHECK_EQ(header->profile, 0);
                CHECK_EQ(header->bit_depth, 8);
                CHECK(header->subsampling_x && header->subsampling_y);
                CHECK_EQ(static_cast<int>(header->refresh_frame_flags), 0xFF);
            }
            if (!header->show_existing_frame) {
                CHECK(header->uncompressed_header_size > 0 && header->compressed_header_size > 0);
                CHECK(header->uncompressed_header_size + header->compressed_header_size < static_cast<int>(frame.size()));
                CHECK(header->base_q_idx > 0 && header->base_q_idx < 256);
            }
            if (header->show_frame)
                shown_here++;
            else
                hidden++;
        }
        // Every block of the container shows exactly one picture.
        CHECK_EQ(shown_here, 1);
        shown += shown_here;
    }
    CHECK(all_read);
    CHECK_EQ(shown, 10);
    CHECK_EQ(key_frames, 2);
    (void)hidden;

    // A frame that does not begin as VP9 frames do, or a key frame without
    // its sync code, is refused.
    media::Vp9HeaderReader fresh;
    Bytes broken = blocks[0].data;
    broken[1] ^= 0xFF; // the sync code follows the first byte of a key frame
    CHECK(!fresh.read(broken).has_value());
    CHECK(!fresh.read(Bytes { 0x00, 0x00 }).has_value());
    // An inter frame before any key frame has nothing to refer to.
    CHECK(!fresh.read(blocks[1].data).has_value());
}

void test_filter_levels()
{
    Vp9FrameHeader header;
    header.loop_filter.level = 40; // at 32 and above the deltas count double
    header.loop_filter.delta_enabled = true;
    header.loop_filter.ref_deltas = { 1, 0, -1, -1 };
    header.loop_filter.mode_deltas = { 0, 2 };
    auto levels = media::vp9_segment_filter_levels(header, 0);
    CHECK_EQ(static_cast<int>(levels[0][0]), 42);
    CHECK_EQ(static_cast<int>(levels[1][0]), 40);
    CHECK_EQ(static_cast<int>(levels[1][1]), 44);
    CHECK_EQ(static_cast<int>(levels[3][0]), 38);
    // A segment with a level of its own, absolute.
    header.segmentation.enabled = true;
    header.segmentation.abs_or_delta_update = true;
    header.segmentation.feature_enabled[2][1] = true;
    header.segmentation.feature_data[2][1] = 10;
    levels = media::vp9_segment_filter_levels(header, 2);
    CHECK_EQ(static_cast<int>(levels[0][0]), 12);
    CHECK_EQ(static_cast<int>(levels[2][1]), 12);
    // And a quantizer of its own, as a delta, clamped.
    header.segmentation.abs_or_delta_update = false;
    header.base_q_idx = 250;
    header.segmentation.feature_enabled[5][0] = true;
    header.segmentation.feature_data[5][0] = 20;
    CHECK_EQ(media::vp9_segment_qindex(header, 5), 255);
    CHECK_EQ(media::vp9_segment_qindex(header, 4), 250);
    // Without deltas every reference and mode has the segment's level.
    header.loop_filter.delta_enabled = false;
    levels = media::vp9_segment_filter_levels(header, 0);
    CHECK_EQ(static_cast<int>(levels[3][1]), 40);
}

}

int main()
{
    test_quantizer_tables();
    test_superframes();
    test_stream_headers();
    test_filter_levels();
    return test::report("test_vp9");
}
