// No H.264 hardware path written for this system.
#include "media/H264Accelerator.h"

namespace sashfold::media {

std::unique_ptr<H264Accelerator> H264Accelerator::open(std::string& error, VideoApi)
{
    error = "no H.264 decoder is written for this system";
    return nullptr;
}

}
