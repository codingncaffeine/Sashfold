#include "text/TrueTypeWriter.h"

#include <algorithm>
#include <array>
#include <map>

namespace sashfold::text {

namespace {

constexpr std::uint32_t make_tag(char a, char b, char c, char d)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24
        | static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16
        | static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8
        | static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

// Big-endian byte building.
struct Buffer {
    std::vector<std::uint8_t> bytes;

    std::size_t size() const { return bytes.size(); }
    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u16(std::uint16_t value)
    {
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    void i16(std::int16_t value) { u16(static_cast<std::uint16_t>(value)); }
    void u32(std::uint32_t value)
    {
        u16(static_cast<std::uint16_t>(value >> 16));
        u16(static_cast<std::uint16_t>(value));
    }
    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void tag(char a, char b, char c, char d) { u32(make_tag(a, b, c, d)); }
    void ascii(std::string const& text)
    {
        for (char const c : text)
            bytes.push_back(static_cast<std::uint8_t>(c));
    }
    void append(std::vector<std::uint8_t> const& more)
    {
        bytes.insert(bytes.end(), more.begin(), more.end());
    }
    void pad4()
    {
        while (bytes.size() % 4 != 0)
            bytes.push_back(0);
    }
    void u16_at(std::size_t at, std::uint16_t value)
    {
        bytes[at] = static_cast<std::uint8_t>(value >> 8);
        bytes[at + 1] = static_cast<std::uint8_t>(value);
    }
    void u32_at(std::size_t at, std::uint32_t value)
    {
        u16_at(at, static_cast<std::uint16_t>(value >> 16));
        u16_at(at + 2, static_cast<std::uint16_t>(value));
    }
};

std::uint32_t checksum(std::vector<std::uint8_t> const& bytes)
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        std::uint32_t word = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            word <<= 8;
            if (i + j < bytes.size())
                word |= bytes[i + j];
        }
        sum += word;
    }
    return sum;
}

struct Box {
    std::int16_t x_min = 0;
    std::int16_t y_min = 0;
    std::int16_t x_max = 0;
    std::int16_t y_max = 0;
    bool empty = true;

    void add(long long x, long long y)
    {
        auto const clamp = [](long long value) {
            return static_cast<std::int16_t>(std::clamp<long long>(value, -32768, 32767));
        };
        if (empty) {
            x_min = x_max = clamp(x);
            y_min = y_max = clamp(y);
            empty = false;
            return;
        }
        x_min = std::min(x_min, clamp(x));
        x_max = std::max(x_max, clamp(x));
        y_min = std::min(y_min, clamp(y));
        y_max = std::max(y_max, clamp(y));
    }
};

// Flattened extents of a glyph, composites included (depth-limited so a
// self-referencing description cannot recurse forever).
struct Extents {
    Box box;
    std::size_t points = 0;
    std::size_t contours = 0;
    int depth = 0;
};

Extents extents_of(FontDescription const& font, std::uint16_t index, int depth, long long dx,
    long long dy)
{
    Extents result;
    if (depth > 8 || index >= font.glyphs.size())
        return result;
    WriterGlyph const& glyph = font.glyphs[index];
    if (glyph.components.empty()) {
        for (GlyphPoint const& point : glyph.outline.points)
            result.box.add(point.x + dx, point.y + dy);
        result.points = glyph.outline.points.size();
        result.contours = glyph.outline.contour_ends.size();
        return result;
    }
    for (WriterComponent const& component : glyph.components) {
        Extents const child = extents_of(font, component.glyph, depth + 1, dx + component.dx,
            dy + component.dy);
        if (!child.box.empty) {
            result.box.add(child.box.x_min, child.box.y_min);
            result.box.add(child.box.x_max, child.box.y_max);
        }
        result.points += child.points;
        result.contours += child.contours;
        result.depth = std::max(result.depth, child.depth + 1);
    }
    return result;
}

