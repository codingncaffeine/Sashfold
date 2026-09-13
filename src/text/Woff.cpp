#include "text/Woff.h"

#include "core/Brotli.h"
#include "core/Inflate.h"

#include <algorithm>
#include <array>

namespace sashfold::text {

namespace {

constexpr std::uint32_t tag_woff = 0x774F4646u; // 'wOFF'
constexpr std::uint32_t tag_woff2 = 0x774F4632u; // 'wOF2'

std::uint32_t u32(std::vector<std::uint8_t> const& bytes, std::size_t at)
{
    return static_cast<std::uint32_t>(bytes[at]) << 24 | static_cast<std::uint32_t>(bytes[at + 1]) << 16
        | static_cast<std::uint32_t>(bytes[at + 2]) << 8 | static_cast<std::uint32_t>(bytes[at + 3]);
}

std::uint16_t u16(std::vector<std::uint8_t> const& bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(static_cast<unsigned>(bytes[at]) << 8 | bytes[at + 1]);
}

void push_u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void push_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    push_u16(out, static_cast<std::uint16_t>(value >> 16));
    push_u16(out, static_cast<std::uint16_t>(value));
}

} // namespace

bool is_woff(std::vector<std::uint8_t> const& bytes)
{
    return bytes.size() >= 4 && u32(bytes, 0) == tag_woff;
}

bool is_woff2(std::vector<std::uint8_t> const& bytes)
{
    return bytes.size() >= 4 && u32(bytes, 0) == tag_woff2;
}

std::optional<std::vector<std::uint8_t>> unwrap_woff(std::vector<std::uint8_t> const& bytes, std::size_t max_output)
{
    // The header: the signature, the sfnt's own version (its flavor), the
    // table count; the rest — lengths, metadata, private data — is not
    // needed to rebuild the font.
    if (!is_woff(bytes) || bytes.size() < 44)
        return std::nullopt;
    std::uint32_t const flavor = u32(bytes, 4);
    std::size_t const table_count = u16(bytes, 12);
    if (table_count == 0 || table_count > 512 || bytes.size() < 44 + table_count * 20)
        return std::nullopt;

    struct Entry {
        std::uint32_t tag;
        std::uint32_t offset;
        std::uint32_t compressed_length;
        std::uint32_t length;
        std::uint32_t checksum;
    };
    std::vector<Entry> entries;
    std::size_t total = 12 + table_count * 16;
    for (std::size_t i = 0; i < table_count; ++i) {
        std::size_t const at = 44 + i * 20;
        Entry const entry { u32(bytes, at), u32(bytes, at + 4), u32(bytes, at + 8), u32(bytes, at + 12),
            u32(bytes, at + 16) };
        if (entry.offset > bytes.size() || entry.compressed_length > bytes.size() - entry.offset
            || entry.compressed_length > entry.length)
            return std::nullopt;
        total += (static_cast<std::size_t>(entry.length) + 3) & ~static_cast<std::size_t>(3);
        if (total > max_output)
            return std::nullopt;
        entries.push_back(entry);
    }

    // The sfnt: its header and a directory with the offsets filled in as
    // the tables land, each inflated when it was compressed and 4-aligned.
    std::vector<std::uint8_t> out;
    out.reserve(total);
    push_u32(out, flavor);
    push_u16(out, static_cast<std::uint16_t>(table_count));
    std::uint16_t search_range = 16;
    std::uint16_t entry_selector = 0;
    while (search_range * 2 <= table_count * 16) {
        search_range = static_cast<std::uint16_t>(search_range * 2);
        ++entry_selector;
    }
    push_u16(out, search_range);
    push_u16(out, entry_selector);
    push_u16(out, static_cast<std::uint16_t>(table_count * 16 - search_range));
    std::size_t const directory = out.size();
    out.resize(directory + table_count * 16, 0);
    for (std::size_t i = 0; i < table_count; ++i) {
        Entry const& entry = entries[i];
        std::vector<std::uint8_t> const compressed(bytes.begin() + static_cast<std::ptrdiff_t>(entry.offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(entry.offset + entry.compressed_length));
        std::vector<std::uint8_t> table;
        if (entry.compressed_length < entry.length) {
            std::optional<std::vector<std::uint8_t>> inflated = zlib_decompress(compressed, entry.length);
            if (!inflated || inflated->size() != entry.length)
                return std::nullopt;
            table = std::move(*inflated);
        } else {
            table = compressed;
        }
        std::size_t const record = directory + i * 16;
        auto const put = [&](std::size_t at, std::uint32_t value) {
            out[at] = static_cast<std::uint8_t>(value >> 24);
            out[at + 1] = static_cast<std::uint8_t>(value >> 16);
            out[at + 2] = static_cast<std::uint8_t>(value >> 8);
            out[at + 3] = static_cast<std::uint8_t>(value);
        };
        put(record, entry.tag);
        put(record + 4, entry.checksum);
        put(record + 8, static_cast<std::uint32_t>(out.size()));
        put(record + 12, entry.length);
        out.insert(out.end(), table.begin(), table.end());
        while (out.size() % 4 != 0)
            out.push_back(0);
    }
    return out;
}

// --- WOFF 2.0 ------------------------------------------------------------------

namespace {

constexpr std::uint32_t make_tag(char const* name)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(name[0])) << 24
        | static_cast<std::uint32_t>(static_cast<unsigned char>(name[1])) << 16
        | static_cast<std::uint32_t>(static_cast<unsigned char>(name[2])) << 8
        | static_cast<std::uint32_t>(static_cast<unsigned char>(name[3]));
}

