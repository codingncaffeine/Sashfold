#include "text/Cff.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

namespace sashfold::text {

namespace {

struct Reader {
    std::vector<std::uint8_t> const& bytes;
    std::size_t end; // the table's end, not the file's

    bool has(std::size_t offset, std::size_t length) const
    {
        return offset <= end && length <= end - offset;
    }
    std::uint8_t u8(std::size_t at) const { return has(at, 1) ? bytes[at] : 0; }
    std::uint16_t u16(std::size_t at) const
    {
        return static_cast<std::uint16_t>(static_cast<unsigned>(u8(at)) << 8 | u8(at + 1));
    }
    std::uint32_t u24(std::size_t at) const
    {
        return static_cast<std::uint32_t>(u8(at)) << 16 | static_cast<std::uint32_t>(u8(at + 1)) << 8 | u8(at + 2);
    }
    std::uint32_t u32(std::size_t at) const
    {
        return static_cast<std::uint32_t>(u8(at)) << 24 | static_cast<std::uint32_t>(u8(at + 1)) << 16
            | static_cast<std::uint32_t>(u8(at + 2)) << 8 | u8(at + 3);
    }
    std::uint32_t sized(std::size_t at, std::uint8_t size) const
    {
        switch (size) {
        case 1: return u8(at);
        case 2: return u16(at);
        case 3: return u24(at);
        default: return u32(at);
        }
    }
};

// A DICT: operators with the operands that came before each. Only the
// operators this reader uses are kept; a two-byte operator is 1200 + its
// second byte.
struct DictEntry {
    int op;
    std::vector<double> operands;
};

std::vector<DictEntry> parse_dict(Reader const& reader, std::size_t from, std::size_t length)
{
    std::vector<DictEntry> entries;
    std::vector<double> operands;
    std::size_t const to = from + length;
    std::size_t pos = from;
    while (pos < to && reader.has(pos, 1)) {
        std::uint8_t const b0 = reader.u8(pos);
        if (b0 <= 21) {
            int op = b0;
            ++pos;
            if (b0 == 12) {
                op = 1200 + reader.u8(pos);
                ++pos;
            }
            entries.push_back(DictEntry { op, operands });
            operands.clear();
            if (entries.size() > 256)
                break;
        } else if (b0 == 28) {
            operands.push_back(static_cast<std::int16_t>(reader.u16(pos + 1)));
            pos += 3;
        } else if (b0 == 29) {
            operands.push_back(static_cast<std::int32_t>(reader.u32(pos + 1)));
            pos += 5;
        } else if (b0 == 30) {
            // A real number, in nibbles: digits, a point, E and E-, and f to end.
            std::string text;
            ++pos;
            bool done = false;
            while (!done && reader.has(pos, 1) && text.size() < 64) {
                std::uint8_t const byte = reader.u8(pos++);
                for (std::uint8_t nibble : { static_cast<std::uint8_t>(byte >> 4), static_cast<std::uint8_t>(byte & 0xF) }) {
                    if (nibble <= 9)
                        text += static_cast<char>('0' + nibble);
                    else if (nibble == 0xA)
                        text += '.';
                    else if (nibble == 0xB)
                        text += 'E';
                    else if (nibble == 0xC)
                        text += "E-";
                    else if (nibble == 0xE)
                        text += '-';
                    else if (nibble == 0xF) {
                        done = true;
                        break;
                    }
                }
            }
            double const value = text.empty() ? 0.0 : std::strtod(text.c_str(), nullptr);
            operands.push_back(std::isfinite(value) ? value : 0.0);
        } else if (b0 >= 32 && b0 <= 246) {
            operands.push_back(static_cast<int>(b0) - 139);
            ++pos;
        } else if (b0 >= 247 && b0 <= 250) {
            operands.push_back((static_cast<int>(b0) - 247) * 256 + reader.u8(pos + 1) + 108);
            pos += 2;
        } else if (b0 >= 251 && b0 <= 254) {
            operands.push_back(-(static_cast<int>(b0) - 251) * 256 - reader.u8(pos + 1) - 108);
            pos += 2;
        } else {
            ++pos; // 22-27 and 255 are reserved
        }
        if (operands.size() > 48)
            operands.erase(operands.begin());
    }
    return entries;
}

std::vector<double> const* dict_get(std::vector<DictEntry> const& dict, int op)
{
    for (DictEntry const& entry : dict) {
        if (entry.op == op)
            return &entry.operands;
    }
    return nullptr;
}

// The Standard Encoding (CFF Appendix B): a code's standard string id, 0
// where the code has no name. What endchar's accent composition speaks in.
std::uint16_t standard_encoding_sid(std::uint8_t code)
{
    if (code >= 32 && code <= 126)
        return static_cast<std::uint16_t>(code - 31);
    static constexpr std::array<std::uint16_t, 95> upper = {
        // 161..255
        96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, // 161-175
        0, 111, 112, 113, 114, // 176-180
        0, 115, 116, 117, 118, 119, 120, 121, 122, // 181-189
        0, 123, // 190-191
        0, 124, 125, 126, 127, 128, 129, 130, 131, // 192-200
        0, 132, 133, // 201-203
        0, 134, 135, 136, 137, // 204-208
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 209-224
        138, // 225
        0, 139, // 226-227
        0, 0, 0, 0, // 228-231
        140, 141, 142, 143, // 232-235
        0, 0, 0, 0, 0, // 236-240
        144, // 241
        0, 0, 0, // 242-244
        145, // 245
        0, 0, // 246-247
        146, 147, 148, 149, // 248-251
        0, 0, 0, 0 // 252-255
    };
    if (code >= 161)
        return upper[static_cast<std::size_t>(code - 161)];
    return 0;
}

constexpr std::size_t max_points = 20000;
constexpr int max_instructions = 200000;
constexpr int max_depth = 10;

} // namespace

// The Type 2 charstring interpreter: one glyph's program, run over the
// font's subroutines, leaving contours in the outline.
struct Type2 {
    CffFont const& font;
    Reader reader;
    GlyphOutline& out;
    CffFont::Private const& priv;
    double stack[48] = {};
    int sp = 0;
    double x = 0;
    double y = 0;
    int stem_count = 0;
    bool width_seen = false;
    bool open = false;
    double start_x = 0;
    double start_y = 0;
    int instructions = 0;
    double transient[32] = {};
    bool finished = false;

