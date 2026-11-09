#include "frame_io/video_source.h"

#include "frame_io/video_file.h"
#include "scratch_folder.h"
#if defined(FRAME_IO_HAS_FFMPEG)
#include "ffmpeg_clip.h"
#include "frame_io/ffmpeg_video.h"
#endif

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frame_io::ReadStatus;
using test_support::ScratchFolder;

// An MJPG clip OpenCV writes and reads with its own codec on every platform: `count` grey frames at 25 fps.
std::string writeEightBitClip(const ScratchFolder& folder, int count) {
    const std::string path = folder.file("clip.avi");
    cv::VideoWriter writer(path, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 25.0, cv::Size(64, 48));
    EXPECT_TRUE(writer.isOpened());
    for (int i = 0; i < count; ++i) {
        writer.write(cv::Mat(48, 64, CV_8UC3, cv::Scalar::all(i * 20)));
    }
    return path;
}

// Ten 8-bit frames, numbered from 0 and 40 ms apart, then the end.
void expectTenEightBitFrames(frame_io::ReplaySource& source) {
    for (std::uint64_t number = 0; number < 10; ++number) {
        const frame_io::ReadResult result = source.next();
        ASSERT_EQ(result.status, ReadStatus::Frame) << result.error;
        EXPECT_EQ(result.frame.number, number);
        EXPECT_EQ(result.frame.at, std::chrono::milliseconds(40) * number);
        EXPECT_EQ(result.frame.image.type(), CV_8UC3);
    }
    EXPECT_EQ(source.next().status, ReadStatus::End);
    EXPECT_EQ(source.describe(), "clip.avi");
}

