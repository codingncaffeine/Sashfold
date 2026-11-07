#include "media/linux/H264Accelerators.h"

namespace sashfold::media {

std::unique_ptr<H264Accelerator> H264Accelerator::open(std::string& error, VideoApi api)
{
    std::string vulkan_error;
    if (api == VideoApi::Any || api == VideoApi::Vulkan) {
        if (std::unique_ptr<H264Accelerator> accelerator = open_vulkan_h264(vulkan_error))
            return accelerator;
        if (api == VideoApi::Vulkan) {
            error = vulkan_error;
            return nullptr;
        }
    }
    // VA-API for H.264 is owed (GAPS plan 6b): Vulkan Video is the only way yet.
    error = api == VideoApi::Vaapi ? "VA-API H.264 decoding is not written yet" : "Vulkan Video: " + vulkan_error;
    return nullptr;
}

}
