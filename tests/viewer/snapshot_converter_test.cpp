#include "latch.h"
#include "snapshot_converter.h"

#include "viewer_core/image_history.h"

#include <QCoreApplication>
#include <QTest>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using test_support::Latch;

SnapshotJob jobNamed(const std::string& name) {
    return {.name = name, .original = cv::Mat(3, 4, CV_16UC1, cv::Scalar(1000))};
}

std::vector<std::string> namesOf(const std::vector<SnapshotResult>& results) {
    std::vector<std::string> names;
    for (const SnapshotResult& result : results) {
        names.push_back(result.job.name);
    }
    return names;
}

TEST(SnapshotConverter, ImmediateDeliversBeforeReturning) {
    std::vector<SnapshotResult> results;
    SnapshotConverter converter(SnapshotConverter::Mode::Immediate,
                                [&results](SnapshotResult result) { results.push_back(std::move(result)); });

    converter.submit(jobNamed("camera 14:03:22.517"));

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].job.name, "camera 14:03:22.517");
    EXPECT_EQ(
        cv::norm(results[0].display, viewer_core::historyDisplay(results[0].job.original, {}), cv::NORM_INF),
        0.0);
}

// Unlike live frames, no snapshot is ever replaced: each is converted, and they arrive in the order taken.
TEST(SnapshotConverter, ThreadedDeliversEveryOneInOrderOnTheUiThread) {
    const std::thread::id uiThread = std::this_thread::get_id();
    Latch latch;
    std::atomic<bool> convertedElsewhere = true;
    std::vector<SnapshotResult> results;
    bool receivedOnUiThread = true;
    SnapshotConverter converter(
        SnapshotConverter::Mode::Threaded,
        [&](SnapshotResult result) {
            receivedOnUiThread = receivedOnUiThread && std::this_thread::get_id() == uiThread;
            results.push_back(std::move(result));
        },
        [&](const SnapshotJob& job) {
            convertedElsewhere = convertedElsewhere && std::this_thread::get_id() != uiThread;
            if (job.name == "a") {
                latch.hold();
            }
            return viewer_core::historyDisplay(job.original, job.origin.tags);
        });

    converter.submit(jobNamed("a"));
    ASSERT_TRUE(latch.waitUntilEntered());
    converter.submit(jobNamed("b")); // both wait behind a
    converter.submit(jobNamed("c"));
    latch.release();

    ASSERT_TRUE(QTest::qWaitFor([&results] { return results.size() == 3; }));
    EXPECT_EQ(namesOf(results), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_TRUE(convertedElsewhere);
    EXPECT_TRUE(receivedOnUiThread);
}

// A snapshot that could not be converted still comes back, without a picture, so the window can say so.
TEST(SnapshotConverter, AFailedConversionComesBackWithoutAPicture) {
    std::vector<SnapshotResult> results;
    SnapshotConverter converter(
        SnapshotConverter::Mode::Threaded,
        [&results](SnapshotResult result) { results.push_back(std::move(result)); },
        [](const SnapshotJob& job) -> cv::Mat {
            if (job.name == "a") {
                throw std::bad_alloc();
            }
            return viewer_core::historyDisplay(job.original, {});
        });

    converter.submit(jobNamed("a"));
    converter.submit(jobNamed("b"));

    ASSERT_TRUE(QTest::qWaitFor([&results] { return results.size() == 2; }));
    EXPECT_TRUE(results[0].display.empty());
    EXPECT_FALSE(results[1].display.empty());
}

// Closing the window drops the snapshots still waiting and waits for the one being converted.
TEST(SnapshotConverter, DestroyedWhileConvertingDeliversNothing) {
    Latch latch;
    std::vector<SnapshotResult> results;
    auto converter = std::make_unique<SnapshotConverter>(
        SnapshotConverter::Mode::Threaded,
        [&results](SnapshotResult result) { results.push_back(std::move(result)); },
        [&latch](const SnapshotJob& job) {
            latch.hold();
            return viewer_core::historyDisplay(job.original, {});
        });
    converter->submit(jobNamed("a"));
    ASSERT_TRUE(latch.waitUntilEntered());
    converter->submit(jobNamed("b"));

    std::thread releaser([&latch] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        latch.release();
    });
    converter.reset();
    releaser.join();
    QCoreApplication::processEvents();

    EXPECT_TRUE(results.empty());
}

} // namespace
