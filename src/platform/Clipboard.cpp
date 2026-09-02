#include "platform/Clipboard.h"

namespace sashfold::platform {

namespace {

bool g_process_clipboard = false;
std::optional<std::string> g_process_text;

} // namespace

void use_process_clipboard(bool enabled)
{
    g_process_clipboard = enabled;
}

bool write_clipboard_text(std::string const& utf8)
{
    if (g_process_clipboard) {
        g_process_text = utf8;
        return true;
    }
    if (os_write_clipboard_text(utf8))
        return true;
    // No backend on this OS yet: the process keeps it, so copy and paste
    // still round-trip inside the browser.
    g_process_text = utf8;
    return false;
}

std::optional<std::string> read_clipboard_text()
{
    if (g_process_clipboard)
        return g_process_text;
    if (std::optional<std::string> text = os_read_clipboard_text())
        return text;
    return g_process_text;
}

}
