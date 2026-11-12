#include "lab_core/mrz_worker.h"

#include "fake_mrz_reader.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <chrono>
#include <memory>
#include <thread>

namespace {

using lab_core::MrzJob;
using lab_core::MrzWorker;
using test_support::ReaderScript;
using test_support::scriptedReader;

MrzJob jobOf(std::uint64_t frame, std::uint64_t resetCount = 0) {
    MrzJob job;
    job.bgr = cv::Mat(720, 1280, CV_8UC3, cv::Scalar::all(200));
    job.record.frame.image = job.bgr;
    job.record.frame.number = frame;
    job.record.quad = {{{100, 100}, {1100, 100}, {1100, 600}, {100, 600}}};
    job.resetCount = resetCount;
    return job;
}

TEST(MrzWorker, ManualNewestJobReplacesAWaitingOne) {
    auto script = std::make_shared<ReaderScript>();
    MrzWorker worker(MrzWorker::Mode::Manual, scriptedReader(script));
    worker.submit(jobOf(1));
    worker.submit(jobOf(2));
    EXPECT_TRUE(worker.busy());
    EXPECT_TRUE(worker.pump());
    EXPECT_FALSE(worker.pump());
    EXPECT_FALSE(worker.busy());
    const std::vector<lab_core::MrzDone> done = worker.take();
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done[0].record.frame.number, 2u);
    EXPECT_EQ(script->reads, 1);
    EXPECT_TRUE(worker.take().empty());
}

// The plan review's critical finding: a replaced job takes its frame with it.
TEST(MrzWorker, ReplacedJobFreesItsFrame) {
    auto script = std::make_shared<ReaderScript>();
    MrzWorker worker(MrzWorker::Mode::Manual, scriptedReader(script));
    cv::Mat image(10, 10, CV_8UC3, cv::Scalar::all(1));
    MrzJob first = jobOf(1);
    first.record.frame.image = image;
    worker.submit(std::move(first));
    EXPECT_GT(image.u->refcount, 1);
    worker.submit(jobOf(2));
    EXPECT_EQ(image.u->refcount, 1);
}

TEST(MrzWorker, ImmediateReadsInSubmit) {
    auto script = std::make_shared<ReaderScript>();
    MrzWorker worker(MrzWorker::Mode::Immediate, scriptedReader(script));
    worker.submit(jobOf(7, 3));
    const std::vector<lab_core::MrzDone> done = worker.take();
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done[0].record.frame.number, 7u);
    EXPECT_EQ(done[0].resetCount, 3u);
}

TEST(MrzWorker, OpensOnceOnTheFirstJob) {
    auto script = std::make_shared<ReaderScript>();
    MrzWorker worker(MrzWorker::Mode::Manual, scriptedReader(script));
    EXPECT_EQ(script->makes, 0);
    worker.submit(jobOf(1));
    worker.pump();
    worker.submit(jobOf(2));
    worker.pump();
    EXPECT_EQ(script->makes, 1);
    EXPECT_EQ(script->reads, 2);
}

TEST(MrzWorker, FailedOpenIsReportedOnce) {
    auto script = std::make_shared<ReaderScript>();
    script->failMake = true;
    MrzWorker worker(MrzWorker::Mode::Manual, scriptedReader(script));
    EXPECT_FALSE(worker.unavailable());
    worker.submit(jobOf(1));
    worker.pump();
    ASSERT_TRUE(worker.unavailable());
    EXPECT_EQ(*worker.unavailable(), "model missing");
    worker.submit(jobOf(2));
    EXPECT_FALSE(worker.pump());
    EXPECT_EQ(script->makes, 1);
    EXPECT_TRUE(worker.take().empty());

    auto runtime = std::make_shared<ReaderScript>();
    runtime->failMake = true;
    runtime->failure = doc_vision::OpenFailure::Runtime;
    MrzWorker refused(MrzWorker::Mode::Immediate, scriptedReader(runtime));
    refused.submit(jobOf(1));
    ASSERT_TRUE(refused.unavailable());
    EXPECT_EQ(*refused.unavailable(), "ONNX Runtime refused it");
}

TEST(MrzWorker, ReadsTheFlattenedPage) {
    auto script = std::make_shared<ReaderScript>();
    MrzWorker worker(MrzWorker::Mode::Immediate, scriptedReader(script));
    worker.submit(jobOf(1));
    ASSERT_EQ(script->pages.size(), 1u);
    EXPECT_EQ(script->pages[0], cv::Size(1250, 880));
}

TEST(MrzWorker, ThrowingReaderIsAnError) {
    auto script = std::make_shared<ReaderScript>();
    script->throwOnRead = true;
    MrzWorker worker(MrzWorker::Mode::Manual, scriptedReader(script));
    worker.submit(jobOf(1));
    worker.pump();
    std::vector<lab_core::MrzDone> done = worker.take();
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done[0].error, "the reader failed");
    script->throwOnRead = false;
    worker.submit(jobOf(2));
    worker.pump();
    done = worker.take();
    ASSERT_EQ(done.size(), 1u);
    EXPECT_TRUE(done[0].error.empty());
}

TEST(MrzWorker, DestructorCancelsASlowRead) {
    auto script = std::make_shared<ReaderScript>();
    script->blockReads = true;
    auto worker = std::make_unique<MrzWorker>(MrzWorker::Mode::Threaded, scriptedReader(script));
    worker->submit(jobOf(1));
    ASSERT_TRUE(script->waitFor([&] { return script->readStarted; }));
    const auto started = std::chrono::steady_clock::now();
    worker.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    EXPECT_GE(script->cancels, 1);
}

TEST(MrzWorker, DestructorDuringTheOpenSkipsTheRead) {
    auto script = std::make_shared<ReaderScript>();
    script->blockMake = true;
    script->blockReads = true; // a read after the open would hold the destructor until it is cancelled
    auto worker = std::make_unique<MrzWorker>(MrzWorker::Mode::Threaded, scriptedReader(script));
    worker->submit(jobOf(1));
    ASSERT_TRUE(script->waitFor([&] { return script->makeStarted; }));
    std::thread destroying([&worker] { worker.reset(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the destructor is waiting for the open
    {
        const std::lock_guard lock(script->mutex);
        script->releaseMake = true;
    }
    script->changed.notify_all();
    destroying.join();
    EXPECT_EQ(script->reads, 0);
}

} // namespace
