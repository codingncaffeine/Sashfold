#include "text/TrueType.h"

#include "core/Unicode.h"

#include <algorithm>
#include <utility>

namespace sashfold::text {

namespace {

constexpr std::uint32_t make_tag(char a, char b, char c, char d)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24
        | static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16
        | static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8
        | static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

constexpr std::uint32_t tag_ttcf = make_tag('t', 't', 'c', 'f');
constexpr std::uint32_t tag_true = make_tag('t', 'r', 'u', 'e');
constexpr std::uint32_t tag_otto = make_tag('O', 'T', 'T', 'O');
constexpr std::uint32_t tag_head = make_tag('h', 'e', 'a', 'd');
constexpr std::uint32_t tag_hhea = make_tag('h', 'h', 'e', 'a');
constexpr std::uint32_t tag_hmtx = make_tag('h', 'm', 't', 'x');
constexpr std::uint32_t tag_maxp = make_tag('m', 'a', 'x', 'p');
constexpr std::uint32_t tag_loca = make_tag('l', 'o', 'c', 'a');
constexpr std::uint32_t tag_glyf = make_tag('g', 'l', 'y', 'f');
constexpr std::uint32_t tag_cmap = make_tag('c', 'm', 'a', 'p');
constexpr std::uint32_t tag_name = make_tag('n', 'a', 'm', 'e');
constexpr std::uint32_t tag_os2 = make_tag('O', 'S', '/', '2');
constexpr std::uint32_t tag_cff = make_tag('C', 'F', 'F', ' ');

// Nothing a font can say moves an offset past this: keeps every table
// offset inside 32 bits and a hostile file from asking for the world.
constexpr std::size_t max_font_bytes = 256u * 1024u * 1024u;
constexpr int max_composite_depth = 8;
constexpr int max_components = 256;
constexpr std::size_t max_points = 65535;

// Simple-glyph flag bits.
constexpr std::uint8_t flag_on_curve = 0x01;
constexpr std::uint8_t flag_x_short = 0x02;
constexpr std::uint8_t flag_y_short = 0x04;
constexpr std::uint8_t flag_repeat = 0x08;
constexpr std::uint8_t flag_x_same_or_positive = 0x10;
constexpr std::uint8_t flag_y_same_or_positive = 0x20;

// Composite component flag bits.
constexpr std::uint16_t comp_args_are_words = 0x0001;
constexpr std::uint16_t comp_args_are_xy = 0x0002;
constexpr std::uint16_t comp_have_scale = 0x0008;
constexpr std::uint16_t comp_more_components = 0x0020;
constexpr std::uint16_t comp_have_xy_scale = 0x0040;
constexpr std::uint16_t comp_have_two_by_two = 0x0080;
constexpr std::uint16_t comp_scaled_offset = 0x0800;
constexpr std::uint16_t comp_unscaled_offset = 0x1000;

// OS/2 fsSelection bits.
constexpr std::uint16_t selection_italic = 0x0001;
constexpr std::uint16_t selection_use_typo_metrics = 0x0080;

// head macStyle bits.
constexpr std::uint16_t mac_style_bold = 0x0001;
constexpr std::uint16_t mac_style_italic = 0x0002;

// Bounds-checked big-endian reads over the whole file: a read outside the
// file yields zero, and callers that must know call `has` first.
struct Reader {
    std::vector<std::uint8_t> const& bytes;

    bool has(std::size_t offset, std::size_t length) const
    {
        return offset <= bytes.size() && length <= bytes.size() - offset;
    }

    std::uint8_t u8(std::size_t at) const { return has(at, 1) ? bytes[at] : 0; }

    std::uint16_t u16(std::size_t at) const
    {
        if (!has(at, 2))
            return 0;
        return static_cast<std::uint16_t>(static_cast<unsigned>(bytes[at]) << 8
            | static_cast<unsigned>(bytes[at + 1]));
    }

