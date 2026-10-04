#include "text/FontManager.h"

#include "core/Ascii.h"
#include "core/LineBreak.h"
#include "platform/Fonts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>

namespace sashfold::text {

namespace {

std::string lowercased(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text)
        out += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
    return out;
}

// What each generic family means on a machine: the first name found wins.
// One list serves every OS; the names that are not installed cost nothing.
std::vector<std::string_view> generic_candidates(std::string_view generic)
{
    static constexpr std::array<std::string_view, 8> serif { "Times New Roman", "Times", "DejaVu Serif",
        "Liberation Serif", "Noto Serif", "Georgia", "Tinos", "FreeSerif" };
    static constexpr std::array<std::string_view, 9> sans { "Arial", "Helvetica", "Helvetica Neue",
        "DejaVu Sans", "Liberation Sans", "Noto Sans", "Arimo", "FreeSans", "Segoe UI" };
    static constexpr std::array<std::string_view, 5> system_ui { "Segoe UI", "Helvetica Neue", "Ubuntu",
        "Cantarell", "Noto Sans" };
    static constexpr std::array<std::string_view, 9> monospace { "Consolas", "Menlo", "DejaVu Sans Mono",
        "Liberation Mono", "Noto Sans Mono", "Courier New", "Cousine", "FreeMono", "Courier" };
    static constexpr std::array<std::string_view, 3> cursive { "Comic Sans MS", "Apple Chancery",
        "URW Chancery L" };
    static constexpr std::array<std::string_view, 2> fantasy { "Impact", "Papyrus" };

    std::vector<std::string_view> out;
    auto const add = [&](auto const& names) { out.insert(out.end(), names.begin(), names.end()); };
    if (generic == "serif")
        add(serif);
    else if (generic == "sans-serif")
        add(sans);
    else if (generic == "system-ui" || generic == "ui-sans-serif") {
        add(system_ui);
        add(sans);
    } else if (generic == "monospace" || generic == "ui-monospace")
        add(monospace);
    else if (generic == "cursive") {
        add(cursive);
        add(sans);
    } else if (generic == "fantasy") {
        add(fantasy);
        add(sans);
    } else if (generic == "ui-serif" || generic == "ui-rounded" || generic == "math" || generic == "emoji"
        || generic == "fangsong") {
        add(serif);
    }
    return out;
}

// Faces worth asking first when the page's own fonts lack a glyph: broad
// Latin and symbol coverage, then the CJK workhorses, then the colour
// emoji fonts for what nothing else has.
constexpr std::array<std::string_view, 24> fallback_families { "Segoe UI", "Arial Unicode MS", "Noto Sans",
    "DejaVu Sans", "Segoe UI Symbol", "Segoe UI Historic", "Arial", "Helvetica", "Times New Roman",
    "Lucida Sans Unicode", "Apple Symbols", "Yu Gothic", "Meiryo", "MS Gothic", "Microsoft YaHei",
    "Microsoft JhengHei", "Malgun Gothic", "Hiragino Sans", "PingFang SC", "Noto Sans CJK JP",
    "Noto Sans CJK SC", "Segoe UI Emoji", "Noto Color Emoji", "Apple Color Emoji" };

bool has_font_extension(std::filesystem::path const& path)
{
    std::string const extension = lowercased(path.extension().string());
    return extension == ".ttf" || extension == ".ttc" || extension == ".otf";
}

// FNV-1a over a font file: the key that tells one page font's bytes from
// another's without keeping the bytes.
std::uint64_t fnv1a(std::vector<std::uint8_t> const& bytes)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (std::uint8_t const byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

// How far a face is from what was asked (css-fonts-4 §5.2), lower being
// closer. The stretch decides first: for a normal or narrower request
// every face at or below it, nearest first, comes before any wider one;
// for a wider request the other way round. Then the slant. Then the
// weight: a request between 400 and 500 takes the nearest above up to
// 500, then below, then above 500; a lighter request takes the nearest
// below then above; a bolder one the nearest above then below.
long match_distance(int stretch, int weight, bool italic, int wanted_stretch, int wanted_weight, bool wanted_italic)
{
    constexpr long far_side = 1000; // past every distance on the near side
    long stretch_score = 0;
    if (wanted_stretch <= 100)
        stretch_score = stretch <= wanted_stretch ? wanted_stretch - stretch : far_side + (stretch - wanted_stretch);
    else
        stretch_score = stretch >= wanted_stretch ? stretch - wanted_stretch : far_side + (wanted_stretch - stretch);
    long weight_score = 0;
    if (wanted_weight >= 400 && wanted_weight <= 500) {
        if (weight >= wanted_weight && weight <= 500)
            weight_score = weight - wanted_weight;
        else if (weight < wanted_weight)
            weight_score = far_side + (wanted_weight - weight);
        else
            weight_score = 2 * far_side + (weight - 500);
    } else if (wanted_weight < 400) {
        weight_score = weight <= wanted_weight ? wanted_weight - weight : far_side + (weight - wanted_weight);
    } else {
        weight_score = weight >= wanted_weight ? weight - wanted_weight : far_side + (wanted_weight - weight);
    }
    return stretch_score * 1000000L + (italic != wanted_italic ? 100000L : 0L) + weight_score;
}

// A face kept to its unicode-range: it has a glyph for a code point in
// the range and none for any other, and draws and measures as the face
// it wraps.
class RangedFace final : public Face {
public:
    RangedFace(Face const& inner, std::vector<std::pair<char32_t, char32_t>> ranges)
        : m_inner(inner)
        , m_ranges(std::move(ranges))
    {
    }

    std::string const& family() const override { return m_inner.family(); }
    bool is_bold() const override { return m_inner.is_bold(); }
    bool is_italic() const override { return m_inner.is_italic(); }
    bool designs_every_style() const override { return m_inner.designs_every_style(); }
    bool is_monospace() const override { return m_inner.is_monospace(); }
    bool covers(char32_t code_point) const override
    {
        for (auto const& [first, last] : m_ranges) {
            if (code_point >= first && code_point <= last)
                return true;
        }
        return false;
    }
    std::uint32_t glyph_index(char32_t code_point) const override
    {
        return covers(code_point) ? m_inner.glyph_index(code_point) : 0;
    }
    FaceMetrics metrics(float size) const override { return m_inner.metrics(size); }
    float advance(std::uint32_t glyph, float size) const override { return m_inner.advance(glyph, size); }
    float kerning(std::uint32_t left, std::uint32_t right, float size) const override
    {
        return m_inner.kerning(left, right, size);
    }
    void draw_glyph(Bitmap& target, std::uint32_t glyph, float x, float baseline_y, float size, Color color,
        bool bold, bool italic) const override
    {
        m_inner.draw_glyph(target, glyph, x, baseline_y, size, color, bold, italic);
    }

private:
    Face const& m_inner;
    std::vector<std::pair<char32_t, char32_t>> m_ranges;
};

} // namespace

bool FontStack::Waiting::covers(char32_t code_point) const
{
    if (ranges.empty())
        return true;
    for (auto const& [first, last] : ranges) {
        if (code_point >= first && code_point <= last)
            return true;
    }
    return false;
}

// The waiting faces a code point gets as far as — the ones that keep it in
// their range, ahead of the first face with a glyph for it — are asked
// for. A face that comes changes the stack, and the walk starts again.
void FontStack::ask_for(char32_t code_point) const
{
    for (;;) {
        std::size_t answering = m_faces.size();
        for (std::size_t i = 0; i < m_faces.size(); ++i) {
            if (m_faces[i]->glyph_index(code_point) != 0) {
                answering = i;
                break;
            }
        }
        bool changed = false;
        for (std::size_t i = 0; i < m_waiting.size() && !changed; ++i) {
            Waiting const& waiting = m_waiting[i];
            if (waiting.asked || waiting.before > answering || !waiting.covers(code_point))
                continue;
            changed = m_manager->ask(*this, i);
        }
        if (!changed)
            return;
    }
}

// The same for the first available font: a waiting face that would be it
// is asked for by whoever reads the line's metrics, text or no text.
void FontStack::ask_primary() const
{
    for (;;) {
        std::size_t primary = m_faces.size();
        for (std::size_t i = 0; i < m_faces.size(); ++i) {
            if (m_faces[i] == m_primary) {
                primary = i;
                break;
            }
        }
        bool changed = false;
        for (std::size_t i = 0; i < m_waiting.size() && !changed; ++i) {
            Waiting const& waiting = m_waiting[i];
            if (waiting.asked || waiting.before > primary || !waiting.covers(U' '))
                continue;
            changed = m_manager->ask(*this, i);
        }
        if (!changed)
            return;
    }
}

Face const& FontStack::face_for(char32_t code_point) const
{
    // A character drawn as a picture by default — a smiling face, a flag
    // — goes to a colour face when the machine has one for it, before any
    // face on the stack with a black-and-white glyph for it, as Firefox
    // and Chrome give it.
    if (m_manager && m_manager->system_fonts() && is_emoji_presentation(code_point)) {
        if (Face const* face = m_manager->color_face_for(code_point))
            return *face;
    }
    if (m_unasked != 0) [[unlikely]]
        ask_for(code_point);
    for (Face const* face : m_faces) {
        if (face->glyph_index(code_point) != 0)
            return *face;
    }
    if (m_manager && m_manager->system_fonts()) {
        if (Face const* face = m_manager->fallback_for(code_point))
            return *face;
    }
    return builtin_face();
}

FontStack::Glyph FontStack::glyph_for(char32_t code_point) const
{
    Face const& face = face_for(code_point);
    std::uint32_t const glyph = face.glyph_index(code_point);
    if (glyph != 0)
        return Glyph { &face, glyph };
    return Glyph { &builtin_face(), code_point };
}

float FontStack::measure(std::u32string_view text, float size, bool kern) const
{
    // The built-in face alone is fixed pitch: count times advance, which is
    // exact where a running sum would drift.
    if (builtin_alone())
        return static_cast<float>(text.size()) * m_faces[0]->advance(0, size);
    bool const cacheable = text.size() <= widths_run_limit;
    std::uint64_t const by = std::uint64_t { std::bit_cast<std::uint32_t>(size) } << 1 | (kern ? 1u : 0u);
    if (cacheable) {
        if (auto const runs = m_widths.find(by); runs != m_widths.end()) {
            if (auto const found = runs->second.find(text); found != runs->second.end())
                return found->second;
        }
    }
    // A face the page had at hand can come while the run is measured (a
    // glyph asked for it): the stack is filled again then, its widths let
    // go of, and this width, made of both sets of faces, is not kept.
    std::uint64_t const faces = m_faces_filled;
    float width = 0;
    Glyph previous { nullptr, 0 };
    for (char32_t const c : text) {
        Glyph const glyph = glyph_for(c);
        if (kern && previous.face == glyph.face)
            width += glyph.face->kerning(previous.glyph, glyph.glyph, size);
        width += glyph.face->advance(glyph.glyph, size);
        previous = glyph;
    }
    if (cacheable && faces == m_faces_filled) {
        if (m_widths_count >= widths_bound) {
            m_widths.clear();
            m_widths_count = 0;
        }
        m_widths[by].emplace(std::u32string(text), width);
        ++m_widths_count;
    }
    return width;
}

FontManager& FontManager::instance()
{
    static FontManager manager;
    return manager;
}

void FontManager::add_font_file(std::string const& path)
{
    std::lock_guard<std::recursive_mutex> const lock(m_mutex);
    // Kept in a list of its own as well as the catalogue: a face handed over
    // by name is installed as far as this manager is concerned, and answers
    // for its family whether or not the machine's own fonts are in play. The
    // machine is not scanned on its account — a caller that turned the
    // system fonts off wants exactly the faces it named.
    for (FaceInfo& info : TrueTypeFont::scan_file(path)) {
        if (!info.has_outlines)
            continue;
        std::string const family = lowercased(info.family);
        m_added_by_family[family].push_back(m_catalogue.size());
        m_by_family[family].push_back(m_catalogue.size());
        m_catalogue.push_back(std::move(info));
    }
    ++m_generation;
    m_fallbacks.clear();
    m_color_fallbacks.clear();
}

void FontManager::set_system_fonts(bool enabled)
{
    std::lock_guard<std::recursive_mutex> const lock(m_mutex);
    if (m_system_fonts == enabled)
        return;
    m_system_fonts = enabled;
    ++m_generation;
    m_fallbacks.clear();
    m_color_fallbacks.clear();
}

// A stack, once handed out, is referenced by every text run laid out with
// it; when the answers change, the old stacks are set aside rather than
// destroyed, so a layout that still holds one paints and measures as before.
void FontManager::retire_stacks(ThreadFonts& fonts)
{
    for (auto& [key, stack] : fonts.stacks)
        m_retired_stacks.push_back(std::move(stack));
    fonts.stacks.clear();
    ++fonts.serial;
}

FontManager::ThreadFonts& FontManager::mine() const
{
    std::unique_ptr<ThreadFonts>& fonts = m_threads[std::this_thread::get_id()];
    if (!fonts)
        fonts = std::make_unique<ThreadFonts>();
    return *fonts;
}

bool FontManager::same_page_faces(std::vector<PageFace> const& left, std::vector<PageFace> const& right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].face != right[i].face || left[i].wait != right[i].wait || left[i].family_lower != right[i].family_lower
            || left[i].weight != right[i].weight || left[i].italic != right[i].italic || left[i].stretch != right[i].stretch
            || left[i].weight_max != right[i].weight_max || left[i].stretch_max != right[i].stretch_max)
            return false;
    }
    return true;
}

