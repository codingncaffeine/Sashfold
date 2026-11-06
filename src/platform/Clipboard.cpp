#include "platform/Clipboard.h"

namespace sashfold::platform {

namespace {

bool g_process_clipboard = false;
std::optional<ClipboardContent> g_process_content;

} // namespace

void use_process_clipboard(bool enabled)
{
    g_process_clipboard = enabled;
}

bool write_clipboard(ClipboardContent const& content)
{
    if (g_process_clipboard) {
        g_process_content = content;
        return true;
    }
    if (os_write_clipboard(content))
        return true;
    // No backend on this OS yet: the process keeps it, so copy and paste
    // still round-trip inside the browser.
    g_process_content = content;
    return false;
}

std::optional<ClipboardContent> read_clipboard()
{
    if (g_process_clipboard)
        return g_process_content;
    if (std::optional<ClipboardContent> content = os_read_clipboard())
        return content;
    return g_process_content;
}

bool write_clipboard_text(std::string const& utf8)
{
    return write_clipboard(ClipboardContent { utf8, {} });
}

std::optional<std::string> read_clipboard_text()
{
    std::optional<ClipboardContent> content = read_clipboard();
    if (!content)
        return std::nullopt;
    return std::move(content->text);
}

}