    std::int16_t i16(std::size_t at) const { return static_cast<std::int16_t>(u16(at)); }

    std::uint32_t u32(std::size_t at) const
    {
        if (!has(at, 4))
            return 0;
        return static_cast<std::uint32_t>(bytes[at]) << 24
            | static_cast<std::uint32_t>(bytes[at + 1]) << 16
            | static_cast<std::uint32_t>(bytes[at + 2]) << 8
            | static_cast<std::uint32_t>(bytes[at + 3]);
    }
};

// A sequential reader over one span. Running off the end clears `ok` and
// every later read yields zero, so a parse checks `ok` once at the end.
class Cursor {
public:
    Cursor(Reader const& reader, std::size_t from, std::size_t to)
        : m_reader(reader)
        , m_pos(from)
        , m_end(std::min(to, reader.bytes.size()))
        , m_ok(from <= m_end)
    {
    }

    bool ok() const { return m_ok; }

    std::uint8_t u8()
    {
        if (!take(1))
            return 0;
        return m_reader.bytes[m_pos++];
    }
    std::int8_t i8() { return static_cast<std::int8_t>(u8()); }
    std::uint16_t u16()
    {
        if (!take(2))
            return 0;
        std::uint16_t const value = m_reader.u16(m_pos);
        m_pos += 2;
        return value;
    }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    void skip(std::size_t count)
    {
        if (take(count))
            m_pos += count;
    }

private:
    bool take(std::size_t count)
    {
        if (!m_ok || count > m_end - m_pos) {
            m_ok = false;
            return false;
        }
        return true;
    }

