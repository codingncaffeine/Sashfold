#include "text/FontManager.h"

#include "core/Ascii.h"
#include "platform/Fonts.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
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
// Latin and symbol coverage, then the CJK workhorses.
constexpr std::array<std::string_view, 22> fallback_families { "Segoe UI", "Arial Unicode MS", "Noto Sans",
    "DejaVu Sans", "Segoe UI Symbol", "Segoe UI Historic", "Arial", "Helvetica", "Times New Roman",
    "Lucida Sans Unicode", "Apple Symbols", "Yu Gothic", "Meiryo", "MS Gothic", "Microsoft YaHei",
    "Microsoft JhengHei", "Malgun Gothic", "Hiragino Sans", "PingFang SC", "Noto Sans CJK JP",
    "Noto Sans CJK SC", "Segoe UI Emoji" };

bool has_font_extension(std::filesystem::path const& path)
{
    std::string const extension = lowercased(path.extension().string());
    return extension == ".ttf" || extension == ".ttc" || extension == ".otf";
}

} // namespace

Face const& FontStack::face_for(char32_t code_point) const
{
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

float FontStack::measure(std::u32string_view text, float size) const
{
    // The built-in face alone is fixed pitch: count times advance, which is
    // exact where a running sum would drift.
    if (m_faces.size() == 1)
        return static_cast<float>(text.size()) * m_faces[0]->advance(0, size);
    float width = 0;
    for (char32_t const c : text) {
        Glyph const glyph = glyph_for(c);
        width += glyph.face->advance(glyph.glyph, size);
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
    scan();
    for (FaceInfo& info : TrueTypeFont::scan_file(path)) {
        if (!info.has_outlines)
            continue;
        m_by_family[lowercased(info.family)].push_back(m_catalogue.size());
        m_catalogue.push_back(std::move(info));
    }
    m_stacks.clear();
    m_fallbacks.clear();
}

void FontManager::set_system_fonts(bool enabled)
{
    if (m_system_fonts == enabled)
        return;
    m_system_fonts = enabled;
    m_stacks.clear();
    m_fallbacks.clear();
}

std::vector<FaceInfo> const& FontManager::catalogue()
{
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

// CSS font matching, kept to weight and slant: a face of the requested
// slant beats one of the other, then the nearest weight — below the
// request for normal text, above it for bold.
Face const* FontManager::best_face(std::string const& family_lower, int weight, bool italic)
{
    auto const it = m_by_family.find(family_lower);
    if (it == m_by_family.end())
        return nullptr;
    std::size_t best = 0;
    long best_score = -1;
    for (std::size_t const index : it->second) {
        FaceInfo const& info = m_catalogue[index];
        long score = info.italic != italic ? 100000 : 0;
        int const distance = std::abs(static_cast<int>(info.weight_class) - weight);
        bool const wrong_side = weight <= 500 ? info.weight_class > weight : info.weight_class < weight;
        score += distance * 2 + (wrong_side ? 1 : 0);
        if (best_score < 0 || score < best_score) {
            best_score = score;
            best = index;
        }
    }
    return load(best);
}

FontStack const& FontManager::resolve(FontRequest const& request)
{
    std::string key = request.italic ? "i" : "n";
    key += std::to_string(request.weight);
    for (std::string const& family : request.families) {
        key += '\n';
        key += family;
    }
    if (auto const it = m_stacks.find(key); it != m_stacks.end())
        return *it->second;

    auto stack = std::make_unique<FontStack>();
    stack->m_manager = this;
    if (m_system_fonts) {
        scan();
        auto const add = [&](Face const* face) {
            if (face && std::find(stack->m_faces.begin(), stack->m_faces.end(), face) == stack->m_faces.end())
                stack->m_faces.push_back(face);
        };
        auto const add_family = [&](std::string const& name) {
            std::string const lower = lowercased(name);
            std::vector<std::string_view> const generics = generic_candidates(lower);
            if (generics.empty()) {
                add(best_face(lower, request.weight, request.italic));
                return;
            }
            for (std::string_view const candidate : generics) {
                if (Face const* face = best_face(lowercased(candidate), request.weight, request.italic)) {
                    add(face);
                    return;
                }
            }
        };
        for (std::string const& family : request.families)
            add_family(family);
        if (stack->m_faces.empty())
            add_family("serif"); // the initial value, when nothing asked for exists
    }
    stack->m_faces.push_back(&builtin_face());
    FontStack const& result = *stack;
    m_stacks.emplace(std::move(key), std::move(stack));
    return result;
}

// A face is asked only when its OS/2 ranges claim the code point's block
// (or say nothing at all), so a CJK character loads the CJK fonts and not
// every symbol font on the way. Faces already loaded are free to ask.
Face const* FontManager::fallback_for(char32_t code_point)
{
    if (auto const it = m_fallbacks.find(code_point); it != m_fallbacks.end())
        return it->second;
    scan();
    Face const* found = nullptr;
    for (auto const& [index, face] : m_loaded) {
        if (face && face->glyph_index(code_point) != 0) {
            found = face.get();
            break;
        }
    }
    auto const try_index = [&](std::size_t index) {
        if (found)
            return;
        FaceInfo const& info = m_catalogue[index];
        if (info.italic || info.weight_class > 500 || !(info.claims(code_point) || info.claims_nothing()))
            return;
        if (Face const* face = load(index); face && face->glyph_index(code_point) != 0)
            found = face;
    };
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

}
