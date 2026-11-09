#pragma once

// A synthetic document for the outline's tests, and those of the programs that use it (live_sender, the
// Document Lab).

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cstddef>
#include <vector>

namespace test_support {

inline const std::vector<cv::Point> kCardCorners{{100, 80}, {540, 100}, {520, 400}, {120, 380}};

// A light card on a darker desk, `scale` times the 640×480 picture.
inline cv::Mat cardFrame(int scale = 1) {
    cv::Mat frame(480 * scale, 640 * scale, CV_8UC3, cv::Scalar::all(40));
    std::vector<cv::Point> corners;
    for (const cv::Point& corner : kCardCorners) {
        corners.push_back(corner * scale);
    }
    cv::fillConvexPoly(frame, corners, cv::Scalar::all(220));
    return frame;
}

// Each of the card's corners is within `tolerance` of a different found point.
inline bool findsTheCorners(const std::vector<cv::Point2d>& found, int scale, double tolerance) {
    if (found.size() != 4) {
        return false;
    }
    std::vector<bool> used(found.size(), false);
    for (const cv::Point& corner : kCardCorners) {
        bool matched = false;
        for (std::size_t i = 0; i < found.size() && !matched; ++i) {
            if (!used[i] && cv::norm(found[i] - cv::Point2d(corner * scale)) <= tolerance) {
                used[i] = true;
                matched = true;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

} // namespace test_support
