#include "platform/Fonts.h"

#include <cstdlib>

namespace sashfold::platform {

std::vector<std::string> system_font_directories()
{
    std::vector<std::string> directories;
    char const* home = std::getenv("HOME");
    std::string const user_home = home && *home ? home : "";
#ifdef __APPLE__
    directories.push_back("/System/Library/Fonts");
    directories.push_back("/System/Library/Fonts/Supplemental");
    directories.push_back("/Library/Fonts");
    if (!user_home.empty())
        directories.push_back(user_home + "/Library/Fonts");
#else
    directories.push_back("/usr/share/fonts");
    directories.push_back("/usr/local/share/fonts");
    if (!user_home.empty()) {
        directories.push_back(user_home + "/.local/share/fonts");
        directories.push_back(user_home + "/.fonts");
    }
#endif
    return directories;
}

}
