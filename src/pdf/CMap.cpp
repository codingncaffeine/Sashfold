#include "pdf/CMap.h"

#include "pdf/Parser.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>

namespace sashfold::pdf {

namespace {

constexpr int k_max_depth = 4;
constexpr std::size_t k_max_entries = 4'000'000;

std::uint32_t code_of(std::string const& bytes)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < bytes.size() && i < 4; ++i)
        value = (value << 8) | static_cast<std::uint8_t>(bytes[i]);
    return value;
}

// The folders the system keeps CMap resources in, as poppler and Ghostscript look.
std::vector<std::filesystem::path> cmap_folders()
{
    std::vector<std::filesystem::path> folders;
    for (char const* base : { "/usr/share/poppler/cMap", "/usr/local/share/poppler/cMap", "/usr/share/ghostscript/Resource/CMap",
             "/usr/share/fonts/cmap", "/opt/homebrew/share/poppler/cMap", "/usr/local/opt/poppler-data/share/poppler/cMap" }) {
        std::error_code error;
        if (std::filesystem::is_directory(base, error))
            folders.emplace_back(base);
    }
    return folders;
}

std::optional<std::vector<std::uint8_t>> read_file(std::filesystem::path const& path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || std::filesystem::file_size(path, error) > 64u * 1024u * 1024u)
        return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool safe_name(std::string const& name)
{
    if (name.empty() || name.size() > 64)
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '+'; });
}

} // namespace

std::u32string utf16be_to_u32(std::string_view bytes)
{
    std::u32string out;
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        char32_t unit = (static_cast<char32_t>(static_cast<std::uint8_t>(bytes[i])) << 8) | static_cast<std::uint8_t>(bytes[i + 1]);
        if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < bytes.size()) {
            char32_t const low = (static_cast<char32_t>(static_cast<std::uint8_t>(bytes[i + 2])) << 8) | static_cast<std::uint8_t>(bytes[i + 3]);
            if (low >= 0xDC00 && low < 0xE000) {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        out += unit;
    }
    if (bytes.size() == 1)
        out += static_cast<char32_t>(static_cast<std::uint8_t>(bytes[0]));
    return out;
}

std::shared_ptr<CMap const> CMap::identity(bool vertical)
{
    static std::shared_ptr<CMap const> const horizontal = [] {
        auto map = std::make_shared<CMap>();
        map->m_identity = true;
        map->m_name = "Identity-H";
        map->m_codespaces.push_back({ 2, 0, 0xFFFF });
        return map;
    }();
    static std::shared_ptr<CMap const> const upright = [] {
        auto map = std::make_shared<CMap>();
        map->m_identity = true;
        map->m_vertical = true;
        map->m_name = "Identity-V";
        map->m_codespaces.push_back({ 2, 0, 0xFFFF });
        return map;
    }();
    return vertical ? upright : horizontal;
}

std::shared_ptr<CMap const> CMap::predefined(std::string const& name)
{
    if (name == "Identity-H")
        return identity(false);
    if (name == "Identity-V")
        return identity(true);
    if (!safe_name(name))
        return nullptr;
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<CMap const>> cache;
    {
        std::lock_guard const lock(mutex);
        auto const it = cache.find(name);
        if (it != cache.end())
            return it->second;
    }
    std::shared_ptr<CMap const> found;
    for (std::filesystem::path const& folder : cmap_folders()) {
        std::optional<std::vector<std::uint8_t>> bytes = read_file(folder / name);
        if (!bytes) {
            // poppler-data also keeps them by collection.
            std::error_code error;
            for (auto const& entry : std::filesystem::directory_iterator(folder, error)) {
                if (entry.is_directory(error)) {
                    bytes = read_file(entry.path() / name);
                    if (bytes)
                        break;
                }
            }
        }
        if (bytes) {
            found = parse(*bytes);
            break;
        }
    }
    std::lock_guard const lock(mutex);
    cache.emplace(name, found);
    return found;
}

void CMap::merge(CMap const& base)
{
    if (m_codespaces.empty())
        m_codespaces = base.m_codespaces;
    m_ranges.insert(m_ranges.begin(), base.m_ranges.begin(), base.m_ranges.end());
    for (auto const& [key, value] : base.m_chars)
        m_chars.emplace(key, value);
    for (auto const& [key, value] : base.m_unicode)
        m_unicode.emplace(key, value);
    m_unicode_ranges.insert(m_unicode_ranges.begin(), base.m_unicode_ranges.begin(), base.m_unicode_ranges.end());
    if (base.m_identity && m_ranges.empty() && m_chars.empty())
        m_identity = true;
    m_vertical = m_vertical || base.m_vertical;
}

