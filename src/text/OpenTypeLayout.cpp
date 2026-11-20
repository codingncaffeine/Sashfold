#include "text/OpenTypeLayout.h"

#include "text/TrueType.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace sashfold::text::otl {

namespace {

// The longest input, backtrack or lookahead sequence a contextual rule may
// match, and how deep nested lookups and attachment chains are followed:
// the limits other engines use, so a hostile font cannot recurse without
// end and real fonts are read the way their makers tested them.
constexpr unsigned max_context_length = 64;
constexpr int max_nesting = 64;

}

// --- Coverage, ClassDef, digests ---------------------------------------------

int coverage_index(Bytes const& b, std::uint32_t coverage, std::uint32_t glyph)
{
    if (coverage == 0)
        return -1;
    std::uint16_t const format = b.u16(coverage);
    if (format == 1) {
        std::uint32_t const count = b.u16(coverage + 2);
        std::uint32_t lo = 0;
        std::uint32_t hi = count;
        while (lo < hi) {
            std::uint32_t const mid = (lo + hi) / 2;
            std::uint16_t const g = b.u16(coverage + 4 + mid * 2);
            if (glyph < g)
                hi = mid;
            else if (glyph > g)
                lo = mid + 1;
            else
                return b.has(coverage + 4 + mid * 2, 2) ? static_cast<int>(mid) : -1;
        }
        return -1;
    }
    if (format == 2) {
        std::uint32_t const count = b.u16(coverage + 2);
        std::uint32_t lo = 0;
        std::uint32_t hi = count;
        while (lo < hi) {
            std::uint32_t const mid = (lo + hi) / 2;
            std::uint32_t const record = coverage + 4 + mid * 6;
            std::uint16_t const first = b.u16(record);
            std::uint16_t const last = b.u16(record + 2);
            if (glyph < first)
                hi = mid;
            else if (glyph > last)
                lo = mid + 1;
            else
                return b.has(record, 6) ? static_cast<int>(b.u16(record + 4) + (glyph - first)) : -1;
        }
    }
    return -1;
}

std::uint16_t glyph_class(Bytes const& b, std::uint32_t class_def, std::uint32_t glyph)
{
    if (class_def == 0)
        return 0;
    std::uint16_t const format = b.u16(class_def);
    if (format == 1) {
        std::uint32_t const start = b.u16(class_def + 2);
        std::uint32_t const count = b.u16(class_def + 4);
        if (glyph < start || glyph - start >= count)
            return 0;
        return b.u16(class_def + 6 + (glyph - start) * 2);
    }
    if (format == 2) {
        std::uint32_t const count = b.u16(class_def + 2);
        std::uint32_t lo = 0;
        std::uint32_t hi = count;
        while (lo < hi) {
            std::uint32_t const mid = (lo + hi) / 2;
            std::uint32_t const record = class_def + 4 + mid * 6;
            std::uint16_t const first = b.u16(record);
            std::uint16_t const last = b.u16(record + 2);
            if (glyph < first)
                hi = mid;
            else if (glyph > last)
                lo = mid + 1;
            else
                return b.u16(record + 4);
        }
    }
    return 0;
}

void GlyphDigest::add(std::uint32_t glyph)
{
    for (int k = 0; k < 3; ++k)
        masks[static_cast<std::size_t>(k)] |= 1ull << ((glyph >> shifts[k]) & 63);
}

void GlyphDigest::add_range(std::uint32_t first, std::uint32_t last)
{
    for (int k = 0; k < 3; ++k) {
        std::uint32_t const a = first >> shifts[k];
        std::uint32_t const z = last >> shifts[k];
        if (z - a >= 63) {
            masks[static_cast<std::size_t>(k)] = ~0ull;
            continue;
        }
        for (std::uint32_t v = a; v <= z; ++v)
            masks[static_cast<std::size_t>(k)] |= 1ull << (v & 63);
    }
}

namespace {

void add_coverage(GlyphDigest& digest, Bytes const& b, std::uint32_t coverage)
{
    std::uint16_t const format = coverage ? b.u16(coverage) : 0;
    if (format == 1) {
        std::uint32_t const count = b.u16(coverage + 2);
        for (std::uint32_t k = 0; k < count && b.has(coverage + 4 + k * 2, 2); ++k)
            digest.add(b.u16(coverage + 4 + k * 2));
        return;
    }
    if (format == 2) {
        std::uint32_t const count = b.u16(coverage + 2);
        for (std::uint32_t k = 0; k < count && b.has(coverage + 4 + k * 6, 6); ++k) {
            std::uint16_t const first = b.u16(coverage + 4 + k * 6);
            std::uint16_t const last = b.u16(coverage + 4 + k * 6 + 2);
            if (first <= last)
                digest.add_range(first, last);
        }
        return;
    }
    // A coverage the reader cannot read applies to nothing; one it does not
    // know (a newer format) is not ruled out.
    if (format != 0)
        digest.add_all();
}

// The coverage that decides whether a subtable can apply at a glyph: the
// one its first input glyph is checked against.
std::uint32_t first_coverage(Bytes const& b, bool gpos, std::uint16_t type, std::uint32_t at)
{
    std::uint16_t const format = b.u16(at);
    bool const context = gpos ? type == 7 : type == 5;
    bool const chain = gpos ? type == 8 : type == 6;
    if (context && format == 3)
        return b.u16(at + 2) ? b.offset16(at, at + 6) : 0;
    if (chain && format == 3) {
        std::uint32_t const backtrack = b.u16(at + 2);
        std::uint32_t const input_at = at + 4 + backtrack * 2;
        return b.u16(input_at) ? b.offset16(at, input_at + 2) : 0;
    }
    return b.offset16(at, at + 2);
}

}

// --- The common tables ---------------------------------------------------------

std::uint32_t LayoutTable::find_script(std::uint32_t tag) const
{
    if (script_list == 0)
        return 0;
    std::uint32_t const count = bytes.u16(script_list);
    for (std::uint32_t k = 0; k < count; ++k) {
        std::uint32_t const record = script_list + 2 + k * 6;
        if (!bytes.has(record, 6))
            break;
        if (bytes.u32(record) == tag)
            return bytes.offset16(script_list, record + 4);
    }
    return 0;
}

std::uint32_t LayoutTable::find_lang_sys(std::uint32_t script, std::uint32_t tag) const
{
    if (script == 0)
        return 0;
    std::uint32_t const count = bytes.u16(script + 2);
    for (std::uint32_t k = 0; k < count; ++k) {
        std::uint32_t const record = script + 4 + k * 6;
        if (!bytes.has(record, 6))
            break;
        if (bytes.u32(record) == tag)
            return bytes.offset16(script, record + 4);
    }
    return 0;
}

int LayoutTable::find_feature(std::uint32_t lang_sys, std::uint32_t tag) const
{
    if (lang_sys == 0)
        return -1;
    std::uint32_t const count = bytes.u16(lang_sys + 4);
    for (std::uint32_t k = 0; k < count; ++k) {
        std::uint32_t const at = lang_sys + 6 + k * 2;
        if (!bytes.has(at, 2))
            break;
        std::uint16_t const index = bytes.u16(at);
        if (feature_tag(index) == tag)
            return index;
    }
    return -1;
}

std::uint32_t LayoutTable::feature_tag(std::uint16_t feature_index) const
{
    if (feature_list == 0 || feature_index >= bytes.u16(feature_list))
        return 0;
    return bytes.u32(feature_list + 2 + static_cast<std::uint32_t>(feature_index) * 6);
}

int LayoutTable::feature_variation_at_default() const
{
    if (feature_variations == 0)
        return -1;
    std::uint32_t const count = bytes.u32(feature_variations + 4);
    for (std::uint32_t k = 0; k < count && k < 0xFFFF; ++k) {
        std::uint32_t const record = feature_variations + 8 + k * 8;
        if (!bytes.has(record, 8))
            break;
        std::uint32_t const set = bytes.offset32(feature_variations, record);
        // A null condition set matches every instance.
        bool matches = true;
        if (set) {
            std::uint32_t const conditions = bytes.u16(set);
            for (std::uint32_t c = 0; c < conditions && matches; ++c) {
                std::uint32_t const condition = bytes.offset32(set, set + 2 + c * 4);
                if (condition == 0 || bytes.u16(condition) != 1) {
                    matches = false; // a condition format this reader does not know is false
                    break;
                }
                // Format 1: an axis range, in normalized F2DOT14; the
                // default instance has every axis at 0.
                std::int16_t const min = bytes.i16(condition + 4);
                std::int16_t const max = bytes.i16(condition + 6);
                matches = min <= 0 && 0 <= max;
            }
        }
        if (matches)
            return static_cast<int>(k);
    }
    return -1;
}

