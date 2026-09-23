#include "media/Vp9Accelerator.h"

// Where this build has no way to the video hardware yet: Windows (Media
// Foundation) and macOS (VideoToolbox) follow the Linux one. A stream plays
// through the software decoder instead.

namespace sashfold::media {

std::unique_ptr<Vp9Accelerator> Vp9Accelerator::open(std::string& error, VideoApi)
{
    error = "this build has no way to the machine's video hardware";
    return nullptr;
}

}
