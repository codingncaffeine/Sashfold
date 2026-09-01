#include "html/InputStream.h"

#include "core/Ascii.h"
#include "core/Unicode.h"

namespace sashfold::html {

InputStream::InputStream(std::u32string code_points)
{
    // Newline normalization: CRLF -> LF, lone CR -> LF.
    m_data.reserve(code_points.size());
    for (std::size_t i = 0; i < code_points.size(); ++i) {
        if (code_points[i] == U'\r') {
            m_data.push_back(U'\n');
            if (i + 1 < code_points.size() && code_points[i + 1] == U'\n')
                ++i;
        } else {
            m_data.push_back(code_points[i]);
        }
    }
}

InputStream InputStream::from_utf8(std::string_view bytes)
{
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB
        && static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.remove_prefix(3);
    return InputStream(decode_utf8(bytes));
}

std::optional<char32_t> InputStream::next()
{
    if (m_position >= m_data.size())
        return std::nullopt;
    return m_data[m_position++];
}

bool InputStream::lookahead_equals_ignoring_case(std::u32string_view expected) const
{
    if (!has(expected.size() - 1))
        return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (to_ascii_lowercase(m_data[m_position + i]) != to_ascii_lowercase(expected[i]))
            return false;
    }
    return true;
}

}
