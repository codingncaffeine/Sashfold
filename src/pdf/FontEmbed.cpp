#include "pdf/FontEmbed.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <unordered_map>

namespace sashfold::pdf {

namespace {

using Bytes = std::vector<std::uint8_t>;

std::uint32_t u8_at(Bytes const& b, std::size_t at) { return at < b.size() ? b[at] : 0; }
std::uint32_t u16_at(Bytes const& b, std::size_t at) { return (u8_at(b, at) << 8) | u8_at(b, at + 1); }
std::int32_t s16_at(Bytes const& b, std::size_t at) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(u16_at(b, at))); }
std::uint32_t u32_at(Bytes const& b, std::size_t at) { return (u16_at(b, at) << 16) | u16_at(b, at + 2); }

void put16(Bytes& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}
void put32(Bytes& out, std::uint32_t v)
{
    put16(out, v >> 16);
    put16(out, v & 0xFFFF);
}
void set32(Bytes& out, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}

std::uint32_t checksum(Bytes const& data, std::size_t at, std::size_t length)
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < length; i += 4) {
        std::uint32_t word = 0;
        for (std::size_t j = 0; j < 4; ++j)
            word = (word << 8) | (i + j < length ? data[at + i + j] : 0);
        sum += word;
    }
    return sum;
}

// An sfnt of the tables given (OpenType §5 "Organization of an OpenType
// font"): the directory sorted by tag, each table padded to four bytes,
// the head table's checkSumAdjustment set over the whole file.
Bytes write_sfnt(std::map<std::string, Bytes> tables)
{
    Bytes out;
    auto const count = static_cast<std::uint32_t>(tables.size());
    std::uint32_t power = 1, log2 = 0;
    while (power * 2 <= count) {
        power *= 2;
        ++log2;
    }
    put32(out, 0x00010000);
    put16(out, count);
    put16(out, power * 16);
    put16(out, log2);
    put16(out, count * 16 - power * 16);
    std::size_t const directory = out.size();
    out.resize(out.size() + 16 * tables.size());
    std::size_t head_at = 0;
    std::size_t row = directory;
    if (auto it = tables.find("head"); it != tables.end() && it->second.size() >= 12)
        set32(it->second, 8, 0);
    for (auto const& [tag, data] : tables) {
        while (out.size() % 4)
            out.push_back(0);
        std::size_t const at = out.size();
        if (tag == "head")
            head_at = at;
        out.insert(out.end(), data.begin(), data.end());
        for (std::size_t i = 0; i < 4; ++i)
            out[row + i] = static_cast<std::uint8_t>(i < tag.size() ? tag[i] : ' ');
        set32(out, row + 4, checksum(out, at, data.size()));
        set32(out, row + 8, static_cast<std::uint32_t>(at));
        set32(out, row + 12, static_cast<std::uint32_t>(data.size()));
        row += 16;
    }
    while (out.size() % 4)
        out.push_back(0);
    if (head_at)
        set32(out, head_at + 8, 0xB1B0AFBAu - checksum(out, 0, out.size()));
    return out;
}

// --- CFF (Adobe Technical Note #5176) ---

struct CffIndex {
    std::size_t count = 0;
    std::size_t off_size = 0;
    std::size_t offsets = 0;
    std::size_t data = 0; // offsets are relative to data - 1
    std::size_t end = 0;
    bool ok = false;
};

CffIndex read_index(Bytes const& b, std::size_t at, std::size_t limit)
{
    CffIndex index;
    if (at + 2 > limit)
        return index;
    index.count = u16_at(b, at);
    if (index.count == 0) {
        index.end = at + 2;
        index.ok = true;
        return index;
    }
    index.off_size = u8_at(b, at + 2);
    if (index.off_size < 1 || index.off_size > 4)
        return index;
    index.offsets = at + 3;
    index.data = index.offsets + (index.count + 1) * index.off_size - 1;
    if (index.data >= limit)
        return index;
    auto const last = [&] {
        std::size_t v = 0;
        for (std::size_t i = 0; i < index.off_size; ++i)
            v = (v << 8) | u8_at(b, index.offsets + index.count * index.off_size + i);
        return v;
    }();
    index.end = index.data + last;
    index.ok = index.end <= limit;
    return index;
}

std::pair<std::size_t, std::size_t> index_item(Bytes const& b, CffIndex const& index, std::size_t i)
{
    auto const offset = [&](std::size_t k) {
        std::size_t v = 0;
        for (std::size_t j = 0; j < index.off_size; ++j)
            v = (v << 8) | u8_at(b, index.offsets + k * index.off_size + j);
        return v;
    };
    if (!index.ok || i >= index.count)
        return { 0, 0 };
    std::size_t const start = index.data + offset(i), end = index.data + offset(i + 1);
    if (end < start || end > index.end)
        return { 0, 0 };
    return { start, end - start };
}