void FontManager::settle_page_faces(std::vector<PageFace>& faces)
{
    std::erase_if(faces, [](PageFace& entry) {
        if (!entry.wait || !entry.wait->settled)
            return false;
        if (!entry.wait->face)
            return true;
        entry.face = entry.wait->face;
        entry.wait = nullptr;
        entry.ranges.clear();
        return false;
    });
}

void FontManager::restore_page_faces(std::vector<PageFace> faces)
{
    std::lock_guard<std::recursive_mutex> const lock(m_mutex);
    ThreadFonts& fonts = mine();
    // A font that came since the set was taken is in it from here on.
    settle_page_faces(faces);
    if (same_page_faces(faces, fonts.page_faces))
        return;
    fonts.page_faces = std::move(faces);
    retire_stacks(fonts);
}

std::size_t FontManager::page_font_count() const
{
    std::lock_guard<std::recursive_mutex> const lock(m_mutex);
    std::size_t here = 0;
    for (PageFace const& entry : mine().page_faces)
        here += entry.face ? 1 : 0;
    return here;
}

std::size_t FontManager::page_fonts_waiting() const
{
    std::lock_guard<std::recursive_mutex> const lock(m_mutex);
    std::size_t waiting = 0;
    for (PageFace const& entry : mine().page_faces)
        waiting += entry.wait ? 1 : 0;
    return waiting;
}