    // --- The contour under construction ------------------------------------------

    static std::int16_t clamp16(double v)
    {
        // The arithmetic operators can overflow a value to infinity: that
        // is not a coordinate, and rounding it is undefined.
        if (!std::isfinite(v))
            return 0;
        return static_cast<std::int16_t>(std::clamp(std::lround(std::clamp(v, -40000.0, 40000.0)), -32768L, 32767L));
    }

    bool add_point(double px, double py, bool on_curve)
    {
        if (out.points.size() >= max_points)
            return false;
        GlyphPoint const point { clamp16(px), clamp16(py), on_curve };
        // A repeated on-curve point adds nothing to the contour.
        if (on_curve && !out.points.empty() && open) {
            GlyphPoint const& last = out.points.back();
            if (last.on_curve && last.x == point.x && last.y == point.y)
                return true;
        }
        out.points.push_back(point);
        return true;
    }

    void close_contour()
    {
        if (!open)
            return;
        open = false;
        std::size_t const first = out.contour_ends.empty() ? 0 : out.contour_ends.back() + 1u;
        // The closing edge back to the start is implied; a contour that
        // ended on its own start point has that point twice.
        if (out.points.size() > first + 1) {
            GlyphPoint const& last = out.points.back();
            GlyphPoint const& head = out.points[first];
            if (last.on_curve && last.x == head.x && last.y == head.y)
                out.points.pop_back();
        }
        if (out.points.size() <= first + 1) {
            out.points.resize(first); // a contour of one point draws nothing
            return;
        }
        out.contour_ends.push_back(static_cast<std::uint16_t>(out.points.size() - 1));
    }

    bool move_to(double dx, double dy)
    {
        close_contour();
        x += dx;
        y += dy;
        start_x = x;
        start_y = y;
        open = true;
        return add_point(x, y, true);
    }

    bool line_to(double dx, double dy)
    {
        if (!open && !move_to(0, 0))
            return false;
        x += dx;
        y += dy;
        return add_point(x, y, true);
    }