Bytes write_index(std::vector<Bytes> const& items)
{
    Bytes out;
    put16(out, static_cast<std::uint32_t>(items.size()));
    if (items.empty())
        return out;
    std::size_t total = 1;
    for (Bytes const& item : items)
        total += item.size();
    std::size_t const off_size = total < 0x100 ? 1 : total < 0x10000 ? 2 : total < 0x1000000 ? 3 : 4;
    out.push_back(static_cast<std::uint8_t>(off_size));
    std::size_t offset = 1;
    auto const put_offset = [&](std::size_t v) {
        for (std::size_t j = off_size; j-- > 0;)
            out.push_back(static_cast<std::uint8_t>(v >> (8 * j)));
    };
    put_offset(offset);
    for (Bytes const& item : items) {
        offset += item.size();
        put_offset(offset);
    }
    for (Bytes const& item : items)
        out.insert(out.end(), item.begin(), item.end());
    return out;
}

// One DICT entry: its operator (escaped ones as 1200 + the second byte),
// its operands' values and their bytes as written.
struct DictEntry {
    int op = 0;
    std::vector<double> operands;
    Bytes raw; // the operands' bytes
};

std::vector<DictEntry> read_dict(Bytes const& b, std::size_t at, std::size_t end)
{
    std::vector<DictEntry> entries;
    DictEntry current;
    std::size_t operand_start = at;
    while (at < end && at < b.size()) {
        std::uint32_t const b0 = b[at];
        if (b0 <= 21) {
            int op = static_cast<int>(b0);
            current.raw.assign(b.begin() + static_cast<std::ptrdiff_t>(operand_start), b.begin() + static_cast<std::ptrdiff_t>(at));
            ++at;
            if (b0 == 12) {
                op = 1200 + static_cast<int>(u8_at(b, at));
                ++at;
            }
            current.op = op;
            entries.push_back(std::move(current));
            current = {};
            operand_start = at;
        } else if (b0 == 28) {
            current.operands.push_back(s16_at(b, at + 1));
            at += 3;
        } else if (b0 == 29) {
            current.operands.push_back(static_cast<std::int32_t>(u32_at(b, at + 1)));
            at += 5;
        } else if (b0 == 30) {
            // A real: nibbles to the 0xF that ends it.
            std::string text;
            ++at;
            bool done = false;
            while (at < end && !done) {
                for (int shift : { 4, 0 }) {
                    std::uint32_t const nibble = (b[at] >> shift) & 0xF;
                    if (nibble <= 9)
                        text.push_back(static_cast<char>('0' + nibble));
                    else if (nibble == 0xA)
                        text.push_back('.');
                    else if (nibble == 0xB)
                        text.push_back('E');
                    else if (nibble == 0xC)
                        text += "E-";
                    else if (nibble == 0xE)
                        text.push_back('-');
                    else if (nibble == 0xF) {
                        done = true;
                        break;
                    }
                }
                ++at;
            }
            current.operands.push_back(std::strtod(text.c_str(), nullptr));
        } else if (b0 >= 32 && b0 <= 246) {
            current.operands.push_back(static_cast<double>(b0) - 139);
            ++at;
        } else if (b0 >= 247 && b0 <= 250) {
            current.operands.push_back((static_cast<double>(b0) - 247) * 256 + u8_at(b, at + 1) + 108);
            at += 2;
        } else if (b0 >= 251 && b0 <= 254) {
            current.operands.push_back(-(static_cast<double>(b0) - 251) * 256 - u8_at(b, at + 1) - 108);
            at += 2;
        } else {
            ++at; // reserved
        }
    }
    return entries;
}

DictEntry const* find_entry(std::vector<DictEntry> const& dict, int op)
{
    for (DictEntry const& e : dict) {
        if (e.op == op)
            return &e;
    }
    return nullptr;
}

void put_dict_int(Bytes& out, std::int32_t v)
{
    // Always the five-byte form, so a dictionary's size does not depend on
    // the offsets it holds.
    out.push_back(29);
    put32(out, static_cast<std::uint32_t>(v));
}

void put_op(Bytes& out, int op)
{
    if (op >= 1200) {
        out.push_back(12);
        out.push_back(static_cast<std::uint8_t>(op - 1200));
    } else {
        out.push_back(static_cast<std::uint8_t>(op));
    }
}

std::size_t subr_bias(std::size_t count)
{
    return count < 1240 ? 107 : count < 33900 ? 1131 : 32768;
}

// A Type 2 charstring followed through the subroutines it calls
// (Technical Note #5177 §4): the operands on a stack so a call's number is
// known, the stem hints counted so a hintmask's bytes are stepped over.
// With `out`, the charstring is written again with every call replaced by
// the subroutine's own operators (the calls' numbers and the returns
// left out): the glyph needs no subroutines at all. False when the
// charstring does what the walk does not follow (arithmetic that computes
// a subroutine's number, a number out of range), and the caller keeps the
// glyph as it is with every subroutine.
struct SubrWalk {
    Bytes const& b;
    CffIndex const& global;
    CffIndex const* local;
    Bytes* out = nullptr;
    std::vector<double> stack;
    std::size_t stems = 0;
    std::size_t last_number_at = 0; // in `out`: where the last operand written starts
    bool ended = false;
    int work = 0;

    void copy(std::size_t from, std::size_t to)
    {
        if (out)
            out->insert(out->end(), b.begin() + static_cast<std::ptrdiff_t>(from), b.begin() + static_cast<std::ptrdiff_t>(std::min(to, b.size())));
    }