std::vector<std::uint16_t> LayoutTable::feature_lookups(std::uint16_t feature_index, int variation) const
{
    std::vector<std::uint16_t> out;
    if (feature_list == 0 || feature_index >= bytes.u16(feature_list))
        return out;
    std::uint32_t feature = bytes.offset16(feature_list, feature_list + 2 + static_cast<std::uint32_t>(feature_index) * 6 + 4);
    if (variation >= 0) {
        std::uint32_t const record = feature_variations + 8 + static_cast<std::uint32_t>(variation) * 8;
        std::uint32_t const substitution = bytes.offset32(feature_variations, record + 4);
        if (substitution) {
            std::uint32_t const count = bytes.u16(substitution + 4);
            for (std::uint32_t k = 0; k < count; ++k) {
                std::uint32_t const at = substitution + 6 + k * 6;
                if (!bytes.has(at, 6))
                    break;
                if (bytes.u16(at) == feature_index) {
                    feature = bytes.offset32(substitution, at + 2);
                    break;
                }
            }
        }
    }
    if (feature == 0)
        return out;
    std::uint32_t const count = bytes.u16(feature + 2);
    for (std::uint32_t k = 0; k < count && bytes.has(feature + 4 + k * 2, 2); ++k)
        out.push_back(bytes.u16(feature + 4 + k * 2));
    return out;
}

std::uint16_t GlyphDefinitions::props_of(std::uint32_t glyph) const
{
    switch (glyph_class(bytes, glyph_class_def, glyph)) {
    case 1:
        return glyph_props::base_glyph;
    case 2:
        return glyph_props::ligature;
    case 3:
        return static_cast<std::uint16_t>(glyph_props::mark
            | (glyph_class(bytes, mark_attach_class_def, glyph) & 0xFF) << 8);
    default:
        return 0;
    }
}

bool GlyphDefinitions::mark_set_covers(std::uint32_t set, std::uint32_t glyph) const
{
    if (mark_glyph_sets == 0 || set >= bytes.u16(mark_glyph_sets + 2))
        return false;
    std::uint32_t const coverage = bytes.offset32(mark_glyph_sets, mark_glyph_sets + 4 + set * 4);
    return coverage_index(bytes, coverage, glyph) >= 0;
}

namespace {

Bytes bytes_of(std::span<std::uint8_t const> table)
{
    if (table.size() > 0xFFFFFFF0u)
        return {};
    return Bytes { table.data(), static_cast<std::uint32_t>(table.size()) };
}

void parse_layout_table(LayoutTable& t, bool gpos)
{
    t.gpos = gpos;
    Bytes const& b = t.bytes;
    if (b.size < 10 || b.u16(0) != 1) {
        t.bytes = {};
        return;
    }
    t.script_list = b.offset16(0, 4);
    t.feature_list = b.offset16(0, 6);
    t.lookup_list = b.offset16(0, 8);
    if (b.u16(2) >= 1 && b.size >= 14)
        t.feature_variations = b.offset32(0, 10);
    if (t.lookup_list == 0)
        return;
    std::uint32_t const count = b.u16(t.lookup_list);
    t.lookups.resize(count);
    std::uint16_t const extension_type = gpos ? 9 : 7;
    for (std::uint32_t k = 0; k < count; ++k) {
        Lookup& lookup = t.lookups[k];
        std::uint32_t const at = b.offset16(t.lookup_list, t.lookup_list + 2 + k * 2);
        if (at == 0)
            continue;
        lookup.type = b.u16(at);
        std::uint16_t const flag = b.u16(at + 2);
        std::uint32_t const subtables = b.u16(at + 4);
        lookup.props = flag;
        if (flag & lookup_flag::use_mark_filtering_set)
            lookup.props |= static_cast<std::uint32_t>(b.u16(at + 6 + subtables * 2)) << 16;
        for (std::uint32_t s = 0; s < subtables && b.has(at + 6 + s * 2, 2); ++s) {
            std::uint32_t sub = b.offset16(at, at + 6 + s * 2);
            if (sub == 0)
                continue;
            std::uint16_t type = lookup.type;
            if (type == extension_type) {
                // §Extension: the real type and a 32-bit offset from here.
                if (b.u16(sub) != 1)
                    continue;
                type = b.u16(sub + 2);
                sub = b.offset32(sub, sub + 4);
                if (sub == 0 || type == extension_type)
                    continue;
            }
            lookup.subtables.push_back(Lookup::Subtable { type, sub });
            add_coverage(lookup.digest, b, first_coverage(b, gpos, type, sub));
        }
        if (!lookup.subtables.empty()) {
            lookup.type = lookup.subtables.front().type;
            lookup.reverse = !gpos && lookup.type == 8;
        }
    }
}

}

LayoutTables LayoutTables::parse(TrueTypeFont const& font)
{
    LayoutTables t;
    t.glyph_count = font.glyph_count();
    t.gdef.bytes = bytes_of(font.table(make_tag('G', 'D', 'E', 'F')));
    {
        Bytes const& b = t.gdef.bytes;
        if (b.size >= 12 && b.u16(0) == 1) {
            t.gdef.glyph_class_def = b.offset16(0, 4);
            t.gdef.mark_attach_class_def = b.offset16(0, 10);
            if (b.u16(2) >= 2 && b.size >= 14)
                t.gdef.mark_glyph_sets = b.offset16(0, 12);
        } else {
            t.gdef.bytes = {};
        }
    }
    t.gsub.bytes = bytes_of(font.table(make_tag('G', 'S', 'U', 'B')));
    parse_layout_table(t.gsub, false);
    t.gpos.bytes = bytes_of(font.table(make_tag('G', 'P', 'O', 'S')));
    parse_layout_table(t.gpos, true);
    t.kern = bytes_of(font.table(make_tag('k', 'e', 'r', 'n')));
    t.cmap = bytes_of(font.table(make_tag('c', 'm', 'a', 'p')));
    {
        Bytes const& b = t.cmap;
        std::uint32_t const records = b.u16(2);
        for (std::uint32_t k = 0; k < records; ++k) {
            std::uint32_t const record = 4 + k * 8;
            if (!b.has(record, 8))
                break;
            if (b.u16(record) == 0 && b.u16(record + 2) == 5) {
                std::uint32_t const at = b.u32(record + 4);
                if (at < b.size && b.u16(at) == 14)
                    t.cmap14 = at;
                break;
            }
        }
    }
    return t;
}

std::int64_t LayoutTables::variation_glyph(char32_t base, char32_t selector) const
{
    if (cmap14 == 0)
        return 0;
    Bytes const& b = cmap;
    std::uint32_t const count = b.u32(cmap14 + 6);
    // VariationSelector records: selector (24 bits), default UVS offset,
    // non-default UVS offset; sorted by selector.
    std::uint32_t lo = 0;
    std::uint32_t hi = std::min<std::uint32_t>(count, b.size / 11);
    auto u24 = [&](std::uint32_t at) { return static_cast<char32_t>(b.u8(at)) << 16 | static_cast<char32_t>(b.u16(at + 1)); };
    while (lo < hi) {
        std::uint32_t const mid = (lo + hi) / 2;
        std::uint32_t const record = cmap14 + 10 + mid * 11;
        char32_t const s = u24(record);
        if (selector < s) {
            hi = mid;
            continue;
        }
        if (selector > s) {
            lo = mid + 1;
            continue;
        }
        // Non-default: (unicode 24 bits, glyph 16 bits) mappings.
        if (std::uint32_t const nondefault = b.u32(record + 7)) {
            std::uint32_t const at = cmap14 + nondefault;
            std::uint32_t const n = b.u32(at);
            std::uint32_t l = 0;
            std::uint32_t h = std::min<std::uint32_t>(n, b.size / 5);
            while (l < h) {
                std::uint32_t const m = (l + h) / 2;
                char32_t const u = u24(at + 4 + m * 5);
                if (base < u)
                    h = m;
                else if (base > u)
                    l = m + 1;
                else
                    return b.u16(at + 4 + m * 5 + 3);
            }
        }
        // Default: ranges (start 24 bits, additional count 8 bits).
        if (std::uint32_t const fallback = b.u32(record + 3)) {
            std::uint32_t const at = cmap14 + fallback;
            std::uint32_t const n = b.u32(at);
            std::uint32_t l = 0;
            std::uint32_t h = std::min<std::uint32_t>(n, b.size / 4);
            while (l < h) {
                std::uint32_t const m = (l + h) / 2;
                char32_t const start = u24(at + 4 + m * 4);
                char32_t const end = start + b.u8(at + 4 + m * 4 + 3);
                if (base < start)
                    h = m;
                else if (base > end)
                    l = m + 1;
                else
                    return -1;
            }
        }
        return 0;
    }
    return 0;
}

// --- The buffer ------------------------------------------------------------------

void Buffer::begin(std::size_t length)
{
    idx = 0;
    have_output = false;
    failed = false;
    out.clear();
    pos.clear();
    serial = 0;
    max_len = std::max<std::size_t>(length * 64, 16384);
    max_ops = static_cast<std::int64_t>(std::max<std::size_t>(length * 4096, 65536));
}

void Buffer::sync()
{
    if (have_output) {
        while (idx < info.size())
            out.push_back(info[idx++]);
        info.swap(out);
        out.clear();
    }
    have_output = false;
    idx = 0;
    pos.resize(info.size());
}

void Buffer::delete_glyph()
{
    std::uint32_t const cluster = info[idx].cluster;
    bool const survives = (idx + 1 < info.size() && info[idx + 1].cluster == cluster)
        || (!out.empty() && out.back().cluster == cluster);
    if (!survives) {
        if (!out.empty()) {
            // The cluster goes to the glyph before it.
            if (cluster < out.back().cluster) {
                std::uint32_t const old = out.back().cluster;
                for (std::size_t i = out.size(); i > 0 && out[i - 1].cluster == old; --i)
                    out[i - 1].cluster = cluster;
            }
        } else if (idx + 1 < info.size()) {
            merge_clusters(idx, idx + 2);
        }
    }
    ++idx;
}

