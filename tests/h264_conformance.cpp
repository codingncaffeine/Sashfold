// ITU-T H.264.1's conformance bitstreams, decoded and compared picture by
// picture with the reconstructions they ship with. The streams are fetched
// by tools/h264-conformance-fetch.sh (a fresh clone has none); the list
// names the ones held exact, one a line.
//
//   h264_conformance <folder> <streams.txt> [--api software|vulkan|vaapi|playback]
//
// playback decodes as <video> does: the best decoder there is, with its
// fallbacks (media/FrameDecoder.h).
//
// Each stream is raw Annex B, split here into access units (7.4.1.2.3: a
// picture's first slice has first_mb_in_slice 0; units 6 to 9 after a
// slice begin the next) and decoded through the host's buffer bookkeeping.
// Pictures come out in decoding order and the reconstruction is in output
// order, so the comparison is of the two sets of picture digests: order
// cannot make a wrong picture match.

#include "media/FrameDecoder.h"
#include "media/H264.h"
#include "media/H264Decoder.h"
#include "media/H264Software.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace sashfold;
using Bytes = std::vector<std::uint8_t>;

Bytes read_file(std::filesystem::path const& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), {});
}

// FNV-1a over a picture's three planes, as yuv420p lays them out.
std::uint64_t digest(std::uint8_t const* data, std::size_t size)
{
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i)
        h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

// The bytes of a picture's planes as the reconstruction files hold them:
// 8-bit samples as bytes, deeper ones as 16-bit little-endian words, the
// chroma planes at their format's size (a 4:0:0 picture's as 4:2:0 planes
// of mid grey, as the reference decoder writes them).
std::vector<std::uint8_t> planar_bytes(media::Nv12Picture const& p)
{
    media::Nv12Picture::Planar const& q = *p.planar;
    std::vector<std::uint8_t> out;
    auto put = [&](std::vector<std::uint16_t> const& plane, int depth) {
        for (std::uint16_t const v : plane) {
            out.push_back(static_cast<std::uint8_t>(v));
            if (depth > 8)
                out.push_back(static_cast<std::uint8_t>(v >> 8));
        }
    };
    put(q.y, q.bit_depth_luma);
    if (q.chroma_format == 0) {
        std::vector<std::uint16_t> const grey(static_cast<std::size_t>(p.chroma_width()) * static_cast<std::size_t>(p.chroma_height()),
            static_cast<std::uint16_t>(1 << (q.bit_depth_chroma - 1)));
        put(grey, q.bit_depth_chroma);
        put(grey, q.bit_depth_chroma);
    } else {
        put(q.cb, q.bit_depth_chroma);
        put(q.cr, q.bit_depth_chroma);
    }
    return out;
}

std::uint64_t digest(media::Nv12Picture const& p)
{
    if (p.planar) {
        std::vector<std::uint8_t> const bytes = planar_bytes(p);
        return digest(bytes.data(), bytes.size());
    }
    std::vector<std::uint8_t> planar(p.luma);
    int const cw = p.chroma_width(), ch = p.chroma_height();
    for (int c = 0; c < 2; ++c)
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                planar.push_back(p.chroma[static_cast<std::size_t>((y * cw + x) * 2 + c)]);
    return digest(planar.data(), planar.size());
}

// The file with one of `extensions`, the one whose name holds the
// earliest of `prefer` where there are several.
std::optional<std::filesystem::path> find(std::filesystem::path const& folder, std::vector<std::string> const& extensions, std::vector<std::string> const& prefer = {})
{
    std::optional<std::filesystem::path> found;
    std::size_t found_rank = prefer.size();
    for (auto const& entry : std::filesystem::recursive_directory_iterator(folder)) {
        if (!entry.is_regular_file())
            continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end())
            continue;
        std::size_t rank = prefer.size();
        for (std::size_t i = 0; i < prefer.size(); ++i) {
            if (entry.path().filename().string().find(prefer[i]) != std::string::npos) {
                rank = i;
                break;
            }
        }
        if (!found || rank < found_rank) {
            found = entry.path();
            found_rank = rank;
        }
    }
    return found;
}

