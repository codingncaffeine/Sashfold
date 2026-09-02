#include "Test.h"

#include "core/Bitmap.h"
#include "core/Jpeg.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

// The JPEG decoder against an encoder written for the test: a forward DCT,
// quantization, and Huffman coding with tables built here (one flat, one
// with the standard DC lengths), sequential through 4:4:4, 4:2:0 and
// grayscale, odd sizes, restart intervals, byte stuffing, and an Adobe RGB
// file; progressive with spectral selection alone and with successive
// approximation — end-of-band runs, refinement bits, the lot; then
// truncations, corruption, and whatever JPEGs the machine has lying around.

using namespace sashfold;

namespace {

constexpr double pi = 3.14159265358979323846;

constexpr std::array<std::uint8_t, 64> zigzag { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26,
    33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

struct Table {
    std::array<std::uint8_t, 17> counts {}; // per code length 1..16
    std::vector<std::uint8_t> symbols;
    std::array<std::uint16_t, 256> code {};
    std::array<std::uint8_t, 256> length {};

    void assign_codes()
    {
        int next = 0;
        std::size_t index = 0;
        for (int len = 1; len <= 16; ++len) {
            for (int i = 0; i < counts[static_cast<std::size_t>(len)]; ++i) {
                code[symbols[index]] = static_cast<std::uint16_t>(next);
                length[symbols[index]] = static_cast<std::uint8_t>(len);
                ++next;
                ++index;
            }
            next <<= 1;
        }
    }
};

// Every AC symbol at eight bits, the last one at nine so no code is all ones.
Table flat_ac_table()
{
    Table table;
    table.counts[8] = 255;
    table.counts[9] = 1;
    for (int symbol = 0; symbol < 256; ++symbol)
        table.symbols.push_back(static_cast<std::uint8_t>(symbol));
    table.assign_codes();
    return table;
}

// The standard luminance DC lengths: twelve categories over lengths 2..9.
Table standard_dc_table()
{
    Table table;
    table.counts = { 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
    table.symbols = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    table.assign_codes();
    return table;
}

Table flat_dc_table()
{
    Table table;
    table.counts[4] = 12;
    table.symbols = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    table.assign_codes();
    return table;
}

class Writer {
public:
    std::vector<std::uint8_t> out;

    void byte(std::uint8_t value) { out.push_back(value); }
    void u16(int value)
    {
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }
    void marker(std::uint8_t id)
    {
        out.push_back(0xFF);
        out.push_back(id);
    }

    // Entropy-coded bits, most significant first, 0xFF stuffed with 0x00.
    void bits(int value, int count)
    {
        for (int i = count - 1; i >= 0; --i) {
            m_bits = static_cast<std::uint32_t>(m_bits << 1 | ((value >> i) & 1));
            if (++m_count == 8) {
                out.push_back(static_cast<std::uint8_t>(m_bits));
                if (m_bits == 0xFF)
                    out.push_back(0x00);
                m_bits = 0;
                m_count = 0;
            }
        }
    }
    void flush()
    {
        while (m_count != 0)
            bits(1, 1); // pad with ones, as the standard suggests
    }

private:
    std::uint32_t m_bits = 0;
    int m_count = 0;
};

int category(int value)
{
    int magnitude = std::abs(value);
    int size = 0;
    while (magnitude) {
        magnitude >>= 1;
        ++size;
    }
    return size;
}

int magnitude_bits(int value, int size)
{
    return value >= 0 ? value : value + (1 << size) - 1;
}

struct Options {
    bool color = true;
    bool subsample = false; // 4:2:0 luma
    int restart_interval = 0;
    bool adobe_rgb = false;
    bool standard_dc = false;
    int quality = 1; // the quantizer for every coefficient
    bool progressive = false;
    bool successive = false; // progressive with refinement scans
    bool lie_about_frame = false; // a progressive header on sequential data
};

using Block = std::array<int, 64>;

// A JPEG of the bitmap, sequential or progressive. Not fast, not clever:
// the reference.
std::vector<std::uint8_t> encode(Bitmap const& image, Options const& options)
{
    Writer w;
    w.marker(0xD8);
    if (options.adobe_rgb) {
        w.marker(0xEE);
        w.u16(14);
        for (char const c : std::string("Adobe"))
            w.byte(static_cast<std::uint8_t>(c));
        w.u16(100);
        w.u16(0);
        w.u16(0);
        w.byte(0); // transform 0: RGB
    }
    w.marker(0xDB);
    w.u16(2 + 65);
    w.byte(0);
    for (int k = 0; k < 64; ++k)
        w.byte(static_cast<std::uint8_t>(options.quality));
    int const count = options.color ? 3 : 1;
    int const luma = options.subsample ? 2 : 1;
    w.marker(options.progressive || options.lie_about_frame ? 0xC2 : 0xC0);
    w.u16(8 + count * 3);
    w.byte(8);
    w.u16(image.height());
    w.u16(image.width());
    w.byte(static_cast<std::uint8_t>(count));
    auto const id_of = [&](int c) {
        return options.adobe_rgb ? static_cast<std::uint8_t>("RGB"[c]) : static_cast<std::uint8_t>(c + 1);
    };
    for (int c = 0; c < count; ++c) {
        w.byte(id_of(c));
        w.byte(static_cast<std::uint8_t>(c == 0 ? (luma << 4 | luma) : 0x11));
        w.byte(0);
    }
    Table const dc = options.standard_dc ? standard_dc_table() : flat_dc_table();
    Table const ac = flat_ac_table();
    for (int table_class = 0; table_class < 2; ++table_class) {
        Table const& table = table_class == 0 ? dc : ac;
        w.marker(0xC4);
        w.u16(static_cast<int>(2 + 17 + table.symbols.size()));
        w.byte(static_cast<std::uint8_t>(table_class << 4));
        for (int len = 1; len <= 16; ++len)
            w.byte(table.counts[static_cast<std::size_t>(len)]);
        for (std::uint8_t const symbol : table.symbols)
            w.byte(symbol);
    }
    if (options.restart_interval) {
        w.marker(0xDD);
        w.u16(4);
        w.u16(options.restart_interval);
    }

    // Every block's quantized coefficients, per component, over the grid
    // padded to whole MCUs.
    int const h_max = luma;
    int const mcus_x = (image.width() + 8 * h_max - 1) / (8 * h_max);
    int const mcus_y = (image.height() + 8 * h_max - 1) / (8 * h_max);
    auto const plane_sample = [&](int component, int px, int py, int factor) {
        auto const pixel_value = [&](int x, int y) {
            x = std::clamp(x, 0, image.width() - 1);
            y = std::clamp(y, 0, image.height() - 1);
            Color const c = image.pixel(x, y);
            if (!options.color)
                return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
            if (options.adobe_rgb)
                return static_cast<double>(component == 0 ? c.r : component == 1 ? c.g : c.b);
            double const yy = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
            if (component == 0)
                return yy;
            if (component == 1)
                return 128 - 0.168736 * c.r - 0.331264 * c.g + 0.5 * c.b;
            return 128 + 0.5 * c.r - 0.418688 * c.g - 0.081312 * c.b;
        };
        if (factor == 1)
            return pixel_value(px, py);
        double sum = 0;
        for (int dy = 0; dy < factor; ++dy)
            for (int dx = 0; dx < factor; ++dx)
                sum += pixel_value(px * factor + dx, py * factor + dy);
        return sum / (factor * factor);
    };
    struct Plane {
        int blocks_wide = 0;
        int blocks_high = 0;
        int valid_wide = 0; // the blocks a non-interleaved scan visits
        int valid_high = 0;
        std::vector<Block> blocks;
        Block const& at(int bx, int by) const { return blocks[static_cast<std::size_t>(by * blocks_wide + bx)]; }
    };
    std::vector<Plane> planes;
    for (int c = 0; c < count; ++c) {
        int const factor_blocks = c == 0 ? luma : 1;
        int const factor = c == 0 ? 1 : luma;
        Plane plane;
        plane.blocks_wide = mcus_x * factor_blocks;
        plane.blocks_high = mcus_y * factor_blocks;
        plane.valid_wide = ((image.width() * factor_blocks + h_max - 1) / h_max + 7) / 8;
        plane.valid_high = ((image.height() * factor_blocks + h_max - 1) / h_max + 7) / 8;
        for (int by = 0; by < plane.blocks_high; ++by) {
            for (int bx = 0; bx < plane.blocks_wide; ++bx) {
                std::array<double, 64> samples {};
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        samples[static_cast<std::size_t>(y * 8 + x)]
                            = plane_sample(c, bx * 8 + x, by * 8 + y, factor) - 128;
                Block block {};
                for (int v = 0; v < 8; ++v) {
                    for (int u = 0; u < 8; ++u) {
                        double sum = 0;
                        for (int y = 0; y < 8; ++y)
                            for (int x = 0; x < 8; ++x)
                                sum += samples[static_cast<std::size_t>(y * 8 + x)]
                                    * std::cos((2 * x + 1) * u * pi / 16) * std::cos((2 * y + 1) * v * pi / 16);
                        double const cu = u == 0 ? 1 / std::sqrt(2.0) : 1;
                        double const cv = v == 0 ? 1 / std::sqrt(2.0) : 1;
                        block[static_cast<std::size_t>(v * 8 + u)]
                            = static_cast<int>(std::lround(sum * cu * cv / 4 / options.quality));
                    }
                }
                plane.blocks.push_back(block);
            }
        }
        planes.push_back(std::move(plane));
    }

    auto const write_sos = [&](std::vector<int> const& scan_components, int ss, int se, int ah, int al) {
        w.marker(0xDA);
        w.u16(6 + static_cast<int>(scan_components.size()) * 2);
        w.byte(static_cast<std::uint8_t>(scan_components.size()));
        for (int const c : scan_components) {
            w.byte(id_of(c));
            w.byte(0x00);
        }
        w.byte(static_cast<std::uint8_t>(ss));
        w.byte(static_cast<std::uint8_t>(se));
        w.byte(static_cast<std::uint8_t>(ah << 4 | al));
    };
    std::array<int, 3> prediction { 0, 0, 0 };
    int restart_number = 0;
    auto const restart_if_due = [&](int unit_index, auto&& before) {
        if (options.restart_interval && unit_index > 0 && unit_index % options.restart_interval == 0) {
            before();
            w.flush();
            w.marker(static_cast<std::uint8_t>(0xD0 + (restart_number++ & 7)));
            prediction = { 0, 0, 0 };
        }
    };
    auto const write_dc = [&](int component, int value) {
        int const diff = value - prediction[static_cast<std::size_t>(component)];
        prediction[static_cast<std::size_t>(component)] = value;
        int const size = category(diff);
        w.bits(dc.code[static_cast<std::size_t>(size)], dc.length[static_cast<std::size_t>(size)]);
        if (size)
            w.bits(magnitude_bits(diff, size), size);
    };
    auto const write_ac_symbol = [&](int symbol) {
        w.bits(ac.code[static_cast<std::size_t>(symbol)], ac.length[static_cast<std::size_t>(symbol)]);
    };

    if (!options.progressive) {
        write_sos([&] {
            std::vector<int> all;
            for (int c = 0; c < count; ++c)
                all.push_back(c);
            return all;
        }(), 0, 63, 0, 0);
        int mcu_index = 0;
        for (int my = 0; my < mcus_y; ++my) {
            for (int mx = 0; mx < mcus_x; ++mx) {
                restart_if_due(mcu_index, [] {});
                for (int c = 0; c < count; ++c) {
                    int const blocks = c == 0 ? luma : 1;
                    for (int by = 0; by < blocks; ++by) {
                        for (int bx = 0; bx < blocks; ++bx) {
                            Block const& block = planes[static_cast<std::size_t>(c)].at(mx * blocks + bx, my * blocks + by);
                            write_dc(c, block[0]);
                            int run = 0;
                            for (int k = 1; k < 64; ++k) {
                                int const value = block[zigzag[static_cast<std::size_t>(k)]];
                                if (value == 0) {
                                    ++run;
                                    continue;
                                }
                                while (run > 15) {
                                    write_ac_symbol(0xF0);
                                    run -= 16;
                                }
                                int const s = category(value);
                                write_ac_symbol(run << 4 | s);
                                w.bits(magnitude_bits(value, s), s);
                                run = 0;
                            }
                            if (run > 0)
                                write_ac_symbol(0x00);
                        }
                    }
                }
                ++mcu_index;
            }
        }
        w.flush();
        w.marker(0xD9);
        return w.out;
    }

    // Progressive. The DC scans are interleaved over MCUs; the AC scans
    // visit one component's own block grid with end-of-band runs, and the
    // refinement scans append one bit per coefficient the band already has.
    int eob_run = 0;
    std::vector<int> buffered_bits; // correction bits waiting for the run's symbol
    auto const emit_eob_run = [&]() {
        if (eob_run == 0)
            return;
        int nbits = 0;
        while ((2 << nbits) <= eob_run)
            ++nbits;
        write_ac_symbol(nbits << 4);
        if (nbits)
            w.bits(eob_run & ((1 << nbits) - 1), nbits);
        eob_run = 0;
        for (int const bit : buffered_bits)
            w.bits(bit, 1);
        buffered_bits.clear();
    };
    auto const dc_scan = [&](int ah, int al) {
        std::vector<int> all;
        for (int c = 0; c < count; ++c)
            all.push_back(c);
        write_sos(all, 0, 0, ah, al);
        prediction = { 0, 0, 0 };
        int mcu_index = 0;
        for (int my = 0; my < mcus_y; ++my) {
            for (int mx = 0; mx < mcus_x; ++mx) {
                restart_if_due(mcu_index, [] {});
                for (int c = 0; c < count; ++c) {
                    int const blocks = c == 0 ? luma : 1;
                    for (int by = 0; by < blocks; ++by) {
                        for (int bx = 0; bx < blocks; ++bx) {
                            int const dc_value = planes[static_cast<std::size_t>(c)].at(mx * blocks + bx, my * blocks + by)[0];
                            if (ah == 0)
                                write_dc(c, dc_value >> al); // arithmetic shift: the DC point transform
                            else
                                w.bits((dc_value >> al) & 1, 1);
                        }
                    }
                }
                ++mcu_index;
            }
        }
        w.flush();
    };
    auto const ac_first_scan = [&](int c, int ss, int se, int al) {
        write_sos({ c }, ss, se, 0, al);
        Plane const& plane = planes[static_cast<std::size_t>(c)];
        eob_run = 0;
        int unit = 0;
        for (int by = 0; by < plane.valid_high; ++by) {
            for (int bx = 0; bx < plane.valid_wide; ++bx) {
                restart_if_due(unit, emit_eob_run);
                Block const& block = plane.at(bx, by);
                int run = 0;
                for (int k = ss; k <= se; ++k) {
                    int value = block[zigzag[static_cast<std::size_t>(k)]];
                    // The AC point transform truncates the magnitude toward zero.
                    int const magnitude = std::abs(value) >> al;
                    if (magnitude == 0) {
                        ++run;
                        continue;
                    }
                    value = value < 0 ? -magnitude : magnitude;
                    emit_eob_run();
                    while (run > 15) {
                        write_ac_symbol(0xF0);
                        run -= 16;
                    }
                    int const s = category(value);
                    write_ac_symbol(run << 4 | s);
                    w.bits(magnitude_bits(value, s), s);
                    run = 0;
                }
                if (run > 0) {
                    ++eob_run;
                    if (eob_run == 0x7FFF)
                        emit_eob_run();
                }
                ++unit;
            }
        }
        emit_eob_run();
        w.flush();
    };
    auto const ac_refine_scan = [&](int c, int ss, int se, int al) {
        write_sos({ c }, ss, se, al + 1, al);
        Plane const& plane = planes[static_cast<std::size_t>(c)];
        eob_run = 0;
        buffered_bits.clear();
        int unit = 0;
        for (int by = 0; by < plane.valid_high; ++by) {
            for (int bx = 0; bx < plane.valid_wide; ++bx) {
                restart_if_due(unit, emit_eob_run);
                Block const& block = plane.at(bx, by);
                std::array<int, 64> magnitudes {};
                int last_new = ss - 1; // the last coefficient that becomes nonzero in this pass
                for (int k = ss; k <= se; ++k) {
                    magnitudes[static_cast<std::size_t>(k)] = std::abs(block[zigzag[static_cast<std::size_t>(k)]]) >> al;
                    if (magnitudes[static_cast<std::size_t>(k)] == 1)
                        last_new = k;
                }
                int run = 0;
                std::vector<int> pending; // correction bits since the last symbol
                for (int k = ss; k <= se; ++k) {
                    int const magnitude = magnitudes[static_cast<std::size_t>(k)];
                    if (magnitude == 0) {
                        ++run;
                        continue;
                    }
                    while (run > 15 && k <= last_new) {
                        emit_eob_run();
                        write_ac_symbol(0xF0);
                        run -= 16;
                        for (int const bit : pending)
                            w.bits(bit, 1);
                        pending.clear();
                    }
                    if (magnitude > 1) {
                        pending.push_back(magnitude & 1); // a coefficient the band already had
                        continue;
                    }
                    emit_eob_run();
                    write_ac_symbol(run << 4 | 1);
                    w.bits(block[zigzag[static_cast<std::size_t>(k)]] < 0 ? 0 : 1, 1);
                    for (int const bit : pending)
                        w.bits(bit, 1);
                    pending.clear();
                    run = 0;
                }
                if (run > 0 || !pending.empty()) {
                    ++eob_run;
                    buffered_bits.insert(buffered_bits.end(), pending.begin(), pending.end());
                    if (eob_run == 0x7FFF || buffered_bits.size() > 900)
                        emit_eob_run();
                }
                ++unit;
            }
        }
        emit_eob_run();
        w.flush();
    };

    if (!options.successive) {
        dc_scan(0, 0);
        for (int c = 0; c < count; ++c) {
            ac_first_scan(c, 1, 5, 0);
            ac_first_scan(c, 6, 63, 0);
        }
    } else {
        // The usual script: coarse first, then the missing bits.
        dc_scan(0, 1);
        ac_first_scan(0, 1, 5, 2);
        ac_first_scan(0, 6, 63, 2);
        for (int c = 1; c < count; ++c)
            ac_first_scan(c, 1, 63, 1);
        ac_refine_scan(0, 1, 63, 1);
        dc_scan(1, 0);
        for (int c = 1; c < count; ++c)
            ac_refine_scan(c, 1, 63, 0);
        ac_refine_scan(0, 1, 63, 0);
    }
    w.marker(0xD9);
    return w.out;
}

Bitmap test_picture(int width, int height)
{
    Bitmap picture(width, height, Color::rgb(0, 0, 0));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto const r = static_cast<std::uint8_t>(x * 255 / std::max(1, width - 1));
            auto const g = static_cast<std::uint8_t>(y * 255 / std::max(1, height - 1));
            auto const b = static_cast<std::uint8_t>(((x / 4 + y / 4) % 2) ? 200 : 40);
            picture.set_pixel(x, y, Color::rgb(r, g, b));
        }
    }
    return picture;
}

// Largest per-channel difference between two same-sized bitmaps.
int max_difference(Bitmap const& a, Bitmap const& b)
{
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            Color const p = a.pixel(x, y);
            Color const q = b.pixel(x, y);
            worst = std::max({ worst, std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b) });
        }
    }
    return worst;
}