bool Buffer::move_to(std::size_t i)
{
    if (!have_output) {
        idx = std::min(i, info.size());
        return true;
    }
    if (i > out.size()) {
        std::size_t const wanted = i - out.size();
        std::size_t const count = std::min(wanted, info.size() - idx);
        out.insert(out.end(), info.begin() + static_cast<std::ptrdiff_t>(idx),
            info.begin() + static_cast<std::ptrdiff_t>(idx + count));
        idx += count;
        return count == wanted;
    }
    if (i < out.size()) {
        std::size_t const count = out.size() - i;
        if (idx < count) {
            info.insert(info.begin(), count - idx, GlyphInfo {});
            idx = count;
        }
        idx -= count;
        std::copy(out.begin() + static_cast<std::ptrdiff_t>(i), out.end(), info.begin() + static_cast<std::ptrdiff_t>(idx));
        out.resize(i);
    }
    return true;
}

void Buffer::merge_clusters(std::size_t start, std::size_t end)
{
    if (end > info.size())
        end = info.size();
    if (end <= start + 1)
        return;
    std::uint32_t cluster = info[start].cluster;
    for (std::size_t i = start + 1; i < end; ++i)
        cluster = std::min(cluster, info[i].cluster);
    // The neighbors that shared a cluster with the ends join too.
    if (cluster != info[end - 1].cluster) {
        while (end < info.size() && info[end - 1].cluster == info[end].cluster)
            ++end;
    }
    if (cluster != info[start].cluster) {
        while (start > idx && info[start - 1].cluster == info[start].cluster)
            --start;
    }
    // At the start of the input, the same cluster continues in the output.
    if (have_output && start == idx && info[start].cluster != cluster) {
        std::uint32_t const old = info[start].cluster;
        for (std::size_t i = out.size(); i > 0 && out[i - 1].cluster == old; --i)
            out[i - 1].cluster = cluster;
    }
    for (std::size_t i = start; i < end; ++i)
        info[i].cluster = cluster;
}

void Buffer::merge_out_clusters(std::size_t start, std::size_t end)
{
    if (end > out.size())
        end = out.size();
    if (end <= start + 1)
        return;
    std::uint32_t cluster = out[start].cluster;
    for (std::size_t i = start + 1; i < end; ++i)
        cluster = std::min(cluster, out[i].cluster);
    while (start > 0 && out[start - 1].cluster == out[start].cluster)
        --start;
    while (end < out.size() && out[end - 1].cluster == out[end].cluster)
        ++end;
    // At the end of the output, the same cluster continues in the input.
    if (end == out.size()) {
        std::uint32_t const old = out[end - 1].cluster;
        for (std::size_t i = idx; i < info.size() && info[i].cluster == old; ++i)
            info[i].cluster = cluster;
    }
    for (std::size_t i = start; i < end; ++i)
        out[i].cluster = cluster;
}

void Buffer::reverse_range(std::size_t start, std::size_t end)
{
    if (end <= start)
        return;
    std::reverse(info.begin() + static_cast<std::ptrdiff_t>(start), info.begin() + static_cast<std::ptrdiff_t>(end));
    if (pos.size() >= end)
        std::reverse(pos.begin() + static_cast<std::ptrdiff_t>(start), pos.begin() + static_cast<std::ptrdiff_t>(end));
}

void Buffer::reverse()
{
    reverse_range(0, info.size());
}

// --- Applying lookups -----------------------------------------------------------

bool ApplyContext::glyph_matches_props(GlyphInfo const& g, std::uint32_t props) const
{
    std::uint16_t const glyph = g.props;
    // A class the lookup ignores.
    if (glyph & props & lookup_flag::ignore_flags)
        return false;
    if (glyph & glyph_props::mark) {
        if (props & lookup_flag::use_mark_filtering_set)
            return tables.gdef.mark_set_covers(props >> 16, g.glyph);
        if (props & lookup_flag::mark_attachment_type)
            return (props & lookup_flag::mark_attachment_type) == (glyph & lookup_flag::mark_attachment_type);
    }
    return true;
}

void ApplyContext::set_substituted_props(GlyphInfo& g, std::uint32_t glyph, std::uint16_t class_guess, bool ligature,
    bool component) const
{
    std::uint16_t props = static_cast<std::uint16_t>(g.props | glyph_props::substituted);
    if (ligature) {
        // A ligature formed from multiplied glyphs is a ligature: the last
        // of the two transformations is the one that counts.
        props = static_cast<std::uint16_t>((props | glyph_props::ligated) & ~glyph_props::multiplied);
    }
    if (component)
        props |= glyph_props::multiplied;
    if (tables.gdef.has_glyph_classes())
        props = static_cast<std::uint16_t>((props & glyph_props::preserve) | tables.gdef.props_of(glyph));
    else if (class_guess)
        props = static_cast<std::uint16_t>((props & glyph_props::preserve) | class_guess);
    g.props = props;
    g.glyph = glyph;
}

void ApplyContext::replace_glyph(std::uint32_t glyph)
{
    set_substituted_props(buffer.cur(), glyph, 0, false, false);
    buffer.replace_glyph(glyph);
}

void ApplyContext::replace_glyph_in_place(std::uint32_t glyph)
{
    set_substituted_props(buffer.cur(), glyph, 0, false, false);
}

void ApplyContext::output_glyph_for_component(std::uint32_t glyph, std::uint16_t class_guess)
{
    GlyphInfo& g = buffer.output_glyph(glyph);
    set_substituted_props(g, glyph, class_guess, false, true);
}

void ApplyContext::replace_glyph_with_ligature(std::uint32_t glyph, std::uint16_t class_guess)
{
    set_substituted_props(buffer.cur(), glyph, class_guess, true, false);
    buffer.replace_glyph(glyph);
}

std::uint32_t ApplyContext::random_number()
{
    // The minimal standard generator (Park and Miller), seeded per run.
    random_state = static_cast<std::uint32_t>(static_cast<std::uint64_t>(random_state) * 48271u % 2147483647u);
    return random_state;
}

namespace {

// Which glyphs a match walks over and which stop it (§Lookup flags; the
// joiners and ignorables as shapers treat them): Skip steps over the
// glyph, Match takes it, Mismatch ends the match.
enum class Verdict {
    Match,
    Skip,
    Mismatch,
};

struct Walker {
    ApplyContext& c;
    std::uint32_t props;
    std::uint32_t mask;
    bool ignore_zwnj;
    bool ignore_zwj;
    bool ignore_hidden;
    std::uint8_t syllable = 0;

    // Context (backtrack and lookahead) matching sees every glyph whatever
    // its features, and steps over the joiners when the feature allows.
    Walker(ApplyContext& context, bool context_match)
        : c(context)
        , props(context.lookup_props)
        , mask(context_match ? ~0u : context.lookup_mask)
        , ignore_zwnj(context.is_gpos() || (context_match && context.auto_zwnj))
        , ignore_zwj(context_match || context.auto_zwj)
        , ignore_hidden(context.is_gpos())
    {
    }

    // 0: the glyph is never seen; 1: it may be stepped over; 2: it is seen.
    int may_skip(GlyphInfo const& g) const
    {
        if (!c.glyph_matches_props(g, props))
            return 0;
        if (is_default_ignorable(g) && (ignore_zwnj || !(g.unicode & unicode_flag::zwnj))
            && (ignore_zwj || !(g.unicode & unicode_flag::zwj))
            && (ignore_hidden || !(g.unicode & unicode_flag::hidden)))
            return 1;
        return 2;
    }

    // `matches` is null when anything with the mask matches.
    template<typename Match>
    Verdict classify(GlyphInfo const& g, Match const& matches) const
    {
        int const skip = may_skip(g);
        if (skip == 0)
            return Verdict::Skip;
        int match; // 0 no, 1 maybe, 2 yes
        if (!(g.mask & mask) || (syllable && g.syllable != syllable))
            match = 0;
        else
            match = matches(g) ? 2 : 0;
        if (match == 2)
            return Verdict::Match;
        if (skip == 2)
            return Verdict::Mismatch;
        return Verdict::Skip;
    }

    Verdict classify_any(GlyphInfo const& g) const
    {
        int const skip = may_skip(g);
        if (skip == 0)
            return Verdict::Skip;
        bool const maybe = (g.mask & mask) && !(syllable && g.syllable != syllable);
        if (maybe && skip == 2)
            return Verdict::Match;
        if (skip == 2)
            return Verdict::Mismatch;
        return Verdict::Skip;
    }

