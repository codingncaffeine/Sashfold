#pragma once

// OpenType Layout: the GDEF, GSUB and GPOS tables of one face, parsed
// once and applied to a glyph buffer — the engine under text::shape
// (Shaper.h). Built from the OpenType specification (1.9): the common
// table formats (ScriptList, LangSys, FeatureList, LookupList, Coverage,
// ClassDef, Device and VariationIndex, FeatureVariations), GDEF's glyph
// classes, mark attachment classes and mark glyph sets, every GSUB lookup
// type (1-8) and every GPOS lookup type (1-9) in every format.
//
// Fonts are attacker-controlled: every read is bounds-checked (a read past
// a table yields zero), every loop is bounded by the data's own counts or
// by fixed limits, nested lookups stop at a fixed depth, and a buffer has
// an operation budget and a length bound — so a hostile font shapes into
// garbage at worst, never into a crash or an endless loop.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sashfold::text {

class TrueTypeFont;

namespace otl {

constexpr std::uint32_t make_tag(char a, char b, char c, char d)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24
        | static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16
        | static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8 | static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

// Bounds-checked big-endian reads over one table: past the end is zero.
struct Bytes {
    std::uint8_t const* data = nullptr;
    std::uint32_t size = 0;

    bool has(std::uint32_t at, std::uint32_t length) const { return at <= size && length <= size - at; }
    std::uint8_t u8(std::uint32_t at) const { return at < size ? data[at] : 0; }
    std::uint16_t u16(std::uint32_t at) const
    {
        return has(at, 2) ? static_cast<std::uint16_t>(data[at] << 8 | data[at + 1]) : 0;
    }
    std::int16_t i16(std::uint32_t at) const { return static_cast<std::int16_t>(u16(at)); }
    std::uint32_t u32(std::uint32_t at) const
    {
        return has(at, 4) ? static_cast<std::uint32_t>(data[at]) << 24 | static_cast<std::uint32_t>(data[at + 1]) << 16
                | static_cast<std::uint32_t>(data[at + 2]) << 8 | data[at + 3]
                          : 0;
    }
    // A 16-bit offset at `at` from `base`: the absolute position, or 0 for
    // a null offset (0 is never a valid subtable position: every table
    // starts with a header before it).
    std::uint32_t offset16(std::uint32_t base, std::uint32_t at) const
    {
        std::uint16_t const o = u16(at);
        return o == 0 || base + o >= size ? 0 : base + o;
    }
    std::uint32_t offset32(std::uint32_t base, std::uint32_t at) const
    {
        std::uint32_t const o = u32(at);
        return o == 0 || o >= size || base + o >= size ? 0 : base + o;
    }
};

// Coverage (§Coverage table): the index of a glyph, or -1.
int coverage_index(Bytes const&, std::uint32_t coverage, std::uint32_t glyph);
// ClassDef (§Class definition table): the class of a glyph, 0 when unlisted.
std::uint16_t glyph_class(Bytes const&, std::uint32_t class_def, std::uint32_t glyph);

// A small bloom filter over glyph ids: whether a lookup can possibly
// apply to a glyph, without reading its tables.
struct GlyphDigest {
    std::array<std::uint64_t, 3> masks { 0, 0, 0 };
    static constexpr unsigned shifts[3] = { 0, 4, 9 };
    void add(std::uint32_t glyph);
    void add_range(std::uint32_t first, std::uint32_t last);
    void add_all() { masks = { ~0ull, ~0ull, ~0ull }; }
    bool intersects(GlyphDigest const& other) const
    {
        for (std::size_t k = 0; k < 3; ++k) {
            if (!(masks[k] & other.masks[k]))
                return false;
        }
        return true;
    }
    bool may_have(std::uint32_t glyph) const
    {
        for (int k = 0; k < 3; ++k) {
            if (!(masks[static_cast<std::size_t>(k)] >> ((glyph >> shifts[k]) & 63) & 1))
                return false;
        }
        return true;
    }
};

// Lookup flags (§Lookup table).
namespace lookup_flag {
inline constexpr std::uint32_t right_to_left = 0x0001;
inline constexpr std::uint32_t ignore_base_glyphs = 0x0002;
inline constexpr std::uint32_t ignore_ligatures = 0x0004;
inline constexpr std::uint32_t ignore_marks = 0x0008;
inline constexpr std::uint32_t ignore_flags = 0x000E;
inline constexpr std::uint32_t use_mark_filtering_set = 0x0010;
inline constexpr std::uint32_t mark_attachment_type = 0xFF00;
}

// Glyph properties: the GDEF class as the bits the lookup flags test it
// by, the mark attachment class above them, and what substitution did.
namespace glyph_props {
inline constexpr std::uint16_t base_glyph = 0x02;
inline constexpr std::uint16_t ligature = 0x04;
inline constexpr std::uint16_t mark = 0x08;
inline constexpr std::uint16_t class_mask = 0x0E;
inline constexpr std::uint16_t substituted = 0x10;
inline constexpr std::uint16_t ligated = 0x20;
inline constexpr std::uint16_t multiplied = 0x40;
inline constexpr std::uint16_t preserve = substituted | ligated | multiplied;
}

// What the shaper knows of each character before glyphs: flags beside the
// general category and the shaping combining class.
namespace unicode_flag {
inline constexpr std::uint16_t ignorable = 0x01; // Default_Ignorable_Code_Point
inline constexpr std::uint16_t zwnj = 0x02;
inline constexpr std::uint16_t zwj = 0x04;
inline constexpr std::uint16_t hidden = 0x08; // an ignorable GSUB must still see: CGJ, Mongolian FVS, tags
inline constexpr std::uint16_t continuation = 0x10; // continues the grapheme before it
inline constexpr std::uint16_t variation_selector_fallback = 0x20;
}

struct GlyphInfo {
    char32_t codepoint = 0; // the character it came from
    std::uint32_t glyph = 0; // the glyph id once mapped
    std::uint32_t mask = 0; // the features that apply to it
    std::uint32_t cluster = 0;
    std::uint16_t props = 0; // glyph_props
    std::uint16_t unicode = 0; // unicode_flag
    std::uint8_t category = 0; // GeneralCategory
    std::uint8_t combining_class = 0; // as the shaper orders marks
    std::uint8_t lig_props = 0; // ligature id (high 3 bits), and component or count
    std::uint8_t syllable = 0;
    std::uint8_t space = 0; // which space it stands for when drawn with U+0020's glyph
    std::uint8_t shaper = 0; // a script shaper's own: the Arabic joining form, the Hangul jamo feature
    std::uint8_t shaper2 = 0;
};

struct GlyphPosition {
    std::int32_t x_advance = 0;
    std::int32_t y_advance = 0;
    std::int32_t x_offset = 0;
    std::int32_t y_offset = 0;
    std::int32_t attach_chain = 0; // to the glyph it hangs from, as an index distance
    std::uint8_t attach_type = 0;
};

inline constexpr std::uint8_t attach_mark = 1;
inline constexpr std::uint8_t attach_cursive = 2;

// Ligature bookkeeping (§GPOS mark-to-ligature needs it): a ligature glyph
// holds an id and its component count; a mark that sat on one of its
// components holds the id and the component's number, 1-based.
inline std::uint8_t lig_id(GlyphInfo const& g) { return static_cast<std::uint8_t>(g.lig_props >> 5); }
inline bool lig_is_base(GlyphInfo const& g) { return (g.lig_props & 0x10) != 0; }
inline unsigned lig_comp(GlyphInfo const& g) { return lig_is_base(g) ? 0 : (g.lig_props & 0x0F); }
inline unsigned lig_num_comps(GlyphInfo const& g)
{
    return ((g.props & glyph_props::ligature) && lig_is_base(g)) ? (g.lig_props & 0x0F) : 1;
}
inline void set_lig_props_for_ligature(GlyphInfo& g, unsigned id, unsigned count)
{
    g.lig_props = static_cast<std::uint8_t>((id << 5) | 0x10 | (count & 0x0F));
}
inline void set_lig_props_for_mark(GlyphInfo& g, unsigned id, unsigned component)
{
    g.lig_props = static_cast<std::uint8_t>((id << 5) | (component & 0x0F));
}
inline bool is_mark(GlyphInfo const& g) { return (g.props & glyph_props::mark) != 0; }
inline bool is_ligature(GlyphInfo const& g) { return (g.props & glyph_props::ligature) != 0; }
inline bool is_base_glyph(GlyphInfo const& g) { return (g.props & glyph_props::base_glyph) != 0; }
inline bool is_multiplied(GlyphInfo const& g) { return (g.props & glyph_props::multiplied) != 0; }
inline bool is_substituted(GlyphInfo const& g) { return (g.props & glyph_props::substituted) != 0; }
// Default-ignorable as lookups see it: a substitution makes it a glyph
// like any other.
inline bool is_default_ignorable(GlyphInfo const& g)
{
    return (g.unicode & unicode_flag::ignorable) && !is_substituted(g);
}

// The glyph buffer: an input array walked by `idx`, and, during a GSUB
// lookup, an output array the lookup writes to (the input's front is
// consumed as the output grows). Positions are filled for positioning.
class Buffer {
public:
    std::vector<GlyphInfo> info;
    std::vector<GlyphInfo> out;
    std::vector<GlyphPosition> pos;
    std::size_t idx = 0;
    bool have_output = false;
    bool failed = false; // a bound was hit; what is there stays as it is
    bool right_to_left = false;
    std::int64_t max_ops = 0;
    std::size_t max_len = 0;
    unsigned serial = 0;
    // Every glyph in the buffer, as a digest: a lookup none of whose
    // glyphs is here is not run at all.
    GlyphDigest digest;
    void update_digest()
    {
        digest = GlyphDigest {};
        for (GlyphInfo const& g : info)
            digest.add(g.glyph);
    }

