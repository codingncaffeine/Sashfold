#include "Test.h"

#include "crypto/Sha2.h"
#include "media/Vp9.h"
#include "media/Vp9Accelerator.h"
#include "media/WebM.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
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

// The pictures of vp9-144p.webm as ffmpeg's decoders make them: the SHA-256
// of each shown frame, planar 4:2:0 (tests/fixtures/media/README.md).
constexpr std::array<char const*, 30> vp9_144p_pictures {
    "3df1e8ffd5a2ac37cd061be7855f44a944836f751da9a6ec883c2a1718a3aefb",
    "7b6e29b56084cb42d18e0b0321d7c69d461302a3005326e446813ed7de02e2e1",
    "3619e1f1b301008e3cc3c66275f868907553a578d627326883a34e17895cb1fd",
    "4dfe2a724ae5f9ebee1e3c7c6e9ed737c73ee6ac43287c4aee7e72b003cf68fa",
    "46b5261d53b28c1c05505bc965e82244c572d533cddfa5dd4f6afb1d2e6678da",
    "a9c607297d6d05d6dbef04b91dda1758f7834b35e8f2446cdf4b45117253ec58",
    "beb0c3e14d7d0895be531479e1885149e0d998d203046af5b4741164b598c290",
    "fa8ad6b1fbbe368a14f00e92417ca2fdedfcfc0cea96b93209ca61a0e818a24c",
    "eb55a0a6d009e2467104400332027d4533fea9e06ba693a04edb12d42630ea59",
    "049513f15db2527b7b517026c1e57805afd1ffce10bab56561f4ff0a1b6455c4",
    "b8d528b6abef200f217bde1b2ed0582880895fe41e3541b48d1a6f4748723307",
    "bfa8cddaedccc04f7717dae1fe08a0ee50617fcb3a06349f6654b4a32875a7f8",
    "e5a9b6af26c30df220754f91ee0c6290ac7864895025c5d3ad29e1ff2cb125f8",
    "0cec871f8f1b075a6dd4b9638502c7bd40bf92492a47cff046db014e553a033b",
    "fbef51bd4986294e663da9751a209cbfadb5d53266668efda70d6b2ec3f75ce4",
    "d4d235f2f9d46e3d3f3d996c97f47d223587275402846ec7366a6af48101d35e",
    "99a6e13790601f7798debe1b8c610eaebe8dbee833f3143ee5d39549ef4e3b1b",
    "7290ec03764b322688fa4dfffa1d5e238295c18c5c7af1c2726706fbbe882e6b",
    "c131fd10f957a9903e15277d0e278a9eb2b6345d4b48bd05f166c3919c485a0a",
    "2d3bf5e4763c490593923a252cad8be4f35d1192089a2ada6a901c7a2cedb53c",
    "9c941a90fdcb69b74501ad92de935b56b71f4b0d03a0335fcdde397a605d4650",
    "ca22cdfb0bc17ccfaef165ae8133ea4c5d1caca813930c4d7b6c0c3231b70e1c",
    "91c120d72357c7d236d92ae1f122d40287a548dd6fb5c6eed1d25825be9d7af8",
    "26bc8f6ea2f84d84a8c15ef613bde7c8795c69db2a58031dc1a9888abec57a94",
    "e2245bf3b7ea43db4028ed77d7c88e5c57246a6e699b843f8da6c9d834486ecc",
    "d6eb9dac28c954fafd026711b67b6f8339019429487b11671e45c7f3c48488d9",
    "5542cf66cd1d3fa0422009e6dd3ba633bf87448d37ac902a79360037fb58f791",
    "337ca45a5eb0756160781aac8b1c62c2c443e1732a8873ea0e9c0d74ef25d555",
    "ad54d008a7877251de539a3bc0d821f06b95f1a90b6905b75af43aa1ba5c49aa",
    "64b04d16c5d1fa666b468d9c18a81ec7dbce40bf29059ed44f627f3caaa83883",
};

