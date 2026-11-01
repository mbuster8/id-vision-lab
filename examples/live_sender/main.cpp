// Streams frames to the viewer as live channels: each frame as "camera", its Canny edges as "edges", or as
// NAME and "NAME edges" with --name, so several can share a viewer. The program has no window of its own:
// the viewer is its screen, with Freeze, Snapshot and the display range.
//
//     live_sender [--camera N | [--hdr] <video file> | --pattern [--size WxH] [--fps N]] [--frames N]
//                 [--name NAME] [--endpoint <uri>]
//     live_sender --list-cameras
//
// Without a source it reads camera 0. A camera runs until Ctrl+C and a file until its end, unless --frames
// stops them sooner; a summary prints either way. A file plays in real time, read ahead on a thread of its
// own: a frame that is ready more than a frame late is skipped and counted, never counted as undelivered.
// With --hdr the file is read with FFmpeg's libraries instead of OpenCV's, as 16-bit frames tagged with the
// video's transfer, primaries and nominal black and white (a live_sender built without them says so).
// The pattern is sent at --fps (30 by default). Exit code: 0 when every frame sent was delivered, 1 when any
// was not, 2 for bad arguments or an unreadable source.

#include "live_sender.h"
#include "read_ahead.h"
#include "video_file.h"
#if defined(LIVE_SENDER_HAS_FFMPEG)
#include "ffmpeg_video.h"
#endif

#include "visual_debug/client.h"

#include <opencv2/core/utils/logger.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr int kExitAllDelivered = 0;
constexpr int kExitNotDelivered = 1;
constexpr int kExitUsageError = 2;
constexpr int kCamerasToTry = 10;
constexpr auto kStopCheckInterval = 50ms; // how quickly Ctrl+C ends a wait between slow frames
constexpr std::size_t kFramesReadAhead = 3;

constexpr const char* kUsage =
    "usage: live_sender [--camera N | [--hdr] <video file> | --pattern [--size WxH] [--fps N]] [--frames N]\n"
    "                   [--name NAME] [--endpoint <uri>]\n"
    "       live_sender --list-cameras\n";

// A video's frame and when it is due, from the start of the video.
struct TimedFrame {
    cv::Mat frame;
    Clock::duration at;
};

#if defined(__APPLE__)
constexpr bool kCamerasNumberedWithoutGaps = true; // AVFoundation numbers the cameras it finds
#else
constexpr bool kCamerasNumberedWithoutGaps = false;
#endif

// Set by Ctrl+C. A signal handler may only touch a lock-free atomic.
std::atomic<bool> stopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void requestStop(int) { stopRequested = true; }

// OpenCV's logger warns for every camera number that does not open; the program says so itself. (Its
// AVFoundation code also prints two lines of its own for such a number, which no setting silences.)
void silenceOpenCv() { cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT); }

int listCameras() {
    silenceOpenCv();
    const auto open = [](int index) -> std::optional<cv::Size> {
        cv::VideoCapture camera(index);
        if (!camera.isOpened()) {
            return std::nullopt;
        }
        return cv::Size(static_cast<int>(camera.get(cv::CAP_PROP_FRAME_WIDTH)),
                        static_cast<int>(camera.get(cv::CAP_PROP_FRAME_HEIGHT)));
    };
    const auto found = live_sender::listCameras(open, kCamerasToTry, kCamerasNumberedWithoutGaps);
    for (const auto& [index, size] : found) {
        std::cout << "--camera " << index << ": " << size.width << 'x' << size.height << '\n';
    }
    if (found.empty()) {
        std::cout << "No camera opened (numbers 0 to " << kCamerasToTry - 1 << " tried).\n";
    }
    return kExitAllDelivered;
}

// The interval between a video's frames, unless its frame rate is unknown or unlikely.
std::optional<Clock::duration> intervalOf(double fps) {
    if (fps < live_sender::kMinPatternFps || fps > live_sender::kMaxPatternFps) {
        return std::nullopt;
    }
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
}

