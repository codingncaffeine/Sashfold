#include "Test.h"

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
    CHECK_EQ(mono.line_gap(), 0);
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

    // --- The world's fonts, when the machine has them -------------------------------------
    for (char const* path : { "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/times.ttf",
             "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/msgothic.ttc",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
             "/usr/share/fonts/TTF/DejaVuSans.ttf",
             "/System/Library/Fonts/Supplemental/Arial.ttf",
             "/System/Library/Fonts/Supplemental/Times New Roman.ttf" }) {
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