constexpr std::uint32_t tag_glyf = make_tag("glyf");
constexpr std::uint32_t tag_loca = make_tag("loca");
constexpr std::uint32_t tag_hmtx = make_tag("hmtx");
constexpr std::uint32_t tag_hhea = make_tag("hhea");
constexpr std::uint32_t tag_head = make_tag("head");
constexpr std::uint32_t tag_ttcf = make_tag("ttcf");

// The tags a WOFF2 table directory names by number (§4.1); 63 means the tag
// is spelled out after the flags.
constexpr std::array<std::uint32_t, 63> known_tags = {
    make_tag("cmap"), make_tag("head"), make_tag("hhea"), make_tag("hmtx"), make_tag("maxp"), make_tag("name"),
    make_tag("OS/2"), make_tag("post"), make_tag("cvt "), make_tag("fpgm"), make_tag("glyf"), make_tag("loca"),
    make_tag("prep"), make_tag("CFF "), make_tag("VORG"), make_tag("EBDT"), make_tag("EBLC"), make_tag("gasp"),
    make_tag("hdmx"), make_tag("kern"), make_tag("LTSH"), make_tag("PCLT"), make_tag("VDMX"), make_tag("vhea"),
    make_tag("vmtx"), make_tag("BASE"), make_tag("GDEF"), make_tag("GPOS"), make_tag("GSUB"), make_tag("EBSC"),
    make_tag("JSTF"), make_tag("MATH"), make_tag("CBDT"), make_tag("CBLC"), make_tag("COLR"), make_tag("CPAL"),
    make_tag("SVG "), make_tag("sbix"), make_tag("acnt"), make_tag("avar"), make_tag("bdat"), make_tag("bloc"),
    make_tag("bsln"), make_tag("cvar"), make_tag("fdsc"), make_tag("feat"), make_tag("fmtx"), make_tag("fvar"),
    make_tag("gvar"), make_tag("hsty"), make_tag("just"), make_tag("lcar"), make_tag("mort"), make_tag("morx"),
    make_tag("opbd"), make_tag("prop"), make_tag("trak"), make_tag("Zapf"), make_tag("Silf"), make_tag("Glat"),
    make_tag("Gloc"), make_tag("Feat"), make_tag("Sill"),
};

// Big-endian reads through a range that refuse to go past its end.
class Cursor {
public:
    Cursor() = default;
    Cursor(std::uint8_t const* data, std::size_t size)
        : m_data(data)
        , m_size(size)
    {
    }