namespace {
std::atomic<std::size_t> g_font_bytes_hashed { 0 };
std::atomic<std::size_t> g_fonts_asked { 0 };
std::atomic<std::size_t> g_fonts_loaded { 0 };
std::atomic<std::size_t> g_fonts_coming { 0 };
std::atomic<std::size_t> g_fonts_failed { 0 };
}

bool page_fonts_on_demand()
{
    static bool const on_demand = [] {
        char const* const value = std::getenv("SASHFOLD_LAZY");
        return !(value && value[0] == '0' && value[1] == '\0');
    }();
    return on_demand;
}

PageFontCensus page_font_census()
{
    return PageFontCensus { g_fonts_asked.load(std::memory_order_relaxed), g_fonts_loaded.load(std::memory_order_relaxed),
        g_fonts_coming.load(std::memory_order_relaxed), g_fonts_failed.load(std::memory_order_relaxed) };
}

std::uint64_t font_bytes_hash(std::vector<std::uint8_t> const& bytes)
{
    g_font_bytes_hashed.fetch_add(1, std::memory_order_relaxed);
    return fnv1a(bytes);
}

std::size_t font_bytes_hashed()
{
    return g_font_bytes_hashed.load(std::memory_order_relaxed);
}

// The face a font's bytes parse to, under the family and descriptors it
// was declared with: parsed once, and kept for as long as the manager
// lives. Null for bytes this engine draws nothing from.
Face const* FontManager::page_face_of(PageFace const& described, std::vector<std::uint8_t> const& bytes,
    std::uint64_t bytes_hash)
{
    std::string key = described.family_lower;
    key += '\n';
    key += std::to_string(described.weight);
    key += described.italic ? 'i' : 'n';
    key += '\n';
    key += std::to_string(described.stretch);
    key += '-';
    key += std::to_string(described.stretch_max);
    key += '/';
    key += std::to_string(described.weight_max);
    key += '\n';
    key += std::to_string(bytes.size());
    key += '\n';
    key += std::to_string(bytes_hash != 0 ? bytes_hash : fnv1a(bytes));
    auto it = m_page_face_cache.find(key);
    if (it == m_page_face_cache.end()) {
        std::unique_ptr<Face> face;
        if (std::optional<TrueTypeFont> parsed = TrueTypeFont::parse(bytes); parsed && parsed->has_outlines())
            face = make_truetype_face(std::move(*parsed));
        it = m_page_face_cache.emplace(std::move(key), std::move(face)).first;
    }
    return it->second.get();
}

