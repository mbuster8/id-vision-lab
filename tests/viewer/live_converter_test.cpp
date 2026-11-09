#include "latch.h"
#include "live_converter.h"
#include "qimage_conversion.h"

#include "viewer_core/display_converter.h"
#include "viewer_core/histogram.h"

#include <QCoreApplication>
#include <QTest>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using KeepView = ImageView::KeepView;
using test_support::Latch;

LiveJob jobFor(const std::string& channel, std::uint64_t frameNumber, KeepView keep = KeepView::IfSameSize) {
    return {.channel = channel,
            .frameNumber = frameNumber,
            .frame = cv::Mat(2, 3, CV_8UC1, cv::Scalar(0)),
            .drawnSize = cv::Size(3, 2),
            .keep = keep};
}

LiveResult resultOf(const LiveJob& job) {
    return {.job = job, .imageSize = QSize(job.frame.cols, job.frame.rows)};
}

std::vector<std::uint64_t> numbersOf(const std::vector<LiveResult>& results) {
    std::vector<std::uint64_t> numbers;
    for (const LiveResult& result : results) {
        numbers.push_back(result.job.frameNumber);
    }
    return numbers;
}

TEST(LiveConverter, ImmediateConvertsBeforeReturning) {
    std::vector<LiveResult> results;
    LiveConverter converter(
        LiveConverter::Mode::Immediate,
        [&results](LiveResult result) { results.push_back(std::move(result)); }, resultOf);

    EXPECT_FALSE(converter.submit(jobFor("camera", 7)).has_value());

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].job.channel, "camera");
    EXPECT_EQ(results[0].job.frameNumber, 7U);
    EXPECT_TRUE(results[0].job.frame.empty()); // released: only its size travels on
    EXPECT_EQ(converter.newestSize(), cv::Size(3, 2));
}

TEST(LiveConverter, ThreadedResultArrivesOnTheUiThread) {
    const std::thread::id uiThread = std::this_thread::get_id();
    std::atomic<bool> convertedElsewhere = false;
    std::vector<LiveResult> results;
    bool receivedOnUiThread = false;
    LiveConverter converter(
        LiveConverter::Mode::Threaded,
        [&](LiveResult result) {
            receivedOnUiThread = std::this_thread::get_id() == uiThread;
            results.push_back(std::move(result));
        },
        [&](const LiveJob& job) {
            convertedElsewhere = std::this_thread::get_id() != uiThread;
            return resultOf(job);
        });

    converter.submit(jobFor("camera", 1));

    ASSERT_TRUE(QTest::qWaitFor([&results] { return !results.empty(); }));
    EXPECT_TRUE(convertedElsewhere);
    EXPECT_TRUE(receivedOnUiThread);
    EXPECT_EQ(results[0].job.channel, "camera");
}

// A job submitted while the worker is busy replaces the one waiting: the worker only ever converts the
// newest.
TEST(LiveConverter, NewestWins) {
    Latch latch;
    std::vector<LiveResult> results;
    LiveConverter converter(
        LiveConverter::Mode::Threaded,
        [&results](LiveResult result) { results.push_back(std::move(result)); },
        [&latch](const LiveJob& job) {
            if (job.frameNumber == 1) {
                latch.hold();
            }
            return resultOf(job);
        });

    EXPECT_FALSE(converter.submit(jobFor("camera", 1)).has_value());
    ASSERT_TRUE(latch.waitUntilEntered());
    EXPECT_FALSE(converter.submit(jobFor("camera", 2)).has_value());
    const std::optional<LiveJob> replaced = converter.submit(jobFor("camera", 3));
    latch.release();

    ASSERT_TRUE(replaced.has_value());
    EXPECT_EQ(replaced->frameNumber, 2U);
    ASSERT_TRUE(QTest::qWaitFor([&results] { return results.size() == 2; }));
    converter.submit(jobFor("camera", 4)); // after 3, so job 2 would have come before it
    ASSERT_TRUE(QTest::qWaitFor([&results] { return results.size() == 3; }));
    EXPECT_EQ(numbersOf(results), (std::vector<std::uint64_t>{1, 3, 4}));
}

// A channel's first frame is fitted to the view; a newer frame replacing that job must not lose the fit.
TEST(LiveConverter, FitSurvivesReplacement) {
    Latch latch;
    std::vector<LiveResult> results;
    LiveConverter converter(
        LiveConverter::Mode::Threaded,
        [&results](LiveResult result) { results.push_back(std::move(result)); },
        [&latch](const LiveJob& job) {
            if (job.frameNumber == 1) {
                latch.hold();
            }
            return resultOf(job);
        });

    converter.submit(jobFor("camera", 1));
    ASSERT_TRUE(latch.waitUntilEntered());
    converter.submit(jobFor("camera", 2, KeepView::No));
    const std::optional<LiveJob> replaced = converter.submit(jobFor("camera", 3, KeepView::IfSameSize));
    latch.release();

    ASSERT_TRUE(replaced.has_value());
    EXPECT_EQ(replaced->keep, KeepView::No);
    ASSERT_TRUE(QTest::qWaitFor([&results] { return results.size() == 2; }));
    EXPECT_EQ(results[1].job.frameNumber, 3U);
    EXPECT_EQ(results[1].job.keep, KeepView::No);
}