#if defined(FRAME_IO_HAS_FFMPEG)
// Every video goes through FFmpeg, so it can seek; an 8-bit SDR one still comes as 8-bit frames.
TEST(VideoSource, EightBitClipComesThroughFfmpegAsEightBit) {
#else
TEST(VideoSource, EightBitClipComesThroughOpenCv) {
#endif
    const ScratchFolder folder;
    const std::string path = writeEightBitClip(folder, 10);
    std::string error;
    const auto source = frame_io::openVideo(path, error);
    ASSERT_NE(source, nullptr) << error;

    expectTenEightBitFrames(*source);
}

// Restart opens the file again from its first frame.
TEST(VideoSource, RestartReopens) {
    const ScratchFolder folder;
    const std::string path = writeEightBitClip(folder, 5);
    std::string error;
    const auto source = frame_io::openVideo(path, error);
    ASSERT_NE(source, nullptr) << error;
    source->next();
    source->next();

    ASSERT_TRUE(source->restart(error)) << error;

    const frame_io::ReadResult first = source->next();
    ASSERT_EQ(first.status, ReadStatus::Frame);
    EXPECT_EQ(first.frame.number, 0U);
    EXPECT_EQ(first.frame.at, std::chrono::nanoseconds(0));
}

TEST(VideoSource, MissingFileIsAnError) {
    const ScratchFolder folder;
    std::string error;

    EXPECT_EQ(frame_io::openVideo(folder.path() / "missing.mov", error), nullptr);
    EXPECT_FALSE(error.empty());
}

// A file cut short part-way (a copy that stopped, a full disk) stops the replay with an error, not as if the
// video had ended: its header still says how many frames it should hold, and the reader comes back with
// fewer. FFmpeg itself reports most damage as the end of the file, so the count is what tells them apart.
TEST(VideoSource, ClipCutShortIsAnError) {
    const ScratchFolder folder;
    const std::string path = writeEightBitClip(folder, 30);
    std::filesystem::resize_file(path, std::filesystem::file_size(path) / 2);
    std::string error;
    const auto source = frame_io::openVideo(path, error);
    ASSERT_NE(source, nullptr) << error;
    ASSERT_EQ(source->frameCount(), std::optional<std::uint64_t>(30));

    frame_io::ReadResult result = source->next();
    int frames = 0;
    while (result.status == ReadStatus::Frame) {
        ++frames;
        result = source->next();
    }

    EXPECT_LT(frames, 30);
    EXPECT_EQ(result.status, ReadStatus::Error);
    // FFmpeg may report the damage itself, or the count does ("ended after 14 of 30 frames").
    EXPECT_NE(result.error.find("clip.avi"), std::string::npos) << result.error;
}

#if defined(FRAME_IO_HAS_FFMPEG)

// FFmpeg's reading of an 8-bit clip matches OpenCV's, which the replay used before.
TEST(VideoSource, EightBitClipMatchesOpenCv) {
    const ScratchFolder folder;
    const std::string path = writeEightBitClip(folder, 10);
    std::string error;
    const auto source = frame_io::openVideo(path, error);
    ASSERT_NE(source, nullptr) << error;
    cv::VideoCapture capture;
    ASSERT_TRUE(frame_io::openVideoFile(capture, path));

    int frames = 0;
    cv::Mat expected;
    while (capture.read(expected) && !expected.empty()) {
        const frame_io::ReadResult read = source->next();
        ASSERT_EQ(read.status, ReadStatus::Frame) << read.error;
        ASSERT_EQ(read.frame.image.size(), expected.size());
        EXPECT_LE(cv::norm(read.frame.image, expected, cv::NORM_L1) /
                      static_cast<double>(expected.total() * 3),
                  2.0)
            << "frame " << frames;
        ++frames;
    }
    EXPECT_EQ(frames, 10);
    EXPECT_EQ(source->next().status, ReadStatus::End);
}

// A phone's portrait video, stored on its side with a display matrix, replays upright in 8 bits.
TEST(VideoSource, RotatedEightBitClipIsUpright) {
    const ScratchFolder folder;
    const std::string path = folder.file("portrait.mov");
    ASSERT_TRUE(test_support::writeClip(
        path, test_support::Clip{.format = AV_PIX_FMT_YUV420P, .u = 128, .v = 128, .rotation = 90.0}));
    std::string error;
    const auto source = frame_io::openVideo(path, error);
    ASSERT_NE(source, nullptr) << error;

    const frame_io::ReadResult read = source->next();

    ASSERT_EQ(read.status, ReadStatus::Frame) << read.error;
    EXPECT_EQ(read.frame.image.type(), CV_8UC3);
    EXPECT_EQ(read.frame.image.size(), cv::Size(48, 64));
}

// Sixty frames at 30 fps, each its own luma, keyframes every twelve.
std::string writeNumbered(const ScratchFolder& folder) {
    const std::string path = folder.file("numbered.mkv");
    EXPECT_EQ(test_support::writeNumberedClip(path, {}), test_support::Written::Yes);
    return path;
}

TEST(VideoSource, CanSeekAndHasARate) {
    const ScratchFolder folder;
    const std::string path = writeNumbered(folder);
    std::string error;
    const auto source = frame_io::openVideo(path, error);
    ASSERT_NE(source, nullptr) << error;

    EXPECT_TRUE(source->canSeek());
    EXPECT_EQ(source->frameRate(), std::optional<double>(30.0));
}

TEST(VideoSource, SeeksAndNumbersFromTheTarget) {
    const ScratchFolder folder;
    const std::string path = writeNumbered(folder);
    std::string error;
    const auto sequential = frame_io::openVideo(path, error);
    ASSERT_NE(sequential, nullptr) << error;
    frame_io::ReadResult expected;
    for (int index = 0; index <= 37; ++index) {
        expected = sequential->next();
        ASSERT_EQ(expected.status, ReadStatus::Frame) << expected.error;
    }
    const auto source = frame_io::openVideo(path, error);
    ASSERT_NE(source, nullptr) << error;

    source->seek(37);
    const frame_io::ReadResult landed = source->next();

    ASSERT_EQ(landed.status, ReadStatus::Frame) << landed.error;
    EXPECT_EQ(landed.frame.number, 37U);
    EXPECT_EQ(landed.frame.at, expected.frame.at);
    EXPECT_EQ(cv::norm(landed.frame.image, expected.frame.image, cv::NORM_INF), 0.0);
    EXPECT_EQ(source->next().frame.number, 38U);
}

// The frames after a seek count towards the file's frame count from the target, so its end is not taken for
// a file cut short.
TEST(VideoSource, NormalEndAfterASeekIsTheEnd) {
    const ScratchFolder folder;
    const std::string path = writeNumbered(folder);
    std::string error;
    const auto source = frame_io::openVideo(path, error);
    ASSERT_NE(source, nullptr) << error;

    source->seek(50);
    int frames = 0;
    frame_io::ReadResult read = source->next();
    while (read.status == ReadStatus::Frame) {
        ++frames;
        read = source->next();
    }

    EXPECT_EQ(frames, 10);
    EXPECT_EQ(read.status, ReadStatus::End) << read.error;
}

// Holds the first seek on the reading thread until the source abandons it, or ten seconds pass, so a test
// sees whether destroying, restarting or seeking again waits for it. Declare it before the source.
class HeldSeek {
public:
    frame_io::VideoOptions options() {
        return {.beforeSeek = [this](const std::function<bool()>& abandoned) {
            {
                const std::lock_guard lock(m_mutex);
                if (++m_calls > 1) {
                    return;
                }
                m_entered = true;
            }
            m_changed.notify_all();
            const auto until = std::chrono::steady_clock::now() + 10s;
            while (!abandoned() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(1ms);
            }
        }};
    }

    bool waitUntilEntered() {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, 10s, [this] { return m_entered; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    int m_calls = 0;
    bool m_entered = false;
};

TEST(VideoSource, DestroyAbandonsAHeldSeek) {
    const ScratchFolder folder;
    const std::string path = writeNumbered(folder);
    HeldSeek held;
    std::string error;
    auto source = frame_io::openVideo(path, error, held.options());
    ASSERT_NE(source, nullptr) << error;
    source->seek(20);
    ASSERT_TRUE(held.waitUntilEntered());

    const auto started = std::chrono::steady_clock::now();
    source.reset();

    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
}

TEST(VideoSource, RestartAbandonsAHeldSeek) {
    const ScratchFolder folder;
    const std::string path = writeNumbered(folder);
    HeldSeek held;
    std::string error;
    const auto source = frame_io::openVideo(path, error, held.options());
    ASSERT_NE(source, nullptr) << error;
    source->seek(20);
    ASSERT_TRUE(held.waitUntilEntered());

    const auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(source->restart(error)) << error;

    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    EXPECT_EQ(source->next().frame.number, 0U);
}

TEST(VideoSource, NewerSeekAbandonsAHeldSeek) {
    const ScratchFolder folder;
    const std::string path = writeNumbered(folder);
    HeldSeek held;
    std::string error;
    const auto source = frame_io::openVideo(path, error, held.options());
    ASSERT_NE(source, nullptr) << error;
    source->seek(10);
    ASSERT_TRUE(held.waitUntilEntered());

    const auto started = std::chrono::steady_clock::now();
    source->seek(40);

    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    const frame_io::ReadResult read = source->next();
    ASSERT_EQ(read.status, ReadStatus::Frame) << read.error;
    EXPECT_EQ(read.frame.number, 40U);
}

// A 10-bit or HLG video goes through FFmpeg's libraries, as 16-bit frames with its tags.
TEST(VideoSource, TenBitClipComesThroughFfmpeg) {
    const ScratchFolder folder;
    const std::string path = folder.file("hlg.mkv");
    ASSERT_TRUE(test_support::writeClip(path, test_support::Clip{.matrix = AVCOL_SPC_BT2020_NCL,
                                                                 .transfer = AVCOL_TRC_ARIB_STD_B67,
                                                                 .primaries = AVCOL_PRI_BT2020}));
    std::string error;
    const auto source = frame_io::openVideo(path, error);
    ASSERT_NE(source, nullptr) << error;

    int frames = 0;
    for (frame_io::ReadResult result = source->next(); result.status == ReadStatus::Frame;
         result = source->next()) {
        EXPECT_EQ(result.frame.image.type(), CV_16UC3);
        EXPECT_EQ(result.frame.tags.transfer, visual_debug::Transfer::Hlg);
        EXPECT_EQ(result.frame.number, static_cast<std::uint64_t>(frames));
        ++frames;
    }
    EXPECT_EQ(frames, test_support::kClipFrames);
}

// FFmpeg reports most damage to a 10-bit or HDR file as its end, so such a file needs its header's frame
// count as much as an 8-bit one does to stop with an error when it is cut short (ClipCutShortIsAnError).
TEST(VideoSource, TenBitClipKnowsItsFrameCount) {
    const ScratchFolder folder;
    const std::string path = folder.file("hlg.mkv");
    ASSERT_TRUE(test_support::writeClip(path, test_support::Clip{.transfer = AVCOL_TRC_ARIB_STD_B67}));
    std::string error;
    const auto source = frame_io::openVideo(path, error);
    ASSERT_NE(source, nullptr) << error;

    EXPECT_EQ(source->frameCount(), std::optional<std::uint64_t>(test_support::kClipFrames));
    ASSERT_TRUE(source->restart(error)) << error;
    EXPECT_EQ(source->frameCount(), std::optional<std::uint64_t>(test_support::kClipFrames));
}

// FFmpeg tells a failed read from the end of the file; only the end leaves lastError empty.
TEST(FfmpegVideo, LastErrorEmptyAtTheEnd) {
    const ScratchFolder folder;
    const std::string path = folder.file("plain.mkv");
    ASSERT_TRUE(test_support::writeClip(path, test_support::Clip{}));
    std::string error;
    const auto video = frame_io::FfmpegVideo::open(path, false, error);
    ASSERT_NE(video, nullptr) << error;

    while (video->read()) {
    }

    EXPECT_TRUE(video->lastError().empty()) << video->lastError();
}

#endif

} // namespace