std::shared_ptr<CMap const> CMap::parse(std::vector<std::uint8_t> const& data, int depth)
{
    if (depth > k_max_depth)
        return nullptr;
    auto map = std::make_shared<CMap>();
    Lexer lexer(std::span<std::uint8_t const>(data.data(), data.size()));
    std::vector<Token> operands;
    std::size_t entries = 0;
    for (;;) {
        Token token = lexer.next();
        if (token.kind == Token::Kind::Eof)
            break;
        if (token.kind != Token::Kind::Keyword) {
            if (operands.size() < 64)
                operands.push_back(std::move(token));
            continue;
        }
        std::string const& op = token.text;
        if (op == "usecmap" && !operands.empty() && operands.back().kind == Token::Kind::Name) {
            if (std::shared_ptr<CMap const> const base = predefined(operands.back().text))
                map->merge(*base);
        } else if (op == "def" && operands.size() >= 2 && operands[operands.size() - 2].kind == Token::Kind::Name) {
            Token const& key = operands[operands.size() - 2];
            Token const& value = operands.back();
            if (key.text == "WMode" && value.kind == Token::Kind::Integer)
                map->m_vertical = value.integer == 1;
            else if (key.text == "CMapName" && value.kind == Token::Kind::Name)
                map->m_name = value.text;
        } else if (op == "begincodespacerange") {
            for (int guard = 0; guard < 1024; ++guard) {
                Token lo = lexer.next();
                if (lo.kind == Token::Kind::Keyword || lo.kind == Token::Kind::Eof)
                    break;
                Token hi = lexer.next();
                if (lo.text.empty() || lo.text.size() > 4 || hi.text.size() != lo.text.size())
                    continue;
                map->m_codespaces.push_back({ static_cast<int>(lo.text.size()), code_of(lo.text), code_of(hi.text) });
            }
        } else if (op == "begincidrange" || op == "beginnotdefrange") {
            bool const notdef = op == "beginnotdefrange";
            for (; entries < k_max_entries; ++entries) {
                Token lo = lexer.next();
                if (lo.kind == Token::Kind::Keyword || lo.kind == Token::Kind::Eof)
                    break;
                Token hi = lexer.next();
                Token cid = lexer.next();
                if (lo.text.empty() || lo.text.size() > 4 || cid.kind != Token::Kind::Integer)
                    continue;
                if (notdef)
                    continue; // unmapped codes are the notdef CID already
                map->m_ranges.push_back({ static_cast<int>(lo.text.size()), code_of(lo.text), code_of(hi.text), static_cast<std::uint32_t>(cid.integer) });
            }
        } else if (op == "begincidchar") {
            for (; entries < k_max_entries; ++entries) {
                Token code = lexer.next();
                if (code.kind == Token::Kind::Keyword || code.kind == Token::Kind::Eof)
                    break;
                Token cid = lexer.next();
                if (code.text.empty() || code.text.size() > 4 || cid.kind != Token::Kind::Integer)
                    continue;
                map->m_chars[{ static_cast<int>(code.text.size()), code_of(code.text) }] = static_cast<std::uint32_t>(cid.integer);
            }
        } else if (op == "beginbfchar") {
            for (; entries < k_max_entries; ++entries) {
                Token code = lexer.next();
                if (code.kind == Token::Kind::Keyword || code.kind == Token::Kind::Eof)
                    break;
                Token dst = lexer.next();
                if (code.text.empty() || code.text.size() > 4)
                    continue;
                std::u32string text;
                if (dst.kind == Token::Kind::Name) {
                    // A glyph name destination (allowed by the standard).
                    for (char const c : dst.text)
                        text += static_cast<char32_t>(static_cast<std::uint8_t>(c));
                } else {
                    text = utf16be_to_u32(dst.text);
                }
                map->m_unicode[{ static_cast<int>(code.text.size()), code_of(code.text) }] = std::move(text);
            }
        } else if (op == "beginbfrange") {
            for (; entries < k_max_entries; ++entries) {
                Token lo = lexer.next();
                if (lo.kind == Token::Kind::Keyword || lo.kind == Token::Kind::Eof)
                    break;
                Token hi = lexer.next();
                Token dst = lexer.next();
                if (lo.text.empty() || lo.text.size() > 4 || hi.text.size() != lo.text.size())
                    continue;
                UnicodeRange range;
                range.length = static_cast<int>(lo.text.size());
                range.low = code_of(lo.text);
                range.high = code_of(hi.text);
                if (range.high < range.low || range.high - range.low > 65535)
                    continue;
                if (dst.kind == Token::Kind::ArrayOpen) {
                    // One destination per code.
                    for (int guard = 0; guard < 65536; ++guard) {
                        Token item = lexer.next();
                        if (item.kind == Token::Kind::Eof || item.kind == Token::Kind::ArrayClose)
                            break;
                        range.list.push_back(utf16be_to_u32(item.text));
                    }
                } else {
                    range.start = utf16be_to_u32(dst.text);
                }
                map->m_unicode_ranges.push_back(std::move(range));
            }
        } else if (op == "begincmap" || op == "endcmap" || op.rfind("end", 0) == 0) {
            // structure only
        }
        operands.clear();
    }
    return map;
}