void write_box(Buffer& out, Box const& box)
{
    out.i16(box.x_min);
    out.i16(box.y_min);
    out.i16(box.x_max);
    out.i16(box.y_max);
}

std::vector<std::uint8_t> encode_simple_glyph(GlyphOutline const& outline, Box const& box)
{
    Buffer out;
    if (outline.contour_ends.empty() || outline.points.empty())
        return {};
    out.i16(static_cast<std::int16_t>(outline.contour_ends.size()));
    write_box(out, box);
    for (std::uint16_t const end : outline.contour_ends)
        out.u16(end);
    out.u16(0); // no instructions

    std::size_t const count = outline.points.size();
    std::vector<std::uint8_t> flags(count);
    std::vector<long long> dxs(count);
    std::vector<long long> dys(count);
    long long previous_x = 0;
    long long previous_y = 0;
    for (std::size_t i = 0; i < count; ++i) {
        GlyphPoint const& point = outline.points[i];
        long long const dx = point.x - previous_x;
        long long const dy = point.y - previous_y;
        previous_x = point.x;
        previous_y = point.y;
        dxs[i] = dx;
        dys[i] = dy;
        std::uint8_t flag = point.on_curve ? 0x01 : 0x00;
        if (dx == 0)
            flag |= 0x10;
        else if (dx > -256 && dx < 256)
            flag |= static_cast<std::uint8_t>(0x02 | (dx > 0 ? 0x10 : 0x00));
        if (dy == 0)
            flag |= 0x20;
        else if (dy > -256 && dy < 256)
            flag |= static_cast<std::uint8_t>(0x04 | (dy > 0 ? 0x20 : 0x00));
        flags[i] = flag;
    }
    // Flags, run-length packed with the repeat bit.
    for (std::size_t i = 0; i < count;) {
        std::size_t run = 1;
        while (i + run < count && flags[i + run] == flags[i] && run < 256)
            ++run;
        if (run == 1) {
            out.u8(flags[i]);
        } else {
            out.u8(static_cast<std::uint8_t>(flags[i] | 0x08));
            out.u8(static_cast<std::uint8_t>(run - 1));
        }
        i += run;
    }
    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t const flag = flags[i];
        if (flag & 0x02)
            out.u8(static_cast<std::uint8_t>(dxs[i] < 0 ? -dxs[i] : dxs[i]));
        else if (!(flag & 0x10))
            out.i16(static_cast<std::int16_t>(dxs[i]));
    }
    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t const flag = flags[i];
        if (flag & 0x04)
            out.u8(static_cast<std::uint8_t>(dys[i] < 0 ? -dys[i] : dys[i]));
        else if (!(flag & 0x20))
            out.i16(static_cast<std::int16_t>(dys[i]));
    }
    return std::move(out.bytes);
}

std::vector<std::uint8_t> encode_composite_glyph(std::vector<WriterComponent> const& components,
    Box const& box)
{
    Buffer out;
    out.i16(-1);
    write_box(out, box);
    for (std::size_t i = 0; i < components.size(); ++i) {
        WriterComponent const& component = components[i];
        bool const words = component.dx < -128 || component.dx > 127 || component.dy < -128
            || component.dy > 127;
        std::uint16_t flags = 0x0002 | 0x0004; // ARGS_ARE_XY_VALUES | ROUND_XY_TO_GRID
        if (words)
            flags |= 0x0001; // ARG_1_AND_2_ARE_WORDS
        if (i + 1 < components.size())
            flags |= 0x0020; // MORE_COMPONENTS
        out.u16(flags);
        out.u16(component.glyph);
        if (words) {
            out.i16(component.dx);
            out.i16(component.dy);
        } else {
            out.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(component.dx)));
            out.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(component.dy)));
        }
    }
    return std::move(out.bytes);
}