    // The next glyph of the input after `i` that matches, or false.
    template<typename Match>
    bool next(std::size_t& i, Match const& matches) const
    {
        Buffer const& b = c.buffer;
        while (i + 1 < b.len()) {
            ++i;
            Verdict const v = classify(b.info[i], matches);
            if (v == Verdict::Match)
                return true;
            if (v == Verdict::Mismatch)
                return false;
        }
        return false;
    }
    bool next_any(std::size_t& i) const
    {
        Buffer const& b = c.buffer;
        while (i + 1 < b.len()) {
            ++i;
            Verdict const v = classify_any(b.info[i]);
            if (v == Verdict::Match)
                return true;
            if (v == Verdict::Mismatch)
                return false;
        }
        return false;
    }
    // The previous glyph of the backtrack (the output, during GSUB) before
    // `i`, or false.
    template<typename Match>
    bool prev(std::size_t& i, Match const& matches) const
    {
        Buffer& b = c.buffer;
        while (i > 0) {
            --i;
            Verdict const v = classify(b.backtrack(i), matches);
            if (v == Verdict::Match)
                return true;
            if (v == Verdict::Mismatch)
                return false;
        }
        return false;
    }
    bool prev_any(std::size_t& i) const
    {
        Buffer& b = c.buffer;
        while (i > 0) {
            --i;
            Verdict const v = classify_any(b.backtrack(i));
            if (v == Verdict::Match)
                return true;
            if (v == Verdict::Mismatch)
                return false;
        }
        return false;
    }
};

using Positions = std::array<std::size_t, max_context_length>;

// Matches the rest of an input sequence after the current glyph (§GSUB
// and §GPOS contextual lookups), with the rule that a ligature cannot be
// formed across marks that sit on different components of an earlier
// ligature. `matches(g, k)` tests the k-th input item, k from 1.
template<typename Match>
bool match_input(ApplyContext& c, unsigned count, Match const& matches, std::size_t& end, Positions& positions,
    unsigned* total_components)
{
    if (count > max_context_length)
        return false;
    Buffer& b = c.buffer;
    Walker walker(c, false);
    walker.syllable = c.per_syllable ? b.cur().syllable : 0;
    std::size_t i = b.idx;
    unsigned total = 0;
    unsigned const first_id = lig_id(b.cur());
    unsigned const first_comp = lig_comp(b.cur());
    enum { NotChecked, MayNotSkip, MaySkip } ligature_base = NotChecked;
    for (unsigned k = 1; k < count; ++k) {
        if (!walker.next(i, [&](GlyphInfo const& g) { return matches(g, k); }))
            return false;
        positions[k] = i;
        unsigned const this_id = lig_id(b.info[i]);
        unsigned const this_comp = lig_comp(b.info[i]);
        if (first_id && first_comp) {
            // The first glyph sat on a ligature component: every other
            // must sit on the same one — unless the ligature itself is a
            // glyph this lookup does not see.
            if (first_id != this_id || first_comp != this_comp) {
                if (ligature_base == NotChecked) {
                    bool found = false;
                    std::size_t j = b.backtrack_len();
                    while (j > 0 && lig_id(b.backtrack(j - 1)) == first_id) {
                        if (lig_comp(b.backtrack(j - 1)) == 0) {
                            --j;
                            found = true;
                            break;
                        }
                        --j;
                    }
                    ligature_base = (found && walker.may_skip(b.backtrack(j)) == 0) ? MaySkip : MayNotSkip;
                }
                if (ligature_base == MayNotSkip)
                    return false;
            }
        } else if (this_id && this_comp && this_id != first_id) {
            // Nor may a later one sit on a component of another ligature.
            return false;
        }
        total += lig_num_comps(b.info[i]);
    }
    end = i + 1;
    if (total_components)
        *total_components = total + lig_num_comps(b.cur());
    positions[0] = b.idx;
    return true;
}

template<typename Match>
bool match_backtrack(ApplyContext& c, unsigned count, Match const& matches, std::size_t& start)
{
    Walker walker(c, true);
    std::size_t i = c.buffer.backtrack_len();
    for (unsigned k = 0; k < count; ++k) {
        if (!walker.prev(i, [&](GlyphInfo const& g) { return matches(g, k); }))
            return false;
    }
    start = i;
    return true;
}

template<typename Match>
bool match_lookahead(ApplyContext& c, unsigned count, Match const& matches, std::size_t from, std::size_t& end)
{
    Walker walker(c, true);
    std::size_t i = from - 1;
    for (unsigned k = 0; k < count; ++k) {
        if (!walker.next(i, [&](GlyphInfo const& g) { return matches(g, k); }))
            return false;
    }
    end = i + 1;
    return true;
}

// Replaces the matched input with a ligature glyph and does the ligature
// bookkeeping: a ligature made of marks alone stays a mark and gets no id;
// otherwise the ligature gets a fresh id and its component count, and the
// marks between its components (and right after it) are numbered by the
// component they sat on, so that mark-to-ligature attaches them there.
void ligate(ApplyContext& c, unsigned count, Positions const& positions, std::size_t match_end, std::uint32_t lig_glyph,
    unsigned total_components)
{
    Buffer& b = c.buffer;
    bool is_base_ligature = is_base_glyph(b.info[positions[0]]);
    bool is_mark_ligature = is_mark(b.info[positions[0]]);
    for (unsigned k = 1; k < count; ++k) {
        if (!is_mark(b.info[positions[k]])) {
            is_base_ligature = false;
            is_mark_ligature = false;
            break;
        }
    }
    bool const is_lig = !is_base_ligature && !is_mark_ligature;
    std::uint16_t const klass = is_lig ? glyph_props::ligature : 0;
    unsigned const id = is_lig ? b.next_lig_id() : 0;
    unsigned last_id = lig_id(b.cur());
    unsigned last_components = lig_num_comps(b.cur());
    unsigned components_so_far = last_components;
    b.merge_clusters(b.idx, match_end);
    if (is_lig) {
        set_lig_props_for_ligature(b.cur(), id, total_components);
        if (b.cur().category == 12 /* Mn */)
            b.cur().category = 7; // a ligature of a base and marks is a letter (Lo)
    }
    c.replace_glyph_with_ligature(lig_glyph, klass);
    for (unsigned k = 1; k < count; ++k) {
        while (b.idx < positions[k] && !b.failed) {
            if (is_lig) {
                unsigned this_comp = lig_comp(b.cur());
                if (this_comp == 0)
                    this_comp = last_components;
                unsigned const new_comp = components_so_far - last_components + std::min(this_comp, last_components);
                set_lig_props_for_mark(b.cur(), id, new_comp);
            }
            b.next_glyph();
        }
        last_id = lig_id(b.cur());
        last_components = lig_num_comps(b.cur());
        components_so_far += last_components;
        b.skip_glyph(); // the component itself is gone
    }
    if (!is_mark_ligature && last_id) {
        // Marks after the last component that sat on an earlier ligature
        // now sit on this one.
        for (std::size_t i = b.idx; i < b.len(); ++i) {
            if (lig_id(b.info[i]) != last_id)
                break;
            unsigned const this_comp = lig_comp(b.info[i]);
            if (this_comp == 0)
                break;
            unsigned const new_comp = components_so_far - last_components + std::min(this_comp, last_components);
            set_lig_props_for_mark(b.info[i], id, new_comp);
        }
    }
}

// Runs a contextual rule's nested lookups (§SequenceLookupRecord) at the
// matched positions, keeping the positions true as nested lookups grow or
// shrink the sequence, and leaves the buffer after the match.
void apply_nested(ApplyContext& c, unsigned count, Positions& positions, Bytes const& b, std::uint32_t records,
    unsigned record_count, std::size_t match_end)
{
    Buffer& buffer = c.buffer;
    // Positions as distances from the start of the output.
    std::ptrdiff_t const shift = static_cast<std::ptrdiff_t>(buffer.backtrack_len()) - static_cast<std::ptrdiff_t>(buffer.idx);
    std::ptrdiff_t end = static_cast<std::ptrdiff_t>(match_end) + shift;
    for (unsigned k = 0; k < count; ++k)
        positions[k] = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(positions[k]) + shift);
    for (unsigned r = 0; r < record_count && !buffer.failed; ++r) {
        std::uint32_t const record = records + r * 4;
        if (!b.has(record, 4))
            break;
        unsigned const index = b.u16(record);
        std::uint16_t const lookup = b.u16(record + 2);
        if (index >= count)
            continue;
        std::size_t const original = buffer.backtrack_len() + buffer.lookahead_len();
        // Earlier nested lookups may have deleted this position.
        if (positions[index] >= original)
            continue;
        if (!buffer.move_to(positions[index]))
            break;
        if (!c.recurse(lookup))
            continue;
        std::size_t const now = buffer.backtrack_len() + buffer.lookahead_len();
        std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(now) - static_cast<std::ptrdiff_t>(original);
        if (delta == 0)
            continue;
        end += delta;
        if (end < static_cast<std::ptrdiff_t>(positions[index])) {
            // A nested lookup deleted more than the rest of the match: the
            // end never moves before where that lookup applied.
            delta += static_cast<std::ptrdiff_t>(positions[index]) - end;
            end = static_cast<std::ptrdiff_t>(positions[index]);
        }
        unsigned next = index + 1;
        if (delta > 0) {
            if (static_cast<unsigned>(delta) + count > max_context_length)
                break;
        } else {
            delta = std::max<std::ptrdiff_t>(delta, static_cast<std::ptrdiff_t>(next) - static_cast<std::ptrdiff_t>(count));
            next = static_cast<unsigned>(static_cast<std::ptrdiff_t>(next) - delta);
        }
        // Shift the later positions along, and fill the new ones in.
        Positions const copy = positions;
        for (unsigned m = next; m < count; ++m)
            positions[static_cast<std::size_t>(static_cast<std::ptrdiff_t>(m) + delta)] = copy[m];
        next = static_cast<unsigned>(static_cast<std::ptrdiff_t>(next) + delta);
        count = static_cast<unsigned>(static_cast<std::ptrdiff_t>(count) + delta);
        for (unsigned j = index + 1; j < next; ++j)
            positions[j] = positions[j - 1] + 1;
        for (; next < count; ++next)
            positions[next] = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(positions[next]) + delta);
    }
    buffer.move_to(static_cast<std::size_t>(std::max<std::ptrdiff_t>(end, 0)));
}