void FontManager::set_page_fonts(std::vector<PageFont> const& fonts)
{
    std::lock_guard<std::recursive_mutex> const lock(m_mutex);
    std::vector<PageFace> faces;
    for (PageFont const& font : fonts) {
        PageFace described { lowercased(font.family), font.weight, font.italic, nullptr, font.stretch,
            std::max(font.weight, font.weight_max), std::max(font.stretch, font.stretch_max) };
        if (font.waiting) {
            // Nothing fetched: its place is taken by what it waits on, or,
            // where the page has been asked since, by what came of that.
            if (font.waiting->settled) {
                if (!font.waiting->face)
                    continue;
                described.face = font.waiting->face;
            } else {
                described.wait = font.waiting;
                described.ranges = font.unicode_ranges;
            }
            faces.push_back(std::move(described));
            continue;
        }
        Face const* face = page_face_of(described, font.bytes, font.bytes_hash);
        if (!face)
            continue; // not a font this engine draws: the family falls through to the next
        if (!font.unicode_ranges.empty())
            face = ranged_face(face, font.unicode_ranges);
        described.face = face;
        faces.push_back(std::move(described));
    }
    ThreadFonts& mine_now = mine();
    if (same_page_faces(faces, mine_now.page_faces))
        return; // the same fonts as the last page: every stack still answers right
    mine_now.page_faces = std::move(faces);
    retire_stacks(mine_now);
}

