#include "platform/Accessibility.h"

namespace sashfold::platform {

// No bridge on this system yet (Windows' UI Automation and macOS's
// NSAccessibility are not written): no tree is ever made.
std::unique_ptr<Accessibility> open_accessibility(std::string const&, std::function<void()>) { return nullptr; }

}
