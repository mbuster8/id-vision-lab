#pragma once

#include <visual_debug/image_description.h>

#include <opencv2/core.hpp>

#include <array>
#include <cstdint>

namespace frame_io {

// Camera frames arrive as YUV in one of a few layouts, with a matrix and a range their format names. OpenCV's
// own conversions know one of each (BT.601, video range), so the others are converted here, exactly. BT.2020
// is the matrix of HDR video.
enum class YuvMatrix { Bt601, Bt709, Bt2020 };
// Video range: luma 16 to 235, chroma 16 to 240 (10-bit: 64 to 940, 64 to 960). Full range: all codes.
enum class YuvRange { Video, Full };
// Nv12 and Nv21: a luma plane and one plane of interleaved chroma at half width and height (UV, or VU).
// Yuyv and Uyvy: one packed plane at half-width chroma. I420 and Yv12: three planes, chroma at half width and
// height (Y, U, V, or Y, V, U).
enum class YuvLayout { Nv12, Nv21, Yuyv, Uyvy, I420, Yv12 };

// A frame's planes in the order its layout stores them (for Yv12, data[1] is V), each with its bytes per row.
// The caller keeps the memory alive for the call.
struct YuvPlanes {
    YuvLayout layout = YuvLayout::Nv12;
    cv::Size size;
    std::array<const std::uint8_t*, 3> data{};
    std::array<int, 3> stride{};
};

// CV_8UC3 BGR, a new buffer. Empty if the size is empty or odd where the layout halves it, or a plane is
// missing.
cv::Mat yuvToBgr(const YuvPlanes& planes, YuvMatrix matrix, YuvRange range);

// P010 (10-bit codes in the top bits of 16-bit words, a luma plane and interleaved UV at half width and
// height) to CV_16UC3 BGR on FfmpegVideo's scale: a 10-bit level v lands at v × 65535 / 1023, so video
// range's black is 4100 and white 60218, and the codes above white keep their values (up to 65535) instead of
// clipping. Empty for the same reasons as yuvToBgr.
cv::Mat p010ToBgr16(const std::uint8_t* y, int yStride, const std::uint8_t* uv, int uvStride, cv::Size size,
                    YuvMatrix matrix, YuvRange range);

// The black and white levels p010ToBgr16 gives for a range: the tags that tell the viewer where white is.
visual_debug::ValueRange p010ValueRange(YuvRange range);

} // namespace frame_io
