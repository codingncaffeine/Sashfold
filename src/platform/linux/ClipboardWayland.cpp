#include "platform/Clipboard.h"

// The Linux clipboard is the Wayland selection, which rides the window's
// connection (wl_data_device on the window's seat). With no window open
// there is no selection to speak to, and the process-private clipboard in
// Clipboard.cpp stands in.

#include "platform/linux/WindowWayland.h"

namespace sashfold::platform {

bool os_write_clipboard_text(std::string const& utf8)
{
    WaylandWindow* const window = wayland_window();
    return window && wayland_write_clipboard_text(*window, utf8);
}

std::optional<std::string> os_read_clipboard_text()
{
    WaylandWindow* const window = wayland_window();
    if (!window)
        return std::nullopt;
    return wayland_read_clipboard_text(*window);
}

}
