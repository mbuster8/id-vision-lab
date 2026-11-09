// A check, outside GoogleTest so it also runs from an installed package: Qt Multimedia's backend loads in a
// process that also uses frame_io's video readers, and a video opens through frame_io, through OpenCV, and
// with FFmpeg's libraries when frame_io has them. On Windows that process must load one FFmpeg build only,
// vcpkg's, for OpenCV, frame_io and Qt's plug-in alike. Prints a line per check; exits 0 only if every check
// passes.

#include "frame_io/video_file.h"
#if defined(FRAME_IO_HAS_FFMPEG)
#include "frame_io/ffmpeg_video.h"
#endif

#include <QCoreApplication>
#include <QMediaFormat>
#include <QTemporaryDir>
#include <QtGlobal>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <iostream>
#include <memory>
#include <string>

namespace {

constexpr int kFrames = 16;

bool check(bool passed, const std::string& what) {
    std::cout << (passed ? "pass: " : "FAIL: ") << what << '\n';
    return passed;
}

// A short MJPG clip from OpenCV's own writer, which every build has, as frame_io's video_file tests write
// theirs. (Written through the default backend instead, the macOS CI runner's frame_io read it short.)
bool writeClip(const std::string& path) {
    cv::VideoWriter writer(path, cv::CAP_OPENCV_MJPEG, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 25.0,
                           cv::Size(64, 48));
    if (!writer.isOpened()) {
        return false;
    }
    for (int i = 0; i < kFrames; ++i) {
        writer.write(cv::Mat(48, 64, CV_8UC3, cv::Scalar::all(i * 10)));
    }
    return true;
}

int framesIn(cv::VideoCapture& capture) {
    int count = 0;
    cv::Mat frame;
    while (capture.read(frame) && !frame.empty()) {
        ++count;
    }
    return count;
}

// " (15 of 16)", so a failure says how far it got.
std::string countOf(int frames) {
    return " (" + std::to_string(frames) + " of " + std::to_string(kFrames) + ")";
}

std::string backendOf(const cv::VideoCapture& capture) { return " through " + capture.getBackendName(); }

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    bool passed = true;

    const auto formats = QMediaFormat().supportedFileFormats(QMediaFormat::Decode);
    std::cout << "Qt " << qVersion() << ": " << formats.size()
              << " file formats the multimedia backend decodes\n";
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // Qt 6.4's GStreamer backend lists none without GStreamer's plug-ins, which a build machine need not
    // have.
    passed = check(!formats.isEmpty(), "Qt Multimedia's backend loaded") && passed;
#endif

    const QTemporaryDir folder;
    const std::string path = (folder.path() + QStringLiteral("/smoke.avi")).toStdString();
    passed = check(writeClip(path), "OpenCV wrote a clip") && passed;

    cv::VideoCapture throughFrameIo;
    const bool openedByFrameIo = frame_io::openVideoFile(throughFrameIo, path);
    const int framesByFrameIo = openedByFrameIo ? framesIn(throughFrameIo) : 0;
    passed = check(framesByFrameIo == kFrames, "frame_io read every frame" + countOf(framesByFrameIo) +
                                                   (openedByFrameIo ? backendOf(throughFrameIo) : "")) &&
             passed;

    cv::VideoCapture throughOpenCv(path);
    const int framesByOpenCv = throughOpenCv.isOpened() ? framesIn(throughOpenCv) : 0;
    passed =
        check(framesByOpenCv == kFrames, "OpenCV read every frame" + countOf(framesByOpenCv) +
                                             (throughOpenCv.isOpened() ? backendOf(throughOpenCv) : "")) &&
        passed;

#if defined(FRAME_IO_HAS_FFMPEG)
    std::string error;
    std::unique_ptr<frame_io::FfmpegVideo> video = frame_io::FfmpegVideo::open(path, false, error);
    int frames = 0;
    while (video && video->read()) {
        ++frames;
    }
    const std::string reason = error.empty() ? std::string() : ": " + error;
    passed =
        check(frames == kFrames, "FFmpeg's libraries read every frame" + countOf(frames) + reason) && passed;
#endif

    return passed ? 0 : 1;
}
