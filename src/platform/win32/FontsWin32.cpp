#include "platform/Fonts.h"

#include <cstdlib>

namespace sashfold::platform {

std::vector<std::string> system_font_directories()
{
    std::vector<std::string> directories;
    if (char const* windir = std::getenv("WINDIR"); windir && *windir)
        directories.push_back(std::string(windir) + "\\Fonts");
    else
        directories.push_back("C:\\Windows\\Fonts");
    // Fonts installed for one user only live under the profile.
    if (char const* local = std::getenv("LOCALAPPDATA"); local && *local)
        directories.push_back(std::string(local) + "\\Microsoft\\Windows\\Fonts");
    return directories;
}

}
