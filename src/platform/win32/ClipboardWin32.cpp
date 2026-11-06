#include "platform/Clipboard.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace sashfold::platform {

namespace {

// UTF-8 to UTF-16 with a terminating zero, surrogates for the astral plane.
std::vector<wchar_t> to_utf16(std::string const& utf8)
{
    std::vector<wchar_t> out;
    std::size_t i = 0;
    while (i < utf8.size()) {
        auto const lead = static_cast<unsigned char>(utf8[i]);
        char32_t code_point = 0;
        std::size_t length = 1;
        if (lead < 0x80) {
            code_point = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            code_point = lead & 0x1F;
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            code_point = lead & 0x0F;
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            code_point = lead & 0x07;
            length = 4;
        } else {
            code_point = 0xFFFD;
        }
        if (i + length > utf8.size()) {
            code_point = 0xFFFD;
            length = 1;
        } else {
            for (std::size_t k = 1; k < length; ++k)
                code_point = (code_point << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        }
        i += length;
        if (code_point >= 0x10000) {
            code_point -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (code_point >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (code_point & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(code_point));
        }
    }
    out.push_back(0);
    return out;
}

std::string to_utf8(wchar_t const* text)
{
    std::string out;
    for (std::size_t i = 0; text[i] != 0; ++i) {
        char32_t code_point = static_cast<wchar_t>(text[i]);
        if (code_point >= 0xD800 && code_point < 0xDC00 && text[i + 1] >= 0xDC00
            && text[i + 1] < 0xE000) {
            code_point = 0x10000 + ((code_point - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            ++i;
        }
        if (code_point < 0x80) {
            out += static_cast<char>(code_point);
        } else if (code_point < 0x800) {
            out += static_cast<char>(0xC0 | (code_point >> 6));
            out += static_cast<char>(0x80 | (code_point & 0x3F));
        } else if (code_point < 0x10000) {
            out += static_cast<char>(0xE0 | (code_point >> 12));
            out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code_point & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code_point >> 18));
            out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code_point & 0x3F));
        }
    }
    return out;
}

// The "HTML Format" clipboard format (CF_HTML): the markup in UTF-8 behind
// a header of byte offsets to the document and to the fragment in it.
std::string cf_html_of(std::string const& fragment)
{
    static constexpr char header[] = "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n";
    static constexpr std::string_view before = "<html><body>\r\n<!--StartFragment-->";
    static constexpr std::string_view after = "<!--EndFragment-->\r\n</body></html>";
    char probe[160];
    std::size_t const header_length = static_cast<std::size_t>(std::snprintf(probe, sizeof probe, header, std::size_t {}, std::size_t {}, std::size_t {}, std::size_t {}));
    std::size_t const start_html = header_length;
    std::size_t const start_fragment = start_html + before.size();
    std::size_t const end_fragment = start_fragment + fragment.size();
    std::size_t const end_html = end_fragment + after.size();
    char head[160];
    std::snprintf(head, sizeof head, header, start_html, end_html, start_fragment, end_fragment);
    return std::string(head) + std::string(before) + fragment + std::string(after);
}

std::optional<std::string> fragment_of_cf_html(std::string const& data)
{
    auto const number_after = [&](std::string_view key) -> std::optional<std::size_t> {
        std::size_t const at = data.find(key);
        if (at == std::string::npos)
            return std::nullopt;
        std::size_t value = 0;
        bool any = false;
        for (std::size_t i = at + key.size(); i < data.size() && data[i] >= '0' && data[i] <= '9'; ++i) {
            value = value * 10 + static_cast<std::size_t>(data[i] - '0');
            any = true;
        }
        if (!any)
            return std::nullopt;
        return value;
    };
    std::optional<std::size_t> const start = number_after("StartFragment:");
    std::optional<std::size_t> const end = number_after("EndFragment:");
    if (!start || !end || *start > *end || *end > data.size())
        return std::nullopt;
    return data.substr(*start, *end - *start);
}

bool put_global(UINT format, void const* bytes, std::size_t size)
{
    HGLOBAL const handle = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!handle)
        return false;
    bool ok = false;
    if (void* const memory = GlobalLock(handle)) {
        std::memcpy(memory, bytes, size);
        GlobalUnlock(handle);
        ok = SetClipboardData(format, handle) != nullptr;
    }
    if (!ok)
        GlobalFree(handle);
    return ok;
}

} // namespace

bool os_write_clipboard(ClipboardContent const& content)
{
    if (!OpenClipboard(nullptr))
        return false;
    bool ok = false;
    if (EmptyClipboard()) {
        std::vector<wchar_t> const wide = to_utf16(content.text);
        ok = put_global(CF_UNICODETEXT, wide.data(), wide.size() * sizeof(wchar_t));
        if (ok && !content.html.empty()) {
            if (UINT const html_format = RegisterClipboardFormatW(L"HTML Format")) {
                std::string const data = cf_html_of(content.html);
                put_global(html_format, data.c_str(), data.size() + 1);
            }
        }
    }
    CloseClipboard();
    return ok;
}

std::optional<ClipboardContent> os_read_clipboard()
{
    UINT const html_format = RegisterClipboardFormatW(L"HTML Format");
    bool const has_text = IsClipboardFormatAvailable(CF_UNICODETEXT);
    bool const has_html = html_format && IsClipboardFormatAvailable(html_format);
    if ((!has_text && !has_html) || !OpenClipboard(nullptr))
        return std::nullopt;
    ClipboardContent content;
    if (has_text) {
        if (HANDLE const handle = GetClipboardData(CF_UNICODETEXT)) {
            if (void const* const memory = GlobalLock(handle)) {
                content.text = to_utf8(static_cast<wchar_t const*>(memory));
                GlobalUnlock(handle);
            }
        }
    }
    if (has_html) {
        if (HANDLE const handle = GetClipboardData(html_format)) {
            if (void const* const memory = GlobalLock(handle)) {
                std::size_t const size = GlobalSize(handle);
                char const* const bytes = static_cast<char const*>(memory);
                std::string const data(bytes, strnlen(bytes, size));
                GlobalUnlock(handle);
                if (std::optional<std::string> fragment = fragment_of_cf_html(data))
                    content.html = std::move(*fragment);
            }
        }
    }
    CloseClipboard();
    return content;
}
}
