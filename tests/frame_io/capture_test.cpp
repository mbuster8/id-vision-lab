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
#include <vector>

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

// Deep review F8: a capture whose JSON cannot be written leaves no picture behind, so a retry is not left
// beside an orphan the user was told was not saved. A name of 255 bytes for the PNG makes the JSON's 256,
// over the file systems' limit.
TEST(Capture, FailedJsonLeavesNoPicture) {
    const ScratchFolder folder;
    std::string error;
    const std::string baseName(255 - std::string(" frame 7.png").size(), 'a');

    const std::filesystem::path png =
        frame_io::writeCapture(folder.path(), baseName, frameNumberSix(), {}, error);

    EXPECT_TRUE(png.empty());
    EXPECT_FALSE(error.empty());
    for (const auto& entry : std::filesystem::directory_iterator(folder.path())) {
        ADD_FAILURE() << "left behind: " << entry.path().filename().string().size() << " bytes of name";
    }
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

namespace {

nlohmann::json captureJson(const std::filesystem::path& png) {
    std::filesystem::path json = png;
    json.replace_extension(".json");
    std::ifstream stream(json);
    return nlohmann::json::parse(stream);
}

// Where the processor found the document, as the viewer's overlays describe it, so a capture keeps it.
TEST(Capture, KeepsTheOverlays) {
    const ScratchFolder folder;
    std::string error;
    const frame_io::CaptureInfo info{
        .source = "camera",
        .findings = {"document: found"},
        .overlays = {visual_debug::Polygon{.points = {{1, 2}, {30, 2}, {30, 20}}, .label = "Document"}}};

    const std::filesystem::path png =
        frame_io::writeCapture(folder.path(), "c", frameNumberSix(), info, error);

    ASSERT_FALSE(png.empty()) << error;
    const nlohmann::json json = captureJson(png);
    ASSERT_EQ(json["overlays"].size(), 1U);
    EXPECT_EQ(json["overlays"][0]["shape"], "polygon");
    EXPECT_EQ(json["overlays"][0]["label"], "Document");
    EXPECT_EQ(json["overlays"][0]["points"][1], nlohmann::json::array({30, 2}));
}

// A processor's finding that is not UTF-8 is written with U+FFFD in its place, never thrown on.
TEST(Capture, InvalidUtf8FindingIsReplaced) {
    const ScratchFolder folder;
    std::string error;

    const std::filesystem::path png =
        frame_io::writeCapture(folder.path(), "c", frameNumberSix(), {.findings = {"a\xff"}}, error);

    ASSERT_FALSE(png.empty()) << error;
    EXPECT_EQ(captureJson(png)["findings"][0], "a\xEF\xBF\xBD");
}

// A4: what the caller adds, such as the MRZ a user chose to save, as named objects in the order given.
TEST(Capture, SectionsAreWrittenInOrder) {
    const ScratchFolder folder;
    std::string error;
    frame_io::CaptureInfo info;
    info.sections = {
        {.name = "mrz", .values = {{"line 1", "P<UTO"}, {"surname", "ERIKSSON"}, {"born", "740812"}}},
        {.name = "second", .values = {{"z", "1"}, {"a", "2"}}}};

    const std::filesystem::path png =
        frame_io::writeCapture(folder.path(), "c", frameNumberSix(), info, error);

    ASSERT_FALSE(png.empty()) << error;
    std::filesystem::path json = png;
    json.replace_extension(".json");
    std::ifstream stream(json);
    const nlohmann::ordered_json read = nlohmann::ordered_json::parse(stream);
    ASSERT_TRUE(read.contains("mrz"));
    std::vector<std::string> keys;
    for (const auto& [key, value] : read["mrz"].items()) {
        keys.push_back(key);
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"line 1", "surname", "born"}));
    EXPECT_EQ(read["mrz"]["surname"], "ERIKSSON");
    std::vector<std::string> second;
    for (const auto& [key, value] : read["second"].items()) {
        second.push_back(key);
    }
    EXPECT_EQ(second, (std::vector<std::string>{"z", "a"}));
}

TEST(Capture, NoSectionsNoKeys) {
    const ScratchFolder folder;
    std::string error;
    const std::filesystem::path png = frame_io::writeCapture(folder.path(), "c", frameNumberSix(), {}, error);
    ASSERT_FALSE(png.empty()) << error;
    const nlohmann::json json = captureJson(png);
    EXPECT_EQ(json.size(), 5U); // source, frame, at, tags, findings
}

TEST(Capture, SectionValuesAreUtf8) {
    const ScratchFolder folder;
    std::string error;
    frame_io::CaptureInfo info;
    info.sections = {{.name = "mrz", .values = {{"surname", "ÖSTERGÅRD"}, {"bad", "a\xff"}}}};
    const std::filesystem::path png =
        frame_io::writeCapture(folder.path(), "c", frameNumberSix(), info, error);
    ASSERT_FALSE(png.empty()) << error;
    const nlohmann::json json = captureJson(png);
    EXPECT_EQ(json["mrz"]["surname"], "ÖSTERGÅRD");
    EXPECT_EQ(json["mrz"]["bad"], "a\xEF\xBF\xBD");
}

// A section may not overwrite what the capture already says.
TEST(Capture, ReservedSectionNameIsRefused) {
    const ScratchFolder folder;
    std::string error;
    frame_io::CaptureInfo info;
    info.findings = {"document: found"};
    info.sections = {{.name = "findings", .values = {{"x", "y"}}}};
    const std::filesystem::path png =
        frame_io::writeCapture(folder.path(), "c", frameNumberSix(), info, error);
    EXPECT_TRUE(png.empty());
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(std::filesystem::is_empty(folder.path()));
}

} // namespace
