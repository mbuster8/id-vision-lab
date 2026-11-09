#include "frame_io/video_source.h"

#include "frame_io/read_ahead.h"
#include "frame_io/video_file.h"
#if defined(FRAME_IO_HAS_FFMPEG)
#include "frame_io/ffmpeg_video.h"
#endif

#include <opencv2/videoio.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace frame_io {
namespace {

constexpr std::size_t kFramesAhead = 3;

// UTF-8, as OpenCV (under the UTF-8 code page on Windows) and FFmpeg take paths.
std::string utf8Of(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string fileNameOf(const std::filesystem::path& path) { return utf8Of(path.filename()); }

// The next frame of one opened reading of the file, given the number it gets: the frame, the end, or an
// error.
using Reader = std::function<ReadResult(std::uint64_t number)>;
// Makes the reader's next frame the first at or after `at`: false with `error` when it cannot, or with no
// error when `abandon` gave it up.
using Seeker = std::function<bool(std::chrono::nanoseconds at, const std::function<bool()>& abandon,
                                  std::string& error)>;

// One opening of the file: how to read it from its first frame, and what it says about itself.
struct Opened {
    Reader reader;
    Seeker seeker; // empty when it cannot seek
    std::optional<std::uint64_t> count;
    std::optional<double> rate;
};

// FFmpeg reports most damage, and a file cut short, as the end of the file. A reader that ends well before
// the frame count its file's header gives is taken for one that failed: at least two frames short and under
// 95 % of the count, since a count worked out from a duration and a rate can be a frame or so off.
bool endedEarly(std::uint64_t frames, const std::optional<std::uint64_t>& count) {
    return count && frames + 2 <= *count && static_cast<double>(frames) < 0.95 * static_cast<double>(*count);
}

Opened openWithOpenCv(const std::string& path, std::string& error) {
    auto capture = std::make_shared<cv::VideoCapture>();
    try {
        if (!openVideoFile(*capture, path)) {
            error = fileNameOf(path) + ": no reader opens it";
            return {};
        }
    } catch (const cv::Exception&) {
        error = fileNameOf(path) + ": no reader opens it";
        return {};
    }
    Opened opened;
    const double frames = capture->get(cv::CAP_PROP_FRAME_COUNT);
    opened.count =
        frames >= 1.0 ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(frames)) : std::nullopt;
    const double fps = capture->get(cv::CAP_PROP_FPS);
    opened.rate = fps > 0.0 ? std::optional<double>(fps) : std::nullopt;
    opened.reader = [capture, fps](std::uint64_t number) -> ReadResult {
        cv::Mat image;
        if (!capture->read(image) || image.empty()) {
            return {}; // OpenCV cannot tell a failed read from the end
        }
        const std::int64_t at =
            fps > 0.0 ? std::llround(static_cast<double>(number) * 1e9 / fps) : static_cast<std::int64_t>(0);
        Frame frame{.image = image, .number = number, .at = std::chrono::nanoseconds(at), .tags = {}};
        return {.status = ReadStatus::Frame, .frame = std::move(frame), .error = {}};
    };
    return opened;
}

#if defined(FRAME_IO_HAS_FFMPEG)
Opened openWithFfmpeg(const std::string& path) {
    std::string error;
    const std::shared_ptr<FfmpegVideo> video = FfmpegVideo::open(
        path, FfmpegVideo::Options{.allowHardware = true, .eightBitAsEightBit = true}, error);
    if (!video) {
        return {};
    }
    const VideoInfo& info = video->info();
    const visual_debug::ImageDescription tags{.valueRange = info.valueRange,
                                              .transfer = info.transfer,
                                              .primaries = info.primaries,
                                              .overlays = {}};
    Opened opened;
    opened.count = info.frameCount;
    opened.rate = info.fps > 0.0 ? std::optional<double>(info.fps) : std::nullopt;
    opened.reader = [video, tags](std::uint64_t number) -> ReadResult {
        std::optional<VideoFrame> read = video->read();
        if (!read) {
            if (video->lastError().empty()) {
                return {};
            }
            return {.status = ReadStatus::Error, .frame = {}, .error = video->lastError()};
        }
        Frame frame{.image = read->image, .number = number, .at = read->at, .tags = tags};
        return {.status = ReadStatus::Frame, .frame = std::move(frame), .error = {}};
    };
    if (info.seekable) {
        opened.seeker = [video](std::chrono::nanoseconds at, const std::function<bool()>& abandon,
                                std::string& seekError) {
            if (video->seek(at, abandon)) {
                return true;
            }
            seekError = video->lastError();
            return false;
        };
    }
    return opened;
}
#endif

// The file opened from its first frame: through FFmpeg when it can, else through OpenCV.
Opened openReader(const std::filesystem::path& file, std::string& error) {
    const std::string path = utf8Of(file);
#if defined(FRAME_IO_HAS_FFMPEG)
    if (Opened opened = openWithFfmpeg(path); opened.reader) {
        return opened;
    }
#endif
    return openWithOpenCv(path, error);
}

class VideoSource final : public ReplaySource {
public:
    VideoSource(std::filesystem::path file, Opened opened, VideoOptions options)
        : m_file(std::move(file)), m_options(std::move(options)), m_opened(std::move(opened)) {
        start(0, false);
    }