    bool run(std::size_t at, std::size_t end, int depth)
    {
        if (depth > 10)
            return false;
        while (at < end && !ended) {
            if (++work > 200000)
                return false;
            std::uint32_t const b0 = b[at];
            std::size_t const token = at;
            if (b0 == 28 || b0 >= 32) {
                if (b0 == 28) {
                    stack.push_back(s16_at(b, at + 1));
                    at += 3;
                } else if (b0 <= 246) {
                    stack.push_back(static_cast<double>(b0) - 139);
                    ++at;
                } else if (b0 <= 250) {
                    stack.push_back((static_cast<double>(b0) - 247) * 256 + u8_at(b, at + 1) + 108);
                    at += 2;
                } else if (b0 <= 254) {
                    stack.push_back(-(static_cast<double>(b0) - 251) * 256 - u8_at(b, at + 1) - 108);
                    at += 2;
                } else {
                    stack.push_back(static_cast<std::int32_t>(u32_at(b, at + 1)) / 65536.0);
                    at += 5;
                }
                if (out)
                    last_number_at = out->size();
                copy(token, at);
                continue;
            }
            ++at;
            switch (b0) {
            case 1: // hstem
            case 3: // vstem
            case 18: // hstemhm
            case 23: // vstemhm
                stems += stack.size() / 2;
                stack.clear();
                copy(token, at);
                break;
            case 19: // hintmask
            case 20: // cntrmask
                stems += stack.size() / 2;
                stack.clear();
                at += (stems + 7) / 8;
                copy(token, at);
                break;
            case 10: // callsubr
            case 29: { // callgsubr
                if (stack.empty())
                    return false;
                CffIndex const* index = b0 == 10 ? local : &global;
                if (!index || !index->ok)
                    return false;
                double const n = stack.back() + static_cast<double>(subr_bias(index->count));
                stack.pop_back();
                if (n < 0 || n >= static_cast<double>(index->count) || n != std::floor(n))
                    return false;
                if (out)
                    out->resize(last_number_at); // the call's number goes with the call
                auto const [start, length] = index_item(b, *index, static_cast<std::size_t>(n));
                if (!run(start, start + length, depth + 1))
                    return false;
                break;
            }
            case 11: // return
                return depth > 0;
            case 14: // endchar
                copy(token, at);
                ended = true;
                return true;
            case 12: {
                std::uint32_t const b1 = u8_at(b, at);
                ++at;
                // The flex operators clear the stack; the arithmetic and
                // storage ones are not followed.
                if (b1 >= 34 && b1 <= 37) {
                    stack.clear();
                    copy(token, at);
                    break;
                }
                return false;
            }
            default:
                stack.clear();
                copy(token, at);
                break;
            }
        }
        return true;
    }
};

} // namespace

// --- SystemFace ---

std::shared_ptr<SystemFace const> SystemFace::load(std::string const& path, std::size_t face_index)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, std::shared_ptr<SystemFace const>> faces;
    std::string const key = path + "#" + std::to_string(face_index);
    {
        std::lock_guard const lock(mutex);
        if (auto const it = faces.find(key); it != faces.end())
            return it->second;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return nullptr;
    Bytes bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::shared_ptr<SystemFace const> face = from_bytes(std::move(bytes), face_index, key);
    std::lock_guard const lock(mutex);
    return faces.emplace(key, face).first->second;
}

std::shared_ptr<SystemFace const> SystemFace::from_bytes(Bytes bytes, std::size_t face_index, std::string key)
{
    std::shared_ptr<SystemFace> face(new SystemFace());
    face->m_bytes = std::move(bytes);
    face->m_key = std::move(key);
    if (!face->read(face_index))
        return nullptr;
    return face;
}

std::pair<std::uint32_t, std::uint32_t> SystemFace::table(char const* tag) const
{
    auto const it = m_tables.find(tag);
    return it == m_tables.end() ? std::pair<std::uint32_t, std::uint32_t> { 0, 0 } : it->second;
}

