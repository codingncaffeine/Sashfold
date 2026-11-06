#include "platform/Clipboard.h"

// No clipboard backend on macOS yet: the AppKit pasteboard arrives with
// its shell. Until then the process-private clipboard in Clipboard.cpp
// stands in.

namespace sashfold::platform {

bool os_write_clipboard(ClipboardContent const&)
{
    return false;
}

std::optional<ClipboardContent> os_read_clipboard()
{
    return std::nullopt;
}

}
