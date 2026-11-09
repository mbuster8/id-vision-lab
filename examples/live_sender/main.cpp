// Streams frames to the viewer as live channels: each frame as NAME and its Canny edges as "NAME edges". NAME
// is --name's, else "camera N" for camera N or the video file's name, so several can share a viewer; the
// pattern's channels are "camera" and "edges". The program has no window of its own: the viewer is its
// screen, with Freeze, Snapshot and the display range.
//
//     live_sender [--camera N | [--hdr] <video file> | --pattern [--size WxH] [--fps N]] [--frames N]
//                 [--name NAME] [--outline] [--endpoint <uri>]
//     live_sender --list-cameras
//     live_sender --help
//
// Without a source it reads camera 0. A camera runs until Ctrl+C and a file until its end, unless --frames
// stops them sooner; a summary prints either way. A file plays in real time, read ahead on a thread of its
// own: a frame that is ready more than a frame late is skipped and counted, never counted as undelivered;
// when the reading itself is too slow, the video plays slower instead. With --hdr the file is read with
// FFmpeg's libraries instead of OpenCV's, as 16-bit frames tagged with the video's transfer, primaries and
// nominal black and white (a live_sender built without them says so). With --outline each frame carries the
// outline of the largest four-cornered shape in it, such as a card held to the camera, as an overlay the
// viewer draws over the frame. The pattern is sent at --fps (30 by
// default). Exit code: 0 when every frame sent was delivered, 1 when any was not, 2 for bad arguments or an
// unreadable source.

#include "frame_io/read_ahead.h"
#include "frame_io/video_file.h"
#include "live_sender.h"
#if defined(FRAME_IO_HAS_FFMPEG)
#include "frame_io/ffmpeg_video.h"
#endif

#include "visual_debug/client.h"

#include <opencv2/core/utils/logger.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
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
    "                   [--name NAME] [--outline] [--endpoint <uri>]\n"
    "       live_sender --list-cameras\n"
    "       live_sender --help\n";

// The same examples as the user guide's "Running live_sender".
constexpr const char* kExamples =
    "\n"
    "Sends each frame and its Canny edges to the Visual Debug Viewer as two live channels.\n"
    "\n"
    "examples:\n"
    "  live_sender --list-cameras             the camera numbers that open, with their sizes\n"
    "  live_sender --camera 0                 the first camera, as channels \"camera 0\" and \"camera 0 "
    "edges\"\n"
    "  live_sender --camera 1                 another camera beside it, such as an iPhone through "
    "Continuity\n"
    "                                         Camera, in channels of its own\n"
    "  live_sender --camera 1 --name phone    the same camera, as channels \"phone\" and \"phone edges\"\n"
    "  live_sender \"My video.mov\"             a video file in real time, in channels named after the file\n"
    "  live_sender --hdr \"iPhone HDR.MOV\"     an HDR video as 16-bit frames with its HLG or PQ tags\n"
    "  live_sender --outline                  the first camera, with the outline of a card or passport\n"
    "                                         held to it (whole in view, on a darker surface) drawn over\n"
    "                                         each frame\n"
    "  live_sender --pattern                  a moving test pattern, no camera needed\n"
    "  live_sender --pattern --size 3840x2160 --fps 30\n"
    "                                         a 4K load test\n"
    "  live_sender --pattern --endpoint tcp://127.0.0.1:7140\n"
    "                                         to a second viewer, started with --endpoint "
    "tcp://127.0.0.1:7140\n"
    "\n"
    "Quote a file name that has spaces in straight quotes (\"), not typographic ones.\n"
    "Ctrl+C stops it; a summary of what was sent and delivered prints either way.\n";

