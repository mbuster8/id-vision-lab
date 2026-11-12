#include "doc_vision/page_geometry.h"

#include <opencv2/imgproc.hpp>
// OpenCV 5 moved getPerspectiveTransform and contourArea into its geometry module.
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace doc_vision {

namespace {

std::vector<cv::Point2f> asFloats(const Quad& quad) { return {quad.begin(), quad.end()}; }

double length(cv::Point2d from, cv::Point2d to) { return cv::norm(to - from); }

} // namespace

Quad orderedCorners(const std::array<cv::Point2d, 4>& points) {
    // Clockwise in the image (y down): ascending angle about the centroid.
    const cv::Point2d centre = (points[0] + points[1] + points[2] + points[3]) * 0.25;
    std::array<cv::Point2d, 4> ring = points;
    std::sort(ring.begin(), ring.end(), [&](cv::Point2d a, cv::Point2d b) {
        return std::atan2(a.y - centre.y, a.x - centre.x) < std::atan2(b.y - centre.y, b.x - centre.x);
    });

    // The two pairs of opposite sides, by their mean length; a side of the longer pair (either, for a square)
    // may start the page.
    std::array<double, 4> side{};
    for (int i = 0; i < 4; ++i) {
        side[i] = length(ring[i], ring[(i + 1) % 4]);
    }
    const double first = (side[0] + side[2]) / 2;
    const double second = (side[1] + side[3]) / 2;
    const double tolerance = 1e-9 * std::max(first, second);

    int start = -1;
    cv::Point2d best;
    for (int i = 0; i < 4; ++i) {
        const double pair = i % 2 == 0 ? first : second;
        const double other = i % 2 == 0 ? second : first;
        if (pair + tolerance < other) {
            continue;
        }
        const cv::Point2d middle = (ring[i] + ring[(i + 1) % 4]) * 0.5;
        const bool higher = start < 0 || middle.y < best.y - 1e-9 ||
                            (std::abs(middle.y - best.y) <= 1e-9 && middle.x < best.x);
        if (higher) {
            start = i;
            best = middle;
        }
    }
    return {ring[start], ring[(start + 1) % 4], ring[(start + 2) % 4], ring[(start + 3) % 4]};
}

Quad alignTo(const Quad& next, const Quad& previous) {
    const auto cost = [&](int shift) {
        double sum = 0;
        for (int i = 0; i < 4; ++i) {
            sum += length(next[(i + shift) % 4], previous[i]);
        }
        return sum;
    };
    if (cost(2) < cost(0)) {
        return {next[2], next[3], next[0], next[1]};
    }
    return next;
}

cv::Mat flatten(const cv::Mat& image, const Quad& quad, cv::Size size) {
    const float right = static_cast<float>(size.width - 1);
    const float bottom = static_cast<float>(size.height - 1);
    const std::vector<cv::Point2f> target{{0, 0}, {right, 0}, {right, bottom}, {0, bottom}};
    cv::Mat flat;
    cv::warpPerspective(image, flat, cv::getPerspectiveTransform(asFloats(quad), target), size,
                        cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    return flat;
}

Quad pageToFrame(const Quad& onPage, const Quad& pageQuad, cv::Size pageSize) {
    // getPerspectiveTransform, as flatten uses: findHomography is calib3d's in OpenCV 4, which this library
    // does not link.
    const float right = static_cast<float>(pageSize.width - 1);
    const float bottom = static_cast<float>(pageSize.height - 1);
    const std::vector<cv::Point2f> page{{0, 0}, {right, 0}, {right, bottom}, {0, bottom}};
    const cv::Mat homography = cv::getPerspectiveTransform(page, asFloats(pageQuad));
    std::vector<cv::Point2d> points(onPage.begin(), onPage.end());
    std::vector<cv::Point2d> mapped;
    cv::perspectiveTransform(points, mapped, homography);
    return {mapped[0], mapped[1], mapped[2], mapped[3]};
}

Quad unturned(const Quad& onTurnedPage, cv::Size pageSize) {
    Quad out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = {pageSize.width - 1 - onTurnedPage[i].x, pageSize.height - 1 - onTurnedPage[i].y};
    }
    return out;
}

double fillShare(const Quad& quad, cv::Size frame) {
    return cv::contourArea(asFloats(quad)) / (static_cast<double>(frame.width) * frame.height);
}

double longerSideShare(const Quad& quad, cv::Size frame) {
    double longest = 0;
    for (int i = 0; i < 4; ++i) {
        longest = std::max(longest, length(quad[i], quad[(i + 1) % 4]));
    }
    return longest / std::max(frame.width, frame.height);
}

double extentShare(const Quad& quad, cv::Size frame) {
    double left = quad[0].x, right = quad[0].x, top = quad[0].y, bottom = quad[0].y;
    for (const cv::Point2d& corner : quad) {
        left = std::min(left, corner.x);
        right = std::max(right, corner.x);
        top = std::min(top, corner.y);
        bottom = std::max(bottom, corner.y);
    }
    return std::max((right - left) / frame.width, (bottom - top) / frame.height);
}

double edgeMarginShare(const Quad& quad, cv::Size frame) {
    double margin = std::numeric_limits<double>::max();
    for (const cv::Point2d& corner : quad) {
        margin = std::min(
            {margin, corner.x, corner.y, frame.width - 1.0 - corner.x, frame.height - 1.0 - corner.y});
    }
    return margin / std::min(frame.width, frame.height);
}

double tiltDegrees(const Quad& quad, cv::Size frame, double horizontalFovDegrees) {
    // The homography from the page's plane (millimetres) to the image is K · [r1 r2 t] up to scale; with K
    // assumed, its first two columns give the plane's axes, and their cross product its normal.
    const std::vector<cv::Point2f> plane{{0, 0},
                                         {static_cast<float>(kTd3WidthMm), 0},
                                         {static_cast<float>(kTd3WidthMm), static_cast<float>(kTd3HeightMm)},
                                         {0, static_cast<float>(kTd3HeightMm)}};
    const cv::Mat h = cv::getPerspectiveTransform(plane, asFloats(quad));
    const double f = (frame.width / 2.0) / std::tan(horizontalFovDegrees / 2.0 * std::numbers::pi / 180.0);
    const cv::Matx33d inverseK(1 / f, 0, -frame.width / (2 * f), 0, 1 / f, -frame.height / (2 * f), 0, 0, 1);
    const cv::Matx33d m = inverseK * cv::Matx33d(h.ptr<double>());
    cv::Vec3d r1(m(0, 0), m(1, 0), m(2, 0));
    cv::Vec3d r2(m(0, 1), m(1, 1), m(2, 1));
    r1 /= cv::norm(r1);
    r2 /= cv::norm(r2);
    const cv::Vec3d normal = r1.cross(r2);
    const double cosine = std::min(1.0, std::abs(normal[2]) / cv::norm(normal));
    return std::acos(cosine) * 180.0 / std::numbers::pi;
}

} // namespace doc_vision