    Reader const& m_reader;
    std::size_t m_pos;
    std::size_t m_end;
    bool m_ok;
};

std::int16_t clamp16(long long value)
{
    return static_cast<std::int16_t>(std::clamp<long long>(value, -32768, 32767));
}

// F2Dot14 times an integer, rounded to nearest.
long long fixed_mul(long long fixed, long long value)
{
    long long const product = fixed * value;
    return (product + (product >= 0 ? 8192 : -8192)) / 16384;
}

} // namespace

// Appends code points [first, last] -> glyph.., merging with the previous run
// when the mapping simply continues it.
void TrueTypeFont::push_run(std::vector<Run>& runs, char32_t first, char32_t last,
    std::uint32_t glyph)
{
    if (!runs.empty()) {
        Run& back = runs.back();
        if (back.last + 1 == first && back.glyph + (first - back.first) == glyph) {
            back.last = last;
            return;
        }
    }
    runs.push_back(Run { first, last, glyph });
}

// A format 4 segment with idRangeOffset 0: glyph = (c + delta) mod 65536,
// which walks in step with c until it wraps, so the run splits there.
void TrueTypeFont::push_delta_run(std::vector<Run>& runs, char32_t first, char32_t last,
    std::uint16_t delta)
{
    std::uint32_t const first_glyph = (first + delta) & 0xFFFFu;
    std::uint32_t const span = last - first;
    if (first_glyph + span <= 0xFFFFu) {
        push_run(runs, first, last, first_glyph);
        return;
    }
    char32_t const wrap_at = first + (0xFFFFu - first_glyph);
    push_run(runs, first, wrap_at, first_glyph);
    push_run(runs, wrap_at + 1, last, 0);
}

std::size_t TrueTypeFont::face_count(std::vector<std::uint8_t> const& bytes)
{
    if (bytes.size() < 12 || bytes.size() > max_font_bytes)
        return 0;
    Reader const reader { bytes };
    std::uint32_t const version = reader.u32(0);
    if (version == tag_ttcf) {
        std::size_t const declared = reader.u32(8);
        return std::min(declared, (bytes.size() - 12) / 4);
    }
    if (version == 0x00010000 || version == tag_true || version == tag_otto)
        return 1;
    return 0;
}

std::optional<TrueTypeFont> TrueTypeFont::parse(std::vector<std::uint8_t> bytes,
    std::size_t face_index)
{
    if (bytes.size() < 12 || bytes.size() > max_font_bytes)
        return std::nullopt;
    TrueTypeFont font;
    font.m_bytes = std::move(bytes);
    if (!font.load(face_index))
        return std::nullopt;
    return std::optional<TrueTypeFont>(std::move(font));
}

bool TrueTypeFont::load(std::size_t face_index)
{
    Reader const reader { m_bytes };
    std::uint32_t directory = 0;
    if (reader.u32(0) == tag_ttcf) {
        if (face_index >= face_count(m_bytes))
            return false;
        std::size_t const at = 12 + face_index * 4;
        if (!reader.has(at, 4))
            return false;
        directory = reader.u32(at);
    } else if (face_index != 0) {
        return false;
    }
    if (!load_directory(directory))
        return false;
    if (!load_head_and_metrics())
        return false;
    if (!load_cmap())
        return false;
    load_names();
    load_os2();
    return true;
}

bool TrueTypeFont::load_directory(std::uint32_t offset)
{
    Reader const reader { m_bytes };
    if (!reader.has(offset, 12))
        return false;
    std::uint32_t const version = reader.u32(offset);
    if (version != 0x00010000 && version != tag_true && version != tag_otto)
        return false;
    std::uint16_t const table_count = reader.u16(offset + 4);
    if (table_count == 0 || !reader.has(offset + 12, static_cast<std::size_t>(table_count) * 16))
        return false;
    for (std::uint16_t i = 0; i < table_count; ++i) {
        std::size_t const record = offset + 12 + static_cast<std::size_t>(i) * 16;
        std::uint32_t const tag = reader.u32(record);
        std::uint32_t const table_offset = reader.u32(record + 8);
        std::uint32_t const table_length = reader.u32(record + 12);
        if (table_length == 0 || table_offset >= m_bytes.size())
            continue; // an empty or absent table is simply not there
        Table const table { table_offset,
            std::min(table_length, static_cast<std::uint32_t>(m_bytes.size() - table_offset)) };
        switch (tag) {
        case tag_head: m_head = table; break;
        case tag_hhea: m_hhea = table; break;
        case tag_hmtx: m_hmtx = table; break;
        case tag_maxp: m_maxp = table; break;
        case tag_loca: m_loca = table; break;
        case tag_glyf: m_glyf = table; break;
        case tag_cmap: m_cmap = table; break;
        case tag_name: m_name = table; break;
        case tag_os2: m_os2 = table; break;
        case tag_cff: m_has_cff = true; break;
        default: break;
        }
    }
    return true;
}

bool TrueTypeFont::load_head_and_metrics()
{
    Reader const reader { m_bytes };
    if (m_head.length < 54 || m_maxp.length < 6 || m_hhea.length < 36 || !m_hmtx.present())
        return false;

    m_units_per_em = reader.u16(m_head.offset + 18);
    if (m_units_per_em < 16 || m_units_per_em > 16384)
        return false;
    m_mac_style = reader.u16(m_head.offset + 44);
    std::int16_t const index_to_loc = reader.i16(m_head.offset + 50);
    if (index_to_loc != 0 && index_to_loc != 1)
        return false;
    m_long_loca = index_to_loc == 1;

    m_glyph_count = reader.u16(m_maxp.offset + 4);
    if (m_glyph_count == 0)
        return false;

    m_ascender = reader.i16(m_hhea.offset + 4);
    m_descender = reader.i16(m_hhea.offset + 6);
    m_line_gap = reader.i16(m_hhea.offset + 8);
    m_metric_count = reader.u16(m_hhea.offset + 34);
    if (m_metric_count == 0)
        return false;
    m_metric_count = std::min(m_metric_count, m_glyph_count);

    m_has_glyf = m_glyf.present() && m_loca.present();
    m_weight_class = (m_mac_style & mac_style_bold) ? 700 : 400;
    m_italic = (m_mac_style & mac_style_italic) != 0;
    return true;
}

bool TrueTypeFont::load_cmap()
{
    if (!m_cmap.present())
        return true; // metrics and outlines still serve; nothing maps
    Reader const reader { m_bytes };
    std::size_t const base = m_cmap.offset;
    std::size_t const end = base + m_cmap.length;
    if (m_cmap.length < 4)
        return true;
    std::size_t const record_count = std::min<std::size_t>(reader.u16(base + 2), (m_cmap.length - 4) / 8);

    struct Candidate {
        int score;
        std::size_t offset;
    };
    std::vector<Candidate> candidates;
    for (std::size_t i = 0; i < record_count; ++i) {
        std::size_t const record = base + 4 + i * 8;
        std::uint16_t const platform = reader.u16(record);
        std::uint16_t const encoding = reader.u16(record + 2);
        std::uint32_t const offset = reader.u32(record + 4);
        int score = 0;
        if (platform == 0)
            score = encoding == 5 ? 0 : 3; // Unicode; 5 is variation sequences
        else if (platform == 3)
            score = encoding == 10 ? 4 : encoding == 1 ? 2 : encoding == 0 ? 1 : 0;
        if (score == 0 || offset >= m_cmap.length)
            continue;
        candidates.push_back(Candidate { score, base + offset });
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](Candidate const& a, Candidate const& b) { return a.score > b.score; });