bool CMap::in_codespace(std::uint32_t code, int length) const
{
    for (Codespace const& space : m_codespaces) {
        if (space.length != length)
            continue;
        // Byte by byte, as the standard compares.
        bool inside = true;
        for (int b = 0; b < length && inside; ++b) {
            int const shift = 8 * (length - 1 - b);
            std::uint32_t const v = (code >> shift) & 0xFF;
            inside = v >= ((space.low >> shift) & 0xFF) && v <= ((space.high >> shift) & 0xFF);
        }
        if (inside)
            return true;
    }
    return false;
}

bool CMap::next_code(std::string_view bytes, std::size_t& at, std::uint32_t& code, int& length) const
{
    if (at >= bytes.size())
        return false;
    if (m_codespaces.empty()) {
        // No codespace: two bytes per code, as Identity reads.
        length = std::min<int>(2, static_cast<int>(bytes.size() - at));
        code = 0;
        for (int i = 0; i < length; ++i)
            code = (code << 8) | static_cast<std::uint8_t>(bytes[at + static_cast<std::size_t>(i)]);
        at += static_cast<std::size_t>(length);
        return true;
    }
    std::uint32_t value = 0;
    for (int n = 1; n <= 4 && at + static_cast<std::size_t>(n) <= bytes.size(); ++n) {
        value = (value << 8) | static_cast<std::uint8_t>(bytes[at + static_cast<std::size_t>(n) - 1]);
        if (in_codespace(value, n)) {
            code = value;
            length = n;
            at += static_cast<std::size_t>(n);
            return true;
        }
    }
    // No range matches: the shortest codespace length's worth of bytes is one (notdef) code.
    int shortest = 4;
    for (Codespace const& space : m_codespaces)
        shortest = std::min(shortest, space.length);
    length = std::min<int>(shortest, static_cast<int>(bytes.size() - at));
    code = 0;
    for (int i = 0; i < length; ++i)
        code = (code << 8) | static_cast<std::uint8_t>(bytes[at + static_cast<std::size_t>(i)]);
    at += static_cast<std::size_t>(std::max(1, length));
    return true;
}

std::uint32_t CMap::cid(std::uint32_t code, int length) const
{
    if (m_identity && m_ranges.empty() && m_chars.empty())
        return code;
    auto const it = m_chars.find({ length, code });
    if (it != m_chars.end())
        return it->second;
    // The last range that holds the code wins (later definitions override).
    for (auto r = m_ranges.rbegin(); r != m_ranges.rend(); ++r) {
        if (r->length == length && code >= r->low && code <= r->high)
            return r->cid + (code - r->low);
    }
    return 0;
}

std::u32string CMap::unicode(std::uint32_t code, int length) const
{
    auto const it = m_unicode.find({ length, code });
    if (it != m_unicode.end())
        return it->second;
    for (auto r = m_unicode_ranges.rbegin(); r != m_unicode_ranges.rend(); ++r) {
        if (r->length != length || code < r->low || code > r->high)
            continue;
        std::uint32_t const offset = code - r->low;
        if (!r->list.empty())
            return offset < r->list.size() ? r->list[offset] : std::u32string {};
        std::u32string text = r->start;
        if (!text.empty())
            text.back() += offset;
        return text;
    }
    return {};
}

std::shared_ptr<std::vector<char32_t> const> cid_to_unicode_table(std::string const& collection)
{
    if (!safe_name(collection))
        return nullptr;
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<std::vector<char32_t> const>> cache;
    std::lock_guard const lock(mutex);
    auto const it = cache.find(collection);
    if (it != cache.end())
        return it->second;
    std::shared_ptr<std::vector<char32_t> const> table;
    for (char const* base : { "/usr/share/poppler/cidToUnicode", "/usr/local/share/poppler/cidToUnicode",
             "/opt/homebrew/share/poppler/cidToUnicode" }) {
        std::optional<std::vector<std::uint8_t>> const bytes = read_file(std::filesystem::path(base) / collection);
        if (!bytes)
            continue;
        // One hex value per line, the line number being the CID.
        auto values = std::make_shared<std::vector<char32_t>>();
        char32_t value = 0;
        bool have = false;
        for (std::uint8_t const c : *bytes) {
            if (c == '\n') {
                values->push_back(have ? value : 0);
                value = 0;
                have = false;
                continue;
            }
            int digit = -1;
            if (c >= '0' && c <= '9')
                digit = c - '0';
            else if (c >= 'a' && c <= 'f')
                digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                digit = c - 'A' + 10;
            if (digit >= 0) {
                value = value * 16 + static_cast<char32_t>(digit);
                have = true;
            }
        }
        table = values;
        break;
    }
    cache.emplace(collection, table);
    return table;
}

}