// The OS/2 Unicode range bits this writer knows how to claim.
struct UnicodeRange {
    int bit;
    char32_t first;
    char32_t last;
};
constexpr std::array<UnicodeRange, 18> unicode_ranges { {
    { 0, 0x0000, 0x007F }, { 1, 0x0080, 0x00FF }, { 2, 0x0100, 0x017F }, { 3, 0x0180, 0x024F },
    { 4, 0x0250, 0x02AF }, { 6, 0x0300, 0x036F }, { 7, 0x0370, 0x03FF }, { 9, 0x0400, 0x04FF },
    { 29, 0x1E00, 0x1EFF }, { 31, 0x2000, 0x206F }, { 32, 0x2070, 0x209F }, { 33, 0x20A0, 0x20CF },
    { 35, 0x2100, 0x214F }, { 37, 0x2190, 0x21FF }, { 38, 0x2200, 0x22FF }, { 45, 0x25A0, 0x25FF },
    { 46, 0x2600, 0x26FF }, { 68, 0xFF00, 0xFFEF },
} };

void append_utf16(Buffer& out, std::string const& text)
{
    // Family names here are ASCII; anything else is spelled as U+FFFD
    // rather than mis-decoded.
    for (char const c : text) {
        auto const byte = static_cast<unsigned char>(c);
        out.u16(byte < 0x80 ? byte : 0xFFFD);
    }
}

std::string without_spaces(std::string const& text)
{
    std::string result;
    for (char const c : text) {
        if (c != ' ')
            result.push_back(c);
    }
    return result;
}

} // namespace

