#include "lab_core/document_outline.h"

#include <opencv2/imgproc.hpp>
// OpenCV 5 moved the contour shape functions (approxPolyDP, arcLength, contourArea, isContourConvex) from
// imgproc into the geometry module; OpenCV 4 has no such header.
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace lab_core {
namespace {

// An 8- or 16-bit greyscale, BGR or BGRA frame as 8-bit grey.
cv::Mat greyOf(const cv::Mat& frame) {
    cv::Mat eightBit = frame;
    if (frame.depth() == CV_16U) {
        // The top 8 bits: the offset makes convertTo's rounding a truncation, so 257 · v gives back v
        // exactly.
        frame.convertTo(eightBit, CV_8U, 1.0 / 256.0, -127.5 / 256.0);
    }
    cv::Mat grey = eightBit;
    if (eightBit.channels() == 3) {
        cv::cvtColor(eightBit, grey, cv::COLOR_BGR2GRAY);
    } else if (eightBit.channels() == 4) {
        cv::cvtColor(eightBit, grey, cv::COLOR_BGRA2GRAY);
    }
    return grey;
}

// Four corners for a convex hull: the finest polygon approximation that has exactly four, if any does. A hull
// bulged by a thumb or a lifted page corner needs a coarser one than a clean card.
std::optional<std::vector<cv::Point>> fourCornersOf(const std::vector<cv::Point>& hull) {
    const double perimeter = cv::arcLength(hull, true);
    for (int percent = 2; percent <= 10; ++percent) {
        std::vector<cv::Point> corners;
        cv::approxPolyDP(hull, corners, percent / 100.0 * perimeter, true);
        if (corners.size() == 4) {
            return corners;
        }
        if (corners.size() < 4) {
            break;
        }
    }
    return std::nullopt;
}

// A document seen at an angle still has four clear corners and no tiny side; and an outline counts only when
// the whole document is in view, not cut by the frame's edge.
bool plausibleDocument(const std::vector<cv::Point>& corners, cv::Size frame) {
    constexpr double kMinimumAngle = 45.0;
    constexpr double kMaximumAngle = 135.0;
    constexpr double kMinimumSideShare = 0.2;
    constexpr int kBorder = 2;
    double longest = 0.0;
    double shortest = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const cv::Point& corner = corners[i];
        if (corner.x < kBorder || corner.y < kBorder || corner.x >= frame.width - kBorder ||
            corner.y >= frame.height - kBorder) {
            return false;
        }
        const cv::Point2d previous = corners[(i + corners.size() - 1) % corners.size()] - corner;
        const cv::Point2d next = corners[(i + 1) % corners.size()] - corner;
        const double side = cv::norm(next);
        longest = std::max(longest, side);
        shortest = std::min(shortest, side);
        const double cosine = previous.dot(next) / (cv::norm(previous) * side);
        const double angle = std::acos(std::clamp(cosine, -1.0, 1.0)) * 180.0 / CV_PI;
        if (angle < kMinimumAngle || angle > kMaximumAngle) {
            return false;
        }
    }
    return shortest >= kMinimumSideShare * longest;
}

} // namespace

cv::Mat edgesOf(const cv::Mat& frame) {
    cv::Mat edges;
    cv::Canny(greyOf(frame), edges, 50, 150);
    return edges;
}

std::optional<std::vector<cv::Point2d>> documentOutline(const cv::Mat& frame) {
    constexpr int kLongestSide = 640;
    constexpr double kMinimumShare = 0.10;
    constexpr double kMinimumFill = 0.9;
    constexpr double kCannyLow = 30;
    constexpr double kCannyHigh = 90;
    if (frame.empty()) {
        return std::nullopt;
    }
    const double scale = std::min(1.0, static_cast<double>(kLongestSide) / std::max(frame.cols, frame.rows));
    cv::Mat small = frame;
    if (scale < 1.0) {
        cv::resize(frame, small, cv::Size(), scale, scale, cv::INTER_AREA);
    }
    const cv::Mat grey = greyOf(small);
    cv::Mat blurred;
    // Softens the print and the guilloche patterns of a document's page, which would otherwise break its edge
    // into the text's.
    cv::GaussianBlur(grey, blurred, cv::Size(5, 5), 0);
    cv::Mat edges;
    cv::Canny(blurred, edges, kCannyLow, kCannyHigh);
    // Bridges the gaps along an edge that is faint in places, such as a light page on a light desk.
    cv::morphologyEx(edges, edges, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)));
    std::vector<std::vector<cv::Point>> contours;
    // Every contour, inner ones included: a card on a cluttered desk is rarely the outermost.
    cv::findContours(edges, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    const double minimumArea = kMinimumShare * small.cols * small.rows;
    const double frameMean = cv::mean(grey)[0];
    std::vector<cv::Point> best;
    double bestArea = 0.0;
    for (const std::vector<cv::Point>& contour : contours) {
        std::vector<cv::Point> hull;
        cv::convexHull(contour, hull);
        const double hullArea = cv::contourArea(hull);
        if (hullArea < minimumArea || hullArea <= bestArea) {
            continue;
        }
        const auto corners = fourCornersOf(hull);
        if (!corners || !plausibleDocument(*corners, small.size())) {
            continue;
        }
        // The four corners must account for the hull, as a document's do: a hand or a cable is no
        // quadrilateral.
        const double area = cv::contourArea(*corners);
        if (std::min(area, hullArea) < kMinimumFill * std::max(area, hullArea)) {
            continue;
        }
        // A page is lighter than what it lies on, or a keyboard's keys would make an outline too.
        cv::Mat inside = cv::Mat::zeros(grey.size(), CV_8U);
        cv::fillConvexPoly(inside, *corners, cv::Scalar(255));
        if (cv::mean(grey, inside)[0] < frameMean) {
            continue;
        }
        best = *corners;
        bestArea = area;
    }
    if (best.empty()) {
        return std::nullopt;
    }
    // A pixel of the small copy covers 1/scale frame pixels: its centre, x + 0.5, maps to (x + 0.5) / scale
    // in the frame's coordinates, where a pixel's centre is at + 0.5 again.
    std::vector<cv::Point2d> points;
    for (const cv::Point& corner : best) {
        points.emplace_back((corner.x + 0.5) / scale - 0.5, (corner.y + 0.5) / scale - 0.5);
    }
    return points;
}

std::optional<std::vector<cv::Point2d>> OutlineHold::next(std::optional<std::vector<cv::Point2d>> found) {
    if (found) {
        m_last = std::move(found);
        m_missed = 0;
    } else if (m_last && ++m_missed > kOutlineHoldFrames) {
        m_last.reset();
    }
    return m_last;
}

} // namespace lab_core
