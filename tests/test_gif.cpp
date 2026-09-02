#include "Test.h"

#include "core/Bitmap.h"
#include "core/Gif.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The GIF decoder: two real GIFs the web is full of (the 1x1 transparent
// pixel and a 1x1 color), then images written by an LZW encoder in this
// test across every code size, with interlace, local tables, a
// transparent index, a frame smaller than its screen, and a table that
// fills and clears; then malformed and truncated input.

using namespace sashfold;

namespace {

// A GIF LZW encoder for the test, one step ahead of the decoder as the
// format requires: it emits a code, then learns the next entry.
std::vector<std::uint8_t> lzw_encode(std::vector<std::uint8_t> const& indices, int min_code_size)
{
    int const clear = 1 << min_code_size;
    int const end = clear + 1;
    std::vector<std::uint8_t> out;
    std::uint32_t bits = 0;
    int bit_count = 0;
    int code_size = min_code_size + 1;
    auto const emit = [&](int code) {
        bits |= static_cast<std::uint32_t>(code) << bit_count;
        bit_count += code_size;
        while (bit_count >= 8) {
            out.push_back(static_cast<std::uint8_t>(bits & 0xFF));
            bits >>= 8;
            bit_count -= 8;
        }
    };
    std::map<std::pair<int, std::uint8_t>, int> table;
    int next = end + 1;
    emit(clear);
    if (indices.empty()) {
        emit(end);
    } else {
        int current = indices[0];
        for (std::size_t i = 1; i < indices.size(); ++i) {
            std::uint8_t const k = indices[i];
            auto const it = table.find({ current, k });
            if (it != table.end()) {
                current = it->second;
                continue;
            }
            emit(current);
            if (next < 4096) {
                table.emplace(std::make_pair(current, k), next++);
                if (next > (1 << code_size) && code_size < 12)
                    ++code_size;
            } else {
                emit(clear);
                table.clear();
                next = end + 1;
                code_size = min_code_size + 1;
            }
            current = k;
        }
        emit(current);
        emit(end);
    }
    if (bit_count > 0)
        out.push_back(static_cast<std::uint8_t>(bits & 0xFF));
    return out;
}

struct Frame {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    bool interlaced = false;
    std::vector<Color> local_table;
    std::vector<std::uint8_t> indices; // row-major, in screen order
    int min_code_size = 8;
};

struct HandMadeGif {
    int screen_width = 0;
    int screen_height = 0;
    std::vector<Color> global_table;
    int transparent = -1;
    std::vector<Frame> frames;
    bool trailer = true;
    bool version87 = false;
};

void put_u16(std::vector<std::uint8_t>& out, int value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

int table_bits(std::size_t entries)
{
    int bits = 0;
    while ((std::size_t { 2 } << bits) < entries)
        ++bits;
    return bits;
}

void put_table(std::vector<std::uint8_t>& out, std::vector<Color> const& table)
{
    std::size_t const count = std::size_t { 2 } << table_bits(table.size());
    for (std::size_t i = 0; i < count; ++i) {
        Color const c = i < table.size() ? table[i] : Color::rgb(0, 0, 0);
        out.insert(out.end(), { c.r, c.g, c.b });
    }
}

std::vector<std::uint8_t> build_gif(HandMadeGif const& spec)
{
    std::vector<std::uint8_t> out = { 'G', 'I', 'F', '8', static_cast<std::uint8_t>(spec.version87 ? '7' : '9'), 'a' };
    put_u16(out, spec.screen_width);
    put_u16(out, spec.screen_height);
    std::uint8_t packed = 0;
    if (!spec.global_table.empty())
        packed = static_cast<std::uint8_t>(0x80 | table_bits(spec.global_table.size()));
    out.push_back(packed);
    out.push_back(0); // background index
    out.push_back(0); // aspect
    if (!spec.global_table.empty())
        put_table(out, spec.global_table);
    if (spec.transparent >= 0) {
        out.insert(out.end(), { 0x21, 0xF9, 4, 0x01, 0, 0, static_cast<std::uint8_t>(spec.transparent), 0 });
    }
    out.insert(out.end(), { 0x21, 0xFE, 3, 'h', 'i', '!', 0 }); // a comment, skipped
    for (Frame const& frame : spec.frames) {
        out.push_back(0x2C);
        put_u16(out, frame.left);
        put_u16(out, frame.top);
        put_u16(out, frame.width);
        put_u16(out, frame.height);
        std::uint8_t image_packed = frame.interlaced ? 0x40 : 0;
        if (!frame.local_table.empty())
            image_packed |= static_cast<std::uint8_t>(0x80 | table_bits(frame.local_table.size()));
        out.push_back(image_packed);
        if (!frame.local_table.empty())
            put_table(out, frame.local_table);
        out.push_back(static_cast<std::uint8_t>(frame.min_code_size));
        // Rows in file order: interlaced frames store the passes.
        std::vector<std::uint8_t> ordered;
        if (frame.interlaced) {
            for (auto const& [start, step] : std::vector<std::pair<int, int>> { { 0, 8 }, { 4, 8 }, { 2, 4 }, { 1, 2 } }) {
                for (int row = start; row < frame.height; row += step) {
                    ordered.insert(ordered.end(),
                        frame.indices.begin() + static_cast<std::ptrdiff_t>(row * frame.width),
                        frame.indices.begin() + static_cast<std::ptrdiff_t>((row + 1) * frame.width));
                }
            }
        } else {
            ordered = frame.indices;
        }
        std::vector<std::uint8_t> const data = lzw_encode(ordered, frame.min_code_size);
        for (std::size_t at = 0; at < data.size(); at += 255) {
            std::size_t const length = std::min<std::size_t>(255, data.size() - at);
            out.push_back(static_cast<std::uint8_t>(length));
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                data.begin() + static_cast<std::ptrdiff_t>(at + length));
        }
        out.push_back(0);
    }
    if (spec.trailer)
        out.push_back(0x3B);
    return out;
}

} // namespace

