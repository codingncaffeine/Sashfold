#include "Test.h"

#include "crypto/Sha2.h"
#include "media/H264Decoder.h"
#include "media/H264.h"
#include "media/Mp4.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// The H.264 bitstream layer over the streams libx264 wrote into the MP4
// fixtures (tools/mp4-make-tests.sh): the avcC's parameter sets, the
// picture's size through the cropping, each slice's header, and the
// picture order counts, which must order the frames as their presentation
// times do and name each picture's type as ffprobe does.

using namespace sashfold;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes read_file(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// "pts_time pict_type" per video frame, by presentation time.
std::map<std::int64_t, char> read_frame_types(std::string const& path)
{
    std::map<std::int64_t, char> out;
    std::ifstream in(path);
    double pts = 0;
    std::string type;
    while (in >> pts >> type)
        out[static_cast<std::int64_t>(pts * 1e6 + (pts < 0 ? -0.5 : 0.5))] = type.empty() ? '?' : type[0];
    return out;
}

void check_stream(std::string const& folder, std::string const& name)
{
    Bytes const file = read_file(folder + "/" + name + ".mp4");
    std::map<std::int64_t, char> const types = read_frame_types(folder + "/" + name + ".frames");
    CHECK(!file.empty() && !types.empty());
    media::Mp4Parser parser;
    CHECK(parser.append(file));
    media::Mp4Track const* video = nullptr;
    for (media::Mp4Track const& t : parser.init().tracks) {
        if (t.kind == media::Mp4Track::Kind::Video)
            video = &t;
    }
    CHECK(video != nullptr);
    if (!video)
        return;
    std::optional<media::H264DecoderConfiguration> const config = media::parse_avcc(video->codec_private);
    CHECK(config && config->sps.size() == 1 && config->pps.size() == 1 && config->length_size == 4);
    if (!config)
        return;
    CHECK(config->profile_idc == 100); // libx264's High profile
    std::optional<media::H264Sps> const sps = media::parse_h264_sps(config->sps[0]);
    CHECK(sps);
    if (!sps)
        return;
    CHECK(sps->profile_idc == 100 && sps->level_idc == 11 && sps->chroma_format_idc == 1);
    CHECK(sps->width() == 160 && sps->height() == 120); // 160 x 128 coded, 8 rows cropped
    CHECK(sps->coded_width() == 160 && sps->coded_height() == 128 && sps->frame_cropping);
    // libx264 counts pictures by their frame numbers (type 2) when it writes
    // no B pictures, by the count in each header (type 0) otherwise.
    CHECK(sps->frame_mbs_only && sps->pic_order_cnt_type == (name == "video-only" ? 2 : 0) && sps->log2_max_frame_num >= 4);
    CHECK(sps->vui.present && sps->vui.timing_present && sps->vui.time_scale == 48 && sps->vui.num_units_in_tick == 1);
    CHECK(sps->vui.bitstream_restriction && sps->vui.max_num_reorder_frames == (name == "video-only" ? 0 : 2));
    CHECK(sps->dpb_frames() >= sps->max_num_ref_frames && sps->dpb_frames() <= 16);
    std::optional<media::H264Pps> const pps = media::parse_h264_pps(config->pps[0], &*sps);
    CHECK(pps);
    if (!pps)
        return;
    CHECK(pps->entropy_coding_mode && pps->sps_id == sps->id && pps->deblocking_filter_control_present);
    CHECK(pps->transform_8x8_mode && !pps->pic_scaling_matrix_present);
    CHECK(pps->scaling.list_4x4[0][0] == 16 && pps->scaling.list_8x8[0][63] == 16); // flat
    // Every frame: its slices read, its order count against its time.
    media::H264PocState poc_state;
    struct Seen {
        std::int64_t pts_us;
        int poc;
        char type;
        bool reference;
    };
    std::vector<Seen> seen;
    int slices = 0;
    for (media::Mp4Frame const& frame : parser.take_frames()) {
        if (frame.track != video->id)
            continue;
        std::vector<media::H264Nal> const nals = media::split_h264_length_prefixed(frame.data, config->length_size);
        CHECK(!nals.empty());
        std::optional<media::H264SliceHeader> first;
        for (media::H264Nal const& nal : nals) {
            if (nal.type != 1 && nal.type != 5)
                continue;
            std::optional<media::H264SliceHeader> const header = media::parse_h264_slice_header(nal.bytes, *sps, *pps);
            CHECK(header);
            if (!header)
                continue;
            ++slices;
            if (!first)
                first = header;
            CHECK(header->pps_id == pps->id && !header->field_pic && header->header_bits > 8);
        }
        CHECK(first);
        if (!first)
            continue;
        CHECK(first->idr == frame.key);
        media::H264PocState::Poc const poc = poc_state.next(*first, *sps);
        char type = 'I';
        if (first->slice_type == media::H264SliceType::P)
            type = 'P';
        else if (first->slice_type == media::H264SliceType::B)
            type = 'B';
        seen.push_back({ (frame.time_ns + (frame.time_ns < 0 ? -500 : 500)) / 1000, poc.frame(), type, first->is_reference() });
    }
    CHECK(slices >= static_cast<int>(types.size()));
    CHECK(seen.size() == types.size());
    // Presentation order by count and by time agree, within each IDR period.
    std::vector<Seen> by_time = seen;
    std::sort(by_time.begin(), by_time.end(), [](Seen const& a, Seen const& b) { return a.pts_us < b.pts_us; });
    bool ordered = true;
    for (std::size_t i = 1; i < by_time.size(); ++i) {
        if (by_time[i].poc <= by_time[i - 1].poc && by_time[i].type != 'I')
            ordered = false;
    }
    CHECK(ordered);
    // Each picture's type as ffprobe names it, by its time.
    int mismatches = 0;
    for (Seen const& s : seen) {
        auto const it = types.find(s.pts_us);
        if (it == types.end() || it->second != s.type) {
            if (mismatches < 3)
                std::fprintf(stderr, "    %s: frame at %.6f s is %c, ffprobe says %c\n", name.c_str(), s.pts_us / 1e6, s.type,
                    it == types.end() ? '?' : it->second);
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    // A B-pyramid: libx264 keeps some B pictures as references.
    if (name != "video-only")
        CHECK(std::any_of(seen.begin(), seen.end(), [](Seen const& s) { return s.type == 'B' && s.reference; }));
}

// The planes of a picture, Y then Cb then Cr, as ffmpeg writes yuv420p.
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

// Through the hardware: every picture of a file decoded in the file's
// order, put in presentation order, and each the very bytes ffmpeg's
// decoder gives (H.264 decoding is exact by the standard), with
// B-pyramids, a stream of P pictures only, and a fragmented file.
void check_hardware(std::string const& folder, std::string const& name, media::VideoApi api, char const* api_name)
{
    std::string why;
    std::unique_ptr<media::H264Accelerator> accelerator = media::H264Accelerator::open(why, api);
    char const* const expected = std::getenv("SASHFOLD_EXPECT_VIDEO_HARDWARE");
    if (!accelerator) {
        std::cerr << "test_h264: not decoding " << name << " through " << api_name << ": " << why << "\n";
        CHECK(!(expected && std::strstr(expected, api_name)));
        return;
    }
    std::cerr << "test_h264: decoding " << name << " through " << api_name << " on " << accelerator->device() << "\n";
    media::H264Decoder decoder(std::move(accelerator));
    Bytes const file = read_file(folder + "/" + name + ".mp4");
    std::vector<std::string> truth;
    {
        std::ifstream in(folder + "/" + name + ".pictures");
        std::string line;
        while (std::getline(in, line))
            truth.push_back(line);
    }
    CHECK(!file.empty() && !truth.empty());
    media::Mp4Parser parser;
    CHECK(parser.append(file));
    media::Mp4Track const* video = nullptr;
    for (media::Mp4Track const& t : parser.init().tracks) {
        if (t.kind == media::Mp4Track::Kind::Video)
            video = &t;
    }
    CHECK(video && decoder.set_configuration(video->codec_private));
    if (!video)
        return;
    std::vector<std::pair<std::int64_t, std::string>> decoded;
    for (media::Mp4Frame const& frame : parser.take_frames()) {
        if (frame.track != video->id)
            continue;
        std::optional<media::Nv12Picture> const picture = decoder.decode(frame.data);
        if (picture)
            decoded.emplace_back(frame.time_ns, planar_digest(*picture));
        else
            decoded.emplace_back(frame.time_ns, "none");
    }
    std::sort(decoded.begin(), decoded.end());
    CHECK(decoded.size() == truth.size());
    std::size_t exact = 0;
    for (std::size_t i = 0; i < decoded.size() && i < truth.size(); ++i) {
        if (decoded[i].second == truth[i])
            ++exact;
        else if (exact + 3 > i)
            std::fprintf(stderr, "    %s picture %zu (%.6f s): %s, ffmpeg %s\n", name.c_str(), i, decoded[i].first / 1e9, decoded[i].second.c_str(),
                truth[i].c_str());
    }
    CHECK(exact == truth.size());
    CHECK(decoder.counts().failed == 0 && decoder.counts().gaps == 0);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        CHECK(!"the fixtures folder is the first argument");
        return test::report("test_h264");
    }
    std::string const folder = argv[1];
    check_stream(folder, "plain-faststart");
    check_stream(folder, "fragmented");
    check_stream(folder, "video-only");
    for (char const* name : { "plain-faststart", "plain-moov-last", "video-only", "fragmented", "fragmented-absolute" })
        check_hardware(folder, name, media::VideoApi::Vulkan, "vulkan");
    // Annex B: start codes of three and four bytes, trailing zeros no unit's,
    // the emulation prevention bytes taken out of a payload.
    {
        Bytes const stream = { 0, 0, 0, 1, 0x67, 0x42, 0, 0, 3, 1, 0x80, 0, 0, 1, 0x68, 0xCE, 0x38, 0x80, 0, 0, 0, 1, 0x65, 0x88, 0, 0 };
        std::vector<media::H264Nal> const nals = media::split_h264_annex_b(stream);
        CHECK(nals.size() == 3 && nals[0].type == 7 && nals[1].type == 8 && nals[2].type == 5 && nals[2].ref_idc == 3);
        CHECK(nals[2].bytes.size() == 2);
        Bytes const rbsp = media::h264_rbsp(nals[0].bytes);
        CHECK(rbsp == (Bytes { 0x42, 0, 0, 1, 0x80 }));
    }
    // Not parameter sets at all.
    CHECK(!media::parse_h264_sps(Bytes { 0x67 }));
    CHECK(!media::parse_h264_sps(Bytes { 0x68, 0xCE, 0x38, 0x80 }));
    CHECK(!media::parse_avcc(Bytes { 2, 100, 0, 11 }));
    return test::report("test_h264");
}