    [[nodiscard]] bool u8(std::uint8_t& value)
    {
        if (m_at >= m_size)
            return false;
        value = m_data[m_at++];
        return true;
    }
    [[nodiscard]] bool u16(std::uint16_t& value)
    {
        if (m_size - m_at < 2)
            return false;
        value = static_cast<std::uint16_t>(static_cast<unsigned>(m_data[m_at]) << 8 | m_data[m_at + 1]);
        m_at += 2;
        return true;
    }
    [[nodiscard]] bool i16(std::int16_t& value)
    {
        std::uint16_t raw = 0;
        if (!u16(raw))
            return false;
        value = static_cast<std::int16_t>(raw);
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t& value)
    {
        std::uint16_t high = 0;
        std::uint16_t low = 0;
        if (m_size - m_at < 4 || !u16(high) || !u16(low))
            return false;
        value = static_cast<std::uint32_t>(high) << 16 | low;
        return true;
    }
    // UIntBase128 (§3.1): seven bits a byte, most significant first, at
    // most five bytes, no leading zero byte, nothing past 32 bits.
    [[nodiscard]] bool base128(std::uint32_t& value)
    {
        std::uint32_t accumulated = 0;
        for (int i = 0; i < 5; ++i) {
            std::uint8_t byte = 0;
            if (!u8(byte) || (i == 0 && byte == 0x80) || (accumulated & 0xFE000000u) != 0)
                return false;
            accumulated = accumulated << 7 | (byte & 0x7Fu);
            if ((byte & 0x80) == 0) {
                value = accumulated;
                return true;
            }
        }
        return false;
    }
    // 255UInt16 (§3.1): a byte below 253 is the value; 253 is followed by
    // a whole UInt16, 255 by a byte added to 253, 254 by one added to 506.
    [[nodiscard]] bool u255(std::uint16_t& value)
    {
        std::uint8_t code = 0;
        if (!u8(code))
            return false;
        if (code == 253)
            return u16(value);
        if (code == 254 || code == 255) {
            std::uint8_t more = 0;
            if (!u8(more))
                return false;
            value = static_cast<std::uint16_t>(more + (code == 255 ? 253 : 506));
            return true;
        }
        value = code;
        return true;
    }
    [[nodiscard]] bool bytes(std::size_t count, std::uint8_t const*& at)
    {
        if (count > m_size - m_at)
            return false;
        at = m_data + m_at;
        m_at += count;
        return true;
    }
    std::size_t offset() const { return m_at; }

private:
    std::uint8_t const* m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_at = 0;
};

void push_i16(std::vector<std::uint8_t>& out, int value)
{
    push_u16(out, static_cast<std::uint16_t>(value));
}

std::uint32_t checksum(std::vector<std::uint8_t> const& data)
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < data.size(); i += 4) {
        std::uint32_t word = 0;
        for (std::size_t k = 0; k < 4; ++k)
            word = word << 8 | (i + k < data.size() ? data[i + k] : 0u);
        sum += word;
    }
    return sum;
}

// A point's coordinate deltas from the triplet its flag chooses (§5.2): the
// flag's low seven bits say how many bytes follow and how their bits split
// between x and y, the lowest bit of that index is x's sign (set:
// positive) and the next y's.
[[nodiscard]] bool triplet(std::uint8_t flag, Cursor& glyphs, int& dx, int& dy)
{
    unsigned const index = flag & 0x7Fu;
    auto const signed_by = [](unsigned bits, unsigned magnitude) {
        return (bits & 1) ? static_cast<int>(magnitude) : -static_cast<int>(magnitude);
    };
    std::uint8_t b0 = 0;
    std::uint8_t b1 = 0;
    std::uint8_t b2 = 0;
    std::uint8_t b3 = 0;
    if (index < 10) {
        if (!glyphs.u8(b0))
            return false;
        dx = 0;
        dy = signed_by(index, ((index & 14u) << 7) + b0);
    } else if (index < 20) {
        if (!glyphs.u8(b0))
            return false;
        dx = signed_by(index, (((index - 10) & 14u) << 7) + b0);
        dy = 0;
    } else if (index < 84) {
        if (!glyphs.u8(b0))
            return false;
        unsigned const b = index - 20;
        dx = signed_by(index, 1 + (b & 0x30u) + (b0 >> 4u));
        dy = signed_by(index >> 1, 1 + ((b & 0x0Cu) << 2) + (b0 & 0x0Fu));
    } else if (index < 120) {
        if (!glyphs.u8(b0) || !glyphs.u8(b1))
            return false;
        unsigned const b = index - 84;
        dx = signed_by(index, 1 + ((b / 12) << 8) + b0);
        dy = signed_by(index >> 1, 1 + (((b % 12) >> 2) << 8) + b1);
    } else if (index < 124) {
        if (!glyphs.u8(b0) || !glyphs.u8(b1) || !glyphs.u8(b2))
            return false;
        dx = signed_by(index, (static_cast<unsigned>(b0) << 4) + (b1 >> 4u));
        dy = signed_by(index >> 1, ((b1 & 0x0Fu) << 8) + b2);
    } else {
        if (!glyphs.u8(b0) || !glyphs.u8(b1) || !glyphs.u8(b2) || !glyphs.u8(b3))
            return false;
        dx = signed_by(index, (static_cast<unsigned>(b0) << 8) + b1);
        dy = signed_by(index >> 1, (static_cast<unsigned>(b2) << 8) + b3);
    }
    return true;
}