// The page's own face for a family, chosen the way best_face chooses: the
// requested stretch and slant first, then the nearest weight; every face
// that matches equally well — a family split into unicode-range pieces —
// answers together, in the order declared. A face not fetched yet is
// matched by what it was declared as, like any other.
std::vector<FontManager::PageFace const*> FontManager::page_faces(std::string const& family_lower, int weight, int stretch,
    bool italic) const
{
    std::vector<PageFace const*> best;
    long best_score = -1;
    for (PageFace const& candidate : mine().page_faces) {
        if (candidate.family_lower != family_lower)
            continue;
        // A face answering a range stands at the point of it nearest the
        // request (css-fonts-4 §5.2.1).
        int const face_stretch = std::clamp(stretch, candidate.stretch, candidate.stretch_max);
        int const face_weight = std::clamp(weight, candidate.weight, candidate.weight_max);
        long const score = match_distance(face_stretch, face_weight, candidate.italic, stretch, weight, italic);
        if (best_score < 0 || score < best_score) {
            best_score = score;
            best.clear();
        }
        if (score == best_score)
            best.push_back(&candidate);
    }
    return best;
}

Face const* FontManager::ranged_face(Face const* face, std::vector<std::pair<char32_t, char32_t>> const& ranges)
{
    std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(face));
    for (auto const& [first, last] : ranges) {
        key += ' ';
        key += std::to_string(first);
        key += '-';
        key += std::to_string(last);
    }
    auto it = m_ranged_faces.find(key);
    if (it == m_ranged_faces.end())
        it = m_ranged_faces.emplace(std::move(key), std::make_unique<RangedFace>(*face, ranges)).first;
    return it->second.get();
}

std::vector<FaceInfo> const& FontManager::catalogue()
{
    std::lock_guard<std::recursive_mutex> const lock(m_mutex);
    scan();
    return m_catalogue;
}

