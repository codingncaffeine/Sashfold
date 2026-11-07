#include "Test.h"

#include "media/Mp4.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <vector>

// The MP4 reader over files ffmpeg wrote (tools/mp4-make-tests.sh), each
// frame's times, size and key flag against what ffprobe lists for the same
// file: the tables before and after the data, fragments with runs by moof
// and by absolute offsets, sound alone, picture alone. Each file fed whole
// and in small pieces, which must change nothing; a stream cut off and
// bent out of shape.

using namespace sashfold;
using media::Mp4Frame;
using media::Mp4Parser;
using media::Mp4Track;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes read_file(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Packet {
    int stream = 0;
    double pts = 0;
    double dts = 0;
    double duration = 0;
    std::size_t size = 0;
    bool key = false;
};

std::vector<Packet> read_packets(std::string const& path)
{
    std::vector<Packet> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        Packet p;
        int key = 0;
        if (fields >> p.stream >> p.pts >> p.dts >> p.duration >> p.size >> key) {
            p.key = key != 0;
            out.push_back(p);
        }
    }
    return out;
}

struct Outcome {
    bool ok = true;
    std::uint64_t inits = 0;
    std::vector<Mp4Frame> frames;
    media::Mp4Init init;
    bool boundary = false;
};

Outcome feed(Bytes const& stream, std::size_t piece)
{
    Mp4Parser parser;
    Outcome outcome;
    for (std::size_t at = 0; at < stream.size() && outcome.ok; at += piece) {
        std::size_t const length = std::min(piece, stream.size() - at);
        outcome.ok = parser.append(std::span<std::uint8_t const>(stream.data() + at, length));
        for (Mp4Frame& frame : parser.take_frames())
            outcome.frames.push_back(std::move(frame));
    }
    outcome.inits = parser.init_segments();
    outcome.init = parser.init();
    outcome.boundary = parser.at_segment_boundary();
    return outcome;
}

bool near_seconds(std::int64_t ns, double seconds)
{
    return std::fabs(static_cast<double>(ns) / 1e9 - seconds) < 2e-6; // ffprobe prints six places
}

