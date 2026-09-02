#include "platform/Clipboard.h"

// No clipboard backend on Linux or macOS yet: the Wayland data-device
// protocol and the AppKit pasteboard arrive with their shells. Until then
// the process-private clipboard in Clipboard.cpp stands in.

namespace sashfold::platform {

bool os_write_clipboard_text(std::string const&)
{
    return false;
}

std::optional<std::string> os_read_clipboard_text()
{
    return std::nullopt;
}

}
