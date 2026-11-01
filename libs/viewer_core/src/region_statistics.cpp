#include "viewer_core/region_statistics.h"

#include <opencv2/core.hpp>

#include <limits>

namespace viewer_core {

RegionStatistics regionStatistics(const cv::Mat& image, cv::Rect region) {
    RegionStatistics statistics;
    statistics.region = region & cv::Rect(0, 0, image.cols, image.rows);
    if (statistics.region.empty()) {
        return statistics;
    }
    std::vector<cv::Mat> planes;
    cv::split(image(statistics.region), planes);
    const bool floatingPoint = image.depth() == CV_32F || image.depth() == CV_64F;
    bool anyFinite = false;
    for (const cv::Mat& plane : planes) {
        // NaN fails every comparison, so one "|v| <= largest" test leaves out NaN and ±infinity alike.
        cv::Mat finite;
        if (floatingPoint) {
            const double largest = image.depth() == CV_32F ? std::numeric_limits<float>::max()
                                                           : std::numeric_limits<double>::max();
            cv::compare(cv::abs(plane), largest, finite, cv::CMP_LE);
            const auto counted = static_cast<std::size_t>(cv::countNonZero(finite));
            statistics.nonFinite += plane.total() - counted;
            if (counted == 0) {
                constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
                statistics.channels.push_back({kNan, kNan, kNan, kNan});
                continue;
            }
        }
        anyFinite = true;
        ChannelStatistics channel;
        cv::minMaxLoc(plane, &channel.min, &channel.max, nullptr, nullptr, finite);
        cv::Scalar mean;
        cv::Scalar deviation;
        cv::meanStdDev(plane, mean, deviation, finite); // over n: the population's
        channel.mean = mean[0];
        channel.standardDeviation = deviation[0];
        statistics.channels.push_back(channel);
    }
    if (!anyFinite) {
        statistics.channels.clear();
    }
    return statistics;
}

} // namespace viewer_core
