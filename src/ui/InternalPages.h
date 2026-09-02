#pragma once

// Pages the shell generates itself (plan M3): error pages — including the
// certificate-error page, which offers no way through on purpose — the
// about:sashfold page, and the view-source and plain-text wrappers. All are
// ordinary HTML rendered by the engine, so they look the same on every OS
// and need nothing the engine cannot already do.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::ui {

inline constexpr std::string_view version_string = "0.3 (M3 preview)";

std::string html_escape(std::string_view text);

// A load that failed before any document arrived.
std::string error_page(std::string_view heading, std::string_view detail, std::string_view url);

// Certificate validation failed: scary, specific, and final (§7 policy).
std::string certificate_error_page(std::string_view host, std::string_view url);

std::string about_sashfold_page();

// The raw bytes of a document as escaped, preformatted text.
std::string source_page(std::string_view url, std::vector<std::uint8_t> const& bytes);

// Plain text, shown preformatted.
std::string text_page(std::string_view title, std::vector<std::uint8_t> const& bytes);

// A content type the engine cannot show and no downloads folder to save it
// to; nothing is written to disk.
std::string unsupported_content_page(std::string_view url, std::string_view content_type,
    std::size_t byte_count);

// What a download saved, and where; the file is never opened.
std::string download_page(std::string_view file_name, std::string_view path,
    std::size_t byte_count, std::string_view content_type, bool marked);

}