    // A seek held on the reading thread gives up at once rather than finishing first.
    ~VideoSource() override { stopReading(); }

    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;

    ReadResult next() override {
        if (m_errorTaken) {
            return {};
        }
        std::optional<ReadResult> result = m_ahead->take();
        if (!result) {
            return {};
        }
        if (result->status == ReadStatus::Error) {
            m_errorTaken = true;
            result->error = fileNameOf(m_file) + ": " + result->error;
        }
        return std::move(*result);
    }

    bool ready() const override { return m_ahead->ready(); }

    void setWake(std::function<void()> wake) override {
        m_wake = std::move(wake);
        m_ahead->setWake(m_wake);
    }

    bool restart(std::string& error) override {
        Opened opened = openReader(m_file, error);
        if (!opened.reader) {
            return false;
        }
        stopReading(); // before the reading it uses is replaced
        m_opened = std::move(opened);
        start(0, false);
        return true;
    }

    bool canSeek() const override { return m_opened.seeker && m_opened.rate; }

    void seek(std::uint64_t index) override {
        if (canSeek()) {
            start(index, true);
        }
    }

    std::optional<std::uint64_t> frameCount() const override { return m_opened.count; }
    std::optional<double> frameRate() const override { return m_opened.rate; }
    std::string describe() const override { return fileNameOf(m_file); }

private:
    // Gives up the current reading: a seek in it stops between frames, a read finishes its frame.
    void stopReading() {
        if (m_abandoned) {
            m_abandoned->store(true);
        }
        m_ahead.reset();
    }

    // Reads ahead from frame `first`, after seeking to it when `seekFirst`; a stream ends after its first
    // error. Frames are counted from `first`, so the end after a seek is not taken for a file cut short.
    void start(std::uint64_t first, bool seekFirst) {
        stopReading();
        m_errorTaken = false;
        m_abandoned = std::make_shared<std::atomic<bool>>(false);
        Seeker seeker = seekFirst ? m_opened.seeker : Seeker{};
        const std::chrono::nanoseconds at(
            seekFirst ? std::llround(static_cast<double>(first) * 1e9 / *m_opened.rate) : 0);
        m_ahead = std::make_unique<ReadAhead<ReadResult>>(
            [reader = m_opened.reader, seeker = std::move(seeker), beforeSeek = m_options.beforeSeek,
             abandoned = m_abandoned, at, count = m_opened.count, number = first,
             ended = false]() mutable -> std::optional<ReadResult> {
                if (ended) {
                    return std::nullopt;
                }
                if (seeker) {
                    const Seeker seekNow = std::exchange(seeker, Seeker{});
                    const std::function<bool()> isAbandoned = [abandoned] { return abandoned->load(); };
                    if (beforeSeek) {
                        beforeSeek(isAbandoned);
                    }
                    std::string error;
                    if (!seekNow(at, isAbandoned, error)) {
                        if (isAbandoned()) {
                            return std::nullopt; // given up: nobody takes what this reading reads
                        }
                        ended = true;
                        return ReadResult{.status = ReadStatus::Error,
                                          .frame = {},
                                          .error = "could not seek to frame " + std::to_string(number + 1) +
                                                   ": " + error};
                    }
                }
                ReadResult result = reader(number);
                if (result.status == ReadStatus::End && endedEarly(number, count)) {
                    result = {.status = ReadStatus::Error,
                              .frame = {},
                              .error = "ended after " + std::to_string(number) + " of " +
                                       std::to_string(*count) + " frames"};
                }
                if (result.status == ReadStatus::End) {
                    return std::nullopt;
                }
                ended = result.status == ReadStatus::Error;
                number += result.status == ReadStatus::Frame ? 1 : 0;
                return result;
            },
            kFramesAhead,
            [](std::string what) -> std::optional<ReadResult> {
                return ReadResult{
                    .status = ReadStatus::Error, .frame = {}, .error = "reading failed: " + what};
            });
        if (m_wake) {
            m_ahead->setWake(m_wake);
        }
    }

    std::filesystem::path m_file;
    VideoOptions m_options;
    Opened m_opened;
    std::function<void()> m_wake;
    bool m_errorTaken = false;
    std::shared_ptr<std::atomic<bool>> m_abandoned; // the current reading's
    std::unique_ptr<ReadAhead<ReadResult>> m_ahead;
};

} // namespace

std::unique_ptr<ReplaySource> openVideo(const std::filesystem::path& file, std::string& error,
                                        VideoOptions options) {
    Opened opened = openReader(file, error);
    if (!opened.reader) {
        return nullptr;
    }
    return std::make_unique<VideoSource>(file, std::move(opened), std::move(options));
}

} // namespace frame_io
