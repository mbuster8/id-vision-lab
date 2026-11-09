#pragma once

#include "visual_debug/image_description.h"

#include <opencv2/core/mat.hpp>

#include <chrono>
#include <cstdint>

namespace frame_io {

// One frame of a source's run: a camera's, a video's, a folder's or a recording's.
struct Frame {
    cv::Mat image;                 // 8U or 16U, 1, 3 or 4 channels, BGR order; never changed once made
    std::uint64_t number = 0;      // from 0 in the run
    std::chrono::nanoseconds at{}; // from the run's first frame, never decreasing
    visual_debug::ImageDescription tags{}; // value range, transfer and primaries; never overlays
};

// What a frame may hold: what PNG can store losslessly and the processors read. Not empty, 8- or 16-bit, with
// 1, 3 or 4 channels.
bool isAcceptedImage(const cv::Mat& image);

// The tags a frame of `image` carries: `tags`, and for a 16-bit image without a value range the nominal 0 to
// 65535, so Auto does not stretch each frame from its own extremes.
visual_debug::ImageDescription tagsFor(const cv::Mat& image, visual_debug::ImageDescription tags);

} // namespace frame_io
