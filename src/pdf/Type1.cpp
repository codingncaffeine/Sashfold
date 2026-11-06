#include "pdf/Type1.h"

#include "pdf/Encoding.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace sashfold::pdf {

namespace {

constexpr std::size_t k_max_glyphs = 65536;
constexpr int k_max_depth = 10;

bool is_hex(std::uint8_t c)
{
    return std::isxdigit(c) != 0;
}

bool is_space(std::uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0;
}

std::size_t find(std::vector<std::uint8_t> const& data, std::string_view needle, std::size_t from = 0, std::size_t to = SIZE_MAX)
{
    std::size_t const end = std::min(to, data.size());
    if (needle.size() > end)
        return SIZE_MAX;
    for (std::size_t i = from; i + needle.size() <= end; ++i) {
        if (std::memcmp(data.data() + i, needle.data(), needle.size()) == 0)
            return i;
    }
    return SIZE_MAX;
}

// A whitespace-separated token at `at` (advanced past it).
std::string token(std::vector<std::uint8_t> const& data, std::size_t& at)
{
    while (at < data.size() && is_space(data[at]))
        ++at;
    std::size_t const start = at;
    if (at < data.size() && (data[at] == '/' || data[at] == '[' || data[at] == ']' || data[at] == '{' || data[at] == '}')) {
        ++at;
        if (data[start] != '/')
            return std::string(1, static_cast<char>(data[start]));
    }
    while (at < data.size() && !is_space(data[at]) && data[at] != '/' && data[at] != '[' && data[at] != ']' && data[at] != '{'
           && data[at] != '}')
        ++at;
    return std::string(data.begin() + static_cast<std::ptrdiff_t>(start), data.begin() + static_cast<std::ptrdiff_t>(at));
}

} // namespace

std::vector<std::uint8_t> type1_decrypt(std::vector<std::uint8_t> const& data, std::uint16_t key, std::size_t skip)
{
    std::vector<std::uint8_t> out;
    out.reserve(data.size());
    std::uint16_t r = key;
    for (std::size_t i = 0; i < data.size(); ++i) {
        std::uint8_t const cipher = data[i];
        std::uint8_t const plain = static_cast<std::uint8_t>(cipher ^ (r >> 8));
        r = static_cast<std::uint16_t>((cipher + r) * 52845u + 22719u);
        if (i >= skip)
            out.push_back(plain);
    }
    return out;
}

