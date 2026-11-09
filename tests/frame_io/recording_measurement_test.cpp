// How fast the recorder writes, with 4 and 6 writer threads, run by hand in a release build:
//   frame_io_tests --gtest_also_run_disabled_tests --gtest_filter=*Measurement*
// Into DOCUMENT_LAB_OUT when set (the disk the lab would record to), else a temporary folder. Prints its
// numbers and asserts nothing about them.

#include "frame_io/recording.h"

#include "scratch_folder.h"
#include "test_support.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

// A frame that compresses about as a camera's does: a smooth gradient with sensor-like noise on it.
cv::Mat cameraLikeFrame(cv::Size size) {
    cv::Mat gradient(size, CV_8UC3);
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            gradient.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<std::uint8_t>(60 + 120 * x / size.width),
                                                     static_cast<std::uint8_t>(80 + 100 * y / size.height),
                                                     static_cast<std::uint8_t>(140));
        }
    }
    cv::Mat noise(size, CV_16SC3);
    cv::randn(noise, cv::Scalar::all(0), cv::Scalar::all(4));
    cv::Mat frame;
    cv::add(gradient, noise, frame, cv::noArray(), CV_8UC3);
    return frame;
}

TEST(RecordingMeasurement, DISABLED_WriterThreads) {
    constexpr int kFrames = 60;
    const test_support::ScratchFolder scratch;
    const std::optional<std::string> out = test_support::environmentValue("DOCUMENT_LAB_OUT");
    const std::filesystem::path parent = out ? std::filesystem::path(*out) : scratch.path();
    for (const cv::Size size : {cv::Size(1920, 1080), cv::Size(1920, 1440), cv::Size(3840, 2160)}) {
        const cv::Mat image = cameraLikeFrame(size);
        for (const int threads : {4, 6}) {
            std::string error;
            const std::filesystem::path folder = frame_io::createUniqueFolder(
                parent, "measurement " + std::to_string(size.width) + "x" + std::to_string(threads), error);
            ASSERT_FALSE(folder.empty()) << error;
            frame_io::RecordingWriter::Options options;
            options.threads = threads;
            options.queueBytes = std::size_t{1} << 40; // every frame queued: this measures the writers only
            options.freeSpace = [](const std::filesystem::path&) { return std::uintmax_t{1} << 50; };
            auto writer = frame_io::RecordingWriter::start(
                folder, {.kind = "camera", .name = "measurement", .format = {}, .startedAt = {}}, options,
                error);
            ASSERT_TRUE(writer) << error;
            const Clock::time_point start = Clock::now();
            for (int i = 0; i < kFrames; ++i) {
                ASSERT_EQ(writer->push({.image = image,
                                        .number = static_cast<std::uint64_t>(i),
                                        .at = std::chrono::milliseconds(33 * i),
                                        .tags = {}}),
                          frame_io::RecordingWriter::Push::Queued);
            }
            writer->finish();
            const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
            std::uintmax_t bytes = 0;
            for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                if (entry.path().extension() == ".png") {
                    bytes += entry.file_size();
                }
            }
            std::cout << "[ measurement ] " << size.width << "×" << size.height << ", " << threads
                      << " threads: " << kFrames / seconds << " frames/s, " << bytes / kFrames / 1e6
                      << " MB per frame\n";
            std::filesystem::remove_all(folder);
        }
    }
}

} // namespace