int main()
{
    // --- Two GIFs the web is full of ----------------------------------------------------
    std::vector<std::uint8_t> const transparent_pixel { 0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00,
        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x21, 0xF9, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x2C,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3B };
    CHECK(looks_like_gif(transparent_pixel));
    CHECK(!looks_like_gif({ 'G', 'I', 'F' }));
    std::optional<Bitmap> const clear = decode_gif(transparent_pixel);
    if (CHECK(clear.has_value())) {
        CHECK_EQ(clear->width(), 1);
        CHECK_EQ(clear->height(), 1);
        CHECK_EQ(static_cast<int>(clear->pixel(0, 0).a), 0);
    }
    std::vector<std::uint8_t> const red_pixel { 'G', 'I', 'F', '8', '9', 'a', 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00,
        0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02,
        0x44, 0x01, 0x00, 0x3B };
    std::optional<Bitmap> const red = decode_gif(red_pixel);
    if (CHECK(red.has_value()))
        CHECK(red->pixel(0, 0) == Color::rgb(255, 0, 0));

    // --- Round trips through the test's encoder ---------------------------------------
    for (int min_code_size = 2; min_code_size <= 8; ++min_code_size) {
        int const colors = 1 << min_code_size;
        HandMadeGif spec;
        spec.screen_width = 23;
        spec.screen_height = 17;
        for (int i = 0; i < colors; ++i)
            spec.global_table.push_back(Color::rgb(static_cast<std::uint8_t>(i * 255 / std::max(1, colors - 1)),
                static_cast<std::uint8_t>(i * 3), static_cast<std::uint8_t>(255 - i)));
        Frame frame;
        frame.width = 23;
        frame.height = 17;
        frame.min_code_size = min_code_size;
        for (int y = 0; y < 17; ++y) {
            for (int x = 0; x < 23; ++x)
                frame.indices.push_back(static_cast<std::uint8_t>((x * 7 + y * y) % colors));
        }
        spec.frames.push_back(frame);
        std::optional<Bitmap> const decoded = decode_gif(build_gif(spec));
        if (!CHECK(decoded.has_value()))
            continue;
        bool same = decoded->width() == 23 && decoded->height() == 17;
        for (int y = 0; y < 17 && same; ++y) {
            for (int x = 0; x < 23 && same; ++x)
                same = decoded->pixel(x, y) == spec.global_table[frame.indices[static_cast<std::size_t>(y * 23 + x)]];
        }
        CHECK(same);
    }

    // A big flat image fills the table past 4096 entries and clears it.
    {
        HandMadeGif spec;
        spec.screen_width = 300;
        spec.screen_height = 200;
        for (int i = 0; i < 256; ++i)
            spec.global_table.push_back(Color::rgb(static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i / 2), 7));
        Frame frame;
        frame.width = 300;
        frame.height = 200;
        for (int y = 0; y < 200; ++y) {
            for (int x = 0; x < 300; ++x)
                frame.indices.push_back(static_cast<std::uint8_t>((x / 3 + y / 5) % 256));
        }
        spec.frames.push_back(frame);
        std::vector<std::uint8_t> const bytes = build_gif(spec);
        std::optional<Bitmap> const decoded = decode_gif(bytes);
        if (CHECK(decoded.has_value())) {
            bool same = true;
            for (int y = 0; y < 200 && same; ++y) {
                for (int x = 0; x < 300 && same; ++x)
                    same = decoded->pixel(x, y) == spec.global_table[frame.indices[static_cast<std::size_t>(y * 300 + x)]];
            }
            CHECK(same);
        }
        CHECK(bytes.size() < 300 * 200); // it did compress
    }

    // Interlace, a local table, a transparent index, a frame inside a larger screen.
    {
        HandMadeGif spec;
        spec.screen_width = 10;
        spec.screen_height = 12;
        spec.global_table = { Color::rgb(1, 1, 1), Color::rgb(2, 2, 2) };
        spec.transparent = 3;
        Frame frame;
        frame.left = 2;
        frame.top = 1;
        frame.width = 6;
        frame.height = 9;
        frame.interlaced = true;
        frame.local_table = { Color::rgb(10, 0, 0), Color::rgb(0, 20, 0), Color::rgb(0, 0, 30), Color::rgb(9, 9, 9) };
        frame.min_code_size = 2;
        for (int y = 0; y < 9; ++y) {
            for (int x = 0; x < 6; ++x)
                frame.indices.push_back(static_cast<std::uint8_t>((x + y) % 4));
        }
        spec.frames.push_back(frame);
        std::optional<Bitmap> const decoded = decode_gif(build_gif(spec));
        if (CHECK(decoded.has_value())) {
            CHECK_EQ(decoded->width(), 10);
            CHECK_EQ(decoded->height(), 12);
            CHECK_EQ(static_cast<int>(decoded->pixel(0, 0).a), 0); // outside the frame
            CHECK_EQ(static_cast<int>(decoded->pixel(9, 11).a), 0);
            bool same = true;
            for (int y = 0; y < 9 && same; ++y) {
                for (int x = 0; x < 6 && same; ++x) {
                    std::uint8_t const index = frame.indices[static_cast<std::size_t>(y * 6 + x)];
                    Color const expected = index == 3 ? Color::rgba(0, 0, 0, 0) : frame.local_table[index];
                    same = decoded->pixel(2 + x, 1 + y) == expected;
                }
            }
            CHECK(same);
        }
        // A frame past the screen's edge is clipped, not an error.
        spec.frames[0].left = 7;
        spec.frames[0].top = 8;
        std::optional<Bitmap> const clipped = decode_gif(build_gif(spec));
        CHECK(clipped.has_value());
        if (clipped)
            CHECK(clipped->pixel(7, 8) == frame.local_table[0]);
        // GIF87a decodes the same way (without the control extension).
        spec.frames[0].left = 0;
        spec.frames[0].top = 0;
        spec.transparent = -1;
        spec.version87 = true;
        CHECK(decode_gif(build_gif(spec)).has_value());
    }

    // --- Malformed and truncated: nullopt or a partial picture, never a crash -----------
    {
        HandMadeGif spec;
        spec.screen_width = 4;
        spec.screen_height = 4;
        spec.global_table = { Color::rgb(0, 0, 0), Color::rgb(255, 255, 255) };
        Frame frame;
        frame.width = 4;
        frame.height = 4;
        frame.min_code_size = 2;
        frame.indices.assign(16, 1);
        spec.frames.push_back(frame);
        std::vector<std::uint8_t> const bytes = build_gif(spec);
        CHECK(decode_gif(bytes).has_value());
        for (std::size_t length = 0; length < bytes.size(); ++length)
            (void)decode_gif(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length)));
        CHECK(!decode_gif({}).has_value());
        CHECK(!decode_gif(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + 13)).has_value());
        HandMadeGif no_table = spec;
        no_table.global_table.clear();
        CHECK(!decode_gif(build_gif(no_table)).has_value());
        HandMadeGif no_frames = spec;
        no_frames.frames.clear();
        CHECK(!decode_gif(build_gif(no_frames)).has_value());
        HandMadeGif huge = spec;
        huge.screen_width = 60000;
        huge.screen_height = 60000;
        CHECK(!decode_gif(build_gif(huge)).has_value());
        std::vector<std::uint8_t> bad_code = bytes;
        bad_code[bad_code.size() - 4] = 0xFF; // garbage in the LZW data
        (void)decode_gif(bad_code);
        HandMadeGif bad_min = spec;
        bad_min.frames[0].min_code_size = 12;
        CHECK(!decode_gif(build_gif(bad_min)).has_value());
        std::vector<std::uint8_t> wrong_block = bytes;
        wrong_block[13 + 6] = 0x99; // where the comment extension began
        CHECK(!decode_gif(wrong_block).has_value());
        CHECK(!decode_gif(bytes, 3).has_value()); // over the pixel budget
    }

    return test::report("gif");
}
