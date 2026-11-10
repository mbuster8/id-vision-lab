#include "lab_core/processors.h"

#include "card_frame.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <array>
#include <charconv>
#include <string>
#include <variant>

namespace {

using lab_core::ProcessorKind;

frame_io::Frame frameOf(cv::Mat image, std::uint64_t number = 0) {
    return {.image = std::move(image), .number = number, .at = {}, .tags = {}};
}

std::string oneDecimal(double value) {
    std::array<char, 32> buffer{};
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, 1);
    return {buffer.data(), result.ptr};
}

TEST(Processors, NamesAreThePickersEntries) {
    EXPECT_EQ(lab_core::nameOf(ProcessorKind::None), "None");
    EXPECT_EQ(lab_core::nameOf(ProcessorKind::Edges), "Edges");
    EXPECT_EQ(lab_core::nameOf(ProcessorKind::Outline), "Outline");
}

TEST(Processors, NoneGivesNothing) {
    const auto processor = lab_core::makeProcessor(ProcessorKind::None);

    const lab_core::ProcessorResult result = processor->process(frameOf(test_support::cardFrame()));

    EXPECT_TRUE(result.overlays.empty());
    EXPECT_TRUE(result.images.empty());
    EXPECT_TRUE(result.findings.empty());
}

TEST(Processors, EdgesGivesTheEdgeImageAndItsShare) {
    cv::Mat frame(100, 100, CV_8UC3, cv::Scalar::all(0));
    cv::rectangle(frame, cv::Rect(30, 30, 40, 40), cv::Scalar::all(255), cv::FILLED);
    const auto processor = lab_core::makeProcessor(ProcessorKind::Edges);

    const lab_core::ProcessorResult result = processor->process(frameOf(frame));

    ASSERT_EQ(result.images.size(), 1U);
    EXPECT_EQ(result.images[0].name, "edges");
    EXPECT_EQ(result.images[0].image.type(), CV_8UC1);
    EXPECT_EQ(result.images[0].image.size(), frame.size());
    const int edgePixels = cv::countNonZero(result.images[0].image);
    EXPECT_GT(edgePixels, 0);
    ASSERT_EQ(result.findings.size(), 1U);
    EXPECT_EQ(result.findings[0],
              (lab_core::Finding{"Edges", oneDecimal(100.0 * edgePixels / (100 * 100)) + " %"}));
    EXPECT_TRUE(result.overlays.empty());
}

const visual_debug::Polygon* documentPolygonIn(const lab_core::ProcessorResult& result) {
    if (result.overlays.size() != 1) {
        return nullptr;
    }
    const auto* polygon = std::get_if<visual_debug::Polygon>(&result.overlays[0]);
    return polygon && polygon->label == "Document" ? polygon : nullptr;
}

TEST(Processors, OutlineFindsTheCard) {
    const auto processor = lab_core::makeProcessor(ProcessorKind::Outline);

    const lab_core::ProcessorResult result = processor->process(frameOf(test_support::cardFrame()));

    const visual_debug::Polygon* polygon = documentPolygonIn(result);
    ASSERT_NE(polygon, nullptr);
    EXPECT_TRUE(test_support::findsTheCorners(polygon->points, 1, 4.0));
    EXPECT_EQ(polygon->colour, visual_debug::kOverlayGreen);
    EXPECT_EQ(result.findings, (std::vector<lab_core::Finding>{{"Document", "found"}}));
    EXPECT_TRUE(result.images.empty());
}

// The outline stays over a frame or two in which it is lost, but a new source or run starts without it.
TEST(Processors, OutlineHoldsAndResets) {
    const cv::Mat plain(480, 640, CV_8UC3, cv::Scalar::all(40));
    const auto processor = lab_core::makeProcessor(ProcessorKind::Outline);
    processor->process(frameOf(test_support::cardFrame(), 0));

    const lab_core::ProcessorResult held = processor->process(frameOf(plain, 1));
    EXPECT_NE(documentPolygonIn(held), nullptr);
    EXPECT_EQ(held.findings, (std::vector<lab_core::Finding>{{"Document", "found"}}));

    processor->reset();
    const lab_core::ProcessorResult afterReset = processor->process(frameOf(plain, 2));
    EXPECT_TRUE(afterReset.overlays.empty());
    EXPECT_EQ(afterReset.findings, (std::vector<lab_core::Finding>{{"Document", "not found"}}));
}

TEST(Processors, FindingAsText) {
    EXPECT_EQ(lab_core::toText(lab_core::Finding{"Sharpness", "41 (min 200)", lab_core::Finding::Tone::Bad}),
              "Sharpness: 41 (min 200)");
    EXPECT_EQ(lab_core::toText(lab_core::Finding{"Plain note", ""}), "Plain note");
}

} // namespace
