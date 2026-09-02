#pragma once

// The clipboard seam: text in, text out, through the OS interface where one
// exists. A clipboard private to this process stands in for --script runs
// (a test never touches the user's clipboard) and on OSes without a
// backend yet.

#include <optional>
#include <string>

namespace sashfold::platform {

// Routes every read and write to the process-private clipboard.
void use_process_clipboard(bool enabled);

bool write_clipboard_text(std::string const& utf8);
std::optional<std::string> read_clipboard_text();

// The OS backends (one per platform).
bool os_write_clipboard_text(std::string const& utf8);
std::optional<std::string> os_read_clipboard_text();

}
