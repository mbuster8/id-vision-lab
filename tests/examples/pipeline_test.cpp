#include "pipeline.h"

#include "visual_debug/wire_format.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

// Sixteen random hexadecimal digits, ASCII, for a name no other run uses.
std::u8string randomSuffix() {
    std::random_device device;
    const std::uint64_t value = (std::uint64_t{device()} << 32U) | device();
    std::u8string digits;
    for (int shift = 60; shift >= 0; shift -= 4) {
        digits.push_back(u8"0123456789abcdef"[(value >> shift) & 0xFU]);
    }
    return digits;
}

TEST(Pipeline, TestPatternIsColourAndSized) {
    const cv::Mat pattern = pipeline_demo::makeTestPattern();

    EXPECT_EQ(pattern.type(), CV_8UC3);
    EXPECT_EQ(pattern.size(), cv::Size(640, 480));
}

TEST(Pipeline, StagesProduceTheDocumentedTypes) {
    const std::vector<std::pair<std::string, int>> expected{{"Original", CV_8UC3},
                                                            {"With alpha", CV_8UC4},
                                                            {"Greyscale", CV_8UC1},
                                                            {"Gaussian blur", CV_8UC1},
                                                            {"Sobel X", CV_16SC1},
                                                            {"Canny edges", CV_8UC1},
                                                            {"Distance transform", CV_32FC1},
                                                            {"Laplacian", CV_64FC1},
                                                            {"Centre crop", CV_8UC3},
                                                            {"Multi-channel 16-bit", CV_16UC3}};
    const cv::Mat original = pipeline_demo::makeTestPattern();

    const auto stages = pipeline_demo::buildPipeline();

    ASSERT_EQ(stages.size(), expected.size());
    for (std::size_t i = 0; i < stages.size(); ++i) {
        SCOPED_TRACE(expected[i].first);
        EXPECT_EQ(stages[i].name, expected[i].first);
        const cv::Mat output = stages[i].run(original);
        EXPECT_EQ(output.type(), expected[i].second);
        EXPECT_TRUE(visual_debug::isSupportedImage(output)); // every stage can be sent
    }
}

TEST(Pipeline, CentreCropIsANonContinuousRegionOfInterest) {
    const cv::Mat original = pipeline_demo::makeTestPattern();

    const cv::Mat crop = pipeline_demo::buildPipeline().at(8).run(original);

    EXPECT_FALSE(crop.isContinuous());
    EXPECT_EQ(crop.size(), cv::Size(320, 240));
}

TEST(Pipeline, StagesLeaveTheOriginalUnchanged) {
    const cv::Mat original = pipeline_demo::makeTestPattern();
    const cv::Mat copy = original.clone();

    for (const auto& stage : pipeline_demo::buildPipeline()) {
        (void)stage.run(original);
    }

    EXPECT_EQ(cv::norm(original, copy, cv::NORM_INF), 0.0);
}

TEST(Pipeline, WorksOnGreyscaleInput) {
    const cv::Mat grey(48, 64, CV_8UC1, cv::Scalar(100));

    for (const auto& stage : pipeline_demo::buildPipeline()) {
        SCOPED_TRACE(stage.name);
        EXPECT_TRUE(visual_debug::isSupportedImage(stage.run(grey)));
    }
}

// The path arrives as UTF-8, as argv does on macOS and Linux, and on Windows through the UTF-8 code page in
// the program's manifest.
TEST(Pipeline, ReadsAnImageFromANonAsciiPath) {
    // A folder of its own, so two runs at once (two presets or two clones) never remove each other's file.
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / (u8"pipeline_test_ž_日本_" + randomSuffix());
    ASSERT_TRUE(std::filesystem::create_directory(folder));
    const std::filesystem::path file = folder / u8"obrázok č.1.png";
    std::vector<unsigned char> png;
    ASSERT_TRUE(cv::imencode(".png", cv::Mat(48, 64, CV_8UC3, cv::Scalar(10, 20, 30)), png));
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }
    const std::u8string utf8 = file.u8string();

    const auto image = pipeline_demo::readImage(std::string(utf8.begin(), utf8.end()));

    std::filesystem::remove_all(folder);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->size(), cv::Size(64, 48));
    EXPECT_EQ(image->at<cv::Vec3b>(0, 0), cv::Vec3b(10, 20, 30));
}

TEST(Pipeline, ReadImageRefusesAMissingFile) {
    EXPECT_FALSE(pipeline_demo::readImage("no such file.png").has_value());
}

} // namespace
