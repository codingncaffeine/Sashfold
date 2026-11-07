#pragma once

// The two ways to the video hardware on Linux for H.264, which
// H264Accelerator::open tries in the order of preference.

#include "media/H264Accelerator.h"

namespace sashfold::media {

std::unique_ptr<H264Accelerator> open_vulkan_h264(std::string& error);
std::unique_ptr<H264Accelerator> open_vaapi_h264(std::string& error);

}
