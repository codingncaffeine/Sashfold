#include "platform/Notifier.h"

namespace sashfold::platform {

// Windows and macOS show notifications through services of their own;
// neither is written yet.
std::unique_ptr<Notifier> open_notifier(std::string, std::function<void()>)
{
    return nullptr;
}

}