    // A cubic as quadratics: split until each piece is within a unit of
    // the quadratic through its ends and their mid-control (the error of
    // that approximation is bounded by the distance between the two
    // candidate controls over about twenty).
    bool quadratics_for(double p0x, double p0y, double c1x, double c1y, double c2x, double c2y, double p3x,
        double p3y, int depth)
    {
        double const q1x = 3 * c1x - p0x;
        double const q1y = 3 * c1y - p0y;
        double const q2x = 3 * c2x - p3x;
        double const q2y = 3 * c2y - p3y;
        double const dx = q1x - q2x;
        double const dy = q1y - q2y;
        double const error = std::sqrt(dx * dx + dy * dy) / 20.78;
        if (error <= 1.0 || depth >= 4) {
            double const cx = (q1x + q2x) / 4;
            double const cy = (q1y + q2y) / 4;
            return add_point(cx, cy, false) && add_point(p3x, p3y, true);
        }
        // de Casteljau at the midpoint.
        double const m01x = (p0x + c1x) / 2, m01y = (p0y + c1y) / 2;
        double const m12x = (c1x + c2x) / 2, m12y = (c1y + c2y) / 2;
        double const m23x = (c2x + p3x) / 2, m23y = (c2y + p3y) / 2;
        double const a_x = (m01x + m12x) / 2, a_y = (m01y + m12y) / 2;
        double const b_x = (m12x + m23x) / 2, b_y = (m12y + m23y) / 2;
        double const mx = (a_x + b_x) / 2, my = (a_y + b_y) / 2;
        return quadratics_for(p0x, p0y, m01x, m01y, a_x, a_y, mx, my, depth + 1)
            && quadratics_for(mx, my, b_x, b_y, m23x, m23y, p3x, p3y, depth + 1);
    }

    bool curve_to(double dx1, double dy1, double dx2, double dy2, double dx3, double dy3)
    {
        if (!open && !move_to(0, 0))
            return false;
        double const c1x = x + dx1;
        double const c1y = y + dy1;
        double const c2x = c1x + dx2;
        double const c2y = c1y + dy2;
        double const px = c2x + dx3;
        double const py = c2y + dy3;
        bool const ok = quadratics_for(x, y, c1x, c1y, c2x, c2y, px, py, 0);
        x = px;
        y = py;
        return ok;
    }

    // --- The width ---------------------------------------------------------------

    // The first stack-clearing operator may carry the advance width as an
    // extra leading argument, which is not ours to draw.
    void take_width(int even_args)
    {
        if (width_seen)
            return;
        width_seen = true;
        if (sp % 2 == 1 && even_args == 0) {
            // stems and hintmask: an odd count means a width first
            drop_first();
        } else if (even_args > 0 && sp > even_args) {
            drop_first();
        }
    }

    void drop_first()
    {
        for (int i = 1; i < sp; ++i)
            stack[i - 1] = stack[i];
        --sp;
    }

    // --- Running --------------------------------------------------------------------

