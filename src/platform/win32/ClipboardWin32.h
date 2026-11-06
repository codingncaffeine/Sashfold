#pragma once

// What the Windows clipboard and the window's drags share: markup as the
// "HTML Format" clipboard format writes it (a header of offsets, then the
// fragment between its markers).

#include <string>

namespace sashfold::platform {

std::string windows_cf_html(std::string const& fragment);

}
