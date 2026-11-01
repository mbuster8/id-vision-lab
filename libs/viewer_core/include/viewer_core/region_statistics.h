#pragma once

#include <opencv2/core/mat.hpp>

#include <cstddef>
#include <vector>

namespace viewer_core {

struct ChannelStatistics {
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double standardDeviation = 0.0; // of the population: over n values, not n - 1
};

struct RegionStatistics {
    cv::Rect region;                         // clipped to the image
    std::vector<ChannelStatistics> channels; // in OpenCV's order; empty when the region has no finite value
    std::size_t nonFinite = 0;               // values left out (NaN, ±infinity), over all channels
};

// The original values of every channel in `region` of a supported image (2-D, depth 8U to 64F, 1-4
// channels). Non-finite values are left out and counted; a channel with none finite while others have some
// gets NaN statistics.
RegionStatistics regionStatistics(const cv::Mat& image, cv::Rect region);

} // namespace viewer_core
