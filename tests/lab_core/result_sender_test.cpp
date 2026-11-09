#include "lab_core/result_sender.h"

#include "fake_sender.h"
#include "latch.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <memory>
#include <string>
#include <variant>

namespace {

using lab_core::ResultSender;
using test_support::RecordingSender;
using visual_debug::SendResult;

// A result of frame `number`, a 4×3 picture of that value, with tags, one overlay and an edges picture.
lab_core::LabResult resultOf(std::uint64_t number) {
    lab_core::LabResult result;
    result.generation = 1;
    result.frame = {.image = cv::Mat(3, 4, CV_8UC3, cv::Scalar::all(static_cast<double>(number))),
                    .number = number,
                    .at = {},
                    .tags = {.valueRange = {0, 255}, .transfer = {}, .primaries = {}, .overlays = {}}};
    result.processed.overlays.push_back(visual_debug::Polygon{{{0, 0}, {3, 0}, {3, 2}}, "Document"});
    result.processed.images.push_back(
        {.name = "edges", .image = cv::Mat(3, 4, CV_8UC1, cv::Scalar(255)), .description = {}});
    return result;
}

// The sender that a ResultSender takes over, with a pointer the test keeps to look at its calls.
struct Fake {
    explicit Fake(
        std::function<SendResult(std::string_view)> answer =
            [](std::string_view) { return SendResult::Delivered; },
        test_support::Latch* holdLive = nullptr)
        : owned(std::make_unique<RecordingSender>(std::move(answer), holdLive)), sender(owned.get()) {}
    std::unique_ptr<RecordingSender> owned;
    RecordingSender* sender;
};

TEST(ResultSender, ChannelsAreNamedAfterTheLab) {
    EXPECT_STREQ(lab_core::kSourceChannel, "lab");
    EXPECT_EQ(lab_core::channelFor("edges"), "lab edges");
}

// The frame goes out as the "lab" channel with its tags and the processor's overlays, then each picture as
// its own channel.
TEST(ResultSender, SendsTheFrameAndEveryImage) {
    Fake fake;
    ResultSender sender(ResultSender::Mode::Immediate, std::move(fake.owned));

    sender.submit(resultOf(7));
    EXPECT_TRUE(sender.pump());

    const auto calls = fake.sender->calls();
    ASSERT_EQ(calls.size(), 2U);
    EXPECT_EQ(calls[0].name, "lab");
    EXPECT_TRUE(calls[0].live);
    EXPECT_EQ(calls[0].image.at<cv::Vec3b>(0, 0), cv::Vec3b(7, 7, 7));
    EXPECT_EQ(calls[0].description.valueRange, (visual_debug::ValueRange{0, 255}));
    ASSERT_EQ(calls[0].description.overlays.size(), 1U);
    EXPECT_EQ(std::get<visual_debug::Polygon>(calls[0].description.overlays[0]).label, "Document");
    EXPECT_EQ(calls[1].name, "lab edges");
    EXPECT_TRUE(calls[1].live);
    EXPECT_EQ(calls[1].image.type(), CV_8UC1);
    EXPECT_FALSE(sender.pump()); // nothing waits
    EXPECT_EQ(sender.stats().sent, 1U);
    EXPECT_EQ(sender.stats().last, SendResult::Delivered);
}

// A result not yet sent when a newer one comes is replaced whole, so the viewer's channels show one frame.
TEST(ResultSender, NewestResultWins) {
    Fake fake;
    ResultSender sender(ResultSender::Mode::Immediate, std::move(fake.owned));

    sender.submit(resultOf(1));
    sender.submit(resultOf(2));
    EXPECT_TRUE(sender.pump());

    const auto calls = fake.sender->calls();
    ASSERT_EQ(calls.size(), 2U);
    EXPECT_EQ(calls[0].image.at<cv::Vec3b>(0, 0), cv::Vec3b(2, 2, 2));
    EXPECT_EQ(sender.stats().sent, 1U);
    EXPECT_EQ(sender.stats().notSent, 1U);
}

// The status line shows the frame's channel's result, such as a viewer that is not there.
TEST(ResultSender, CountsTheLastResult) {
    Fake fake([](std::string_view channel) {
        return channel == "lab" ? SendResult::ViewerUnavailable : SendResult::Delivered;
    });
    ResultSender sender(ResultSender::Mode::Immediate, std::move(fake.owned));

    EXPECT_EQ(sender.stats().last, std::nullopt);
    sender.submit(resultOf(1));
    sender.pump();

    EXPECT_EQ(sender.stats().last, SendResult::ViewerUnavailable);
}

// On its own thread, a slow send holds nobody up: what comes meanwhile waits, the newest replacing the rest.
TEST(ResultSender, SendsOnItsOwnThreadNewestFirst) {
    test_support::Latch latch;
    Fake fake([](std::string_view) { return SendResult::Delivered; }, &latch);
    ResultSender sender(ResultSender::Mode::Threaded, std::move(fake.owned));

    sender.submit(resultOf(1));
    ASSERT_TRUE(latch.waitUntilEntered()); // result 1 is being sent
    sender.submit(resultOf(2));
    sender.submit(resultOf(3)); // replaces 2
    EXPECT_EQ(sender.stats().notSent, 1U);
    latch.release();

    ASSERT_TRUE(fake.sender->waitForCalls(4));
    const auto calls = fake.sender->calls();
    EXPECT_EQ(calls[0].image.at<cv::Vec3b>(0, 0), cv::Vec3b(1, 1, 1));
    EXPECT_EQ(calls[2].image.at<cv::Vec3b>(0, 0), cv::Vec3b(3, 3, 3));
    EXPECT_EQ(calls[2].name, "lab");
}

} // namespace