using live_sender::TimedFrame;

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
        if (!frame_io::openVideoFile(capture, file.path)) {
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

#if defined(FRAME_IO_HAS_FFMPEG)
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
std::unique_ptr<frame_io::FfmpegVideo> openHdrVideo(const std::string& path) {
    std::string error;
    std::unique_ptr<frame_io::FfmpegVideo> video = frame_io::FfmpegVideo::open(path, true, error);
    if (!video) {
        std::cerr << "cannot open " << path << ": " << error << '\n';
        return nullptr;
    }
    const frame_io::VideoInfo& info = video->info();
    std::cout << live_sender::fileNameOf(path) << ": " << info.size.width << 'x' << info.size.height << ", "
              << info.bitDepth << "-bit, " << nameOf(info.transfer) << ", " << nameOf(info.primaries) << ", "
              << info.decoder << '\n';
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
        std::cerr << kUsage << "(live_sender --help shows examples)\n";
        return kExitUsageError;
    }
    if (arguments->help) {
        std::cout << kUsage << kExamples;
        return kExitAllDelivered;
    }
    if (arguments->listCameras) {
        return listCameras();
    }
#if !defined(FRAME_IO_HAS_FFMPEG)
    if (arguments->hdr) {
        std::cerr << "live_sender was built without FFmpeg, so --hdr is not available\n";
        return kExitUsageError;
    }
#endif

    cv::VideoCapture capture;
#if defined(FRAME_IO_HAS_FFMPEG)
    std::unique_ptr<frame_io::FfmpegVideo> video; // --hdr's
#endif
    std::optional<Clock::duration> interval;
    std::function<std::optional<cv::Mat>()> next;
    std::function<std::optional<TimedFrame>()> readTimed; // a file's
    std::optional<live_sender::FramePacer> pacer;         // the pattern's
    std::optional<frame_io::ReadAhead<TimedFrame>>
        ahead; // a file's, after `capture` and `video`: it reads them
    std::optional<live_sender::RealTimeFrames> realTime; // a file's, played from `ahead`
    visual_debug::ImageDescription description;
    std::string described; // the description in the summary, as " (HLG, BT.2020)"
    if (const auto* pattern = std::get_if<live_sender::PatternSource>(&arguments->source)) {
        interval =
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / pattern->fps));
        pacer.emplace(*interval, Clock::now());
        next = [size = pattern->size, index = 0]() mutable -> std::optional<cv::Mat> {
            return live_sender::patternFrame(index++, size);
        };
    } else if (arguments->hdr) {
#if defined(FRAME_IO_HAS_FFMPEG)
        video = openHdrVideo(std::get<live_sender::FileSource>(arguments->source).path);
        if (!video) {
            return kExitUsageError;
        }
        const frame_io::VideoInfo& info = video->info();
        interval = intervalOf(info.fps);
        description = {.valueRange = info.valueRange, .transfer = info.transfer, .primaries = info.primaries};
        described = std::string(" (") + nameOf(info.transfer) + ", " + nameOf(info.primaries) + ")";
        readTimed = [&video]() -> std::optional<TimedFrame> {
            std::optional<frame_io::VideoFrame> frame = video->read();
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
        // A file is decoded ahead on a thread of its own and played in real time (see RealTimeFrames).
        ahead.emplace(readTimed, kFramesReadAhead);
        realTime.emplace([&ahead] { return ahead->ready(); }, [&ahead] { return ahead->take(); }, interval,
                         [] { return Clock::now(); }, waitUntil);
        next = [&realTime] { return realTime->next(); };
    }

    const auto waitAndContinue = [&pacer] {
        if (pacer && waitUntil(pacer->due())) {
            pacer->sent(Clock::now());
        }
        return !stopRequested;
    };

    std::signal(SIGINT, requestStop);
    visual_debug::Client client(visual_debug::ClientOptions{.endpoint = arguments->endpoint});
    const live_sender::ChannelNames names = live_sender::channelNames(*arguments);
    const live_sender::Tally tally = live_sender::run(next, client, arguments->frames, waitAndContinue, names,
                                                      description, arguments->outline);

    if (tally.camera.sent == 0 && !stopRequested) {
        std::cerr << "no frame could be read\n";
        return kExitUsageError;
    }
    printTally(names.camera + described, tally.camera, realTime ? realTime->skipped() : 0);
    printTally(names.edges, tally.edges);
    const bool allDelivered =
        tally.camera.delivered == tally.camera.sent && tally.edges.delivered == tally.edges.sent;
    return allDelivered ? kExitAllDelivered : kExitNotDelivered;
}