// Opens a camera or a video file. A file's own frame rate becomes the interval between frames; a camera
// delivers frames at its own pace, so it has none.
bool open(cv::VideoCapture& capture, const live_sender::Arguments& arguments,
          std::optional<Clock::duration>& interval) {
    try {
        if (const auto* camera = std::get_if<live_sender::CameraSource>(&arguments.source)) {
            silenceOpenCv(); // a number that does not open is reported below
            return capture.open(camera->index);
        }
        const auto& file = std::get<live_sender::FileSource>(arguments.source);
        if (!live_sender::openVideoFile(capture, file.path)) {
            return false;
        }
        interval = intervalOf(capture.get(cv::CAP_PROP_FPS));
        return true;
    } catch (const std::exception&) {
        return false; // reported by the caller like any other source that does not open
    }
}

void printTally(const std::string& channel, const live_sender::ChannelTally& tally, int skipped = 0) {
    std::cout << channel << ": " << tally.sent << " sent, " << tally.delivered << " delivered";
    for (const auto& [result, times] : tally.failed) {
        std::cout << ", " << times << ' ' << visual_debug::toString(result);
    }
    if (skipped > 0) {
        std::cout << ", " << skipped << " skipped";
    }
    std::cout << '\n';
}

// Sleeps until `due`, waking often enough to notice Ctrl+C. False if Ctrl+C came first.
bool waitUntil(Clock::time_point due) {
    while (!stopRequested && Clock::now() < due) {
        std::this_thread::sleep_for(std::min<Clock::duration>(due - Clock::now(), kStopCheckInterval));
    }
    return !stopRequested;
}

#if defined(LIVE_SENDER_HAS_FFMPEG)
const char* nameOf(visual_debug::Transfer transfer) {
    switch (transfer) {
    case visual_debug::Transfer::Hlg:
        return "HLG";
    case visual_debug::Transfer::Pq:
        return "PQ";
    case visual_debug::Transfer::Sdr:
        break;
    }
    return "SDR";
}

const char* nameOf(visual_debug::Primaries primaries) {
    return primaries == visual_debug::Primaries::Bt2020 ? "BT.2020" : "BT.709";
}

// Opens a video for --hdr and says what it is, for example "clip.MOV: 3840x2160, 10-bit, HLG, BT.2020,
// VideoToolbox". Nothing if it does not open, also said.
std::unique_ptr<live_sender::FfmpegVideo> openHdrVideo(const std::string& path) {
    std::string error;
    std::unique_ptr<live_sender::FfmpegVideo> video = live_sender::FfmpegVideo::open(path, true, error);
    if (!video) {
        std::cerr << "cannot open " << path << ": " << error << '\n';
        return nullptr;
    }
    const live_sender::VideoInfo& info = video->info();
    std::cout << std::filesystem::path(path).filename().string() << ": " << info.size.width << 'x'
              << info.size.height << ", " << info.bitDepth << "-bit, " << nameOf(info.transfer) << ", "
              << nameOf(info.primaries) << ", " << info.decoder << '\n';
    if (info.dolbyVisionProfile == 5) {
        std::cerr << "warning: Dolby Vision profile 5 has no HDR10 or HLG base layer, so its colours will be "
                     "wrong\n";
    }
    return video;
}
#endif

} // namespace

