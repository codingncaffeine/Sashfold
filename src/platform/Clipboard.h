#pragma once

// The clipboard seam: what a copy holds, in, out, through the OS interface
// where one exists. A clipboard private to this process stands in for
// --script runs (a test never touches the user's clipboard) and on OSes
// without a backend yet.

#include <optional>
#include <string>

namespace sashfold::platform {

// What a copy puts on the clipboard: its text always, and its markup
// (text/html) when it came from a page, so a paste into a rich editor
// keeps the links, the bold and the lists as every browser does.
struct ClipboardContent {
    std::string text;
    std::string html; // empty when the copy had none
};

// Routes every read and write to the process-private clipboard.
void use_process_clipboard(bool enabled);

bool write_clipboard(ClipboardContent const&);
std::optional<ClipboardContent> read_clipboard();

// The text alone (the address bar, a form field).
bool write_clipboard_text(std::string const& utf8);
std::optional<std::string> read_clipboard_text();

// The OS backends (one per platform).
bool os_write_clipboard(ClipboardContent const&);
std::optional<ClipboardContent> os_read_clipboard();

}
