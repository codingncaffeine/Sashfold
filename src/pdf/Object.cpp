#include "pdf/Object.h"

#include <array>
#include <cmath>
#include <limits>

namespace sashfold::pdf {

namespace {

std::string const k_empty_string;
Array const k_empty_array;
Dict const k_empty_dict;
Object const k_null;

void append_utf8(std::string& out, std::uint32_t code)
{
    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
        code = 0xFFFD;
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

// PDFDocEncoding (Annex D.2) where it differs from Latin-1: 0x18-0x1F and
// 0x80-0xA0.
constexpr std::array<std::uint16_t, 8> k_pdfdoc_18 = { 0x02D8, 0x02C7, 0x02C6, 0x02D9, 0x02DD, 0x02DB, 0x02DA, 0x02DC };
constexpr std::array<std::uint16_t, 33> k_pdfdoc_80 = {
    0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044, 0x2039, 0x203A, 0x2212, 0x2030, 0x201E,
    0x201C, 0x201D, 0x2018, 0x2019, 0x201A, 0x2122, 0xFB01, 0xFB02, 0x0141, 0x0152, 0x0160, 0x0178, 0x017D,
    0x0131, 0x0142, 0x0153, 0x0161, 0x017E, 0xFFFD, 0x20AC,
};

} // namespace

std::int64_t Object::as_int(std::int64_t fallback) const
{
    if (auto const* i = std::get_if<std::int64_t>(&value))
        return *i;
    if (auto const* r = std::get_if<double>(&value)) {
        if (std::isfinite(*r) && std::abs(*r) < 9e18)
            return static_cast<std::int64_t>(std::llround(*r));
    }
    return fallback;
}

double Object::as_number(double fallback) const
{
    if (auto const* i = std::get_if<std::int64_t>(&value))
        return static_cast<double>(*i);
    if (auto const* r = std::get_if<double>(&value))
        return *r;
    return fallback;
}

std::string const& Object::as_string() const
{
    if (auto const* s = std::get_if<String>(&value))
        return s->bytes;
    return k_empty_string;
}

std::string const& Object::as_name() const
{
    if (auto const* n = std::get_if<Name>(&value))
        return n->text;
    return k_empty_string;
}

Array const& Object::as_array() const
{
    if (auto const* a = std::get_if<std::shared_ptr<Array const>>(&value))
        return **a;
    return k_empty_array;
}

Dict const& Object::as_dict() const
{
    if (auto const* d = std::get_if<std::shared_ptr<Dict const>>(&value))
        return **d;
    if (auto const* s = std::get_if<std::shared_ptr<Stream const>>(&value))
        return (*s)->dict;
    return k_empty_dict;
}

Stream const* Object::as_stream() const
{
    if (auto const* s = std::get_if<std::shared_ptr<Stream const>>(&value))
        return s->get();
    return nullptr;
}

void Dict::set(std::string key, Object value)
{
    for (auto& entry : m_entries) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    }
    m_entries.emplace_back(std::move(key), std::move(value));
}

Object const* Dict::find(std::string_view key) const
{
    for (auto const& entry : m_entries) {
        if (entry.first == key)
            return &entry.second;
    }
    return nullptr;
}

Object const& Dict::get(std::string_view key) const
{
    Object const* const found = find(key);
    return found ? *found : k_null;
}

std::vector<std::uint8_t> Stream::raw() const
{
    if (!source || start > source->size())
        return {};
    std::size_t const n = std::min(length, source->size() - start);
    return { source->begin() + static_cast<std::ptrdiff_t>(start), source->begin() + static_cast<std::ptrdiff_t>(start + n) };
}

std::string text_string_to_utf8(std::string const& bytes)
{
    std::string out;
    auto const byte = [&](std::size_t i) { return static_cast<std::uint8_t>(bytes[i]); };
    if (bytes.size() >= 2 && byte(0) == 0xFE && byte(1) == 0xFF) {
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            std::uint32_t code = (static_cast<std::uint32_t>(byte(i)) << 8) | byte(i + 1);
            if (code >= 0xD800 && code < 0xDC00 && i + 3 < bytes.size()) {
                std::uint32_t const low = (static_cast<std::uint32_t>(byte(i + 2)) << 8) | byte(i + 3);
                if (low >= 0xDC00 && low < 0xE000) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            // A language escape (U+001B ... U+001B) is dropped with its code.
            if (code == 0x1B) {
                std::size_t j = i + 2;
                while (j + 1 < bytes.size() && !(byte(j) == 0 && byte(j + 1) == 0x1B))
                    j += 2;
                i = j;
                continue;
            }
            append_utf8(out, code);
        }
        return out;
    }
    if (bytes.size() >= 3 && byte(0) == 0xEF && byte(1) == 0xBB && byte(2) == 0xBF)
        return bytes.substr(3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        std::uint8_t const b = byte(i);
        std::uint32_t code = b;
        if (b >= 0x18 && b <= 0x1F)
            code = k_pdfdoc_18[b - 0x18];
        else if (b >= 0x80 && b <= 0xA0)
            code = k_pdfdoc_80[b - 0x80];
        append_utf8(out, code);
    }
    return out;
}

}