    void begin(std::size_t length);
    std::size_t len() const { return info.size(); }
    GlyphInfo& cur(std::size_t i = 0) { return info[idx + i]; }
    GlyphInfo const& cur(std::size_t i = 0) const { return info[idx + i]; }
    GlyphPosition& cur_pos() { return pos[idx]; }
    GlyphInfo& prev() { return out.back(); }
    std::size_t out_len() const { return have_output ? out.size() : idx; }
    std::size_t backtrack_len() const { return have_output ? out.size() : idx; }
    std::size_t lookahead_len() const { return info.size() - idx; }
    GlyphInfo& backtrack(std::size_t i) { return have_output ? out[i] : info[i]; }

    bool spend(std::int64_t ops = 1)
    {
        max_ops -= ops;
        if (max_ops < 0)
            failed = true;
        return !failed;
    }
    unsigned next_lig_id()
    {
        unsigned id;
        do
            id = ++serial & 7;
        while (id == 0);
        return id;
    }

    void clear_output()
    {
        have_output = true;
        out.clear();
    }
    // The output becomes the input; positions are resized to it.
    void sync();
    void next_glyph()
    {
        if (have_output)
            out.push_back(info[idx]);
        ++idx;
    }
    void copy_glyph() { out.push_back(info[idx]); }
    void skip_glyph() { ++idx; }
    void replace_glyph(std::uint32_t glyph)
    {
        if (have_output) {
            out.push_back(info[idx]);
            out.back().glyph = glyph;
            ++idx;
        } else {
            info[idx++].glyph = glyph;
        }
    }
    GlyphInfo& output_glyph(std::uint32_t glyph)
    {
        out.push_back(info[idx]);
        out.back().glyph = glyph;
        return out.back();
    }
    bool output_allowed() const { return out.size() + (info.size() - idx) < max_len; }
    // Removes the current item, merging its cluster into a neighbor's
    // when it was a cluster of its own.
    void delete_glyph();
    // Moves the boundary between output and input so that the output holds
    // `i` items: input is copied out, or output moved back in.
    bool move_to(std::size_t i);

