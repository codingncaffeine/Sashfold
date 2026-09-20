#include "Test.h"
#include "WebmBuilder.h"

#include "media/StreamBuffer.h"
#include "media/WebM.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <vector>

// The WebM stream parser over streams written byte by byte here — the
// initialization segment, sized and unsized Clusters, block groups, every
// lacing, elements passed over, input bent out of shape — each also fed a
// byte at a time, which must change nothing; and over two real files whose
// packets another reader listed. Then the buffer those frames are filed in:
// what is buffered, the offset and the window, key frames waited for,
// removal with what leaned on it, overlaps, sequence mode and eviction.

using namespace sashfold;
using media::WebmFrame;
using media::WebmParser;
using media::WebmTrack;
using namespace sashfold::test::webm;

namespace {

struct Outcome {
    bool ok = true;
    std::uint64_t inits = 0;
    std::vector<WebmFrame> frames;
    bool boundary = false;
};

Outcome feed(Bytes const& stream, std::size_t piece)
{
    WebmParser parser;
    Outcome outcome;
    for (std::size_t at = 0; at < stream.size() && outcome.ok; at += piece) {
        std::size_t const length = std::min(piece, stream.size() - at);
        outcome.ok = parser.append(std::span<std::uint8_t const>(stream.data() + at, length));
        for (WebmFrame& frame : parser.take_frames())
            outcome.frames.push_back(std::move(frame));
    }
    outcome.inits = parser.init_segments();
    outcome.boundary = parser.at_segment_boundary();
    return outcome;
}

std::string describe(std::vector<WebmFrame> const& frames)
{
    std::ostringstream out;
    for (WebmFrame const& frame : frames) {
        out << frame.track << '@' << frame.time_ns / 1000000 << (frame.key ? "K" : "") << '+';
        if (frame.duration_ns)
            out << *frame.duration_ns / 1000000;
        else
            out << '?';
        out << ':';
        for (std::uint8_t const byte : frame.data)
            out << static_cast<char>(byte);
        out << ' ';
    }
    return out.str();
}

// Whole, and then a byte at a time: the same frames either way.
std::string frames_of(Bytes const& stream, bool expect_ok = true)
{
    Outcome const whole = feed(stream, stream.size());
    Outcome const bytewise = feed(stream, 1);
    Outcome const sevens = feed(stream, 7);
    CHECK_EQ(whole.ok, expect_ok);
    CHECK_EQ(bytewise.ok, expect_ok);
    CHECK_EQ(describe(bytewise.frames), describe(whole.frames));
    CHECK_EQ(describe(sevens.frames), describe(whole.frames));
    CHECK_EQ(bytewise.inits, whole.inits);
    return describe(whole.frames);
}

void test_init_segment()
{
    WebmParser parser;
    Bytes const init = init_segment();
    CHECK(parser.append(init));
    CHECK_EQ(parser.init_segments(), std::uint64_t { 1 });
    CHECK(parser.at_segment_boundary());
    media::WebmInit const& read = parser.init();
    CHECK_EQ(read.timecode_scale_ns, std::uint64_t { 1000000 });
    CHECK(read.duration_seconds.has_value());
    CHECK_EQ(read.duration_seconds.value_or(0), 2.5);
    CHECK_EQ(read.tracks.size(), std::size_t { 2 });
    if (read.tracks.size() == 2) {
        CHECK(read.tracks[0].kind == WebmTrack::Kind::Video);
        CHECK_EQ(read.tracks[0].codec_id, std::string("V_VP9"));
        CHECK_EQ(read.tracks[0].width, 320u);
        CHECK_EQ(read.tracks[0].height, 180u);
        CHECK(read.tracks[1].kind == WebmTrack::Kind::Audio);
        CHECK_EQ(read.tracks[1].codec_id, std::string("A_OPUS"));
        CHECK_EQ(read.tracks[1].codec_private.size(), std::size_t { 4 });
        CHECK_EQ(read.tracks[1].sample_rate, 48000.0);
        CHECK_EQ(read.tracks[1].channels, 2u);
        CHECK_EQ(read.tracks[1].default_duration_ns, std::uint64_t { 20000000 });
        CHECK_EQ(read.tracks[1].codec_delay_ns, std::uint64_t { 6500000 });
        CHECK_EQ(read.tracks[1].seek_preroll_ns, std::uint64_t { 80000000 });
    }
    CHECK(read.track(2) != nullptr);
    CHECK(read.track(3) == nullptr);

    // A second initialization segment replaces the first.
    CHECK(parser.append(init));
    CHECK_EQ(parser.init_segments(), std::uint64_t { 2 });

    // Half an initialization segment is not a boundary, and reset() forgets it.
    Bytes const half(init.begin(), init.begin() + 30);
    CHECK(parser.append(half));
    CHECK(!parser.at_segment_boundary());
    parser.reset();
    CHECK(parser.at_segment_boundary());
    CHECK_EQ(parser.init().tracks.size(), std::size_t { 2 });
}

void test_clusters()
{
    // A sized Cluster: a key frame, a dependent frame, a frame of a track
    // nobody named, an audio block group with a duration and one that leans
    // on another block; then something to pass over, then a second Cluster.
    Bytes const first = element(0x1F43B675,
        concat({ element(0xE7, uint_body(1000)), element(0xA3, block(1, 0, 0x80, text("key"))), element(0xA3, block(1, 33, 0x00, text("dep"))),
            element(0xA3, block(9, 0, 0x80, text("stray"))),
            element(0xA0, concat({ element(0xA1, block(2, 5, 0x00, text("aud"))), element(0x9B, uint_body(21)) })),
            element(0xA0, concat({ element(0xA1, block(1, 66, 0x00, text("ref"))), element(0xFB, Bytes { 0xDF }) })),
            element(0xEC, Bytes(40, 0)) }));
    Bytes const cues = element(0x1C53BB6B, Bytes(300, 7));
    Bytes const second = element(0x1F43B675, concat({ element(0xE7, uint_body(2000)), element(0xA3, block(1, -10, 0x80, text("neg"))) }));
    std::string const expected = "1@1000K+?:key 1@1033+?:dep 2@1005K+21:aud 1@1066+?:ref 1@1990K+?:neg ";
    CHECK_EQ(frames_of(concat({ init_segment(), first, cues, second })), expected);

    // Clusters of unknown size end where the next one begins.
    Bytes const open_first = concat({ unsized(0x1F43B675), element(0xE7, uint_body(10)), element(0xA3, block(2, 0, 0x80, text("a"))) });
    Bytes const open_second = concat({ unsized(0x1F43B675), element(0xE7, uint_body(50)), element(0xA3, block(2, 0, 0x80, text("b"))) });
    CHECK_EQ(frames_of(concat({ init_segment(), open_first, open_second })), std::string("2@10K+?:a 2@50K+?:b "));

    // After a whole sized Cluster the parser is between segments; in the
    // middle of one it is not.
    Bytes const stream = concat({ init_segment(), second });
    CHECK(feed(stream, stream.size()).boundary);
    CHECK(!feed(Bytes(stream.begin(), stream.end() - 2), stream.size()).boundary);
}

void test_lacing()
{
    auto const cluster_of = [](Bytes const& laced_block) {
        return concat({ init_segment(), element(0x1F43B675, concat({ element(0xE7, uint_body(0)), element(0xA3, laced_block) })) });
    };
    // Xiph: three frames, the first 2 bytes, the second 3, the last what is left.
    CHECK_EQ(frames_of(cluster_of(block(2, 0, 0x82, concat({ Bytes { 2, 2, 3 }, text("aabbbcccc") })))),
        std::string("2@0K+20:aa 2@20K+20:bbb 2@40K+20:cccc "));
    // Fixed: equal shares.
    CHECK_EQ(frames_of(cluster_of(block(2, 0, 0x84, concat({ Bytes { 1 }, text("xxyy") })))), std::string("2@0K+20:xx 2@20K+20:yy "));
    // EBML: 4, then 4 - 2 = 2 (0xBD is -2 in one byte), then the rest.
    CHECK_EQ(frames_of(cluster_of(block(2, 0, 0x86, concat({ Bytes { 2, 0x84, 0xBD }, text("ddddeefff") })))),
        std::string("2@0K+20:dddd 2@20K+20:ee 2@40K+20:fff "));
    // A Xiph lace longer than the block, and a fixed lace that does not divide.
    CHECK_EQ(frames_of(cluster_of(block(2, 0, 0x82, concat({ Bytes { 1, 200 }, text("short") }))), false), std::string());
    CHECK_EQ(frames_of(cluster_of(block(2, 0, 0x84, concat({ Bytes { 1 }, text("odd") }))), false), std::string());
}

void test_malformed()
{
    Bytes const timecode = element(0xE7, uint_body(0));
    Bytes const frame = element(0xA3, block(1, 0, 0x80, text("f")));
    // Media before any initialization segment.
    CHECK_EQ(frames_of(element(0x1F43B675, concat({ timecode, frame })), false), std::string());
    // A block before its Cluster's Timecode.
    CHECK_EQ(frames_of(concat({ init_segment(), element(0x1F43B675, concat({ frame, timecode })) }), false), std::string());
    // A child that runs past the end of its Cluster.
    Bytes overrun = concat({ init_segment(), id_bytes(0x1F43B675), size_bytes(4), timecode, frame });
    CHECK_EQ(frames_of(overrun, false), std::string());
    // Not EBML at all, and a document of another type.
    CHECK_EQ(frames_of(Bytes { 0x00, 0x00, 0x00, 0x01 }, false), std::string());
    CHECK_EQ(frames_of(element(0x1A45DFA3, element(0x4282, text("other"))), false), std::string());
    // A block too large to hold whole is refused by its header alone.
    Bytes huge = concat({ init_segment(), unsized(0x1F43B675), timecode, id_bytes(0xA3), Bytes { 0x08, 0x10, 0x00, 0x00, 0x00 } });
    CHECK_EQ(frames_of(huge, false), std::string());
    // A block too short for its own header.
    CHECK_EQ(frames_of(concat({ init_segment(), element(0x1F43B675, concat({ timecode, element(0xA3, Bytes { 0x81, 0 }) })) }), false), std::string());
    // Cut short is not malformed: the frames that arrived whole are given.
    Bytes cut = concat({ init_segment(), element(0x1F43B675, concat({ timecode, frame, frame })) });
    cut.resize(cut.size() - 1);
    CHECK_EQ(frames_of(cut), std::string("1@0K+?:f "));
    // Once failed, failed until reset.
    WebmParser parser;
    CHECK(!parser.append(Bytes { 0x00, 0x00 }));
    CHECK(!parser.append(init_segment()));
    parser.reset();
    CHECK(parser.append(init_segment()));
}

Bytes read_file(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A real file against the packets another reader found in it. `offset_ms`
// is what that reader took off each time (the codec delay, for audio).
void test_real_file(std::string const& fixtures, std::string const& stem, std::string const& codec, double offset_ms)
{
    Bytes const stream = read_file(fixtures + "/" + stem + ".webm");
    CHECK(!stream.empty());
    Outcome const whole = feed(stream, stream.size());
    Outcome const pieces = feed(stream, 101);
    CHECK(whole.ok);
    CHECK_EQ(whole.inits, std::uint64_t { 1 });
    CHECK_EQ(describe(pieces.frames), describe(whole.frames));

    WebmParser parser;
    CHECK(parser.append(stream));
    CHECK_EQ(parser.init().tracks.size(), std::size_t { 1 });
    if (!parser.init().tracks.empty())
        CHECK_EQ(parser.init().tracks[0].codec_id, codec);

    std::ifstream list(fixtures + "/" + stem + ".packets");
    std::string line;
    std::size_t index = 0;
    while (std::getline(list, line)) {
        std::istringstream fields(line);
        std::string pts, duration, size, flags;
        std::getline(fields, pts, ',');
        std::getline(fields, duration, ',');
        std::getline(fields, size, ',');
        std::getline(fields, flags, ',');
        if (index >= whole.frames.size()) {
            CHECK(index < whole.frames.size());
            break;
        }
        WebmFrame const& frame = whole.frames[index];
        CHECK_EQ(frame.data.size(), static_cast<std::size_t>(std::stoul(size)));
        CHECK_EQ(frame.key, flags.starts_with("K"));
        double const ours_ms = static_cast<double>(frame.time_ns) / 1e6 - offset_ms;
        CHECK(std::abs(ours_ms - std::stod(pts)) <= 1.0);
        ++index;
    }
    CHECK(index > 0);
    CHECK_EQ(whole.frames.size(), index);
}

std::string ranges_text(media::TimeRanges const& ranges)
{
    std::ostringstream out;
    for (media::TimeRange const& range : ranges)
        out << '[' << std::llround(range.start * 1000) << ',' << std::llround(range.end * 1000) << ") ";
    return out.str();
}

void test_stream_buffer()
{
    using media::StreamBuffer;
    Bytes const init = init_segment(true, false);
    {
        // Key, dependent, dependent: each lasts until the next, the last as
        // long as the one before it.
        StreamBuffer buffer;
        StreamBuffer::Appended const first = buffer.append(init);
        CHECK(first.ok);
        CHECK(first.init_segment);
        CHECK(!first.frames);
        CHECK(buffer.has_init());
        StreamBuffer::Appended const second = buffer.append(video_cluster(0, { 0, -33, -66 }));
        CHECK(second.ok);
        CHECK(!second.init_segment);
        CHECK(second.frames);
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[0,99) "));
        CHECK_EQ(buffer.bytes(), std::size_t { 12 });
        CHECK_EQ(std::llround(buffer.highest_end() * 1000), 99ll);
        // The next Cluster carries straight on; one far off is a second range.
        CHECK(buffer.append(video_cluster(99, { 0, -33 })).ok);
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[0,165) "));
        CHECK(buffer.append(video_cluster(165, { 0, -33 })).ok);
        CHECK(buffer.append(video_cluster(5000, { 0, -33 })).ok);
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[0,231) [5000,5066) "));
        // Removing from the dependent frame at 33 to inside the second group
        // takes what leaned on those frames too, up to the key frame at 165.
        buffer.remove(0.030, 0.140);
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[0,33) [165,231) [5000,5066) "));
        // Removing to where no frame is ends at the next key frame.
        buffer.remove(0.180, 0.300);
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[0,33) [165,198) [5000,5066) "));
        CHECK_EQ(buffer.bytes(), std::size_t { 16 });
    }
    {
        // A removal in which no frame begins still runs on to the next key
        // frame: what follows its end leaned on what it cut into.
        StreamBuffer buffer;
        buffer.append(init);
        buffer.append(video_cluster(0, { 0, -33, -66, -99, -132, 165, -198 }));
        buffer.remove(0.040, 0.050);
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[0,66) [165,231) "));
    }
    {
        // Frames before the first key frame are dropped.
        StreamBuffer buffer;
        buffer.append(init);
        buffer.append(video_cluster(0, { 0, -33, 66, -99 }));
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[0,132) "));
        StreamBuffer late;
        late.append(init);
        late.append(video_cluster(1000, { -1, -33, 66, -99 }));
        CHECK_EQ(ranges_text(late.buffered()), std::string("[1066,1132) "));
    }
    {
        // The offset moves every frame; the window drops what falls outside
        // it, and what follows waits for a key frame.
        StreamBuffer buffer;
        buffer.append(init);
        buffer.timestamp_offset = 10;
        buffer.append(video_cluster(0, { 0, -33 }));
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[10000,10066) "));
        StreamBuffer windowed;
        windowed.append(init);
        windowed.append_window_start = 0.020;
        windowed.append(video_cluster(0, { 0, -33, -66, 99, -132 }));
        CHECK_EQ(ranges_text(windowed.buffered()), std::string("[99,165) "));
    }
    {
        // The same stretch appended again takes the place of the first, byte for byte.
        StreamBuffer buffer;
        buffer.append(init);
        buffer.append(video_cluster(0, { 0, -33, -66, 99, -132 }, 100));
        CHECK_EQ(buffer.bytes(), std::size_t { 500 });
        buffer.reset_parser();
        buffer.append(video_cluster(0, { 0, -33, -66 }, 10));
        CHECK_EQ(buffer.bytes(), std::size_t { 230 });
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[0,165) "));
        // A frame landing inside an old one's time takes it and its dependents out.
        buffer.reset_parser();
        buffer.append(video_cluster(110, { 0 }, 1));
        CHECK_EQ(buffer.bytes(), std::size_t { 31 });
    }
    {
        // Sequence mode lays each run of frames after the last, whatever
        // times it carries.
        StreamBuffer buffer;
        buffer.mode = StreamBuffer::Mode::Sequence;
        buffer.group_start = 0.0;
        buffer.append(init);
        buffer.append(video_cluster(7000, { 0, -33 }));
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[0,66) "));
        buffer.append(video_cluster(3000, { 0, -33 }));
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[0,132) "));
    }
    {
        // A second initialization segment must describe the same tracks.
        StreamBuffer buffer;
        CHECK(buffer.append(init).ok);
        CHECK(buffer.append(init).init_segment);
        CHECK(!buffer.append(init_segment(false, true)).ok);
        StreamBuffer nothing;
        CHECK(!nothing.append(init_segment(false, false)).ok);
    }
    {
        // Eviction gives up what lies before the playing group, no more.
        StreamBuffer buffer;
        buffer.append(init);
        std::size_t const big = 40u * 1024u * 1024u;
        for (std::uint64_t at : { 0u, 1000u, 2000u })
            buffer.append(video_cluster(at, { 0, -500 }, big / 2));
        CHECK_EQ(buffer.bytes(), 3 * big);
        CHECK(buffer.evict(2.2, big / 2));
        CHECK_EQ(buffer.bytes(), 3 * big);
        CHECK(buffer.evict(2.2, big));
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[2000,3000) "));
        CHECK(!buffer.evict(2.2, 4 * big));
    }
    {
        // Sixty frames a second under a track default of 33 ms, as real streams
        // are written: a frame lasts until the next one, whatever the default says.
        StreamBuffer buffer;
        buffer.append(init_segment(true, false, 33333333));
        buffer.append(video_cluster(0, { 0, -17, -33, -50 }));
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[0,67) "));
        CHECK_EQ(buffer.bytes(), std::size_t { 16 });
    }
    // Two tracks: buffered is where both have frames; ended, the shorter
    // counts as reaching the longer's end.
    {
        StreamBuffer buffer;
        buffer.append(init_segment());
        Bytes body = element(0xE7, uint_body(0));
        for (int at : { 0, 33, 66, 99 })
            append(body, element(0xA3, block(1, at, 0x80, text("v"))));
        for (int at : { 0, 20, 40 })
            append(body, element(0xA3, block(2, at, 0x80, text("a"))));
        CHECK(buffer.append(element(0x1F43B675, body)).ok);
        CHECK_EQ(ranges_text(buffer.buffered()), std::string("[0,60) "));
        CHECK_EQ(ranges_text(buffer.buffered(true)), std::string("[0,132) "));
    }
    CHECK_EQ(ranges_text(media::intersect({ { 0, 5 }, { 7, 9 } }, { { 1, 2 }, { 4, 8 } })), std::string("[1000,2000) [4000,5000) [7000,8000) "));
    media::TimeRanges const two { { 0, 5 }, { 7, 9 } };
    CHECK(media::range_at(two, 4.9) == &two[0]);
    CHECK(media::range_at(two, 5.0) == nullptr);
    CHECK(media::range_at(two, 9.0, true) == &two[1]);
}

void test_real_buffer(std::string const& fixtures)
{
    media::StreamBuffer video;
    CHECK(video.append(read_file(fixtures + "/tiny-vp9.webm")).ok);
    CHECK_EQ(ranges_text(video.buffered()), std::string("[0,1000) "));
    media::StreamBuffer audio;
    CHECK(audio.append(read_file(fixtures + "/tiny-opus.webm")).ok);
    media::TimeRanges const ranges = audio.buffered();
    CHECK_EQ(ranges.size(), std::size_t { 1 });
    if (ranges.size() == 1) {
        CHECK_EQ(ranges[0].start, 0.0);
        CHECK(ranges[0].end > 1.0 && ranges[0].end < 1.03);
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::string const fixtures = argc > 1 ? argv[1] : "tests/fixtures/media";
    test_init_segment();
    test_clusters();
    test_lacing();
    test_malformed();
    test_real_file(fixtures, "tiny-vp9", "V_VP9", 0.0);
    test_real_file(fixtures, "tiny-opus", "A_OPUS", 6.5);
    test_stream_buffer();
    test_real_buffer(fixtures);
    return sashfold::test::report("webm");
}
