#pragma once

#include "viewer_core/display_settings.h"

#include <opencv2/core/mat.hpp>

#include <cstddef>

namespace viewer_core {

// Converts a supported image (2-D, depth 8U to 64F, 1-4 channels) to 8-bit RGBA (CV_8UC4) for display.
// 8U values are shown as they are; other depths are min-max normalised to 0-255 across all channels together,
// ignoring NaN and infinities, which are shown as 0. Throws std::invalid_argument for anything else.
cv::Mat toDisplayImage(const cv::Mat& image);

// The same with a display range and colour map. Auto with Grey is exactly toDisplayImage(image). Hold and
// Fixed map settings.range to 0-255 for every depth, 8U included, clipping values outside it; a range whose
// minimum is not below its maximum shows black. A colour map applies to single-channel images only. NaN and
// infinities are black (0, 0, 0, 255) whatever the map.
cv::Mat toDisplayImage(const cv::Mat& image, const DisplaySettings& settings);

// The same at `size`, each side clamped to the image's own and at least 1: for a picture drawn smaller than
// the image, at a fraction of the cost. Values are mapped to 8 bits at full size, with the whole image's
// range for Auto, then shrunk by area averaging, then coloured; so the picture looks as the full one would,
// and a NaN or infinity stays one dark pixel among its neighbours instead of blackening its whole block.
cv::Mat toDisplayImage(const cv::Mat& image, const DisplaySettings& settings, cv::Size size);

// The largest size of the image's shape within `bounds`, never larger than the image, each side at least 1.
cv::Size sizeWithin(cv::Size image, cv::Size bounds);

// The range Auto uses for this image: 0-255 for 8U, otherwise the minimum and maximum of its finite values
// across all channels. Throws std::invalid_argument for an image toDisplayImage cannot show.
DisplayRange autoRange(const cv::Mat& image);

// The bytes toDisplayImage's result takes for this image, known without converting it.
std::size_t displayBytes(const cv::Mat& image);

} // namespace viewer_core
