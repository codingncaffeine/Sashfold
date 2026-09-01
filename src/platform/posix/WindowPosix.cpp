#include "platform/Window.h"

namespace sashfold::platform {

std::unique_ptr<Window> Window::create(std::string const&, int, int)
{
    // Wayland (M3.5) and AppKit (M5) are on the roadmap; nothing links yet.
    return nullptr;
}

}