    // Clusters (monotonic): the items [start, end) of the input take the
    // least cluster among them, and so does anything adjacent that shared
    // a cluster with an item whose cluster changed.
    void merge_clusters(std::size_t start, std::size_t end);
    void merge_out_clusters(std::size_t start, std::size_t end);
    void reverse_range(std::size_t start, std::size_t end);
    void reverse();
    // Stable insertion sort of [start, end) by `less`, merging the clusters
    // of every item that moves with the ones it passes.
    template<typename Less>
    void sort(std::size_t start, std::size_t end, Less less)
    {
        for (std::size_t i = start + 1; i < end; ++i) {
            std::size_t j = i;
            while (j > start && less(info[i], info[j - 1]))
                --j;
            if (i == j)
                continue;
            merge_clusters(j, i + 1);
            GlyphInfo const moved = info[i];
            for (std::size_t k = i; k > j; --k)
                info[k] = info[k - 1];
            info[j] = moved;
        }
    }
};

// One lookup as the font has it: its type with an extension resolved, its
// flags, and its subtables as absolute offsets with their resolved types.
struct Lookup {
    std::uint16_t type = 0;
    std::uint32_t props = 0; // flags, with the mark filtering set in the high half
    struct Subtable {
        std::uint16_t type = 0;
        std::uint32_t offset = 0;
    };
    std::vector<Subtable> subtables;
    GlyphDigest digest;
    bool reverse = false; // GSUB type 8: applied last to first
};

// GSUB or GPOS: the common header's lists and the lookups, parsed.
struct LayoutTable {
    Bytes bytes;
    bool gpos = false;
    std::uint32_t script_list = 0;
    std::uint32_t feature_list = 0;
    std::uint32_t lookup_list = 0;
    std::uint32_t feature_variations = 0;
    std::vector<Lookup> lookups;