std::vector<std::uint8_t> write_truetype(FontDescription const& font)
{
    if (font.glyphs.empty() || font.glyphs.size() > 65535)
        return {};
    std::size_t const glyph_count = font.glyphs.size();

    // --- glyf + loca -----------------------------------------------------------
    std::vector<std::uint32_t> offsets;
    std::vector<Box> boxes(glyph_count);
    Buffer glyf;
    Box font_box;
    std::size_t max_points = 0;
    std::size_t max_contours = 0;
    std::size_t max_composite_points = 0;
    std::size_t max_composite_contours = 0;
    std::size_t max_components = 0;
    int max_depth = 0;
    for (std::size_t i = 0; i < glyph_count; ++i) {
        WriterGlyph const& glyph = font.glyphs[i];
        offsets.push_back(static_cast<std::uint32_t>(glyf.size()));
        Extents const extents = extents_of(font, static_cast<std::uint16_t>(i), 0, 0, 0);
        boxes[i] = extents.box;
        if (!extents.box.empty) {
            font_box.add(extents.box.x_min, extents.box.y_min);
            font_box.add(extents.box.x_max, extents.box.y_max);
        }
        if (glyph.components.empty()) {
            glyf.append(encode_simple_glyph(glyph.outline, extents.box));
            max_points = std::max(max_points, extents.points);
            max_contours = std::max(max_contours, extents.contours);
        } else {
            glyf.append(encode_composite_glyph(glyph.components, extents.box));
            max_composite_points = std::max(max_composite_points, extents.points);
            max_composite_contours = std::max(max_composite_contours, extents.contours);
            max_components = std::max(max_components, glyph.components.size());
            max_depth = std::max(max_depth, extents.depth);
        }
        glyf.pad4();
    }
    offsets.push_back(static_cast<std::uint32_t>(glyf.size()));
    bool const long_loca = font.long_loca || glyf.size() > 0x1FFFE;
    Buffer loca;
    for (std::uint32_t const offset : offsets) {
        if (long_loca)
            loca.u32(offset);
        else
            loca.u16(static_cast<std::uint16_t>(offset / 2));
    }

    // --- hmtx + hhea -------------------------------------------------------------
    std::size_t metric_count = glyph_count;
    while (metric_count > 1
        && font.glyphs[metric_count - 1].advance == font.glyphs[metric_count - 2].advance)
        --metric_count;
    Buffer hmtx;
    std::uint16_t advance_max = 0;
    std::int16_t min_lsb = 0;
    std::int16_t min_rsb = 0;
    std::int16_t x_max_extent = 0;
    bool first_ink = true;
    long long advance_total = 0;
    for (std::size_t i = 0; i < glyph_count; ++i) {
        std::uint16_t const advance = font.glyphs[i].advance;
        std::int16_t const lsb = boxes[i].empty ? 0 : boxes[i].x_min;
        if (i < metric_count)
            hmtx.u16(advance);
        hmtx.i16(lsb);
        advance_max = std::max(advance_max, advance);
        advance_total += advance;
        if (boxes[i].empty)
            continue;
        auto const rsb = static_cast<std::int16_t>(
            std::clamp<long long>(static_cast<long long>(advance) - boxes[i].x_max, -32768, 32767));
        if (first_ink) {
            min_lsb = lsb;
            min_rsb = rsb;
            x_max_extent = boxes[i].x_max;
            first_ink = false;
        } else {
            min_lsb = std::min(min_lsb, lsb);
            min_rsb = std::min(min_rsb, rsb);
            x_max_extent = std::max(x_max_extent, boxes[i].x_max);
        }
    }
    Buffer hhea;
    hhea.u32(0x00010000);
    hhea.i16(font.ascender);
    hhea.i16(font.descender);
    hhea.i16(font.line_gap);
    hhea.u16(advance_max);
    hhea.i16(min_lsb);
    hhea.i16(min_rsb);
    hhea.i16(x_max_extent);
    hhea.i16(1); // caretSlopeRise
    hhea.i16(0); // caretSlopeRun
    hhea.i16(0); // caretOffset
    for (int i = 0; i < 4; ++i)
        hhea.i16(0);
    hhea.i16(0); // metricDataFormat
    hhea.u16(static_cast<std::uint16_t>(metric_count));

    // --- head ---------------------------------------------------------------------
    bool const bold = font.weight_class >= 600;
    Buffer head;
    head.u32(0x00010000); // version
    head.u32(0x00010000); // fontRevision
    head.u32(0); // checksumAdjustment, patched once the file is whole
    head.u32(0x5F0F3CF5); // magic
    head.u16(0x000B); // baseline at y=0, lsb at x=0, integer scaling
    head.u16(font.units_per_em);
    head.u32(0); // created: no timestamps, the bytes must be reproducible
    head.u32(0);
    head.u32(0); // modified
    head.u32(0);
    write_box(head, font_box);
    head.u16(static_cast<std::uint16_t>((bold ? 0x0001 : 0) | (font.italic ? 0x0002 : 0)));
    head.u16(8); // lowestRecPPEM
    head.i16(2); // fontDirectionHint
    head.i16(long_loca ? 1 : 0);
    head.i16(0); // glyphDataFormat

    // --- maxp ---------------------------------------------------------------------
    Buffer maxp;
    maxp.u32(0x00010000);
    maxp.u16(static_cast<std::uint16_t>(glyph_count));
    maxp.u16(static_cast<std::uint16_t>(max_points));
    maxp.u16(static_cast<std::uint16_t>(max_contours));
    maxp.u16(static_cast<std::uint16_t>(max_composite_points));
    maxp.u16(static_cast<std::uint16_t>(max_composite_contours));
    maxp.u16(2); // maxZones
    maxp.u16(0); // maxTwilightPoints
    maxp.u16(0); // maxStorage
    maxp.u16(0); // maxFunctionDefs
    maxp.u16(0); // maxInstructionDefs
    maxp.u16(0); // maxStackElements
    maxp.u16(0); // maxSizeOfInstructions
    maxp.u16(static_cast<std::uint16_t>(max_components));
    maxp.u16(static_cast<std::uint16_t>(max_depth));

    // --- cmap ---------------------------------------------------------------------
    std::map<char32_t, std::uint16_t> mapping;
    for (auto const& [code_point, glyph] : font.mappings) {
        if (code_point > 0x10FFFF || glyph == 0 || glyph >= glyph_count)
            continue;
        mapping.emplace(code_point, glyph); // the first mapping of a code point wins
    }
    std::array<std::uint32_t, 4> range_bits { 0, 0, 0, 0 };
    char32_t first_bmp = 0xFFFF;
    char32_t last_bmp = 0;
    for (auto const& [code_point, glyph] : mapping) {
        for (UnicodeRange const& range : unicode_ranges) {
            if (code_point >= range.first && code_point <= range.last)
                range_bits[static_cast<std::size_t>(range.bit / 32)] |= 1u << (range.bit % 32);
        }
        if (code_point <= 0xFFFF) {
            first_bmp = std::min(first_bmp, code_point);
            last_bmp = std::max(last_bmp, code_point);
        }
    }

    // Format 4: segments of consecutive BMP code points; a segment whose
    // glyphs also run consecutively uses idDelta, the others the glyph array.
    struct Segment {
        char32_t first;
        char32_t last;
        std::vector<std::uint16_t> glyphs; // empty for a delta segment
        std::uint16_t delta;
    };
    std::vector<Segment> segments;
    {
        std::vector<std::pair<char32_t, std::uint16_t>> bmp;
        for (auto const& [code_point, glyph] : mapping) {
            if (code_point < 0xFFFF)
                bmp.emplace_back(code_point, glyph);
        }
        for (std::size_t i = 0; i < bmp.size();) {
            std::size_t j = i + 1;
            while (j < bmp.size() && bmp[j].first == bmp[j - 1].first + 1)
                ++j;
            bool consecutive = true;
            for (std::size_t k = i + 1; k < j; ++k)
                consecutive = consecutive && bmp[k].second == bmp[k - 1].second + 1;
            Segment segment { bmp[i].first, bmp[j - 1].first, {}, 0 };
            if (consecutive) {
                segment.delta = static_cast<std::uint16_t>(
                    (static_cast<std::uint32_t>(bmp[i].second) - bmp[i].first) & 0xFFFFu);
            } else {
                for (std::size_t k = i; k < j; ++k)
                    segment.glyphs.push_back(bmp[k].second);
            }
            segments.push_back(std::move(segment));
            i = j;
        }
        segments.push_back(Segment { 0xFFFF, 0xFFFF, {}, 1 });
    }
    Buffer format4;
    {
        std::size_t const segment_count = segments.size();
        std::size_t array_size = 0;
        for (Segment const& segment : segments)
            array_size += segment.glyphs.size();
        std::size_t const length = 16 + segment_count * 8 + array_size * 2;
        if (length <= 0xFFFF) {
            std::size_t search_range = 1;
            std::uint16_t entry_selector = 0;
            while (search_range * 2 <= segment_count) {
                search_range *= 2;
                ++entry_selector;
            }
            format4.u16(4);
            format4.u16(static_cast<std::uint16_t>(length));
            format4.u16(0); // language
            format4.u16(static_cast<std::uint16_t>(segment_count * 2));
            format4.u16(static_cast<std::uint16_t>(search_range * 2));
            format4.u16(entry_selector);
            format4.u16(static_cast<std::uint16_t>(segment_count * 2 - search_range * 2));
            for (Segment const& segment : segments)
                format4.u16(static_cast<std::uint16_t>(segment.last));
            format4.u16(0); // reservedPad
            for (Segment const& segment : segments)
                format4.u16(static_cast<std::uint16_t>(segment.first));
            for (Segment const& segment : segments)
                format4.u16(segment.delta);
            std::size_t array_index = 0;
            for (std::size_t i = 0; i < segment_count; ++i) {
                Segment const& segment = segments[i];
                if (segment.glyphs.empty()) {
                    format4.u16(0);
                } else {
                    format4.u16(static_cast<std::uint16_t>((segment_count - i) * 2 + array_index * 2));
                    array_index += segment.glyphs.size();
                }
            }
            for (Segment const& segment : segments) {
                for (std::uint16_t const glyph : segment.glyphs)
                    format4.u16(glyph);
            }
        }
    }
    Buffer format12;
    {
        struct Group {
            char32_t first;
            char32_t last;
            std::uint16_t glyph;
        };
        std::vector<Group> groups;
        for (auto const& [code_point, glyph] : mapping) {
            if (!groups.empty() && groups.back().last + 1 == code_point
                && groups.back().glyph + (code_point - groups.back().first) == glyph) {
                groups.back().last = code_point;
                continue;
            }
            groups.push_back(Group { code_point, code_point, glyph });
        }
        format12.u16(12);
        format12.u16(0);
        format12.u32(static_cast<std::uint32_t>(16 + groups.size() * 12));
        format12.u32(0); // language
        format12.u32(static_cast<std::uint32_t>(groups.size()));
        for (Group const& group : groups) {
            format12.u32(group.first);
            format12.u32(group.last);
            format12.u32(group.glyph);
        }
    }
    Buffer cmap;
    {
        bool const have4 = format4.size() != 0;
        std::uint16_t const record_count = have4 ? 3 : 2;
        std::uint32_t const format12_at = 4u + record_count * 8u + static_cast<std::uint32_t>(format4.size());
        std::uint32_t const format4_at = 4u + record_count * 8u;
        cmap.u16(0);
        cmap.u16(record_count);
        cmap.u16(0); // Unicode platform, full repertoire
        cmap.u16(4);
        cmap.u32(format12_at);
        if (have4) {
            cmap.u16(3); // Windows, Unicode BMP
            cmap.u16(1);
            cmap.u32(format4_at);
        }
        cmap.u16(3); // Windows, Unicode full repertoire
        cmap.u16(10);
        cmap.u32(format12_at);
        cmap.append(format4.bytes);
        cmap.append(format12.bytes);
    }

    // --- OS/2 -------------------------------------------------------------------------
    Buffer os2;
    {
        auto const scaled = [&](long long per_mille) {
            return static_cast<std::int16_t>(font.units_per_em * per_mille / 1000);
        };
        os2.u16(4); // version
        os2.i16(static_cast<std::int16_t>(advance_total / static_cast<long long>(glyph_count)));
        os2.u16(font.weight_class);
        os2.u16(5); // usWidthClass: medium
        os2.u16(0); // fsType: installable
        os2.i16(scaled(650)); // subscript x size
        os2.i16(scaled(600)); // subscript y size
        os2.i16(0);
        os2.i16(scaled(75));
        os2.i16(scaled(650)); // superscript x size
        os2.i16(scaled(600));
        os2.i16(0);
        os2.i16(scaled(350));
        os2.i16(scaled(50)); // strikeout size
        os2.i16(scaled(259)); // strikeout position
        os2.i16(0); // sFamilyClass
        std::array<std::uint8_t, 10> const panose { 2, 0, 0,
            static_cast<std::uint8_t>(font.fixed_pitch ? 9 : 0), 0, 0, 0, 0, 0, 0 };
        for (std::uint8_t const byte : panose)
            os2.u8(byte);
        for (std::uint32_t const bits : range_bits)
            os2.u32(bits);
        os2.ascii("SASH");
        std::uint16_t selection = 0x0080; // USE_TYPO_METRICS
        if (font.italic)
            selection |= 0x0001;
        if (bold)
            selection |= 0x0020;
        if (!font.italic && !bold)
            selection |= 0x0040; // REGULAR
        os2.u16(selection);
        os2.u16(static_cast<std::uint16_t>(mapping.empty() ? 0 : first_bmp));
        os2.u16(static_cast<std::uint16_t>(mapping.empty() ? 0 : last_bmp));
        os2.i16(font.ascender);
        os2.i16(font.descender);
        os2.i16(font.line_gap);
        os2.u16(static_cast<std::uint16_t>(std::max<int>(font.ascender, font_box.y_max)));
        os2.u16(static_cast<std::uint16_t>(std::max<int>(-font.descender, -font_box.y_min)));
        os2.u32(1); // ulCodePageRange1: Latin 1
        os2.u32(0);
        os2.i16(font.x_height);
        os2.i16(font.cap_height);
        os2.u16(0); // usDefaultChar
        os2.u16(0x20); // usBreakChar
        os2.u16(1); // usMaxContext
    }

    // --- name ------------------------------------------------------------------------
    Buffer name;
    {
        std::string const full = font.subfamily == "Regular" ? font.family
                                                             : font.family + " " + font.subfamily;
        std::vector<std::pair<std::uint16_t, std::string>> const strings {
            { 0, font.family + " is an original face of the Sashfold project" },
            { 1, font.family },
            { 2, font.subfamily },
            { 3, font.family + " " + font.subfamily },
            { 4, full },
            { 5, font.version },
            { 6, without_spaces(font.family) + "-" + without_spaces(font.subfamily) },
        };
        Buffer storage;
        struct Record {
            std::uint16_t platform, encoding, language, id, length, offset;
        };
        std::vector<Record> records;
        for (auto const& [id, text] : strings) { // Macintosh Roman first: platform order
            auto const offset = static_cast<std::uint16_t>(storage.size());
            storage.ascii(text);
            records.push_back(Record { 1, 0, 0, id, static_cast<std::uint16_t>(text.size()), offset });
        }
        for (auto const& [id, text] : strings) {
            auto const offset = static_cast<std::uint16_t>(storage.size());
            append_utf16(storage, text);
            records.push_back(
                Record { 3, 1, 0x0409, id, static_cast<std::uint16_t>(text.size() * 2), offset });
        }
        name.u16(0);
        name.u16(static_cast<std::uint16_t>(records.size()));
        name.u16(static_cast<std::uint16_t>(6 + records.size() * 12));
        for (Record const& record : records) {
            name.u16(record.platform);
            name.u16(record.encoding);
            name.u16(record.language);
            name.u16(record.id);
            name.u16(record.length);
            name.u16(record.offset);
        }
        name.append(storage.bytes);
    }

    // --- post ----------------------------------------------------------------------------
    Buffer post;
    post.u32(0x00030000); // no glyph names
    post.i32(0); // italicAngle
    post.i16(static_cast<std::int16_t>(-static_cast<int>(font.units_per_em) * 75 / 1000));
    post.i16(static_cast<std::int16_t>(font.units_per_em * 50 / 1000));
    post.u32(font.fixed_pitch ? 1 : 0);
    post.u32(0);
    post.u32(0);
    post.u32(0);
    post.u32(0);

    // --- the file: directory, then the tables in tag order -----------------------------
    struct Entry {
        std::uint32_t tag;
        std::vector<std::uint8_t> const* data;
    };
    std::vector<Entry> const entries {
        { make_tag('O', 'S', '/', '2'), &os2.bytes }, { make_tag('c', 'm', 'a', 'p'), &cmap.bytes },
        { make_tag('g', 'l', 'y', 'f'), &glyf.bytes }, { make_tag('h', 'e', 'a', 'd'), &head.bytes },
        { make_tag('h', 'h', 'e', 'a'), &hhea.bytes }, { make_tag('h', 'm', 't', 'x'), &hmtx.bytes },
        { make_tag('l', 'o', 'c', 'a'), &loca.bytes }, { make_tag('m', 'a', 'x', 'p'), &maxp.bytes },
        { make_tag('n', 'a', 'm', 'e'), &name.bytes }, { make_tag('p', 'o', 's', 't'), &post.bytes },
    };
    Buffer file;
    auto const table_count = static_cast<std::uint16_t>(entries.size());
    std::uint16_t search_range = 16;
    std::uint16_t entry_selector = 0;
    while (search_range * 2 <= table_count * 16) {
        search_range = static_cast<std::uint16_t>(search_range * 2);
        ++entry_selector;
    }
    file.u32(0x00010000);
    file.u16(table_count);
    file.u16(search_range);
    file.u16(entry_selector);
    file.u16(static_cast<std::uint16_t>(table_count * 16 - search_range));
    std::size_t offset = 12 + static_cast<std::size_t>(table_count) * 16;
    std::size_t head_offset = 0;
    for (Entry const& entry : entries) {
        file.u32(entry.tag);
        file.u32(checksum(*entry.data));
        file.u32(static_cast<std::uint32_t>(offset));
        file.u32(static_cast<std::uint32_t>(entry.data->size()));
        if (entry.tag == make_tag('h', 'e', 'a', 'd'))
            head_offset = offset;
        offset += (entry.data->size() + 3) & ~static_cast<std::size_t>(3);
    }
    for (Entry const& entry : entries) {
        file.append(*entry.data);
        file.pad4();
    }
    file.u32_at(head_offset + 8, 0xB1B0AFBAu - checksum(file.bytes));
    return std::move(file.bytes);
}