// --- GSUB ---------------------------------------------------------------------

bool apply_single_subst(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    std::uint16_t const format = b.u16(at);
    std::uint32_t const glyph = c.buffer.cur().glyph;
    int const index = coverage_index(b, b.offset16(at, at + 2), glyph);
    if (index < 0)
        return false;
    if (format == 1) {
        c.replace_glyph((glyph + static_cast<std::uint32_t>(b.i16(at + 4))) & 0xFFFFu);
        return true;
    }
    if (format == 2) {
        if (static_cast<unsigned>(index) >= b.u16(at + 4))
            return false;
        c.replace_glyph(b.u16(at + 6 + static_cast<std::uint32_t>(index) * 2));
        return true;
    }
    return false;
}

bool apply_multiple_subst(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    if (b.u16(at) != 1)
        return false;
    Buffer& buffer = c.buffer;
    int const index = coverage_index(b, b.offset16(at, at + 2), buffer.cur().glyph);
    if (index < 0 || static_cast<unsigned>(index) >= b.u16(at + 4))
        return false;
    std::uint32_t const sequence = b.offset16(at, at + 6 + static_cast<std::uint32_t>(index) * 2);
    if (sequence == 0)
        return false;
    std::uint32_t const count = b.u16(sequence);
    if (!b.has(sequence + 2, count * 2))
        return false;
    if (count == 1) {
        c.replace_glyph(b.u16(sequence + 2));
        return true;
    }
    if (count == 0) {
        // Deleting a glyph is allowed (§Multiple substitution).
        buffer.delete_glyph();
        return true;
    }
    if (!buffer.output_allowed()) {
        buffer.failed = true;
        return false;
    }
    std::uint16_t const klass = is_ligature(buffer.cur()) ? glyph_props::base_glyph : 0;
    unsigned const id = lig_id(buffer.cur());
    for (std::uint32_t k = 0; k < count; ++k) {
        // A glyph attached to a ligature keeps that attachment.
        if (!id)
            set_lig_props_for_mark(buffer.cur(), 0, k);
        c.output_glyph_for_component(b.u16(sequence + 2 + k * 2), klass);
    }
    buffer.skip_glyph();
    return true;
}

bool apply_alternate_subst(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    if (b.u16(at) != 1)
        return false;
    Buffer& buffer = c.buffer;
    int const index = coverage_index(b, b.offset16(at, at + 2), buffer.cur().glyph);
    if (index < 0 || static_cast<unsigned>(index) >= b.u16(at + 4))
        return false;
    std::uint32_t const set = b.offset16(at, at + 6 + static_cast<std::uint32_t>(index) * 2);
    if (set == 0)
        return false;
    std::uint32_t const count = b.u16(set);
    if (count == 0 || c.lookup_mask == 0)
        return false;
    // The feature's value picks the alternate, 1-based.
    unsigned const shift = static_cast<unsigned>(std::countr_zero(c.lookup_mask));
    std::uint32_t alternate = (buffer.cur().mask & c.lookup_mask) >> shift;
    if (c.random && alternate == (c.lookup_mask >> shift))
        alternate = c.random_number() % count + 1;
    if (alternate == 0 || alternate > count)
        return false;
    c.replace_glyph(b.u16(set + 2 + (alternate - 1) * 2));
    return true;
}

bool apply_ligature_subst(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    if (b.u16(at) != 1)
        return false;
    Buffer& buffer = c.buffer;
    int const index = coverage_index(b, b.offset16(at, at + 2), buffer.cur().glyph);
    if (index < 0 || static_cast<unsigned>(index) >= b.u16(at + 4))
        return false;
    std::uint32_t const set = b.offset16(at, at + 6 + static_cast<std::uint32_t>(index) * 2);
    if (set == 0)
        return false;
    std::uint32_t const ligatures = b.u16(set);
    for (std::uint32_t l = 0; l < ligatures && b.has(set + 2 + l * 2, 2); ++l) {
        std::uint32_t const ligature = b.offset16(set, set + 2 + l * 2);
        if (ligature == 0)
            continue;
        std::uint16_t const lig_glyph = b.u16(ligature);
        unsigned const count = b.u16(ligature + 2);
        if (count == 0)
            continue;
        if (count == 1) {
            c.replace_glyph(lig_glyph);
            return true;
        }
        std::size_t end = 0;
        Positions positions {};
        unsigned total = 0;
        auto const matches = [&](GlyphInfo const& g, unsigned k) {
            return g.glyph == b.u16(ligature + 4 + (k - 1) * 2);
        };
        if (!match_input(c, count, matches, end, positions, &total))
            continue;
        ligate(c, count, positions, end, lig_glyph, total);
        return true;
    }
    return false;
}

bool apply_reverse_chain_subst(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    if (b.u16(at) != 1 || c.nesting_left != max_nesting)
        return false; // never as a nested lookup
    Buffer& buffer = c.buffer;
    int const index = coverage_index(b, b.offset16(at, at + 2), buffer.cur().glyph);
    if (index < 0)
        return false;
    std::uint32_t const backtrack_count = b.u16(at + 4);
    std::uint32_t const lookahead_at = at + 6 + backtrack_count * 2;
    std::uint32_t const lookahead_count = b.u16(lookahead_at);
    std::uint32_t const substitute_at = lookahead_at + 2 + lookahead_count * 2;
    if (static_cast<unsigned>(index) >= b.u16(substitute_at))
        return false;
    std::size_t start = 0;
    std::size_t end = 0;
    auto const backtrack = [&](GlyphInfo const& g, unsigned k) {
        return coverage_index(b, b.offset16(at, at + 6 + k * 2), g.glyph) >= 0;
    };
    auto const lookahead = [&](GlyphInfo const& g, unsigned k) {
        return coverage_index(b, b.offset16(at, lookahead_at + 2 + k * 2), g.glyph) >= 0;
    };
    if (!match_backtrack(c, backtrack_count, backtrack, start)
        || !match_lookahead(c, lookahead_count, lookahead, buffer.idx + 1, end))
        return false;
    c.replace_glyph_in_place(b.u16(substitute_at + 2 + static_cast<std::uint32_t>(index) * 2));
    return true;
}

// --- Contextual lookups, shared by GSUB and GPOS ----------------------------------

// One rule of a (chained) context subtable, whatever its format: input
// items from the second on, backtrack and lookahead items, each tested by
// glyph, by class or by coverage.
struct Rule {
    unsigned input_count = 0; // including the first glyph
    std::uint32_t input = 0; // where items 1.. are (glyph or class values), or coverage offsets 0..
    unsigned backtrack_count = 0;
    std::uint32_t backtrack = 0;
    unsigned lookahead_count = 0;
    std::uint32_t lookahead = 0;
    unsigned record_count = 0;
    std::uint32_t records = 0;
};

enum class MatchBy {
    Glyph,
    Class,
    Coverage,
};

struct RuleMatchers {
    MatchBy by = MatchBy::Glyph;
    std::uint32_t base = 0; // the subtable, for coverage offsets
    std::uint32_t input_classes = 0;
    std::uint32_t backtrack_classes = 0;
    std::uint32_t lookahead_classes = 0;
};

bool item_matches(Bytes const& b, RuleMatchers const& m, std::uint32_t class_def, GlyphInfo const& g, std::uint32_t at)
{
    std::uint16_t const value = b.u16(at);
    switch (m.by) {
    case MatchBy::Glyph:
        return g.glyph == value;
    case MatchBy::Class:
        return glyph_class(b, class_def, g.glyph) == value;
    case MatchBy::Coverage:
        return value != 0 && coverage_index(b, m.base + value, g.glyph) >= 0;
    }
    return false;
}

bool apply_rule(ApplyContext& c, Bytes const& b, RuleMatchers const& m, Rule const& rule)
{
    if (rule.input_count == 0 || rule.input_count > max_context_length)
        return false;
    Buffer& buffer = c.buffer;
    // Coverage rules list the first input glyph too; glyph and class
    // rules start at the second.
    std::uint32_t const input_from = m.by == MatchBy::Coverage ? rule.input + 2 : rule.input;
    std::size_t end = 0;
    Positions positions {};
    auto const input = [&](GlyphInfo const& g, unsigned k) {
        return item_matches(b, m, m.input_classes, g, input_from + (k - 1) * 2);
    };
    if (!match_input(c, rule.input_count, input, end, positions, nullptr))
        return false;
    std::size_t start = 0;
    auto const backtrack = [&](GlyphInfo const& g, unsigned k) {
        return item_matches(b, m, m.backtrack_classes, g, rule.backtrack + k * 2);
    };
    if (!match_backtrack(c, rule.backtrack_count, backtrack, start))
        return false;
    std::size_t lookahead_end = 0;
    auto const lookahead = [&](GlyphInfo const& g, unsigned k) {
        return item_matches(b, m, m.lookahead_classes, g, rule.lookahead + k * 2);
    };
    if (!match_lookahead(c, rule.lookahead_count, lookahead, end, lookahead_end))
        return false;
    apply_nested(c, rule.input_count, positions, b, rule.records, rule.record_count, end);
    (void)buffer;
    return true;
}