    bool present() const { return bytes.size != 0; }
    // §ScriptList: the script table for a tag, or 0.
    std::uint32_t find_script(std::uint32_t tag) const;
    // §Script: the LangSys for a language tag, or 0; `default_lang_sys`
    // is the script's default.
    std::uint32_t find_lang_sys(std::uint32_t script, std::uint32_t tag) const;
    std::uint32_t default_lang_sys(std::uint32_t script) const { return bytes.offset16(script, script); }
    // §LangSys: its required feature index (0xFFFF for none) and the
    // index of a feature with this tag among its features, or -1.
    std::uint16_t required_feature(std::uint32_t lang_sys) const { return lang_sys ? bytes.u16(lang_sys + 2) : 0xFFFF; }
    int find_feature(std::uint32_t lang_sys, std::uint32_t tag) const;
    std::uint32_t feature_tag(std::uint16_t feature_index) const;
    std::uint16_t feature_count() const { return bytes.u16(feature_list); }
    // §FeatureVariations: which record applies at the default instance
    // (every axis at its default, normalized 0), or -1.
    int feature_variation_at_default() const;
    // The lookup indices of a feature, with a FeatureVariations record's
    // replacement feature table used when one applies.
    std::vector<std::uint16_t> feature_lookups(std::uint16_t feature_index, int variation) const;
};

// GDEF (§GDEF): glyph classes, mark attachment classes, mark glyph sets.
struct GlyphDefinitions {
    Bytes bytes;
    std::uint32_t glyph_class_def = 0;
    std::uint32_t mark_attach_class_def = 0;
    std::uint32_t mark_glyph_sets = 0;
    bool has_glyph_classes() const { return glyph_class_def != 0; }
    // The glyph_props bits of a glyph's class: 0 when unclassified.
    std::uint16_t props_of(std::uint32_t glyph) const;
    bool mark_set_covers(std::uint32_t set, std::uint32_t glyph) const;
};

// Everything the shaper reads from one face: parsed once, then read-only
// and shared by every thread that shapes with the face.
struct LayoutTables {
    GlyphDefinitions gdef;
    LayoutTable gsub;
    LayoutTable gpos;
    Bytes kern; // the `kern` table, for fonts with no GPOS kerning
    Bytes cmap; // for variation sequences (format 14)
    std::uint32_t cmap14 = 0; // the format 14 subtable, absolute within cmap, or 0
    std::uint16_t glyph_count = 0;

