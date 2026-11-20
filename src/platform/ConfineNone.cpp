#include "platform/Confine.h"

namespace sashfold::platform {

// macOS has its own way (a sandbox profile), not written yet; Windows'
// half is platform/win32/ConfineWin32.cpp.
bool forbid_new_programs(std::string* why_not)
{
    if (why_not != nullptr)
        *why_not = "not written for this system yet";
    return false;
}

bool keep_writes_to(std::vector<std::string> const&, std::string* why_not)
{
    if (why_not != nullptr)
        *why_not = "not written for this system yet";
    return false;
}

bool keep_writes_to(std::vector<std::string> const& folders, std::vector<std::string> const&, std::string* why_not)
{
    return keep_writes_to(folders, why_not);
}

}
