#pragma once

// Pages the shell generates itself: error pages — including the
// certificate-error page, which offers no way through on purpose — the
// about:sashfold page, and the view-source and plain-text wrappers. All are
// ordinary HTML rendered by the engine, so they look the same on every OS
// and need nothing the engine cannot already do.

#include "core/Bitmap.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::ui {

inline constexpr std::string_view version_string = "0.3 (preview)";

std::string html_escape(std::string_view text);

// What the new-tab page is drawn from: the local time it opens at and the
// milliseconds until the next minute (its clock ticks from there on the
// page's own timers), the date in words, the pictures it rotates through as
// file: URLs with the one to show first at the front, how long each stays
// (0 never rotates), and the theme's colors — the page belongs to the chrome.
struct NewTabPage {
    int hour = 0;
    int minute = 0;
    int next_minute_ms = 60000;
    std::string date;
    std::vector<std::string> pictures;
    int rotate_ms = 20000;
    Color background;
    Color background_end;
    Color text;
    Color text_muted;
};

// A clock, a greeting by the hour and the date over the theme's pictures,
// or over a gradient of the theme's colors when it names none. Zero network.
std::string new_tab_page(NewTabPage const& page);

// A load that failed before any document arrived.
std::string error_page(std::string_view heading, std::string_view detail, std::string_view url);

// Certificate validation failed: scary, specific, and final — no override.
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