int main(int argc, char* argv[]) {
    const auto arguments = live_sender::parseArguments(argc, argv);
    if (!arguments) {
        std::cerr << kUsage;
        return kExitUsageError;
    }
    if (arguments->listCameras) {
        return listCameras();
    }
#if !defined(LIVE_SENDER_HAS_FFMPEG)
    if (arguments->hdr) {
        std::cerr << "live_sender was built without FFmpeg, so --hdr is not available\n";
        return kExitUsageError;
    }
#endif

    cv::VideoCapture capture;
#if defined(LIVE_SENDER_HAS_FFMPEG)
    std::unique_ptr<live_sender::FfmpegVideo> video; // --hdr's
#endif
    std::optional<Clock::duration> interval;
    std::function<std::optional<cv::Mat>()> next;
    std::function<std::optional<TimedFrame>()> readTimed; // a file's
    std::optional<live_sender::FramePacer> pacer;         // the pattern's
    std::optional<live_sender::ReadAhead<TimedFrame>>
        ahead; // a file's, after `capture` and `video`: it reads them
    std::optional<live_sender::Playback> playback;
    visual_debug::ImageDescription description;
    std::string described; // the description in the summary, as " (HLG, BT.2020)"
    int skipped = 0;
    if (const auto* pattern = std::get_if<live_sender::PatternSource>(&arguments->source)) {
        interval =
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / pattern->fps));
        pacer.emplace(*interval, Clock::now());
        next = [size = pattern->size, index = 0]() mutable -> std::optional<cv::Mat> {
            return live_sender::patternFrame(index++, size);
        };
    } else if (arguments->hdr) {
#if defined(LIVE_SENDER_HAS_FFMPEG)
        video = openHdrVideo(std::get<live_sender::FileSource>(arguments->source).path);
        if (!video) {
            return kExitUsageError;
        }
        const live_sender::VideoInfo& info = video->info();
        interval = intervalOf(info.fps);
        description = {.valueRange = info.valueRange, .transfer = info.transfer, .primaries = info.primaries};
        described = std::string(" (") + nameOf(info.transfer) + ", " + nameOf(info.primaries) + ")";
        readTimed = [&video]() -> std::optional<TimedFrame> {
            std::optional<live_sender::VideoFrame> frame = video->read();
            if (!frame) {
                return std::nullopt;
            }
            return TimedFrame{std::move(frame->image),
                              std::chrono::duration_cast<Clock::duration>(frame->at)};
        };
#endif
    } else {
        if (!open(capture, *arguments, interval)) {
            if (const auto* camera = std::get_if<live_sender::CameraSource>(&arguments->source)) {
                std::cerr << "cannot open camera " << camera->index
                          << " (live_sender --list-cameras shows the numbers that open)\n";
            } else {
                std::cerr << "cannot open " << std::get<live_sender::FileSource>(arguments->source).path
                          << '\n';
            }
            return kExitUsageError;
        }
        const auto read = [&capture]() -> std::optional<cv::Mat> {
            cv::Mat frame;
            if (!capture.read(frame) || frame.empty()) {
                return std::nullopt;
            }
            return frame;
        };
        if (std::holds_alternative<live_sender::CameraSource>(arguments->source)) {
            next = read; // a camera delivers at its own pace
        } else {
            readTimed = [read, interval, index = 0]() mutable -> std::optional<TimedFrame> {
                std::optional<cv::Mat> frame = read();
                if (!frame) {
                    return std::nullopt;
                }
                const Clock::duration at = interval ? *interval * index++ : Clock::duration::zero();
                return TimedFrame{std::move(*frame), at};
            };
        }
    }
    if (readTimed) {
        // A file is decoded ahead on a thread of its own and played at its own rate from its first frame: a
        // frame due long ago is skipped rather than shown late. Without a frame rate it is sent as fast as it
        // reads.
        ahead.emplace(readTimed, kFramesReadAhead);
        next = [&ahead, &playback, &skipped, interval]() -> std::optional<cv::Mat> {
            while (std::optional<TimedFrame> timed = ahead->take()) {
                if (!interval) {
                    return std::move(timed->frame);
                }
                if (!playback) {
                    playback.emplace(Clock::now() - timed->at, *interval);
                }
                if (playback->late(timed->at, Clock::now())) {
                    ++skipped;
                    continue;
                }
                if (!waitUntil(playback->due(timed->at))) {
                    return std::nullopt; // Ctrl+C
                }
                return std::move(timed->frame);
            }
            return std::nullopt;
        };
    }

    const auto waitAndContinue = [&pacer] {
        if (pacer && waitUntil(pacer->due())) {
            pacer->sent(Clock::now());
        }
        return !stopRequested;
    };

    std::signal(SIGINT, requestStop);
    visual_debug::Client client(visual_debug::ClientOptions{.endpoint = arguments->endpoint});
    const live_sender::ChannelNames names = live_sender::channelNames(arguments->name);
    const live_sender::Tally tally =
        live_sender::run(next, client, arguments->frames, waitAndContinue, names, description);

    if (tally.camera.sent == 0 && !stopRequested) {
        std::cerr << "no frame could be read\n";
        return kExitUsageError;
    }
    printTally(names.camera + described, tally.camera, skipped);
    printTally(names.edges, tally.edges);
    const bool allDelivered =
        tally.camera.delivered == tally.camera.sent && tally.edges.delivered == tally.edges.sent;
    return allDelivered ? kExitAllDelivered : kExitNotDelivered;
}
