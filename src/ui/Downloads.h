#pragma once

// Downloads as fetch (plan M3, §7): a response the engine cannot render is
// written to the downloads directory, marked with the mark of the web, and
// never opened — Sashfold shows a page saying what it saved and where.

#include "net/Url.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sashfold::ui {

// Whether the shell renders this content type itself (HTML, XHTML, anything
// textual) rather than downloading it.
bool is_renderable_content_type(std::string const& content_type);

// The file name for a download: Content-Disposition's filename* or filename
// when present, else the last path segment of the URL, else "download";
// always a single sanitized path component.
std::string download_file_name(std::string const* content_disposition, net::Url const& url);

struct DownloadResult {
    std::string path; // where the bytes landed; empty on failure
    std::string file_name; // possibly suffixed to avoid overwriting
    bool marked = false; // the mark of the web was written
    std::string error;
};

// Writes the bytes under a fresh name in the directory (an existing name
// gets " (1)", " (2)", ... before its extension) and marks the file with
// its origin: on Windows the Zone.Identifier stream (ZoneId 3, the
// Internet zone) with the source and referrer URLs.
DownloadResult save_download(std::string const& directory, std::string const& file_name,
    std::vector<std::uint8_t> const& bytes, net::Url const& source, std::string const& referrer);

}
