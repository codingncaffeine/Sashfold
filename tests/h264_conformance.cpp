// ITU-T H.264.1's conformance bitstreams, decoded and compared picture by
// picture with the reconstructions they ship with. The streams are fetched
// by tools/h264-conformance-fetch.sh (a fresh clone has none); the list
// names the ones held exact, one a line.
//
//   h264_conformance <folder> <streams.txt> [--api software|vulkan|vaapi]
//
// Each stream is raw Annex B, split here into access units (7.4.1.2.3: a
// picture's first slice has first_mb_in_slice 0; units 6 to 9 after a
// slice begin the next) and decoded through the host's buffer bookkeeping.
// Pictures come out in decoding order and the reconstruction is in output
// order, so the comparison is of the two sets of picture digests: order
// cannot make a wrong picture match.

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

std::uint64_t digest(media::Nv12Picture const& p)
{
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
    for (media::H264Nal const& nal : media::split_h264_annex_b(stream)) {
        if (nal.type == 1 || nal.type == 5) {
            std::vector<std::uint8_t> const rbsp = media::h264_rbsp(nal.bytes);
            bool const first = !rbsp.empty() && (rbsp[0] & 0x80); // ue(v) 0 is a lone one bit
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

Result check(std::filesystem::path const& folder, media::VideoApi api)
{
    Result r;
    auto const bitstream = find(folder, { ".264", ".jsv", ".26l", ".avc", ".h264", ".jvt", ".bit" });
    // The cropped output where a stream ships it beside the coded size
    // ("_nogray"), the decoder's reconstruction ("rec") over other files.
    auto const yuv = find(folder, { ".yuv", ".qcif", ".cif" }, { "nogray", "rec" });
    if (!bitstream || !yuv) {
        r.note = "no bitstream or reconstruction in " + folder.string();
        return r;
    }
    std::string why;
    std::unique_ptr<media::H264Accelerator> accelerator = api == media::VideoApi::Software ? media::open_software_h264() : media::H264Accelerator::open(why, api);
    if (!accelerator) {
        r.note = "no decoder: " + why;
        return r;
    }
    media::H264Decoder decoder(std::move(accelerator));
    std::vector<std::uint64_t> ours;
    int width = 0, height = 0;
    for (Bytes const& unit : access_units(read_file(*bitstream))) {
        if (std::optional<media::Nv12Picture> const picture = decoder.decode(unit)) {
            ours.push_back(digest(*picture));
            width = picture->width;
            height = picture->height;
        }
    }
    r.decoded = ours.size();
    if (width == 0) {
        r.note = "nothing decoded";
        return r;
    }
    Bytes const reference = read_file(*yuv);
    std::size_t const size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
        + 2 * static_cast<std::size_t>((width + 1) / 2) * static_cast<std::size_t>((height + 1) / 2);
    std::multiset<std::uint64_t> want;
    for (std::size_t at = 0; at + size <= reference.size(); at += size)
        want.insert(digest(reference.data() + at, size));
    r.reference = want.size();
    for (std::uint64_t const d : ours) {
        auto const it = want.find(d);
        if (it != want.end()) {
            want.erase(it);
            ++r.matched;
        }
    }
    if (reference.size() % size)
        r.note = "reconstruction size is not a whole number of " + std::to_string(width) + "x" + std::to_string(height) + " pictures";
    return r;
}

}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: h264_conformance <folder> <streams.txt> [--api software|vulkan|vaapi]\n");
        return 2;
    }
    std::filesystem::path const root = argv[1];
    media::VideoApi api = media::VideoApi::Software;
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--api") == 0) {
            std::string const a = argv[i + 1];
            api = a == "vulkan" ? media::VideoApi::Vulkan : (a == "vaapi" ? media::VideoApi::Vaapi : media::VideoApi::Software);
        }
    }
    std::ifstream list(argv[2]);
    std::string line;
    int failed = 0, passed = 0;
    while (std::getline(list, line)) {
        std::string const name = line.substr(0, line.find_first_of(" \t#"));
        if (name.empty())
            continue;
        Result const r = check(root / name, api);
        bool const ok = r.reference > 0 && r.matched == r.reference && r.decoded == r.reference && r.note.empty();
        std::printf("%s %-22s %zu of %zu pictures (%zu decoded)%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), r.matched, r.reference, r.decoded,
            r.note.empty() ? "" : ": ", r.note.c_str());
        (ok ? passed : failed)++;
    }
    std::printf("h264_conformance: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