    for (Candidate const& candidate : candidates) {
        std::vector<Run> runs;
        if (candidate.offset >= end || !load_cmap_subtable(candidate.offset, runs) || runs.empty())
            continue;
        std::stable_sort(runs.begin(), runs.end(),
            [](Run const& a, Run const& b) { return a.first < b.first; });
        // Overlaps are a malformed font's problem to lose: the earlier run wins.
        for (Run run : runs) {
            if (!m_runs.empty() && run.first <= m_runs.back().last) {
                char32_t const first = m_runs.back().last + 1;
                if (first > run.last)
                    continue;
                run.glyph += first - run.first;
                run.first = first;
                if (run.glyph > 0xFFFFu)
                    continue;
            }
            push_run(m_runs, run.first, run.last, run.glyph);
        }
        if (!m_runs.empty())
            return true;
    }
    return true;
}

bool TrueTypeFont::load_cmap_subtable(std::size_t at, std::vector<Run>& runs) const
{
    Reader const reader { m_bytes };
    std::size_t const end = static_cast<std::size_t>(m_cmap.offset) + m_cmap.length;
    if (at + 4 > end)
        return false;
    std::uint16_t const format = reader.u16(at);
    switch (format) {
    case 0: {
        if (at + 6 + 256 > end)
            return false;
        for (char32_t c = 0; c < 256; ++c) {
            if (std::uint8_t const glyph = reader.u8(at + 6 + c))
                push_run(runs, c, c, glyph);
        }
        return true;
    }
    case 4: {
        std::size_t const segment_count = reader.u16(at + 6) / 2;
        if (segment_count == 0 || at + 16 + segment_count * 8 > end)
            return false;
        std::size_t const ends = at + 14;
        std::size_t const starts = ends + segment_count * 2 + 2;
        std::size_t const deltas = starts + segment_count * 2;
        std::size_t const range_offsets = deltas + segment_count * 2;
        for (std::size_t i = 0; i < segment_count; ++i) {
            char32_t const last = reader.u16(ends + i * 2);
            char32_t const first = reader.u16(starts + i * 2);
            std::uint16_t const delta = reader.u16(deltas + i * 2);
            std::size_t const range_offset_at = range_offsets + i * 2;
            std::uint16_t const range_offset = reader.u16(range_offset_at);
            if (first > last)
                continue;
            if (range_offset == 0) {
                push_delta_run(runs, first, last, delta);
                continue;
            }
            for (char32_t c = first; c <= last; ++c) {
                std::size_t const glyph_at = range_offset_at + range_offset + (c - first) * 2;
                if (glyph_at + 2 > end)
                    break; // the array runs off the table: the rest is unmapped
                std::uint32_t glyph = reader.u16(glyph_at);
                if (glyph != 0)
                    glyph = (glyph + delta) & 0xFFFFu;
                if (glyph != 0)
                    push_run(runs, c, c, glyph);
                if (c == 0xFFFF)
                    break;
            }
        }
        return true;
    }
    case 6: {
        if (at + 10 > end)
            return false;
        char32_t const first = reader.u16(at + 6);
        std::size_t const count = std::min<std::size_t>(reader.u16(at + 8), (end - (at + 10)) / 2);
        for (std::size_t i = 0; i < count; ++i) {
            if (std::uint16_t const glyph = reader.u16(at + 10 + i * 2))
                push_run(runs, first + static_cast<char32_t>(i), first + static_cast<char32_t>(i), glyph);
        }
        return true;
    }
    case 12: {
        if (at + 16 > end)
            return false;
        std::size_t const group_count = std::min<std::size_t>(reader.u32(at + 12), (end - (at + 16)) / 12);
        for (std::size_t i = 0; i < group_count; ++i) {
            std::size_t const group = at + 16 + i * 12;
            std::uint32_t const first = reader.u32(group);
            std::uint32_t last = reader.u32(group + 4);
            std::uint32_t const glyph = reader.u32(group + 8);
            if (first > last || first > 0x10FFFF || glyph > 0xFFFFu)
                continue;
            last = std::min<std::uint32_t>(last, 0x10FFFF);
            if (glyph + (last - first) > 0xFFFFu)
                last = first + (0xFFFFu - glyph);
            push_run(runs, first, last, glyph);
        }
        return true;
    }
    default:
        return false;
    }
}

