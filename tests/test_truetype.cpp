#include "Test.h"

#include "text/Rasterizer.h"
#include "text/SashfoldMono.h"
#include "text/TrueType.h"
#include "text/TrueTypeWriter.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

// The TrueType loop: the writer emits Sashfold Mono as a font file, the
// committed fixture must equal that emission byte for byte, and the reader
// gets back every metric, mapping and outline the face was built from —
// composites, aliases, both loca widths, a collection. Then hostile bytes:
// truncations and bit flips of the fixture must never crash the parser. Any
// system fonts the machine has are read too, as a check against the world.

using namespace sashfold;
using text::TrueTypeFont;

namespace {

std::vector<std::uint8_t> read_file(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
}

// Shoelace area of one contour; negative is clockwise with y up.
long long signed_area(text::GlyphOutline const& outline, std::size_t contour)
{
    std::size_t const first = contour == 0 ? 0 : outline.contour_ends[contour - 1] + 1u;
    std::size_t const last = outline.contour_ends[contour];
    long long area = 0;
    for (std::size_t i = first; i <= last; ++i) {
        text::GlyphPoint const& a = outline.points[i];
        text::GlyphPoint const& b = outline.points[i == last ? first : i + 1];
        area += static_cast<long long>(a.x) * b.y - static_cast<long long>(b.x) * a.y;
    }
    return area;
}

bool same_points(text::GlyphOutline const& a, std::size_t a_from, text::GlyphOutline const& b,
    std::size_t count, int dy)
{
    if (a.points.size() < a_from + count || b.points.size() < count)
        return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (a.points[a_from + i].x != b.points[i].x || a.points[a_from + i].y != b.points[i].y + dy)
            return false;
    }
    return true;
}

// Walks a parsed face the way a renderer would; the point is not to crash.
void exercise(TrueTypeFont const& font, std::size_t glyph_limit)
{
    std::size_t const count = std::min<std::size_t>(font.glyph_count(), glyph_limit);
    for (std::size_t glyph = 0; glyph < count; ++glyph) {
        auto const g = static_cast<std::uint16_t>(glyph);
        (void)font.outline(g);
        (void)font.advance_width(g);
        (void)font.left_side_bearing(g);
    }
    for (char32_t const c : { 0x41u, 0x61u, 0xE9u, 0x4E00u, 0x1F600u, 0xFFFFu })
        (void)font.glyph_index(c);
    (void)font.family_name();
}