bool same_pixels(Bitmap const& a, Bitmap const& b)
{
    return a.width() == b.width() && a.height() == b.height() && a.pixels() == b.pixels();
}

} // namespace

int main(int argc, char** argv)
{
    Bitmap const picture = test_picture(37, 29);

    // --seed <file>: write the test picture as a JPEG for the fuzzing corpus.
    if (argc == 3 && std::string(argv[1]) == "--seed") {
        std::vector<std::uint8_t> const bytes = encode(picture, Options {});
        std::ofstream file(argv[2], std::ios::binary);
        file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::cout << "wrote " << argv[2] << " (" << bytes.size() << " bytes)\n";
        return file ? 0 : 1;
    }

    // --- 4:4:4 color, both DC tables ------------------------------------------------------
    {
        Options options;
        std::vector<std::uint8_t> const bytes = encode(picture, options);
        CHECK(looks_like_jpeg(bytes));
        CHECK(!looks_like_jpeg({ 0xFF, 0xD8, 0x00 }));
        std::optional<Bitmap> const decoded = decode_jpeg(bytes);
        if (CHECK(decoded.has_value())) {
            CHECK_EQ(decoded->width(), 37);
            CHECK_EQ(decoded->height(), 29);
            int const worst = max_difference(*decoded, picture);
            std::cout << "  4:4:4 worst channel error " << worst << "\n";
            CHECK(worst <= 6); // DCT rounding, chroma rounding, and the checker's hard edges
        }
        options.standard_dc = true;
        std::optional<Bitmap> const standard = decode_jpeg(encode(picture, options));
        CHECK(standard && max_difference(*standard, picture) <= 6);
    }

    // --- 4:2:0, grayscale, Adobe RGB, restarts, coarse quantization -------------------------
    {
        Options subsampled;
        subsampled.subsample = true;
        std::optional<Bitmap> const decoded = decode_jpeg(encode(picture, subsampled));
        if (CHECK(decoded.has_value())) {
            int worst_red = 0;
            for (int y = 0; y < 29; ++y)
                for (int x = 0; x < 37; ++x)
                    worst_red = std::max(worst_red, std::abs(decoded->pixel(x, y).r - picture.pixel(x, y).r));
            std::cout << "  4:2:0 worst red error " << worst_red << "\n";
            CHECK(worst_red <= 40);
            CHECK(max_difference(*decoded, picture) <= 120);
        }
        Options gray;
        gray.color = false;
        std::optional<Bitmap> const decoded_gray = decode_jpeg(encode(picture, gray));
        if (CHECK(decoded_gray.has_value())) {
            Color const p = decoded_gray->pixel(20, 10);
            CHECK(p.r == p.g && p.g == p.b);
            Color const c = picture.pixel(20, 10);
            int const expected = static_cast<int>(std::lround(0.299 * c.r + 0.587 * c.g + 0.114 * c.b));
            CHECK(std::abs(p.r - expected) <= 3);
        }
        Options adobe;
        adobe.adobe_rgb = true;
        std::optional<Bitmap> const decoded_rgb = decode_jpeg(encode(picture, adobe));
        CHECK(decoded_rgb && max_difference(*decoded_rgb, picture) <= 3);
        Options restarts;
        restarts.restart_interval = 3;
        std::vector<std::uint8_t> const with_restarts = encode(picture, restarts);
        int markers = 0;
        for (std::size_t i = 0; i + 1 < with_restarts.size(); ++i)
            markers += with_restarts[i] == 0xFF && with_restarts[i + 1] >= 0xD0 && with_restarts[i + 1] <= 0xD7;
        CHECK(markers > 3);
        std::optional<Bitmap> const decoded_restarts = decode_jpeg(with_restarts);
        CHECK(decoded_restarts && max_difference(*decoded_restarts, picture) <= 6);
        Options coarse;
        coarse.quality = 16;
        std::optional<Bitmap> const decoded_coarse = decode_jpeg(encode(picture, coarse));
        CHECK(decoded_coarse && max_difference(*decoded_coarse, picture) < 90);
        Bitmap const flat(300, 200, Color::rgb(120, 60, 200));
        std::optional<Bitmap> const decoded_flat = decode_jpeg(encode(flat, Options {}));
        CHECK(decoded_flat && max_difference(*decoded_flat, flat) <= 2);
    }

    // --- Progressive: spectral selection, then successive approximation ---------------------
    {
        std::optional<Bitmap> const sequential = decode_jpeg(encode(picture, Options {}));
        Options spectral;
        spectral.progressive = true;
        std::vector<std::uint8_t> const spectral_bytes = encode(picture, spectral);
        std::optional<Bitmap> const decoded_spectral = decode_jpeg(spectral_bytes);
        // The same coefficients arrive in bands: the picture is the sequential one exactly.
        CHECK(decoded_spectral && sequential && same_pixels(*decoded_spectral, *sequential));

        Options successive;
        successive.progressive = true;
        successive.successive = true;
        std::vector<std::uint8_t> const successive_bytes = encode(picture, successive);
        std::optional<Bitmap> const decoded_successive = decode_jpeg(successive_bytes);
        CHECK(decoded_successive && sequential && same_pixels(*decoded_successive, *sequential));
        CHECK(successive_bytes != spectral_bytes);

        // With 4:2:0, coarse quantization and restarts too.
        Options everything;
        everything.progressive = true;
        everything.successive = true;
        everything.subsample = true;
        everything.quality = 5;
        everything.restart_interval = 2;
        Options sequential_everything = everything;
        sequential_everything.progressive = false;
        sequential_everything.successive = false;
        std::optional<Bitmap> const a = decode_jpeg(encode(picture, everything));
        std::optional<Bitmap> const b = decode_jpeg(encode(picture, sequential_everything));
        CHECK(a && b && same_pixels(*a, *b));
        Bitmap const big = test_picture(211, 97);
        Options big_options;
        big_options.progressive = true;
        big_options.successive = true;
        big_options.quality = 3;
        std::optional<Bitmap> const big_progressive = decode_jpeg(encode(big, big_options));
        Options big_sequential;
        big_sequential.quality = 3;
        std::optional<Bitmap> const big_baseline = decode_jpeg(encode(big, big_sequential));
        CHECK(big_progressive && big_baseline && same_pixels(*big_progressive, *big_baseline));
        CHECK(big_baseline && max_difference(*big_baseline, big) <= 12);

        // A progressive file cut off after its first scans is a coarse picture, not nothing.
        std::optional<Bitmap> const coarse_only = decode_jpeg(std::vector<std::uint8_t>(successive_bytes.begin(),
            successive_bytes.begin() + static_cast<std::ptrdiff_t>(successive_bytes.size() / 2)));
        CHECK(coarse_only && max_difference(*coarse_only, picture) <= 100);
    }

    // --- Malformed and truncated -------------------------------------------------------------
    {
        std::vector<std::uint8_t> const bytes = encode(picture, Options {});
        CHECK(!decode_jpeg({}).has_value());
        CHECK(!decode_jpeg(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + 40)).has_value());
        for (std::size_t length = 0; length < bytes.size(); length += 7)
            (void)decode_jpeg(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length)));
        std::optional<Bitmap> const partial
            = decode_jpeg(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() * 2 / 3)));
        if (CHECK(partial.has_value())) {
            CHECK(std::abs(partial->pixel(1, 1).r - picture.pixel(1, 1).r) <= 6);
            CHECK(partial->pixel(36, 28) == Color::rgb(128, 128, 128));
        }
        Options lying;
        lying.lie_about_frame = true;
        (void)decode_jpeg(encode(picture, lying)); // sequential scan data under a progressive header
        CHECK(!decode_jpeg(bytes, 100).has_value()); // over the pixel budget
        std::vector<std::uint8_t> corrupted = bytes;
        for (std::size_t i = 200; i < corrupted.size(); i += 13)
            corrupted[i] ^= 0x5A;
        (void)decode_jpeg(corrupted);
        Options successive;
        successive.progressive = true;
        successive.successive = true;
        std::vector<std::uint8_t> const progressive_bytes = encode(picture, successive);
        for (std::size_t length = 0; length < progressive_bytes.size(); length += 11)
            (void)decode_jpeg(std::vector<std::uint8_t>(progressive_bytes.begin(),
                progressive_bytes.begin() + static_cast<std::ptrdiff_t>(length)));
        std::vector<std::uint8_t> corrupted_progressive = progressive_bytes;
        for (std::size_t i = 300; i < corrupted_progressive.size(); i += 17)
            corrupted_progressive[i] ^= 0xA5;
        (void)decode_jpeg(corrupted_progressive);
    }

    // --- JPEGs the machine has -------------------------------------------------------------------
    for (int i = 1; i < argc; ++i) {
        std::error_code error;
        int seen = 0;
        for (auto const& entry : std::filesystem::recursive_directory_iterator(argv[i], error)) {
            std::string extension = entry.path().extension().string();
            for (char& c : extension)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (extension != ".jpg" && extension != ".jpeg")
                continue;
            if (++seen > 8)
                break;
            std::ifstream file(entry.path(), std::ios::binary);
            std::vector<std::uint8_t> const bytes((std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
            std::optional<Bitmap> const decoded = decode_jpeg(bytes);
            std::cout << "  " << entry.path().string() << ": "
                      << (decoded ? std::to_string(decoded->width()) + "x" + std::to_string(decoded->height())
                                  : std::string("declined"))
                      << "\n";
        }
    }

    return test::report("jpeg");
}
