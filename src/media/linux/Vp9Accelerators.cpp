#include "media/linux/Vp9Accelerators.h"

namespace sashfold::media {

// Vulkan Video first: it reaches NVIDIA's decoder with the driver NVIDIA
// ships, which VA-API does not, and it is the interface the compositor will
// take decoded pictures from. VA-API second, for a driver whose Vulkan does
// not decode VP9.
std::unique_ptr<Vp9Accelerator> Vp9Accelerator::open(std::string& error, VideoApi api)
{
    std::string vulkan_error;
    if (api != VideoApi::Vaapi) {
        if (auto accelerator = open_vulkan_vp9(vulkan_error))
            return accelerator;
        if (api == VideoApi::Vulkan) {
            error = "Vulkan Video: " + vulkan_error;
            return nullptr;
        }
    }
    std::string vaapi_error;
    if (auto accelerator = open_vaapi_vp9(vaapi_error))
        return accelerator;
    error = api == VideoApi::Vaapi ? "VA-API: " + vaapi_error : "Vulkan Video: " + vulkan_error + "; VA-API: " + vaapi_error;
    return nullptr;
}

}
