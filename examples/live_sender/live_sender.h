#pragma once

#include "visual_debug/image_sender.h"

#include <opencv2/core/mat.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace live_sender {

struct CameraSource {
    int index = 0;
};

struct FileSource {
    std::string path;
};

// A generated moving picture, sent at `fps` frames a second.
struct PatternSource {
    cv::Size size{640, 480};
    double fps = 30.0;
};

struct Arguments {
    std::variant<CameraSource, FileSource, PatternSource> source = CameraSource{};
    std::optional<int> frames; // stop after this many; otherwise at the source's end or on Ctrl+C
    std::string endpoint;      // visual_debug::defaultEndpoint() unless given
    std::string name;          // the channels' name; empty for the default ones
    bool listCameras = false;
    bool help = false; // print the usage and examples
    bool hdr = false;  // a video file's frames as 16-bit data with their transfer and primaries
    bool outline =
        false; // each frame with the outline of the largest four-cornered shape in it, as an overlay
};

inline constexpr double kMinPatternFps = 0.1;
inline constexpr double kMaxPatternFps = 1000.0;

// live_sender [--camera N | [--hdr] <video file> | --pattern [--size WxH] [--fps N]] [--frames N]
//             [--name NAME] [--outline] [--endpoint <uri>]
// live_sender --list-cameras
// Nothing for anything else: an unknown option, a missing or malformed value, two sources, --size or --fps
// without --pattern, --hdr without a video file, a pattern the viewer could not take or a rate outside
// kMinPatternFps to
// kMaxPatternFps, a name that is empty or would not keep "NAME edges" whole on the wire (invalid UTF-8, or
// more than visual_debug::kMaxNameBytes less 6 bytes), or --list-cameras or --help with other arguments.
std::optional<Arguments> parseArguments(int argc, char* argv[]);

// The two channels a live_sender streams: the frames and their edges.
struct ChannelNames {
    std::string camera = "camera";
    std::string edges = "edges";
};

// "camera" and "edges" for an empty name; otherwise NAME and "NAME edges", so several live_senders can share
// a viewer.
ChannelNames channelNames(const std::string& name);

// The channels for these arguments: --name's; without it, "camera N" for camera N and a video file's name
// (its first 32 characters, as valid UTF-8), so each row says where it comes from; the pattern's are the
// default.
ChannelNames channelNames(const Arguments& arguments);

// The cameras that open among numbers 0 to count - 1, with their frame sizes; `open` tries one. With
// stopAtFirstMissing, the first number that does not open ends the list: macOS numbers its cameras without
// gaps, and OpenCV prints a complaint of its own for every number that does not open there.
std::vector<std::pair<int, cv::Size>> listCameras(const std::function<std::optional<cv::Size>(int)>& open,
                                                  int count, bool stopAtFirstMissing);

// A BGR test picture that moves (a scrolling gradient, a disc crossing it, the frame number), so consecutive
// indices give different frames and the edges channel has outlines to show. `index` from 0, size from 1×1.
cv::Mat patternFrame(int index, cv::Size size);

// The Canny edges (thresholds 50 and 150) of an 8- or 16-bit greyscale, BGR or BGRA frame (of a 16-bit one,
// its top 8 bits): CV_8UC1, the same size.
cv::Mat edgesOf(const cv::Mat& frame);

// The corners of the largest document-like shape in a frame (any frame edgesOf takes), such as a card or a
// passport page, in the frame's pixels: four corners fitted to an outline's convex hull, covering at least 10
// % of the frame, wholly in view, lighter inside than the frame on average, with no corner sharper than 45°
// or blunter than 135° and no side under a fifth of the longest. Nothing when no shape qualifies. Found on a
// blurred copy at most 640 pixels on its longest side, so a 4K frame costs about what a small one does. A
// demonstration of overlays, not a document detector: it works best with the whole document in view on a
// darker surface; a light page on a light desk, or a page the frame cuts off, is rarely found.
std::optional<std::vector<cv::Point2d>> documentOutline(const cv::Mat& frame);