void TrueTypeFont::load_names()
{
    m_family = name_string(16);
    if (m_family.empty())
        m_family = name_string(1);
    m_subfamily = name_string(17);
    if (m_subfamily.empty())
        m_subfamily = name_string(2);
}

std::string TrueTypeFont::name_string(std::uint16_t name_id) const
{
    if (m_name.length < 6)
        return {};
    Reader const reader { m_bytes };
    std::size_t const base = m_name.offset;
    std::size_t const end = base + m_name.length;
    std::size_t const count = std::min<std::size_t>(reader.u16(base + 2), (m_name.length - 6) / 12);
    std::size_t const storage = base + reader.u16(base + 4);
    int best_score = 0;
    std::string best;
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t const record = base + 6 + i * 12;
        std::uint16_t const platform = reader.u16(record);
        std::uint16_t const encoding = reader.u16(record + 2);
        std::uint16_t const language = reader.u16(record + 4);
        if (reader.u16(record + 6) != name_id)
            continue;
        std::size_t const length = reader.u16(record + 8);
        std::size_t const at = storage + reader.u16(record + 10);
        int score = 0;
        bool utf16 = true;
        if (platform == 3 && (encoding == 1 || encoding == 10))
            score = language == 0x0409 ? 6 : 5;
        else if (platform == 0)
            score = 4;
        else if (platform == 3)
            score = 3;
        else if (platform == 1 && encoding == 0) {
            score = language == 0 ? 2 : 1;
            utf16 = false;
        }
        if (score <= best_score || at + length > end)
            continue;
        std::string text;
        if (utf16) {
            for (std::size_t j = 0; j + 1 < length; j += 2) {
                char32_t unit = reader.u16(at + j);
                if (unit >= 0xD800 && unit <= 0xDBFF && j + 3 < length) {
                    char32_t const low = reader.u16(at + j + 2);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                        j += 2;
                    }
                }
                append_utf8(text, is_surrogate(unit) ? 0xFFFD : unit);
            }
        } else {
            for (std::size_t j = 0; j < length; ++j) {
                std::uint8_t const byte = reader.u8(at + j);
                if (byte < 0x80)
                    text.push_back(static_cast<char>(byte));
            }
        }
        if (text.empty())
            continue;
        best = std::move(text);
        best_score = score;
    }
    return best;
}

