#include "frame_io/capture.h"

#include "scratch_folder.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace std::chrono_literals;
using test_support::ScratchFolder;

frame_io::Frame frameNumberSix() {
    return {.image = cv::Mat(6, 4, CV_16UC1, cv::Scalar(1234)),
            .number = 6,
            .at = 200ms,
            .tags = {.valueRange = {0, 65535},
                     .transfer = visual_debug::Transfer::Pq,
                     .primaries = {},
                     .overlays = {}}};
}

// The frame on screen as the user numbers it (frame 7, the seventh), at full resolution, with what was known
// about it.
TEST(Capture, WritesPngAndJson) {
    const ScratchFolder folder;
    std::string error;

    const std::filesystem::path png = frame_io::writeCapture(
        folder.path(), "2026-11-09 14.03.22", frameNumberSix(),
        {.source = "clip.mov", .findings = {"document: found", "edges: 4.2 %"}}, error);

    ASSERT_FALSE(png.empty()) << error;
    EXPECT_EQ(png.filename(), "2026-11-09 14.03.22 frame 7.png");
    const cv::Mat read = cv::imread(png.string(), cv::IMREAD_UNCHANGED);
    EXPECT_EQ(cv::norm(read, frameNumberSix().image, cv::NORM_INF), 0.0);
    std::ifstream stream(folder.path() / "2026-11-09 14.03.22 frame 7.json");
    const nlohmann::json json = nlohmann::json::parse(stream);
    EXPECT_EQ(json["source"], "clip.mov");
    EXPECT_EQ(json["frame"], 7);
    EXPECT_EQ(json["at"], 200'000'000);
    EXPECT_EQ(json["tags"]["transfer"], "pq");
    EXPECT_EQ(json["findings"][1], "edges: 4.2 %");
}

TEST(Capture, TakenNameGetsANumber) {
    const ScratchFolder folder;
    std::string error;

    frame_io::writeCapture(folder.path(), "same", frameNumberSix(), {}, error);
    const std::filesystem::path second =
        frame_io::writeCapture(folder.path(), "same", frameNumberSix(), {}, error);

    EXPECT_EQ(second.filename(), "same frame 7 2.png");
    EXPECT_TRUE(std::filesystem::exists(folder.path() / "same frame 7 2.json"));
}

} // namespace
