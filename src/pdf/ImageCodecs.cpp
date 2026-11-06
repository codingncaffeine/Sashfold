#include "pdf/ImageCodecs.h"

// The codecs not written yet decline, and their images are left out of
// the page as an unreadable picture is. Each moves to a file of its own as
// it is written (GAPS plan item 5b): CCITT fax is in Ccitt.cpp, JPEG 2000 in
// Jpx.cpp.

namespace sashfold::pdf {

std::optional<std::vector<std::uint8_t>> decode_jbig2(std::vector<std::uint8_t> const&, std::vector<std::uint8_t> const&, int, int)
{
    return std::nullopt;
}

}
