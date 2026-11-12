#include "doc_vision/mrz_reader.h"

#include "procedural_page.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

using doc_vision::MrzReading;

std::filesystem::path modelAt(const char* utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8)));
}
std::filesystem::path detectionModel() { return modelAt(ID_VISION_LAB_MRZ_DETECTION_MODEL); }
std::filesystem::path recognitionModel() { return modelAt(ID_VISION_LAB_MRZ_RECOGNITION_MODEL); }

std::unique_ptr<doc_vision::MrzScanner> openScanner() {
    std::string error;
    auto scanner = doc_vision::MrzScanner::open(detectionModel(), recognitionModel(), error);
    EXPECT_TRUE(scanner) << error;
    return scanner;
}

cv::Point2d centreOf(const doc_vision::Quad& quad) { return (quad[0] + quad[1] + quad[2] + quad[3]) * 0.25; }

// The recogniser's output as the model gives it: [1, steps, classes], one-hot at each step's token.
doc_vision::Tensor tokens(const std::vector<int>& sequence) {
    constexpr int kClasses = 40;
    doc_vision::Tensor tensor;
    tensor.shape = {1, static_cast<std::int64_t>(sequence.size()), kClasses};
    tensor.data.assign(sequence.size() * kClasses, 0.0f);
    for (std::size_t step = 0; step < sequence.size(); ++step) {
        tensor.data[step * kClasses + static_cast<std::size_t>(sequence[step])] = 1.0f;
    }
    return tensor;
}

TEST(MrzTokens, DecodesLinesAndStopsAtEos) {
    // 3 'A', 0 PAD, 2 SEP, 29 '0', 39 '<', 1 EOS, 4 'B' (after the end).
    EXPECT_EQ(doc_vision::decodeMrzTokens(tokens({3, 0, 2, 29, 39, 1, 4})),
              (std::vector<std::string>{"A", "0<"}));
    doc_vision::Tensor flat;
    flat.shape = {1, 6};
    flat.data.assign(6, 0.0f);
    EXPECT_TRUE(doc_vision::decodeMrzTokens(flat).empty());
}

TEST(MrzScanner, MissingModelIsAModelFailure) {
    std::string error;
    doc_vision::OpenFailure failure = doc_vision::OpenFailure::None;
    EXPECT_FALSE(doc_vision::MrzScanner::open("no-such-detection.onnx", recognitionModel(), error, &failure));
    EXPECT_EQ(failure, doc_vision::OpenFailure::Model);
    EXPECT_FALSE(error.empty());
}

// Measured on 12 November: the band at y 765–848, x 50–1183; Hershey glyphs misread 2–7 characters a line,
// and ONNX Runtime's kernels differ by platform, so no text is asserted.
TEST(MrzScanner, FindsTheSpecimensBand) {
    const auto scanner = openScanner();
    ASSERT_TRUE(scanner);
    std::string error;
    const MrzReading reading = scanner->read(test_support::specimenPage(), error);
    EXPECT_TRUE(error.empty()) << error;
    ASSERT_TRUE(reading.band);
    EXPECT_GT(centreOf(*reading.band).y, 0.8 * 880);
    EXPECT_GT(reading.widthShare * 1250, 0.85 * 1250);
    EXPECT_GE(reading.peak, doc_vision::kBandPeak);
    ASSERT_EQ(reading.lines.size(), 2u);
    EXPECT_EQ(reading.lines[0].size(), 44u);
    EXPECT_EQ(reading.lines[1].size(), 44u);
}

TEST(MrzScanner, BlankPageHasNoBand) {
    const auto scanner = openScanner();
    ASSERT_TRUE(scanner);
    std::string error;
    const MrzReading reading = scanner->read(cv::Mat(880, 1250, CV_8UC3, cv::Scalar(225, 232, 236)), error);
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_FALSE(reading.band);
    EXPECT_TRUE(reading.lines.empty());
}

TEST(MrzScanner, CancelledReadFailsAtOnce) {
    const auto scanner = openScanner();
    ASSERT_TRUE(scanner);
    scanner->cancel();
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    const MrzReading reading = scanner->read(test_support::specimenPage(), error);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(50));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(reading.lines.empty());
}

// A reader that finds "the band" where the page's one white pixel is: a band at that row, across the page.
class MarkerReader final : public doc_vision::MrzReader {
public:
    MrzReading read(const cv::Mat& page, std::string&) override {
        ++reads;
        cv::Point at;
        cv::Mat grey;
        cv::extractChannel(page, grey, 0);
        cv::minMaxLoc(grey, nullptr, nullptr, nullptr, &at);
        MrzReading reading;
        const double y = at.y;
        reading.band = doc_vision::Quad{{{50, y - 10}, {1200, y - 10}, {1200, y + 10}, {50, y + 10}}};
        reading.lines = {"line"};
        return reading;
    }
    int reads = 0;
};

cv::Mat pageWithMarker(int y) {
    cv::Mat page(880, 1250, CV_8UC3, cv::Scalar::all(100));
    page.at<cv::Vec3b>(y, 600) = cv::Vec3b(255, 255, 255);
    return page;
}

TEST(MrzReadUpright, TurnsAnUpsideDownPage) {
    MarkerReader reader;
    bool turned = false;
    std::string error;
    const MrzReading top = doc_vision::readUpright(reader, pageWithMarker(100), false, turned, error);
    EXPECT_EQ(reader.reads, 2);
    EXPECT_TRUE(turned);
    ASSERT_TRUE(top.band);
    EXPECT_NEAR(centreOf(*top.band).y, 879 - 100, 1.0);

    MarkerReader bottomReader;
    const MrzReading bottom =
        doc_vision::readUpright(bottomReader, pageWithMarker(800), false, turned, error);
    EXPECT_EQ(bottomReader.reads, 1);
    EXPECT_FALSE(turned);
    ASSERT_TRUE(bottom.band);
    EXPECT_NEAR(centreOf(*bottom.band).y, 800, 1.0);

    MarkerReader firstTurned;
    doc_vision::readUpright(firstTurned, pageWithMarker(100), true, turned, error);
    EXPECT_EQ(firstTurned.reads, 1);
    EXPECT_TRUE(turned);
    EXPECT_TRUE(error.empty());
}

TEST(MrzReadUpright, RealSpecimenTurned) {
    const auto scanner = openScanner();
    ASSERT_TRUE(scanner);
    cv::Mat turnedPage;
    cv::rotate(test_support::specimenPage(), turnedPage, cv::ROTATE_180);
    bool turned = false;
    std::string error;
    const MrzReading reading = doc_vision::readUpright(*scanner, turnedPage, false, turned, error);
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_TRUE(turned);
    ASSERT_EQ(reading.lines.size(), 2u);
    EXPECT_EQ(reading.lines[0].size(), 44u);
    EXPECT_EQ(reading.lines[1].size(), 44u);
}

} // namespace
