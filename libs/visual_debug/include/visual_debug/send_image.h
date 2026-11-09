#pragma once

#include "visual_debug/image_sender.h"

#include <opencv2/core/mat.hpp>

#include <string_view>

namespace visual_debug {

// The one-line debugging call: sends an image to the viewer at defaultEndpoint() through one process-wide
// Client. Safe from any thread (calls are serialised). Never throws.
SendResult sendImage(const cv::Mat& image, std::string_view name) noexcept;

// The same for one frame of a live channel, such as a camera: the viewer shows it in place of the channel's
// previous frame instead of adding it to the history. Shares sendImage's client.
SendResult sendLive(const cv::Mat& frame, std::string_view channel) noexcept;

// Both again, with a description of the image: the range its values nominally span, and for HDR video its
// transfer and primaries. For example sendLive(frame, "phone", {.valueRange = {0, 1023}}).
SendResult sendImage(const cv::Mat& image, std::string_view name,
                     const ImageDescription& description) noexcept;
SendResult sendLive(const cv::Mat& frame, std::string_view channel,
                    const ImageDescription& description) noexcept;

// Whether a viewer answers at defaultEndpoint(), and the end of a live channel, through the same client.
ViewerState ping() noexcept;
SendResult endLive(std::string_view channel) noexcept;

} // namespace visual_debug