    bool run(std::size_t from, std::size_t length, int depth)
    {
        if (depth > max_depth)
            return false;
        std::size_t pos = from;
        std::size_t const to = from + length;
        while (pos < to && !finished) {
            if (++instructions > max_instructions || !reader.has(pos, 1))
                return false;
            std::uint8_t const b0 = reader.u8(pos);
            // Operands.
            if (b0 >= 32 || b0 == 28) {
                double value;
                if (b0 == 28) {
                    value = static_cast<std::int16_t>(reader.u16(pos + 1));
                    pos += 3;
                } else if (b0 <= 246) {
                    value = static_cast<int>(b0) - 139;
                    ++pos;
                } else if (b0 <= 250) {
                    value = (static_cast<int>(b0) - 247) * 256 + reader.u8(pos + 1) + 108;
                    pos += 2;
                } else if (b0 <= 254) {
                    value = -(static_cast<int>(b0) - 251) * 256 - reader.u8(pos + 1) - 108;
                    pos += 2;
                } else {
                    value = static_cast<std::int32_t>(reader.u32(pos + 1)) / 65536.0;
                    pos += 5;
                }
                if (sp >= 48)
                    return false;
                stack[sp++] = value;
                continue;
            }
            ++pos;
            switch (b0) {
            case 1: // hstem
            case 3: // vstem
            case 18: // hstemhm
            case 23: // vstemhm
                take_width(0);
                stem_count += sp / 2;
                sp = 0;
                break;
            case 19: // hintmask
            case 20: // cntrmask
                take_width(0);
                stem_count += sp / 2; // vstem hints left implicit before a mask
                sp = 0;
                pos += static_cast<std::size_t>((stem_count + 7) / 8);
                break;
            case 21: // rmoveto
                take_width(2);
                if (sp < 2 || !move_to(stack[0], stack[1]))
                    return false;
                sp = 0;
                break;
            case 22: // hmoveto
                take_width(1);
                if (sp < 1 || !move_to(stack[0], 0))
                    return false;
                sp = 0;
                break;
            case 4: // vmoveto
                take_width(1);
                if (sp < 1 || !move_to(0, stack[0]))
                    return false;
                sp = 0;
                break;
            case 5: // rlineto
                for (int i = 0; i + 1 < sp; i += 2) {
                    if (!line_to(stack[i], stack[i + 1]))
                        return false;
                }
                sp = 0;
                break;
            case 6: // hlineto
            case 7: { // vlineto
                bool horizontal = b0 == 6;
                for (int i = 0; i < sp; ++i) {
                    if (!(horizontal ? line_to(stack[i], 0) : line_to(0, stack[i])))
                        return false;
                    horizontal = !horizontal;
                }
                sp = 0;
                break;
            }
            case 8: // rrcurveto
                for (int i = 0; i + 5 < sp; i += 6) {
                    if (!curve_to(stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4], stack[i + 5]))
                        return false;
                }
                sp = 0;
                break;
            case 24: { // rcurveline
                int i = 0;
                for (; i + 5 < sp - 2; i += 6) {
                    if (!curve_to(stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4], stack[i + 5]))
                        return false;
                }
                if (i + 1 < sp && !line_to(stack[i], stack[i + 1]))
                    return false;
                sp = 0;
                break;
            }
            case 25: { // rlinecurve
                int i = 0;
                for (; i + 1 < sp - 6; i += 2) {
                    if (!line_to(stack[i], stack[i + 1]))
                        return false;
                }
                if (i + 5 < sp
                    && !curve_to(stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4], stack[i + 5]))
                    return false;
                sp = 0;
                break;
            }
            case 26: // vvcurveto
            case 27: { // hhcurveto
                int i = 0;
                double d1 = 0;
                if (sp % 4 == 1) {
                    d1 = stack[0];
                    i = 1;
                }
                for (; i + 3 < sp; i += 4) {
                    bool const ok = b0 == 26 ? curve_to(d1, stack[i], stack[i + 1], stack[i + 2], 0, stack[i + 3])
                                             : curve_to(stack[i], d1, stack[i + 1], stack[i + 2], stack[i + 3], 0);
                    if (!ok)
                        return false;
                    d1 = 0;
                }
                sp = 0;
                break;
            }
            case 30: // vhcurveto
            case 31: { // hvcurveto
                bool horizontal = b0 == 31;
                int i = 0;
                while (i + 3 < sp) {
                    bool const last = i + 8 > sp;
                    double const dlast = (last && i + 4 < sp) ? stack[i + 4] : 0;
                    bool const ok = horizontal
                        ? curve_to(stack[i], 0, stack[i + 1], stack[i + 2], dlast, stack[i + 3])
                        : curve_to(0, stack[i], stack[i + 1], stack[i + 2], stack[i + 3], dlast);
                    if (!ok)
                        return false;
                    horizontal = !horizontal;
                    i += 4;
                }
                sp = 0;
                break;
            }
            case 10: { // callsubr
                if (sp < 1)
                    return false;
                int const index = static_cast<int>(stack[--sp]) + bias(priv.subrs.count);
                CffFont::Span subr;
                if (!priv.has_subrs || index < 0 || !font.index_item(reader.bytes, priv.subrs, static_cast<std::size_t>(index), subr))
                    return false;
                if (!run(subr.offset, subr.length, depth + 1))
                    return false;
                break;
            }
            case 29: { // callgsubr
                if (sp < 1)
                    return false;
                int const index = static_cast<int>(stack[--sp]) + bias(font.m_global_subrs.count);
                CffFont::Span subr;
                if (index < 0 || !font.index_item(reader.bytes, font.m_global_subrs, static_cast<std::size_t>(index), subr))
                    return false;
                if (!run(subr.offset, subr.length, depth + 1))
                    return false;
                break;
            }
            case 11: // return
                return true;
            case 14: { // endchar
                take_width(sp >= 4 ? 4 : 0);
                if (sp >= 4) {
                    // Accent composition, as the seac of Type 1: the base
                    // glyph, then the accent glyph moved by (adx, ady).
                    std::uint8_t const achar = static_cast<std::uint8_t>(std::clamp(stack[sp - 1], 0.0, 255.0));
                    std::uint8_t const bchar = static_cast<std::uint8_t>(std::clamp(stack[sp - 2], 0.0, 255.0));
                    double const ady = stack[sp - 3];
                    double const adx = stack[sp - 4];
                    sp = 0;
                    close_contour();
                    finished = true;
                    return compose(bchar, achar, adx, ady, depth);
                }
                sp = 0;
                close_contour();
                finished = true;
                return true;
            }
            case 12: { // escape
                if (!reader.has(pos, 1))
                    return false;
                std::uint8_t const b1 = reader.u8(pos++);
                if (!escape(b1))
                    return false;
                break;
            }
            default:
                // A reserved operator clears the stack, as the specification
                // says an unknown one should be treated.
                sp = 0;
                break;
            }
        }
        return true;
    }

    static int bias(std::size_t count)
    {
        return count < 1240 ? 107 : count < 33900 ? 1131 : 32768;
    }

    bool escape(std::uint8_t b1)
    {
        switch (b1) {
        case 35: { // flex
            if (sp < 13)
                return false;
            if (!curve_to(stack[0], stack[1], stack[2], stack[3], stack[4], stack[5])
                || !curve_to(stack[6], stack[7], stack[8], stack[9], stack[10], stack[11]))
                return false;
            sp = 0;
            return true;
        }
        case 34: { // hflex
            if (sp < 7)
                return false;
            double const y0 = y;
            if (!curve_to(stack[0], 0, stack[1], stack[2], stack[3], 0)
                || !curve_to(stack[4], 0, stack[5], y0 - y, stack[6], 0))
                return false;
            sp = 0;
            return true;
        }
        case 36: { // hflex1
            if (sp < 9)
                return false;
            double const y0 = y;
            if (!curve_to(stack[0], stack[1], stack[2], stack[3], stack[4], 0))
                return false;
            // The last point returns to the starting y.
            double const dy6 = y0 - (y + stack[7]);
            if (!curve_to(stack[5], 0, stack[6], stack[7], stack[8], dy6))
                return false;
            sp = 0;
            return true;
        }
        case 37: { // flex1
            if (sp < 11)
                return false;
            double const x0 = x;
            double const y0 = y;
            double dx = 0;
            double dy = 0;
            for (int i = 0; i < 10; i += 2) {
                dx += stack[i];
                dy += stack[i + 1];
            }
            if (!curve_to(stack[0], stack[1], stack[2], stack[3], stack[4], stack[5]))
                return false;
            // The sixth point: d6 is whichever of dx6 and dy6 is the larger
            // travel, and the other returns to the start.
            double const c1x = x + stack[6];
            double const c1y = y + stack[7];
            double const c2x = c1x + stack[8];
            double const c2y = c1y + stack[9];
            double px;
            double py;
            if (std::abs(dx) > std::abs(dy)) {
                px = c2x + stack[10];
                py = y0;
            } else {
                px = x0;
                py = c2y + stack[10];
            }
            bool const ok = quadratics_for(x, y, c1x, c1y, c2x, c2y, px, py, 0);
            x = px;
            y = py;
            if (!ok)
                return false;
            sp = 0;
            return true;
        }
        // The arithmetic operators, kept for the few old fonts that use them.
        case 3: // and
        case 4: // or
        case 5: // not
        case 9: // abs
        case 10: // add
        case 11: // sub
        case 12: // div
        case 14: // neg
        case 15: // eq
        case 18: // drop
        case 24: // mul
        case 26: // sqrt
        case 27: // dup
        case 28: // exch
            return arithmetic(b1);
        case 20: { // put
            if (sp < 2)
                return false;
            int const index = static_cast<int>(stack[sp - 1]);
            if (index >= 0 && index < 32)
                transient[index] = stack[sp - 2];
            sp -= 2;
            return true;
        }
        case 21: { // get
            if (sp < 1)
                return false;
            int const index = static_cast<int>(stack[sp - 1]);
            stack[sp - 1] = index >= 0 && index < 32 ? transient[index] : 0;
            return true;
        }
        case 22: { // ifelse
            if (sp < 4)
                return false;
            double const result = stack[sp - 2] <= stack[sp - 1] ? stack[sp - 4] : stack[sp - 3];
            sp -= 3;
            stack[sp - 1] = result;
            return true;
        }
        case 23: // random: any number in (0, 1]
            if (sp >= 48)
                return false;
            stack[sp++] = 0.5;
            return true;
        case 29: { // index
            if (sp < 1)
                return false;
            int const i = std::max(0, static_cast<int>(stack[sp - 1]));
            stack[sp - 1] = sp - 2 - i >= 0 ? stack[sp - 2 - i] : 0;
            return true;
        }
        case 30: { // roll
            if (sp < 2)
                return false;
            int const n = static_cast<int>(stack[sp - 2]);
            int const j = static_cast<int>(stack[sp - 1]);
            sp -= 2;
            if (n <= 0 || n > sp)
                return true;
            int const shift = ((j % n) + n) % n;
            std::rotate(stack + sp - n, stack + sp - shift, stack + sp);
            return true;
        }
        default:
            sp = 0; // dotsection and anything reserved
            return true;
        }
    }

    bool arithmetic(std::uint8_t b1)
    {
        auto const binary = [&](auto f) {
            if (sp < 2)
                return false;
            stack[sp - 2] = f(stack[sp - 2], stack[sp - 1]);
            --sp;
            return true;
        };
        auto const unary = [&](auto f) {
            if (sp < 1)
                return false;
            stack[sp - 1] = f(stack[sp - 1]);
            return true;
        };
        switch (b1) {
        case 3: return binary([](double a, double b) { return a != 0 && b != 0 ? 1.0 : 0.0; });
        case 4: return binary([](double a, double b) { return a != 0 || b != 0 ? 1.0 : 0.0; });
        case 5: return unary([](double a) { return a == 0 ? 1.0 : 0.0; });
        case 9: return unary([](double a) { return std::abs(a); });
        case 10: return binary([](double a, double b) { return a + b; });
        case 11: return binary([](double a, double b) { return a - b; });
        case 12: return binary([](double a, double b) { return b != 0 ? a / b : 0.0; });
        case 14: return unary([](double a) { return -a; });
        case 15: return binary([](double a, double b) { return a == b ? 1.0 : 0.0; });
        case 18:
            if (sp < 1)
                return false;
            --sp;
            return true;
        case 24: return binary([](double a, double b) { return a * b; });
        case 26: return unary([](double a) { return a >= 0 ? std::sqrt(a) : 0.0; });
        case 27:
            if (sp < 1 || sp >= 48)
                return false;
            stack[sp] = stack[sp - 1];
            ++sp;
            return true;
        case 28:
            if (sp < 2)
                return false;
            std::swap(stack[sp - 1], stack[sp - 2]);
            return true;
        default: return false;
        }
    }

    // The accent composition of endchar: both glyphs run as their own
    // programs into this outline, the accent moved by (adx, ady).
    bool compose(std::uint8_t bchar, std::uint8_t achar, double adx, double ady, int depth)
    {
        if (depth > 2)
            return false;
        std::uint16_t const base = font.glyph_of_standard_code(reader.bytes, bchar);
        std::uint16_t const accent = font.glyph_of_standard_code(reader.bytes, achar);
        if (base == 0 || accent == 0)
            return false;
        for (int part = 0; part < 2; ++part) {
            std::uint16_t const glyph = part == 0 ? base : accent;
            CffFont::Span program;
            if (!font.index_item(reader.bytes, font.m_charstrings, glyph, program))
                return false;
            Type2 inner { font, reader, out, font.private_for(glyph) };
            inner.instructions = instructions;
            inner.x = part == 0 ? 0 : adx;
            inner.y = part == 0 ? 0 : ady;
            if (!inner.run(program.offset, program.length, depth + 1))
                return false;
            inner.close_contour();
            instructions = inner.instructions;
        }
        return true;
    }
};

