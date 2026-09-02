#pragma once

// Where the OS keeps its fonts. The files are data (the pledge's line);
// reading them is ours, but only the platform knows the directories.

#include <string>
#include <vector>

namespace sashfold::platform {

// System-wide and per-user font directories, existing or not, in the
// order a lookup should prefer them.
std::vector<std::string> system_font_directories();

}