bool SystemFace::read(std::size_t face_index)
{
    Bytes const& b = m_bytes;
    std::size_t start = 0;
    if (b.size() >= 12 && u32_at(b, 0) == 0x74746366) { // 'ttcf'
        std::uint32_t const count = u32_at(b, 8);
        if (face_index >= count)
            return false;
        start = u32_at(b, 12 + 4 * face_index);
    }
    std::uint32_t const tables = u16_at(b, start + 4);
    if (start + 12 + 16 * static_cast<std::size_t>(tables) > b.size())
        return false;
    for (std::uint32_t i = 0; i < tables; ++i) {
        std::size_t const row = start + 12 + 16 * i;
        std::string tag(b.begin() + static_cast<std::ptrdiff_t>(row), b.begin() + static_cast<std::ptrdiff_t>(row + 4));
        std::uint32_t const offset = u32_at(b, row + 8), length = u32_at(b, row + 12);
        if (static_cast<std::size_t>(offset) + length > b.size())
            continue;
        m_tables[tag] = { offset, length };
    }
    m_cff = table("CFF ");
    std::optional<text::TrueTypeFont> font = text::TrueTypeFont::parse(m_bytes, face_index);
    if (!font)
        return false;
    m_font = std::make_unique<text::TrueTypeFont>(std::move(*font));
    if (!m_cff.second && !table("glyf").second)
        return false;

    // Name ID 6, in the Windows (3,1) UTF-16 or the Macintosh (1,0) record.
    auto const [name_at, name_length] = table("name");
    if (name_length >= 6) {
        std::uint32_t const count = u16_at(b, name_at + 2), strings = name_at + u16_at(b, name_at + 4);
        for (std::uint32_t i = 0; i < count && m_postscript_name.empty(); ++i) {
            std::size_t const r = name_at + 6 + 12 * i;
            if (r + 12 > name_at + name_length || u16_at(b, r + 6) != 6)
                continue;
            std::uint32_t const platform = u16_at(b, r), length = u16_at(b, r + 8), offset = u16_at(b, r + 10);
            std::string name;
            for (std::uint32_t k = 0; k < length; k += platform == 3 ? 2 : 1) {
                std::uint32_t const c = platform == 3 ? u16_at(b, strings + offset + k) : u8_at(b, strings + offset + k);
                if (c > 0x20 && c < 0x7F && c != '[' && c != ']' && c != '(' && c != ')' && c != '{' && c != '}' && c != '<' && c != '>'
                    && c != '/' && c != '%')
                    name.push_back(static_cast<char>(c));
            }
            m_postscript_name = name;
        }
    }
    if (m_postscript_name.empty()) {
        for (char const c : m_font->family_name() + "-" + m_font->subfamily_name()) {
            if (c > 0x20 && c < 0x7F && c != '/' && c != '(' && c != ')')
                m_postscript_name.push_back(c);
        }
    }

    // The descriptor's numbers (§9.8), in thousandths of the em.
    double const upem = m_font->units_per_em() ? m_font->units_per_em() : 1000;
    auto const scaled = [&](double v) { return std::round(v * 1000 / upem); };
    auto const [head_at, head_length] = table("head");
    if (head_length >= 54) {
        m_metrics.bbox = { scaled(s16_at(b, head_at + 36)), scaled(s16_at(b, head_at + 38)), scaled(s16_at(b, head_at + 40)),
            scaled(s16_at(b, head_at + 42)) };
    }
    m_metrics.ascent = scaled(m_font->ascender());
    m_metrics.descent = scaled(m_font->descender());
    m_metrics.cap_height = m_font->cap_height() ? scaled(m_font->cap_height()) : std::round(m_metrics.ascent * 0.7);
    auto const [post_at, post_length] = table("post");
    if (post_length >= 16) {
        m_metrics.italic_angle = static_cast<std::int32_t>(u32_at(b, post_at + 4)) / 65536.0;
    }
    // StemV is not in the font's tables; estimated from the weight class
    // as a stem's thickness grows with it (a regular face about 80, a bold
    // one about 140, in thousandths of the em).
    m_metrics.stem_v = std::round(50 + std::pow(m_font->weight_class() / 65.0, 2));
    bool const fixed = post_length >= 16 && u32_at(b, post_at + 12) != 0;
    // Symbolic (bit 3): its glyphs are reached by glyph id, not by a
    // standard Latin character set; FixedPitch (1) and Italic (7) as the
    // face says.
    m_metrics.flags = 4 | (fixed ? 1u : 0u) | (m_font->is_italic() || m_metrics.italic_angle != 0 ? 64u : 0u);
    return true;
}

int SystemFace::advance(std::uint16_t glyph) const
{
    double const upem = m_font->units_per_em() ? m_font->units_per_em() : 1000;
    return static_cast<int>(std::lround(m_font->advance_width(glyph) * 1000.0 / upem));
}