std::optional<Type1Font> Type1Font::parse(std::vector<std::uint8_t> const& file_in, std::size_t length1, std::size_t length2)
{
    // A PFB file: segments of 0x80 type length; joined into the plain form.
    std::vector<std::uint8_t> file;
    if (file_in.size() > 6 && file_in[0] == 0x80 && (file_in[1] == 1 || file_in[1] == 2)) {
        std::size_t at = 0;
        std::size_t first_segment = 0;
        bool first = true;
        while (at + 6 <= file_in.size() && file_in[at] == 0x80 && (file_in[at + 1] == 1 || file_in[at + 1] == 2)) {
            std::size_t const n = file_in[at + 2] | (file_in[at + 3] << 8) | (file_in[at + 4] << 16)
                | (static_cast<std::size_t>(file_in[at + 5]) << 24);
            at += 6;
            std::size_t const take = std::min(n, file_in.size() - at);
            file.insert(file.end(), file_in.begin() + static_cast<std::ptrdiff_t>(at), file_in.begin() + static_cast<std::ptrdiff_t>(at + take));
            if (first) {
                first_segment = take;
                first = false;
            }
            at += take;
        }
        length1 = first_segment;
        length2 = 0;
    } else {
        file = file_in;
    }
    Type1Font font;
    // The cleartext part: up to "eexec".
    std::size_t const eexec = find(file, "eexec");
    if (length1 == 0 || length1 > file.size())
        length1 = eexec == SIZE_MAX ? file.size() : eexec + 5;
    else if (eexec != SIZE_MAX && eexec + 5 <= file.size() && eexec + 5 != length1 && length1 < eexec)
        length1 = eexec + 5;
    // The encoding.
    std::size_t encoding_at = find(file, "/Encoding", 0, length1);
    if (encoding_at != SIZE_MAX) {
        std::size_t at = encoding_at + 9;
        std::string const first = token(file, at);
        if (first == "StandardEncoding") {
            font.m_standard_encoding = true;
            for (int code = 0; code < 256; ++code) {
                char const* name = encoding_data::k_standard[static_cast<std::size_t>(code)];
                font.m_encoding[static_cast<std::size_t>(code)] = name ? name : "";
            }
        } else {
            // "dup <code> /<name> put" entries until "readonly def" or "def".
            for (int guard = 0; guard < 4096 && at < length1; ++guard) {
                std::string const t = token(file, at);
                if (t.empty() || t == "def" || t == "readonly")
                    break;
                if (t != "dup")
                    continue;
                std::string const code_token = token(file, at);
                std::string const name_token = token(file, at);
                int const code = std::atoi(code_token.c_str());
                if (code >= 0 && code < 256 && name_token.size() > 1 && name_token[0] == '/')
                    font.m_encoding[static_cast<std::size_t>(code)] = name_token.substr(1);
            }
        }
    }
    std::size_t matrix_at = find(file, "/FontMatrix", 0, length1);
    if (matrix_at != SIZE_MAX) {
        std::size_t at = matrix_at + 11;
        std::string t = token(file, at);
        if (t == "[" || t == "{") {
            for (std::size_t i = 0; i < 6; ++i) {
                t = token(file, at);
                char* end = nullptr;
                double const v = std::strtod(t.c_str(), &end);
                if (end == t.c_str())
                    break;
                font.m_font_matrix[i] = v;
            }
        }
    }
    // The encrypted part: binary, or hexadecimal when its first four bytes are hex digits.
    std::size_t start = length1;
    while (start < file.size() && is_space(file[start]))
        ++start;
    std::size_t end = length2 > 0 ? std::min(file.size(), length1 + length2) : file.size();
    if (end <= start)
        end = file.size();
    std::vector<std::uint8_t> encrypted(file.begin() + static_cast<std::ptrdiff_t>(start), file.begin() + static_cast<std::ptrdiff_t>(end));
    if (encrypted.size() >= 4 && is_hex(encrypted[0]) && is_hex(encrypted[1]) && is_hex(encrypted[2]) && is_hex(encrypted[3])) {
        std::vector<std::uint8_t> binary;
        int pending = -1;
        for (std::uint8_t const c : encrypted) {
            if (!is_hex(c))
                continue;
            int const v = std::isdigit(c) ? c - '0' : (std::tolower(c) - 'a' + 10);
            if (pending < 0) {
                pending = v;
            } else {
                binary.push_back(static_cast<std::uint8_t>(pending * 16 + v));
                pending = -1;
            }
        }
        encrypted = std::move(binary);
    }
    std::vector<std::uint8_t> const priv = type1_decrypt(encrypted, 55665, 4);
    // lenIV, then the Subrs array and the CharStrings dictionary.
    int len_iv = 4;
    std::size_t const len_iv_at = find(priv, "/lenIV");
    if (len_iv_at != SIZE_MAX) {
        std::size_t at = len_iv_at + 6;
        len_iv = std::atoi(token(priv, at).c_str());
    }
    auto const read_binary = [&](std::size_t& at, std::size_t length) -> std::optional<std::vector<std::uint8_t>> {
        // "RD " or "-| ": one space, then the bytes.
        std::string const rd = token(priv, at);
        (void)rd;
        if (at < priv.size())
            ++at;
        if (at + length > priv.size())
            return std::nullopt;
        std::vector<std::uint8_t> bytes(priv.begin() + static_cast<std::ptrdiff_t>(at), priv.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        if (len_iv >= 0)
            return type1_decrypt(bytes, 4330, static_cast<std::size_t>(len_iv));
        return bytes;
    };
    std::size_t const subrs_at = find(priv, "/Subrs");
    if (subrs_at != SIZE_MAX) {
        std::size_t at = subrs_at + 6;
        int const count = std::atoi(token(priv, at).c_str());
        if (count > 0 && count < 65536)
            font.m_subrs.resize(static_cast<std::size_t>(count));
        for (int guard = 0; guard < 65536 && at < priv.size(); ++guard) {
            std::size_t const before = at;
            std::string const t = token(priv, at);
            if (t == "dup") {
                int const index = std::atoi(token(priv, at).c_str());
                std::size_t const length = static_cast<std::size_t>(std::max(0, std::atoi(token(priv, at).c_str())));
                std::optional<std::vector<std::uint8_t>> bytes = read_binary(at, length);
                if (!bytes)
                    break;
                if (index >= 0 && static_cast<std::size_t>(index) < font.m_subrs.size())
                    font.m_subrs[static_cast<std::size_t>(index)] = std::move(*bytes);
                continue;
            }
            if (t == "ND" || t == "|-" || t == "noaccess" || t == "def" || t == "NP" || t == "|" || t == "put" || t == "readonly"
                || t == "array")
                continue;
            if (t.empty() || t.find("CharStrings") != std::string::npos || at == before)
                break;
        }
    }
    std::size_t const charstrings_at = find(priv, "/CharStrings");
    if (charstrings_at == SIZE_MAX)
        return std::nullopt;
    std::size_t at = charstrings_at + 12;
    token(priv, at); // count
    for (std::size_t guard = 0; guard < k_max_glyphs * 4 && at < priv.size() && font.m_charstrings.size() < k_max_glyphs; ++guard) {
        std::size_t const before = at;
        std::string const t = token(priv, at);
        if (t.empty() || at == before)
            break;
        if (t[0] != '/' || t.size() < 2) {
            if (t == "end" && font.m_charstrings.size() > 0)
                break;
            continue;
        }
        std::string const name = t.substr(1);
        std::string const length_token = token(priv, at);
        char* number_end = nullptr;
        long const length = std::strtol(length_token.c_str(), &number_end, 10);
        if (number_end == length_token.c_str() || length < 0)
            continue;
        std::optional<std::vector<std::uint8_t>> bytes = read_binary(at, static_cast<std::size_t>(length));
        if (!bytes)
            break;
        font.m_charstrings.emplace(name, std::move(*bytes));
    }
    if (font.m_charstrings.empty())
        return std::nullopt;
    return font;
}

struct Type1State {
    svg::Path path;
    std::vector<double> stack;
    std::vector<double> ps_stack; // results an OtherSubr hands back to "pop"
    double x = 0;
    double y = 0;
    double sbx = 0;
    double advance = 0;
    bool open = false;
    bool flex = false;
    std::vector<std::array<double, 2>> flex_points;
    double flex_start_x = 0; // where the pen was when the flex began
    double flex_start_y = 0;
    bool seac = false;
    int ops = 0;
    double offset_x = 0; // seac's accent placement
    double offset_y = 0;
};

bool Type1Font::run(std::vector<std::uint8_t> const& cs, int depth, Type1State& st) const
{
    if (depth > k_max_depth)
        return false;
    auto const point = [&](double px, double py) {
        return svg::Point { static_cast<float>(px + st.offset_x), static_cast<float>(py + st.offset_y) };
    };
    auto const close_path = [&] {
        if (st.open) {
            st.path.close();
            st.open = false;
        }
    };
    auto const move_to = [&](double nx, double ny) {
        st.x = nx;
        st.y = ny;
        if (st.flex) {
            st.flex_points.push_back({ nx, ny });
            return;
        }
        close_path();
        st.path.move_to(point(st.x, st.y));
        st.open = true;
    };
    auto const line_to = [&](double nx, double ny) {
        if (!st.open) {
            st.path.move_to(point(st.x, st.y));
            st.open = true;
        }
        st.x = nx;
        st.y = ny;
        st.path.line_to(point(st.x, st.y));
    };
    auto const curve_to = [&](double x1, double y1, double x2, double y2, double x3, double y3) {
        if (!st.open) {
            st.path.move_to(point(st.x, st.y));
            st.open = true;
        }
        st.path.cubic_to(point(x1, y1), point(x2, y2), point(x3, y3));
        st.x = x3;
        st.y = y3;
    };
    auto const arg = [&](std::size_t i) { return i < st.stack.size() ? st.stack[i] : 0.0; };
    for (std::size_t i = 0; i < cs.size();) {
        if (++st.ops > 100000)
            return false;
        std::uint8_t const v = cs[i++];
        if (v >= 32) {
            double number = 0;
            if (v <= 246) {
                number = v - 139;
            } else if (v <= 250) {
                if (i >= cs.size())
                    return false;
                number = (v - 247) * 256 + cs[i++] + 108;
            } else if (v <= 254) {
                if (i >= cs.size())
                    return false;
                number = -(v - 251) * 256 - cs[i++] - 108;
            } else {
                if (i + 4 > cs.size())
                    return false;
                std::int32_t const n = static_cast<std::int32_t>((static_cast<std::uint32_t>(cs[i]) << 24) | (cs[i + 1] << 16) | (cs[i + 2] << 8) | cs[i + 3]);
                number = n;
                i += 4;
            }
            if (st.stack.size() < 48)
                st.stack.push_back(number);
            continue;
        }
        switch (v) {
        case 1: // hstem
        case 3: // vstem
            st.stack.clear();
            break;
        case 4: // vmoveto
            move_to(st.x, st.y + arg(0));
            st.stack.clear();
            break;
        case 5: // rlineto
            line_to(st.x + arg(0), st.y + arg(1));
            st.stack.clear();
            break;
        case 6: // hlineto
            line_to(st.x + arg(0), st.y);
            st.stack.clear();
            break;
        case 7: // vlineto
            line_to(st.x, st.y + arg(0));
            st.stack.clear();
            break;
        case 8: // rrcurveto
            curve_to(st.x + arg(0), st.y + arg(1), st.x + arg(0) + arg(2), st.y + arg(1) + arg(3), st.x + arg(0) + arg(2) + arg(4),
                st.y + arg(1) + arg(3) + arg(5));
            st.stack.clear();
            break;
        case 9: // closepath
            close_path();
            st.stack.clear();
            break;
        case 10: { // callsubr
            if (st.stack.empty())
                return false;
            int const index = static_cast<int>(st.stack.back());
            st.stack.pop_back();
            if (index < 0 || static_cast<std::size_t>(index) >= m_subrs.size())
                return false;
            if (!run(m_subrs[static_cast<std::size_t>(index)], depth + 1, st))
                return false;
            break;
        }
        case 11: // return
            return true;
        case 13: // hsbw: side bearing x, width
            st.sbx = arg(0);
            st.x = arg(0);
            st.y = 0;
            if (!st.seac)
                st.advance = arg(1);
            st.stack.clear();
            break;
        case 14: // endchar
            close_path();
            st.stack.clear();
            return true;
        case 21: // rmoveto
            move_to(st.x + arg(0), st.y + arg(1));
            st.stack.clear();
            break;
        case 22: // hmoveto
            move_to(st.x + arg(0), st.y);
            st.stack.clear();
            break;
        case 30: // vhcurveto
            curve_to(st.x, st.y + arg(0), st.x + arg(1), st.y + arg(0) + arg(2), st.x + arg(1) + arg(3), st.y + arg(0) + arg(2));
            st.stack.clear();
            break;
        case 31: // hvcurveto
            curve_to(st.x + arg(0), st.y, st.x + arg(0) + arg(1), st.y + arg(2), st.x + arg(0) + arg(1), st.y + arg(2) + arg(3));
            st.stack.clear();
            break;
        case 12: {
            if (i >= cs.size())
                return false;
            std::uint8_t const e = cs[i++];
            switch (e) {
            case 0: // dotsection
            case 1: // vstem3
            case 2: // hstem3
                st.stack.clear();
                break;
            case 6: { // seac: asb adx ady bchar achar
                double const asb = arg(0);
                double const adx = arg(1);
                double const ady = arg(2);
                int const bchar = static_cast<int>(arg(3));
                int const achar = static_cast<int>(arg(4));
                st.stack.clear();
                char const* base_name = bchar >= 0 && bchar < 256 ? encoding_data::k_standard[static_cast<std::size_t>(bchar)] : nullptr;
                char const* accent_name = achar >= 0 && achar < 256 ? encoding_data::k_standard[static_cast<std::size_t>(achar)] : nullptr;
                if (!base_name || !accent_name || depth > 0)
                    return false;
                auto const base = m_charstrings.find(base_name);
                auto const accent = m_charstrings.find(accent_name);
                if (base == m_charstrings.end() || accent == m_charstrings.end())
                    return false;
                close_path();
                double const keep_advance = st.advance;
                double const sbx = st.sbx;
                st.seac = true;
                st.offset_x = 0;
                st.offset_y = 0;
                if (!run(base->second, depth + 1, st))
                    return false;
                close_path();
                // The accent placed by its own side bearing against the base's.
                st.offset_x = sbx - asb + adx;
                st.offset_y = ady;
                st.x = 0;
                st.y = 0;
                if (!run(accent->second, depth + 1, st))
                    return false;
                close_path();
                st.offset_x = 0;
                st.offset_y = 0;
                st.advance = keep_advance;
                return true;
            }
            case 7: // sbw: sbx sby wx wy
                st.sbx = arg(0);
                st.x = arg(0);
                st.y = arg(1);
                if (!st.seac)
                    st.advance = arg(2);
                st.stack.clear();
                break;
            case 12: { // div
                if (st.stack.size() >= 2) {
                    double const b = st.stack.back();
                    st.stack.pop_back();
                    double const a = st.stack.back();
                    st.stack.back() = b == 0 ? 0 : a / b;
                }
                break;
            }
            case 16: { // callothersubr: args... n othersubr#
                if (st.stack.size() < 2)
                    return false;
                int const number = static_cast<int>(st.stack.back());
                st.stack.pop_back();
                int const count = static_cast<int>(st.stack.back());
                st.stack.pop_back();
                if (count < 0 || static_cast<std::size_t>(count) > st.stack.size())
                    return false;
                std::vector<double> args(st.stack.end() - count, st.stack.end());
                st.stack.resize(st.stack.size() - static_cast<std::size_t>(count));
                st.ps_stack.clear();
                if (number == 1) {
                    // Flex begins: the next seven moves are its points.
                    st.flex = true;
                    st.flex_points.clear();
                    st.flex_start_x = st.x;
                    st.flex_start_y = st.y;
                } else if (number == 2) {
                    // A flex point was added by the rmoveto before it.
                } else if (number == 0) {
                    // Flex ends: two curves through the collected points.
                    st.flex = false;
                    if (st.flex_points.size() >= 7) {
                        auto const p = st.flex_points;
                        st.x = st.flex_start_x;
                        st.y = st.flex_start_y;
                        curve_to(p[1][0], p[1][1], p[2][0], p[2][1], p[3][0], p[3][1]);
                        curve_to(p[4][0], p[4][1], p[5][0], p[5][1], p[6][0], p[6][1]);
                    }
                    // The end point is handed back for setcurrentpoint.
                    if (args.size() >= 3) {
                        st.ps_stack = { args[2], args[1] };
                    }
                } else if (number == 3) {
                    // Hint replacement: the subroutine number comes back to be ignored.
                    st.ps_stack = { 3 };
                } else {
                    // Others hand their arguments back in reverse, as PostScript would.
                    for (auto it = args.begin(); it != args.end(); ++it)
                        st.ps_stack.push_back(*it);
                }
                break;
            }
            case 17: { // pop: a result of the OtherSubr onto the charstring stack
                double value = 0;
                if (!st.ps_stack.empty()) {
                    value = st.ps_stack.back();
                    st.ps_stack.pop_back();
                }
                if (st.stack.size() < 48)
                    st.stack.push_back(value);
                break;
            }
            case 33: // setcurrentpoint
                st.x = arg(0);
                st.y = arg(1);
                st.stack.clear();
                break;
            default:
                st.stack.clear();
                break;
            }
            break;
        }
        default:
            st.stack.clear();
            break;
        }
    }
    return true;
}

std::optional<svg::Path> Type1Font::glyph(std::string_view name, double* advance) const
{
    auto const it = m_charstrings.find(name);
    if (it == m_charstrings.end())
        return std::nullopt;
    Type1State state;
    if (!run(it->second, 0, state))
        return std::nullopt;
    if (advance)
        *advance = state.advance;
    return state.path;
}

}