// How many frames an outline is kept after it is lost: a third of a second at 30 frames a second.
inline constexpr int kOutlineHoldFrames = 10;

// Keeps the last outline found for kOutlineHoldFrames frames after the detector loses it, so a frame or two
// it misses (motion blur, a finger crossing an edge) do not make the outline flicker.
class OutlineHold {
public:
    // The outline to show for a frame in which `found` was found (or nothing was).
    std::optional<std::vector<cv::Point2d>> next(std::optional<std::vector<cv::Point2d>> found);

private:
    std::optional<std::vector<cv::Point2d>> m_last;
    int m_missed = 0;
};

// When each frame is due: one interval after the previous one was due, so a timer that wakes a little late
// every time does not slow the stream down. After a real stall (a frame sent a whole interval late or more)
// the schedule starts again from then, so a slow send delays the stream instead of making it rush to catch
// up.
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;

    FramePacer(Clock::duration interval, Clock::time_point start);
    Clock::time_point due() const;
    void sent(Clock::time_point now); // the frame that was due went out at `now`

private:
    Clock::duration m_interval;
    Clock::time_point m_due;
};

// Plays a video in real time from `start`: the frame at `at` into the video is due at start + at. A frame
// more than one frame interval past due when it is ready is late, and skipped, so a slow reader or sender
// drops frames instead of slowing the video down.
class Playback {
public:
    using Clock = std::chrono::steady_clock;

    Playback(Clock::time_point start, Clock::duration frameInterval);
    Clock::time_point due(Clock::duration at) const;
    bool late(Clock::duration at, Clock::time_point now) const;

private:
    Clock::time_point m_start;
    Clock::duration m_frameInterval;
};

// A video's frame and when it is due, from the start of the video.
struct TimedFrame {
    cv::Mat frame;
    Playback::Clock::duration at;
};

// A video's frames in real time, as `take` hands them over from a reader running ahead (`ready`: whether
// take() would return at once; nothing from take() is the end). Each frame is due its `at` after the first
// one was shown, and waited for with `waitUntil`, whose false (Ctrl+C) ends the video. A frame that was ready
// but is more than a frame interval late is skipped: the sending fell behind, and skipping catches up. One
// that was not ready when asked for is never skipped: the reading fell behind, which skipping cannot catch
// up, so the schedule starts again from it and the video plays slower instead of freezing. Without an
// interval the frames come as fast as they are read.
class RealTimeFrames {
public:
    using Clock = Playback::Clock;

    RealTimeFrames(std::function<bool()> ready, std::function<std::optional<TimedFrame>()> take,
                   std::optional<Clock::duration> interval, std::function<Clock::time_point()> now,
                   std::function<bool(Clock::time_point)> waitUntil);
    std::optional<cv::Mat> next(); // nothing at the end, or when waitUntil returns false
    int skipped() const;

private:
    std::function<bool()> m_ready;
    std::function<std::optional<TimedFrame>()> m_take;
    std::optional<Clock::duration> m_interval;
    std::function<Clock::time_point()> m_now;
    std::function<bool(Clock::time_point)> m_waitUntil;
    std::optional<Playback> m_playback;
    int m_skipped = 0;
};

struct ChannelTally {
    int sent = 0;
    int delivered = 0;
    std::map<visual_debug::SendResult, int> failed; // how often each other result came back
};

struct Tally {
    ChannelTally camera;
    ChannelTally edges;
};

// Until `frames` frames have been sent, `next` has none left, or `waitAndContinue` returns false: calls
// `waitAndContinue` (which paces the frames and watches for Ctrl+C), reads a frame, and sends it as live
// channel names.camera, described by `description`, and its edges as live channel names.edges. With
// `outline`, each camera frame also carries documentOutline's corners as a polygon labelled "Document", found
// before the frame is sent, since the overlay must travel with it.
Tally run(const std::function<std::optional<cv::Mat>()>& next, visual_debug::ImageSender& sender,
          std::optional<int> frames, const std::function<bool()>& waitAndContinue,
          const ChannelNames& names = ChannelNames{}, const visual_debug::ImageDescription& description = {},
          bool outline = false);

} // namespace live_sender
