#include "core/Gif.h"

#include <algorithm>
#include <array>
#include <string>

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

std::optional<GifAnimation> scan_gif(std::vector<std::uint8_t> const& bytes, std::size_t max_pixels)
{
    if (!looks_like_gif(bytes))
        return std::nullopt;
    Reader reader(bytes, 6);
    GifAnimation animation;
    std::uint16_t const screen_width = reader.u16();
    std::uint16_t const screen_height = reader.u16();
    std::uint8_t const packed = reader.u8();
    reader.u8(); // background index: the screen starts transparent regardless
    reader.u8(); // pixel aspect ratio
    if (!reader.ok() || screen_width == 0 || screen_height == 0
        || static_cast<std::size_t>(screen_width) * screen_height > max_pixels)
        return std::nullopt;
    animation.width = screen_width;
    animation.height = screen_height;
    if (packed & 0x80) {
        if (!reader.read_color_table(packed & 0x07, animation.global_table))
            return std::nullopt;
    }

    // What the graphic control extension before an image says of it; it
    // holds for that one image and is forgotten after it.
    GifFrame pending;
    constexpr std::size_t max_frames = 10000;
    while (reader.ok()) {
        std::uint8_t const block = reader.u8();
        if (!reader.ok() || block == 0x3B)
            break; // the trailer, or a file that stops: what came stands
        if (block == 0x21) {
            std::uint8_t const label = reader.u8();
            if (label == 0xF9) {
                // Graphic control: size, packed (bit 0: the transparent index
                // is meaningful; bits 2 to 4: what becomes of the frame when
                // the next is due), delay in hundredths, transparent index.
                std::uint8_t const size = reader.u8();
                if (size != 4)
                    return std::nullopt;
                std::uint8_t const flags = reader.u8();
                std::uint16_t const delay = reader.u16();
                std::uint8_t const index = reader.u8();
                pending.transparent = (flags & 0x01) ? index : -1;
                pending.disposal = (flags >> 2) & 0x07;
                pending.delay_ms = static_cast<std::uint32_t>(delay) * 10u;
                if (!reader.skip_sub_blocks())
                    return std::nullopt;
                continue;
            }
            if (label == 0xFF) {
                // An application extension; NETSCAPE2.0's says how often the
                // animation plays: 0 is without end, n is n times more.
                // The first block is the application's name, eleven bytes;
                // what follows it is the application's own.
                std::uint8_t const size = reader.u8();
                std::string name;
                for (std::uint8_t i = 0; i < size; ++i)
                    name.push_back(static_cast<char>(reader.u8()));
                std::vector<std::uint8_t> data;
                if (!reader.ok() || !reader.read_sub_blocks(data))
                    return std::nullopt;
                if ((name == "NETSCAPE2.0" || name == "ANIMEXTS1.0") && data.size() >= 3 && data[0] == 1) {
                    std::uint32_t const times
                        = static_cast<std::uint32_t>(data[1]) | static_cast<std::uint32_t>(data[2]) << 8;
                    animation.loops = times == 0 ? 0 : times + 1;
                }
                continue;
            }
            if (!reader.skip_sub_blocks())
                return std::nullopt;
            continue;
        }
        if (block != 0x2C)
            break; // not a block of this format: what came stands

        GifFrame frame = pending;
        pending = GifFrame {};
        frame.left = reader.u16();
        frame.top = reader.u16();
        frame.width = reader.u16();
        frame.height = reader.u16();
        frame.descriptor = reader.position(); // the packed byte: tables and data follow
        std::uint8_t const image_packed = reader.u8();
        if (image_packed & 0x80) {
            std::vector<Color> unused;
            if (!reader.read_color_table(image_packed & 0x07, unused))
                break;
        }
        reader.u8(); // the minimum code size
        if (!reader.ok() || !reader.skip_sub_blocks())
            break;
        if (frame.width == 0 || frame.height == 0
            || static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) > max_pixels)
            return std::nullopt;
        animation.frames.push_back(frame);
        if (animation.frames.size() >= max_frames)
            break;
    }
    if (animation.frames.empty())
        return std::nullopt;
    return animation;
}

bool draw_gif_frame(std::vector<std::uint8_t> const& bytes, GifAnimation const& animation, std::size_t index,
    Bitmap& canvas)
{
    if (index >= animation.frames.size())
        return false;
    GifFrame const& frame = animation.frames[index];
    Reader reader(bytes, frame.descriptor);
    std::uint8_t const image_packed = reader.u8();
    std::vector<Color> local_table;
    if (image_packed & 0x80) {
        if (!reader.read_color_table(image_packed & 0x07, local_table))
            return false;
    }
    bool const interlaced = (image_packed & 0x40) != 0;
    std::uint8_t const min_code_size = reader.u8();
    std::vector<std::uint8_t> data;
    if (!reader.ok() || !reader.read_sub_blocks(data))
        return false;
    std::vector<Color> const& table = local_table.empty() ? animation.global_table : local_table;
    if (table.empty())
        return false;
    std::size_t const width = static_cast<std::size_t>(frame.width);
    std::size_t const height = static_cast<std::size_t>(frame.height);
    std::vector<std::uint8_t> indices;
    if (!lzw_decode(data, min_code_size, width * height, indices))
        return false;

    // Interlaced frames arrive in four passes of rows.
    std::vector<std::uint32_t> row_order;
    if (interlaced) {
        row_order.reserve(height);
        constexpr std::array<std::pair<int, int>, 4> passes { { { 0, 8 }, { 4, 8 }, { 2, 4 }, { 1, 2 } } };
        for (auto const& [start, step] : passes) {
            for (int row = start; row < frame.height; row += step)
                row_order.push_back(static_cast<std::uint32_t>(row));
        }
    }
    for (std::size_t i = 0; i < indices.size(); ++i) {
        std::uint32_t const decoded_row = static_cast<std::uint32_t>(i / width);
        std::uint32_t const row = interlaced ? row_order[decoded_row] : decoded_row;
        std::uint32_t const column = static_cast<std::uint32_t>(i % width);
        std::uint8_t const value = indices[i];
        // A transparent pixel leaves what is under it: that is how a frame
        // changes only part of the picture before it.
        if (static_cast<int>(value) == frame.transparent || value >= table.size())
            continue;
        int const x = frame.left + static_cast<int>(column);
        int const y = frame.top + static_cast<int>(row);
        if (canvas.contains(x, y))
            canvas.set_pixel(x, y, table[value]);
    }
    return true;
}

std::optional<Bitmap> decode_gif(std::vector<std::uint8_t> const& bytes, std::size_t max_pixels)
{
    std::optional<GifAnimation> const animation = scan_gif(bytes, max_pixels);
    if (!animation)
        return std::nullopt;
    Bitmap out(animation->width, animation->height, Color::rgba(0, 0, 0, 0));
    if (!draw_gif_frame(bytes, *animation, 0, out))
        return std::nullopt;
    return out;
}

}
