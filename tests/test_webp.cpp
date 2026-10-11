#include "Test.h"

#include "core/Bitmap.h"
#include "core/Webp.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace sashfold;

// WebP pictures against libwebp's own decode of them, pixel for pixel: the
// fixtures are synthetic pictures (gradients, shapes, noise, an alpha ramp
// with holes) encoded by libwebp with chosen settings — lossy at several
// qualities with segments, eight partitions, the simple, normal and no loop
// filter and its sharpness, the alpha chunk raw, lossless-coded and
// filtered, lossless with and without transforms and cache — and
// expected.txt holds each one's size and the FNV-1a hash of libwebp's RGBA
// (WebPDecodeRGBA, its default chroma upsampling).

namespace {

std::vector<std::uint8_t> read_file(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::uint64_t fnv64(std::vector<std::uint8_t> const& bytes)
{
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint8_t b : bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

int main(int argc, char** argv)
{
    std::string const folder = argc > 1 ? argv[1] : "tests/fixtures/webp";
    std::ifstream expected(folder + "/expected.txt");
    CHECK(expected.good());
    std::string line;
    int cases = 0;
    while (std::getline(expected, line)) {
        std::istringstream fields(line);
        std::string name, hash;
        int width = 0, height = 0;
        fields >> name >> width >> height >> hash;
        std::vector<std::uint8_t> const bytes = read_file(folder + "/" + name + ".webp");
        ++cases;
        CHECK(looks_like_webp(bytes));
        std::optional<Bitmap> const picture = decode_webp(bytes);
        if (!CHECK(picture.has_value())) {
            std::fprintf(stderr, "  %s: not decoded\n", name.c_str());
            continue;
        }
        CHECK_EQ(picture->width(), width);
        CHECK_EQ(picture->height(), height);
        char ours[17];
        std::snprintf(ours, sizeof ours, "%016llx", static_cast<unsigned long long>(fnv64(picture->pixels())));
        if (!CHECK_EQ(std::string(ours), hash))
            std::fprintf(stderr, "  %s: pixels differ from libwebp's\n", name.c_str());

        // Cut short anywhere, or with a byte changed, a picture is refused
        // or decoded — never read past its end.
        for (std::size_t keep = 0; keep < bytes.size(); keep += 1 + bytes.size() / 23) {
            std::vector<std::uint8_t> const cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(keep));
            (void)decode_webp(cut);
        }
        for (std::size_t at = 12; at < bytes.size(); at += 1 + bytes.size() / 29) {
            std::vector<std::uint8_t> changed = bytes;
            changed[at] ^= 0x5a;
            (void)decode_webp(changed);
        }
    }
    CHECK_EQ(cases, 20);
    // A size past the ceiling is refused before anything is made.
    std::vector<std::uint8_t> const big = read_file(folder + "/lossy-64x48.webp");
    CHECK(!decode_webp(big, 64 * 48 - 1).has_value());
    CHECK(decode_webp(big, 64 * 48).has_value());
    CHECK(!looks_like_webp({ 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E' }));
    return test::report("webp");
}
