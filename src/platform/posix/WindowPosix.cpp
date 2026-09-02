#include "platform/Window.h"

namespace sashfold::platform {

std::unique_ptr<Window> Window::create(std::string const&, int, int)
{
    // The Wayland and AppKit shells are not written yet; nothing links.
    return nullptr;
}

}
