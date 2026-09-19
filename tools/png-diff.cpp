// png_diff says where two pictures differ: how many pixels, and the box
// that holds them all. It is how a change to the chrome proves it changed
// only what it meant to before its goldens are blessed — with --within, the
// differences must all lie inside the boxes given, or the exit status says
// so. A build tool, never shipped.
//
//   png_diff <before.png> <after.png> [--within <x> <y> <width> <height>]...
//
// Exit status: 0 when the pictures are the same, or differ only inside the
// boxes given; 1 when they differ elsewhere (or anywhere, with no box);
// 2 for a picture that cannot be read, two sizes, or a usage error.
#include "core/Bitmap.h"
#include "core/Png.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

std::optional<Bitmap> read_png(char const* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::vector<std::uint8_t> const bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return decode_png(bytes);
}

bool same(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fputs("usage: png_diff <before.png> <after.png> [--within <x> <y> <width> <height>]...\n", stderr);
        return 2;
    }
    std::vector<Rect> allowed;
    for (int i = 3; i < argc; i += 5) {
        if (std::string(argv[i]) != "--within" || i + 4 >= argc) {
            std::fputs("png_diff: --within takes x y width height\n", stderr);
            return 2;
        }
        allowed.push_back(Rect { std::atoi(argv[i + 1]), std::atoi(argv[i + 2]), std::atoi(argv[i + 3]), std::atoi(argv[i + 4]) });
    }
    std::optional<Bitmap> const before = read_png(argv[1]);
    std::optional<Bitmap> const after = read_png(argv[2]);
    if (!before || !after) {
        std::fprintf(stderr, "png_diff: cannot read %s\n", !before ? argv[1] : argv[2]);
        return 2;
    }
    if (before->width() != after->width() || before->height() != after->height()) {
        std::printf("sizes differ: %dx%d and %dx%d\n", before->width(), before->height(), after->width(), after->height());
        return 2;
    }
    long long differing = 0;
    long long outside = 0;
    int left = before->width();
    int top = before->height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < before->height(); ++y) {
        for (int x = 0; x < before->width(); ++x) {
            if (same(before->pixel(x, y), after->pixel(x, y)))
                continue;
            ++differing;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
            bool inside = false;
            for (Rect const& box : allowed)
                inside = inside || box.contains(x, y);
            if (!inside)
                ++outside;
        }
    }
    if (differing == 0) {
        std::puts("same");
        return 0;
    }
    std::printf("%lld pixels differ, within x %d..%d y %d..%d", differing, left, right, top, bottom);
    if (!allowed.empty())
        std::printf("; %lld outside the boxes given", outside);
    std::puts("");
    return outside == 0 && !allowed.empty() ? 0 : 1;
}
