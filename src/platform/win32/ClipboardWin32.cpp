#include "platform/Clipboard.h"

#include "core/Bitmap.h"
#include "core/Png.h"

#include <algorithm>
#include <thread>
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

// A device-independent bitmap (CF_DIB: a BITMAPINFOHEADER and the rows) as
// PNG: 24- and 32-bit rows, bottom-up or top-down, with the channel masks a
// BI_BITFIELDS header carries. A 32-bit picture whose alpha is all zero is
// opaque (most programs leave it so).
std::vector<std::uint8_t> png_of_dib(std::uint8_t const* bytes, std::size_t size)
{
    if (size < sizeof(BITMAPINFOHEADER))
        return {};
    BITMAPINFOHEADER header;
    std::memcpy(&header, bytes, sizeof header);
    int const width = header.biWidth;
    bool const top_down = header.biHeight < 0;
    int const height = top_down ? -header.biHeight : header.biHeight;
    int const bits = header.biBitCount;
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 || (bits != 24 && bits != 32)
        || (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS))
        return {};
    std::size_t offset = header.biSize;
    std::uint32_t masks[3] = { 0x00ff0000, 0x0000ff00, 0x000000ff };
    if (header.biCompression == BI_BITFIELDS && header.biSize == sizeof(BITMAPINFOHEADER)) {
        if (size < offset + 12)
            return {};
        std::memcpy(masks, bytes + offset, 12);
        offset += 12;
    }
    std::size_t const row = (static_cast<std::size_t>(width) * static_cast<std::size_t>(bits) / 8 + 3) / 4 * 4;
    if (size < offset + row * static_cast<std::size_t>(height))
        return {};
    auto const shift_of = [](std::uint32_t mask) {
        int shift = 0;
        while (mask != 0 && (mask & 1) == 0) {
            mask >>= 1;
            ++shift;
        }
        return shift;
    };
    int const shifts[3] = { shift_of(masks[0]), shift_of(masks[1]), shift_of(masks[2]) };
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    bool any_alpha = false;
    for (int y = 0; y < height; ++y) {
        std::uint8_t const* const source = bytes + offset + row * static_cast<std::size_t>(top_down ? y : height - 1 - y);
        std::uint8_t* const target = pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            std::uint8_t* const out = target + static_cast<std::size_t>(x) * 4;
            if (bits == 24) {
                std::uint8_t const* const in = source + static_cast<std::size_t>(x) * 3;
                out[0] = in[2];
                out[1] = in[1];
                out[2] = in[0];
                out[3] = 255;
            } else {
                std::uint32_t value;
                std::memcpy(&value, source + static_cast<std::size_t>(x) * 4, 4);
                for (int c = 0; c < 3; ++c)
                    out[c] = static_cast<std::uint8_t>((value & masks[c]) >> shifts[c]);
                out[3] = static_cast<std::uint8_t>(value >> 24);
                any_alpha = any_alpha || out[3] != 0;
            }
        }
    }
    if (bits == 32 && !any_alpha) {
        for (std::size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
    }
    return encode_png(Bitmap(width, height, std::move(pixels)));
}

std::vector<std::uint8_t> global_bytes(HANDLE handle)
{
    std::vector<std::uint8_t> bytes;
    if (void const* const memory = GlobalLock(handle)) {
        std::size_t const size = GlobalSize(handle);
        if (size <= largest_clipboard_picture)
            bytes.assign(static_cast<std::uint8_t const*>(memory), static_cast<std::uint8_t const*>(memory) + size);
        GlobalUnlock(handle);
    }
    return bytes;
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
    UINT const png_format = RegisterClipboardFormatW(L"PNG");
    bool const has_text = IsClipboardFormatAvailable(CF_UNICODETEXT);
    bool const has_html = html_format && IsClipboardFormatAvailable(html_format);
    bool const has_png = png_format && IsClipboardFormatAvailable(png_format);
    bool const has_dib = IsClipboardFormatAvailable(CF_DIB);
    if ((!has_text && !has_html && !has_png && !has_dib) || !OpenClipboard(nullptr))
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
    // A picture: as PNG where the program put one (browsers, editors), else
    // from the bitmap every program can put (a screenshot).
    if (has_png) {
        if (HANDLE const handle = GetClipboardData(png_format))
            content.png = global_bytes(handle);
    }
    if (content.png.empty() && has_dib) {
        if (HANDLE const handle = GetClipboardData(CF_DIB)) {
            std::vector<std::uint8_t> const dib = global_bytes(handle);
            content.png = png_of_dib(dib.data(), dib.size());
        }
    }
    CloseClipboard();
    return content;
}

// Read on a thread of its own: a program that renders its clipboard late
// (Windows asks it at the read) never holds the window's thread.
bool os_start_clipboard_read(std::shared_ptr<ClipboardRead> const& read, std::function<void()> wake)
{
    std::thread([read, wake = std::move(wake)] {
        read->finish(os_read_clipboard());
        if (wake)
            wake();
    }).detach();
    return true;
}

std::optional<bool> os_clipboard_offers_content()
{
    UINT const html_format = RegisterClipboardFormatW(L"HTML Format");
    UINT const png_format = RegisterClipboardFormatW(L"PNG");
    return IsClipboardFormatAvailable(CF_UNICODETEXT) || (html_format && IsClipboardFormatAvailable(html_format))
        || (png_format && IsClipboardFormatAvailable(png_format)) || IsClipboardFormatAvailable(CF_DIB);
}
}