// The glyf and loca tables rebuilt from a transformed glyf (§5.1, §5.3), and
// each glyph's xMin, which a transformed hmtx is rebuilt from.
struct Glyphs {
    std::vector<std::uint8_t> glyf;
    std::vector<std::uint8_t> loca;
    std::vector<std::int16_t> x_min;
};

std::optional<Glyphs> rebuild_glyphs(std::uint8_t const* data, std::size_t size, std::size_t loca_length,
    std::size_t max_output)
{
    Cursor header(data, size);
    std::uint16_t reserved = 0;
    std::uint16_t options = 0;
    std::uint16_t glyph_count = 0;
    std::uint16_t index_format = 0;
    if (!header.u16(reserved) || !header.u16(options) || !header.u16(glyph_count) || !header.u16(index_format))
        return std::nullopt;
    std::array<std::uint32_t, 7> sizes {};
    for (std::uint32_t& stream_size : sizes) {
        if (!header.u32(stream_size))
            return std::nullopt;
    }
    if (index_format > 1 || loca_length != (std::size_t { glyph_count } + 1) * (index_format ? 4 : 2))
        return std::nullopt;
    // The substreams follow the header one after another: contour counts,
    // point counts, point flags, coordinates and instruction lengths,
    // composite components, bounding boxes, instructions.
    std::array<Cursor, 7> streams;
    std::size_t at = header.offset();
    for (std::size_t i = 0; i < streams.size(); ++i) {
        if (sizes[i] > size - at)
            return std::nullopt;
        streams[i] = Cursor(data + at, sizes[i]);
        at += sizes[i];
    }
    Cursor& contours = streams[0];
    Cursor& point_counts = streams[1];
    Cursor& flags = streams[2];
    Cursor& coordinates = streams[3];
    Cursor& composites = streams[4];
    Cursor& boxes = streams[5];
    Cursor& instructions = streams[6];
    std::uint8_t const* overlap = nullptr;
    if (options & 1) {
        if ((std::size_t { glyph_count } + 7) / 8 > size - at)
            return std::nullopt;
        overlap = data + at;
    }
    std::uint8_t const* box_bits = nullptr;
    if (!boxes.bytes(4 * ((std::size_t { glyph_count } + 31) / 32), box_bits))
        return std::nullopt;
    auto const bit = [](std::uint8_t const* bits, std::size_t index) {
        return ((bits[index >> 3] >> (7 - (index & 7))) & 1u) != 0;
    };

    Glyphs out;
    out.x_min.assign(glyph_count, 0);
    std::vector<std::size_t> offsets(std::size_t { glyph_count } + 1, 0);
    for (std::size_t glyph = 0; glyph < glyph_count; ++glyph) {
        offsets[glyph] = out.glyf.size();
        std::int16_t contour_count = 0;
        if (!contours.i16(contour_count))
            return std::nullopt;
        bool const explicit_box = bit(box_bits, glyph);
        if (contour_count == 0) {
            if (explicit_box)
                return std::nullopt;
            continue;
        }
        std::array<std::int16_t, 4> box {};
        if (contour_count > 0) {
            std::vector<std::uint16_t> ends(static_cast<std::size_t>(contour_count));
            std::uint32_t total = 0;
            for (std::uint16_t& end : ends) {
                std::uint16_t count = 0;
                if (!point_counts.u255(count))
                    return std::nullopt;
                total += count;
                if (total == 0 || total > 0xFFFF)
                    return std::nullopt;
                end = static_cast<std::uint16_t>(total - 1);
            }
            struct Point {
                int x;
                int y;
                bool on_curve;
            };
            std::vector<Point> points(total);
            int x = 0;
            int y = 0;
            for (Point& point : points) {
                std::uint8_t flag = 0;
                int dx = 0;
                int dy = 0;
                if (!flags.u8(flag) || !triplet(flag, coordinates, dx, dy))
                    return std::nullopt;
                x += dx;
                y += dy;
                if (x < -32768 || x > 32767 || y < -32768 || y > 32767)
                    return std::nullopt;
                point = Point { x, y, (flag & 0x80) == 0 };
            }
            std::uint16_t instruction_length = 0;
            std::uint8_t const* code = nullptr;
            if (!coordinates.u255(instruction_length) || !instructions.bytes(instruction_length, code))
                return std::nullopt;
            if (explicit_box) {
                for (std::int16_t& value : box) {
                    if (!boxes.i16(value))
                        return std::nullopt;
                }
            } else {
                int min_x = points[0].x;
                int min_y = points[0].y;
                int max_x = points[0].x;
                int max_y = points[0].y;
                for (Point const& point : points) {
                    min_x = std::min(min_x, point.x);
                    min_y = std::min(min_y, point.y);
                    max_x = std::max(max_x, point.x);
                    max_y = std::max(max_y, point.y);
                }
                box = { static_cast<std::int16_t>(min_x), static_cast<std::int16_t>(min_y),
                    static_cast<std::int16_t>(max_x), static_cast<std::int16_t>(max_y) };
            }
            out.x_min[glyph] = box[0];
            // The record (OFF §5.3.3): contours, box, end points,
            // instructions, a flag per point, then the deltas each in its
            // shortest form.
            push_i16(out.glyf, contour_count);
            for (std::int16_t const value : box)
                push_i16(out.glyf, value);
            for (std::uint16_t const end : ends)
                push_u16(out.glyf, end);
            push_u16(out.glyf, instruction_length);
            out.glyf.insert(out.glyf.end(), code, code + instruction_length);
            std::vector<std::uint8_t> point_flags;
            std::vector<std::uint8_t> xs;
            std::vector<std::uint8_t> ys;
            int previous_x = 0;
            int previous_y = 0;
            for (std::size_t i = 0; i < points.size(); ++i) {
                std::uint8_t point_flag = points[i].on_curve ? 0x01 : 0x00;
                if (i == 0 && overlap && bit(overlap, glyph))
                    point_flag |= 0x40;
                int const dx = points[i].x - previous_x;
                int const dy = points[i].y - previous_y;
                if (dx < -32768 || dx > 32767 || dy < -32768 || dy > 32767)
                    return std::nullopt;
                if (dx == 0) {
                    point_flag |= 0x10;
                } else if (dx > -256 && dx < 256) {
                    point_flag |= dx > 0 ? 0x12 : 0x02;
                    xs.push_back(static_cast<std::uint8_t>(dx > 0 ? dx : -dx));
                } else {
                    push_i16(xs, dx);
                }
                if (dy == 0) {
                    point_flag |= 0x20;
                } else if (dy > -256 && dy < 256) {
                    point_flag |= dy > 0 ? 0x24 : 0x04;
                    ys.push_back(static_cast<std::uint8_t>(dy > 0 ? dy : -dy));
                } else {
                    push_i16(ys, dy);
                }
                point_flags.push_back(point_flag);
                previous_x = points[i].x;
                previous_y = points[i].y;
            }
            out.glyf.insert(out.glyf.end(), point_flags.begin(), point_flags.end());
            out.glyf.insert(out.glyf.end(), xs.begin(), xs.end());
            out.glyf.insert(out.glyf.end(), ys.begin(), ys.end());
        } else if (contour_count == -1) {
            // A composite: its components as they were, its box always
            // spelled out.
            if (!explicit_box)
                return std::nullopt;
            for (std::int16_t& value : box) {
                if (!boxes.i16(value))
                    return std::nullopt;
            }
            out.x_min[glyph] = box[0];
            push_i16(out.glyf, -1);
            for (std::int16_t const value : box)
                push_i16(out.glyf, value);
            bool has_instructions = false;
            for (bool more = true; more;) {
                std::uint16_t component = 0;
                if (!composites.u16(component))
                    return std::nullopt;
                std::size_t arguments = 2 + ((component & 0x0001) ? 4u : 2u);
                if (component & 0x0008)
                    arguments += 2;
                else if (component & 0x0040)
                    arguments += 4;
                else if (component & 0x0080)
                    arguments += 8;
                std::uint8_t const* argument_bytes = nullptr;
                if (!composites.bytes(arguments, argument_bytes))
                    return std::nullopt;
                push_u16(out.glyf, component);
                out.glyf.insert(out.glyf.end(), argument_bytes, argument_bytes + arguments);
                has_instructions = has_instructions || (component & 0x0100) != 0;
                more = (component & 0x0020) != 0;
            }
            if (has_instructions) {
                std::uint16_t instruction_length = 0;
                std::uint8_t const* code = nullptr;
                if (!coordinates.u255(instruction_length) || !instructions.bytes(instruction_length, code))
                    return std::nullopt;
                push_u16(out.glyf, instruction_length);
                out.glyf.insert(out.glyf.end(), code, code + instruction_length);
            }
        } else {
            return std::nullopt;
        }
        // Short offsets are halves, so a glyph ends on an even byte there.
        while (out.glyf.size() % (index_format ? 4 : 2) != 0)
            out.glyf.push_back(0);
        if (out.glyf.size() > max_output)
            return std::nullopt;
    }
    offsets[glyph_count] = out.glyf.size();
    if (index_format == 0 && out.glyf.size() > 0x1FFFE)
        return std::nullopt;
    for (std::size_t const offset : offsets) {
        if (index_format == 0)
            push_u16(out.loca, static_cast<std::uint16_t>(offset / 2));
        else
            push_u32(out.loca, static_cast<std::uint32_t>(offset));
    }
    return out;
}