void TrueTypeFont::load_os2()
{
    if (m_os2.length < 78)
        return;
    Reader const reader { m_bytes };
    std::size_t const base = m_os2.offset;
    std::uint16_t const weight = reader.u16(base + 4);
    if (weight >= 1 && weight <= 1000)
        m_weight_class = weight;
    std::uint16_t const selection = reader.u16(base + 62);
    if (selection & selection_italic)
        m_italic = true;
    if (selection & selection_use_typo_metrics) {
        std::int16_t const typo_ascender = reader.i16(base + 68);
        std::int16_t const typo_descender = reader.i16(base + 70);
        if (typo_ascender != 0 || typo_descender != 0) {
            m_ascender = typo_ascender;
            m_descender = typo_descender;
            m_line_gap = reader.i16(base + 72);
        }
    }
    if (m_ascender == 0 && m_descender == 0) {
        // hhea said nothing: the Windows metrics are the last word.
        m_ascender = clamp16(reader.u16(base + 74));
        m_descender = clamp16(-static_cast<long long>(reader.u16(base + 76)));
    }
    if (m_os2.length >= 90) {
        m_x_height = reader.i16(base + 86);
        m_cap_height = reader.i16(base + 88);
    }
}

std::uint16_t TrueTypeFont::glyph_index(char32_t code_point) const
{
    auto const it = std::upper_bound(m_runs.begin(), m_runs.end(), code_point,
        [](char32_t c, Run const& run) { return c < run.first; });
    if (it == m_runs.begin())
        return 0;
    Run const& run = *(it - 1);
    if (code_point > run.last)
        return 0;
    std::uint32_t const glyph = run.glyph + (code_point - run.first);
    return glyph < m_glyph_count ? static_cast<std::uint16_t>(glyph) : 0;
}

std::size_t TrueTypeFont::mapped_code_points() const
{
    std::size_t total = 0;
    for (Run const& run : m_runs)
        total += run.last - run.first + 1;
    return total;
}

std::uint16_t TrueTypeFont::advance_width(std::uint16_t glyph) const
{
    if (m_metric_count == 0 || glyph >= m_glyph_count)
        return 0;
    std::size_t const index = std::min<std::size_t>(glyph, m_metric_count - 1u);
    std::size_t const at = index * 4;
    if (at + 2 > m_hmtx.length)
        return 0;
    return Reader { m_bytes }.u16(m_hmtx.offset + at);
}

std::int16_t TrueTypeFont::left_side_bearing(std::uint16_t glyph) const
{
    if (m_metric_count == 0 || glyph >= m_glyph_count)
        return 0;
    std::size_t const at = glyph < m_metric_count
        ? static_cast<std::size_t>(glyph) * 4 + 2
        : static_cast<std::size_t>(m_metric_count) * 4
            + static_cast<std::size_t>(glyph - m_metric_count) * 2;
    if (at + 2 > m_hmtx.length)
        return 0;
    return Reader { m_bytes }.i16(m_hmtx.offset + at);
}

bool TrueTypeFont::glyph_span(std::uint16_t glyph, std::uint32_t& offset, std::uint32_t& length) const
{
    if (!m_has_glyf || glyph >= m_glyph_count)
        return false;
    Reader const reader { m_bytes };
    std::uint32_t start;
    std::uint32_t next;
    if (m_long_loca) {
        std::size_t const at = static_cast<std::size_t>(glyph) * 4;
        if (at + 8 > m_loca.length)
            return false;
        start = reader.u32(m_loca.offset + at);
        next = reader.u32(m_loca.offset + at + 4);
    } else {
        std::size_t const at = static_cast<std::size_t>(glyph) * 2;
        if (at + 4 > m_loca.length)
            return false;
        start = static_cast<std::uint32_t>(reader.u16(m_loca.offset + at)) * 2;
        next = static_cast<std::uint32_t>(reader.u16(m_loca.offset + at + 2)) * 2;
    }
    if (next < start || start > m_glyf.length)
        return false;
    next = std::min(next, m_glyf.length);
    offset = m_glyf.offset + start;
    length = next - start;
    return true;
}

