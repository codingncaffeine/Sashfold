#include "ui/PdfViewer.h"

#include "pdf/Extract.h"
#include "pdf/Render.h"
#include "platform/Random.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace sashfold::ui {

std::string pdf_viewer_html(); // PdfViewerPage.cpp

namespace {

std::mutex g_registry_mutex;
std::map<std::string, std::weak_ptr<PdfHandle>>& registry()
{
    static std::map<std::string, std::weak_ptr<PdfHandle>> handles;
    return handles;
}

std::string json_string(std::string_view text)
{
    std::string out = "\"";
    for (char const c : text) {
        auto const u = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (u < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", u);
                out += buffer;
            } else {
                out.push_back(c);
            }
        }
    }
    return out + "\"";
}

std::string number(double v)
{
    if (!std::isfinite(v))
        return "0";
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.2f", v);
    // Trailing zeros off: 12.50 → 12.5, 3.00 → 3.
    std::string s = buffer;
    while (s.find('.') != std::string::npos && (s.back() == '0' || s.back() == '.')) {
        bool const dot = s.back() == '.';
        s.pop_back();
        if (dot)
            break;
    }
    return s;
}

// The ligatures as the letters they join, so find and copy see words.
std::string unligature(std::string const& text)
{
    static constexpr std::array<std::pair<char const*, char const*>, 7> k_ligatures = { {
        { "\xEF\xAC\x80", "ff" }, { "\xEF\xAC\x81", "fi" }, { "\xEF\xAC\x82", "fl" }, { "\xEF\xAC\x83", "ffi" },
        { "\xEF\xAC\x84", "ffl" }, { "\xEF\xAC\x85", "st" }, { "\xEF\xAC\x86", "st" },
    } };
    if (text.find("\xEF\xAC") == std::string::npos)
        return text;
    std::string out = text;
    for (auto const& [ligature, letters] : k_ligatures) {
        std::size_t at = 0;
        while ((at = out.find(ligature, at)) != std::string::npos) {
            out.replace(at, 3, letters);
            at += std::strlen(letters);
        }
    }
    return out;
}

std::string destination_json(pdf::Destination const& d)
{
    std::string out = "\"page\":" + std::to_string(d.page);
    if (d.y)
        out += ",\"y\":" + number(*d.y);
    if (d.x)
        out += ",\"x\":" + number(*d.x);
    return out;
}

void outline_json(std::vector<pdf::OutlineItem> const& items, std::string& out, int depth)
{
    out += "[";
    bool first = true;
    for (pdf::OutlineItem const& item : items) {
        if (!first)
            out += ",";
        first = false;
        out += "{\"title\":" + json_string(item.title);
        if (item.destination)
            out += "," + destination_json(*item.destination);
        if (!item.url.empty())
            out += ",\"url\":" + json_string(item.url);
        if (item.open)
            out += ",\"open\":true";
        if (!item.children.empty() && depth < 32) {
            out += ",\"children\":";
            outline_json(item.children, out, depth + 1);
        }
        out += "}";
    }
    out += "]";
}

// A 32-bit top-down BMP (BITMAPV4 header with an alpha mask): written and
// read back in a blink, which is what a page's picture needs on its way
// from the loader's thread to the page's.
std::vector<std::uint8_t> encode_bmp(Bitmap const& bitmap)
{
    std::uint32_t const width = static_cast<std::uint32_t>(bitmap.width());
    std::uint32_t const height = static_cast<std::uint32_t>(bitmap.height());
    std::uint32_t const header = 14 + 108;
    std::uint32_t const size = header + width * height * 4;
    std::vector<std::uint8_t> out;
    out.reserve(size);
    auto const u16 = [&](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    auto const u32 = [&](std::uint32_t v) {
        u16(v & 0xFFFF);
        u16(v >> 16);
    };
    out.push_back('B');
    out.push_back('M');
    u32(size);
    u32(0);
    u32(header);
    u32(108); // BITMAPV4HEADER
    u32(width);
    u32(static_cast<std::uint32_t>(-static_cast<std::int32_t>(height))); // top-down
    u16(1);
    u16(32);
    u32(3); // BI_BITFIELDS
    u32(width * height * 4);
    u32(2835);
    u32(2835);
    u32(0);
    u32(0);
    u32(0x00FF0000); // red
    u32(0x0000FF00); // green
    u32(0x000000FF); // blue
    u32(0xFF000000); // alpha
    u32(0x73524742); // 'sRGB'
    for (int i = 0; i < 9; ++i)
        u32(0); // endpoints
    u32(0);
    u32(0);
    u32(0); // gamma
    std::vector<std::uint8_t> const& pixels = bitmap.pixels(); // RGBA
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        out.push_back(pixels[i + 2]);
        out.push_back(pixels[i + 1]);
        out.push_back(pixels[i]);
        out.push_back(pixels[i + 3]);
    }
    return out;
}