// A TrueType program with every glyph id it had and the outlines of the
// given glyphs alone (with the components of composite ones): what a
// CIDFontType2 needs (§9.9: head, hhea, loca, maxp, cvt, prep, glyf, hmtx,
// fpgm), with the CIDs being the glyph ids.
Bytes SystemFace::truetype_subset(std::vector<std::uint16_t> const& glyphs) const
{
    Bytes const& b = m_bytes;
    auto const [head_at, head_length] = table("head");
    auto const [loca_at, loca_length] = table("loca");
    auto const [glyf_at, glyf_length] = table("glyf");
    if (head_length < 54 || !loca_length || !glyf_length)
        return {};
    bool const long_loca = s16_at(b, head_at + 50) != 0;
    std::size_t const count = m_font->glyph_count();
    auto const span = [&](std::size_t g) -> std::pair<std::size_t, std::size_t> {
        if (g >= count)
            return { 0, 0 };
        std::size_t start = long_loca ? u32_at(b, loca_at + 4 * g) : 2 * u16_at(b, loca_at + 2 * g);
        std::size_t end = long_loca ? u32_at(b, loca_at + 4 * (g + 1)) : 2 * u16_at(b, loca_at + 2 * (g + 1));
        if (end < start || end > glyf_length)
            return { 0, 0 };
        return { glyf_at + start, end - start };
    };
    // The glyphs kept: the ones asked for, .notdef, and every component.
    std::set<std::size_t> kept;
    std::vector<std::size_t> pending(glyphs.begin(), glyphs.end());
    pending.push_back(0);
    while (!pending.empty()) {
        std::size_t const g = pending.back();
        pending.pop_back();
        if (g >= count || !kept.insert(g).second)
            continue;
        auto const [at, length] = span(g);
        if (length < 10 || s16_at(b, at) >= 0)
            continue;
        // A composite glyph (OpenType "glyf": composite glyph description).
        std::size_t p = at + 10;
        for (int components = 0; components < 1024 && p + 4 <= at + length; ++components) {
            std::uint32_t const flags = u16_at(b, p);
            std::uint32_t const component = u16_at(b, p + 2);
            if (!kept.contains(component))
                pending.push_back(component);
            p += 4 + ((flags & 1) ? 4 : 2);
            if (flags & 8)
                p += 2;
            else if (flags & 0x40)
                p += 4;
            else if (flags & 0x80)
                p += 8;
            if (!(flags & 0x20))
                break;
        }
    }
    // The font ends at the last glyph kept: the ids past it are no CID's.
    std::size_t const kept_count = *kept.rbegin() + 1;
    std::map<std::string, Bytes> tables;
    Bytes glyf, loca;
    for (std::size_t g = 0; g <= kept_count; ++g) {
        put32(loca, static_cast<std::uint32_t>(glyf.size()));
        if (g == kept_count)
            break;
        if (kept.contains(g)) {
            auto const [at, length] = span(g);
            glyf.insert(glyf.end(), b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + length));
            while (glyf.size() % 4)
                glyf.push_back(0);
        }
    }
    tables["glyf"] = std::move(glyf);
    tables["loca"] = std::move(loca);
    Bytes head(b.begin() + head_at, b.begin() + head_at + head_length);
    head[50] = 0;
    head[51] = 1; // indexToLocFormat: long offsets
    tables["head"] = std::move(head);
    for (char const* tag : { "hhea", "maxp", "cvt ", "fpgm", "prep" }) {
        auto const [at, length] = table(tag);
        if (length)
            tables[tag] = Bytes(b.begin() + at, b.begin() + at + length);
    }
    // maxp's numGlyphs and hhea's numberOfHMetrics for the shorter font;
    // hmtx's metrics for the glyphs it keeps (a long metric each up to
    // numberOfHMetrics, a side bearing after).
    auto const [hhea_at, hhea_length] = table("hhea");
    auto const [hmtx_at, hmtx_length] = table("hmtx");
    if (hhea_length < 36 || tables["maxp"].size() < 6 || !hmtx_length)
        return {};
    std::size_t const metrics = std::min<std::size_t>(u16_at(b, hhea_at + 34), kept_count);
    tables["maxp"][4] = static_cast<std::uint8_t>(kept_count >> 8);
    tables["maxp"][5] = static_cast<std::uint8_t>(kept_count & 0xFF);
    tables["hhea"][34] = static_cast<std::uint8_t>(metrics >> 8);
    tables["hhea"][35] = static_cast<std::uint8_t>(metrics & 0xFF);
    std::size_t const original_metrics = u16_at(b, hhea_at + 34);
    Bytes hmtx;
    for (std::size_t g = 0; g < kept_count; ++g) {
        if (g < metrics) {
            for (std::size_t k = 0; k < 4; ++k)
                hmtx.push_back(static_cast<std::uint8_t>(u8_at(b, hmtx_at + 4 * g + k)));
        } else {
            std::size_t const at = hmtx_at + 4 * original_metrics + 2 * (g - original_metrics);
            hmtx.push_back(static_cast<std::uint8_t>(u8_at(b, at)));
            hmtx.push_back(static_cast<std::uint8_t>(u8_at(b, at + 1)));
        }
    }
    tables["hmtx"] = std::move(hmtx);
    return write_sfnt(std::move(tables));
}

