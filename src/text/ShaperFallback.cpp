// What the shaper does for a font that lacks the tables: kerning from the
// `kern` table when GPOS does not kern, and marks placed from glyph
// extents when there is no GPOS at all — centered over or under their
// base by their combining class (Unicode §3.11's placement classes), each
// stacked clear of the ones placed before it.

#include "text/ShaperInternal.h"
#include "text/TrueType.h"

#include <algorithm>
#include <optional>

namespace sashfold::text::shaping {

namespace {

// --- The `kern` table ------------------------------------------------------------

// One horizontal subtable's adjustment for a pair: formats 0 (pairs) and
// 2 (class arrays) of the OpenType and the older Apple header.
std::int32_t kern_value(otl::Bytes const& b, std::uint32_t at, std::uint8_t format, std::uint32_t header,
    std::uint32_t left, std::uint32_t right)
{
    std::uint32_t const body = at + header;
    if (format == 0) {
        std::uint32_t const pairs = b.u16(body);
        std::uint32_t const key = left << 16 | right;
        std::uint32_t lo = 0;
        std::uint32_t hi = std::min<std::uint32_t>(pairs, b.size / 6);
        while (lo < hi) {
            std::uint32_t const mid = (lo + hi) / 2;
            std::uint32_t const record = body + 8 + mid * 6;
            std::uint32_t const k = b.u32(record);
            if (key < k)
                hi = mid;
            else if (key > k)
                lo = mid + 1;
            else
                return b.has(record, 6) ? b.i16(record + 4) : 0;
        }
        return 0;
    }
    if (format == 2) {
        // rowWidth, leftClassTable, rightClassTable, array: offsets from the
        // subtable's start; the class values are premultiplied offsets.
        auto class_of = [&](std::uint32_t table, std::uint32_t glyph) -> std::uint32_t {
            if (table == 0)
                return 0;
            std::uint32_t const first = b.u16(at + table);
            std::uint32_t const count = b.u16(at + table + 2);
            if (glyph < first || glyph - first >= count)
                return 0;
            return b.u16(at + table + 4 + (glyph - first) * 2);
        };
        std::uint32_t const left_class = class_of(b.u16(body + 2), left);
        std::uint32_t const right_class = class_of(b.u16(body + 4), right);
        std::uint32_t const array = b.u16(body + 6);
        if (left_class == 0 && right_class == 0)
            return 0;
        std::uint32_t const value_at = at + left_class + right_class;
        if (left_class < array || !b.has(value_at, 2))
            return 0;
        return b.i16(value_at);
    }
    return 0;
}

}

void apply_kern_table(Plan const& plan, ShapeFont const& font, Buffer& b)
{
    otl::Bytes const& k = font.tables.kern;
    if (plan.kern_mask == 0 || k.size < 4)
        return;
    bool const apple = k.u32(0) == 0x00010000;
    std::uint32_t const tables = apple ? k.u32(4) : k.u16(2);
    std::uint32_t at = apple ? 8 : 4;
    for (std::uint32_t t = 0; t < tables && t < 64 && at < k.size; ++t) {
        std::uint32_t length;
        std::uint8_t format;
        bool horizontal;
        bool cross_stream;
        std::uint32_t header;
        if (apple) {
            length = k.u32(at);
            std::uint16_t const coverage = k.u16(at + 4);
            format = static_cast<std::uint8_t>(coverage & 0xFF);
            horizontal = !(coverage & 0x8000) && !(coverage & 0x2000);
            cross_stream = (coverage & 0x4000) != 0;
            header = 8;
        } else {
            length = k.u16(at + 2);
            std::uint16_t const coverage = k.u16(at + 4);
            format = static_cast<std::uint8_t>(coverage >> 8);
            horizontal = (coverage & 0x01) != 0;
            cross_stream = (coverage & 0x04) != 0;
            header = 6;
        }
        std::uint32_t const subtable = at;
        if (length < header)
            break;
        at += length;
        if (!horizontal || (format != 0 && format != 2))
            continue;
        // Pairs of glyphs side by side, stepping over marks; the adjustment
        // is shared between the two, as kerning from this table always has
        // been when marks may sit between.
        for (std::size_t i = 0; i < b.len(); ++i) {
            if (!(b.info[i].mask & plan.kern_mask))
                continue;
            std::size_t j = i + 1;
            while (j < b.len() && (otl::is_mark(b.info[j]) || otl::is_default_ignorable(b.info[j])))
                ++j;
            if (j >= b.len() || !(b.info[j].mask & plan.kern_mask))
                continue;
            std::int32_t const kern = kern_value(k, subtable, format, header, b.info[i].glyph, b.info[j].glyph);
            if (kern == 0)
                continue;
            if (cross_stream) {
                b.pos[j].y_offset = kern;
                continue;
            }
            std::int32_t const first = kern >> 1;
            std::int32_t const second = kern - first;
            b.pos[i].x_advance += first;
            b.pos[j].x_advance += second;
            b.pos[j].x_offset += second;
            i = j - 1;
        }
    }
}

// --- Marks without GPOS --------------------------------------------------------

namespace {

// The placement classes of Unicode's canonical combining classes (§3.11).
enum : std::uint8_t {
    attached_below_left = 200,
    attached_below = 202,
    attached_above = 214,
    attached_above_right = 216,
    below_left = 218,
    below = 220,
    below_right = 222,
    left = 224,
    right = 226,
    above_left = 228,
    above = 230,
    above_right = 232,
    double_below = 233,
    double_above = 234,
};

// The fixed-position classes of the scripts whose marks Unicode numbers by
// mark rather than by place, as the place each is drawn in. The values are
// the shaping classes (Shaper.cpp's order).
std::uint8_t placement_class(char32_t c, std::uint8_t klass)
{
    if (klass >= 200)
        return klass;
    if ((c & ~0xFFu) == 0x0E00) {
        if (klass == 0) {
            switch (c) {
            case 0x0E31: case 0x0E34: case 0x0E35: case 0x0E36: case 0x0E37: case 0x0E47: case 0x0E4C: case 0x0E4D:
            case 0x0E4E:
                return above_right;
            case 0x0EB1: case 0x0EB4: case 0x0EB5: case 0x0EB6: case 0x0EB7: case 0x0EBB: case 0x0ECC: case 0x0ECD:
                return above;
            case 0x0EBC:
                return below;
            default:
                break;
            }
        } else if (c == 0x0E3A) {
            return below_right; // Thai phinthu
        }
    }
    switch (klass) {
    // Hebrew (shaping classes): the vowels and meteg below; rafe on top;
    // the shin dot right, the sin dot and holam left; varika above.
    case 22: case 15: case 16: case 17: case 23: case 18: case 19: case 20: case 21: case 24: case 25:
        return below;
    case 13:
        return attached_above;
    case 10:
        return above_right;
    case 11: case 14:
        return above_left;
    case 26:
        return above;
    case 12:
        return klass; // dagesh sits inside the letter: left where it is
    // Arabic and Syriac: the kasras below, the rest above.
    case 28: case 29: case 31: case 32: case 27: case 34: case 35: case 36:
        return above;
    case 30: case 33:
        return below;
    // Thai, Lao, Tibetan.
    case 3:
        return below_right;
    case 107:
        return above_right;
    case 118:
        return below;
    case 122:
        return above;
    case 129:
        return below;
    case 132:
        return above;
    case 131:
        return below;
    default:
        return klass;
    }
}

struct Extents {
    std::int32_t x_bearing = 0;
    std::int32_t y_bearing = 0; // top, y up
    std::int32_t width = 0;
    std::int32_t height = 0; // negative: from the top down
};

std::optional<Extents> extents_of(ShapeFont const& font, std::uint32_t glyph)
{
    if (glyph > 0xFFFF)
        return std::nullopt;
    std::optional<TrueTypeFont::GlyphBox> const box = font.font.glyph_box(static_cast<std::uint16_t>(glyph));
    if (!box)
        return Extents {};
    return Extents { box->x_min, box->y_max, box->x_max - box->x_min, box->y_min - box->y_max };
}

void place_mark(ShapeFont const& font, Buffer& b, Extents& base, std::size_t i, std::uint8_t klass)
{
    std::optional<Extents> const found = extents_of(font, b.info[i].glyph);
    if (!found)
        return;
    Extents const mark = *found;
    std::int32_t const gap = font.font.units_per_em() / 16;
    GlyphPosition& p = b.pos[i];
    p.x_offset = 0;
    p.y_offset = 0;
    // Across: centered, or to the base's left or right edge.
    switch (klass) {
    case attached_below_left:
    case below_left:
    case above_left:
        p.x_offset += base.x_bearing - mark.x_bearing;
        break;
    case attached_above_right:
    case below_right:
    case above_right:
        p.x_offset += base.x_bearing + base.width - mark.width - mark.x_bearing;
        break;
    case double_below:
    case double_above:
        if (!b.right_to_left) {
            p.x_offset += base.x_bearing + base.width - mark.width / 2 - mark.x_bearing;
            break;
        }
        p.x_offset += base.x_bearing - mark.width / 2 - mark.x_bearing;
        break;
    default:
        p.x_offset += base.x_bearing + (base.width - mark.width) / 2 - mark.x_bearing;
        break;
    }
    // Up and down: below the base (and what is already below it), or above.
    switch (klass) {
    case double_below:
    case below_left:
    case below:
    case below_right:
        base.height -= gap;
        [[fallthrough]];
    case attached_below_left:
    case attached_below:
        p.y_offset = base.y_bearing + base.height - mark.y_bearing;
        // A below mark is never moved up.
        if ((gap > 0) == (p.y_offset > 0)) {
            base.height -= p.y_offset;
            p.y_offset = 0;
        }
        base.height += mark.height;
        break;
    case double_above:
    case above_left:
    case above:
    case above_right:
        base.y_bearing += gap;
        base.height -= gap;
        [[fallthrough]];
    case attached_above:
    case attached_above_right:
        p.y_offset = base.y_bearing - (mark.y_bearing + mark.height);
        // Nor an above mark moved far down.
        if ((gap > 0) != (p.y_offset > 0)) {
            std::int32_t const correction = -p.y_offset / 2;
            base.y_bearing += correction;
            base.height -= correction;
            p.y_offset += correction;
        }
        base.y_bearing -= mark.height;
        base.height += mark.height;
        break;
    default:
        break;
    }
}

void place_around_base(ShapeFont const& font, Buffer& b, std::size_t base, std::size_t end, bool adjust_offsets)
{
    std::optional<Extents> found = extents_of(font, b.info[base].glyph);
    if (!found) {
        for (std::size_t i = base + 1; i < end; ++i) {
            if (b.info[i].combining_class) {
                if (adjust_offsets)
                    b.pos[i].x_offset -= b.pos[i].x_advance;
                b.pos[i].x_advance = 0;
                b.pos[i].y_advance = 0;
            }
        }
        return;
    }
    Extents base_extents = *found;
    base_extents.y_bearing += b.pos[base].y_offset;
    base_extents.x_bearing = 0;
    // The advance, not the ink, is what marks center over: it works for
    // glyphs with no ink too.
    base_extents.width = font.advance(b.info[base].glyph);
    unsigned const id = otl::lig_id(b.info[base]);
    int const components = static_cast<int>(otl::lig_num_comps(b.info[base]));
    std::int32_t x_offset = 0;
    std::int32_t y_offset = 0;
    if (!b.right_to_left) {
        x_offset -= b.pos[base].x_advance;
        y_offset -= b.pos[base].y_advance;
    }
    Extents component = base_extents;
    int last_component = -1;
    unsigned last_class = 255;
    Extents cluster = base_extents;
    for (std::size_t i = base + 1; i < end; ++i) {
        std::uint8_t const klass = b.info[i].combining_class;
        if (!klass) {
            if (!b.right_to_left) {
                x_offset -= b.pos[i].x_advance;
                y_offset -= b.pos[i].y_advance;
            } else {
                x_offset += b.pos[i].x_advance;
                y_offset += b.pos[i].y_advance;
            }
            continue;
        }
        if (components > 1) {
            // A mark on a ligature goes over the component it sat on.
            int this_component = static_cast<int>(otl::lig_comp(b.info[i])) - 1;
            if (!id || id != otl::lig_id(b.info[i]) || this_component >= components || this_component < 0)
                this_component = components - 1;
            if (last_component != this_component) {
                last_component = this_component;
                last_class = 255;
                component = base_extents;
                if (!b.right_to_left)
                    component.x_bearing += (this_component * component.width) / components;
                else
                    component.x_bearing += ((components - 1 - this_component) * component.width) / components;
                component.width /= components;
            }
        }
        if (last_class != klass) {
            last_class = klass;
            cluster = component;
        }
        place_mark(font, b, cluster, i, klass);
        b.pos[i].x_advance = 0;
        b.pos[i].y_advance = 0;
        b.pos[i].x_offset += x_offset;
        b.pos[i].y_offset += y_offset;
    }
}

}

void fallback_recategorize_marks(Plan const&, Buffer& b)
{
    for (GlyphInfo& g : b.info) {
        if (static_cast<GeneralCategory>(g.category) != GeneralCategory::Mn)
            continue;
        std::uint8_t klass = g.combining_class;
        // Arabic marks UTR #53 moved to the front sort as below or above.
        if (klass == 1)
            klass = below;
        else if (klass == 2)
            klass = above;
        g.combining_class = placement_class(g.codepoint, klass);
    }
}

void fallback_position_marks(Plan const&, ShapeFont const& font, Buffer& b, bool adjust_offsets)
{
    // Each base with the marks after it, mark by mark. A default ignorable
    // (a joiner, a soft hyphen) between them draws nothing and keeps the
    // marks on the base before it.
    auto continues = [&](GlyphInfo const& g) { return is_unicode_mark(g) || otl::is_default_ignorable(g); };
    std::size_t start = 0;
    auto place_cluster = [&](std::size_t from, std::size_t to) {
        if (to - from < 2)
            return;
        for (std::size_t i = from; i < to; ++i) {
            if (continues(b.info[i]))
                continue;
            std::size_t j = i + 1;
            while (j < to && continues(b.info[j]))
                ++j;
            place_around_base(font, b, i, j, adjust_offsets);
            i = j - 1;
        }
    };
    for (std::size_t i = 1; i < b.len(); ++i) {
        if (!continues(b.info[i])) {
            place_cluster(start, i);
            start = i;
        }
    }
    place_cluster(start, b.len());
}

}
