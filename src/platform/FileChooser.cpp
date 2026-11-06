#include "platform/FileChooser.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace sashfold::platform {

namespace {

struct KnownType {
    std::string_view extension;
    std::string_view type;
};

// The types the engines give the files people upload most (Chromium's
// built-in table and the shared-mime-info names for the rest).
constexpr KnownType known_types[] = {
    { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "jpe", "image/jpeg" }, { "jfif", "image/jpeg" },
    { "gif", "image/gif" }, { "webp", "image/webp" }, { "avif", "image/avif" }, { "heic", "image/heic" }, { "heif", "image/heif" },
    { "bmp", "image/bmp" }, { "ico", "image/vnd.microsoft.icon" }, { "svg", "image/svg+xml" }, { "svgz", "image/svg+xml" },
    { "tif", "image/tiff" }, { "tiff", "image/tiff" }, { "jxl", "image/jxl" },
    { "mp4", "video/mp4" }, { "m4v", "video/mp4" }, { "webm", "video/webm" }, { "mov", "video/quicktime" }, { "mkv", "video/x-matroska" },
    { "avi", "video/x-msvideo" }, { "ogv", "video/ogg" }, { "mpeg", "video/mpeg" }, { "mpg", "video/mpeg" }, { "3gp", "video/3gpp" },
    { "mp3", "audio/mpeg" }, { "wav", "audio/wav" }, { "ogg", "audio/ogg" }, { "oga", "audio/ogg" }, { "opus", "audio/ogg" },
    { "m4a", "audio/x-m4a" }, { "aac", "audio/aac" }, { "flac", "audio/flac" }, { "weba", "audio/webm" }, { "mid", "audio/midi" },
    { "pdf", "application/pdf" }, { "zip", "application/zip" }, { "gz", "application/gzip" }, { "tgz", "application/gzip" },
    { "tar", "application/x-tar" }, { "7z", "application/x-7z-compressed" }, { "rar", "application/vnd.rar" },
    { "json", "application/json" }, { "xml", "text/xml" }, { "csv", "text/csv" }, { "txt", "text/plain" }, { "text", "text/plain" },
    { "md", "text/markdown" }, { "html", "text/html" }, { "htm", "text/html" }, { "css", "text/css" }, { "js", "text/javascript" },
    { "mjs", "text/javascript" }, { "ics", "text/calendar" }, { "vcf", "text/vcard" }, { "rtf", "application/rtf" },
    { "doc", "application/msword" }, { "docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
    { "xls", "application/vnd.ms-excel" }, { "xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
    { "ppt", "application/vnd.ms-powerpoint" }, { "pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation" },
    { "odt", "application/vnd.oasis.opendocument.text" }, { "ods", "application/vnd.oasis.opendocument.spreadsheet" },
    { "odp", "application/vnd.oasis.opendocument.presentation" }, { "epub", "application/epub+zip" },
    { "wasm", "application/wasm" }, { "woff", "font/woff" }, { "woff2", "font/woff2" }, { "ttf", "font/ttf" }, { "otf", "font/otf" },
};

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}

std::string type_of_file(std::string const& name)
{
    std::size_t const dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 == name.size())
        return {};
    std::string const extension = lower(std::string_view(name).substr(dot + 1));
    for (KnownType const& known : known_types) {
        if (known.extension == extension)
            return std::string(known.type);
    }
    return {};
}

std::vector<std::string> patterns_for_type(std::string const& type)
{
    std::string const wanted = lower(type);
    std::vector<std::string> out;
    bool const wildcard = wanted.size() > 2 && wanted.ends_with("/*");
    std::string_view const family = wildcard ? std::string_view(wanted).substr(0, wanted.size() - 1) : std::string_view();
    for (KnownType const& known : known_types) {
        if (wildcard ? known.type.starts_with(family) : known.type == wanted)
            out.push_back("*." + std::string(known.extension));
    }
    return out;
}

std::optional<std::string> path_of_file_uri(std::string const& uri)
{
    constexpr std::string_view scheme = "file://";
    if (uri.compare(0, scheme.size(), scheme) != 0)
        return std::nullopt;
    std::string_view rest = std::string_view(uri).substr(scheme.size());
    // file://host/path: only the local host (empty or "localhost").
    std::size_t const slash = rest.find('/');
    if (slash == std::string_view::npos)
        return std::nullopt;
    std::string_view const host = rest.substr(0, slash);
    if (!host.empty() && host != "localhost")
        return std::nullopt;
    rest = rest.substr(slash);
    auto const hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string path;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size() && hex(rest[i + 1]) >= 0 && hex(rest[i + 2]) >= 0) {
            path += static_cast<char>(hex(rest[i + 1]) * 16 + hex(rest[i + 2]));
            i += 2;
        } else {
            path += rest[i];
        }
    }
#ifdef _WIN32
    // file:///C:/dir → C:/dir
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':')
        path.erase(0, 1);
#endif
    return path;
}

}
