#pragma once

// The Wayland window's face to the rest of the Linux platform layer: the
// clipboard rides the same connection (wl_data_device), so the clipboard
// seam asks the live window.

#include <optional>
#include <string>

namespace sashfold::platform {

class WaylandWindow;

// The window that is open, if one is; the clipboard goes through it.
WaylandWindow* wayland_window();
bool wayland_write_clipboard_text(WaylandWindow&, std::string const& utf8);
std::optional<std::string> wayland_read_clipboard_text(WaylandWindow&);

}
