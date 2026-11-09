#include "media/linux/H264Accelerators.h"

#include "media/H264Software.h"

namespace sashfold::media {

std::unique_ptr<H264Accelerator> H264Accelerator::open(std::string& error, VideoApi api)
{
    if (api == VideoApi::Software)
        return open_software_h264();
    std::string vulkan_error;
    if (api == VideoApi::Any || api == VideoApi::Vulkan) {
        if (std::unique_ptr<H264Accelerator> accelerator = open_vulkan_h264(vulkan_error))
            return accelerator;
        if (api == VideoApi::Vulkan) {
            error = vulkan_error;
            return nullptr;
        }
    }
    std::string vaapi_error;
    if (std::unique_ptr<H264Accelerator> accelerator = open_vaapi_h264(vaapi_error))
        return accelerator;
    // No hardware decoder: the processor's, as every browser falls back to.
    if (api == VideoApi::Any)
        return open_software_h264();
    error = api == VideoApi::Vaapi ? "VA-API: " + vaapi_error : "Vulkan Video: " + vulkan_error + "; VA-API: " + vaapi_error;
    return nullptr;
}

}
