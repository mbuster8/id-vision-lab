#include "frame_io/video_file.h"

#include <opencv2/core/utils/logger.hpp>

namespace frame_io {

bool openVideoFile(cv::VideoCapture& capture, const std::string& path) {
#if defined(__APPLE__)
    // A file AVFoundation cannot read is not an error yet, so OpenCV's logger is kept quiet. Its AVFoundation
    // code still prints one line of its own ("Couldn't read video stream"), which no setting silences.
    const cv::utils::logging::LogLevel level =
        cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);
    const bool opened = capture.open(path, cv::CAP_AVFOUNDATION);
    cv::utils::logging::setLogLevel(level);
    if (opened) {
        return true;
    }
#endif
    return capture.open(path);
}

} // namespace frame_io