// The stream as access units, each re-framed with start codes: a slice
// with first_mb_in_slice 0, or a unit of types 6 to 9, after the current
// unit's slices begins the next.
std::vector<Bytes> access_units(Bytes const& stream)
{
    std::vector<Bytes> units;
    Bytes current;
    bool has_slice = false;
    auto flush = [&] {
        if (!current.empty())
            units.push_back(std::move(current));
        current.clear();
        has_slice = false;
    };
    // The sets, for the colour_plane_id of a stream coded in separate
    // colour planes: each plane's first slice has first_mb_in_slice 0.
    std::map<int, media::H264Sps> sets_sps;
    std::map<int, media::H264Pps> sets_pps;
    auto later_plane = [&](media::H264Nal const& nal) {
        for (auto const& [id, p] : sets_pps) {
            auto const s = sets_sps.find(p.sps_id);
            if (s == sets_sps.end() || !s->second.separate_colour_plane)
                continue;
            if (std::optional<media::H264SliceHeader> h = media::parse_h264_slice_header(nal.bytes, s->second, p); h && h->pps_id == id)
                return h->colour_plane_id != 0;
        }
        return false;
    };
    for (media::H264Nal const& nal : media::split_h264_annex_b(stream)) {
        if (nal.type == 7) {
            if (std::optional<media::H264Sps> s = media::parse_h264_sps(nal.bytes))
                sets_sps[s->id] = *s;
        } else if (nal.type == 8) {
            std::optional<media::H264Pps> p = media::parse_h264_pps(nal.bytes, nullptr);
            if (p && sets_sps.contains(p->sps_id))
                p = media::parse_h264_pps(nal.bytes, &sets_sps[p->sps_id]);
            if (p)
                sets_pps[p->id] = *p;
        }
        if (nal.type == 1 || nal.type == 5) {
            std::vector<std::uint8_t> const rbsp = media::h264_rbsp(nal.bytes);
            bool const first = !rbsp.empty() && (rbsp[0] & 0x80) && !later_plane(nal); // ue(v) 0 is a lone one bit
            if (has_slice && first)
                flush();
            has_slice = true;
        } else if (nal.type >= 6 && nal.type <= 9 && has_slice) {
            flush();
        }
        static constexpr std::uint8_t start[4] = { 0, 0, 0, 1 };
        current.insert(current.end(), start, start + 4);
        current.insert(current.end(), nal.bytes.begin(), nal.bytes.end());
    }
    flush();
    return units;
}

struct Result {
    std::size_t decoded = 0;
    std::size_t reference = 0;
    std::size_t matched = 0;
    std::string note;
};

// The stream as playback hands it to a frame decoder: an avcC record of
// its first parameter sets, and each access unit with four-byte lengths —
// the two fields of a frame in one sample, as MP4 and Matroska store them.
struct Track {
    Bytes avcc;
    std::vector<Bytes> samples;
};

Track as_track(std::vector<Bytes> const& units)
{
    Track t;
    Bytes sps, pps;
    std::map<int, media::H264Sps> sets_sps;
    std::map<int, media::H264Pps> sets_pps;
    // The sample holding a first field alone: its frame_num and parity.
    std::optional<std::pair<int, bool>> open_field;
    for (Bytes const& unit : units) {
        Bytes sample;
        std::optional<media::H264SliceHeader> first;
        for (media::H264Nal const& nal : media::split_h264_annex_b(unit)) {
            if (nal.type == 7 && sps.empty())
                sps.assign(nal.bytes.begin(), nal.bytes.end());
            if (nal.type == 8 && pps.empty())
                pps.assign(nal.bytes.begin(), nal.bytes.end());
            if (nal.type == 7) {
                if (std::optional<media::H264Sps> s = media::parse_h264_sps(nal.bytes))
                    sets_sps[s->id] = *s;
            }
            if (nal.type == 8) {
                std::optional<media::H264Pps> p = media::parse_h264_pps(nal.bytes, nullptr);
                if (p && sets_sps.contains(p->sps_id))
                    p = media::parse_h264_pps(nal.bytes, &sets_sps[p->sps_id]);
                if (p)
                    sets_pps[p->id] = *p;
            }
            if ((nal.type == 1 || nal.type == 5) && !first) {
                for (auto const& [id, p] : sets_pps) {
                    auto const s = sets_sps.find(p.sps_id);
                    if (s == sets_sps.end())
                        continue;
                    if (std::optional<media::H264SliceHeader> h = media::parse_h264_slice_header(nal.bytes, s->second, p); h && h->pps_id == id) {
                        first = h;
                        break;
                    }
                }
            }
            auto const n = static_cast<std::uint32_t>(nal.bytes.size());
            std::uint8_t const length[4] = { static_cast<std::uint8_t>(n >> 24), static_cast<std::uint8_t>(n >> 16), static_cast<std::uint8_t>(n >> 8),
                static_cast<std::uint8_t>(n) };
            sample.insert(sample.end(), length, length + 4);
            sample.insert(sample.end(), nal.bytes.begin(), nal.bytes.end());
        }
        bool const field = first && first->field_pic;
        if (field && open_field && open_field->first == first->frame_num && open_field->second != first->bottom_field && !t.samples.empty()) {
            t.samples.back().insert(t.samples.back().end(), sample.begin(), sample.end());
            open_field.reset();
            continue;
        }
        open_field.reset();
        if (field)
            open_field = std::pair { first->frame_num, first->bottom_field };
        t.samples.push_back(std::move(sample));
    }
    if (sps.size() < 4 || pps.empty())
        return t;
    t.avcc = { 1, sps[1], sps[2], sps[3], 0xFF, 0xE1, static_cast<std::uint8_t>(sps.size() >> 8), static_cast<std::uint8_t>(sps.size()) };
    t.avcc.insert(t.avcc.end(), sps.begin(), sps.end());
    t.avcc.insert(t.avcc.end(), { 1, static_cast<std::uint8_t>(pps.size() >> 8), static_cast<std::uint8_t>(pps.size()) });
    t.avcc.insert(t.avcc.end(), pps.begin(), pps.end());
    return t;
}