void FontManager::scan()
{
    if (m_scanned)
        return;
    m_scanned = true;
    constexpr std::size_t file_budget = 4096; // a pathological directory tree stops here
    std::size_t files = 0;
    for (std::string const& directory : platform::system_font_directories()) {
        std::error_code error;
        std::filesystem::path const root(directory);
        if (!std::filesystem::is_directory(root, error))
            continue;
        std::filesystem::recursive_directory_iterator it(root,
            std::filesystem::directory_options::skip_permission_denied, error);
        std::filesystem::recursive_directory_iterator const end;
        while (!error && it != end && files < file_budget) {
            std::filesystem::directory_entry const& entry = *it;
            if (entry.is_regular_file(error) && has_font_extension(entry.path())) {
                ++files;
                for (FaceInfo& info : TrueTypeFont::scan_file(entry.path().string())) {
                    if (!info.has_outlines)
                        continue; // CFF outlines are declined: the face cannot draw
                    m_by_family[lowercased(info.family)].push_back(m_catalogue.size());
                    m_catalogue.push_back(std::move(info));
                }
            }
            it.increment(error);
        }
    }
}

Face const* FontManager::load(std::size_t index)
{
    if (auto const it = m_loaded.find(index); it != m_loaded.end())
        return it->second.get();
    FaceInfo const& info = m_catalogue[index];
    std::unique_ptr<Face> face;
    std::ifstream file(info.path, std::ios::binary);
    if (file) {
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        if (std::optional<TrueTypeFont> font = TrueTypeFont::parse(std::move(bytes), info.face_index))
            face = make_truetype_face(std::move(*font));
    }
    auto const [it, inserted] = m_loaded.emplace(index, std::move(face));
    (void)inserted;
    return it->second.get();
}

// CSS font matching (css-fonts-4 §5.2): the nearest stretch, then a face
// of the requested slant, then the nearest weight — below the request for
// normal text, above it for bold.
Face const* FontManager::best_of(std::vector<std::size_t> const& indices, int weight, int stretch, bool italic)
{
    if (indices.empty())
        return nullptr;
    std::size_t best = 0;
    long best_score = -1;
    for (std::size_t const index : indices) {
        FaceInfo const& info = m_catalogue[index];
        long const score
            = match_distance(info.stretch, static_cast<int>(info.weight_class), info.italic, stretch, weight, italic);
        if (best_score < 0 || score < best_score) {
            best_score = score;
            best = index;
        }
    }
    return load(best);
}

Face const* FontManager::best_face(std::string const& family_lower, int weight, int stretch, bool italic)
{
    auto const it = m_by_family.find(family_lower);
    return it == m_by_family.end() ? nullptr : best_of(it->second, weight, stretch, italic);
}

// The same, over the faces handed to the manager by name rather than found
// on the machine.
Face const* FontManager::added_face(std::string const& family_lower, int weight, int stretch, bool italic)
{
    auto const it = m_added_by_family.find(family_lower);
    return it == m_added_by_family.end() ? nullptr : best_of(it->second, weight, stretch, italic);
}

FontStack const& FontManager::resolve(FontRequest const& request)
{
    std::lock_guard<std::recursive_mutex> const lock(m_mutex);
    std::string key = request.italic ? "i" : "n";
    key += std::to_string(request.weight);
    key += 's';
    key += std::to_string(request.stretch);
    for (std::string const& family : request.families) {
        key += '\n';
        key += family;
    }
    if (!request.page_fonts)
        key += "\n\tthe machine's alone";
    ThreadFonts& fonts = mine();
    if (fonts.made_for != m_generation) {
        // The machine's faces are not what this thread's stacks were
        // resolved against: they are set aside, and asked for afresh.
        retire_stacks(fonts);
        fonts.made_for = m_generation;
    }
    if (auto const it = fonts.stacks.find(key); it != fonts.stacks.end())
        return *it->second;

    auto stack = std::make_unique<FontStack>();
    stack->m_manager = this;
    stack->m_request = request;
    fill(*stack, fonts);
    FontStack const& result = *stack;
    fonts.stacks.emplace(std::move(key), std::move(stack));
    return result;
}

