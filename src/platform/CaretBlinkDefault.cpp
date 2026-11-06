#include "platform/CaretBlink.h"

// No desktop setting read on this OS yet: the caret blinks as AppKit's
// does by default.

namespace sashfold::platform {

void start_caret_blink_lookup()
{
}

std::optional<CaretBlink> caret_blink()
{
    return CaretBlink { 500, 0 };
}

}