std::optional<GlyphOutline> TrueTypeFont::outline(std::uint16_t glyph) const
{
    GlyphOutline out;
    if (!outline_into(glyph, out, 0))
        return std::nullopt;
    if (!out.points.empty()) {
        out.x_min = out.x_max = out.points[0].x;
        out.y_min = out.y_max = out.points[0].y;
        for (GlyphPoint const& point : out.points) {
            out.x_min = std::min(out.x_min, point.x);
            out.x_max = std::max(out.x_max, point.x);
            out.y_min = std::min(out.y_min, point.y);
            out.y_max = std::max(out.y_max, point.y);
        }
    }
    return out;
}

bool TrueTypeFont::outline_into(std::uint16_t glyph, GlyphOutline& out, int depth) const
{
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    if (!glyph_span(glyph, offset, length))
        return false;
    if (length == 0)
        return true; // no contours at all: a space
    if (length < 10)
        return false;
    std::int16_t const contour_count = Reader { m_bytes }.i16(offset);
    return contour_count >= 0 ? parse_simple_glyph(offset, length, out)
                              : parse_composite_glyph(offset, length, out, depth);
}

bool TrueTypeFont::parse_simple_glyph(std::uint32_t offset, std::uint32_t length,
    GlyphOutline& out) const
{
    Reader const reader { m_bytes };
    Cursor cursor(reader, offset, static_cast<std::size_t>(offset) + length);
    std::int16_t const contour_count = cursor.i16();
    cursor.skip(8); // the file's bounding box: recomputed from the points
    if (contour_count < 0)
        return false;

    std::size_t const base = out.points.size();
    std::vector<std::uint16_t> ends;
    ends.reserve(static_cast<std::size_t>(contour_count));
    for (std::int16_t i = 0; i < contour_count; ++i) {
        std::uint16_t const end = cursor.u16();
        if (i > 0 && end <= ends.back())
            return false; // contours must advance
        ends.push_back(end);
    }
    std::size_t const point_count = ends.empty() ? 0 : static_cast<std::size_t>(ends.back()) + 1;
    if (base + point_count > max_points)
        return false;
    cursor.skip(cursor.u16()); // instructions: no hinting here
    if (!cursor.ok())
        return false;

    std::vector<std::uint8_t> flags;
    flags.reserve(point_count);
    while (flags.size() < point_count && cursor.ok()) {
        std::uint8_t const flag = cursor.u8();
        flags.push_back(flag);
        if (flag & flag_repeat) {
            std::size_t const repeats = std::min<std::size_t>(cursor.u8(), point_count - flags.size());
            flags.insert(flags.end(), repeats, flag);
        }
    }
    if (!cursor.ok())
        return false;

    out.points.resize(base + point_count);
    long long value = 0;
    for (std::size_t i = 0; i < point_count; ++i) {
        std::uint8_t const flag = flags[i];
        if (flag & flag_x_short) {
            long long const delta = cursor.u8();
            value += (flag & flag_x_same_or_positive) ? delta : -delta;
        } else if (!(flag & flag_x_same_or_positive)) {
            value += cursor.i16();
        }
        out.points[base + i].x = clamp16(value);
        out.points[base + i].on_curve = (flag & flag_on_curve) != 0;
    }
    value = 0;
    for (std::size_t i = 0; i < point_count; ++i) {
        std::uint8_t const flag = flags[i];
        if (flag & flag_y_short) {
            long long const delta = cursor.u8();
            value += (flag & flag_y_same_or_positive) ? delta : -delta;
        } else if (!(flag & flag_y_same_or_positive)) {
            value += cursor.i16();
        }
        out.points[base + i].y = clamp16(value);
    }
    if (!cursor.ok())
        return false;
    for (std::uint16_t const end : ends)
        out.contour_ends.push_back(static_cast<std::uint16_t>(base + end));
    return true;
}