// A stack's faces for the request it answers, against the calling thread's
// page fonts as they stand: what resolve() hands out, and what a stack is
// given again when one of its waiting faces has come.
void FontManager::fill(FontStack& filled, ThreadFonts& fonts)
{
    FontStack* const stack = &filled;
    FontRequest const& request = stack->m_request;
    stack->m_faces.clear();
    stack->m_waiting.clear();
    stack->m_widths.clear(); // measured with the faces it had
    stack->m_widths_count = 0;
    ++stack->m_faces_filled;
    auto const add = [&](Face const* face) {
        if (face && std::find(stack->m_faces.begin(), stack->m_faces.end(), face) == stack->m_faces.end())
            stack->m_faces.push_back(face);
    };
    auto const add_family = [&](std::string const& name) {
        std::string const lower = lowercased(name);
        // A page's own font shadows an installed one of the same name —
        // for whoever asks on a page's behalf — and does so from the
        // moment it is declared: while it waits, the text goes on to the
        // next family, not to the machine's font of that name.
        std::vector<PageFace const*> const page = request.page_fonts
            ? page_faces(lower, request.weight, request.stretch, request.italic)
            : std::vector<PageFace const*> {};
        if (!page.empty()) {
            for (PageFace const* entry : page) {
                if (entry->face)
                    add(entry->face);
                else
                    stack->m_waiting.push_back(
                        FontStack::Waiting { stack->m_faces.size(), entry->wait, entry->ranges, entry->wait->coming });
            }
            return;
        }
        // A face handed over by name answers next, with the machine's fonts
        // on or off: a test suite brings its own measuring sticks.
        if (Face const* face = added_face(lower, request.weight, request.stretch, request.italic)) {
            add(face);
            return;
        }
        if (!m_system_fonts)
            return;
        scan();
        std::vector<std::string_view> const generics = generic_candidates(lower);
        if (generics.empty()) {
            add(best_face(lower, request.weight, request.stretch, request.italic));
            return;
        }
        for (std::string_view const candidate : generics) {
            if (Face const* face = best_face(lowercased(candidate), request.weight, request.stretch, request.italic)) {
                add(face);
                return;
            }
        }
    };
    for (std::string const& family : request.families)
        add_family(family);
    if (m_system_fonts && stack->m_faces.empty())
        add_family("serif"); // the initial value, when nothing asked for exists
    stack->m_faces.push_back(&builtin_face());
    // The first available font (css-fonts-4 §2.1): the first whose
    // unicode-range has the space in it — a face kept to a range without
    // one is not, whatever glyphs it has.
    stack->m_primary = stack->m_faces.back();
    std::size_t primary = stack->m_faces.size() - 1;
    for (std::size_t i = 0; i < stack->m_faces.size(); ++i) {
        if (stack->m_faces[i]->covers(U' ')) {
            stack->m_primary = stack->m_faces[i];
            primary = i;
            break;
        }
    }
    stack->m_owner = &fonts;
    stack->m_serial = fonts.serial;
    stack->m_unasked = 0;
    stack->m_asks_primary = false;
    for (FontStack::Waiting const& waiting : stack->m_waiting) {
        if (waiting.asked)
            continue;
        ++stack->m_unasked;
        if (waiting.before <= primary && waiting.covers(U' '))
            stack->m_asks_primary = true;
    }
}