// --- The font ---------------------------------------------------------------------

std::optional<CffFont> CffFont::parse(std::vector<std::uint8_t> const& bytes, std::size_t offset, std::size_t length)
{
    if (offset > bytes.size() || length > bytes.size() - offset || length < 4)
        return std::nullopt;
    CffFont font;
    font.m_offset = offset;
    font.m_end = offset + length;
    Reader const reader { bytes, font.m_end };
    if (reader.u8(offset) != 1) // the major version: CFF 1 only
        return std::nullopt;
    std::size_t pos = offset + reader.u8(offset + 2); // the header's size
    Index names;
    Index top_dicts;
    Index strings;
    if (!font.read_index(bytes, pos, names, pos) || !font.read_index(bytes, pos, top_dicts, pos)
        || !font.read_index(bytes, pos, strings, pos) || !font.read_index(bytes, pos, font.m_global_subrs, pos))
        return std::nullopt;
    Span top;
    if (top_dicts.count < 1 || !font.index_item(bytes, top_dicts, 0, top))
        return std::nullopt;
    std::vector<DictEntry> const dict = parse_dict(reader, top.offset, top.length);
    if (std::vector<double> const* const type = dict_get(dict, 1206); type && !type->empty() && (*type)[0] != 2)
        return std::nullopt; // Type 1 charstrings are not read
    std::vector<double> const* const charstrings = dict_get(dict, 17);
    if (!charstrings || charstrings->empty())
        return std::nullopt;
    std::size_t next = 0;
    if ((*charstrings)[0] < 0 || !font.read_index(bytes, offset + static_cast<std::size_t>((*charstrings)[0]), font.m_charstrings, next))
        return std::nullopt;
    if (font.m_charstrings.count == 0 || font.m_charstrings.count > 65535)
        return std::nullopt;
    font.m_glyph_count = static_cast<std::uint16_t>(font.m_charstrings.count);
    if (std::vector<double> const* const matrix = dict_get(dict, 1207); matrix && matrix->size() == 6) {
        double const scale = (*matrix)[0];
        if (std::isfinite(scale) && scale > 0)
            font.m_matrix_scale = scale;
    }
    if (std::vector<double> const* const charset = dict_get(dict, 15); charset && !charset->empty() && (*charset)[0] > 2)
        font.m_charset = offset + static_cast<std::size_t>((*charset)[0]);
    if (std::vector<double> const* const priv = dict_get(dict, 18); priv && priv->size() == 2 && (*priv)[1] >= 0 && (*priv)[0] >= 0)
        (void)font.read_private(bytes, offset + static_cast<std::size_t>((*priv)[1]), static_cast<std::size_t>((*priv)[0]), font.m_private);
    // A CID-keyed font: a font dict per group of glyphs, each with its own
    // private dict and subroutines, and a table saying which glyph is whose.
    if (dict_get(dict, 1230)) {
        font.m_cid = true;
        std::vector<double> const* const fdarray = dict_get(dict, 1236);
        std::vector<double> const* const fdselect = dict_get(dict, 1237);
        if (!fdarray || fdarray->empty() || !fdselect || fdselect->empty())
            return std::nullopt;
        Index fonts;
        if ((*fdarray)[0] < 0 || !font.read_index(bytes, offset + static_cast<std::size_t>((*fdarray)[0]), fonts, next) || fonts.count == 0
            || fonts.count > 256)
            return std::nullopt;
        for (std::size_t i = 0; i < fonts.count; ++i) {
            Private entry;
            Span span;
            if (font.index_item(bytes, fonts, i, span)) {
                std::vector<DictEntry> const fd = parse_dict(reader, span.offset, span.length);
                if (std::vector<double> const* const p = dict_get(fd, 18); p && p->size() == 2 && (*p)[1] >= 0 && (*p)[0] >= 0)
                    (void)font.read_private(bytes, offset + static_cast<std::size_t>((*p)[1]), static_cast<std::size_t>((*p)[0]), entry);
            }
            font.m_fd_privates.push_back(std::move(entry));
        }
        font.m_fd_select.assign(font.m_glyph_count, 0);
        if ((*fdselect)[0] < 0)
            return std::nullopt;
        std::size_t const select = offset + static_cast<std::size_t>((*fdselect)[0]);
        std::uint8_t const format = reader.u8(select);
        if (format == 0) {
            for (std::size_t g = 0; g < font.m_glyph_count; ++g)
                font.m_fd_select[g] = reader.u8(select + 1 + g);
        } else if (format == 3) {
            std::size_t const ranges = reader.u16(select + 1);
            std::size_t const sentinel = reader.u16(select + 3 + ranges * 3);
            for (std::size_t r = 0; r < ranges; ++r) {
                std::size_t const first = reader.u16(select + 3 + r * 3);
                std::uint8_t const fd = reader.u8(select + 5 + r * 3);
                std::size_t const next_first = r + 1 < ranges ? reader.u16(select + 3 + (r + 1) * 3) : sentinel;
                for (std::size_t g = first; g < next_first && g < font.m_glyph_count; ++g)
                    font.m_fd_select[g] = fd;
            }
        } else {
            return std::nullopt;
        }
    }
    return font;
}