Result check(std::filesystem::path const& folder, media::VideoApi api, bool playback)
{
    Result r;
    if (!std::filesystem::is_directory(folder)) {
        r.note = "not fetched (tools/h264-conformance-fetch.sh)";
        return r;
    }
    auto const bitstream = find(folder, { ".264", ".jsv", ".26l", ".avc", ".h264", ".jvt", ".bit" });
    // The cropped output where a stream ships it beside the coded size
    // ("_nogray"), the decoder's reconstruction ("rec") over other files.
    auto const yuv = find(folder, { ".yuv", ".qcif", ".cif" }, { "nogray", "rec" });
    if (!bitstream || !yuv) {
        r.note = "no bitstream or reconstruction in " + folder.string();
        return r;
    }
    std::string why;
    std::vector<std::uint64_t> ours;
    int width = 0, height = 0;
    std::size_t picture_bytes = 0;
    auto take = [&](media::Nv12Picture const& picture) {
        ours.push_back(digest(picture));
        width = picture.width;
        height = picture.height;
        picture_bytes = picture.planar ? planar_bytes(picture).size() : 0;
    };
    if (playback) {
        // The way <video> decodes: the best decoder there is, with its
        // fallbacks.
        Track const track = as_track(access_units(read_file(*bitstream)));
        std::unique_ptr<media::FrameDecoder> decoder = media::open_frame_decoder("V_H264", track.avcc, why);
        if (!decoder) {
            r.note = "no decoder: " + why;
            return r;
        }
        for (Bytes const& sample : track.samples) {
            media::FrameDecoder::Result result = decoder->decode(sample, true);
            if (result.shown)
                take(*result.shown);
        }
    } else {
        std::unique_ptr<media::H264Accelerator> accelerator = api == media::VideoApi::Software ? media::open_software_h264() : media::H264Accelerator::open(why, api);
        if (!accelerator) {
            r.note = "no decoder: " + why;
            return r;
        }
        media::H264Decoder decoder(std::move(accelerator));
        for (Bytes const& unit : access_units(read_file(*bitstream))) {
            if (std::optional<media::Nv12Picture> const picture = decoder.decode(unit))
                take(*picture);
        }
    }
    r.decoded = ours.size();
    if (width == 0) {
        r.note = "nothing decoded";
        return r;
    }
    std::size_t const size = picture_bytes ? picture_bytes
                                           : static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
            + 2 * static_cast<std::size_t>((width + 1) / 2) * static_cast<std::size_t>((height + 1) / 2);
    // The reconstruction a picture at a time (some run to gigabytes).
    std::multiset<std::uint64_t> want;
    std::ifstream reference(*yuv, std::ios::binary);
    Bytes picture(size);
    std::size_t total = 0;
    while (reference.read(reinterpret_cast<char*>(picture.data()), static_cast<std::streamsize>(size))) {
        want.insert(digest(picture.data(), size));
        total += size;
    }
    total += static_cast<std::size_t>(reference.gcount());
    r.reference = want.size();
    std::optional<std::size_t> first_unmatched;
    for (std::size_t i = 0; i < ours.size(); ++i) {
        auto const it = want.find(ours[i]);
        if (it != want.end()) {
            want.erase(it);
            ++r.matched;
        } else if (!first_unmatched) {
            first_unmatched = i;
        }
    }
    if (first_unmatched)
        r.note = "first picture matching none: " + std::to_string(*first_unmatched) + " in decoding order";
    if (total % size)
        r.note = "reconstruction size is not a whole number of " + std::to_string(width) + "x" + std::to_string(height) + " pictures";
    return r;
}

}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: h264_conformance <folder> <streams.txt> [--api software|vulkan|vaapi|playback]\n");
        return 2;
    }
    std::filesystem::path const root = argv[1];
    media::VideoApi api = media::VideoApi::Software;
    bool playback = false;
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--api") == 0) {
            std::string const a = argv[i + 1];
            api = a == "vulkan" ? media::VideoApi::Vulkan : (a == "vaapi" ? media::VideoApi::Vaapi : media::VideoApi::Software);
            playback = a == "playback";
        }
    }
    std::ifstream list(argv[2]);
    std::string line;
    int failed = 0, passed = 0;
    while (std::getline(list, line)) {
        std::string const name = line.substr(0, line.find_first_of(" \t#"));
        if (name.empty())
            continue;
        Result const r = check(root / name, api, playback);
        bool const ok = r.reference > 0 && r.matched == r.reference && r.decoded == r.reference && r.note.empty();
        std::printf("%s %-22s %zu of %zu pictures (%zu decoded)%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), r.matched, r.reference, r.decoded,
            r.note.empty() ? "" : ": ", r.note.c_str());
        (ok ? passed : failed)++;
    }
    std::printf("h264_conformance: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
