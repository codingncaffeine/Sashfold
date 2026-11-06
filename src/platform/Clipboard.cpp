#include "platform/Clipboard.h"

#include <utility>

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

bool ClipboardRead::done() const
{
    std::lock_guard const lock(m_mutex);
    return m_done;
}

std::optional<ClipboardContent> ClipboardRead::take()
{
    std::optional<ClipboardContent> content;
    {
        std::lock_guard const lock(m_mutex);
        content = std::exchange(m_content, std::nullopt);
    }
    return content ? content : std::exchange(fallback, std::nullopt);
}

void ClipboardRead::finish(std::optional<ClipboardContent> content)
{
    std::lock_guard const lock(m_mutex);
    if (m_done)
        return;
    m_content = std::move(content);
    m_done = true;
}

std::shared_ptr<ClipboardRead> start_clipboard_read(std::function<void()> wake)
{
    auto read = std::make_shared<ClipboardRead>();
    read->fallback = g_process_content;
    if (g_process_clipboard || !os_start_clipboard_read(read, std::move(wake)))
        read->finish(g_process_clipboard ? g_process_content : std::nullopt);
    return read;
}

bool clipboard_offers_content()
{
    auto const has = [](std::optional<ClipboardContent> const& content) {
        return content && (!content->text.empty() || !content->html.empty() || !content->png.empty());
    };
    if (g_process_clipboard)
        return has(g_process_content);
    if (std::optional<bool> const offers = os_clipboard_offers_content())
        return *offers || has(g_process_content);
    return has(g_process_content);
}

bool write_clipboard_text(std::string const& utf8)
{
    return write_clipboard(ClipboardContent { utf8, {}, {} });
}

std::optional<std::string> read_clipboard_text()
{
    std::optional<ClipboardContent> content = read_clipboard();
    if (!content)
        return std::nullopt;
    return std::move(content->text);
}

}
