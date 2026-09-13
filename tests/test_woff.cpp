#include "Test.h"

#include "core/Inflate.h"
#include "text/SashfoldMono.h"
#include "text/TrueType.h"
#include "text/Woff.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// WOFF 1.0: Sashfold Mono's own TrueType file wrapped the way a web font
// is — every table zlib-compressed, or stored — unwraps to a font the
// reader reads as the original, straight through TrueTypeFont::parse too;
// and a wrapper that lies about its tables unwraps to nothing.
//
// WOFF 2.0: the fixture font as the reference encoder (woff2_compress)
// wrote it — one brotli stream, glyf and loca transformed — unwraps to a
// font whose every glyph, advance and cmap entry are the fixture TTF's.

using namespace sashfold;
using text::TrueTypeFont;

namespace {

std::uint32_t u32(std::vector<std::uint8_t> const& bytes, std::size_t at)
{
    return static_cast<std::uint32_t>(bytes[at]) << 24 | static_cast<std::uint32_t>(bytes[at + 1]) << 16
        | static_cast<std::uint32_t>(bytes[at + 2]) << 8 | static_cast<std::uint32_t>(bytes[at + 3]);
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

std::optional<std::vector<std::uint8_t>> read_file(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::ostringstream stream;
    stream << file.rdbuf();
    std::string const text = std::move(stream).str();
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

// An sfnt wrapped as WOFF 1.0, its tables compressed or stored.
std::vector<std::uint8_t> wrap(std::vector<std::uint8_t> const& sfnt, bool compress)
{
    std::size_t const table_count = (static_cast<std::size_t>(sfnt[4]) << 8) | sfnt[5];
    std::vector<std::uint8_t> out;
    push_u32(out, 0x774F4646u);
    push_u32(out, u32(sfnt, 0));
    push_u32(out, 0); // the length, patched
    push_u16(out, static_cast<std::uint16_t>(table_count));
    push_u16(out, 0);
    std::size_t total = 12 + table_count * 16;
    for (std::size_t i = 0; i < table_count; ++i)
        total += (u32(sfnt, 12 + i * 16 + 12) + 3u) & ~3u;
    push_u32(out, static_cast<std::uint32_t>(total));
    push_u16(out, 1);
    push_u16(out, 0);
    for (int i = 0; i < 5; ++i)
        push_u32(out, 0); // no metadata, no private data
    std::size_t const directory = out.size();
    out.resize(directory + table_count * 20, 0);
    for (std::size_t i = 0; i < table_count; ++i) {
        std::size_t const record = 12 + i * 16;
        std::uint32_t const offset = u32(sfnt, record + 8);
        std::uint32_t const length = u32(sfnt, record + 12);
        std::vector<std::uint8_t> const table(sfnt.begin() + offset, sfnt.begin() + offset + length);
        std::vector<std::uint8_t> stored = compress ? zlib_compress(table) : table;
        if (stored.size() >= table.size())
            stored = table; // a table that does not shrink is stored as it is
        std::size_t const entry = directory + i * 20;
        auto const put = [&](std::size_t at, std::uint32_t value) {
            out[at] = static_cast<std::uint8_t>(value >> 24);
            out[at + 1] = static_cast<std::uint8_t>(value >> 16);
            out[at + 2] = static_cast<std::uint8_t>(value >> 8);
            out[at + 3] = static_cast<std::uint8_t>(value);
        };
        put(entry, u32(sfnt, record));
        put(entry + 4, static_cast<std::uint32_t>(out.size()));
        put(entry + 8, static_cast<std::uint32_t>(stored.size()));
        put(entry + 12, length);
        put(entry + 16, u32(sfnt, record + 4));
        out.insert(out.end(), stored.begin(), stored.end());
        while (out.size() % 4 != 0)
            out.push_back(0);
    }
    std::size_t const length = out.size();
    out[8] = static_cast<std::uint8_t>(length >> 24);
    out[9] = static_cast<std::uint8_t>(length >> 16);
    out[10] = static_cast<std::uint8_t>(length >> 8);
    out[11] = static_cast<std::uint8_t>(length);
    return out;
}

// Whether two fonts draw the same: every glyph's advance, its points with
// their on-curve bits, and its contour ends.
bool same_glyphs(TrueTypeFont const& font, TrueTypeFont const& expected)
{
    if (font.glyph_count() != expected.glyph_count())
        return false;
    for (std::uint32_t index = 0; index < expected.glyph_count(); ++index) {
        auto const glyph = static_cast<std::uint16_t>(index);
        if (font.advance_width(glyph) != expected.advance_width(glyph))
            return false;
        std::optional<text::GlyphOutline> const outline = font.outline(glyph);
        std::optional<text::GlyphOutline> const reference = expected.outline(glyph);
        if (outline.has_value() != reference.has_value())
            return false;
        if (!outline)
            continue;
        if (outline->points.size() != reference->points.size() || outline->contour_ends != reference->contour_ends)
            return false;
        for (std::size_t i = 0; i < outline->points.size(); ++i) {
            text::GlyphPoint const& a = outline->points[i];
            text::GlyphPoint const& b = reference->points[i];
            if (a.x != b.x || a.y != b.y || a.on_curve != b.on_curve)
                return false;
        }
    }
    return true;
}

}

int main(int argc, char** argv)
{
    std::vector<std::uint8_t> const sfnt = text::SashfoldMono::instance().to_truetype();
    std::optional<TrueTypeFont> const original = TrueTypeFont::parse(sfnt);
    if (!CHECK(original.has_value()))
        return test::report("woff");

    for (bool const compress : { false, true }) {
        std::vector<std::uint8_t> const woff = wrap(sfnt, compress);
        CHECK(text::is_woff(woff));
        CHECK(!text::is_woff2(woff));
        CHECK(!text::is_woff(sfnt));
        if (compress)
            CHECK(woff.size() < sfnt.size()); // the point of the wrapper
        std::optional<std::vector<std::uint8_t>> const unwrapped = text::unwrap_woff(woff);
        if (!CHECK(unwrapped.has_value()))
            continue;
        // The same tables at the same lengths, so the same font.
        CHECK_EQ(unwrapped->size(), sfnt.size());
        CHECK_EQ(TrueTypeFont::face_count(*unwrapped), 1u);
        std::optional<TrueTypeFont> const font = TrueTypeFont::parse(*unwrapped);
        if (CHECK(font.has_value())) {
            CHECK_EQ(font->family_name(), original->family_name());
            CHECK_EQ(font->glyph_count(), original->glyph_count());
            CHECK_EQ(font->units_per_em(), original->units_per_em());
            std::uint16_t const a = font->glyph_index(U'A');
            CHECK_EQ(a, original->glyph_index(U'A'));
            CHECK_EQ(font->advance_width(a), original->advance_width(a));
            std::optional<text::GlyphOutline> const outline = font->outline(a);
            std::optional<text::GlyphOutline> const expected = original->outline(a);
            CHECK(outline && expected && outline->points.size() == expected->points.size());
        }
        // The reader unwraps for itself: a WOFF is a font to it.
        CHECK_EQ(TrueTypeFont::face_count(woff), 1u);
        std::optional<TrueTypeFont> const direct = TrueTypeFont::parse(woff);
        CHECK(direct && direct->family_name() == original->family_name());

        // A wrapper cut short, one whose table claims a length its bytes do
        // not inflate to, and one whose tables would not fit the cap all
        // unwrap to nothing.
        std::vector<std::uint8_t> const short_woff(woff.begin(), woff.begin() + 60);
        CHECK(!text::unwrap_woff(short_woff).has_value());
        std::vector<std::uint8_t> lying = woff;
        lying[44 + 12 + 3] = static_cast<std::uint8_t>(lying[44 + 12 + 3] + 7); // the first table's origLength
        CHECK(!text::unwrap_woff(lying).has_value());
        CHECK(!text::unwrap_woff(woff, 1024).has_value());
    }
    CHECK(!text::unwrap_woff(sfnt).has_value()); // not a wrapper at all
    CHECK(!text::unwrap_woff({}).has_value());

    // WOFF 2.0, from the fixtures directory the test is given.
    if (!CHECK(argc >= 2))
        return test::report("woff");
    std::filesystem::path const fixtures = argv[1];
    std::optional<std::vector<std::uint8_t>> const woff2 = read_file(fixtures / "SashfoldMono.woff2");
    std::optional<std::vector<std::uint8_t>> const ttf = read_file(fixtures / "SashfoldMono.ttf");
    if (!CHECK(woff2.has_value() && ttf.has_value()))
        return test::report("woff");
    std::optional<TrueTypeFont> const reference = TrueTypeFont::parse(*ttf);
    if (!CHECK(reference.has_value()))
        return test::report("woff");
    CHECK(text::is_woff2(*woff2));
    CHECK(!text::is_woff(*woff2));
    std::optional<std::vector<std::uint8_t>> const unwrapped = text::unwrap_woff2(*woff2);
    if (CHECK(unwrapped.has_value())) {
        CHECK_EQ(TrueTypeFont::face_count(*unwrapped), 1u);
        std::optional<TrueTypeFont> const font = TrueTypeFont::parse(*unwrapped);
        if (CHECK(font.has_value())) {
            CHECK_EQ(font->family_name(), reference->family_name());
            CHECK_EQ(font->units_per_em(), reference->units_per_em());
            // The glyphs were rebuilt from the transformed streams, not
            // copied: the test that they came out right is every one of them.
            CHECK(same_glyphs(*font, *reference));
            bool same_cmap = true;
            for (char32_t c = 0; c < 0x3000; ++c)
                same_cmap = same_cmap && font->glyph_index(c) == reference->glyph_index(c);
            CHECK(same_cmap);
        }
    }
    // The reader unwraps it for itself, as it does a WOFF.
    CHECK_EQ(TrueTypeFont::face_count(*woff2), 1u);
    std::optional<TrueTypeFont> const direct = TrueTypeFont::parse(*woff2);
    CHECK(direct && same_glyphs(*direct, *reference));

    // Cut short anywhere, over the cap, or not WOFF2 at all: nothing.
    bool every_cut_refused = true;
    for (std::size_t cut = 0; cut < woff2->size(); ++cut) {
        std::vector<std::uint8_t> const cut_short(woff2->begin(), woff2->begin() + static_cast<std::ptrdiff_t>(cut));
        if (text::unwrap_woff2(cut_short).has_value())
            every_cut_refused = false;
    }
    CHECK(every_cut_refused);
    CHECK(!text::unwrap_woff2(*woff2, 1024).has_value());
    CHECK(!text::unwrap_woff2(*ttf).has_value());
    CHECK(!text::unwrap_woff2(wrap(sfnt, true)).has_value());
    // A flipped byte anywhere decodes to a font or to nothing, never a crash
    // (what the sanitizer lane runs this for).
    for (std::size_t at = 0; at < woff2->size(); ++at) {
        std::vector<std::uint8_t> flipped = *woff2;
        flipped[at] = static_cast<std::uint8_t>(flipped[at] ^ 0x5A);
        std::optional<std::vector<std::uint8_t>> const out = text::unwrap_woff2(flipped, 1u << 22);
        if (out)
            (void)TrueTypeFont::parse(*out);
    }

    return test::report("woff");
}