bool FontManager::ask(FontStack const& asking, std::size_t index)
{
    FontStack& stack = const_cast<FontStack&>(asking);
    // One waiting face of the stack is not asked for twice: whatever comes
    // of this, the stack's note of it is that it has been.
    auto const note_asked = [&stack](std::shared_ptr<PageFontWait> const& wait) {
        std::size_t primary = stack.m_faces.size();
        for (std::size_t i = 0; i < stack.m_faces.size(); ++i) {
            if (stack.m_faces[i] == stack.m_primary) {
                primary = i;
                break;
            }
        }
        stack.m_unasked = 0;
        stack.m_asks_primary = false;
        for (FontStack::Waiting& waiting : stack.m_waiting) {
            if (waiting.wait == wait)
                waiting.asked = true;
            if (waiting.asked)
                continue;
            ++stack.m_unasked;
            if (waiting.before <= primary && waiting.covers(U' '))
                stack.m_asks_primary = true;
        }
    };
    std::shared_ptr<PageFontWait> wait;
    std::shared_ptr<PageFontLoader> loader;
    {
        std::lock_guard<std::recursive_mutex> const lock(m_mutex);
        if (index >= stack.m_waiting.size())
            return false;
        wait = stack.m_waiting[index].wait;
        ThreadFonts& fonts = mine();
        // A stack set aside answers the layout that holds it as it did
        // when that layout was made; a face on its way is waited for; and
        // a page with no way to fetch leaves the face waiting.
        bool const current = stack.m_owner == &fonts && stack.m_serial == fonts.serial;
        if (!current || wait->coming || (!wait->settled && !(wait->loader && *wait->loader))) {
            note_asked(wait);
            return false;
        }
        loader = wait->loader;
    }
    if (!wait->settled) {
        // The page is asked with the lock let go: its answer may take a
        // fetch, and the other threads' text is not held for it.
        g_fonts_asked.fetch_add(1, std::memory_order_relaxed);
        PageFontLoad load = (*loader)(*wait);
        std::lock_guard<std::recursive_mutex> const lock(m_mutex);
        ThreadFonts& fonts = mine();
        if (load.outcome == PageFontLoad::Outcome::Coming) {
            g_fonts_coming.fetch_add(1, std::memory_order_relaxed);
            wait->coming = true;
            note_asked(wait);
            return false;
        }
        Face const* face = nullptr;
        if (load.outcome == PageFontLoad::Outcome::Loaded) {
            for (PageFace const& entry : fonts.page_faces) {
                if (entry.wait != wait)
                    continue;
                face = page_face_of(entry, load.bytes, load.bytes_hash);
                if (face && !entry.ranges.empty())
                    face = ranged_face(face, entry.ranges);
                break;
            }
        }
        (face ? g_fonts_loaded : g_fonts_failed).fetch_add(1, std::memory_order_relaxed);
        wait->face = face;
        wait->settled = true;
    }
    // The font has come, or cannot: the thread's set and every stack of it
    // that had the face waiting are what they would have been resolved to
    // with the answer known.
    std::lock_guard<std::recursive_mutex> const lock(m_mutex);
    ThreadFonts& fonts = mine();
    settle_page_faces(fonts.page_faces);
    for (auto& [key, kept] : fonts.stacks) {
        bool had = false;
        for (FontStack::Waiting const& waiting : kept->m_waiting)
            had = had || waiting.wait->settled;
        if (had)
            fill(*kept, fonts);
    }
    if (stack.m_owner != &fonts || stack.m_serial != fonts.serial) {
        note_asked(wait);
        return false;
    }
    return true;
}

// A face is asked only when its OS/2 ranges claim the code point's block
// (or say nothing at all), so a CJK character loads the CJK fonts and not
// every symbol font on the way. Faces already loaded are free to ask.
Face const* FontManager::fallback_for(char32_t code_point)
{
    std::lock_guard<std::recursive_mutex> const lock(m_mutex);
    if (auto const it = m_fallbacks.find(code_point); it != m_fallbacks.end())
        return it->second;
    scan();
    Face const* found = nullptr;
    auto const try_index = [&](std::size_t index) {
        if (found)
            return;
        FaceInfo const& info = m_catalogue[index];
        if (info.italic || info.weight_class > 500 || !(info.claims(code_point) || info.claims_nothing()))
            return;
        if (Face const* face = load(index); face && face->glyph_index(code_point) != 0)
            found = face;
    };
    // What is drawn as a picture by default goes to a colour font first: a
    // text face with a black-and-white smiling face would otherwise win by
    // being loaded already.
    if (is_emoji_presentation(code_point))
        found = color_face_for(code_point);
    for (auto const& [index, face] : m_loaded) {
        if (found)
            break;
        if (face && face->glyph_index(code_point) != 0)
            found = face.get();
    }
    for (std::string_view const family : fallback_families) {
        auto const it = m_by_family.find(lowercased(family));
        if (it == m_by_family.end())
            continue;
        for (std::size_t const index : it->second)
            try_index(index);
    }
    // Then anything on the machine that claims the block outright.
    for (std::size_t index = 0; index < m_catalogue.size() && !found; ++index) {
        if (m_catalogue[index].claims(code_point))
            try_index(index);
    }
    m_fallbacks.emplace(code_point, found);
    return found;
}

Face const* FontManager::color_face_for(char32_t code_point)
{
    std::lock_guard<std::recursive_mutex> const lock(m_mutex);
    if (auto const it = m_color_fallbacks.find(code_point); it != m_color_fallbacks.end())
        return it->second;
    scan();
    Face const* found = nullptr;
    for (std::size_t index = 0; index < m_catalogue.size() && !found; ++index) {
        FaceInfo const& info = m_catalogue[index];
        if (!info.color || info.italic || info.weight_class > 500)
            continue;
        if (Face const* face = load(index); face && face->glyph_index(code_point) != 0)
            found = face;
    }
    m_color_fallbacks.emplace(code_point, found);
    return found;
}

}