// A CID-keyed CFF of the given glyphs (and .notdef), glyph i of the
// subset being CID glyphs[i] — the face's own glyph id — through a format
// 0 charset, so the codes the content uses are the same whatever else a
// subset holds. The subroutines keep their numbers: the ones no kept
// glyph calls are emptied to a lone `return`, so no charstring is
// rewritten. A name-keyed font becomes CID-keyed with one font dictionary
// holding its Private DICT.
Bytes SystemFace::cff_subset(std::vector<std::uint16_t> const& glyphs_in) const
{
    Bytes const& b = m_bytes;
    std::size_t const base = m_cff.first, limit = m_cff.first + m_cff.second;
    if (!m_cff.second || u8_at(b, base) != 1)
        return {};
    std::size_t const header_size = u8_at(b, base + 2);
    CffIndex const names = read_index(b, base + header_size, limit);
    if (!names.ok)
        return {};
    CffIndex const top_index = read_index(b, names.end, limit);
    if (!top_index.ok || top_index.count < 1)
        return {};
    CffIndex const strings = read_index(b, top_index.end, limit);
    if (!strings.ok)
        return {};
    CffIndex const global = read_index(b, strings.end, limit);
    if (!global.ok)
        return {};
    auto const [top_at, top_length] = index_item(b, top_index, 0);
    std::vector<DictEntry> const top = read_dict(b, top_at, top_at + top_length);
    DictEntry const* charstrings_entry = find_entry(top, 17);
    if (!charstrings_entry || charstrings_entry->operands.empty())
        return {};
    CffIndex const charstrings = read_index(b, base + static_cast<std::size_t>(charstrings_entry->operands[0]), limit);
    if (!charstrings.ok)
        return {};
    bool const cid = find_entry(top, 1230) != nullptr;

    // The font dictionaries: each with its entries and its Private DICT.
    struct FontDict {
        std::vector<DictEntry> entries; // without Private
        std::vector<DictEntry> priv; // without Subrs
        CffIndex subrs;
    };
    std::vector<FontDict> fds;
    auto const read_private = [&](std::vector<DictEntry> const& dict, FontDict& fd) {
        DictEntry const* p = find_entry(dict, 18);
        if (!p || p->operands.size() < 2)
            return;
        std::size_t const at = base + static_cast<std::size_t>(p->operands[1]);
        std::size_t const size = static_cast<std::size_t>(p->operands[0]);
        if (at + size > limit)
            return;
        for (DictEntry const& e : read_dict(b, at, at + size)) {
            if (e.op == 19) {
                if (!e.operands.empty())
                    fd.subrs = read_index(b, at + static_cast<std::size_t>(e.operands[0]), limit);
            } else {
                fd.priv.push_back(e);
            }
        }
    };
    std::vector<std::uint8_t> fd_of_glyph(charstrings.count, 0);
    if (cid) {
        DictEntry const* array = find_entry(top, 1236);
        DictEntry const* select = find_entry(top, 1237);
        if (!array || array->operands.empty() || !select || select->operands.empty())
            return {};
        CffIndex const fd_index = read_index(b, base + static_cast<std::size_t>(array->operands[0]), limit);
        if (!fd_index.ok || fd_index.count == 0 || fd_index.count > 255)
            return {};
        for (std::size_t i = 0; i < fd_index.count; ++i) {
            auto const [at, length] = index_item(b, fd_index, i);
            FontDict fd;
            for (DictEntry const& e : read_dict(b, at, at + length)) {
                if (e.op != 18 && e.op != 1238)
                    fd.entries.push_back(e);
            }
            read_private(read_dict(b, at, at + length), fd);
            fds.push_back(std::move(fd));
        }
        std::size_t const s = base + static_cast<std::size_t>(select->operands[0]);
        std::uint32_t const format = u8_at(b, s);
        if (format == 0) {
            for (std::size_t g = 0; g < charstrings.count; ++g)
                fd_of_glyph[g] = static_cast<std::uint8_t>(u8_at(b, s + 1 + g));
        } else if (format == 3) {
            std::uint32_t const ranges = u16_at(b, s + 1);
            for (std::uint32_t r = 0; r < ranges; ++r) {
                std::uint32_t const first = u16_at(b, s + 3 + 3 * r), fd = u8_at(b, s + 5 + 3 * r);
                std::uint32_t const next = u16_at(b, s + 6 + 3 * r);
                for (std::uint32_t g = first; g < next && g < charstrings.count; ++g)
                    fd_of_glyph[g] = static_cast<std::uint8_t>(fd);
            }
        } else {
            return {};
        }
        for (std::uint8_t& fd : fd_of_glyph) {
            if (fd >= fds.size())
                fd = 0;
        }
    } else {
        FontDict fd;
        read_private(top, fd);
        fds.push_back(std::move(fd));
    }

    std::set<std::uint16_t> wanted(glyphs_in.begin(), glyphs_in.end());
    wanted.insert(0);
    std::vector<std::uint16_t> glyphs;
    for (std::uint16_t const g : wanted) {
        if (g < charstrings.count)
            glyphs.push_back(g);
    }

    // The kept glyphs with their subroutines written into them, so the
    // subset carries no subroutines (a CJK face's run to tens of thousands
    // per font dictionary); where one cannot be followed, the glyphs as
    // they are with every subroutine.
    std::vector<Bytes> charstring_items;
    bool flattened = true;
    for (std::uint16_t const g : glyphs) {
        FontDict const& fd = fds[fd_of_glyph[g]];
        Bytes flat;
        SubrWalk walk { b, global, fd.subrs.ok ? &fd.subrs : nullptr, &flat, {}, 0, 0, false, 0 };
        auto const [at, length] = index_item(b, charstrings, g);
        if (!walk.run(at, at + length, 0) || flat.size() > 65535) {
            flattened = false;
            break;
        }
        charstring_items.push_back(std::move(flat));
    }
    if (!flattened) {
        charstring_items.clear();
        for (std::uint16_t const g : glyphs) {
            auto const [at, length] = index_item(b, charstrings, g);
            charstring_items.emplace_back(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + length));
        }
    }
    auto const all_subrs = [&](CffIndex const& index) {
        std::vector<Bytes> items;
        for (std::size_t i = 0; i < index.count; ++i) {
            auto const [at, length] = index_item(b, index, i);
            items.emplace_back(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + length));
        }
        return write_index(items);
    };

    // The pieces, then the offsets that tie them together.
    std::string const font_name = m_postscript_name.empty() ? "Font" : m_postscript_name;
    Bytes const name_index = write_index({ Bytes(font_name.begin(), font_name.end()) });
    Bytes const string_index = write_index({ Bytes { 'A', 'd', 'o', 'b', 'e' }, Bytes { 'I', 'd', 'e', 'n', 't', 'i', 't', 'y' } });
    Bytes const gsubrs = flattened ? write_index({}) : all_subrs(global);
    Bytes charset { 0 };
    for (std::size_t i = 1; i < glyphs.size(); ++i)
        put16(charset, glyphs[i]);
    Bytes fdselect { 0 };
    for (std::uint16_t const g : glyphs)
        fdselect.push_back(fd_of_glyph[g]);
    Bytes const charstrings_out = write_index(charstring_items);
    std::vector<Bytes> privates, local_subrs;
    for (std::size_t i = 0; i < fds.size(); ++i) {
        Bytes priv;
        for (DictEntry const& e : fds[i].priv) {
            priv.insert(priv.end(), e.raw.begin(), e.raw.end());
            put_op(priv, e.op);
        }
        Bytes subrs;
        if (!flattened && fds[i].subrs.ok && fds[i].subrs.count) {
            subrs = all_subrs(fds[i].subrs);
            // Subrs: relative to the Private DICT, which ends with it.
            put_dict_int(priv, static_cast<std::int32_t>(priv.size() + 6));
            put_op(priv, 19);
        }
        privates.push_back(std::move(priv));
        local_subrs.push_back(std::move(subrs));
    }

    std::uint32_t max_cid = glyphs.back();
    // The Top DICT: ROS, the numeric entries kept, then the offsets.
    auto const make_top = [&](std::size_t charset_at, std::size_t fdselect_at, std::size_t charstrings_at, std::size_t fdarray_at) {
        Bytes out;
        put_dict_int(out, 391);
        put_dict_int(out, 392);
        put_dict_int(out, 0);
        put_op(out, 1230);
        for (DictEntry const& e : top) {
            static constexpr int dropped[] = { 0, 1, 2, 3, 4, 13, 14, 15, 16, 17, 18, 1200, 1220, 1221, 1222, 1230, 1234, 1235, 1236, 1237, 1238 };
            if (std::find(std::begin(dropped), std::end(dropped), e.op) != std::end(dropped))
                continue;
            out.insert(out.end(), e.raw.begin(), e.raw.end());
            put_op(out, e.op);
        }
        put_dict_int(out, static_cast<std::int32_t>(max_cid + 1));
        put_op(out, 1234);
        put_dict_int(out, static_cast<std::int32_t>(charset_at));
        put_op(out, 15);
        put_dict_int(out, static_cast<std::int32_t>(charstrings_at));
        put_op(out, 17);
        put_dict_int(out, static_cast<std::int32_t>(fdarray_at));
        put_op(out, 1236);
        put_dict_int(out, static_cast<std::int32_t>(fdselect_at));
        put_op(out, 1237);
        return out;
    };
    auto const make_fdarray = [&](std::size_t privates_at) {
        std::vector<Bytes> items;
        std::size_t at = privates_at;
        for (std::size_t i = 0; i < fds.size(); ++i) {
            Bytes fd;
            for (DictEntry const& e : fds[i].entries) {
                fd.insert(fd.end(), e.raw.begin(), e.raw.end());
                put_op(fd, e.op);
            }
            put_dict_int(fd, static_cast<std::int32_t>(privates[i].size()));
            put_dict_int(fd, static_cast<std::int32_t>(at));
            put_op(fd, 18);
            at += privates[i].size() + local_subrs[i].size();
            items.push_back(std::move(fd));
        }
        return write_index(items);
    };
    Bytes const top_size_probe = write_index({ make_top(0, 0, 0, 0) });
    std::size_t at = 4 + name_index.size() + top_size_probe.size() + string_index.size() + gsubrs.size();
    std::size_t const charset_at = at;
    at += charset.size();
    std::size_t const fdselect_at = at;
    at += fdselect.size();
    std::size_t const charstrings_at = at;
    at += charstrings_out.size();
    std::size_t const fdarray_at = at;
    Bytes const fdarray_probe = make_fdarray(0);
    std::size_t const privates_at = fdarray_at + fdarray_probe.size();
    Bytes const fdarray = make_fdarray(privates_at);
    Bytes const top_index_out = write_index({ make_top(charset_at, fdselect_at, charstrings_at, fdarray_at) });

    Bytes out { 1, 0, 4, 4 };
    for (Bytes const* piece : std::array<Bytes const*, 8> { &name_index, &top_index_out, &string_index, &gsubrs, &charset, &fdselect, &charstrings_out, &fdarray })
        out.insert(out.end(), piece->begin(), piece->end());
    for (std::size_t i = 0; i < fds.size(); ++i) {
        out.insert(out.end(), privates[i].begin(), privates[i].end());
        out.insert(out.end(), local_subrs[i].begin(), local_subrs[i].end());
    }
    return out;
}