// §Sequence context (GSUB 5, GPOS 7).
bool apply_context(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    std::uint16_t const format = b.u16(at);
    std::uint32_t const glyph = c.buffer.cur().glyph;
    if (format == 1 || format == 2) {
        int const covered = coverage_index(b, b.offset16(at, at + 2), glyph);
        if (covered < 0)
            return false;
        RuleMatchers m;
        unsigned set_index = static_cast<unsigned>(covered);
        std::uint32_t sets = at + 6;
        std::uint32_t set_count = b.u16(at + 4);
        if (format == 2) {
            m.by = MatchBy::Class;
            m.input_classes = m.backtrack_classes = m.lookahead_classes = b.offset16(at, at + 4);
            set_index = glyph_class(b, m.input_classes, glyph);
            set_count = b.u16(at + 6);
            sets = at + 8;
        }
        if (set_index >= set_count)
            return false;
        std::uint32_t const set = b.offset16(at, sets + set_index * 2);
        if (set == 0)
            return false;
        std::uint32_t const rules = b.u16(set);
        for (std::uint32_t r = 0; r < rules && b.has(set + 2 + r * 2, 2); ++r) {
            std::uint32_t const rule_at = b.offset16(set, set + 2 + r * 2);
            if (rule_at == 0)
                continue;
            Rule rule;
            rule.input_count = b.u16(rule_at);
            rule.record_count = b.u16(rule_at + 2);
            rule.input = rule_at + 4;
            rule.records = rule.input + (rule.input_count ? rule.input_count - 1 : 0) * 2;
            if (apply_rule(c, b, m, rule))
                return true;
        }
        return false;
    }
    if (format == 3) {
        RuleMatchers m;
        m.by = MatchBy::Coverage;
        m.base = at;
        Rule rule;
        rule.input_count = b.u16(at + 2);
        rule.record_count = b.u16(at + 4);
        rule.input = at + 6;
        rule.records = rule.input + rule.input_count * 2;
        if (rule.input_count == 0 || coverage_index(b, b.offset16(at, rule.input), glyph) < 0)
            return false;
        return apply_rule(c, b, m, rule);
    }
    return false;
}

// §Chained sequence context (GSUB 6, GPOS 8).
bool apply_chain_context(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    std::uint16_t const format = b.u16(at);
    std::uint32_t const glyph = c.buffer.cur().glyph;
    auto read_rule = [&](std::uint32_t rule_at) {
        Rule rule;
        rule.backtrack_count = b.u16(rule_at);
        rule.backtrack = rule_at + 2;
        std::uint32_t const input_at = rule.backtrack + rule.backtrack_count * 2;
        rule.input_count = b.u16(input_at);
        rule.input = input_at + 2;
        std::uint32_t const lookahead_at = rule.input + (rule.input_count ? rule.input_count - 1 : 0) * 2;
        rule.lookahead_count = b.u16(lookahead_at);
        rule.lookahead = lookahead_at + 2;
        std::uint32_t const records_at = rule.lookahead + rule.lookahead_count * 2;
        rule.record_count = b.u16(records_at);
        rule.records = records_at + 2;
        return rule;
    };
    if (format == 1 || format == 2) {
        int const covered = coverage_index(b, b.offset16(at, at + 2), glyph);
        if (covered < 0)
            return false;
        RuleMatchers m;
        unsigned set_index = static_cast<unsigned>(covered);
        std::uint32_t set_count = b.u16(at + 4);
        std::uint32_t sets = at + 6;
        if (format == 2) {
            m.by = MatchBy::Class;
            m.backtrack_classes = b.offset16(at, at + 4);
            m.input_classes = b.offset16(at, at + 6);
            m.lookahead_classes = b.offset16(at, at + 8);
            set_index = glyph_class(b, m.input_classes, glyph);
            set_count = b.u16(at + 10);
            sets = at + 12;
        }
        if (set_index >= set_count)
            return false;
        std::uint32_t const set = b.offset16(at, sets + set_index * 2);
        if (set == 0)
            return false;
        std::uint32_t const rules = b.u16(set);
        for (std::uint32_t r = 0; r < rules && b.has(set + 2 + r * 2, 2); ++r) {
            std::uint32_t const rule_at = b.offset16(set, set + 2 + r * 2);
            if (rule_at == 0)
                continue;
            if (apply_rule(c, b, m, read_rule(rule_at)))
                return true;
        }
        return false;
    }
    if (format == 3) {
        RuleMatchers m;
        m.by = MatchBy::Coverage;
        m.base = at;
        Rule rule;
        rule.backtrack_count = b.u16(at + 2);
        rule.backtrack = at + 4;
        std::uint32_t const input_at = rule.backtrack + rule.backtrack_count * 2;
        rule.input_count = b.u16(input_at);
        rule.input = input_at + 2;
        std::uint32_t const lookahead_at = rule.input + rule.input_count * 2;
        rule.lookahead_count = b.u16(lookahead_at);
        rule.lookahead = lookahead_at + 2;
        std::uint32_t const records_at = rule.lookahead + rule.lookahead_count * 2;
        rule.record_count = b.u16(records_at);
        rule.records = records_at + 2;
        if (rule.input_count == 0 || coverage_index(b, b.offset16(at, rule.input), glyph) < 0)
            return false;
        return apply_rule(c, b, m, rule);
    }
    return false;
}

// --- GPOS ---------------------------------------------------------------------

unsigned value_record_size(std::uint16_t format)
{
    return static_cast<unsigned>(std::popcount(static_cast<unsigned>(format & 0xFF))) * 2;
}

// §ValueRecord: placement and advance adjustments, in design units. The
// Device and VariationIndex tables only adjust at a hinted size or a
// variation instance other than the default; the engine shapes at neither.
void apply_value(Bytes const& b, std::uint32_t at, std::uint16_t format, GlyphPosition& p)
{
    if (format & 0x0001) {
        p.x_offset += b.i16(at);
        at += 2;
    }
    if (format & 0x0002) {
        p.y_offset += b.i16(at);
        at += 2;
    }
    if (format & 0x0004) {
        p.x_advance += b.i16(at);
        at += 2;
    }
    // A vertical advance applies to vertical text only.
    if (format & 0x0008)
        at += 2;
}

// §Anchor tables: formats 1-3 give the point in design units; format 2's
// contour point and format 3's Device tables only move it at hinted sizes.
void anchor_point(Bytes const& b, std::uint32_t anchor, std::int32_t& x, std::int32_t& y)
{
    x = b.i16(anchor + 2);
    y = b.i16(anchor + 4);
}

bool apply_single_pos(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    std::uint16_t const format = b.u16(at);
    int const index = coverage_index(b, b.offset16(at, at + 2), c.buffer.cur().glyph);
    if (index < 0)
        return false;
    std::uint16_t const value_format = b.u16(at + 4);
    if (format == 1) {
        apply_value(b, at + 6, value_format, c.buffer.cur_pos());
    } else if (format == 2) {
        if (static_cast<unsigned>(index) >= b.u16(at + 6))
            return false;
        apply_value(b, at + 8 + static_cast<std::uint32_t>(index) * value_record_size(value_format), value_format,
            c.buffer.cur_pos());
    } else {
        return false;
    }
    ++c.buffer.idx;
    return true;
}

bool apply_pair_pos(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    std::uint16_t const format = b.u16(at);
    Buffer& buffer = c.buffer;
    int const index = coverage_index(b, b.offset16(at, at + 2), buffer.cur().glyph);
    if (index < 0)
        return false;
    std::uint16_t const format1 = b.u16(at + 4);
    std::uint16_t const format2 = b.u16(at + 6);
    unsigned const size1 = value_record_size(format1);
    unsigned const size2 = value_record_size(format2);
    Walker walker(c, false);
    std::size_t second = buffer.idx;
    if (!walker.next_any(second))
        return false;
    std::uint32_t const second_glyph = buffer.info[second].glyph;
    std::uint32_t values = 0;
    if (format == 1) {
        if (static_cast<unsigned>(index) >= b.u16(at + 8))
            return false;
        std::uint32_t const set = b.offset16(at, at + 10 + static_cast<std::uint32_t>(index) * 2);
        if (set == 0)
            return false;
        std::uint32_t const record_size = 2 + size1 + size2;
        std::uint32_t lo = 0;
        std::uint32_t hi = b.u16(set);
        while (lo < hi) {
            std::uint32_t const mid = (lo + hi) / 2;
            std::uint32_t const record = set + 2 + mid * record_size;
            std::uint16_t const g = b.u16(record);
            if (second_glyph < g) {
                hi = mid;
            } else if (second_glyph > g) {
                lo = mid + 1;
            } else {
                if (b.has(record, record_size))
                    values = record + 2;
                break;
            }
        }
        if (values == 0)
            return false;
        apply_value(b, values, format1, buffer.pos[buffer.idx]);
        apply_value(b, values + size1, format2, buffer.pos[second]);
    } else if (format == 2) {
        std::uint32_t const class_def1 = b.offset16(at, at + 8);
        std::uint32_t const class_def2 = b.offset16(at, at + 10);
        std::uint32_t const class1_count = b.u16(at + 12);
        std::uint32_t const class2_count = b.u16(at + 14);
        std::uint32_t const class1 = glyph_class(b, class_def1, buffer.cur().glyph);
        std::uint32_t const class2 = glyph_class(b, class_def2, second_glyph);
        if (class1 >= class1_count || class2 >= class2_count)
            return false;
        values = at + 16 + (class1 * class2_count + class2) * (size1 + size2);
        if (!b.has(values, size1 + size2))
            return false;
        apply_value(b, values, format1, buffer.pos[buffer.idx]);
        apply_value(b, values + size1, format2, buffer.pos[second]);
    } else {
        return false;
    }
    // With a second value record the second glyph is done; without, it
    // may start the next pair.
    buffer.idx = size2 ? second + 1 : second;
    return true;
}