    static LayoutTables parse(TrueTypeFont const&);
    // cmap format 14: the glyph of a variation sequence, or 0 when the
    // sequence is not in the font (and -1 when the font says to use the
    // base character's default glyph).
    std::int64_t variation_glyph(char32_t base, char32_t selector) const;
};

// What one lookup application needs: the tables, the buffer and the
// lookup's settings. Shaper.cpp runs lookups through this.
class ApplyContext {
public:
    ApplyContext(LayoutTables const& layout_tables, LayoutTable const& layout_table, Buffer& glyphs)
        : tables(layout_tables)
        , table(layout_table)
        , buffer(glyphs)
    {
    }

    LayoutTables const& tables;
    LayoutTable const& table;
    Buffer& buffer;
    std::uint32_t lookup_mask = 1;
    std::uint32_t lookup_props = 0;
    bool auto_zwnj = true;
    bool auto_zwj = true;
    bool random = false;
    bool per_syllable = false;
    std::uint32_t random_state = 1;
    int nesting_left = 64;
    // Mark-to-base and mark-to-ligature search back from each mark; the
    // last base found is remembered for the marks after it.
    long last_base = -1;
    std::size_t last_base_until = 0;

    // Applies a lookup over the whole buffer, the way §GSUB and §GPOS say
    // lookups are applied: at each glyph in turn the first subtable that
    // applies does, and the walk continues after what it consumed.
    void apply_lookup(std::size_t lookup_index);
    // Applies a lookup's subtables at the current glyph alone: what a
    // contextual lookup's nested lookups do.
    bool apply_at(std::size_t lookup_index);
    bool recurse(std::size_t lookup_index);

    // Whether a glyph is one this lookup sees, by its class and the
    // lookup's flags (§Lookup flags, GDEF mark classes and sets).
    bool glyph_matches_props(GlyphInfo const&, std::uint32_t props) const;

    // Sets a glyph's properties after a substitution: its GDEF class (or
    // the guess, without GDEF classes) and what was done to it.
    void set_substituted_props(GlyphInfo&, std::uint32_t glyph, std::uint16_t class_guess, bool ligature,
        bool component) const;
    void replace_glyph(std::uint32_t glyph);
    void replace_glyph_in_place(std::uint32_t glyph);
    void output_glyph_for_component(std::uint32_t glyph, std::uint16_t class_guess);
    void replace_glyph_with_ligature(std::uint32_t glyph, std::uint16_t class_guess);
    std::uint32_t random_number();

    bool is_gpos() const { return table.gpos; }
};

// Whether a GSUB lookup substitutes exactly this glyph sequence with no
// context around it: its match starts at the first glyph and takes them
// all. What a syllable shaper asks to learn which forms a font has (a
// reph for ra + virama, a below-base form for virama + consonant).
bool would_substitute(LayoutTables const&, std::size_t lookup_index, std::span<std::uint32_t const> glyphs);

// After GPOS: every mark and cursive attachment resolved to an offset from
// the pen (§GPOS: an attached glyph's offset is relative to what it hangs
// from), for a buffer in logical order shaped in `right_to_left` or not.
void resolve_attachments(Buffer&);

}

}
