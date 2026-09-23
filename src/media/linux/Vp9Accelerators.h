#pragma once

// The two ways to the video hardware on Linux, which Vp9Accelerator::open
// tries in the order of preference (media/linux/Vp9Accelerators.cpp).

#include "media/Vp9Accelerator.h"

namespace sashfold::media {

std::unique_ptr<Vp9Accelerator> open_vulkan_vp9(std::string& error);
std::unique_ptr<Vp9Accelerator> open_vaapi_vp9(std::string& error);

}
