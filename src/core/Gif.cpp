#include "core/Gif.h"

#include <algorithm>
#include <array>

namespace sashfold {

namespace {

constexpr std::size_t max_image_data = 64u * 1024u * 1024u;

// Bounds-checked little-endian reads; running out clears `ok`.
class Reader {
public:
    Reader(std::vector<std::uint8_t> const& bytes, std::size_t at)
        : m_bytes(bytes)
        , m_pos(at)
    {
    }

    bool ok() const { return m_ok; }
    std::size_t position() const { return m_pos; }

    std::uint8_t u8()
    {
        if (m_pos >= m_bytes.size()) {
            m_ok = false;
            return 0;
        }
        return m_bytes[m_pos++];
    }

    std::uint16_t u16()
    {
        std::uint16_t const low = u8();
        std::uint16_t const high = u8();
        return static_cast<std::uint16_t>(high << 8 | low);
    }

    void skip(std::size_t count)
    {
        if (count > m_bytes.size() - m_pos) {
            m_ok = false;
            m_pos = m_bytes.size();
            return;
        }
        m_pos += count;
    }

    // Data sub-blocks: size-prefixed runs ending at a zero size.
    bool skip_sub_blocks()
    {
        while (m_ok) {
            std::uint8_t const size = u8();
            if (size == 0)
                return m_ok;
            skip(size);
        }
        return false;
    }

    bool read_sub_blocks(std::vector<std::uint8_t>& out)
    {
        while (m_ok) {
            std::uint8_t const size = u8();
            if (size == 0)
                return m_ok;
            if (size > m_bytes.size() - m_pos || out.size() + size > max_image_data) {
                m_ok = false;
                return false;
            }
            out.insert(out.end(), m_bytes.begin() + static_cast<std::ptrdiff_t>(m_pos),
                m_bytes.begin() + static_cast<std::ptrdiff_t>(m_pos + size));
            m_pos += size;
        }
        return false;
    }

