#include "platform/Window.h"

namespace sashfold::platform {

std::unique_ptr<Window> Window::create(std::string const&, WindowPlacement const&, Bitmap const*)
{
    // The AppKit shell is not written yet; nothing links. Linux has its
    // Wayland window in platform/linux.
    return nullptr;
}

}