// A cursive attachment chain that already ran through `i` is turned
// around so the whole of it hangs from the new parent.
void reverse_cursive_minor_offset(std::vector<GlyphPosition>& pos, std::size_t i, std::size_t new_parent, int depth)
{
    std::int32_t const chain = pos[i].attach_chain;
    std::uint8_t const type = pos[i].attach_type;
    if (chain == 0 || !(type & attach_cursive) || depth <= 0)
        return;
    pos[i].attach_chain = 0;
    std::ptrdiff_t const j = static_cast<std::ptrdiff_t>(i) + chain;
    if (j < 0 || static_cast<std::size_t>(j) >= pos.size() || static_cast<std::size_t>(j) == new_parent)
        return;
    reverse_cursive_minor_offset(pos, static_cast<std::size_t>(j), new_parent, depth - 1);
    pos[static_cast<std::size_t>(j)].y_offset = -pos[i].y_offset;
    pos[static_cast<std::size_t>(j)].attach_chain = -chain;
    pos[static_cast<std::size_t>(j)].attach_type = type;
}

bool apply_cursive_pos(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    if (b.u16(at) != 1)
        return false;
    Buffer& buffer = c.buffer;
    std::uint32_t const coverage = b.offset16(at, at + 2);
    std::uint32_t const count = b.u16(at + 4);
    int const this_index = coverage_index(b, coverage, buffer.cur().glyph);
    if (this_index < 0 || static_cast<unsigned>(this_index) >= count)
        return false;
    std::uint32_t const entry = b.offset16(at, at + 6 + static_cast<std::uint32_t>(this_index) * 4);
    if (entry == 0)
        return false;
    Walker walker(c, false);
    std::size_t i = buffer.idx;
    if (!walker.prev_any(i))
        return false;
    int const prev_index = coverage_index(b, coverage, buffer.info[i].glyph);
    if (prev_index < 0 || static_cast<unsigned>(prev_index) >= count)
        return false;
    std::uint32_t const exit = b.offset16(at, at + 6 + static_cast<std::uint32_t>(prev_index) * 4 + 2);
    if (exit == 0)
        return false;
    std::size_t const j = buffer.idx;
    std::int32_t exit_x;
    std::int32_t exit_y;
    std::int32_t entry_x;
    std::int32_t entry_y;
    anchor_point(b, exit, exit_x, exit_y);
    anchor_point(b, entry, entry_x, entry_y);
    std::vector<GlyphPosition>& pos = buffer.pos;
    // In the writing direction, the exit of the one meets the entry of the
    // next: the advances are cut to the anchors.
    if (!buffer.right_to_left) {
        pos[i].x_advance = exit_x + pos[i].x_offset;
        std::int32_t const d = entry_x + pos[j].x_offset;
        pos[j].x_advance -= d;
        pos[j].x_offset -= d;
    } else {
        std::int32_t const d = exit_x + pos[i].x_offset;
        pos[i].x_advance -= d;
        pos[i].x_offset -= d;
        pos[j].x_advance = entry_x + pos[j].x_offset;
    }
    // Across it, the child hangs from the parent: with the RightToLeft
    // flag the earlier glyph is the child (the last glyph of a run sits on
    // the baseline), without it the later one.
    std::size_t child = i;
    std::size_t parent = j;
    std::int32_t y_offset = entry_y - exit_y;
    if (!(c.lookup_props & lookup_flag::right_to_left)) {
        std::swap(child, parent);
        y_offset = -y_offset;
    }
    reverse_cursive_minor_offset(pos, child, parent, max_nesting);
    pos[child].attach_type = attach_cursive;
    pos[child].attach_chain = static_cast<std::int32_t>(parent) - static_cast<std::int32_t>(child);
    pos[child].y_offset = y_offset;
    // A parent that hung from this child no longer does.
    if (pos[parent].attach_chain == -pos[child].attach_chain) {
        pos[parent].attach_chain = 0;
        pos[parent].y_offset = 0;
    }
    ++buffer.idx;
    return true;
}

// §MarkArray: puts the current mark's anchor on the anchor of the glyph at
// `target` for the mark's class, read from an anchor matrix row.
bool attach_mark_to(ApplyContext& c, Bytes const& b, std::uint32_t mark_array, int mark_index, std::uint32_t row,
    std::uint32_t row_base, std::uint32_t class_count, std::size_t target)
{
    Buffer& buffer = c.buffer;
    if (mark_array == 0 || static_cast<unsigned>(mark_index) >= b.u16(mark_array))
        return false;
    std::uint32_t const record = mark_array + 2 + static_cast<std::uint32_t>(mark_index) * 4;
    std::uint32_t const mark_class = b.u16(record);
    std::uint32_t const mark_anchor = b.offset16(mark_array, record + 2);
    if (mark_class >= class_count || mark_anchor == 0)
        return false;
    std::uint32_t const base_anchor = b.offset16(row_base, row + mark_class * 2);
    if (base_anchor == 0)
        return false;
    std::int32_t mark_x;
    std::int32_t mark_y;
    std::int32_t base_x;
    std::int32_t base_y;
    anchor_point(b, mark_anchor, mark_x, mark_y);
    anchor_point(b, base_anchor, base_x, base_y);
    GlyphPosition& p = buffer.cur_pos();
    p.x_offset = base_x - mark_x;
    p.y_offset = base_y - mark_y;
    p.attach_type = attach_mark;
    p.attach_chain = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(buffer.idx);
    ++buffer.idx;
    return true;
}

// The glyph a mark attaches to: the nearest earlier glyph that is not a
// mark (by the lookup's own reading of marks), remembered for the next
// mark of the same lookup. Of a glyph sequence a multiple substitution
// made, only the first is a base — unless a mark sits within the
// sequence.
long find_base(ApplyContext& c, Bytes const& b, std::uint32_t base_coverage, bool mark_to_base)
{
    Buffer& buffer = c.buffer;
    std::uint32_t const saved = c.lookup_props;
    c.lookup_props = lookup_flag::ignore_marks;
    Walker walker(c, false);
    c.lookup_props = saved;
    if (c.last_base_until > buffer.idx) {
        c.last_base_until = 0;
        c.last_base = -1;
    }
    for (std::size_t j = buffer.idx; j > c.last_base_until; --j) {
        GlyphInfo const& g = buffer.info[j - 1];
        Verdict v = walker.classify_any(g);
        if (v == Verdict::Match && mark_to_base) {
            bool const accept = !is_multiplied(g) || lig_comp(g) == 0 || j - 1 == 0 || is_mark(buffer.info[j - 2])
                || !is_multiplied(buffer.info[j - 2]) || lig_id(g) != lig_id(buffer.info[j - 2])
                || lig_comp(g) != lig_comp(buffer.info[j - 2]) + 1;
            if (!accept && coverage_index(b, base_coverage, g.glyph) < 0)
                v = Verdict::Skip;
        }
        if (v == Verdict::Match) {
            c.last_base = static_cast<long>(j - 1);
            break;
        }
    }
    c.last_base_until = buffer.idx;
    return c.last_base;
}

bool apply_mark_base_pos(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    if (b.u16(at) != 1)
        return false;
    Buffer& buffer = c.buffer;
    int const mark_index = coverage_index(b, b.offset16(at, at + 2), buffer.cur().glyph);
    if (mark_index < 0)
        return false;
    std::uint32_t const base_coverage = b.offset16(at, at + 4);
    long const base = find_base(c, b, base_coverage, true);
    if (base < 0)
        return false;
    int const base_index = coverage_index(b, base_coverage, buffer.info[static_cast<std::size_t>(base)].glyph);
    if (base_index < 0)
        return false;
    std::uint32_t const class_count = b.u16(at + 6);
    std::uint32_t const mark_array = b.offset16(at, at + 8);
    std::uint32_t const base_array = b.offset16(at, at + 10);
    if (base_array == 0 || static_cast<unsigned>(base_index) >= b.u16(base_array))
        return false;
    std::uint32_t const row = base_array + 2 + static_cast<std::uint32_t>(base_index) * class_count * 2;
    return attach_mark_to(c, b, mark_array, mark_index, row, base_array, class_count, static_cast<std::size_t>(base));
}

