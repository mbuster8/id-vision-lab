#include "camera_formats.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using document_lab::CameraFormat;
using PixelFormat = QVideoFrameFormat::PixelFormat;

CameraFormat format(int width, int height, float fps, PixelFormat pixelFormat = PixelFormat::Format_NV12) {
    return {.size = QSize(width, height), .maxFps = fps, .pixelFormat = pixelFormat};
}

TEST(CameraFormats, LabelNamesSizeRateAndFormat) {
    EXPECT_EQ(format(1920, 1440, 60).label(), QStringLiteral("1920×1440, 60 fps, NV12"));
    EXPECT_EQ(format(1280, 720, 29.97F, PixelFormat::Format_Jpeg).label(),
              QStringLiteral("1280×720, 29.97 fps, Jpeg"));
}

// A camera lists the same format once per frame-rate range and colour space; the picker needs each once,
// the largest first.
TEST(CameraFormats, OncePerSizeRateAndFormat) {
    const std::vector<CameraFormat> distinct = document_lab::distinctFormats(
        {format(1280, 720, 30), format(1920, 1080, 30), format(1280, 720, 30), format(1920, 1080, 60),
         format(1920, 1080, 30, PixelFormat::Format_Jpeg), format(1920, 1080, 30)});

    EXPECT_EQ(distinct, (std::vector<CameraFormat>{format(1920, 1080, 60), format(1920, 1080, 30),
                                                   format(1920, 1080, 30, PixelFormat::Format_Jpeg),
                                                   format(1280, 720, 30)}));
}

// Uncompressed first (JPEG costs a decode per frame), then the most pixels, then the highest rate up to 60.
TEST(CameraFormats, DefaultPrefersUncompressedThenPixels) {
    EXPECT_EQ(document_lab::defaultFormat(
                  {format(1920, 1080, 30, PixelFormat::Format_Jpeg), format(1280, 720, 30)}),
              format(1280, 720, 30));
    EXPECT_EQ(document_lab::defaultFormat({format(1920, 1440, 30), format(1920, 1440, 60)}),
              format(1920, 1440, 60));
    EXPECT_EQ(document_lab::defaultFormat({format(1280, 720, 120), format(1280, 720, 60)}),
              format(1280, 720, 60));
    EXPECT_EQ(
        document_lab::defaultFormat({format(1280, 720, 30, PixelFormat::Format_YUYV), format(1280, 720, 30)}),
        format(1280, 720, 30));
    EXPECT_EQ(document_lab::defaultFormat({}), std::nullopt);
}

} // namespace