// --- The PDF objects ---

std::string to_unicode_cmap(std::map<std::uint16_t, std::u32string> const& codes)
{
    auto const hex16 = [](std::uint32_t v) {
        char buffer[8];
        std::snprintf(buffer, sizeof buffer, "%04X", v & 0xFFFF);
        return std::string(buffer);
    };
    std::string out = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                      "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                      "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
                      "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    std::vector<std::pair<std::uint16_t, std::u32string>> entries;
    for (auto const& [code, text] : codes) {
        if (!text.empty())
            entries.emplace_back(code, text);
    }
    // At most a hundred entries a block (§9.10.3, as for any CMap).
    for (std::size_t i = 0; i < entries.size(); i += 100) {
        std::size_t const n = std::min<std::size_t>(100, entries.size() - i);
        out += std::to_string(n) + " beginbfchar\n";
        for (std::size_t k = i; k < i + n; ++k) {
            out += "<" + hex16(entries[k].first) + "> <";
            for (char32_t const c : entries[k].second) {
                if (c >= 0x10000) {
                    std::uint32_t const v = c - 0x10000;
                    out += hex16(0xD800 + (v >> 10)) + hex16(0xDC00 + (v & 0x3FF));
                } else {
                    out += hex16(c);
                }
            }
            out += ">\n";
        }
        out += "endbfchar\n";
    }
    out += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    return out;
}