bool apply_mark_lig_pos(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    if (b.u16(at) != 1)
        return false;
    Buffer& buffer = c.buffer;
    int const mark_index = coverage_index(b, b.offset16(at, at + 2), buffer.cur().glyph);
    if (mark_index < 0)
        return false;
    std::uint32_t const lig_coverage = b.offset16(at, at + 4);
    long const found = find_base(c, b, lig_coverage, false);
    if (found < 0)
        return false;
    auto const lig = static_cast<std::size_t>(found);
    int const lig_index = coverage_index(b, lig_coverage, buffer.info[lig].glyph);
    if (lig_index < 0)
        return false;
    std::uint32_t const class_count = b.u16(at + 6);
    std::uint32_t const mark_array = b.offset16(at, at + 8);
    std::uint32_t const lig_array = b.offset16(at, at + 10);
    if (lig_array == 0 || static_cast<unsigned>(lig_index) >= b.u16(lig_array))
        return false;
    std::uint32_t const attach = b.offset16(lig_array, lig_array + 2 + static_cast<std::uint32_t>(lig_index) * 2);
    if (attach == 0)
        return false;
    std::uint32_t const components = b.u16(attach);
    if (components == 0)
        return false;
    // The component the mark sat on when the ligature formed, or the last.
    unsigned const id = lig_id(buffer.info[lig]);
    unsigned const mark_id = lig_id(buffer.cur());
    unsigned const mark_comp = lig_comp(buffer.cur());
    unsigned const component = (id && id == mark_id && mark_comp > 0) ? std::min(components, mark_comp) - 1 : components - 1;
    std::uint32_t const row = attach + 2 + component * class_count * 2;
    return attach_mark_to(c, b, mark_array, mark_index, row, attach, class_count, lig);
}

bool apply_mark_mark_pos(ApplyContext& c, Bytes const& b, std::uint32_t at)
{
    if (b.u16(at) != 1)
        return false;
    Buffer& buffer = c.buffer;
    int const mark1_index = coverage_index(b, b.offset16(at, at + 2), buffer.cur().glyph);
    if (mark1_index < 0)
        return false;
    // The previous mark, by this lookup's mark filtering, whatever the
    // other ignore flags say.
    std::uint32_t const saved = c.lookup_props;
    c.lookup_props &= ~lookup_flag::ignore_flags;
    Walker walker(c, false);
    c.lookup_props = saved;
    std::size_t j = buffer.idx;
    if (!walker.prev_any(j))
        return false;
    GlyphInfo const& previous = buffer.info[j];
    if (!is_mark(previous))
        return false;
    // Both marks on the same base, or on the same ligature component, or
    // one of them a ligature of marks itself.
    unsigned const id1 = lig_id(buffer.cur());
    unsigned const id2 = lig_id(previous);
    unsigned const comp1 = lig_comp(buffer.cur());
    unsigned const comp2 = lig_comp(previous);
    bool good = false;
    if (id1 == id2)
        good = id1 == 0 || comp1 == comp2;
    else
        good = (id1 > 0 && !comp1) || (id2 > 0 && !comp2);
    if (!good)
        return false;
    std::uint32_t const mark2_coverage = b.offset16(at, at + 4);
    int const mark2_index = coverage_index(b, mark2_coverage, previous.glyph);
    if (mark2_index < 0)
        return false;
    std::uint32_t const class_count = b.u16(at + 6);
    std::uint32_t const mark1_array = b.offset16(at, at + 8);
    std::uint32_t const mark2_array = b.offset16(at, at + 10);
    if (mark2_array == 0 || static_cast<unsigned>(mark2_index) >= b.u16(mark2_array))
        return false;
    std::uint32_t const row = mark2_array + 2 + static_cast<std::uint32_t>(mark2_index) * class_count * 2;
    return attach_mark_to(c, b, mark1_array, mark1_index, row, mark2_array, class_count, j);
}

bool apply_subtable(ApplyContext& c, std::uint16_t type, std::uint32_t at)
{
    Bytes const& b = c.table.bytes;
    if (!c.is_gpos()) {
        switch (type) {
        case 1: return apply_single_subst(c, b, at);
        case 2: return apply_multiple_subst(c, b, at);
        case 3: return apply_alternate_subst(c, b, at);
        case 4: return apply_ligature_subst(c, b, at);
        case 5: return apply_context(c, b, at);
        case 6: return apply_chain_context(c, b, at);
        case 8: return apply_reverse_chain_subst(c, b, at);
        default: return false;
        }
    }
    switch (type) {
    case 1: return apply_single_pos(c, b, at);
    case 2: return apply_pair_pos(c, b, at);
    case 3: return apply_cursive_pos(c, b, at);
    case 4: return apply_mark_base_pos(c, b, at);
    case 5: return apply_mark_lig_pos(c, b, at);
    case 6: return apply_mark_mark_pos(c, b, at);
    case 7: return apply_context(c, b, at);
    case 8: return apply_chain_context(c, b, at);
    default: return false;
    }
}

}

bool ApplyContext::apply_at(std::size_t lookup_index)
{
    if (lookup_index >= table.lookups.size())
        return false;
    Lookup const& lookup = table.lookups[lookup_index];
    for (Lookup::Subtable const& subtable : lookup.subtables) {
        if (apply_subtable(*this, subtable.type, subtable.offset))
            return true;
    }
    return false;
}

bool ApplyContext::recurse(std::size_t lookup_index)
{
    if (nesting_left <= 0 || lookup_index >= table.lookups.size() || !buffer.spend())
        return false;
    Lookup const& lookup = table.lookups[lookup_index];
    if (lookup.reverse)
        return false;
    std::uint32_t const saved = lookup_props;
    lookup_props = lookup.props;
    --nesting_left;
    bool const applied = apply_at(lookup_index);
    ++nesting_left;
    lookup_props = saved;
    return applied;
}

void ApplyContext::apply_lookup(std::size_t lookup_index)
{
    if (lookup_index >= table.lookups.size() || buffer.len() == 0 || lookup_mask == 0)
        return;
    Lookup const& lookup = table.lookups[lookup_index];
    if (lookup.subtables.empty() || !lookup.digest.intersects(buffer.digest))
        return;
    lookup_props = lookup.props;
    last_base = -1;
    last_base_until = 0;
    nesting_left = max_nesting;
    if (!lookup.reverse) {
        if (!is_gpos())
            buffer.clear_output();
        buffer.idx = 0;
        bool any = false;
        while (buffer.idx < buffer.len() && !buffer.failed) {
            GlyphInfo const& g = buffer.cur();
            bool applied = false;
            if (lookup.digest.may_have(g.glyph) && (g.mask & lookup_mask) && glyph_matches_props(g, lookup_props))
                applied = apply_at(lookup_index);
            if (!applied)
                buffer.next_glyph();
            else
                buffer.spend();
            any = any || applied;
        }
        if (!is_gpos()) {
            buffer.sync();
            if (any)
                buffer.update_digest();
        }
        return;
    }
    // Reverse chaining: last glyph to first, in place.
    buffer.have_output = false;
    for (std::size_t i = buffer.len(); i > 0 && !buffer.failed; --i) {
        buffer.idx = i - 1;
        GlyphInfo const& g = buffer.cur();
        if (lookup.digest.may_have(g.glyph) && (g.mask & lookup_mask) && glyph_matches_props(g, lookup_props))
            apply_at(lookup_index);
    }
    buffer.idx = 0;
    buffer.update_digest();
}

void resolve_attachments(Buffer& buffer)
{
    std::vector<GlyphPosition>& pos = buffer.pos;
    std::size_t const len = pos.size();
    // Each attached glyph's offset becomes relative to the pen: its own
    // plus its parent's, resolved first, and for a mark the advances
    // between the two taken back out.
    auto propagate = [&](auto& self, std::size_t i, int depth) -> void {
        std::int32_t const chain = pos[i].attach_chain;
        if (chain == 0 || depth <= 0)
            return;
        std::uint8_t const type = pos[i].attach_type;
        pos[i].attach_chain = 0;
        std::ptrdiff_t const j_signed = static_cast<std::ptrdiff_t>(i) + chain;
        if (j_signed < 0 || static_cast<std::size_t>(j_signed) >= len)
            return;
        auto const j = static_cast<std::size_t>(j_signed);
        self(self, j, depth - 1);
        if (type & attach_cursive) {
            pos[i].y_offset += pos[j].y_offset;
            return;
        }
        pos[i].x_offset += pos[j].x_offset;
        pos[i].y_offset += pos[j].y_offset;
        if (!buffer.right_to_left) {
            for (std::size_t k = j; k < i; ++k) {
                pos[i].x_offset -= pos[k].x_advance;
                pos[i].y_offset -= pos[k].y_advance;
            }
        } else {
            for (std::size_t k = j + 1; k < i + 1; ++k) {
                pos[i].x_offset += pos[k].x_advance;
                pos[i].y_offset += pos[k].y_advance;
            }
        }
    };
    for (std::size_t i = 0; i < len; ++i)
        propagate(propagate, i, max_nesting);
}

}