net::FetchResult answer(net::Url const& url, std::string const& type, std::vector<std::uint8_t> body, int status = 200)
{
    net::FetchResponse response;
    response.status = status;
    response.status_text = status == 200 ? "OK" : "Not Found";
    response.headers.push_back({ "content-type", type });
    response.headers.push_back({ "cache-control", "no-store" });
    response.body = std::move(body);
    response.final_url = url;
    return { std::move(response), {} };
}

net::FetchResult answer_text(net::Url const& url, std::string const& type, std::string const& text)
{
    return answer(url, type, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::string query_value(net::Url const& url, std::string_view key)
{
    if (!url.query)
        return {};
    std::string_view query = *url.query;
    while (!query.empty()) {
        std::size_t const amp = query.find('&');
        std::string_view const pair = query.substr(0, amp);
        std::size_t const eq = pair.find('=');
        if (pair.substr(0, eq) == key)
            return eq == std::string_view::npos ? std::string {} : std::string(pair.substr(eq + 1));
        if (amp == std::string_view::npos)
            break;
        query = query.substr(amp + 1);
    }
    return {};
}

} // namespace

bool is_pdf_response(std::string const& content_type, std::vector<std::uint8_t> const& bytes)
{
    std::string type;
    for (char const c : content_type) {
        if (c == ';')
            break;
        if (c != ' ' && c != '\t')
            type.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (type == "application/pdf" || type == "application/x-pdf")
        return true;
    // Served with no type, or a generic binary one, a file that begins as a PDF is one.
    bool const generic = type.empty() || type == "application/octet-stream" || type == "binary/octet-stream"
        || type == "application/download" || type == "application/force-download";
    return generic && bytes.size() >= 5 && bytes[0] == '%' && bytes[1] == 'P' && bytes[2] == 'D' && bytes[3] == 'F' && bytes[4] == '-';
}

std::shared_ptr<PdfHandle> open_pdf(std::vector<std::uint8_t> bytes, net::Url const& url, std::string file_name,
    std::optional<net::Url> embedder)
{
    auto handle = std::make_shared<PdfHandle>();
    std::array<std::uint8_t, 16> token {};
    platform::fill_random(token);
    char hex[3];
    for (std::uint8_t const b : token) {
        std::snprintf(hex, sizeof hex, "%02x", b);
        handle->id += hex;
    }
    handle->url = url;
    handle->url.fragment.reset();
    if (embedder) {
        handle->embedder = std::move(embedder);
        handle->embedder->fragment.reset();
    }
    handle->file_name = std::move(file_name);
    handle->bytes = std::move(bytes);
    pdf::Document::Status status {};
    handle->document = pdf::Document::open(handle->bytes, {}, status);
    handle->status = status;
    std::lock_guard const lock(g_registry_mutex);
    // Forget the documents nobody holds any more.
    for (auto it = registry().begin(); it != registry().end();) {
        if (it->second.expired())
            it = registry().erase(it);
        else
            ++it;
    }
    registry()[handle->id] = handle;
    return handle;
}

std::shared_ptr<PdfHandle> find_pdf(std::string const& id)
{
    std::lock_guard const lock(g_registry_mutex);
    auto const it = registry().find(id);
    return it == registry().end() ? nullptr : it->second.lock();
}

std::string pdf_title(PdfHandle const& handle)
{
    std::lock_guard const lock(handle.mutex);
    if (handle.document) {
        std::string const title = handle.document->title();
        if (!title.empty())
            return title;
    }
    return handle.file_name;
}

std::string pdf_viewer_page(PdfHandle const& handle)
{
    std::string page = pdf_viewer_html();
    // The page knows its document by its id and its file's name, nothing else.
    auto const put = [&](std::string const& slot, std::string const& value) {
        std::size_t at = 0;
        while ((at = page.find(slot, at)) != std::string::npos) {
            page.replace(at, slot.size(), value);
            at += value.size();
        }
    };
    put("@@ID@@", handle.id);
    std::string escaped_title;
    for (char const c : pdf_title(handle)) {
        if (c == '<')
            escaped_title += "&lt;";
        else if (c == '&')
            escaped_title += "&amp;";
        else
            escaped_title.push_back(c);
    }
    put("@@TITLE@@", escaped_title);
    return page;
}

std::optional<net::FetchResult> serve_pdf_resource(net::Url const& url, net::Url const& first_party, std::string const& method,
    std::vector<std::uint8_t> const& body)
{
    if (url.scheme != k_pdf_scheme)
        return std::nullopt;
    std::shared_ptr<PdfHandle> const handle = find_pdf(url.host);
    net::Url viewer = first_party;
    viewer.fragment.reset();
    if (!handle
        || (viewer.serialize() != handle->url.serialize()
            && (!handle->embedder || viewer.serialize() != handle->embedder->serialize())))
        return net::FetchResult { std::nullopt, "no such document for this page" };
    std::vector<std::string> const& path = url.path;
    std::shared_ptr<pdf::Document const> document;
    pdf::Document::Status status {};
    {
        std::lock_guard const lock(handle->mutex);
        document = handle->document;
        status = handle->status;
    }
    if (path.size() == 1 && path[0] == "unlock" && method == "POST") {
        std::string const password(body.begin(), body.end());
        pdf::Document::Status opened_status {};
        std::shared_ptr<pdf::Document const> opened = pdf::Document::open(handle->bytes, password, opened_status);
        if (opened) {
            std::lock_guard const lock(handle->mutex);
            handle->document = opened;
            handle->status = opened_status;
        }
        return answer_text(url, "application/json", opened ? "{\"ok\":true}" : "{\"ok\":false}");
    }
    if (path.size() == 1 && path[0] == "file")
        return answer(url, "application/pdf", handle->bytes);
    if (path.size() == 1 && path[0] == "info.json") {
        std::string out = "{\"fileName\":" + json_string(handle->file_name);
        if (!document) {
            out += status == pdf::Document::Status::NeedsPassword ? ",\"needsPassword\":true}" : ",\"error\":true}";
            return answer_text(url, "application/json", out);
        }
        out += ",\"title\":" + json_string(document->title());
        out += ",\"pages\":[";
        for (std::size_t i = 0; i < document->page_count(); ++i) {
            pdf::Page const& page = document->page(i);
            bool const turned = page.rotate % 180 != 0;
            double const w = page.crop_box.width() * page.user_unit;
            double const h = page.crop_box.height() * page.user_unit;
            out += (i ? ",[" : "[") + number(turned ? h : w) + "," + number(turned ? w : h) + "]";
        }
        out += "],\"outline\":";
        outline_json(pdf::document_outline(*document), out, 0);
        out += "}";
        return answer_text(url, "application/json", out);
    }
    if (path.size() >= 2 && path[0] == "page" && document) {
        // page/<n>.bmp, page/<n>/text.json, page/<n>/links.json (n from 0)
        std::string const& last = path[1];
        std::size_t const index = static_cast<std::size_t>(std::strtoul(last.c_str(), nullptr, 10));
        if (index >= document->page_count())
            return answer(url, "text/plain", {}, 404);
        if (path.size() == 2 && last.ends_with(".bmp")) {
            pdf::RenderOptions options;
            options.scale = std::clamp(std::atof(query_value(url, "scale").c_str()), 0.05, 8.0);
            int const rotation = std::atoi(query_value(url, "rotate").c_str());
            options.extra_rotation = ((rotation % 360) + 360) % 360 / 90 * 90;
            std::optional<Bitmap> const picture = pdf::render_page(*document, index, options);
            if (!picture)
                return answer(url, "text/plain", {}, 404);
            return answer(url, "image/bmp", encode_bmp(*picture));
        }
        if (path.size() == 3 && path[2] == "text.json") {
            std::string out = "[";
            bool first = true;
            for (pdf::TextRun const& run : pdf::page_text(*document, index)) {
                if (!first)
                    out += ",";
                first = false;
                out += "[" + json_string(unligature(run.text)) + "," + number(run.x) + "," + number(run.y) + "," + number(run.width) + ","
                    + number(run.height) + "," + number(run.angle) + "," + (run.ends_line ? "1" : "0") + "]";
            }
            return answer_text(url, "application/json", out + "]");
        }
        if (path.size() == 3 && path[2] == "links.json") {
            std::string out = "[";
            bool first = true;
            for (pdf::Link const& link : pdf::page_links(*document, index)) {
                if (!first)
                    out += ",";
                first = false;
                out += "{\"r\":[" + number(link.x0) + "," + number(link.y0) + "," + number(link.x1) + "," + number(link.y1) + "]";
                if (link.destination)
                    out += "," + destination_json(*link.destination);
                else
                    out += ",\"url\":" + json_string(link.url);
                out += "}";
            }
            return answer_text(url, "application/json", out + "]");
        }
    }
    return answer(url, "text/plain", {}, 404);
}

}
