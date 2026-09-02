#include "platform/Clipboard.h"

#include <algorithm>
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

} // namespace

bool os_write_clipboard_text(std::string const& utf8)
{
    if (!OpenClipboard(nullptr))
        return false;
    bool ok = false;
    if (EmptyClipboard()) {
        std::vector<wchar_t> const wide = to_utf16(utf8);
        HGLOBAL const handle = GlobalAlloc(GMEM_MOVEABLE, wide.size() * sizeof(wchar_t));
        if (handle) {
            if (void* const memory = GlobalLock(handle)) {
                std::copy(wide.begin(), wide.end(), static_cast<wchar_t*>(memory));
                GlobalUnlock(handle);
                ok = SetClipboardData(CF_UNICODETEXT, handle) != nullptr;
            }
            if (!ok)
                GlobalFree(handle);
        }
    }
    CloseClipboard();
    return ok;
}

std::optional<std::string> os_read_clipboard_text()
{
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr))
        return std::nullopt;
    std::optional<std::string> text;
    if (HANDLE const handle = GetClipboardData(CF_UNICODETEXT)) {
        if (void const* const memory = GlobalLock(handle)) {
            text = to_utf8(static_cast<wchar_t const*>(memory));
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return text;
}

}
