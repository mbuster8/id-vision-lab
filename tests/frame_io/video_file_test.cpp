#include "frame_io/video_file.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace {

// A folder of its own under the system's temporary one, removed afterwards. Random, as test programs run in
// parallel.
class ScratchFolder {
public:
    ScratchFolder()
        : m_path(std::filesystem::temp_directory_path() /
                 ("live_sender_video_" + std::to_string(std::random_device{}()))) {
        std::filesystem::create_directories(m_path);
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;
    ~ScratchFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    std::string file(const std::string& name) const { return (m_path / name).string(); }

private:
    std::filesystem::path m_path;
};

const int kMjpeg = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');

// Sixteen 64×48 frames, each a different colour, written by the given backend.
bool writeClip(const std::string& path, int api) {
    cv::VideoWriter writer(path, api, kMjpeg, 30.0, cv::Size(64, 48));
    if (!writer.isOpened()) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        writer.write(cv::Mat(48, 64, CV_8UC3, cv::Scalar(i * 10, 100, 200)));
    }
    writer.release();
    return true;
}

int framesIn(cv::VideoCapture& capture) {
    int frames = 0;
    cv::Mat frame;
    while (capture.read(frame)) {
        ++frames;
    }
    return frames;
}

// OpenCV's own MJPEG writer is in every build, so this clip can be made anywhere.
TEST(VideoFile, OpensAClipAndReadsEveryFrame) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.avi");
    ASSERT_TRUE(writeClip(path, cv::CAP_OPENCV_MJPEG));
    cv::VideoCapture capture;

    ASSERT_TRUE(frame_io::openVideoFile(capture, path));

    EXPECT_EQ(framesIn(capture), 16);
}

#if defined(__APPLE__)
// (F9) AVFoundation decodes on the Mac's media engine and keeps HDR colours close to QuickTime's.
TEST(VideoFile, PrefersAVFoundationOnMacOS) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.avi");
    ASSERT_TRUE(writeClip(path, cv::CAP_OPENCV_MJPEG));
    cv::VideoCapture capture;

    ASSERT_TRUE(frame_io::openVideoFile(capture, path));

    EXPECT_EQ(capture.getBackendName(), "AVFOUNDATION");
}

// A file AVFoundation cannot read, such as Matroska, still opens through OpenCV's default backend.
TEST(VideoFile, FallsBackWhenAVFoundationCannotRead) {
    const ScratchFolder folder;
    const std::string path = folder.file("clip.mkv");
    ASSERT_TRUE(writeClip(path, cv::CAP_FFMPEG));
    cv::VideoCapture capture;

    ASSERT_TRUE(frame_io::openVideoFile(capture, path));

    EXPECT_EQ(capture.getBackendName(), "FFMPEG");
    EXPECT_EQ(framesIn(capture), 16);
}
#endif

TEST(VideoFile, ReportsAFileNothingOpens) {
    const ScratchFolder folder;
    cv::VideoCapture capture;

    EXPECT_FALSE(frame_io::openVideoFile(capture, folder.file("missing.mov")));
}

} // namespace
