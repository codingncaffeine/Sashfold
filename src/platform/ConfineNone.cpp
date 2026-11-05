#include "platform/Confine.h"

namespace sashfold::platform {

// Windows and macOS have their own ways (a job object that allows no child
// process, a sandbox profile); neither is written yet.
bool forbid_new_programs(std::string* why_not)
{
    if (why_not != nullptr)
        *why_not = "not written for this system yet";
    return false;
}

}