bool TrueTypeFont::parse_composite_glyph(std::uint32_t offset, std::uint32_t length,
    GlyphOutline& out, int depth) const
{
    if (depth >= max_composite_depth)
        return false;
    Reader const reader { m_bytes };
    Cursor cursor(reader, static_cast<std::size_t>(offset) + 10,
        static_cast<std::size_t>(offset) + length);
    int components = 0;
    std::uint16_t flags = 0;
    do {
        if (++components > max_components)
            return false;
        flags = cursor.u16();
        std::uint16_t const component = cursor.u16();
        bool const xy_values = (flags & comp_args_are_xy) != 0;
        long long arg1;
        long long arg2;
        if (flags & comp_args_are_words) {
            arg1 = xy_values ? cursor.i16() : cursor.u16();
            arg2 = xy_values ? cursor.i16() : cursor.u16();
        } else {
            arg1 = xy_values ? cursor.i8() : cursor.u8();
            arg2 = xy_values ? cursor.i8() : cursor.u8();
        }
        // The 2x2 in F2Dot14: x' = a*x + c*y, y' = b*x + d*y, in the order
        // the file spells them (xscale, scale01, scale10, yscale).
        long long a = 16384;
        long long b = 0;
        long long c = 0;
        long long d = 16384;
        if (flags & comp_have_scale) {
            a = d = cursor.i16();
        } else if (flags & comp_have_xy_scale) {
            a = cursor.i16();
            d = cursor.i16();
        } else if (flags & comp_have_two_by_two) {
            a = cursor.i16();
            b = cursor.i16();
            c = cursor.i16();
            d = cursor.i16();
        }
        if (!cursor.ok())
            return false;

        GlyphOutline child;
        if (!outline_into(component, child, depth + 1))
            return false;
        bool const identity = a == 16384 && b == 0 && c == 0 && d == 16384;
        if (!identity) {
            for (GlyphPoint& point : child.points) {
                long long const x = point.x;
                long long const y = point.y;
                point.x = clamp16(fixed_mul(a, x) + fixed_mul(c, y));
                point.y = clamp16(fixed_mul(b, x) + fixed_mul(d, y));
            }
        }
        long long dx;
        long long dy;
        if (xy_values) {
            dx = arg1;
            dy = arg2;
            if (!identity && (flags & comp_scaled_offset) && !(flags & comp_unscaled_offset)) {
                dx = fixed_mul(a, arg1) + fixed_mul(c, arg2);
                dy = fixed_mul(b, arg1) + fixed_mul(d, arg2);
            }
        } else {
            // Point matching: parent point arg1 lands on child point arg2.
            if (static_cast<std::size_t>(arg1) >= out.points.size()
                || static_cast<std::size_t>(arg2) >= child.points.size())
                return false;
            dx = out.points[static_cast<std::size_t>(arg1)].x
                - child.points[static_cast<std::size_t>(arg2)].x;
            dy = out.points[static_cast<std::size_t>(arg1)].y
                - child.points[static_cast<std::size_t>(arg2)].y;
        }
        if (out.points.size() + child.points.size() > max_points)
            return false;
        std::size_t const base = out.points.size();
        for (GlyphPoint const& point : child.points)
            out.points.push_back(GlyphPoint { clamp16(point.x + dx), clamp16(point.y + dy), point.on_curve });
        for (std::uint16_t const end : child.contour_ends)
            out.contour_ends.push_back(static_cast<std::uint16_t>(base + end));
    } while (flags & comp_more_components);
    return true;
}

}