TEST(LiveConverter, ConversionErrorIsDropped) {
    std::atomic<int> calls = 0;
    std::vector<LiveResult> results;
    LiveConverter converter(
        LiveConverter::Mode::Threaded,
        [&results](LiveResult result) { results.push_back(std::move(result)); },
        [&calls](const LiveJob& job) {
            ++calls;
            if (job.frameNumber == 1) {
                throw std::runtime_error("cannot convert");
            }
            if (job.frameNumber == 2) {
                throw std::bad_alloc();
            }
            return resultOf(job);
        });

    converter.submit(jobFor("camera", 1));
    ASSERT_TRUE(QTest::qWaitFor([&calls] { return calls == 1; }));
    converter.submit(jobFor("camera", 2));
    ASSERT_TRUE(QTest::qWaitFor([&calls] { return calls == 2; }));
    converter.submit(jobFor("camera", 3));

    ASSERT_TRUE(QTest::qWaitFor([&results] { return !results.empty(); }));
    EXPECT_EQ(numbersOf(results), (std::vector<std::uint64_t>{3}));
}

TEST(LiveConverter, ImmediateConversionErrorIsDropped) {
    std::vector<LiveResult> results;
    LiveConverter converter(
        LiveConverter::Mode::Immediate,
        [&results](LiveResult result) { results.push_back(std::move(result)); },
        [](const LiveJob&) -> LiveResult { throw std::runtime_error("cannot convert"); });

    EXPECT_FALSE(converter.submit(jobFor("camera", 1)).has_value());

    EXPECT_TRUE(results.empty());
}

// Destroying the converter waits for the conversion running on its thread and delivers nothing afterwards.
TEST(LiveConverter, DestroyedWhileConverting) {
    Latch latch;
    std::vector<LiveResult> results;
    auto converter = std::make_unique<LiveConverter>(
        LiveConverter::Mode::Threaded,
        [&results](LiveResult result) { results.push_back(std::move(result)); },
        [&latch](const LiveJob& job) {
            if (job.frameNumber == 1) {
                latch.hold();
            }
            return resultOf(job);
        });
    converter->submit(jobFor("camera", 1));
    ASSERT_TRUE(latch.waitUntilEntered());
    converter->submit(jobFor("camera", 2));

    // Released while the destructor waits: whichever comes first, nothing may reach the receiver.
    std::thread releaser([&latch] {
        std::this_thread::sleep_for(20ms);
        latch.release();
    });
    converter.reset();
    releaser.join();
    QCoreApplication::processEvents();

    EXPECT_TRUE(results.empty());
}

TEST(LiveConverter, ConvertsWithTheJobsSettings) {
    cv::Mat frame(2, 2, CV_8UC1);
    frame.at<unsigned char>(0, 0) = 0;
    frame.at<unsigned char>(0, 1) = 50;
    frame.at<unsigned char>(1, 0) = 100;
    frame.at<unsigned char>(1, 1) = 200;
    const viewer_core::DisplaySettings fixed{.mode = viewer_core::RangeMode::Fixed, .range = {0, 100}};
    LiveJob job{.channel = "camera",
                .frameNumber = 1,
                .frame = frame,
                .settings = fixed,
                .drawnSize = frame.size(),
                .showClipping = true};

    const LiveResult plain = convertLiveFrame(job);
    job.histogram = true;
    const LiveResult withHistogram = convertLiveFrame(job);

    const QImage expected = qt_common::toQImage(
        viewer_core::toDisplayImage(frame, fixed, {.size = frame.size(), .showClipping = true}));
    EXPECT_EQ(plain.picture, expected);
    EXPECT_EQ(plain.imageSize, QSize(2, 2));
    EXPECT_FALSE(plain.histogram.has_value());
    EXPECT_FALSE(plain.markers.has_value());
    const viewer_core::ValueExtent extent = viewer_core::valueExtent(frame);
    const auto markers = viewer_core::rangeMarkers(CV_8U, 1, fixed, {}, extent);
    const viewer_core::Histogram histogram = viewer_core::histogramOf(frame, extent, markers);
    ASSERT_TRUE(withHistogram.histogram.has_value());
    EXPECT_EQ(withHistogram.histogram->counts, histogram.counts);
    EXPECT_EQ(withHistogram.histogram->axis, histogram.axis);
    EXPECT_EQ(withHistogram.markers, markers);
}

// Drawn smaller than the frame, as a 4K frame in a window is.
TEST(LiveConverter, ConvertsAtTheDrawnSize) {
    const LiveJob job{.channel = "camera",
                      .frameNumber = 1,
                      .frame = cv::Mat(40, 60, CV_16UC1, cv::Scalar(1000)),
                      .drawnSize = cv::Size(30, 20)};

    const LiveResult result = convertLiveFrame(job);

    EXPECT_EQ(result.picture.size(), QSize(30, 20));
    EXPECT_EQ(result.imageSize, QSize(60, 40));
}

} // namespace