Object embed_type0_font(SystemFace const& face, std::map<std::uint16_t, std::u32string> const& glyphs, std::function<Object(Object)> const& place,
    bool identity_crypt)
{
    auto const stream_object = [&](Dict dict, Bytes data) {
        dict.set("Length", Object::integer(static_cast<std::int64_t>(data.size())));
        if (identity_crypt) {
            dict.set("Filter", Object::array({ Object::name("Crypt") }));
            Dict parms;
            parms.set("Name", Object::name("Identity"));
            dict.set("DecodeParms", Object::array({ Object::dict(std::move(parms)) }));
        }
        Stream stream;
        stream.dict = std::move(dict);
        auto const source = std::make_shared<Bytes const>(std::move(data));
        stream.source = source;
        stream.start = 0;
        stream.length = source->size();
        return Object::stream(std::move(stream));
    };
    std::vector<std::uint16_t> ids;
    for (auto const& [glyph, text] : glyphs)
        ids.push_back(glyph);
    // The subset's tag (§9.6.4): six capitals from the glyphs it holds.
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint16_t const g : ids)
        hash = (hash ^ g) * 1099511628211ull;
    for (char const c : face.key())
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    std::string name;
    for (int i = 0; i < 6; ++i) {
        name.push_back(static_cast<char>('A' + hash % 26));
        hash /= 26;
    }
    name += "+" + face.postscript_name();

    bool const cff = face.has_cff();
    Bytes program = cff ? face.cff_subset(ids) : face.truetype_subset(ids);
    Dict file_dict;
    if (cff)
        file_dict.set("Subtype", Object::name("CIDFontType0C"));
    else
        file_dict.set("Length1", Object::integer(static_cast<std::int64_t>(program.size())));
    Object const file = place(stream_object(std::move(file_dict), std::move(program)));

    SystemFace::Metrics const& m = face.metrics();
    Dict descriptor;
    descriptor.set("Type", Object::name("FontDescriptor"));
    descriptor.set("FontName", Object::name(name));
    descriptor.set("Flags", Object::integer(m.flags));
    descriptor.set("FontBBox", Object::array({ Object::integer(static_cast<std::int64_t>(m.bbox[0])), Object::integer(static_cast<std::int64_t>(m.bbox[1])),
                                   Object::integer(static_cast<std::int64_t>(m.bbox[2])), Object::integer(static_cast<std::int64_t>(m.bbox[3])) }));
    descriptor.set("ItalicAngle", Object::real(m.italic_angle));
    descriptor.set("Ascent", Object::integer(static_cast<std::int64_t>(m.ascent)));
    descriptor.set("Descent", Object::integer(static_cast<std::int64_t>(m.descent)));
    descriptor.set("CapHeight", Object::integer(static_cast<std::int64_t>(m.cap_height)));
    descriptor.set("StemV", Object::integer(static_cast<std::int64_t>(m.stem_v)));
    descriptor.set(cff ? "FontFile3" : "FontFile2", file);
    Object const descriptor_ref = place(Object::dict(std::move(descriptor)));

    // /W: runs of consecutive CIDs, each run's widths in one array.
    Array widths;
    for (std::size_t i = 0; i < ids.size();) {
        std::size_t j = i;
        Array run;
        while (j < ids.size() && (j == i || ids[j] == ids[j - 1] + 1)) {
            run.push_back(Object::integer(face.advance(ids[j])));
            ++j;
        }
        widths.push_back(Object::integer(ids[i]));
        widths.push_back(Object::array(std::move(run)));
        i = j;
    }
    Dict info;
    info.set("Registry", Object::string("Adobe"));
    info.set("Ordering", Object::string("Identity"));
    info.set("Supplement", Object::integer(0));
    Dict cid_font;
    cid_font.set("Type", Object::name("Font"));
    cid_font.set("Subtype", Object::name(cff ? "CIDFontType0" : "CIDFontType2"));
    cid_font.set("BaseFont", Object::name(name));
    cid_font.set("CIDSystemInfo", Object::dict(std::move(info)));
    cid_font.set("FontDescriptor", descriptor_ref);
    cid_font.set("DW", Object::integer(1000));
    cid_font.set("W", Object::array(std::move(widths)));
    if (!cff)
        cid_font.set("CIDToGIDMap", Object::name("Identity"));
    Object const cid_ref = place(Object::dict(std::move(cid_font)));

    std::string const cmap = to_unicode_cmap(glyphs);
    Object const to_unicode = place(stream_object(Dict {}, Bytes(cmap.begin(), cmap.end())));

    Dict type0;
    type0.set("Type", Object::name("Font"));
    type0.set("Subtype", Object::name("Type0"));
    type0.set("BaseFont", Object::name(cff ? name + "-Identity-H" : name));
    type0.set("Encoding", Object::name("Identity-H"));
    type0.set("DescendantFonts", Object::array({ cid_ref }));
    type0.set("ToUnicode", to_unicode);
    return Object::dict(std::move(type0));
}

}
