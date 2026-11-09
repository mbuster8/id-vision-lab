#pragma once

#include "visual_debug/image_description.h"

#include <opencv2/core/mat.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace frame_io {

struct VideoFrame {
    cv::Mat image;                 // CV_16UC3 BGR on the 0–65535 scale, upright
    std::chrono::nanoseconds at{}; // from the first frame
};

struct VideoInfo {
    cv::Size size;    // upright
    double fps = 0.0; // the stream's average rate; 0 if unknown
    int bitDepth = 8;
    visual_debug::ValueRange valueRange;                                // nominal black and white, measured
    visual_debug::Transfer transfer = visual_debug::Transfer::Sdr;      // unknown is SDR
    visual_debug::Primaries primaries = visual_debug::Primaries::Bt709; // unknown is BT.709
    std::string decoder;        // "VideoToolbox", "D3D11VA" or "software"
    int dolbyVisionProfile = 0; // 0 without Dolby Vision
};

// A video file read with FFmpeg's libraries rather than OpenCV's videoio, which hands over 8-bit pictures
// without their transfer and primaries. Every frame comes as 16-bit BGR: a limited-range video's nominal
// black and white land where their codes would on the full scale (64 and 940 of 1023 at 4100 and 60218), so
// the codes above white, where an HLG or PQ video keeps its highlights, keep their values instead of
// clipping.
class FfmpegVideo {
public:
    // Hardware decoding (VideoToolbox on macOS, D3D11VA on Windows) when allowed and the video's codec has
    // it, software otherwise or when the hardware cannot decode the first frame. Nothing, with `error` set,
    // for a file that does not open or has no video stream that decodes.
    static std::unique_ptr<FfmpegVideo> open(const std::string& path, bool allowHardware, std::string& error);

    FfmpegVideo(const FfmpegVideo&) = delete;
    FfmpegVideo& operator=(const FfmpegVideo&) = delete;
    ~FfmpegVideo();

    const VideoInfo& info() const;
    std::optional<VideoFrame> read(); // nothing at the end or on an error
    // Why the last read() gave nothing: empty at the end of the file, the reason after anything else.
    const std::string& lastError() const;

private:
    struct Impl;
    explicit FfmpegVideo(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace frame_io
