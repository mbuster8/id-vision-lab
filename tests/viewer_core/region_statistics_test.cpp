#include "viewer_core/region_statistics.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <limits>
#include <vector>

namespace {

using viewer_core::regionStatistics;

cv::Mat filled(int rows, int cols, int type, const std::vector<double>& values) {
    cv::Mat image(rows, cols, type);
    cv::Mat flat = image.reshape(1, 1);
    for (int i = 0; i < flat.cols; ++i) {
        flat.col(i).setTo(values[static_cast<std::size_t>(i)]);
    }
    return image;
}

TEST(RegionStatistics, KnownValues) {
    const auto statistics = regionStatistics(filled(2, 2, CV_8UC1, {1, 2, 3, 4}), cv::Rect(0, 0, 2, 2));

    ASSERT_EQ(statistics.channels.size(), 1u);
    const auto& channel = statistics.channels.front();
    EXPECT_EQ(channel.min, 1.0);
    EXPECT_EQ(channel.max, 4.0);
    EXPECT_DOUBLE_EQ(channel.mean, 2.5);
    EXPECT_NEAR(channel.standardDeviation, 1.118, 0.001); // population: over n, not n - 1
    EXPECT_EQ(statistics.nonFinite, 0u);
}

TEST(RegionStatistics, PerChannel) {
    const cv::Mat image(3, 4, CV_8UC3, cv::Scalar(10, 20, 30));

    const auto statistics = regionStatistics(image, cv::Rect(1, 1, 2, 2));

    ASSERT_EQ(statistics.channels.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(statistics.channels[i].mean, 10.0 * static_cast<double>(i + 1)) << i;
        EXPECT_EQ(statistics.channels[i].standardDeviation, 0.0) << i;
    }
}

TEST(RegionStatistics, ClippedToTheImage) {
    const cv::Mat image(4, 4, CV_16UC1, cv::Scalar(7));

    const auto statistics = regionStatistics(image, cv::Rect(-5, -5, 10, 10));

    EXPECT_EQ(statistics.region, cv::Rect(0, 0, 4, 4)); // (-5, -5) to (5, 5), within the 4×4 image
    ASSERT_EQ(statistics.channels.size(), 1u);
    EXPECT_EQ(statistics.channels.front().mean, 7.0);
}

TEST(RegionStatistics, EmptyRegion) {
    const auto statistics = regionStatistics(cv::Mat(4, 4, CV_8UC1, cv::Scalar(1)), cv::Rect(10, 10, 3, 3));

    EXPECT_TRUE(statistics.region.empty());
    EXPECT_TRUE(statistics.channels.empty());
}

TEST(RegionStatistics, NonFiniteLeftOut) {
    constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
    constexpr double kInfinity = std::numeric_limits<double>::infinity();

    const auto statistics =
        regionStatistics(filled(1, 4, CV_32FC1, {1, kNan, 3, kInfinity}), cv::Rect(0, 0, 4, 1));

    ASSERT_EQ(statistics.channels.size(), 1u);
    EXPECT_EQ(statistics.channels.front().mean, 2.0);
    EXPECT_EQ(statistics.channels.front().min, 1.0);
    EXPECT_EQ(statistics.channels.front().max, 3.0);
    EXPECT_EQ(statistics.nonFinite, 2u);
}

TEST(RegionStatistics, NothingFiniteHasNoChannels) {
    constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

    const auto statistics = regionStatistics(filled(1, 2, CV_64FC1, {kNan, kNan}), cv::Rect(0, 0, 2, 1));

    EXPECT_TRUE(statistics.channels.empty());
    EXPECT_EQ(statistics.nonFinite, 2u);
}

} // namespace
