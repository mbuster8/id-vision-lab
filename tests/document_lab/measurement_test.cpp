// Measurements of the Document Lab's stages, run by hand in a release build:
//   document_lab_ui_tests --gtest_also_run_disabled_tests --gtest_filter=*Measurement*
// Replays read the clips named by DOCUMENT_LAB_CLIP_4K60, DOCUMENT_LAB_CLIP_1080P30 and
// DOCUMENT_LAB_CLIP_SDR, each skipped when unset. They print their numbers and assert nothing about them.

#include "camera_frame.h"
#include "card_frame.h"
#include "live_converter.h"
#include "test_support.h"

#if defined(FRAME_IO_HAS_FFMPEG)
#include <frame_io/ffmpeg_video.h>
#endif
#include <frame_io/image_folder.h>
#include <frame_io/recording.h>
#include <frame_io/video_file.h>
#include <lab_core/document_outline.h>
#include <lab_core/engine.h>
#include <lab_core/processors.h>

#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// The mean of `runs` calls, in milliseconds, after one call that warms the caches.
double meanMs(int runs, const std::function<void()>& call) {
    call();
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < runs; ++i) {
        call();
    }
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count() / runs;
}

// A 1920×1440 NV12 frame of the card, as the C920's largest 4:3 format delivers it.
QVideoFrame nv12Card(QVideoFrameFormat::ColorSpace space, QVideoFrameFormat::ColorRange range) {
    const cv::Mat bgr = test_support::cardFrame(3);
    cv::Mat i420;
    cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
    QVideoFrameFormat format(QSize(bgr.cols, bgr.rows), QVideoFrameFormat::Format_NV12);
    format.setColorSpace(space);
    format.setColorRange(range);
    QVideoFrame frame(format);
    EXPECT_TRUE(frame.map(QVideoFrame::WriteOnly));
    const int width = bgr.cols;
    const int height = bgr.rows;
    for (int row = 0; row < height; ++row) {
        std::memcpy(frame.bits(0) + static_cast<std::ptrdiff_t>(row) * frame.bytesPerLine(0), i420.ptr(row),
                    static_cast<std::size_t>(width));
    }
    const std::uint8_t* u = i420.ptr(height);
    const std::uint8_t* v = u + static_cast<std::ptrdiff_t>(width / 2) * (height / 2);
    for (int row = 0; row < height / 2; ++row) {
        std::uint8_t* uv = frame.bits(1) + static_cast<std::ptrdiff_t>(row) * frame.bytesPerLine(1);
        for (int x = 0; x < width / 2; ++x) {
            uv[2 * x] = u[row * (width / 2) + x];
            uv[2 * x + 1] = v[row * (width / 2) + x];
        }
    }
    frame.unmap();
    return frame;
}

TEST(DocumentLabMeasurement, DISABLED_CameraFrameConversion) {
    for (const auto& [name, space, range] : {std::tuple{"BT.601 video", QVideoFrameFormat::ColorSpace_BT601,
                                                        QVideoFrameFormat::ColorRange_Video},
                                             std::tuple{"BT.709 video", QVideoFrameFormat::ColorSpace_BT709,
                                                        QVideoFrameFormat::ColorRange_Video},
                                             std::tuple{"BT.709 full", QVideoFrameFormat::ColorSpace_BT709,
                                                        QVideoFrameFormat::ColorRange_Full}}) {
        const QVideoFrame frame = nv12Card(space, range);
        const double ms = meanMs(50, [&frame] {
            std::string error;
            const auto converted = document_lab::toFrame(frame, {}, 0ns, error);
            ASSERT_TRUE(converted) << error;
        });
        std::cout << "[ measurement ] 1920×1440 NV12 " << name << " to BGR: " << ms << " ms\n";
    }
}

TEST(DocumentLabMeasurement, DISABLED_Processors) {
    const frame_io::Frame frame{.image = test_support::cardFrame(3), .number = 0, .at = 0ns, .tags = {}};
    for (const lab_core::ProcessorKind kind :
         {lab_core::ProcessorKind::Edges, lab_core::ProcessorKind::Outline}) {
        const auto processor = lab_core::makeProcessor(kind);
        const double ms = meanMs(50, [&] { processor->process(frame); });
        std::cout << "[ measurement ] " << lab_core::nameOf(kind) << " on 1920×1440: " << ms << " ms\n";
    }
}

// The picture of a 1920×1440 frame for a canvas showing it at 1280×960, as the window makes it.
TEST(DocumentLabMeasurement, DISABLED_CanvasConversion) {
    const cv::Mat frame = test_support::cardFrame(3);
    const LiveJob job{.viewCounter = 0,
                      .channel = "Frame",
                      .frameNumber = 0,
                      .frame = frame,
                      .settings = {},
                      .tags = {},
                      .area = {},
                      .drawnSize = cv::Size(1280, 960),
                      .showClipping = false,
                      .histogram = false,
                      .keep = ImageView::KeepView::IfSameSize};
    const double ms = meanMs(50, [&job] { convertLiveFrame(job); });
    std::cout << "[ measurement ] canvas picture of 1920×1440 at 1280×960: " << ms << " ms\n";
}