// A picture's SHA-256 as ffmpeg's planar output would have it, in hex.
std::string planar_digest(media::Nv12Picture const& picture)
{
    crypto::Sha256 hash;
    hash.update(picture.luma);
    std::size_t const samples = static_cast<std::size_t>(picture.chroma_width()) * static_cast<std::size_t>(picture.chroma_height());
    Bytes plane(samples);
    for (std::size_t offset : { std::size_t { 0 }, std::size_t { 1 } }) {
        for (std::size_t i = 0; i < samples; ++i)
            plane[i] = picture.chroma[2 * i + offset];
        hash.update(plane);
    }
    std::string hex;
    for (std::uint8_t const byte : hash.finish()) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    return hex;
}

// The machine's own VP9 decoders, by each way to them it has: every picture
// of the stream, decoded on the GPU and read back, must be exactly the
// picture ffmpeg's decoders made of it — VP9's decoding is exact by
// specification, so any difference is a parameter told wrong. A machine
// without one skips it, saying so; SASHFOLD_EXPECT_VIDEO_HARDWARE (say
// "vulkan vaapi") names the ways a machine is known to have, which must
// then open.
void test_hardware_decode()
{
    auto const fixtures = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "media";
    std::ifstream in(fixtures / "vp9-144p.webm", std::ios::binary);
    Bytes const stream { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    media::WebmParser parser;
    CHECK(parser.append(stream));
    std::vector<media::WebmFrame> const blocks = parser.take_frames();

    // What the stream is there to exercise, so that a fixture made again
    // without one of them fails here rather than testing less.
    {
        media::Vp9HeaderReader reader;
        int hidden = 0, segmented = 0, backward = 0, fixed_filter = 0, switchable = 0, delta_updates = 0;
        for (media::WebmFrame const& block : blocks) {
            for (auto const frame : media::split_vp9_superframe(block.data)) {
                std::optional<Vp9FrameHeader> const header = reader.read(frame);
                if (!header || header->show_existing_frame)
                    continue;
                hidden += !header->show_frame;
                segmented += header->segmentation.enabled;
                backward += header->ref_sign_bias[3];
                if (!header->intra())
                    (header->interp_filter == 4 ? switchable : fixed_filter)++;
                delta_updates += header->loop_filter.delta_update;
            }
        }
        CHECK_EQ(hidden, 3);
        CHECK(segmented > 0 && backward > 0 && fixed_filter > 0 && switchable > 0 && delta_updates > 0);
    }

    char const* const expected = std::getenv("SASHFOLD_EXPECT_VIDEO_HARDWARE");
    for (auto const& [api, name] : { std::pair { media::VideoApi::Vulkan, "vulkan" }, std::pair { media::VideoApi::Vaapi, "vaapi" } }) {
        std::string why;
        std::unique_ptr<media::Vp9Accelerator> accelerator = media::Vp9Accelerator::open(why, api);
        if (!accelerator) {
            std::cerr << "test_vp9: not decoding through " << name << ": " << why << "\n";
            CHECK(!(expected && std::strstr(expected, name)));
            continue;
        }
        std::cerr << "test_vp9: decoding through " << name << " on " << accelerator->device() << "\n";
        media::Vp9HeaderReader reader;
        std::size_t shown = 0;
        std::size_t exact = 0;
        bool all_decoded = true;
        for (media::WebmFrame const& block : blocks) {
            for (auto const frame : media::split_vp9_superframe(block.data)) {
                std::optional<Vp9FrameHeader> const header = reader.read(frame);
                if (!header) {
                    all_decoded = false;
                    continue;
                }
                std::optional<media::Nv12Picture> picture;
                if (header->show_existing_frame) {
                    picture = accelerator->read_slot(header->frame_to_show);
                } else if (!accelerator->decode(frame, *header)) {
                    all_decoded = false;
                } else if (header->show_frame) {
                    picture = accelerator->read_last();
                }
                if (!header->show_frame)
                    continue;
                if (picture && shown < vp9_144p_pictures.size() && planar_digest(*picture) == vp9_144p_pictures[shown])
                    exact++;
                shown++;
            }
        }
        CHECK(all_decoded);
        CHECK_EQ(shown, vp9_144p_pictures.size());
        CHECK_EQ(exact, vp9_144p_pictures.size());
    }
}

int main()
{
    test_quantizer_tables();
    test_superframes();
    test_stream_headers();
    test_filter_levels();
    test_hardware_decode();
    return test::report("test_vp9");
}
