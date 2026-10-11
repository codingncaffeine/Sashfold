#include "platform/Fonts.h"

#include <cstdlib>
#include <string>
#include <string_view>

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
    // Where fontconfig looks by default: the XDG data directories' fonts
    // folders (a system that keeps its data elsewhere, NixOS or a Flatpak
    // runtime, names them in XDG_DATA_DIRS), then the two fixed system
    // folders and the user's older ~/.fonts. Each folder once.
    auto const add = [&directories](std::string directory) {
        if (directory.empty() || directory.front() != '/')
            return;
        while (directory.size() > 1 && directory.back() == '/')
            directory.pop_back();
        for (std::string const& known : directories) {
            if (known == directory)
                return;
        }
        directories.push_back(std::move(directory));
    };
    char const* const data_home = std::getenv("XDG_DATA_HOME");
    if (data_home && *data_home)
        add(std::string(data_home) + "/fonts");
    else if (!user_home.empty())
        add(user_home + "/.local/share/fonts");
    char const* const data_dirs = std::getenv("XDG_DATA_DIRS");
    std::string_view rest = data_dirs && *data_dirs ? data_dirs : "/usr/local/share:/usr/share";
    while (!rest.empty()) {
        std::size_t const colon = rest.find(':');
        std::string_view entry = rest.substr(0, colon);
        while (entry.size() > 1 && entry.back() == '/')
            entry.remove_suffix(1);
        if (!entry.empty())
            add(std::string(entry) + "/fonts");
        rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
    }
    add("/usr/share/fonts");
    add("/usr/local/share/fonts");
    if (!user_home.empty())
        add(user_home + "/.fonts");
#endif
    return directories;
}

}