bool CffFont::read_index(std::vector<std::uint8_t> const& bytes, std::size_t at, Index& out, std::size_t& next) const
{
    Reader const reader { bytes, m_end };
    if (!reader.has(at, 2))
        return false;
    out.count = reader.u16(at);
    if (out.count == 0) {
        out.off_size = 0;
        out.offsets = out.data = out.end = at + 2;
        next = at + 2;
        return true;
    }
    out.off_size = reader.u8(at + 2);
    if (out.off_size < 1 || out.off_size > 4)
        return false;
    out.offsets = at + 3;
    if (!reader.has(out.offsets, (out.count + 1) * out.off_size))
        return false;
    out.data = out.offsets + (out.count + 1) * out.off_size - 1;
    std::uint32_t const last = reader.sized(out.offsets + out.count * out.off_size, out.off_size);
    if (last < 1 || !reader.has(out.data, last))
        return false;
    out.end = out.data + last;
    next = out.end;
    return true;
}

bool CffFont::index_item(std::vector<std::uint8_t> const& bytes, Index const& index, std::size_t i, Span& out) const
{
    if (i >= index.count)
        return false;
    Reader const reader { bytes, m_end };
    std::uint32_t const start = reader.sized(index.offsets + i * index.off_size, index.off_size);
    std::uint32_t const stop = reader.sized(index.offsets + (i + 1) * index.off_size, index.off_size);
    if (start < 1 || stop < start || !reader.has(index.data + start, stop - start))
        return false;
    out.offset = index.data + start;
    out.length = stop - start;
    return true;
}