// The hmtx table rebuilt from a transformed one (§5.4): the advances, and
// the side bearings either given or taken from each glyph's xMin.
std::optional<std::vector<std::uint8_t>> rebuild_hmtx(std::uint8_t const* data, std::size_t size,
    std::size_t metric_count, std::vector<std::int16_t> const& x_min)
{
    Cursor in(data, size);
    std::uint8_t flags = 0;
    if (!in.u8(flags) || (flags & 0xFC) != 0 || (flags & 3) == 0)
        return std::nullopt;
    std::size_t const glyph_count = x_min.size();
    if (metric_count == 0 || metric_count > glyph_count)
        return std::nullopt;
    std::vector<std::uint16_t> advances(metric_count);
    for (std::uint16_t& advance : advances) {
        if (!in.u16(advance))
            return std::nullopt;
    }
    std::vector<std::int16_t> bearings(glyph_count);
    for (std::size_t glyph = 0; glyph < glyph_count; ++glyph) {
        bool const derived = glyph < metric_count ? (flags & 1) != 0 : (flags & 2) != 0;
        if (derived)
            bearings[glyph] = x_min[glyph];
        else if (!in.i16(bearings[glyph]))
            return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    for (std::size_t glyph = 0; glyph < glyph_count; ++glyph) {
        if (glyph < metric_count)
            push_u16(out, advances[glyph]);
        push_i16(out, bearings[glyph]);
    }
    return out;
}

} // namespace