// Every frame of a clip read as fast as possible, alone and through the engine with Outline.
TEST(DocumentLabMeasurement, DISABLED_ReplayAsFastAsPossible) {
    for (const char* variable :
         {"DOCUMENT_LAB_CLIP_4K60", "DOCUMENT_LAB_CLIP_1080P30", "DOCUMENT_LAB_CLIP_SDR"}) {
        const std::optional<std::string> clip = test_support::environmentValue(variable);
        if (!clip) {
            std::cout << "[ measurement ] " << variable << " not set: skipped\n";
            continue;
        }
        std::string error;
        std::unique_ptr<frame_io::ReplaySource> source = frame_io::openReplay(*clip, error);
        ASSERT_TRUE(source) << error;
        int frames = 0;
        const Clock::time_point start = Clock::now();
        while (source->next().status == frame_io::ReadStatus::Frame) {
            ++frames;
        }
        const double readSeconds = std::chrono::duration<double>(Clock::now() - start).count();

        lab_core::Engine engine(lab_core::Engine::Mode::Immediate, {});
        engine.setAsFastAsPossible(true);
        engine.setProcessor(lab_core::makeProcessor(lab_core::ProcessorKind::Outline));
        const std::string path = *clip;
        engine.startReplay([path](std::string& openError) { return frame_io::openReplay(path, openError); });
        int processed = 0;
        const Clock::time_point engineStart = Clock::now();
        while (engine.status().state != lab_core::EngineState::Ended &&
               engine.status().state != lab_core::EngineState::Stopped) {
            engine.pump();
            if (engine.takeResult()) {
                ++processed;
            }
        }
        const double engineSeconds = std::chrono::duration<double>(Clock::now() - engineStart).count();
        std::cout << "[ measurement ] " << variable << ": " << frames << " frames read at "
                  << frames / readSeconds << " fps; through the engine with Outline at "
                  << processed / engineSeconds << " fps\n";
    }
}

#if defined(FRAME_IO_HAS_FFMPEG)

// Milliseconds for a seek and the read of the frame it lands on, as the replay shows it.
double seekMs(frame_io::FfmpegVideo& video, std::chrono::nanoseconds at) {
    const Clock::time_point start = Clock::now();
    EXPECT_TRUE(video.seek(at)) << video.lastError();
    EXPECT_TRUE(video.read().has_value()) << video.lastError();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::chrono::nanoseconds frameTime(double frame, double fps) {
    return std::chrono::nanoseconds(std::llround(frame * 1e9 / fps));
}

// A seek in each clip, with the hardware decoder and in software: the first after opening, a later one, and
// ten Step Backs one after another, each its own seek (the engine makes one of those waiting, so this is the
// most a held key can cost).
TEST(DocumentLabMeasurement, DISABLED_SeekInAClip) {
    for (const char* variable : {"DOCUMENT_LAB_CLIP_4K60", "DOCUMENT_LAB_CLIP_SDR"}) {
        const std::optional<std::string> clip = test_support::environmentValue(variable);
        if (!clip) {
            std::cout << "[ measurement ] " << variable << " not set: skipped\n";
            continue;
        }
        for (const bool hardware : {true, false}) {
            std::string error;
            const auto video = frame_io::FfmpegVideo::open(
                *clip, frame_io::FfmpegVideo::Options{.allowHardware = hardware, .eightBitAsEightBit = true},
                error);
            ASSERT_TRUE(video) << error;
            const double fps = video->info().fps;
            const double frames = static_cast<double>(video->info().frameCount.value_or(300));
            const double first = seekMs(*video, frameTime(frames / 2, fps));
            const double later = seekMs(*video, frameTime(frames / 3, fps));
            const double midGop = seekMs(*video, std::chrono::milliseconds(1930));
            double backs = 0.0;
            const double from = std::floor(frames * 2 / 3);
            for (int step = 1; step <= 10; ++step) {
                backs += seekMs(*video, frameTime(from - step, fps));
            }
            std::cout << "[ measurement ] " << variable << " (" << video->info().decoder << ", "
                      << video->info().size.width << "×" << video->info().size.height << ", " << fps
                      << " fps): first seek " << first << " ms, a later one " << later << " ms, to 1.93 s "
                      << midGop << " ms; ten Step Backs " << backs << " ms, " << backs / 10 << " ms each\n";
        }
    }
}

// The 8-bit clip read whole through FFmpeg as 8-bit, with the hardware decoder and in software, and through
// OpenCV, as A2 read it.
TEST(DocumentLabMeasurement, DISABLED_ReadEightBitThroughFfmpegAndOpenCv) {
    const std::optional<std::string> clip = test_support::environmentValue("DOCUMENT_LAB_CLIP_SDR");
    if (!clip) {
        std::cout << "[ measurement ] DOCUMENT_LAB_CLIP_SDR not set: skipped\n";
        return;
    }
    for (const bool hardware : {true, false}) {
        std::string error;
        const auto video = frame_io::FfmpegVideo::open(
            *clip, frame_io::FfmpegVideo::Options{.allowHardware = hardware, .eightBitAsEightBit = true},
            error);
        ASSERT_TRUE(video) << error;
        int frames = 0;
        const Clock::time_point start = Clock::now();
        while (video->read()) {
            ++frames;
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        std::cout << "[ measurement ] SDR clip through FFmpeg, 8-bit (" << video->info().decoder
                  << "): " << frames << " frames at " << frames / seconds << " fps\n";
    }
    cv::VideoCapture capture;
    ASSERT_TRUE(frame_io::openVideoFile(capture, *clip));
    int frames = 0;
    cv::Mat image;
    const Clock::time_point start = Clock::now();
    while (capture.read(image) && !image.empty()) {
        ++frames;
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "[ measurement ] SDR clip through OpenCV (" << capture.getBackendName() << "): " << frames
              << " frames at " << frames / seconds << " fps\n";
}

#endif

} // namespace
