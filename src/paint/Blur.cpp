#include "paint/Blur.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sashfold::paint {

namespace {

// One box blur along a line: pixel i averages [i - offset, i - offset + size).
void box_blur_line(std::vector<float>& line, std::vector<float>& scratch, int size, int offset)
{
    int const n = static_cast<int>(line.size());
    if (size <= 1 || n == 0)
        return;
    scratch.assign(line.size(), 0);
    double sum = 0;
    int const first = -offset;
    for (int j = first; j < first + size; ++j)
        sum += (j >= 0 && j < n) ? static_cast<double>(line[static_cast<std::size_t>(j)]) : 0.0;
    for (int i = 0; i < n; ++i) {
        scratch[static_cast<std::size_t>(i)] = static_cast<float>(sum / size);
        int const leaving = i - offset;
        int const entering = i - offset + size;
        if (leaving >= 0 && leaving < n)
            sum -= static_cast<double>(line[static_cast<std::size_t>(leaving)]);
        if (entering >= 0 && entering < n)
            sum += static_cast<double>(line[static_cast<std::size_t>(entering)]);
    }
    line.swap(scratch);
}

}

void gaussian_line(std::vector<float>& line, std::vector<float>& scratch, double sigma)
{
    int const d = static_cast<int>(std::floor(sigma * 3 * 2.5066282746310002 / 4 + 0.5));
    if (d <= 1)
        return;
    if (d % 2 == 1) {
        for (int pass = 0; pass < 3; ++pass)
            box_blur_line(line, scratch, d, d / 2);
    } else {
        box_blur_line(line, scratch, d, d / 2);
        box_blur_line(line, scratch, d, d / 2 - 1);
        box_blur_line(line, scratch, d + 1, d / 2);
    }
}

void gaussian_plane(std::vector<float>& plane, int width, int height, double sigma)
{
    if (sigma <= 0 || width <= 0 || height <= 0)
        return;
    std::size_t const w = static_cast<std::size_t>(width);
    std::vector<float> line;
    std::vector<float> scratch;
    for (int y = 0; y < height; ++y) {
        auto const row = plane.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * w);
        line.assign(row, row + width);
        gaussian_line(line, scratch, sigma);
        std::copy(line.begin(), line.end(), row);
    }
    for (int x = 0; x < width; ++x) {
        line.resize(static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y)
            line[static_cast<std::size_t>(y)] = plane[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)];
        gaussian_line(line, scratch, sigma);
        for (int y = 0; y < height; ++y)
            plane[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] = line[static_cast<std::size_t>(y)];
    }
}

}