std::size_t contours(std::optional<text::GlyphOutline> const& outline)
{
    return outline ? outline->contour_ends.size() : 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: test_truetype <SashfoldMono.ttf>\n";
        return 2;
    }
    std::string const fixture_path = argv[1];
    text::SashfoldMono const& face = text::SashfoldMono::instance();

    // --- The writer's bytes are the fixture's bytes ----------------------------------
    std::vector<std::uint8_t> const generated = face.to_truetype();
    CHECK(generated.size() > 1024);
    std::vector<std::uint8_t> const committed = read_file(fixture_path);
    if (!CHECK(committed == generated))
        std::cerr << "  the fixture drifted from the face: regenerate it with gen_font "
                  << fixture_path << "\n";

    // --- The catalogue view: names without loading ------------------------------------
    std::vector<text::FaceInfo> const scanned = TrueTypeFont::scan_file(fixture_path);
    if (CHECK_EQ(scanned.size(), 1u)) {
        CHECK_EQ(scanned[0].family, std::string("Sashfold Mono"));
        CHECK_EQ(scanned[0].subfamily, std::string("Regular"));
        CHECK_EQ(scanned[0].weight_class, 400);
        CHECK(!scanned[0].italic);
        CHECK(scanned[0].has_outlines);
        CHECK_EQ(scanned[0].face_index, 0u);
        CHECK_EQ(scanned[0].path, fixture_path);
    }
    CHECK(TrueTypeFont::scan_file(fixture_path + ".missing").empty());

    // --- Reading back what was written ------------------------------------------------
    CHECK_EQ(TrueTypeFont::face_count(generated), 1u);
    std::optional<TrueTypeFont> const parsed = TrueTypeFont::parse(generated);
    if (!CHECK(parsed.has_value()))
        return test::report("truetype");
    TrueTypeFont const& mono = *parsed;
    CHECK_EQ(mono.family_name(), std::string("Sashfold Mono"));
    CHECK_EQ(mono.subfamily_name(), std::string("Regular"));
    CHECK_EQ(mono.units_per_em(), 2048);
    CHECK_EQ(mono.ascender(), 1600);
    CHECK_EQ(mono.descender(), -448);
    CHECK_EQ(mono.line_gap(), 384); // 6 of 32 grid units: the face's leading
    CHECK_EQ(mono.x_height(), 960);
    CHECK_EQ(mono.cap_height(), 1408);
    CHECK_EQ(mono.weight_class(), 400);
    CHECK(!mono.is_bold());
    CHECK(!mono.is_italic());
    CHECK(mono.has_outlines());
    CHECK(!mono.has_cff());
    CHECK(mono.glyph_count() > 300);
    CHECK(mono.mapped_code_points() > 300);

    // Every glyph parses and is one cell wide.
    std::size_t malformed = 0;
    std::size_t off_pitch = 0;
    for (std::uint32_t glyph = 0; glyph < mono.glyph_count(); ++glyph) {
        auto const g = static_cast<std::uint16_t>(glyph);
        if (!mono.outline(g))
            ++malformed;
        if (mono.advance_width(g) != 1280)
            ++off_pitch;
    }
    CHECK_EQ(malformed, 0u);
    CHECK_EQ(off_pitch, 0u);

    // The space: mapped from every blank, empty, still one advance wide.
    std::uint16_t const space = mono.glyph_index(U' ');
    CHECK(space != 0);
    CHECK_EQ(mono.glyph_index(0x00A0), space);
    CHECK_EQ(mono.glyph_index(0x2003), space);
    CHECK_EQ(mono.glyph_index(0x3000), space);
    auto const space_outline = mono.outline(space);
    CHECK(space_outline && space_outline->points.empty());
    CHECK_EQ(mono.advance_width(space), 1280);

    // A: three strokes, so three clockwise quads of on-curve points, legs on
    // the baseline, apex on the cap line, inside the cell.
    std::uint16_t const a_glyph = mono.glyph_index(U'A');
    CHECK(a_glyph != 0);
    auto const a = mono.outline(a_glyph);
    if (CHECK(a.has_value())) {
        CHECK_EQ(a->contour_ends.size(), 3u);
        CHECK_EQ(a->points.size(), 12u);
        bool all_on_curve = true;
        for (text::GlyphPoint const& point : a->points)
            all_on_curve = all_on_curve && point.on_curve;
        CHECK(all_on_curve);
        bool all_clockwise = true;
        for (std::size_t contour = 0; contour < a->contour_ends.size(); ++contour)
            all_clockwise = all_clockwise && signed_area(*a, contour) < 0;
        CHECK(all_clockwise);
        CHECK_EQ(a->x_min, 160);
        CHECK_EQ(a->x_max, 1120);
        CHECK_EQ(a->y_min, 0);
        CHECK_EQ(a->y_max, 1408);
        CHECK_EQ(mono.left_side_bearing(a_glyph), a->x_min);
    }

    // Aliases share a glyph; what the face lacks is honestly .notdef.
    CHECK_EQ(mono.glyph_index(0xFF21), a_glyph);
    CHECK_EQ(mono.glyph_index(0x2010), mono.glyph_index(U'-'));
    CHECK_EQ(mono.glyph_index(0x2212), mono.glyph_index(0x2013));
    CHECK_EQ(mono.glyph_index(0x0430), 0);
    CHECK_EQ(mono.glyph_index(0x65E5), 0);
    CHECK_EQ(mono.glyph_index(0x1F600), 0);
    CHECK(mono.glyph_index(0xFFFD) != 0);

    // Composites, flattened: é is e then the acute in place; É lifts the
    // acute six grid rows over the capital; ế stacks two marks; í swaps in
    // the dotless i.
    auto const e = mono.outline(mono.glyph_index(U'e'));
    auto const cap_e = mono.outline(mono.glyph_index(U'E'));
    auto const acute = mono.outline(mono.glyph_index(0x0301));
    auto const circumflex = mono.outline(mono.glyph_index(0x0302));
    auto const e_acute = mono.outline(mono.glyph_index(0x00E9));
    auto const cap_e_acute = mono.outline(mono.glyph_index(0x00C9));
    auto const e_circumflex_acute = mono.outline(mono.glyph_index(0x1EBF));
    auto const dotless_i = mono.outline(mono.glyph_index(0x0131));
    auto const i_acute = mono.outline(mono.glyph_index(0x00ED));
    if (CHECK(e && cap_e && acute && circumflex && e_acute && cap_e_acute && e_circumflex_acute
            && dotless_i && i_acute)) {
        CHECK_EQ(contours(e_acute), contours(e) + contours(acute));
        CHECK_EQ(e_acute->points.size(), e->points.size() + acute->points.size());
        CHECK(same_points(*e_acute, 0, *e, e->points.size(), 0));
        CHECK(same_points(*e_acute, e->points.size(), *acute, acute->points.size(), 0));
        CHECK(same_points(*cap_e_acute, 0, *cap_e, cap_e->points.size(), 0));
        CHECK(same_points(*cap_e_acute, cap_e->points.size(), *acute, acute->points.size(), 384));
        CHECK_EQ(contours(e_circumflex_acute), contours(e) + contours(circumflex) + contours(acute));
        CHECK(same_points(*e_circumflex_acute, e->points.size() + circumflex->points.size(), *acute,
            acute->points.size(), 320));
        CHECK(same_points(*i_acute, 0, *dotless_i, dotless_i->points.size(), 0));
    }

    // --- Bold, italic, the long loca, and a collection ---------------------------------
    text::TrueTypeOptions bold_options;
    bold_options.bold = true;
    text::TrueTypeOptions italic_options;
    italic_options.italic = true;
    italic_options.long_loca = true;
    std::vector<std::uint8_t> const bold_bytes = face.to_truetype(bold_options);
    std::vector<std::uint8_t> const italic_bytes = face.to_truetype(italic_options);
    CHECK(bold_bytes != generated);
    CHECK(italic_bytes != generated);
    std::optional<TrueTypeFont> const bold = TrueTypeFont::parse(bold_bytes);
    if (CHECK(bold.has_value()) && a) {
        CHECK_EQ(bold->subfamily_name(), std::string("Bold"));
        CHECK_EQ(bold->weight_class(), 700);
        CHECK(bold->is_bold());
        CHECK_EQ(bold->glyph_count(), mono.glyph_count());
        auto const bold_a = bold->outline(bold->glyph_index(U'A'));
        CHECK(bold_a && bold_a->x_min < a->x_min && bold_a->x_max > a->x_max);
    }
    std::optional<TrueTypeFont> const italic = TrueTypeFont::parse(italic_bytes);
    if (CHECK(italic.has_value()) && a) {
        CHECK_EQ(italic->subfamily_name(), std::string("Italic"));
        CHECK(italic->is_italic());
        CHECK(!italic->is_bold());
        CHECK_EQ(italic->glyph_count(), mono.glyph_count());
        std::size_t italic_malformed = 0;
        for (std::uint32_t glyph = 0; glyph < italic->glyph_count(); ++glyph) {
            if (!italic->outline(static_cast<std::uint16_t>(glyph)))
                ++italic_malformed;
        }
        CHECK_EQ(italic_malformed, 0u);
        // The shear pivots on the baseline: the A's feet stay, its apex moves;
        // a T's top bar leans past the upright one.
        auto const italic_a = italic->outline(italic->glyph_index(U'A'));
        CHECK(italic_a && !same_points(*italic_a, 0, *a, a->points.size(), 0)
            && italic_a->x_min == a->x_min && italic_a->y_max == a->y_max);
        auto const t = mono.outline(mono.glyph_index(U'T'));
        auto const italic_t = italic->outline(italic->glyph_index(U'T'));
        CHECK(t && italic_t && italic_t->x_max > t->x_max && italic_t->y_max == t->y_max);
    }
    std::vector<std::uint8_t> const collection = text::write_collection({ generated, bold_bytes });
    CHECK_EQ(TrueTypeFont::face_count(collection), 2u);
    std::optional<TrueTypeFont> const first = TrueTypeFont::parse(collection, 0);
    std::optional<TrueTypeFont> const second = TrueTypeFont::parse(collection, 1);
    CHECK(!TrueTypeFont::parse(collection, 2));
    CHECK(first && first->subfamily_name() == "Regular" && first->glyph_count() == mono.glyph_count());
    if (CHECK(second.has_value()) && bold) {
        CHECK(second->is_bold());
        auto const from_collection = second->outline(second->glyph_index(U'A'));
        auto const from_file = bold->outline(bold->glyph_index(U'A'));
        CHECK(from_collection && from_file
            && from_collection->points.size() == from_file->points.size()
            && same_points(*from_collection, 0, *from_file, from_file->points.size(), 0));
    }

    // --- Hostile bytes: never a crash ----------------------------------------------------
    CHECK_EQ(TrueTypeFont::face_count({}), 0u);
    CHECK(!TrueTypeFont::parse({}));
    CHECK(!TrueTypeFont::parse(std::vector<std::uint8_t>(64, 0)));
    CHECK(!TrueTypeFont::parse(std::vector<std::uint8_t>(generated.begin(), generated.begin() + 12)));
    for (std::size_t length = 0; length < generated.size(); length += 97) {
        std::vector<std::uint8_t> const truncated(generated.begin(),
            generated.begin() + static_cast<std::ptrdiff_t>(length));
        if (std::optional<TrueTypeFont> const font = TrueTypeFont::parse(truncated))
            exercise(*font, 256);
    }
    std::uint32_t seed = 0x5A5A1234u;
    for (int round = 0; round < 400; ++round) {
        std::vector<std::uint8_t> corrupted = generated;
        for (int flip = 0; flip < 4; ++flip) {
            seed = seed * 1664525u + 1013904223u;
            corrupted[(seed >> 8) % corrupted.size()] ^= static_cast<std::uint8_t>(1u << ((seed >> 3) & 7));
        }
        if (std::optional<TrueTypeFont> const font = TrueTypeFont::parse(corrupted))
            exercise(*font, 256);
    }
    CHECK(true); // reached: the corruptions above crashed nothing

    // --- CFF: a font built by hand, byte by byte ---------------------------------------------
    // Five glyphs in Type 2 charstrings: .notdef; a square drawn with the
    // line operators; the same square through a local subroutine; a curve;
    // and an accented letter composed by endchar from two others through
    // the charset and the Standard Encoding.
    {
        auto const u16 = [](std::vector<std::uint8_t>& out, unsigned v) {
            out.push_back(static_cast<std::uint8_t>(v >> 8));
            out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        };
        auto const u32 = [](std::vector<std::uint8_t>& out, unsigned long v) {
            for (int shift = 24; shift >= 0; shift -= 8)
                out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
        };
        // A charstring number: the one-, two- and three-byte forms.
        auto const number = [](std::vector<std::uint8_t>& out, int v) {
            if (v >= -107 && v <= 107) {
                out.push_back(static_cast<std::uint8_t>(v + 139));
            } else if (v >= 108 && v <= 1131) {
                out.push_back(static_cast<std::uint8_t>((v - 108) / 256 + 247));
                out.push_back(static_cast<std::uint8_t>((v - 108) % 256));
            } else if (v >= -1131 && v <= -108) {
                out.push_back(static_cast<std::uint8_t>((-v - 108) / 256 + 251));
                out.push_back(static_cast<std::uint8_t>((-v - 108) % 256));
            } else {
                out.push_back(28);
                out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
                out.push_back(static_cast<std::uint8_t>(v & 0xFF));
            }
        };
        // An INDEX of the given items, with two-byte offsets.
        auto const index = [&](std::vector<std::vector<std::uint8_t>> const& items) {
            std::vector<std::uint8_t> out;
            u16(out, static_cast<unsigned>(items.size()));
            if (items.empty())
                return out;
            out.push_back(2);
            unsigned offset = 1;
            u16(out, offset);
            for (auto const& item : items) {
                offset += static_cast<unsigned>(item.size());
                u16(out, offset);
            }
            for (auto const& item : items)
                out.insert(out.end(), item.begin(), item.end());
            return out;
        };
        // A DICT integer in the five-byte form, so every layout below is fixed.
        auto const dict_int = [](std::vector<std::uint8_t>& out, long v) {
            out.push_back(29);
            for (int shift = 24; shift >= 0; shift -= 8)
                out.push_back(static_cast<std::uint8_t>((static_cast<unsigned long>(v) >> shift) & 0xFF));
        };
        std::vector<std::uint8_t> square; // 100 100 rmoveto 600 hlineto 600 vlineto -600 hlineto endchar
        number(square, 100);
        number(square, 100);
        square.push_back(21);
        number(square, 600);
        square.push_back(6);
        number(square, 600);
        square.push_back(7);
        number(square, -600);
        square.push_back(6);
        square.push_back(14);
        std::vector<std::uint8_t> via_subr; // 100 100 rmoveto (subr 0: 600 hlineto) 600 vlineto -600 hlineto endchar
        number(via_subr, 100);
        number(via_subr, 100);
        via_subr.push_back(21);
        number(via_subr, -107); // subroutine 0, biased by 107
        via_subr.push_back(10);
        number(via_subr, 600);
        via_subr.push_back(7);
        number(via_subr, -600);
        via_subr.push_back(6);
        via_subr.push_back(14);
        std::vector<std::uint8_t> subr0;
        number(subr0, 600);
        subr0.push_back(6);
        subr0.push_back(11); // return
        std::vector<std::uint8_t> curve; // 300 700 rmoveto 50 50 100 50 100 0 rrcurveto endchar
        number(curve, 300);
        number(curve, 700);
        curve.push_back(21);
        for (int v : { 50, 50, 100, 50, 100, 0 })
            number(curve, v);
        curve.push_back(8);
        curve.push_back(14);
        std::vector<std::uint8_t> composed; // 0 0 101 194 endchar: e (code 101) with acute (code 194) at (0, 0)
        number(composed, 0);
        number(composed, 0);
        number(composed, 101);
        number(composed, 194);
        composed.push_back(14);
        std::vector<std::uint8_t> const notdef { 14 };
        std::vector<std::uint8_t> const charstrings = index({ notdef, square, via_subr, curve, composed });
        std::vector<std::uint8_t> const local_subrs = index({ subr0 });
        // charset format 0: the SIDs of glyphs 1..4 — A, e, acute, eacute.
        std::vector<std::uint8_t> charset { 0 };
        for (unsigned sid : { 34u, 70u, 125u, 207u })
            u16(charset, sid);
        // The private dict: Subrs at offset 0 from its own start... which
        // is the local subrs placed right after it, so the offset is its
        // own length: 6 bytes (a five-byte integer and the operator).
        std::vector<std::uint8_t> private_dict;
        dict_int(private_dict, 6);
        private_dict.push_back(19);
        // The top dict: charset, CharStrings and Private offsets, filled in
        // once the layout is known. Four five-byte integers and three
        // operators: 4 * 5 + 3 = 23 bytes.
        std::vector<std::uint8_t> const header { 1, 0, 4, 1 };
        std::vector<std::uint8_t> const names = index({ { 'T', 'e', 's', 't' } });
        std::size_t const top_size = 23;
        std::vector<std::uint8_t> const top_index_head = [&] {
            std::vector<std::uint8_t> out;
            u16(out, 1);
            out.push_back(1);
            out.push_back(1);
            out.push_back(static_cast<std::uint8_t>(1 + top_size));
            return out;
        }();
        std::vector<std::uint8_t> const strings = index({});
        std::vector<std::uint8_t> const global_subrs = index({});
        std::size_t const after_dicts = header.size() + names.size() + top_index_head.size() + top_size
            + strings.size() + global_subrs.size();
        std::size_t const charset_at = after_dicts;
        std::size_t const charstrings_at = charset_at + charset.size();
        std::size_t const private_at = charstrings_at + charstrings.size();
        std::vector<std::uint8_t> top;
        dict_int(top, static_cast<long>(charset_at));
        top.push_back(15);
        dict_int(top, static_cast<long>(charstrings_at));
        top.push_back(17);
        dict_int(top, static_cast<long>(private_dict.size()));
        dict_int(top, static_cast<long>(private_at));
        top.push_back(18);
        CHECK_EQ(top.size(), top_size);
        std::vector<std::uint8_t> cff;
        std::vector<std::vector<std::uint8_t> const*> const parts { &header, &names, &top_index_head, &top, &strings,
            &global_subrs, &charset, &charstrings, &private_dict, &local_subrs };
        for (std::vector<std::uint8_t> const* part : parts)
            cff.insert(cff.end(), part->begin(), part->end());

        // The OpenType wrapper: head, hhea, maxp, hmtx, cmap (format 4) and
        // the CFF table, in a directory of six.
        std::vector<std::uint8_t> head(54, 0);
        head[0] = 0;
        head[1] = 1; // version 1.0
        head[12] = 0x5F;
        head[13] = 0x0F;
        head[14] = 0x3C;
        head[15] = 0xF5; // the magic number
        head[18] = 0x03;
        head[19] = 0xE8; // 1000 units per em
        std::vector<std::uint8_t> hhea(36, 0);
        hhea[0] = 0;
        hhea[1] = 1;
        hhea[4] = 0x03;
        hhea[5] = 0x20; // ascender 800
        hhea[6] = 0xFF;
        hhea[7] = 0x38; // descender -200
        hhea[34] = 0;
        hhea[35] = 5; // five metrics
        std::vector<std::uint8_t> maxp;
        u32(maxp, 0x00005000);
        u16(maxp, 5);
        std::vector<std::uint8_t> hmtx;
        for (int i = 0; i < 5; ++i) {
            u16(hmtx, 800);
            u16(hmtx, 0);
        }
        // cmap: one format 4 subtable mapping A, e, acute (U+00B4) and
        // eacute (U+00E9) to glyphs 1 to 4 — four one-code segments and
        // the terminal one.
        std::vector<std::uint8_t> cmap;
        u16(cmap, 0);
        u16(cmap, 1);
        u16(cmap, 3);
        u16(cmap, 1);
        u32(cmap, 12);
        std::vector<std::pair<unsigned, unsigned>> const mappings { { 0x41, 1 }, { 0x65, 2 }, { 0xB4, 3 }, { 0xE9, 4 } };
        unsigned const segments = static_cast<unsigned>(mappings.size()) + 1;
        u16(cmap, 4);
        u16(cmap, 16 + segments * 8); // length
        u16(cmap, 0);
        u16(cmap, segments * 2);
        u16(cmap, 8); // searchRange, entrySelector, rangeShift: unread
        u16(cmap, 2);
        u16(cmap, 0);
        for (auto const& [code, glyph] : mappings)
            u16(cmap, code);
        u16(cmap, 0xFFFF);
        u16(cmap, 0); // reservedPad
        for (auto const& [code, glyph] : mappings)
            u16(cmap, code);
        u16(cmap, 0xFFFF);
        for (auto const& [code, glyph] : mappings)
            u16(cmap, (glyph - code) & 0xFFFF); // idDelta
        u16(cmap, 1);
        for (std::size_t i = 0; i < segments; ++i)
            u16(cmap, 0); // idRangeOffset
        struct TableEntry {
            char const* tag;
            std::vector<std::uint8_t> const* bytes;
        };
        std::vector<TableEntry> const tables { { "CFF ", &cff }, { "cmap", &cmap }, { "head", &head },
            { "hhea", &hhea }, { "hmtx", &hmtx }, { "maxp", &maxp } };
        std::vector<std::uint8_t> otf;
        u32(otf, 0x4F54544Ful); // OTTO
        u16(otf, static_cast<unsigned>(tables.size()));
        u16(otf, 0);
        u16(otf, 0);
        u16(otf, 0);
        std::size_t offset = 12 + tables.size() * 16;
        for (TableEntry const& table : tables) {
            for (int i = 0; i < 4; ++i)
                otf.push_back(static_cast<std::uint8_t>(table.tag[i]));
            u32(otf, 0);
            u32(otf, static_cast<unsigned long>(offset));
            u32(otf, static_cast<unsigned long>(table.bytes->size()));
            offset += (table.bytes->size() + 3) & ~std::size_t(3);
        }
        for (TableEntry const& table : tables) {
            otf.insert(otf.end(), table.bytes->begin(), table.bytes->end());
            while (otf.size() % 4 != 0)
                otf.push_back(0);
        }

        std::optional<TrueTypeFont> const font = TrueTypeFont::parse(otf);
        if (CHECK(font.has_value())) {
            CHECK(font->has_cff());
            CHECK(font->has_outlines());
            CHECK_EQ(font->glyph_count(), std::uint16_t(5));
            CHECK_EQ(font->glyph_index(U'A'), std::uint16_t(1));
            CHECK_EQ(font->glyph_index(U'\u00E9'), std::uint16_t(4));
            // The square: one contour of four on-curve points, 100 to 700.
            auto const square_outline = font->outline(1);
            if (CHECK(square_outline.has_value())) {
                CHECK_EQ(square_outline->contour_ends.size(), std::size_t(1));
                CHECK_EQ(square_outline->points.size(), std::size_t(4));
                CHECK_EQ(square_outline->x_min, std::int16_t(100));
                CHECK_EQ(square_outline->x_max, std::int16_t(700));
                CHECK_EQ(square_outline->y_min, std::int16_t(100));
                CHECK_EQ(square_outline->y_max, std::int16_t(700));
                CHECK(std::all_of(square_outline->points.begin(), square_outline->points.end(),
                    [](text::GlyphPoint const& p) { return p.on_curve; }));
            }
            // Through the subroutine: the same square.
            auto const subr_outline = font->outline(2);
            if (CHECK(subr_outline.has_value()) && square_outline) {
                CHECK_EQ(subr_outline->points.size(), std::size_t(4));
                CHECK(same_points(*subr_outline, 0, *square_outline, 4, 0));
            }
            // The curve: a cubic rewritten as quadratics — off-curve control
            // points between on-curve ends, staying inside the curve's hull.
            auto const acute_outline = font->outline(3);
            if (CHECK(acute_outline.has_value())) {
                CHECK_EQ(acute_outline->contour_ends.size(), std::size_t(1));
                CHECK(acute_outline->points.size() >= 3);
                bool any_off = false;
                for (text::GlyphPoint const& p : acute_outline->points) {
                    any_off = any_off || !p.on_curve;
                    CHECK(p.x >= 300 && p.x <= 550 && p.y >= 700 && p.y <= 800);
                }
                CHECK(any_off);
                CHECK_EQ(acute_outline->points.front().x, std::int16_t(300));
                CHECK_EQ(acute_outline->points.front().y, std::int16_t(700));
            }
            // The accent composition: the letter's contour and the accent's.
            auto const eacute_outline = font->outline(4);
            if (CHECK(eacute_outline.has_value()) && subr_outline && acute_outline) {
                CHECK_EQ(eacute_outline->contour_ends.size(), std::size_t(2));
                CHECK_EQ(eacute_outline->points.size(), subr_outline->points.size() + acute_outline->points.size());
                CHECK(same_points(*eacute_outline, 0, *subr_outline, subr_outline->points.size(), 0));
            }
            CHECK(font->outline(0).has_value()); // .notdef: an empty outline, not a fault
            CHECK(font->outline(0)->points.empty());
            // The square rasterized at 32 px: 600 units of 1000 is 19.2 px a
            // side, about 369 pixels of ink.
            if (square_outline) {
                text::GlyphMask const mask = text::rasterize(*square_outline, font->units_per_em(), 32 * 4);
                long ink = 0;
                for (std::uint8_t const alpha : mask.alpha)
                    ink += alpha;
                CHECK(ink / 255 > 340 && ink / 255 < 400);
            }
        }
        // Hostile bytes: truncations and flips of the built font crash nothing.
        for (std::size_t length = otf.size(); length > 7; length -= 7) {
            std::vector<std::uint8_t> const truncated(otf.begin(), otf.begin() + static_cast<std::ptrdiff_t>(length));
            if (std::optional<TrueTypeFont> const f = TrueTypeFont::parse(truncated))
                exercise(*f, 8);
        }
        std::uint32_t cff_seed = 0x1234ABCDu;
        for (int round = 0; round < 400; ++round) {
            std::vector<std::uint8_t> corrupted = otf;
            for (int flip = 0; flip < 3; ++flip) {
                cff_seed = cff_seed * 1664525u + 1013904223u;
                corrupted[(cff_seed >> 8) % corrupted.size()] ^= static_cast<std::uint8_t>(1u << ((cff_seed >> 3) & 7));
            }
            if (std::optional<TrueTypeFont> const f = TrueTypeFont::parse(corrupted))
                exercise(*f, 8);
        }
        CHECK(true);
    }

    // --- The world's fonts, when the machine has them -------------------------------------
    for (char const* path : { "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/times.ttf",
             "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/msgothic.ttc",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
             "/usr/share/fonts/TTF/DejaVuSans.ttf",
             "/System/Library/Fonts/Supplemental/Arial.ttf",
             "/System/Library/Fonts/Supplemental/Times New Roman.ttf",
             // CFF-flavored OpenType, plain and CID-keyed.
             "/usr/share/fonts/gsfonts/NimbusSans-Regular.otf", "/usr/share/fonts/gsfonts/URWBookman-Light.otf",
             "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/opentype/urw-base35/NimbusSans-Regular.otf",
             "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
             "/System/Library/Fonts/Supplemental/Songti.ttc" }) {
        std::vector<std::uint8_t> const bytes = read_file(path);
        if (bytes.empty())
            continue;
        std::size_t const faces = TrueTypeFont::face_count(bytes);
        CHECK(faces >= 1);
        for (std::size_t index = 0; index < faces; ++index) {
            std::optional<TrueTypeFont> const font = TrueTypeFont::parse(bytes, index);
            if (!CHECK(font.has_value()))
                continue;
            CHECK(!font->family_name().empty());
            CHECK(font->units_per_em() >= 16);
            CHECK(font->ascender() > 0 && font->descender() < 0);
            std::uint16_t const capital = font->glyph_index(U'A');
            CHECK(capital != 0);
            CHECK(font->advance_width(capital) > 0);
            auto const outline = font->outline(capital);
            CHECK(outline && !outline->contour_ends.empty() && outline->points.size() >= 3);
            std::size_t bad = 0;
            for (std::uint32_t glyph = 0; glyph < font->glyph_count(); ++glyph) {
                if (!font->outline(static_cast<std::uint16_t>(glyph)))
                    ++bad;
            }
            CHECK_EQ(bad, 0u);
            std::cout << "  read " << path << " face " << index << ": " << font->family_name()
                      << " " << font->subfamily_name() << ", " << font->glyph_count()
                      << " glyphs, " << font->mapped_code_points() << " code points\n";
        }
    }

    return test::report("truetype");
}