bool CffFont::read_private(std::vector<std::uint8_t> const& bytes, std::size_t offset, std::size_t length, Private& out) const
{
    Reader const reader { bytes, m_end };
    if (!reader.has(offset, length))
        return false;
    std::vector<DictEntry> const dict = parse_dict(reader, offset, length);
    if (std::vector<double> const* const subrs = dict_get(dict, 19); subrs && !subrs->empty() && (*subrs)[0] >= 0) {
        std::size_t next = 0;
        out.has_subrs = read_index(bytes, offset + static_cast<std::size_t>((*subrs)[0]), out.subrs, next);
    }
    return true;
}

void CffFont::load_charset(std::vector<std::uint8_t> const& bytes) const
{
    if (m_charset_loaded)
        return;
    m_charset_loaded = true;
    m_sid_of_glyph.assign(m_glyph_count, 0);
    if (m_charset == 0) {
        for (std::size_t g = 0; g < m_glyph_count; ++g)
            m_sid_of_glyph[g] = static_cast<std::uint16_t>(g); // ISOAdobe: glyph i is string i
        return;
    }
    Reader const reader { bytes, m_end };
    std::uint8_t const format = reader.u8(m_charset);
    std::size_t pos = m_charset + 1;
    std::size_t glyph = 1; // .notdef is always 0
    if (format == 0) {
        for (; glyph < m_glyph_count; ++glyph, pos += 2)
            m_sid_of_glyph[glyph] = reader.u16(pos);
    } else if (format == 1 || format == 2) {
        while (glyph < m_glyph_count && reader.has(pos, format == 1 ? 3 : 4)) {
            std::uint16_t const first = reader.u16(pos);
            std::size_t const left = format == 1 ? reader.u8(pos + 2) : reader.u16(pos + 2);
            pos += format == 1 ? 3 : 4;
            for (std::size_t k = 0; k <= left && glyph < m_glyph_count; ++k, ++glyph)
                m_sid_of_glyph[glyph] = static_cast<std::uint16_t>(first + k);
        }
    }
}

