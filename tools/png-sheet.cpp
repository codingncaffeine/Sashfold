// png_sheet lays pictures out on one sheet, each under a line that says what
// it is: the chrome under a dozen themes, or before and after a change to
// its look, seen at a glance instead of a file at a time. A build tool,
// never shipped.
//
//   png_sheet <out.png> [--columns N] <label>=<picture.png>...
//
// Pictures keep their size; a column is as wide as its widest picture and a
// row as tall as its tallest. Exit status: 0 when the sheet was written, 2
// for a picture that cannot be read, nothing to lay out, or a usage error.
#include "core/Bitmap.h"
#include "core/Png.h"
#include "core/Unicode.h"
#include "text/SashfoldMono.h"

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

constexpr int gutter = 12;
constexpr int label_height = 22;
constexpr float label_size = 13.0f;

std::optional<Bitmap> read_png(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::vector<std::uint8_t> const bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return decode_png(bytes);
}

struct Cell {
    std::string label;
    Bitmap picture;
};

}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fputs("usage: png_sheet <out.png> [--columns N] <label>=<picture.png>...\n", stderr);
        return 2;
    }
    int columns = 1;
    std::vector<Cell> cells;
    for (int i = 2; i < argc; ++i) {
        std::string const arg = argv[i];
        if (arg == "--columns" && i + 1 < argc) {
            columns = std::max(1, std::atoi(argv[++i]));
            continue;
        }
        std::size_t const equals = arg.find('=');
        if (equals == std::string::npos || equals == 0 || equals + 1 >= arg.size()) {
            std::fprintf(stderr, "png_sheet: expected <label>=<picture.png>, got %s\n", arg.c_str());
            return 2;
        }
        std::optional<Bitmap> picture = read_png(arg.substr(equals + 1));
        if (!picture) {
            std::fprintf(stderr, "png_sheet: cannot read %s\n", arg.substr(equals + 1).c_str());
            return 2;
        }
        cells.push_back({ arg.substr(0, equals), std::move(*picture) });
    }
    if (cells.empty()) {
        std::fputs("png_sheet: nothing to lay out\n", stderr);
        return 2;
    }
    columns = std::min(columns, static_cast<int>(cells.size()));
    int const rows = (static_cast<int>(cells.size()) + columns - 1) / columns;
    std::vector<int> column_width(static_cast<std::size_t>(columns), 0);
    std::vector<int> row_height(static_cast<std::size_t>(rows), 0);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        std::size_t const column = i % static_cast<std::size_t>(columns);
        std::size_t const row = i / static_cast<std::size_t>(columns);
        column_width[column] = std::max(column_width[column], cells[i].picture.width());
        row_height[row] = std::max(row_height[row], cells[i].picture.height());
    }
    int width = gutter;
    for (int const w : column_width)
        width += w + gutter;
    int height = gutter;
    for (int const h : row_height)
        height += label_height + h + gutter;

    Bitmap sheet(width, height, Color::rgb(0x18, 0x1a, 0x1f));
    text::SashfoldMono const& font = text::SashfoldMono::instance();
    float const advance = text::SashfoldMono::advance(label_size);
    int y = gutter;
    for (int row = 0; row < rows; ++row) {
        int x = gutter;
        for (int column = 0; column < columns; ++column) {
            std::size_t const index = static_cast<std::size_t>(row * columns + column);
            if (index < cells.size()) {
                float pen = static_cast<float>(x);
                for (char32_t const c : decode_utf8(cells[index].label)) {
                    font.draw_glyph(sheet, c, pen, static_cast<float>(y) + 15.0f, label_size,
                        Color::rgb(0xe6, 0xe8, 0xec), false, false);
                    pen += advance;
                }
                sheet.blit(cells[index].picture, x, y + label_height);
            }
            x += column_width[static_cast<std::size_t>(column)] + gutter;
        }
        y += label_height + row_height[static_cast<std::size_t>(row)] + gutter;
    }
    std::vector<std::uint8_t> const bytes = encode_png(sheet);
    std::ofstream out(argv[1], std::ios::binary);
    out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        std::fprintf(stderr, "png_sheet: cannot write %s\n", argv[1]);
        return 2;
    }
    std::printf("%s: %zu pictures, %d x %d\n", argv[1], cells.size(), width, height);
    return 0;
}
