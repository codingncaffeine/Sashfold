#pragma once

// H.264 decoded on the processor (media/H264Software.cpp and the
// H264Software*.cpp stages): what H264Accelerator::open gives where the
// machine has no hardware decoder, or when asked for VideoApi::Software.

#include "media/H264Accelerator.h"

#include <memory>

namespace sashfold::media {

// Table A-1's largest frame (MaxFS of level 6.2, 8192x4320): the most
// macroblocks a conforming stream's picture holds.
constexpr int h264_max_frame_macroblocks = 139264;

// A picture larger than `max_macroblocks` is refused (a fuzzer's harness
// passes a small bound to keep its memory low). `threads`: how many decode
// each picture; 0 for SASHFOLD_H264_THREADS or the machine's cores.
std::unique_ptr<H264Accelerator> open_software_h264(int max_macroblocks = h264_max_frame_macroblocks, int threads = 0);

}