std::uint16_t CffFont::glyph_of_standard_code(std::vector<std::uint8_t> const& bytes, std::uint8_t code) const
{
    std::uint16_t const sid = standard_encoding_sid(code);
    if (sid == 0 || m_cid)
        return 0;
    // The charset is loaded once, on the first accent.
    load_charset(bytes);
    for (std::size_t g = 1; g < m_sid_of_glyph.size(); ++g) {
        if (m_sid_of_glyph[g] == sid)
            return static_cast<std::uint16_t>(g);
    }
    return 0;
}

CffFont::Private const& CffFont::private_for(std::uint16_t glyph) const
{
    if (m_cid && glyph < m_fd_select.size()) {
        std::uint8_t const fd = m_fd_select[glyph];
        if (fd < m_fd_privates.size())
            return m_fd_privates[fd];
    }
    return m_private;
}

float CffFont::units_scale(std::uint16_t units_per_em) const
{
    // The usual matrix is 1/1000 over a 1000-unit em: a scale of one.
    double const scale = m_matrix_scale * units_per_em;
    if (!std::isfinite(scale) || scale <= 0)
        return 1.0f;
    return std::abs(scale - 1.0) < 1e-6 ? 1.0f : static_cast<float>(scale);
}

bool CffFont::outline(std::vector<std::uint8_t> const& bytes, std::uint16_t glyph, GlyphOutline& out) const
{
    Span program;
    if (!index_item(bytes, m_charstrings, glyph, program))
        return false;
    Type2 interpreter { *this, Reader { bytes, m_end }, out, private_for(glyph) };
    if (!interpreter.run(program.offset, program.length, 0))
        return false;
    interpreter.close_contour();
    return true;
}

}
