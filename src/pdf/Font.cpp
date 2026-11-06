#include "pdf/Font.h"

#include "pdf/Encoding.h"
#include "pdf/Type1.h"
#include "text/Cff.h"
#include "text/FontManager.h"
#include "text/TrueType.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>

namespace sashfold::pdf {

namespace {

// Glyph space units per em in a TrueType's 'head', for its outlines.
double upem_scale(text::TrueTypeFont const& font)
{
    return font.units_per_em() > 0 ? 1.0 / font.units_per_em() : 0.001;
}

svg::Path path_of(text::GlyphOutline const& outline, double scale)
{
    // TrueType's quadratic contours: on-curve points joined by lines, an
    // off-curve point a control point, two in a row with the implied
    // on-curve point between them.
    svg::Path path;
    std::size_t start = 0;
    auto const point = [&](text::GlyphPoint const& p) {
        return svg::Point { static_cast<float>(p.x * scale), static_cast<float>(p.y * scale) };
    };
    auto const mid = [](svg::Point a, svg::Point b) { return svg::Point { (a.x + b.x) / 2, (a.y + b.y) / 2 }; };
    for (std::uint16_t const end_index : outline.contour_ends) {
        std::size_t const end = end_index;
        if (end < start || end >= outline.points.size()) {
            start = end + 1;
            continue;
        }
        std::size_t const n = end - start + 1;
        auto const at = [&](std::size_t i) { return outline.points[start + (i % n)]; };
        // Begin on an on-curve point (or the middle of the first two off-curve ones).
        std::size_t first = 0;
        while (first < n && !at(first).on_curve)
            ++first;
        svg::Point begin;
        if (first == n) {
            begin = mid(point(at(0)), point(at(1)));
            first = 0;
        } else {
            begin = point(at(first));
        }
        path.move_to(begin);
        std::optional<svg::Point> control;
        for (std::size_t k = 1; k <= n; ++k) {
            text::GlyphPoint const& p = at(first + k);
            svg::Point const q = point(p);
            if (p.on_curve) {
                if (control)
                    path.quadratic_to(*control, q);
                else
                    path.line_to(q);
                control.reset();
            } else {
                if (control)
                    path.quadratic_to(*control, mid(*control, q));
                control = q;
            }
        }
        if (control)
            path.quadratic_to(*control, begin);
        path.close();
        start = end + 1;
    }
    return path;
}

std::vector<double> numbers(Document const& doc, Object const& object)
{
    std::vector<double> out;
    for (Object const& item : doc.resolve(object).as_array())
        out.push_back(doc.resolve(item).as_number());
    return out;
}

std::string strip_subset(std::string name)
{
    // "ABCDEF+Name": six capitals and a plus mark a subset.
    if (name.size() > 7 && name[6] == '+'
        && std::all_of(name.begin(), name.begin() + 6, [](char c) { return c >= 'A' && c <= 'Z'; }))
        name = name.substr(7);
    return name;
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains(std::string const& haystack, char const* needle)
{
    return haystack.find(needle) != std::string::npos;
}

// A system font file by family, weight and style, loaded once and kept.
std::shared_ptr<text::TrueTypeFont const> system_font(std::vector<std::string> const& families, bool bold, bool italic)
{
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<text::TrueTypeFont const>> cache;
    std::vector<text::FaceInfo> const& faces = text::FontManager::instance().catalogue();
    for (std::string const& family : families) {
        text::FaceInfo const* best = nullptr;
        int best_score = -1;
        for (text::FaceInfo const& face : faces) {
            if (lower(face.family) != lower(family) || !face.has_outlines)
                continue;
            int score = 0;
            score += (face.weight_class >= 600) == bold ? 2 : 0;
            score += face.italic == italic ? 1 : 0;
            if (score > best_score) {
                best_score = score;
                best = &face;
            }
        }
        if (!best)
            continue;
        std::string const key = best->path + "#" + std::to_string(best->face_index);
        std::lock_guard const lock(mutex);
        auto const it = cache.find(key);
        if (it != cache.end())
            return it->second;
        std::ifstream in(best->path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::optional<text::TrueTypeFont> font = text::TrueTypeFont::parse(std::move(bytes), best->face_index);
        std::shared_ptr<text::TrueTypeFont const> shared = font ? std::make_shared<text::TrueTypeFont const>(std::move(*font)) : nullptr;
        cache.emplace(key, shared);
        if (shared)
            return shared;
    }
    return nullptr;
}

} // namespace

Font::~Font() = default;

std::shared_ptr<Font const> Document::font(Object const& font_object) const
{
    if (!font_object.is_ref())
        return Font::load(*this, font_object);
    Ref const ref = font_object.as_ref();
    {
        std::lock_guard const lock(m_font_mutex);
        auto const it = m_fonts.find(ref);
        if (it != m_fonts.end())
            return it->second;
    }
    // Loaded outside the lock (a font may be large); the first one stored stands.
    std::shared_ptr<Font const> loaded = Font::load(*this, font_object);
    std::lock_guard const lock(m_font_mutex);
    return m_fonts.emplace(ref, std::move(loaded)).first->second;
}

std::shared_ptr<Font const> Font::load(Document const& doc, Object const& font_object)
{
    Object const resolved = doc.resolve(font_object);
    Dict const& dict = resolved.as_dict();
    std::shared_ptr<Font> font(new Font());
    font->m_doc = &doc;
    font->m_code_glyphs.fill(-1);
    std::string const subtype = doc.resolve(dict.get("Subtype")).as_name();
    font->m_base_font = strip_subset(doc.resolve(dict.get("BaseFont")).as_name());
    if (Stream const* to_unicode = doc.resolve(dict.get("ToUnicode")).as_stream())
        font->m_to_unicode = CMap::parse(doc.decode(*to_unicode));
    if (subtype == "Type0")
        font->load_composite(doc, dict);
    else
        font->load_simple(doc, dict);
    // Ascent and descent for text boxes: the descriptor's (the descendant's for Type 0).
    Dict descriptor = doc.resolve(dict.get("FontDescriptor")).as_dict();
    if (subtype == "Type0") {
        Array const& descendants = doc.resolve(dict.get("DescendantFonts")).as_array();
        if (!descendants.empty())
            descriptor = doc.resolve(doc.resolve(descendants[0]).as_dict().get("FontDescriptor")).as_dict();
    }
    double const ascent = doc.resolve(descriptor.get("Ascent")).as_number(0);
    double const descent = doc.resolve(descriptor.get("Descent")).as_number(0);
    if (ascent > 0 && ascent < 3000)
        font->m_ascent = ascent / 1000;
    if (descent < 0 && descent > -3000)
        font->m_descent = descent / 1000;
    if (font->m_kind == Kind::Type3) {
        // A Type 3 font's box through its matrix.
        Array const& box = doc.resolve(dict.get("FontBBox")).as_array();
        if (box.size() == 4) {
            double const top = doc.resolve(box[3]).as_number(0) * font->m_type3_matrix.d;
            double const bottom = doc.resolve(box[1]).as_number(0) * font->m_type3_matrix.d;
            if (top > bottom) {
                font->m_ascent = top;
                font->m_descent = bottom;
            }
        }
    }
    return font;
}

void Font::load_program(Document const& doc, Dict const& descriptor, std::string const& subtype)
{
    Object const file1 = doc.resolve(descriptor.get("FontFile"));
    Object const file2 = doc.resolve(descriptor.get("FontFile2"));
    Object const file3 = doc.resolve(descriptor.get("FontFile3"));
    if (Stream const* stream = file2.as_stream()) {
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(doc.decode(*stream));
        if (std::optional<text::TrueTypeFont> font = text::TrueTypeFont::parse(*bytes)) {
            m_truetype = std::make_shared<text::TrueTypeFont const>(std::move(*font));
            m_program_scale = upem_scale(*m_truetype);
            for (std::size_t g = 0; std::string const& name : m_truetype->glyph_names(mac_glyph_name)) {
                if (!name.empty())
                    m_truetype_names.emplace(name, static_cast<std::uint16_t>(g));
                ++g;
            }
        }
        m_program_bytes = bytes;
        return;
    }
    if (Stream const* stream = file3.as_stream()) {
        std::string const kind = doc.resolve(stream->dict.get("Subtype")).as_name();
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(doc.decode(*stream));
        m_program_bytes = bytes;
        if (kind == "OpenType" || (bytes->size() > 4 && ((*bytes)[0] == 'O' || ((*bytes)[0] == 0 && (*bytes)[1] == 1)))) {
            if (std::optional<text::TrueTypeFont> font = text::TrueTypeFont::parse(*bytes)) {
                m_truetype = std::make_shared<text::TrueTypeFont const>(std::move(*font));
                m_program_scale = upem_scale(*m_truetype);
                return;
            }
        }
        if (std::optional<text::CffFont> cff = text::CffFont::parse(*bytes, 0, bytes->size())) {
            cff->load_names(*bytes);
            m_program_scale = static_cast<double>(cff->units_scale(1000)) * 0.001;
            m_cff = std::make_shared<text::CffFont const>(std::move(*cff));
        }
        return;
    }
    if (Stream const* stream = file1.as_stream()) {
        std::vector<std::uint8_t> const bytes = doc.decode(*stream);
        std::size_t const length1 = static_cast<std::size_t>(std::max<std::int64_t>(0, doc.resolve(stream->dict.get("Length1")).as_int(0)));
        std::size_t const length2 = static_cast<std::size_t>(std::max<std::int64_t>(0, doc.resolve(stream->dict.get("Length2")).as_int(0)));
        if (std::optional<Type1Font> type1 = Type1Font::parse(bytes, length1, length2)) {
            m_type1 = std::make_shared<Type1Font const>(std::move(*type1));
            auto const& m = m_type1->font_matrix();
            m_program_scale = m[0];
            m_program_matrix = canvas::Transform { m[0], m[1], m[2], m[3], m[4], m[5] };
        } else if (bytes.size() > 4 && (bytes[0] == 1 && bytes[1] == 0)) {
            // A CFF mislabeled as Type 1.
            auto copy = std::make_shared<std::vector<std::uint8_t>>(bytes);
            if (std::optional<text::CffFont> cff = text::CffFont::parse(*copy, 0, copy->size())) {
                cff->load_names(*copy);
                m_cff = std::make_shared<text::CffFont const>(std::move(*cff));
                m_program_bytes = copy;
            }
        }
    }
    (void)subtype;
}

void Font::choose_substitute(Dict const& descriptor, std::string const& ordering)
{
    m_substituted = true;
    std::string const name = lower(m_base_font);
    std::int64_t const flags = m_doc ? m_doc->resolve(descriptor.get("Flags")).as_int(0) : 0;
    double const weight = m_doc ? m_doc->resolve(descriptor.get("FontWeight")).as_number(0) : 0;
    bool const bold = contains(name, "bold") || contains(name, "black") || contains(name, "heavy") || contains(name, "semibold")
        || contains(name, "demi") || weight >= 600 || (flags & (1 << 18));
    bool const italic = contains(name, "italic") || contains(name, "oblique") || (flags & 64);
    bool const fixed = (flags & 1) || contains(name, "courier") || contains(name, "mono") || contains(name, "consol");
    bool const serif = (flags & 2) || contains(name, "times") || contains(name, "roman") || contains(name, "serif")
        || contains(name, "georgia") || contains(name, "garamond") || contains(name, "cambria") || contains(name, "minion")
        || contains(name, "book") || contains(name, "mincho") || contains(name, "song") || contains(name, "ming");
    std::vector<std::string> families;
    // The family the PDF names, when the system has it: "Arial-BoldMT" is "Arial".
    std::string family = m_base_font;
    for (char const* cut : { ",", "-", "+" }) {
        std::size_t const at = family.find(cut);
        if (at != std::string::npos)
            family = family.substr(0, at);
    }
    for (char const* suffix : { "MT", "PSMT", "PS" }) {
        std::string const s = suffix;
        if (family.size() > s.size() && family.compare(family.size() - s.size(), s.size(), s) == 0)
            family = family.substr(0, family.size() - s.size());
    }
    if (!family.empty())
        families.push_back(family);
    // Spaces the names leave out ("TimesNewRoman", "CourierNew").
    std::string spaced;
    for (std::size_t i = 0; i < family.size(); ++i) {
        if (i > 0 && std::isupper(static_cast<unsigned char>(family[i])) && std::islower(static_cast<unsigned char>(family[i - 1])))
            spaced += ' ';
        spaced += family[i];
    }
    if (spaced != family)
        families.push_back(spaced);
    if (!ordering.empty()) {
        // A CJK collection: the Noto CJK family of its region.
        char const* region = ordering == "Japan1" ? "JP" : ordering == "GB1" ? "SC" : ordering == "CNS1" ? "TC" : ordering == "Korea1" || ordering == "KR" ? "KR" : "JP";
        families.push_back(std::string(serif ? "Noto Serif CJK " : "Noto Sans CJK ") + region);
        families.push_back(std::string("Noto Sans CJK ") + region);
    }
    if (contains(name, "symbol")) {
        families.insert(families.begin(), { "Standard Symbols PS", "Symbol" });
    } else if (contains(name, "dingbat")) {
        families.insert(families.begin(), { "D050000L", "Dingbats" });
    } else if (contains(name, "arial") || contains(name, "helvetica")) {
        families.insert(families.end(), { "Liberation Sans", "Nimbus Sans", "Arimo" });
    } else if (contains(name, "calibri")) {
        families.insert(families.end(), { "Carlito" });
    } else if (contains(name, "cambria")) {
        families.insert(families.end(), { "Caladea" });
    }
    if (fixed)
        families.insert(families.end(), { "Nimbus Mono PS", "Liberation Mono", "Cousine", "DejaVu Sans Mono" });
    else if (serif)
        families.insert(families.end(), { "Nimbus Roman", "Liberation Serif", "Tinos", "DejaVu Serif" });
    families.insert(families.end(), { "Nimbus Sans", "Liberation Sans", "Arimo", "DejaVu Sans", "Noto Sans" });
    m_truetype = system_font(families, bold, italic);
    m_program_scale = m_truetype ? upem_scale(*m_truetype) : 0.001;
    m_type1.reset();
    m_cff.reset();
    m_program_bytes.reset();
}

void Font::load_simple(Document const& doc, Dict const& dict)
{
    std::string const subtype = doc.resolve(dict.get("Subtype")).as_name();
    m_kind = subtype == "Type3" ? Kind::Type3 : Kind::Simple;
    Dict const& descriptor = doc.resolve(dict.get("FontDescriptor")).as_dict();
    std::int64_t const flags = doc.resolve(descriptor.get("Flags")).as_int(0);
    m_symbolic = (flags & 4) && !(flags & 32);
    // Widths.
    std::int64_t const first = doc.resolve(dict.get("FirstChar")).as_int(0);
    std::vector<double> const widths = numbers(doc, dict.get("Widths"));
    m_missing_width = doc.resolve(descriptor.get("MissingWidth")).as_number(0);
    for (std::size_t i = 0; i < widths.size(); ++i) {
        std::int64_t const code = first + static_cast<std::int64_t>(i);
        if (code >= 0 && code < 256) {
            m_widths[static_cast<std::size_t>(code)] = widths[i];
            m_has_width[static_cast<std::size_t>(code)] = true;
        }
    }
    if (m_kind == Kind::Type3) {
        Array const& matrix = doc.resolve(dict.get("FontMatrix")).as_array();
        if (matrix.size() == 6) {
            canvas::Transform const t { doc.resolve(matrix[0]).as_number(), doc.resolve(matrix[1]).as_number(), doc.resolve(matrix[2]).as_number(),
                doc.resolve(matrix[3]).as_number(), doc.resolve(matrix[4]).as_number(), doc.resolve(matrix[5]).as_number() };
            if (t.is_finite())
                m_type3_matrix = t;
        }
        m_type3_resources = doc.resolve(dict.get("Resources")).as_dict();
        m_char_procs = doc.resolve(dict.get("CharProcs")).as_dict();
    } else {
        load_program(doc, descriptor, subtype);
    }
    // The encoding: the program's own as the base for a symbolic font (or
    // one with no /Encoding), else StandardEncoding; then the named base;
    // then the differences.
    std::array<char const*, 256> const* base = nullptr;
    std::string const base_lower = lower(m_base_font);
    bool const symbol_font = contains(base_lower, "symbol");
    bool const dingbats_font = contains(base_lower, "dingbat");
    Object const encoding = doc.resolve(dict.get("Encoding"));
    Object base_name = encoding.is_name() ? encoding : doc.resolve(encoding.as_dict().get("BaseEncoding"));
    if (std::optional<BaseEncoding> const named = base_encoding_named(base_name.as_name()))
        base = &base_encoding(*named);
    bool use_program_encoding = !base && (m_symbolic || encoding.is_null() || !base_name.is_name());
    if (!base && !use_program_encoding)
        base = &base_encoding(BaseEncoding::Standard);
    if (use_program_encoding) {
        if (m_type1 && !m_type1->standard_encoding()) {
            m_names = m_type1->encoding();
        } else if (m_cff && !m_cff->encoding().empty()) {
            for (std::size_t code = 0; code < 256; ++code) {
                if (std::uint16_t const g = m_cff->encoding()[code])
                    m_code_glyphs[code] = g;
            }
        } else if (symbol_font) {
            base = &base_encoding(BaseEncoding::Symbol);
        } else if (dingbats_font) {
            base = &base_encoding(BaseEncoding::ZapfDingbats);
        } else if (!m_truetype || m_substituted || !m_program_bytes) {
            base = &base_encoding(BaseEncoding::Standard);
        }
        if (m_type1 && m_type1->standard_encoding())
            base = &base_encoding(BaseEncoding::Standard);
    }
    if (base) {
        for (std::size_t code = 0; code < 256; ++code) {
            if ((*base)[code])
                m_names[code] = (*base)[code];
        }
    }
    Array const& differences = doc.resolve(encoding.as_dict().get("Differences")).as_array();
    std::int64_t code = 0;
    for (Object const& item_in : differences) {
        Object const item = doc.resolve(item_in);
        if (item.is_number()) {
            code = item.as_int(0);
        } else if (item.is_name()) {
            if (code >= 0 && code < 256) {
                m_names[static_cast<std::size_t>(code)] = item.as_name();
                m_code_glyphs[static_cast<std::size_t>(code)] = -1; // a named glyph wins over the program's encoding
            }
            ++code;
        }
    }
    if (m_kind == Kind::Type3)
        return;
    bool const embedded = m_truetype || m_cff || m_type1;
    if (!embedded)
        choose_substitute(descriptor, {});
    // Each code's glyph, decided now for the programs that number them.
    for (std::size_t c = 0; c < 256; ++c) {
        if (m_code_glyphs[c] >= 0)
            continue;
        std::string const& name = m_names[c];
        if (m_cff) {
            if (!name.empty()) {
                if (std::optional<std::uint16_t> const g = m_cff->glyph_by_name(name))
                    m_code_glyphs[c] = *g;
            }
            continue;
        }
        if (!m_truetype)
            continue;
        std::optional<std::uint16_t> glyph;
        auto const accept = [&](std::optional<std::uint16_t> g) {
            if (!glyph && g && *g != 0)
                glyph = g;
        };
        if (m_substituted) {
            // The URW Symbol and Dingbats fonts key their cmap by the
            // encoding's own codes, not by Unicode: asked by code first.
            std::string const family = lower(m_truetype->family_name());
            if ((symbol_font || dingbats_font) && (family == "standard symbols ps" || family == "d050000l"))
                accept(m_truetype->glyph_index(static_cast<char32_t>(c)));
            // Otherwise by the text the glyph stands for.
            std::u32string const text = !name.empty() ? glyph_name_unicode(name) : std::u32string {};
            if (!text.empty())
                accept(m_truetype->glyph_index(text[0]));
            if (!glyph && (symbol_font || dingbats_font || m_symbolic))
                accept(m_truetype->subtable_glyph(3, 0, 0xF000 + static_cast<char32_t>(c)));
            if (!glyph && name.empty())
                accept(m_truetype->glyph_index(static_cast<char32_t>(c)));
        } else {
            // §9.6.5.4: a nonsymbolic font by name through (3,1) or (1,0); a
            // symbolic one by code through (3,0) and (1,0).
            if (!m_symbolic && !name.empty()) {
                std::u32string const text = glyph_name_unicode(name);
                if (!text.empty())
                    accept(m_truetype->subtable_glyph(3, 1, text[0]));
                if (std::optional<int> const mac = code_in(base_encoding(BaseEncoding::MacRoman), name))
                    accept(m_truetype->subtable_glyph(1, 0, static_cast<char32_t>(*mac)));
                auto const named = m_truetype_names.find(name);
                if (named != m_truetype_names.end())
                    accept(named->second);
                if (!text.empty())
                    accept(m_truetype->subtable_glyph(0, 3, text[0]));
            }
            for (char32_t const offset : { 0x0000u, 0xF000u, 0xF100u, 0xF200u })
                accept(m_truetype->subtable_glyph(3, 0, offset + static_cast<char32_t>(c)));
            accept(m_truetype->subtable_glyph(1, 0, static_cast<char32_t>(c)));
            if (!name.empty()) {
                auto const named = m_truetype_names.find(name);
                if (named != m_truetype_names.end())
                    accept(named->second);
                std::u32string const text = glyph_name_unicode(name);
                if (!text.empty())
                    accept(m_truetype->subtable_glyph(3, 1, text[0]));
            }
            // No cmap that says: the code is the glyph, as viewers read such subsets.
            if (!glyph && !m_truetype->subtable_glyph(3, 1, 0x20) && !m_truetype->subtable_glyph(3, 0, 0xF020)
                && !m_truetype->subtable_glyph(1, 0, 0x20) && c < m_truetype->glyph_count())
                glyph = static_cast<std::uint16_t>(c);
        }
        if (glyph)
            m_code_glyphs[c] = *glyph;
    }
}

void Font::load_composite(Document const& doc, Dict const& dict)
{
    m_kind = Kind::Composite;
    Object const encoding = doc.resolve(dict.get("Encoding"));
    if (encoding.is_name())
        m_encoding_cmap = CMap::predefined(encoding.as_name());
    else if (Stream const* stream = encoding.as_stream())
        m_encoding_cmap = CMap::parse(doc.decode(*stream));
    if (!m_encoding_cmap)
        m_encoding_cmap = CMap::identity(false);
    m_vertical = m_encoding_cmap->vertical();
    Array const& descendants = doc.resolve(dict.get("DescendantFonts")).as_array();
    Dict const cid_font = descendants.empty() ? Dict {} : doc.resolve(descendants[0]).as_dict();
    std::string const subtype = doc.resolve(cid_font.get("Subtype")).as_name();
    Dict const& system_info = doc.resolve(cid_font.get("CIDSystemInfo")).as_dict();
    std::string const registry = doc.resolve(system_info.get("Registry")).as_string();
    std::string const ordering = doc.resolve(system_info.get("Ordering")).as_string();
    if (!registry.empty() && !ordering.empty()) {
        m_collection = registry + "-" + ordering;
        if (registry == "Adobe" && ordering != "Identity")
            m_collection_unicode = cid_to_unicode_table(m_collection);
    }
    // Widths: /W [c [w...] c1 c2 w ...], /DW; vertically /W2 and /DW2.
    m_default_width = doc.resolve(cid_font.get("DW")).as_number(1000);
    Array const& w = doc.resolve(cid_font.get("W")).as_array();
    for (std::size_t i = 0; i < w.size();) {
        Object const a = doc.resolve(w[i]);
        if (i + 1 >= w.size())
            break;
        Object const b = doc.resolve(w[i + 1]);
        if (a.is_number() && b.is_array()) {
            std::uint32_t cid = static_cast<std::uint32_t>(std::max<std::int64_t>(0, a.as_int()));
            for (Object const& width : b.as_array()) {
                if (m_cid_widths.size() > 200000)
                    break;
                m_cid_widths[cid++] = doc.resolve(width).as_number();
            }
            i += 2;
        } else if (a.is_number() && b.is_number() && i + 2 < w.size()) {
            std::int64_t const lo = a.as_int();
            std::int64_t const hi = b.as_int();
            double const width = doc.resolve(w[i + 2]).as_number();
            for (std::int64_t cid = std::max<std::int64_t>(0, lo); cid <= hi && cid - lo < 65536 && m_cid_widths.size() < 200000; ++cid)
                m_cid_widths[static_cast<std::uint32_t>(cid)] = width;
            i += 3;
        } else {
            ++i;
        }
    }
    std::vector<double> const dw2 = numbers(doc, cid_font.get("DW2"));
    if (dw2.size() == 2)
        m_default_vertical = { dw2[0], dw2[1] };
    Array const& w2 = doc.resolve(cid_font.get("W2")).as_array();
    for (std::size_t i = 0; i < w2.size();) {
        Object const a = doc.resolve(w2[i]);
        if (i + 1 >= w2.size())
            break;
        Object const b = doc.resolve(w2[i + 1]);
        if (a.is_number() && b.is_array()) {
            std::uint32_t cid = static_cast<std::uint32_t>(std::max<std::int64_t>(0, a.as_int()));
            Array const& values = b.as_array();
            for (std::size_t k = 0; k + 2 < values.size(); k += 3)
                m_cid_vertical[cid++] = { doc.resolve(values[k]).as_number(), doc.resolve(values[k + 1]).as_number(), doc.resolve(values[k + 2]).as_number() };
            i += 2;
        } else if (a.is_number() && b.is_number() && i + 4 < w2.size()) {
            std::array<double, 3> const v { doc.resolve(w2[i + 2]).as_number(), doc.resolve(w2[i + 3]).as_number(), doc.resolve(w2[i + 4]).as_number() };
            for (std::int64_t cid = std::max<std::int64_t>(0, a.as_int()); cid <= b.as_int() && cid - a.as_int() < 65536; ++cid)
                m_cid_vertical[static_cast<std::uint32_t>(cid)] = v;
            i += 5;
        } else {
            ++i;
        }
    }
    // CIDToGIDMap: Identity, or a stream of two bytes per CID.
    Object const map = doc.resolve(cid_font.get("CIDToGIDMap"));
    if (Stream const* stream = map.as_stream()) {
        std::vector<std::uint8_t> const bytes = doc.decode(*stream);
        m_cid_to_gid.resize(bytes.size() / 2);
        for (std::size_t i = 0; i < m_cid_to_gid.size(); ++i)
            m_cid_to_gid[i] = static_cast<std::uint16_t>((bytes[2 * i] << 8) | bytes[2 * i + 1]);
        m_cid_to_gid_identity = false;
    }
    Dict const& descriptor = doc.resolve(cid_font.get("FontDescriptor")).as_dict();
    load_program(doc, descriptor, subtype);
    if (!m_truetype && !m_cff && !m_type1)
        choose_substitute(descriptor, ordering);
}

bool Font::next(std::string_view bytes, std::size_t& at, ShownGlyph& glyph) const
{
    if (at >= bytes.size())
        return false;
    glyph = ShownGlyph {};
    if (m_kind != Kind::Composite) {
        std::uint8_t const code = static_cast<std::uint8_t>(bytes[at++]);
        glyph.code = code;
        glyph.cid = code;
        glyph.length = 1;
        glyph.word_space = code == 32;
        double width = m_has_width[code] ? m_widths[code] : m_missing_width;
        if (!m_has_width[code] && width == 0) {
            // No /Widths: the program's own advance (the standard 14 fonts' metrics come from their substitutes).
            if (std::optional<std::uint16_t> const g = glyph_of(glyph))
                width = program_advance(*g);
            else if (m_type1 && !m_names[code].empty()) {
                double advance = 0;
                if (m_type1->glyph(m_names[code], &advance))
                    width = advance * m_program_scale * 1000;
            }
        }
        if (m_kind == Kind::Type3) {
            // Type 3 widths are in glyph space: through the font matrix to text space.
            glyph.width = width * m_type3_matrix.a;
        } else {
            glyph.width = width / 1000;
        }
        return true;
    }
    std::uint32_t code = 0;
    int length = 1;
    if (!m_encoding_cmap->next_code(bytes, at, code, length))
        return false;
    glyph.code = code;
    glyph.length = length;
    glyph.cid = m_encoding_cmap->cid(code, length);
    glyph.word_space = length == 1 && code == 32;
    auto const it = m_cid_widths.find(glyph.cid);
    glyph.width = (it != m_cid_widths.end() ? it->second : m_default_width) / 1000;
    if (m_vertical) {
        auto const v = m_cid_vertical.find(glyph.cid);
        if (v != m_cid_vertical.end()) {
            glyph.vertical_advance = v->second[0] / 1000;
            glyph.origin_x = v->second[1] / 1000;
            glyph.origin_y = v->second[2] / 1000;
        } else {
            glyph.vertical_advance = m_default_vertical[1] / 1000;
            glyph.origin_x = glyph.width / 2;
            glyph.origin_y = m_default_vertical[0] / 1000;
        }
    }
    return true;
}

std::optional<std::uint16_t> Font::glyph_of(ShownGlyph const& glyph) const
{
    if (m_kind == Kind::Simple) {
        std::int32_t const g = m_code_glyphs[glyph.code & 0xFF];
        if (g >= 0)
            return static_cast<std::uint16_t>(g);
        return std::nullopt;
    }
    if (m_kind != Kind::Composite)
        return std::nullopt;
    std::uint32_t const cid = glyph.cid;
    if (m_substituted && m_truetype) {
        // A substitute is looked up by the text the CID stands for.
        std::u32string const text = this->text(glyph);
        if (!text.empty())
            return m_truetype->glyph_index(text[0]);
        if (m_encoding_cmap->is_identity())
            return std::nullopt;
        return std::nullopt;
    }
    if (m_cff)
        return m_cff->glyph_by_cid(static_cast<std::uint16_t>(std::min<std::uint32_t>(cid, 0xFFFF)));
    if (m_truetype) {
        if (!m_cid_to_gid_identity)
            return cid < m_cid_to_gid.size() ? std::optional<std::uint16_t>(m_cid_to_gid[cid]) : std::nullopt;
        return static_cast<std::uint16_t>(std::min<std::uint32_t>(cid, 0xFFFF));
    }
    return std::nullopt;
}

double Font::program_advance(std::uint16_t glyph) const
{
    if (m_truetype)
        return m_truetype->advance_width(glyph) * upem_scale(*m_truetype) * 1000;
    return 0;
}

std::shared_ptr<svg::Path const> Font::build_outline(std::uint16_t glyph, std::string const& type1_name) const
{
    if (m_type1) {
        std::optional<svg::Path> path = m_type1->glyph(type1_name);
        if (!path)
            return nullptr;
        // Through the font matrix to glyph space where 1 = the font size.
        svg::Matrix const m { static_cast<float>(m_program_matrix.a), static_cast<float>(m_program_matrix.b),
            static_cast<float>(m_program_matrix.c), static_cast<float>(m_program_matrix.d), static_cast<float>(m_program_matrix.e),
            static_cast<float>(m_program_matrix.f) };
        return std::make_shared<svg::Path const>(path->transformed(m));
    }
    if (m_cff && m_program_bytes) {
        text::GlyphOutline outline;
        if (!m_cff->outline(*m_program_bytes, glyph, outline))
            return nullptr;
        return std::make_shared<svg::Path const>(path_of(outline, m_program_scale));
    }
    if (m_truetype) {
        std::optional<text::GlyphOutline> const outline = m_truetype->outline(glyph);
        if (!outline)
            return nullptr;
        return std::make_shared<svg::Path const>(path_of(*outline, m_program_scale));
    }
    return nullptr;
}

std::shared_ptr<svg::Path const> Font::outline(ShownGlyph const& glyph) const
{
    if (m_kind == Kind::Type3)
        return nullptr;
    std::uint32_t key = 0;
    std::uint16_t gid = 0;
    std::string name;
    if (m_type1 && m_kind == Kind::Simple) {
        name = m_names[glyph.code & 0xFF];
        if (name.empty() || !m_type1->has_glyph(name))
            return nullptr;
        key = 0x1000000u | (glyph.code & 0xFF);
    } else {
        std::optional<std::uint16_t> const g = glyph_of(glyph);
        if (!g)
            return nullptr;
        gid = *g;
        key = gid;
    }
    // A substitute's glyph is condensed to the PDF's width when it is wider.
    double condense = 1;
    if (m_substituted && m_truetype && glyph.width > 0) {
        double const advance = program_advance(gid) / 1000;
        if (advance > glyph.width * 1.01 && advance > 0)
            condense = glyph.width / advance;
        key ^= static_cast<std::uint32_t>(std::lround(condense * 1000)) << 17;
    }
    {
        std::lock_guard const lock(m_mutex);
        auto const it = m_outlines.find(key);
        if (it != m_outlines.end())
            return it->second;
    }
    std::shared_ptr<svg::Path const> path = build_outline(gid, name);
    if (path && condense != 1)
        path = std::make_shared<svg::Path const>(path->transformed(svg::Matrix::scale(static_cast<float>(condense), 1)));
    std::lock_guard const lock(m_mutex);
    if (m_outlines.size() < 65536)
        m_outlines.emplace(key, path);
    return path;
}

std::u32string Font::text(ShownGlyph const& glyph) const
{
    if (m_to_unicode) {
        std::u32string text = m_to_unicode->unicode(glyph.code, glyph.length);
        if (!text.empty())
            return text;
    }
    if (m_kind != Kind::Composite) {
        std::string const& name = m_names[glyph.code & 0xFF];
        if (!name.empty())
            return glyph_name_unicode(name);
        return {};
    }
    if (m_collection_unicode && glyph.cid < m_collection_unicode->size() && (*m_collection_unicode)[glyph.cid] != 0)
        return std::u32string(1, (*m_collection_unicode)[glyph.cid]);
    // A predefined Unicode CMap's code is its text (UniJIS-UCS2-H and the like).
    if (m_encoding_cmap && m_encoding_cmap->name().rfind("Uni", 0) == 0 && glyph.length == 2)
        return std::u32string(1, static_cast<char32_t>(glyph.code));
    return {};
}

Stream const* Font::type3_procedure(ShownGlyph const& glyph) const
{
    if (m_kind != Kind::Type3 || !m_doc)
        return nullptr;
    std::string const& name = m_names[glyph.code & 0xFF];
    if (name.empty())
        return nullptr;
    Object const* proc = m_char_procs.find(name);
    if (!proc)
        return nullptr;
    // The stream is owned by the document's cache, which outlives every render.
    return m_doc->resolve(*proc).as_stream();
}

}
