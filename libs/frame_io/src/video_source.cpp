#include "frame_io/video_source.h"

#include "frame_io/read_ahead.h"
#include "frame_io/video_file.h"
#if defined(FRAME_IO_HAS_FFMPEG)
#include "frame_io/ffmpeg_video.h"
#endif

#include <opencv2/videoio.hpp>

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

// One opened reading of the file, from its first frame: the next frame, its end, or an error.
using Reader = std::function<ReadResult()>;

// FFmpeg reports most damage, and a file cut short, as the end of the file. A reader that ends well before
// the frame count its file's header gives is taken for one that failed: at least two frames short and under
// 95 % of the count, since a count worked out from a duration and a rate can be a frame or so off.
bool endedEarly(std::uint64_t frames, const std::optional<std::uint64_t>& count) {
    return count && frames + 2 <= *count && static_cast<double>(frames) < 0.95 * static_cast<double>(*count);
}

Reader openWithOpenCv(const std::string& path, std::optional<std::uint64_t>& count, std::string& error) {
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
    const double frames = capture->get(cv::CAP_PROP_FRAME_COUNT);
    count = frames >= 1.0 ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(frames)) : std::nullopt;
    const double fps = capture->get(cv::CAP_PROP_FPS);
    return [capture, fps, number = std::uint64_t{0}]() mutable -> ReadResult {
        cv::Mat image;
        if (!capture->read(image) || image.empty()) {
            return {}; // OpenCV cannot tell a failed read from the end
        }
        const std::int64_t at =
            fps > 0.0 ? std::llround(static_cast<double>(number) * 1e9 / fps) : static_cast<std::int64_t>(0);
        Frame frame{.image = image, .number = number++, .at = std::chrono::nanoseconds(at), .tags = {}};
        return {.status = ReadStatus::Frame, .frame = std::move(frame), .error = {}};
    };
}

#if defined(FRAME_IO_HAS_FFMPEG)
bool needsFfmpeg(const VideoInfo& info) {
    return info.bitDepth > 8 || info.transfer == visual_debug::Transfer::Hlg ||
           info.transfer == visual_debug::Transfer::Pq;
}

Reader readerOf(std::shared_ptr<FfmpegVideo> video) {
    const VideoInfo info = video->info();
    const visual_debug::ImageDescription tags{.valueRange = info.valueRange,
                                              .transfer = info.transfer,
                                              .primaries = info.primaries,
                                              .overlays = {}};
    return [video, tags, number = std::uint64_t{0}]() mutable -> ReadResult {
        std::optional<VideoFrame> read = video->read();
        if (!read) {
            if (video->lastError().empty()) {
                return {};
            }
            return {.status = ReadStatus::Error, .frame = {}, .error = video->lastError()};
        }
        Frame frame{.image = read->image, .number = number++, .at = read->at, .tags = tags};
        return {.status = ReadStatus::Frame, .frame = std::move(frame), .error = {}};
    };
}
#endif

// The file opened from its first frame, by whichever reader suits it.
Reader openReader(const std::filesystem::path& file, std::optional<std::uint64_t>& count,
                  std::string& error) {
    const std::string path = utf8Of(file);
#if defined(FRAME_IO_HAS_FFMPEG)
    std::string ffmpegError;
    std::shared_ptr<FfmpegVideo> video = FfmpegVideo::open(path, true, ffmpegError);
    if (video && needsFfmpeg(video->info())) {
        count = video->info().frameCount;
        return readerOf(std::move(video));
    }
#endif
    return openWithOpenCv(path, count, error);
}

class VideoSource final : public ReplaySource {
public:
    VideoSource(std::filesystem::path file, Reader reader, std::optional<std::uint64_t> count)
        : m_file(std::move(file)), m_count(count) {
        start(std::move(reader));
    }

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
        std::optional<std::uint64_t> count;
        Reader reader = openReader(m_file, count, error);
        if (!reader) {
            return false;
        }
        m_count = count;
        start(std::move(reader));
        return true;
    }

    bool canSeek() const override { return false; }
    void seek(std::uint64_t /*index*/) override {} // a video cannot; canSeek() says so
    std::optional<std::uint64_t> frameCount() const override { return m_count; }
    std::string describe() const override { return fileNameOf(m_file); }

private:
    // Reads ahead with `reader` from its first frame; a stream ends after its first error.
    void start(Reader reader) {
        m_ahead.reset(); // stops the old reading thread first
        m_errorTaken = false;
        m_ahead = std::make_unique<ReadAhead<ReadResult>>(
            [reader = std::move(reader), count = m_count, ended = false,
             frames = std::uint64_t{0}]() mutable -> std::optional<ReadResult> {
                if (ended) {
                    return std::nullopt;
                }
                ReadResult result = reader();
                if (result.status == ReadStatus::End && endedEarly(frames, count)) {
                    result = {.status = ReadStatus::Error,
                              .frame = {},
                              .error = "ended after " + std::to_string(frames) + " of " +
                                       std::to_string(*count) + " frames"};
                }
                if (result.status == ReadStatus::End) {
                    return std::nullopt;
                }
                ended = result.status == ReadStatus::Error;
                frames += result.status == ReadStatus::Frame ? 1 : 0;
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
    std::optional<std::uint64_t> m_count;
    std::function<void()> m_wake;
    bool m_errorTaken = false;
    std::unique_ptr<ReadAhead<ReadResult>> m_ahead;
};

} // namespace

std::unique_ptr<ReplaySource> openVideo(const std::filesystem::path& file, std::string& error) {
    std::optional<std::uint64_t> count;
    Reader reader = openReader(file, count, error);
    if (!reader) {
        return nullptr;
    }
    return std::make_unique<VideoSource>(file, std::move(reader), count);
}

} // namespace frame_io