std::vector<std::uint8_t> write_collection(std::vector<std::vector<std::uint8_t>> const& fonts)
{
    struct Directory {
        std::vector<std::array<std::uint32_t, 4>> records; // tag, checksum, offset, length
    };
    std::vector<Directory> directories;
    for (std::vector<std::uint8_t> const& font : fonts) {
        Directory directory;
        if (font.size() < 12)
            return {};
        std::size_t const table_count = static_cast<std::size_t>(font[4]) << 8 | font[5];
        if (font.size() < 12 + table_count * 16)
            return {};
        auto const u32 = [&](std::size_t at) {
            return static_cast<std::uint32_t>(font[at]) << 24 | static_cast<std::uint32_t>(font[at + 1]) << 16
                | static_cast<std::uint32_t>(font[at + 2]) << 8 | static_cast<std::uint32_t>(font[at + 3]);
        };
        for (std::size_t i = 0; i < table_count; ++i) {
            std::size_t const record = 12 + i * 16;
            std::array<std::uint32_t, 4> const entry { u32(record), u32(record + 4), u32(record + 8),
                u32(record + 12) };
            if (entry[2] > font.size() || entry[3] > font.size() - entry[2])
                return {};
            directory.records.push_back(entry);
        }
        directories.push_back(std::move(directory));
    }

    Buffer file;
    file.tag('t', 't', 'c', 'f');
    file.u32(0x00010000);
    file.u32(static_cast<std::uint32_t>(fonts.size()));
    std::size_t offset = 12 + fonts.size() * 4;
    for (Directory const& directory : directories) {
        file.u32(static_cast<std::uint32_t>(offset));
        offset += 12 + directory.records.size() * 16;
    }
    // Every table lands after all the directories, 4-aligned, in file order.
    std::size_t table_offset = offset;
    for (std::size_t f = 0; f < fonts.size(); ++f) {
        Directory const& directory = directories[f];
        auto const table_count = static_cast<std::uint16_t>(directory.records.size());
        std::uint16_t search_range = 16;
        std::uint16_t entry_selector = 0;
        while (search_range * 2 <= table_count * 16) {
            search_range = static_cast<std::uint16_t>(search_range * 2);
            ++entry_selector;
        }
        file.u32(0x00010000);
        file.u16(table_count);
        file.u16(search_range);
        file.u16(entry_selector);
        file.u16(static_cast<std::uint16_t>(table_count * 16 - search_range));
        for (auto const& record : directory.records) {
            file.u32(record[0]);
            file.u32(record[1]);
            file.u32(static_cast<std::uint32_t>(table_offset));
            file.u32(record[3]);
            table_offset += (static_cast<std::size_t>(record[3]) + 3) & ~static_cast<std::size_t>(3);
        }
    }
    for (std::size_t f = 0; f < fonts.size(); ++f) {
        for (auto const& record : directories[f].records) {
            file.bytes.insert(file.bytes.end(), fonts[f].begin() + record[2],
                fonts[f].begin() + record[2] + record[3]);
            file.pad4();
        }
    }
    return std::move(file.bytes);
}

}