std::optional<std::vector<std::uint8_t>> unwrap_woff2(std::vector<std::uint8_t> const& bytes, std::size_t max_output)
{
    // The header (§3.2). Of it the flavor and the table count rebuild the
    // font, and the compressed size says where the stream ends; metadata
    // and private data are not needed.
    if (!is_woff2(bytes) || bytes.size() < 48)
        return std::nullopt;
    Cursor in(bytes.data(), bytes.size());
    std::uint32_t signature = 0;
    std::uint32_t flavor = 0;
    std::uint32_t file_length = 0;
    std::uint16_t table_count = 0;
    std::uint16_t reserved = 0;
    std::uint32_t sfnt_size = 0;
    std::uint32_t compressed_size = 0;
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::array<std::uint32_t, 5> blocks {};
    if (!in.u32(signature) || !in.u32(flavor) || !in.u32(file_length) || !in.u16(table_count) || !in.u16(reserved)
        || !in.u32(sfnt_size) || !in.u32(compressed_size) || !in.u16(major) || !in.u16(minor))
        return std::nullopt;
    for (std::uint32_t& block : blocks) {
        if (!in.u32(block))
            return std::nullopt;
    }
    // The file must be exactly as long as it says: a file cut short in its
    // padding or its metadata still holds a whole stream, and is refused
    // the way the reference decoder refuses it.
    if (table_count == 0 || table_count > 512 || file_length != bytes.size())
        return std::nullopt;

    // The table directory (§4.1), in the order the tables lie in the stream.
    struct Table {
        std::uint32_t tag = 0;
        bool transformed = false;
        std::size_t source = 0;
        std::size_t source_length = 0;
        std::size_t original_length = 0;
        std::vector<std::uint8_t> data;
        bool ready = false;
    };
    std::vector<Table> tables(table_count);
    std::size_t stream_length = 0;
    for (Table& table : tables) {
        std::uint8_t flags = 0;
        std::uint32_t original = 0;
        if (!in.u8(flags) || ((flags & 63) == 63 ? !in.u32(table.tag) : false))
            return std::nullopt;
        if ((flags & 63) != 63)
            table.tag = known_tags[flags & 63];
        unsigned const version = flags >> 6;
        if (!in.base128(original))
            return std::nullopt;
        // glyf and loca are transformed under version 0 and passed through
        // under 3; hmtx is transformed under 1; nothing else has a transform.
        if (table.tag == tag_glyf || table.tag == tag_loca) {
            if (version != 0 && version != 3)
                return std::nullopt;
            table.transformed = version == 0;
        } else if (table.tag == tag_hmtx) {
            if (version > 1)
                return std::nullopt;
            table.transformed = version == 1;
        } else if (version != 0) {
            return std::nullopt;
        }
        std::uint32_t transformed_length = 0;
        if (table.transformed && !in.base128(transformed_length))
            return std::nullopt;
        if (table.tag == tag_loca && table.transformed && transformed_length != 0)
            return std::nullopt;
        table.original_length = original;
        table.source = stream_length;
        table.source_length = table.transformed ? transformed_length : original;
        if (table.source_length > max_output - stream_length)
            return std::nullopt;
        stream_length += table.source_length;
    }

    // The collection directory (§4.2), or the one font every table belongs to.
    struct Font {
        std::uint32_t flavor = 0;
        std::vector<std::uint16_t> tables;
    };
    std::vector<Font> fonts;
    bool const collection = flavor == tag_ttcf;
    if (collection) {
        std::uint32_t version = 0;
        std::uint16_t font_count = 0;
        if (!in.u32(version) || !in.u255(font_count) || font_count == 0)
            return std::nullopt;
        fonts.resize(font_count);
        for (Font& font : fonts) {
            std::uint16_t count = 0;
            if (!in.u255(count) || count == 0 || !in.u32(font.flavor))
                return std::nullopt;
            font.tables.resize(count);
            for (std::uint16_t& index : font.tables) {
                if (!in.u255(index) || index >= table_count)
                    return std::nullopt;
            }
        }
    } else {
        Font font;
        font.flavor = flavor;
        for (std::uint16_t index = 0; index < table_count; ++index)
            font.tables.push_back(index);
        fonts.push_back(std::move(font));
    }

    // One brotli stream holds every table, and nothing else (§5).
    std::size_t const stream_at = in.offset();
    if (compressed_size > bytes.size() - stream_at)
        return std::nullopt;
    std::optional<std::vector<std::uint8_t>> const stream
        = brotli_decompress(bytes.data() + stream_at, compressed_size, stream_length);
    if (!stream || stream->size() != stream_length)
        return std::nullopt;

    auto const find = [&](Font const& font, std::uint32_t tag) -> std::optional<std::size_t> {
        for (std::uint16_t const index : font.tables) {
            if (tables[index].tag == tag)
                return index;
        }
        return std::nullopt;
    };
    // glyf with its loca, once per pair however many fonts share it; then
    // hmtx, from the glyphs' xMin and hhea's count of metrics.
    std::vector<std::vector<std::int16_t>> x_mins(table_count);
    for (Font const& font : fonts) {
        std::optional<std::size_t> const glyf = find(font, tag_glyf);
        std::optional<std::size_t> const loca = find(font, tag_loca);
        bool const glyf_transformed = glyf && tables[*glyf].transformed;
        bool const loca_transformed = loca && tables[*loca].transformed;
        if (glyf_transformed != loca_transformed)
            return std::nullopt;
        if (!glyf_transformed || tables[*glyf].ready)
            continue;
        if (tables[*loca].ready)
            return std::nullopt;
        std::optional<Glyphs> rebuilt = rebuild_glyphs(stream->data() + tables[*glyf].source,
            tables[*glyf].source_length, tables[*loca].original_length, max_output);
        if (!rebuilt)
            return std::nullopt;
        tables[*glyf].data = std::move(rebuilt->glyf);
        tables[*glyf].ready = true;
        tables[*loca].data = std::move(rebuilt->loca);
        tables[*loca].ready = true;
        x_mins[*glyf] = std::move(rebuilt->x_min);
    }
    for (Font const& font : fonts) {
        std::optional<std::size_t> const hmtx = find(font, tag_hmtx);
        if (!hmtx || !tables[*hmtx].transformed)
            continue;
        std::optional<std::size_t> const glyf = find(font, tag_glyf);
        std::optional<std::size_t> const hhea = find(font, tag_hhea);
        if (!glyf || !tables[*glyf].transformed || !hhea || tables[*hhea].source_length < 36)
            return std::nullopt;
        std::size_t const at = tables[*hhea].source + 34;
        std::size_t const metric_count = static_cast<std::size_t>((*stream)[at]) << 8 | (*stream)[at + 1];
        std::optional<std::vector<std::uint8_t>> rebuilt = rebuild_hmtx(stream->data() + tables[*hmtx].source,
            tables[*hmtx].source_length, metric_count, x_mins[*glyf]);
        if (!rebuilt || (tables[*hmtx].ready && tables[*hmtx].data != *rebuilt))
            return std::nullopt;
        tables[*hmtx].data = std::move(*rebuilt);
        tables[*hmtx].ready = true;
    }
    for (Table& table : tables) {
        if (table.ready)
            continue;
        if (table.transformed)
            return std::nullopt;
        table.data.assign(stream->begin() + static_cast<std::ptrdiff_t>(table.source),
            stream->begin() + static_cast<std::ptrdiff_t>(table.source + table.source_length));
        table.ready = true;
        if (table.tag == tag_head && table.data.size() >= 12)
            std::fill(table.data.begin() + 8, table.data.begin() + 12, std::uint8_t { 0 });
    }

    // The sfnt, or for a collection the TTC header and one offset table per
    // font over shared table data: directories sorted by tag, every table's
    // checksum recomputed, tables 4-aligned.
    auto const padded = [](std::size_t length) { return (length + 3) & ~std::size_t { 3 }; };
    std::size_t header_size = collection ? 12 + 4 * fonts.size() : 0;
    for (Font const& font : fonts)
        header_size += 12 + 16 * font.tables.size();
    std::vector<std::size_t> table_offsets(table_count);
    std::size_t data_at = padded(header_size);
    for (std::size_t i = 0; i < tables.size(); ++i) {
        table_offsets[i] = data_at;
        data_at += padded(tables[i].data.size());
        if (data_at > max_output)
            return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    out.reserve(data_at);
    if (collection) {
        push_u32(out, tag_ttcf);
        push_u16(out, 1);
        push_u16(out, 0);
        push_u32(out, static_cast<std::uint32_t>(fonts.size()));
        std::size_t font_at = 12 + 4 * fonts.size();
        for (Font const& font : fonts) {
            push_u32(out, static_cast<std::uint32_t>(font_at));
            font_at += 12 + 16 * font.tables.size();
        }
    }
    for (Font const& font : fonts) {
        std::vector<std::uint16_t> order = font.tables;
        std::sort(order.begin(), order.end(),
            [&](std::uint16_t a, std::uint16_t b) { return tables[a].tag < tables[b].tag; });
        for (std::size_t i = 1; i < order.size(); ++i) {
            if (tables[order[i]].tag == tables[order[i - 1]].tag)
                return std::nullopt;
        }
        std::size_t const count = order.size();
        push_u32(out, font.flavor);
        push_u16(out, static_cast<std::uint16_t>(count));
        std::uint16_t search_range = 16;
        std::uint16_t entry_selector = 0;
        while (search_range * 2u <= count * 16) {
            search_range = static_cast<std::uint16_t>(search_range * 2);
            ++entry_selector;
        }
        push_u16(out, search_range);
        push_u16(out, entry_selector);
        push_u16(out, static_cast<std::uint16_t>(count * 16 - search_range));
        for (std::uint16_t const index : order) {
            Table const& table = tables[index];
            push_u32(out, table.tag);
            push_u32(out, checksum(table.data));
            push_u32(out, static_cast<std::uint32_t>(table_offsets[index]));
            push_u32(out, static_cast<std::uint32_t>(table.data.size()));
        }
    }
    while (out.size() % 4 != 0)
        out.push_back(0);
    for (Table const& table : tables) {
        out.insert(out.end(), table.data.begin(), table.data.end());
        while (out.size() % 4 != 0)
            out.push_back(0);
    }
    // A single font's head carries the whole file's checksum adjustment.
    if (!collection) {
        for (std::size_t i = 0; i < tables.size(); ++i) {
            if (tables[i].tag != tag_head || tables[i].data.size() < 12)
                continue;
            std::uint32_t const adjustment = 0xB1B0AFBAu - checksum(out);
            std::size_t const at = table_offsets[i] + 8;
            out[at] = static_cast<std::uint8_t>(adjustment >> 24);
            out[at + 1] = static_cast<std::uint8_t>(adjustment >> 16);
            out[at + 2] = static_cast<std::uint8_t>(adjustment >> 8);
            out[at + 3] = static_cast<std::uint8_t>(adjustment);
        }
    }
    return out;
}

}
