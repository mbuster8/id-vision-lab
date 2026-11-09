#include "frame_io/image_folder.h"
#include "frame_io/recording.h"

#include "latch.h"
#include "scratch_folder.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frame_io::Frame;
using frame_io::ReadStatus;
using frame_io::RecordingWriter;
using test_support::ScratchFolder;

const frame_io::RecordingInfo kInfo{.kind = "camera",
                                    .name = "Test camera",
                                    .format = "8×8, 30 fps, NV12",
                                    .startedAt = "2026-11-09T14:03:22"};

Frame frameOf(cv::Mat image, std::uint64_t number, std::chrono::nanoseconds at,
              visual_debug::ImageDescription tags = {}) {
    return {.image = std::move(image), .number = number, .at = at, .tags = std::move(tags)};
}

Frame grey(int value, std::uint64_t number, std::chrono::nanoseconds at) {
    return frameOf(cv::Mat(8, 8, CV_8UC3, cv::Scalar::all(value)), number, at);
}

constexpr std::size_t kGreyBytes = 8 * 8 * 3;

RecordingWriter::Options immediate() {
    RecordingWriter::Options options;
    options.mode = RecordingWriter::Mode::Immediate;
    options.freeSpace = [](const std::filesystem::path&) { return std::uintmax_t{100'000'000'000}; };
    return options;
}

RecordingWriter::Options threaded() {
    RecordingWriter::Options options = immediate();
    options.mode = RecordingWriter::Mode::Threaded;
    return options;
}

std::unique_ptr<RecordingWriter> startIn(const std::filesystem::path& folder,
                                         RecordingWriter::Options options) {
    std::string error;
    auto writer = RecordingWriter::start(folder, kInfo, std::move(options), error);
    EXPECT_NE(writer, nullptr) << error;
    return writer;
}

nlohmann::json jsonIn(const std::filesystem::path& folder) {
    std::ifstream stream(folder / "recording.json");
    return nlohmann::json::parse(stream);
}

TEST(RecordingFolder, TakenNamesGetANumber) {
    const ScratchFolder parent;
    std::string error;

    const auto first = frame_io::createUniqueFolder(parent.path(), "2026-11-09 14.03.22", error);
    const auto second = frame_io::createUniqueFolder(parent.path(), "2026-11-09 14.03.22", error);

    EXPECT_EQ(first.filename(), "2026-11-09 14.03.22");
    EXPECT_EQ(second.filename(), "2026-11-09 14.03.22 2");
    EXPECT_TRUE(std::filesystem::is_directory(second));
}

TEST(Recording, RoundTripsEveryKind) {
    const ScratchFolder folder;
    const visual_debug::ImageDescription hlg{.valueRange = {4100, 60218},
                                             .transfer = visual_debug::Transfer::Hlg,
                                             .primaries = visual_debug::Primaries::Bt2020,
                                             .overlays = {}};
    std::vector<Frame> frames{frameOf(cv::Mat(6, 4, CV_8UC3, cv::Scalar(1, 2, 3)), 7, 1s),
                              frameOf(cv::Mat(6, 4, CV_16UC1, cv::Scalar(40000)), 9, 1s + 40ms, hlg),
                              frameOf(cv::Mat(6, 4, CV_8UC4, cv::Scalar(4, 5, 6, 7)), 12, 1s + 80ms)};
    {
        auto writer = startIn(folder.path(), immediate());
        for (const Frame& frame : frames) {
            ASSERT_EQ(writer->push(frame), RecordingWriter::Push::Queued);
        }
        writer->finish();
    }
    std::string error;
    const auto source = frame_io::openRecording(folder.path(), error);
    ASSERT_NE(source, nullptr) << error;

    for (std::uint64_t index = 0; index < frames.size(); ++index) {
        const frame_io::ReadResult read = source->next();
        ASSERT_EQ(read.status, ReadStatus::Frame) << read.error;
        EXPECT_EQ(read.frame.number, index); // renumbered from 0 in the recording
        EXPECT_EQ(read.frame.at, 40ms * index);
        EXPECT_EQ(read.frame.image.type(), frames[index].image.type());
        EXPECT_EQ(cv::norm(read.frame.image, frames[index].image, cv::NORM_INF), 0.0);
        EXPECT_EQ(read.frame.tags, frames[index].tags);
    }
    EXPECT_EQ(source->next().status, ReadStatus::End);
}

TEST(Recording, JsonMarksIncompleteUntilFinished) {
    const ScratchFolder folder;
    auto writer = startIn(folder.path(), immediate());
    const nlohmann::json started = jsonIn(folder.path());
    EXPECT_EQ(started["format"], "document-lab-recording");
    EXPECT_EQ(started["version"], 1);
    EXPECT_EQ(started["complete"], false);
    EXPECT_TRUE(started["frames"].empty());
    EXPECT_EQ(started["source"]["name"], "Test camera");

    writer->push(grey(10, 0, 0ms));
    writer->push(grey(20, 1, 40ms));
    writer->setCounts({.droppedBeforeProcessing = 1, .notDrawn = 2, .notSent = 3, .notRecorded = 4});
    writer->finish();

    const nlohmann::json finished = jsonIn(folder.path());
    EXPECT_EQ(finished["complete"], true);
    ASSERT_EQ(finished["frames"].size(), 2U);
    EXPECT_EQ(finished["frames"][1]["file"], "000001.png");
    EXPECT_EQ(finished["frames"][1]["at"], 40'000'000);
    EXPECT_EQ(finished["counts"]["droppedBeforeProcessing"], 1);
    EXPECT_EQ(finished["counts"]["notRecorded"], 4);
}

// A recording cut short by a crash still replays: the frames its JSON lists, then any PNGs it does not, in
// name order, each a thirtieth of a second after the one before.
TEST(Recording, IncompleteReplaysListedAndLaterFrames) {
    const ScratchFolder folder;
    std::string started;
    {
        auto writer = startIn(folder.path(), immediate());
        std::ifstream stream(folder.path() / "recording.json");
        started.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        writer->push(grey(10, 0, 0ms));
        writer->push(grey(20, 1, 40ms));
        writer->push(grey(30, 2, 80ms));
        writer->finish();
    }
    std::ofstream(folder.path() / "recording.json", std::ios::trunc) << started;
    std::string error;
    const auto source = frame_io::openRecording(folder.path(), error);
    ASSERT_NE(source, nullptr) << error;

    const std::vector<std::int64_t> times{0, 33'333'333, 66'666'666};
    for (int index = 0; index < 3; ++index) {
        const frame_io::ReadResult read = source->next();
        ASSERT_EQ(read.status, ReadStatus::Frame) << read.error;
        EXPECT_EQ(read.frame.image.at<cv::Vec3b>(0, 0)[0], 10 * (index + 1));
        EXPECT_EQ(read.frame.at.count(), times[static_cast<std::size_t>(index)]);
    }
}

TEST(Recording, WithoutJsonIsAFolderOfImages) {
    const ScratchFolder folder;
    {
        auto writer = startIn(folder.path(), immediate());
        writer->push(grey(10, 0, 0ms));
        writer->push(grey(20, 1, 40ms));
        writer->finish();
    }
    std::filesystem::remove(folder.path() / "recording.json");
    std::string error;

    const auto source = frame_io::openReplay(folder.path(), error);

    ASSERT_NE(source, nullptr) << error;
    EXPECT_EQ(source->frameCount(), std::optional<std::uint64_t>(2));
}

// A live source's frame finds the queue full and is dropped from the recording; a replay waits for room.
TEST(Recording, FullQueueSaysFull) {
    const ScratchFolder folder;
    test_support::Latch latch;
    RecordingWriter::Options options = threaded();
    options.threads = 1;
    options.queueBytes = kGreyBytes; // one frame, queued or being written
    options.beforeWrite = [&latch](std::uint64_t) { latch.hold(); };
    auto writer = startIn(folder.path(), options);

    ASSERT_EQ(writer->push(grey(10, 0, 0ms)), RecordingWriter::Push::Queued);
    ASSERT_TRUE(latch.waitUntilEntered());
    EXPECT_EQ(writer->push(grey(20, 1, 40ms)), RecordingWriter::Push::Full);
    latch.release();

    EXPECT_TRUE(writer->waitForSpace(10s));
    EXPECT_EQ(writer->push(grey(30, 2, 80ms)), RecordingWriter::Push::Queued);
    writer->finish();
    EXPECT_EQ(writer->progress().written, 2U);
}

// Room for the next frame, not merely bytes under the bar: a queue of one and a half frames holding one has
// room left, but not for another, so a replay that waits for it would otherwise wake at once and push in
// vain.
TEST(Recording, WaitForSpaceWaitsForRoomForTheFrame) {
    const ScratchFolder folder;
    test_support::Latch latch;
    RecordingWriter::Options options = threaded();
    options.threads = 1;
    options.queueBytes = kGreyBytes * 3 / 2;
    options.beforeWrite = [&latch](std::uint64_t) { latch.hold(); };
    auto writer = startIn(folder.path(), options);

    ASSERT_EQ(writer->push(grey(10, 0, 0ms)), RecordingWriter::Push::Queued);
    ASSERT_TRUE(latch.waitUntilEntered());
    ASSERT_EQ(writer->push(grey(20, 1, 40ms)), RecordingWriter::Push::Full);

    EXPECT_FALSE(writer->waitForSpace(0ms, kGreyBytes));
    latch.release();
    EXPECT_TRUE(writer->waitForSpace(10s, kGreyBytes));
    EXPECT_EQ(writer->push(grey(20, 1, 40ms)), RecordingWriter::Push::Queued);
}

TEST(Recording, ImmediateQueuesUntilPumped) {
    const ScratchFolder folder;
    RecordingWriter::Options options = immediate();
    options.queueBytes = kGreyBytes;
    auto writer = startIn(folder.path(), options);

    EXPECT_EQ(writer->push(grey(10, 0, 0ms)), RecordingWriter::Push::Queued);
    EXPECT_EQ(writer->push(grey(20, 1, 40ms)), RecordingWriter::Push::Full);
    EXPECT_TRUE(writer->pump());
    EXPECT_EQ(writer->push(grey(30, 2, 80ms)), RecordingWriter::Push::Queued);
    EXPECT_EQ(writer->progress().written, 1U);
    EXPECT_EQ(writer->progress().queued, 1U);
}

TEST(Recording, LowDiskStops) {
    const ScratchFolder folder;
    RecordingWriter::Options options = immediate();
    options.freeSpace = [](const std::filesystem::path&) { return std::uintmax_t{4'999'999'999}; };
    auto writer = startIn(folder.path(), options);

    EXPECT_EQ(writer->push(grey(10, 0, 0ms)), RecordingWriter::Push::LowDisk);
}

#if !defined(_WIN32)
// On Windows the files inherit the recording folder's access list, which the app sets.
TEST(Recording, FilesAreOwnerOnly) {
    const ScratchFolder folder;
    auto writer = startIn(folder.path(), immediate());
    writer->push(grey(10, 0, 0ms));
    writer->finish();

    for (const auto& entry : std::filesystem::directory_iterator(folder.path())) {
        const auto permissions = entry.status().permissions();
        EXPECT_EQ(permissions & std::filesystem::perms::all,
                  std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
            << entry.path();
    }
}
#endif

TEST(Recording, FinishAsyncReportsProgress) {
    const ScratchFolder folder;
    test_support::Latch latch;
    RecordingWriter::Options options = threaded();
    options.beforeWrite = [&latch](std::uint64_t) { latch.hold(); };
    auto writer = startIn(folder.path(), options);
    writer->push(grey(10, 0, 0ms));
    ASSERT_TRUE(latch.waitUntilEntered());

    writer->finishAsync();
    EXPECT_FALSE(writer->progress().finished);
    EXPECT_EQ(jsonIn(folder.path())["complete"], false);

    latch.release();
    writer->finish();
    EXPECT_TRUE(writer->progress().finished);
    EXPECT_EQ(jsonIn(folder.path())["complete"], true);
}

TEST(Recording, DestructionFinishesTheRecording) {
    const ScratchFolder folder;
    {
        auto writer = startIn(folder.path(), threaded());
        for (int index = 0; index < 5; ++index) {
            writer->push(grey(10 * index, static_cast<std::uint64_t>(index), 40ms * index));
        }
    }

    const nlohmann::json json = jsonIn(folder.path());
    EXPECT_EQ(json["complete"], true);
    EXPECT_EQ(json["frames"].size(), 5U);
    EXPECT_TRUE(std::filesystem::exists(folder.path() / "000004.png"));
}

TEST(Replay, OpensEachKind) {
    const ScratchFolder folder;
    ASSERT_TRUE(cv::imwrite(folder.file("still.png"), cv::Mat(4, 4, CV_8UC3, cv::Scalar::all(9))));
    {
        cv::VideoWriter video(folder.file("clip.avi"), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 25.0,
                              cv::Size(16, 16));
        for (int index = 0; index < 3; ++index) {
            video.write(cv::Mat(16, 16, CV_8UC3, cv::Scalar::all(index * 50)));
        }
    }
    const std::filesystem::path images = folder.path() / "images";
    std::filesystem::create_directory(images);
    ASSERT_TRUE(cv::imwrite((images / "1.png").string(), cv::Mat(4, 4, CV_8UC1, cv::Scalar(1))));
    const std::filesystem::path recording = folder.path() / "recording";
    std::filesystem::create_directory(recording);
    {
        auto writer = startIn(recording, immediate());
        writer->push(grey(10, 0, 0ms));
        writer->finish();
    }

    for (const std::filesystem::path& path :
         {folder.path() / "still.png", folder.path() / "clip.avi", images, recording}) {
        std::string error;
        const auto source = frame_io::openReplay(path, error);
        ASSERT_NE(source, nullptr) << path << ": " << error;
        EXPECT_EQ(source->next().status, ReadStatus::Frame) << path;
    }
}

} // namespace