    bool read_color_table(int size_bits, std::vector<Color>& out)
    {
        std::size_t const count = std::size_t { 1 } << (size_bits + 1);
        if (count * 3 > m_bytes.size() - m_pos) {
            m_ok = false;
            return false;
        }
        out.clear();
        for (std::size_t i = 0; i < count; ++i) {
            std::uint8_t const r = u8();
            std::uint8_t const g = u8();
            std::uint8_t const b = u8();
            out.push_back(Color::rgb(r, g, b));
        }
        return true;
    }

private:
    std::vector<std::uint8_t> const& m_bytes;
    std::size_t m_pos;
    bool m_ok = true;
};

// GIF's LZW: codes of a growing width from min+1 to 12 bits, packed least
// significant bit first, a clear code that resets the table and an end
// code. Yields at most pixel_count indices; a stream that stops early
// leaves the rest unwritten (transparent), as browsers show it.
bool lzw_decode(std::vector<std::uint8_t> const& data, int min_code_size, std::size_t pixel_count,
    std::vector<std::uint8_t>& out)
{
    if (min_code_size < 2 || min_code_size > 11)
        return false;
    int const clear = 1 << min_code_size;
    int const end = clear + 1;
    constexpr int table_size = 4096;
    std::array<std::uint16_t, table_size> prefix {};
    std::array<std::uint8_t, table_size> suffix {};
    std::array<std::uint8_t, table_size> first {};
    for (int i = 0; i < clear; ++i) {
        suffix[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
        first[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    }
    int code_size = min_code_size + 1;
    int available = end + 1;
    int previous = -1;
    std::uint32_t bits = 0;
    int bit_count = 0;
    std::size_t pos = 0;
    std::vector<std::uint8_t> stack;
    out.reserve(pixel_count);

    while (out.size() < pixel_count) {
        while (bit_count < code_size) {
            if (pos >= data.size())
                return true; // the stream ended early: what came stands
            bits |= static_cast<std::uint32_t>(data[pos++]) << bit_count;
            bit_count += 8;
        }
        int const code = static_cast<int>(bits & ((1u << code_size) - 1));
        bits >>= code_size;
        bit_count -= code_size;

        if (code == clear) {
            code_size = min_code_size + 1;
            available = end + 1;
            previous = -1;
            continue;
        }
        if (code == end)
            return true;
        if (previous < 0) {
            if (code >= clear)
                return false; // the first code after a clear is a literal
            out.push_back(static_cast<std::uint8_t>(code));
            previous = code;
            continue;
        }
        if (code > available || (code >= clear && code <= end))
            return false;

        // The entry for (previous, first of this string) joins the table
        // before the string is emitted: the encoder is one step ahead.
        std::uint8_t const head = code == available ? first[static_cast<std::size_t>(previous)]
                                                    : first[static_cast<std::size_t>(code)];
        if (available < table_size) {
            prefix[static_cast<std::size_t>(available)] = static_cast<std::uint16_t>(previous);
            suffix[static_cast<std::size_t>(available)] = head;
            first[static_cast<std::size_t>(available)] = first[static_cast<std::size_t>(previous)];
            ++available;
            if (available == (1 << code_size) && code_size < 12)
                ++code_size;
        }

        // Emit the string: walk the chain into the stack, then unwind.
        stack.clear();
        int walk = code;
        int steps = 0;
        while (walk >= clear && steps++ < table_size) {
            stack.push_back(suffix[static_cast<std::size_t>(walk)]);
            walk = prefix[static_cast<std::size_t>(walk)];
        }
        if (walk >= clear)
            return false; // a chain that never reaches a root
        stack.push_back(static_cast<std::uint8_t>(walk));
        for (auto it = stack.rbegin(); it != stack.rend() && out.size() < pixel_count; ++it)
            out.push_back(*it);
        previous = code;
    }
    return true;
}

} // namespace

bool looks_like_gif(std::vector<std::uint8_t> const& bytes)
{
    return bytes.size() >= 6 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == '8'
        && (bytes[4] == '7' || bytes[4] == '9') && bytes[5] == 'a';
}

std::optional<Bitmap> decode_gif(std::vector<std::uint8_t> const& bytes, std::size_t max_pixels)
{
    if (!looks_like_gif(bytes))
        return std::nullopt;
    Reader reader(bytes, 6);
    std::uint16_t const screen_width = reader.u16();
    std::uint16_t const screen_height = reader.u16();
    std::uint8_t const packed = reader.u8();
    reader.u8(); // background index: the screen starts transparent regardless
    reader.u8(); // pixel aspect ratio
    if (!reader.ok() || screen_width == 0 || screen_height == 0
        || static_cast<std::size_t>(screen_width) * screen_height > max_pixels)
        return std::nullopt;
    std::vector<Color> global_table;
    if (packed & 0x80) {
        if (!reader.read_color_table(packed & 0x07, global_table))
            return std::nullopt;
    }

    int transparent = -1;
    while (reader.ok()) {
        std::uint8_t const block = reader.u8();
        if (!reader.ok())
            return std::nullopt;
        if (block == 0x3B)
            return std::nullopt; // the trailer, and no frame came
        if (block == 0x21) {
            std::uint8_t const label = reader.u8();
            if (label == 0xF9) {
                // Graphic control: size, packed (bit 0: transparent index
                // is meaningful), delay, transparent index, terminator.
                std::uint8_t const size = reader.u8();
                if (size != 4)
                    return std::nullopt;
                std::uint8_t const flags = reader.u8();
                reader.u16();
                std::uint8_t const index = reader.u8();
                if (flags & 0x01)
                    transparent = index;
                if (!reader.skip_sub_blocks())
                    return std::nullopt;
                continue;
            }
            if (!reader.skip_sub_blocks())
                return std::nullopt;
            continue;
        }
        if (block != 0x2C)
            return std::nullopt;

        // The image descriptor: the first frame is the picture.
        std::uint16_t const left = reader.u16();
        std::uint16_t const top = reader.u16();
        std::uint16_t const width = reader.u16();
        std::uint16_t const height = reader.u16();
        std::uint8_t const image_packed = reader.u8();
        std::vector<Color> local_table;
        if (image_packed & 0x80) {
            if (!reader.read_color_table(image_packed & 0x07, local_table))
                return std::nullopt;
        }
        bool const interlaced = (image_packed & 0x40) != 0;
        std::uint8_t const min_code_size = reader.u8();
        std::vector<std::uint8_t> data;
        if (!reader.ok() || !reader.read_sub_blocks(data))
            return std::nullopt;
        if (width == 0 || height == 0 || static_cast<std::size_t>(width) * height > max_pixels)
            return std::nullopt;
        std::vector<Color> const& table = local_table.empty() ? global_table : local_table;
        if (table.empty())
            return std::nullopt;
        std::size_t const pixel_count = static_cast<std::size_t>(width) * height;
        std::vector<std::uint8_t> indices;
        if (!lzw_decode(data, min_code_size, pixel_count, indices))
            return std::nullopt;

        Bitmap out(screen_width, screen_height, Color::rgba(0, 0, 0, 0));
        // Interlaced frames arrive in four passes of rows.
        std::vector<std::uint32_t> row_order;
        if (interlaced) {
            row_order.reserve(height);
            constexpr std::array<std::pair<int, int>, 4> passes { { { 0, 8 }, { 4, 8 }, { 2, 4 }, { 1, 2 } } };
            for (auto const& [start, step] : passes) {
                for (int row = start; row < height; row += step)
                    row_order.push_back(static_cast<std::uint32_t>(row));
            }
        }
        for (std::size_t i = 0; i < indices.size(); ++i) {
            std::uint32_t const decoded_row = static_cast<std::uint32_t>(i / width);
            std::uint32_t const row = interlaced ? row_order[decoded_row] : decoded_row;
            std::uint32_t const column = static_cast<std::uint32_t>(i % width);
            std::uint8_t const index = indices[i];
            if (static_cast<int>(index) == transparent || index >= table.size())
                continue;
            int const x = static_cast<int>(left + column);
            int const y = static_cast<int>(top + row);
            if (out.contains(x, y))
                out.set_pixel(x, y, table[index]);
        }
        return out;
    }
    return std::nullopt;
}

}
