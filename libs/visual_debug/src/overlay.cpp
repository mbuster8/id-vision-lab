#include "visual_debug/overlay.h"

#include <array>

namespace visual_debug {

std::vector<cv::Point2d> toPoints(const std::vector<cv::Point>& contour) {
    return {contour.begin(), contour.end()};
}

std::vector<cv::Point2d> toPoints(const std::vector<cv::Point2f>& contour) {
    return {contour.begin(), contour.end()};
}

std::vector<cv::Point2d> toPoints(const cv::RotatedRect& box) {
    std::array<cv::Point2f, 4> corners;
    box.points(corners.data());
    return {corners.begin(), corners.end()};
}

} // namespace visual_debug
