#pragma once

// Test clips written with FFmpeg's own libraries, for the tests of frame_io's FFmpeg reader and of the replay
// sources built on it.

#include <opencv2/core.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

#include "scratch_folder.h"

#include <array>
#include <optional>
#include <string>

namespace test_support {

inline constexpr int kClipFrames = 4;

// A test clip: four frames at 30 fps, each of one Y, U and V, with an optional block of another Y, tagged as
// given. The encoder copies the tags to the stream, where Matroska and MOV store them.
struct Clip {
    const char* encoder = "ffv1"; // lossless, so the codes read back are the codes written
    AVPixelFormat format = AV_PIX_FMT_YUV420P10LE;
    cv::Size size{64, 48};
    AVColorRange range = AVCOL_RANGE_MPEG;
    AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    std::array<int, kClipFrames> y{64, 64, 64, 64}; // each frame's
    int u = 512;
    int v = 512;
    std::optional<cv::Rect> block{}; // drawn with blockY
    int blockY = 940;
    std::optional<double> rotation{}; // degrees, as av_display_rotation_set takes them
};

// Writes the clip; the container follows the file name's extension.
bool writeClip(const std::string& path, const Clip& clip);

} // namespace test_support
