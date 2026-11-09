#include "frame_io/frame.h"

namespace frame_io {

bool isAcceptedImage(const cv::Mat& image) {
    const int channels = image.channels();
    return !image.empty() && (image.depth() == CV_8U || image.depth() == CV_16U) &&
           (channels == 1 || channels == 3 || channels == 4);
}

visual_debug::ImageDescription tagsFor(const cv::Mat& image, visual_debug::ImageDescription tags) {
    if (image.depth() == CV_16U && tags.valueRange.empty()) {
        tags.valueRange = {0.0, 65535.0};
    }
    return tags;
}

} // namespace frame_io
