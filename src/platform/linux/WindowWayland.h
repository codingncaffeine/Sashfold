#pragma once

// The Wayland window's face to the rest of the Linux platform layer: the
// clipboard rides the same connection (wl_data_device), so the clipboard
// seam asks the live window.

#include "platform/Clipboard.h"

#include <optional>
#include <string>

namespace sashfold::platform {

class WaylandWindow;

// The window that is open, if one is; the clipboard goes through it.
WaylandWindow* wayland_window();
bool wayland_write_clipboard(WaylandWindow&, ClipboardContent const&);
std::optional<ClipboardContent> wayland_read_clipboard(WaylandWindow&);
bool wayland_start_clipboard_read(WaylandWindow&, std::shared_ptr<ClipboardRead> const&, std::function<void()> wake);
bool wayland_clipboard_offers_content(WaylandWindow&);

}
