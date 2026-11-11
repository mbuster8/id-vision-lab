#include "doc_vision/page_detector.h"

#include "procedural_page.h"

#include <gtest/gtest.h>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path lc100Model() {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(ID_VISION_LAB_LC100_MODEL)));
}

// A 128 × 128 heatmap with a Gaussian blob (σ 2 cells, peak 0.9) centred at cell (x, y).
cv::Mat blobAt(int x, int y) {
    cv::Mat heat(128, 128, CV_32F);
    for (int row = 0; row < heat.rows; ++row) {
        for (int column = 0; column < heat.cols; ++column) {
            const double d2 = (column - x) * (column - x) + (row - y) * (row - y);
            heat.at<float>(row, column) = static_cast<float>(0.9 * std::exp(-d2 / 8.0));
        }
    }
    return heat;
}

TEST(PageDetector, CornersFromHeatmapsAtCellCentres) {
    const std::vector<cv::Mat> heat{blobAt(16, 20), blobAt(110, 20), blobAt(110, 100), blobAt(16, 100)};
    const auto corners = doc_vision::cornersFromHeatmaps(heat, {1280, 720});
    ASSERT_TRUE(corners.has_value());
    const cv::Point2d expected[4] = {
        {164.5, 114.8125}, {1104.5, 114.8125}, {1104.5, 564.8125}, {164.5, 564.8125}};
    for (int i = 0; i < 4; ++i) {
        EXPECT_NEAR((*corners)[i].x, expected[i].x, 0.5) << i;
        EXPECT_NEAR((*corners)[i].y, expected[i].y, 0.5) << i;
    }
}

TEST(PageDetector, NoBlobNoCorners) {
    std::vector<cv::Mat> heat{blobAt(16, 20), blobAt(110, 20), blobAt(110, 100), blobAt(16, 100)};
    heat[2] = cv::Mat(128, 128, CV_32F, cv::Scalar(0.29));
    EXPECT_FALSE(doc_vision::cornersFromHeatmaps(heat, {1280, 720}).has_value());
}

TEST(PageDetector, ToBgr8) {
    cv::Mat twelveBit(1, 2, CV_16UC1);
    twelveBit.at<std::uint16_t>(0, 0) = 4095;
    twelveBit.at<std::uint16_t>(0, 1) = 2048;
    const cv::Mat scaled = doc_vision::toBgr8(twelveBit, 0, 4095);
    ASSERT_EQ(scaled.type(), CV_8UC3);
    EXPECT_EQ(scaled.at<cv::Vec3b>(0, 0), cv::Vec3b(255, 255, 255));
    EXPECT_EQ(scaled.at<cv::Vec3b>(0, 1), cv::Vec3b(128, 128, 128));

    cv::Mat noRange(1, 2, CV_16UC3);
    noRange.at<cv::Vec3w>(0, 0) = cv::Vec3w(0xABCD, 0xABCD, 0xABCD);
    noRange.at<cv::Vec3w>(0, 1) = cv::Vec3w(65535, 65535, 65535);
    const cv::Mat byTop = doc_vision::toBgr8(noRange);
    EXPECT_EQ(byTop.at<cv::Vec3b>(0, 0), cv::Vec3b(171, 171, 171));
    EXPECT_EQ(byTop.at<cv::Vec3b>(0, 1), cv::Vec3b(255, 255, 255));

    const cv::Mat grey(1, 1, CV_8UC1, cv::Scalar(77));
    EXPECT_EQ(doc_vision::toBgr8(grey).at<cv::Vec3b>(0, 0), cv::Vec3b(77, 77, 77));

    const cv::Mat bgra(1, 1, CV_8UC4, cv::Scalar(1, 2, 3, 4));
    EXPECT_EQ(doc_vision::toBgr8(bgra).at<cv::Vec3b>(0, 0), cv::Vec3b(1, 2, 3));
}

// No exception leaves doc_vision: an empty image, or one of two channels, is no picture rather than a throw.
TEST(PageDetector, ToBgr8OfNothingIsEmpty) {
    EXPECT_TRUE(doc_vision::toBgr8(cv::Mat()).empty());
    EXPECT_TRUE(doc_vision::toBgr8(cv::Mat(2, 2, CV_8UC2, cv::Scalar::all(9))).empty());
}

TEST(PageDetector, MissingModelIsAnError) {
    std::string error;
    EXPECT_EQ(doc_vision::PageDetector::open("missing.onnx", error), nullptr);
    EXPECT_NE(error.find("missing.onnx"), std::string::npos) << error;
}

// The spike's measured post-processing (docsaid.cpp): each heatmap scaled up to the frame before the
// threshold.
std::vector<cv::Point2d> upscaledCorners(const std::vector<cv::Mat>& heat, cv::Size frame) {
    std::vector<cv::Point2d> corners;
    for (const cv::Mat& channel : heat) {
        cv::Mat full;
        cv::resize(channel, full, frame, 0, 0, cv::INTER_LINEAR);
        cv::threshold(full, full, 0.3, 0, cv::THRESH_TOZERO);
        cv::Mat u8, binary;
        full.convertTo(u8, CV_8U, 255.0);
        cv::threshold(u8, binary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        const auto largest =
            std::max_element(contours.begin(), contours.end(), [](const auto& a, const auto& b) {
                return cv::contourArea(a) < cv::contourArea(b);
            });
        const cv::Moments m = cv::moments(*largest);
        corners.emplace_back(m.m10 / m.m00, m.m01 / m.m00);
    }
    return corners;
}

TEST(PageDetector, FindsTheProceduralPage) {
    std::string error;
    auto detector = doc_vision::PageDetector::open(lc100Model(), error);
    ASSERT_NE(detector, nullptr) << error;
    const cv::Size size(1280, 720);
    const cv::Mat frame =
        test_support::pageInFrame(test_support::proceduralPage(), test_support::kPageQuad, size);

    const auto found = detector->find(frame, error);
    ASSERT_TRUE(found.has_value()) << error;
    EXPECT_GT(test_support::quadIoU(found->quad, test_support::kPageQuad), 0.9);

    // The same corners as the spike's upscaled post-processing, within 0.5 % of the frame's diagonal.
    const std::vector<cv::Mat> heat = detector->heatmaps(frame, error);
    ASSERT_EQ(heat.size(), 4u) << error;
    const auto atHeatmap = doc_vision::cornersFromHeatmaps(heat, size);
    ASSERT_TRUE(atHeatmap.has_value());
    const std::vector<cv::Point2d> upscaled = upscaledCorners(heat, size);
    const double tolerance = 0.005 * std::hypot(size.width, size.height);
    for (int i = 0; i < 4; ++i) {
        EXPECT_LT(cv::norm((*atHeatmap)[i] - upscaled[i]), tolerance) << i;
    }
}

TEST(PageDetector, LooksAwayFromAnEmptyDesk) {
    std::string error;
    auto detector = doc_vision::PageDetector::open(lc100Model(), error);
    ASSERT_NE(detector, nullptr) << error;
    const cv::Mat desk(720, 1280, CV_8UC3, test_support::kDesk);
    const auto found = detector->find(desk, error);
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_FALSE(found.has_value()) << "weakest peak " << (found ? found->weakestPeak : 0.0);
}

} // namespace