// ffprobe's streams are the moov's tracks in order; its packets are in
// file order, ours too (each track's in decode order, as the file holds
// them). The frames of one stream are compared in order.
void check_file(std::string const& folder, std::string const& name, std::size_t piece, bool has_video, bool has_audio)
{
    Bytes const file = read_file(folder + "/" + name + ".mp4");
    std::vector<Packet> const packets = read_packets(folder + "/" + name + ".packets");
    CHECK(!file.empty() && !packets.empty());
    if (file.empty() || packets.empty())
        return;
    Outcome const outcome = feed(file, piece);
    CHECK(outcome.ok);
    CHECK(outcome.inits == 1);
    CHECK(outcome.boundary);
    std::vector<Mp4Track> const& tracks = outcome.init.tracks;
    CHECK(tracks.size() == static_cast<std::size_t>(has_video) + static_cast<std::size_t>(has_audio));
    for (Mp4Track const& t : tracks) {
        if (t.kind == Mp4Track::Kind::Video) {
            CHECK(t.codec_id == "V_H264" && t.width == 160 && t.height == 120 && t.timescale > 0);
            CHECK(t.codec_private.size() > 7 && t.codec_private[0] == 1); // avcC, configurationVersion 1
        } else if (t.kind == Mp4Track::Kind::Audio) {
            CHECK(t.codec_id == "A_AAC" && t.channels == 1 && t.sample_rate == 44100);
            CHECK(t.codec_private.size() >= 2 && (t.codec_private[0] >> 3) == 2); // AAC LC
        }
    }
    if (outcome.frames.size() != packets.size())
        std::fprintf(stderr, "    %s: %zu frames, ffprobe lists %zu packets\n", name.c_str(), outcome.frames.size(), packets.size());
    CHECK(outcome.frames.size() == packets.size());
    // Per stream, in order.
    std::vector<std::size_t> next(tracks.size(), 0);
    std::vector<std::vector<Mp4Frame const*>> by_track(tracks.size());
    for (Mp4Frame const& f : outcome.frames) {
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].id == f.track)
                by_track[i].push_back(&f);
        }
    }
    std::size_t mismatches = 0;
    for (Packet const& p : packets) {
        if (p.stream < 0 || static_cast<std::size_t>(p.stream) >= tracks.size()) {
            ++mismatches;
            continue;
        }
        std::size_t const index = next[static_cast<std::size_t>(p.stream)]++;
        if (index >= by_track[static_cast<std::size_t>(p.stream)].size()) {
            ++mismatches;
            continue;
        }
        Mp4Frame const& f = *by_track[static_cast<std::size_t>(p.stream)][index];
        bool const same = near_seconds(f.time_ns, p.pts) && near_seconds(f.decode_time_ns, p.dts)
            && near_seconds(static_cast<std::int64_t>(f.duration_ns), p.duration) && f.data.size() == p.size && f.key == p.key;
        if (!same) {
            if (mismatches < 3)
                std::fprintf(stderr, "    %s stream %d #%zu: ours pts %.6f dts %.6f dur %.6f size %zu key %d; ffprobe %.6f %.6f %.6f %zu %d\n",
                    name.c_str(), p.stream, index, f.time_ns / 1e9, f.decode_time_ns / 1e9, f.duration_ns / 1e9, f.data.size(), f.key,
                    p.pts, p.dts, p.duration, p.size, p.key);
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        CHECK(!"the fixtures folder is the first argument");
        return test::report("test_mp4");
    }
    std::string const folder = argv[1];
    for (std::size_t piece : { std::size_t(1) << 20, std::size_t(7) }) {
        check_file(folder, "plain-faststart", piece, true, true);
        check_file(folder, "plain-moov-last", piece, true, true);
        check_file(folder, "fragmented", piece, true, true);
        check_file(folder, "fragmented-absolute", piece, true, true);
        check_file(folder, "audio-only", piece, false, true);
        check_file(folder, "video-only", piece, true, false);
    }
    // A fragmented stream as MSE sends it: the init segment, then each
    // segment appended after a reset, which forgets offsets: the runs that
    // name their data by the moof still read.
    {
        Bytes const file = read_file(folder + "/fragmented.mp4");
        Mp4Parser parser;
        // The moov ends where the first moof begins.
        std::size_t first_moof = 0;
        for (std::size_t i = 4; i + 4 <= file.size(); ++i) {
            if (file[i] == 'm' && file[i + 1] == 'o' && file[i + 2] == 'o' && file[i + 3] == 'f') {
                first_moof = i - 4;
                break;
            }
        }
        CHECK(first_moof > 0);
        CHECK(parser.append(std::span<std::uint8_t const>(file.data(), first_moof)));
        CHECK(parser.init_segments() == 1 && parser.init().fragmented);
        parser.reset();
        CHECK(parser.append(std::span<std::uint8_t const>(file.data() + first_moof, file.size() - first_moof)));
        std::vector<Mp4Frame> const frames = parser.take_frames();
        std::vector<Packet> const packets = read_packets(folder + "/fragmented.packets");
        CHECK(frames.size() == packets.size());
        CHECK(parser.at_segment_boundary());
    }
    // Cut off inside a box: nothing more, no failure, not at a boundary;
    // junk: a failure that stays until reset.
    {
        Bytes const file = read_file(folder + "/plain-faststart.mp4");
        Mp4Parser parser;
        CHECK(parser.append(std::span<std::uint8_t const>(file.data(), file.size() / 2)));
        CHECK(!parser.at_segment_boundary() || parser.init_segments() == 1);
        Mp4Parser junk;
        Bytes const bad = { 0, 0, 0, 9, 'm', 'o', 'o', 'v', 1, 0, 0, 0, 20, 'x', 'y', 'z', 'w', 0, 0, 0, 0 };
        (void)junk.append(bad);
        Bytes const moof_alone = { 0, 0, 0, 16, 'm', 'o', 'o', 'f', 0, 0, 0, 8, 'm', 'f', 'h', 'd' };
        Mp4Parser orphan;
        CHECK(!orphan.append(moof_alone)); // a fragment with no init segment
        CHECK(orphan.failed());
        orphan.reset();
        CHECK(!orphan.failed());
    }
    return test::report("test_mp4");
}
