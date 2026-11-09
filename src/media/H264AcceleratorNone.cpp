// No H.264 hardware path written for this system: the software decoder.
#include "media/H264Accelerator.h"
#include "media/H264Software.h"

namespace sashfold::media {

std::unique_ptr<H264Accelerator> H264Accelerator::open(std::string& error, VideoApi api)
{
    if (api == VideoApi::Any || api == VideoApi::Software)
        return open_software_h264();
    error = "no H.264 hardware decoder is written for this system";
    return nullptr;
}

}
